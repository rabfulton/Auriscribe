#include "app.h"
#include "overlay.h"
#include "paste.h"
#include "ui_settings.h"
#include "ui_download.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#ifdef __GLIBC__
#include <malloc.h>
#endif

App *app = NULL;

static void on_audio_data(const float *samples, size_t count, void *userdata);
static void on_hotkey(void *userdata);
static gboolean toggle_from_hotkey(gpointer data);
static void on_model_downloaded(const char *model_id, const char *model_path, void *userdata);
static gpointer worker_thread_main(gpointer data);
static gboolean finalize_paste_idle(gpointer data);
static void cancel_model_unload_timer(App *a);
static void schedule_model_unload_timer(App *a);
static gboolean unload_model_timeout_cb(gpointer data);
static void pad_recording_tail(App *a);
static void start_vulkan_warmup_async(void);
static gboolean overlay_append_idle(gpointer data);
static gboolean show_transcribe_error_idle(gpointer data);

typedef struct {
    float *samples;
    size_t count;
    bool flush;
} AudioChunk;

typedef struct {
    unsigned long target_window;
} FinalizePaste;

typedef struct {
    App *app;
    char *text;
} OverlayAppend;

typedef struct {
    char *message;
} ErrorDialog;

static int chunk_queue_sentinel;
#define CHUNK_QUEUE_SENTINEL ((gpointer) &chunk_queue_sentinel)

static const char *env_get(const char *preferred, const char *legacy) {
    const char *v = preferred ? getenv(preferred) : NULL;
    if (v && *v) return v;
    v = legacy ? getenv(legacy) : NULL;
    if (v && *v) return v;
    return NULL;
}

static void try_trim_heap(void) {
#ifdef __GLIBC__
    (void)malloc_trim(0);
#endif
}

static void dbg_chunk(App *a, const char *fmt, ...) {
    if (!a || !a->debug_chunking) return;
    va_list ap;
    va_start(ap, fmt);
    const gint64 now_us = g_get_monotonic_time();
    fprintf(stderr, "[chunk %lldms] ", (long long)(now_us / 1000));
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static unsigned long x11_get_active_window(void) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 0;

    unsigned long result = 0;
    Window root = DefaultRootWindow(dpy);
    Atom prop = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", True);
    if (prop != None) {
        Atom actual_type = None;
        int actual_format = 0;
        unsigned long nitems = 0;
        unsigned long bytes_after = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, root, prop, 0, (~0L), False, AnyPropertyType,
                               &actual_type, &actual_format, &nitems, &bytes_after, &data) == Success) {
            if (data && nitems >= 1) {
                result = *(unsigned long *)data;
            }
            if (data) XFree(data);
        }
    }

    if (!result) {
        Window focus = 0;
        int revert = 0;
        XGetInputFocus(dpy, &focus, &revert);
        if (focus != None && focus != PointerRoot) result = (unsigned long)focus;
    }

    XCloseDisplay(dpy);
    return result;
}

static bool ensure_rec_capacity(App *a, size_t additional) {
    if (!a) return false;
    if (additional == 0) return true;
    if (a->rec_count + additional <= a->rec_capacity) return true;

    size_t new_cap = a->rec_capacity ? a->rec_capacity : (SAMPLE_RATE * 5);
    while (new_cap < a->rec_count + additional) {
        if (new_cap > (SIZE_MAX / 2)) return false;
        new_cap *= 2;
    }
    float *nbuf = realloc(a->rec_buffer, new_cap * sizeof(float));
    if (!nbuf) return false;
    a->rec_buffer = nbuf;
    a->rec_capacity = new_cap;
    return true;
}

static void pad_recording_tail(App *a) {
    if (!a) return;
    if (!a->rec_buffer || a->rec_count == 0) return;

    // Add a small amount of trailing silence. Without it, Whisper can sometimes
    // miss the last token/word when audio ends abruptly at a chunk boundary.
    const size_t pad = (size_t)(SAMPLE_RATE * 0.30f); // ~300ms
    if (!ensure_rec_capacity(a, pad)) return;
    memset(a->rec_buffer + a->rec_count, 0, pad * sizeof(float));
    a->rec_count += pad;
}

static void start_vulkan_warmup_async(void) {
    const char *enabled = env_get("AURISCRIBE_VULKAN_WARMUP", "XFCE_WHISPER_VULKAN_WARMUP");
    if (enabled && strcmp(enabled, "0") == 0) return;
    if (env_get("AURISCRIBE_NO_GPU", "XFCE_WHISPER_NO_GPU")) return;
    const bool debug = env_get("AURISCRIBE_DEBUG_VULKAN_WARMUP", "XFCE_WHISPER_DEBUG_VULKAN_WARMUP") != NULL;

    pid_t pid = fork();
    if (pid == 0) {
        // Double-fork so we don't leave a zombie behind.
        pid_t pid2 = fork();
        if (pid2 == 0) {
            if (!debug) {
                int devnull = open("/dev/null", O_RDWR);
                if (devnull >= 0) {
                    dup2(devnull, STDIN_FILENO);
                    dup2(devnull, STDOUT_FILENO);
                    dup2(devnull, STDERR_FILENO);
                    if (devnull > STDERR_FILENO) close(devnull);
                }
            }

            execl("./auriscribe-worker", "auriscribe-worker", "--warmup-vulkan", NULL);
            execlp("auriscribe-worker", "auriscribe-worker", "--warmup-vulkan", NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid < 0) return;

    // Reap the intermediate child.
    (void)waitpid(pid, NULL, 0);
}

void app_init(GtkApplication *gtk_app) {
    app = calloc(1, sizeof(App));
    app->gtk_app = gtk_app;
    app->config = config_load();
    app->state = STATE_IDLE;
    app->hotkey_toggle_queued = 0;
    app->last_hotkey_us = 0;
    app->model_unload_timeout_id = 0;
    app->model_last_used_us = 0;
    app->chunk_queue = g_async_queue_new();
    g_mutex_init(&app->accum_mutex);
    app->accum_text = g_string_new("");
    app->overlay_text = g_string_new("");
    app->stop_requested = false;
    app->pasted_any = 0;
    app->debug_chunking = env_get("AURISCRIBE_DEBUG_CHUNKING", "XFCE_WHISPER_DEBUG_CHUNKING") != NULL;
    app->debug_last_vad_speech = false;
    app->debug_audio_cb_count = 0;
    app->debug_overlay_latency = env_get("AURISCRIBE_DEBUG_OVERLAY_LATENCY", "XFCE_WHISPER_DEBUG_OVERLAY_LATENCY") != NULL;
    app->debug_prev_overlay_lvl = 0.0f;
    atomic_store(&app->overlay_level_us, 0);
    
    // Initialize VAD
    app->vad = vad_new_energy(app->config->vad_threshold);
    
    // Initialize transcriber
    app->transcriber = transcriber_new();
    
    // Initialize audio
    app->audio = audio_capture_new(app->config->microphone);
    audio_capture_set_callback(app->audio, on_audio_data, app);
    
    // Initialize hotkey
    app->hotkey = hotkey_new(app->config->hotkey);
    hotkey_set_callback(app->hotkey, on_hotkey, app);
    app->hotkey_available = hotkey_start(app->hotkey);
    
    // Also setup signal handler for Wayland
    hotkey_setup_signal(on_hotkey, app);
    
    // Allocate initial recording buffer (grows dynamically; no hard cap)
    app->rec_capacity = SAMPLE_RATE * 10;
    app->rec_buffer = malloc(app->rec_capacity * sizeof(float));

    // Start background worker for chunk transcription
    app->worker_thread = g_thread_new("transcribe-worker", worker_thread_main, app);

    // Kick off Vulkan shader compilation early (in a short-lived worker process)
    // so the first hotkey use doesn't pay the one-time pipeline compile cost.
    start_vulkan_warmup_async();
}

void app_cleanup(void) {
    if (!app) return;

    app->shutting_down = true;

    cancel_model_unload_timer(app);
    overlay_hide(app);

    if (app->chunk_queue) {
        g_async_queue_push(app->chunk_queue, CHUNK_QUEUE_SENTINEL); // sentinel to stop worker
    }
    if (app->worker_thread) {
        g_thread_join(app->worker_thread);
        app->worker_thread = NULL;
    }
    if (app->chunk_queue) {
        g_async_queue_unref(app->chunk_queue);
        app->chunk_queue = NULL;
    }
    if (app->accum_text) {
        g_string_free(app->accum_text, TRUE);
        app->accum_text = NULL;
    }
    if (app->overlay_text) {
        g_string_free(app->overlay_text, TRUE);
        app->overlay_text = NULL;
    }
    g_mutex_clear(&app->accum_mutex);

    hotkey_free(app->hotkey);
    audio_capture_free(app->audio);
    transcriber_free(app->transcriber);
    vad_free(app->vad);
    config_save(app->config);
    config_free(app->config);
    free(app->rec_buffer);
    free(app);
    app = NULL;
}

static void cancel_model_unload_timer(App *a) {
    if (!a) return;
    if (a->model_unload_timeout_id) {
        g_source_remove(a->model_unload_timeout_id);
        a->model_unload_timeout_id = 0;
    }
}

static void schedule_model_unload_timer(App *a) {
    if (!a) return;
    cancel_model_unload_timer(a);
    a->model_unload_timeout_id = g_timeout_add_seconds(15, unload_model_timeout_cb, a);
}

static gboolean unload_model_timeout_cb(gpointer data) {
    App *a = data;
    if (!a || a->shutting_down) return G_SOURCE_REMOVE;

    if (a->state != STATE_IDLE) return G_SOURCE_REMOVE;
    if (!transcriber_is_loaded(a->transcriber)) return G_SOURCE_REMOVE;

    fprintf(stderr, "Idle timeout reached; unloading model to free memory\n");
    transcriber_unload(a->transcriber);
    a->model_unload_timeout_id = 0;
    try_trim_heap();
    return G_SOURCE_REMOVE;
}

static void on_audio_data(const float *samples, size_t count, void *userdata) {
    App *a = userdata;
    if (a->state != STATE_RECORDING) return;
    a->debug_audio_cb_count++;

    if (a->config && a->config->overlay_enabled) {
        float peak = 0.0f;
        for (size_t i = 0; i < count; i++) {
            float v = samples[i];
            if (v < 0) v = -v;
            if (v > peak) peak = v;
        }
        // Slight boost for better visual feedback on quiet mics.
        float lvl = peak * 4.0f;
        if (lvl > 1.0f) lvl = 1.0f;
        overlay_set_level(a, lvl);

        if (a->debug_overlay_latency) {
            const float thr = 0.08f;
            if ((a->debug_prev_overlay_lvl < thr && lvl >= thr) || (a->debug_audio_cb_count % 200) == 0) {
                const gint64 now_us = g_get_monotonic_time();
                fprintf(stderr, "[overlay-lat] audio lvl=%.3f count=%zu t=%lldms\n",
                        lvl, count, (long long)(now_us / 1000));
            }
            a->debug_prev_overlay_lvl = lvl;
        }
    }
    
    if (env_get("AURISCRIBE_DEBUG_AUDIO", "XFCE_WHISPER_DEBUG_AUDIO")) {
        float max = 0;
        for (size_t i = 0; i < count; i++) {
            float abs_val = samples[i] > 0 ? samples[i] : -samples[i];
            if (abs_val > max) max = abs_val;
        }
        static int debug_counter = 0;
        if (++debug_counter % 10 == 0) {
            printf("Audio: %zu samples, max=%.4f\n", count, max);
            fflush(stdout);
        }
    }
    
    // Aggregate input into 30ms frames (480 samples) for the VAD.
    const float *p = samples;
    size_t n = count;
    while (n > 0) {
        const size_t space = 480 - a->vad_accum_count;
        const size_t take = n < space ? n : space;
        memcpy(a->vad_accum + a->vad_accum_count, p, take * sizeof(float));
        a->vad_accum_count += take;
        p += take;
        n -= take;

        if (a->vad_accum_count < 480) break;

        VADResult vr = vad_process(a->vad, a->vad_accum, 480);
        a->vad_accum_count = 0;

        if (a->debug_chunking) {
            const bool state_change = (vr.is_speech != a->debug_last_vad_speech) || vr.speech_ended;
            const bool periodic = vr.is_speech && ((a->debug_audio_cb_count % 10) == 0); // throttle
            if (state_change || periodic) {
                dbg_chunk(a,
                          "audio_cb=%llu vad_frame=%zums vad_is_speech=%d vad_speech_ended=%d vr.count=%zu rec_count=%zu",
                          (unsigned long long)a->debug_audio_cb_count,
                          (size_t)(1000 * 480 / SAMPLE_RATE),
                          (int)vr.is_speech,
                          (int)vr.speech_ended,
                          vr.count,
                          a->rec_count);
            }
            a->debug_last_vad_speech = vr.is_speech;
        }

        if (vr.samples && vr.count > 0) {
            // Append to recording buffer
            if (ensure_rec_capacity(a, vr.count)) {
                memcpy(a->rec_buffer + a->rec_count, vr.samples, vr.count * sizeof(float));
                a->rec_count += vr.count;
            } else {
                fprintf(stderr, "Out of memory while recording (dropping audio)\n");
            }
            free(vr.samples);
        }

        // If we just transitioned from speech to silence, enqueue the chunk for transcription.
        if (vr.speech_ended) {
            if (a->rec_count > 0) {
                pad_recording_tail(a);
                AudioChunk *chunk = calloc(1, sizeof(*chunk));
                chunk->samples = a->rec_buffer;
                chunk->count = a->rec_count;
                chunk->flush = false;

                a->rec_buffer = NULL;
                a->rec_count = 0;
                a->rec_capacity = 0;

                g_async_queue_push(a->chunk_queue, chunk);
                dbg_chunk(a, "enqueued chunk: samples=%zu secs=%.2f", chunk->count, (double)chunk->count / (double)SAMPLE_RATE);
            }
        }
    }
}

static void on_hotkey(void *userdata) {
    (void)userdata;
    if (!app || app->shutting_down || app->in_settings) return;

    // Ignore hotkey presses while processing so we don't queue a "start recording"
    // that fires immediately after transcription finishes.
    if (app->state == STATE_PROCESSING) return;

    // Debounce (repeat events / key auto-repeat).
    const gint64 now_us = g_get_monotonic_time();
    if (app->last_hotkey_us && (now_us - app->last_hotkey_us) < 200000) {
        return;
    }
    app->last_hotkey_us = now_us;

    // Ensure only one toggle is queued at a time.
    if (!g_atomic_int_compare_and_exchange(&app->hotkey_toggle_queued, 0, 1)) {
        return;
    }

    // Must run on main thread for GTK
    g_idle_add(toggle_from_hotkey, NULL);
}

static gboolean toggle_from_hotkey(gpointer data) {
    (void)data;
    if (app && !app->shutting_down && !app->in_settings) {
        app_toggle_recording();
    }
    g_atomic_int_set(&app->hotkey_toggle_queued, 0);
    return G_SOURCE_REMOVE;
}

void app_toggle_recording(void) {
    printf("app_toggle_recording called, state=%d\n", app->state);
    fflush(stdout);
    if (app->state == STATE_IDLE) {
        app_start_recording();
    } else if (app->state == STATE_RECORDING) {
        app_stop_recording();
    }
}

void app_start_recording(void) {
    if (app->state != STATE_IDLE) return;

    cancel_model_unload_timer(app);
    
    printf("app_start_recording: checking model...\n");
    fflush(stdout);
    
    if (!app->config->model_path || !*app->config->model_path) {
        fprintf(stderr, "No model selected (open Settings to choose/download a model)\n");
        return;
    }

    EngineType type = ENGINE_WHISPER;
    if (app->config->model_id && strstr(app->config->model_id, "parakeet")) {
        type = ENGINE_PARAKEET;
    }
        if (!transcriber_is_loaded(app->transcriber) && !transcriber_is_loading(app->transcriber)) {
            if (!transcriber_load_async(app->transcriber, type, app->config->model_path)) {
                fprintf(stderr, "Failed to start model load: %s\n", app->config->model_path);
                return;
            }
        }
    
    printf("app_start_recording: starting audio (model loads in background)...\n");
    fflush(stdout);
    
    app->rec_count = 0;
    app->vad_accum_count = 0;
    vad_reset(app->vad);
    overlay_set_level(app, 0.0f);
    g_atomic_int_set(&app->pasted_any, 0);
    g_atomic_int_set(&app->shown_transcribe_error, 0);
    if (app->overlay_text) g_string_assign(app->overlay_text, "");

    // Capture target window early so we can paste back into it later (X11 only).
    app->target_x11_window = x11_get_active_window();
    app->stop_requested = false;

    // Reset accumulated text for this session and drain any leftover chunks.
    g_mutex_lock(&app->accum_mutex);
    g_string_assign(app->accum_text, "");
    g_mutex_unlock(&app->accum_mutex);
    for (;;) {
        gpointer item = g_async_queue_try_pop(app->chunk_queue);
        if (!item) break;
        if (item == CHUNK_QUEUE_SENTINEL) continue;
        AudioChunk *left = item;
        if (left->samples) free(left->samples);
        free(left);
    }
    
    if (!audio_capture_start(app->audio)) {
        fprintf(stderr, "Failed to start audio capture\n");
        return;
    }
    
    app->state = STATE_RECORDING;
    overlay_show(app);
    tray_set_recording(true);
    printf("Recording started\n");
    fflush(stdout);
    
    // Update tray icon/menu
    if (app->status_item) {
        gtk_menu_item_set_label(GTK_MENU_ITEM(app->status_item), "Recording...");
    }
}

void app_stop_recording(void) {
    if (app->state != STATE_RECORDING) return;
    
    audio_capture_stop(app->audio);
    app->state = STATE_PROCESSING;
    tray_set_recording(false);
    overlay_hide(app);
    
    printf("Recording stopped\n");
    
    if (app->status_item) {
        gtk_menu_item_set_label(GTK_MENU_ITEM(app->status_item), "Processing...");
    }

    // Enqueue any trailing speech.
    if (app->rec_count > 0) {
        pad_recording_tail(app);
        AudioChunk *chunk = calloc(1, sizeof(*chunk));
        chunk->samples = app->rec_buffer;
        chunk->count = app->rec_count;
        chunk->flush = false;
        app->rec_buffer = NULL;
        app->rec_count = 0;
        app->rec_capacity = 0;
        g_async_queue_push(app->chunk_queue, chunk);
    }

    // Enqueue a flush marker so the worker knows to finalize and paste.
    app->stop_requested = true;
    AudioChunk *flush = calloc(1, sizeof(*flush));
    flush->flush = true;
    g_async_queue_push(app->chunk_queue, flush);
}

static gpointer worker_thread_main(gpointer data) {
    App *a = data;
    for (;;) {
        gpointer item = g_async_queue_pop(a->chunk_queue);
        if (item == CHUNK_QUEUE_SENTINEL) break;
        AudioChunk *chunk = item;

        if (chunk->flush) {
            free(chunk);
            FinalizePaste *fp = calloc(1, sizeof(*fp));
            fp->target_window = a->target_x11_window;
            g_idle_add(finalize_paste_idle, fp);
            dbg_chunk(a, "worker: flush received");
            continue;
        }

        if (chunk->samples && chunk->count > 0) {
            // whisper.cpp refuses very short inputs; pad trailing silence to a safe minimum.
            // (Some internal framing can effectively drop ~10ms, so use 1010ms, not 1000ms.)
            const size_t min_samples = (size_t)SAMPLE_RATE + 160;
            if (chunk->count < min_samples) {
                const size_t prev = chunk->count;
                float *padded = realloc(chunk->samples, min_samples * sizeof(float));
                if (!padded) {
                    dbg_chunk(a, "worker: OOM padding short chunk (samples=%zu), dropping", chunk->count);
                    free(chunk->samples);
                    free(chunk);
                    continue;
                }
                memset(padded + prev, 0, (min_samples - prev) * sizeof(float));
                chunk->samples = padded;
                chunk->count = min_samples;
                dbg_chunk(a, "worker: padded short chunk %zu -> %zu samples", prev, chunk->count);
            }

            const gint64 t0_us = g_get_monotonic_time();
            dbg_chunk(a, "worker: processing chunk samples=%zu secs=%.2f", chunk->count, (double)chunk->count / (double)SAMPLE_RATE);
            char *err = NULL;
            char *text = transcriber_process_ex(a->transcriber, chunk->samples, chunk->count,
                                                a->config->language, a->config->translate_to_english,
                                                &err);
            const gint64 t1_us = g_get_monotonic_time();
            dbg_chunk(a, "worker: transcribe done in %.2fs (text_len=%zu)",
                      (double)(t1_us - t0_us) / 1000000.0,
                      text ? strlen(text) : 0);
            if (!text && err && !g_atomic_int_get(&a->shown_transcribe_error)) {
                ErrorDialog *ed = calloc(1, sizeof(*ed));
                if (ed) {
                    if (strstr(err, "ErrorOutOfDeviceMemory") || strstr(err, "out of device memory")) {
                        ed->message = strdup(
                            "GPU ran out of memory while transcribing.\n\n"
                            "Try one of:\n"
                            "- Select a smaller model\n"
                            "- Disable GPU (set AURISCRIBE_NO_GPU=1)\n"
                            "- Close other GPU-heavy apps\n\n"
                            "Details:\n");
                        if (ed->message) {
                            const size_t n = strlen(ed->message) + strlen(err) + 1;
                            ed->message = realloc(ed->message, n);
                            if (ed->message) strcat(ed->message, err);
                        }
                    } else {
                        ed->message = strdup(err);
                    }
                    g_idle_add(show_transcribe_error_idle, ed);
                    g_atomic_int_set(&a->shown_transcribe_error, 1);
                }
            }
            free(err);
            if (text && *text) {
                g_mutex_lock(&a->accum_mutex);
                if (a->accum_text->len > 0) g_string_append_c(a->accum_text, ' ');
                g_string_append(a->accum_text, text);
                g_mutex_unlock(&a->accum_mutex);

                // Live overlay transcript preview (main thread).
                const bool out_overlay = a->config && a->config->chunk_output && (strcmp(a->config->chunk_output, "overlay") == 0 || strcmp(a->config->chunk_output, "both") == 0);
                const bool out_target = a->config && a->config->chunk_output && (strcmp(a->config->chunk_output, "target") == 0 || strcmp(a->config->chunk_output, "both") == 0);
                if (out_overlay) {
                    OverlayAppend *oa = calloc(1, sizeof(*oa));
                    if (oa) {
                        oa->app = a;
                        oa->text = strdup(text);
                        g_idle_add(overlay_append_idle, oa);
                    }
                }

                // Optional: paste each chunk immediately (X11 target window captured at start).
                if (a->config && a->config->paste_each_chunk && out_target) {
                    const bool is_wayland = getenv("WAYLAND_DISPLAY") != NULL;
                    if (!is_wayland) {
                        char *to_paste = NULL;
                        if (g_atomic_int_get(&a->pasted_any) && text[0] != ' ' && text[0] != '\n' && text[0] != '\t') {
                            to_paste = malloc(strlen(text) + 2);
                            if (to_paste) {
                                to_paste[0] = ' ';
                                strcpy(to_paste + 1, text);
                            }
                        }
                        const char *payload = to_paste ? to_paste : text;

                        PasteMethod method = PASTE_AUTO;
                        if (a->config->paste_method && strcmp(a->config->paste_method, "xdotool") == 0) method = PASTE_XDOTOOL;
                        else if (a->config->paste_method && strcmp(a->config->paste_method, "clipboard") == 0) method = PASTE_CLIPBOARD;

                        (void)paste_text_to_x11_window(payload, method, a->target_x11_window);
                        g_atomic_int_set(&a->pasted_any, 1);
                        free(to_paste);
                    }
                }
            }
            free(text);
        }

        free(chunk->samples);
        free(chunk);
    }
    return NULL;
}

static gboolean overlay_append_idle(gpointer data) {
    OverlayAppend *oa = data;
    if (!oa) return G_SOURCE_REMOVE;
    App *a = oa->app;
    if (a && !a->shutting_down && a->overlay_window && oa->text && *oa->text) {
        overlay_append_text(a, oa->text);
        if (a->overlay_area) gtk_widget_queue_draw(a->overlay_area);
    }
    free(oa->text);
    free(oa);
    return G_SOURCE_REMOVE;
}

static gboolean show_transcribe_error_idle(gpointer data) {
    ErrorDialog *ed = data;
    if (!ed) return G_SOURCE_REMOVE;
    if (app && !app->shutting_down) {
        GtkWidget *dlg = gtk_message_dialog_new(
            NULL,
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR,
            GTK_BUTTONS_OK,
            "%s",
            ed->message ? ed->message : "Transcription failed");
        gtk_window_set_title(GTK_WINDOW(dlg), "Auriscribe error");
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
    }
    free(ed->message);
    free(ed);
    return G_SOURCE_REMOVE;
}

static gboolean finalize_paste_idle(gpointer data) {
    FinalizePaste *fp = data;
    if (!app || app->shutting_down) {
        free(fp);
        return G_SOURCE_REMOVE;
    }

    char *final_text = NULL;
    g_mutex_lock(&app->accum_mutex);
    if (app->accum_text->len > 0) {
        final_text = strdup(app->accum_text->str);
    }
    g_mutex_unlock(&app->accum_mutex);

    const bool out_target = app->config && app->config->chunk_output && (strcmp(app->config->chunk_output, "target") == 0 || strcmp(app->config->chunk_output, "both") == 0);
    if (final_text && *final_text && !(app->config && app->config->paste_each_chunk && out_target)) {
        PasteMethod method = PASTE_AUTO;
        if (app->config->paste_method && strcmp(app->config->paste_method, "xdotool") == 0) method = PASTE_XDOTOOL;
        else if (app->config->paste_method && strcmp(app->config->paste_method, "wtype") == 0) method = PASTE_WTYPE;
        else if (app->config->paste_method && strcmp(app->config->paste_method, "clipboard") == 0) method = PASTE_CLIPBOARD;

        paste_text_to_x11_window(final_text, method, fp->target_window);
    }
    free(final_text);

    app->state = STATE_IDLE;
    tray_set_recording(false);
    if (app->status_item) {
        gtk_menu_item_set_label(GTK_MENU_ITEM(app->status_item), "Ready");
    }

    // Model was just used for transcription; schedule unload after idle.
    app->model_last_used_us = g_get_monotonic_time();
    schedule_model_unload_timer(app);

    // Ensure we have an empty buffer ready for the next recording session
    if (!app->rec_buffer) {
        app->rec_capacity = SAMPLE_RATE * 10;
        app->rec_buffer = malloc(app->rec_capacity * sizeof(float));
    }

    // Encourage RSS to drop after large transient allocations (audio buffers, model load/unload).
    try_trim_heap();

    free(fp);
    return G_SOURCE_REMOVE;
}

void app_show_settings(void) {
    if (app->state == STATE_PROCESSING) {
        fprintf(stderr, "Cannot open settings while processing transcription\n");
        return;
    }

    // Pause our own global hotkey while settings are open so the key combo can be captured.
    app->in_settings = true;
    if (app->hotkey) {
        hotkey_free(app->hotkey);
        app->hotkey = NULL;
    }
    app->hotkey_available = false;

    if (app->hotkey_item) {
        gtk_menu_item_set_label(GTK_MENU_ITEM(app->hotkey_item), "Hotkey: paused in settings");
    }

    char *prev_hotkey = app->config->hotkey ? strdup(app->config->hotkey) : NULL;
    char *prev_model_path = app->config->model_path ? strdup(app->config->model_path) : NULL;
    const bool prev_overlay_enabled = app->config->overlay_enabled;
    char *prev_overlay_pos = app->config->overlay_position ? strdup(app->config->overlay_position) : NULL;

    settings_dialog_show(NULL, app->config);

    const bool hotkey_changed =
        (prev_hotkey == NULL && app->config->hotkey != NULL) ||
        (prev_hotkey != NULL && app->config->hotkey == NULL) ||
        (prev_hotkey && app->config->hotkey && strcmp(prev_hotkey, app->config->hotkey) != 0);
    free(prev_hotkey);

    const bool model_changed =
        (prev_model_path == NULL && app->config->model_path != NULL) ||
        (prev_model_path != NULL && app->config->model_path == NULL) ||
        (prev_model_path && app->config->model_path && strcmp(prev_model_path, app->config->model_path) != 0);
    free(prev_model_path);

    const bool overlay_changed =
        (prev_overlay_enabled != app->config->overlay_enabled) ||
        (prev_overlay_pos == NULL && app->config->overlay_position != NULL) ||
        (prev_overlay_pos != NULL && app->config->overlay_position == NULL) ||
        (prev_overlay_pos && app->config->overlay_position && strcmp(prev_overlay_pos, app->config->overlay_position) != 0);
    free(prev_overlay_pos);
    
    // Reload audio device if changed
    audio_capture_free(app->audio);
    app->audio = audio_capture_new(app->config->microphone);
    audio_capture_set_callback(app->audio, on_audio_data, app);
    
    // Update VAD threshold
    vad_free(app->vad);
    app->vad = vad_new_energy(app->config->vad_threshold);

    // On-demand model loading: if settings changed model, unload now so next use loads the new one.
    if (model_changed && transcriber_is_loaded(app->transcriber)) {
        cancel_model_unload_timer(app);
        transcriber_unload(app->transcriber);
    }

    if (overlay_changed && app->state == STATE_RECORDING) {
        if (app->config->overlay_enabled) {
            overlay_show(app);
        } else {
            overlay_hide(app);
        }
    }

    // Always re-register: we paused the hotkey during settings.
    (void)hotkey_changed;
    app->hotkey = hotkey_new(app->config->hotkey);
    hotkey_set_callback(app->hotkey, on_hotkey, app);
    app->hotkey_available = hotkey_start(app->hotkey);
    app->in_settings = false;

    if (app->hotkey_item) {
        char hk_buf[256];
        if (app->hotkey_available) {
            snprintf(hk_buf, sizeof(hk_buf), "Hotkey: %s", app->config->hotkey ? app->config->hotkey : "");
        } else {
            snprintf(hk_buf, sizeof(hk_buf), "Hotkey: unavailable (bind SIGUSR2)");
        }
        gtk_menu_item_set_label(GTK_MENU_ITEM(app->hotkey_item), hk_buf);
    }
}

void app_show_download(void) {
    download_dialog_show(NULL, on_model_downloaded, app);
}

static void on_model_downloaded(const char *model_id, const char *model_path, void *userdata) {
    App *a = userdata;
    if (!a || !model_id || !model_path) return;

    free(a->config->model_id);
    free(a->config->model_path);
    a->config->model_id = strdup(model_id);
    a->config->model_path = strdup(model_path);
    config_save(a->config);

    EngineType type = ENGINE_WHISPER;
    if (strstr(model_id, "parakeet")) type = ENGINE_PARAKEET;

    (void)type;
    // On-demand model loading: keep memory free until recording starts.
    cancel_model_unload_timer(a);
    transcriber_unload(a->transcriber);
}
