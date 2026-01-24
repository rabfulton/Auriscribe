#include "app.h"
#include "paste.h"
#include "ui_settings.h"
#include "ui_download.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

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

typedef struct {
    float *samples;
    size_t count;
    bool flush;
} AudioChunk;

typedef struct {
    unsigned long target_window;
} FinalizePaste;

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
    app->stop_requested = false;
    
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
}

void app_cleanup(void) {
    if (!app) return;

    app->shutting_down = true;

    cancel_model_unload_timer(app);

    if (app->chunk_queue) {
        g_async_queue_push(app->chunk_queue, NULL); // sentinel to stop worker
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

    const gint64 now_us = g_get_monotonic_time();
    if (a->model_last_used_us && (now_us - a->model_last_used_us) < (15 * G_USEC_PER_SEC)) {
        return G_SOURCE_REMOVE;
    }

    transcriber_unload(a->transcriber);
    a->model_unload_timeout_id = 0;
    return G_SOURCE_REMOVE;
}

static void on_audio_data(const float *samples, size_t count, void *userdata) {
    App *a = userdata;
    if (a->state != STATE_RECORDING) return;
    
    if (getenv("XFCE_WHISPER_DEBUG_AUDIO")) {
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
    
    VADResult vr = vad_process(a->vad, samples, count);
    
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
            AudioChunk *chunk = calloc(1, sizeof(*chunk));
            chunk->samples = a->rec_buffer;
            chunk->count = a->rec_count;
            chunk->flush = false;

            a->rec_buffer = NULL;
            a->rec_count = 0;
            a->rec_capacity = 0;

            g_async_queue_push(a->chunk_queue, chunk);
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
    
    if (!transcriber_is_loaded(app->transcriber)) {
        if (!app->config->model_path || !*app->config->model_path) {
            fprintf(stderr, "No model selected (open Settings to choose/download a model)\n");
            return;
        }

        EngineType type = ENGINE_WHISPER;
        if (app->config->model_id && strstr(app->config->model_id, "parakeet")) {
            type = ENGINE_PARAKEET;
        }

        if (!transcriber_load(app->transcriber, type, app->config->model_path)) {
            fprintf(stderr, "Failed to load model: %s\n", app->config->model_path);
            return;
        }
    }
    
    printf("app_start_recording: model OK, starting audio...\n");
    fflush(stdout);
    
    app->rec_count = 0;
    vad_reset(app->vad);

    // Capture target window early so we can paste back into it later (X11 only).
    app->target_x11_window = x11_get_active_window();
    app->stop_requested = false;

    // Reset accumulated text for this session and drain any leftover chunks.
    g_mutex_lock(&app->accum_mutex);
    g_string_assign(app->accum_text, "");
    g_mutex_unlock(&app->accum_mutex);
    for (;;) {
        AudioChunk *left = g_async_queue_try_pop(app->chunk_queue);
        if (!left) break;
        if (left->samples) free(left->samples);
        free(left);
    }
    
    if (!audio_capture_start(app->audio)) {
        fprintf(stderr, "Failed to start audio capture\n");
        return;
    }
    
    app->state = STATE_RECORDING;
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
    
    printf("Recording stopped\n");
    
    if (app->status_item) {
        gtk_menu_item_set_label(GTK_MENU_ITEM(app->status_item), "Processing...");
    }

    // Enqueue any trailing speech.
    if (app->rec_count > 0) {
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
        AudioChunk *chunk = g_async_queue_pop(a->chunk_queue);
        if (chunk == NULL) break; // sentinel

        if (chunk->flush) {
            free(chunk);
            FinalizePaste *fp = calloc(1, sizeof(*fp));
            fp->target_window = a->target_x11_window;
            g_idle_add(finalize_paste_idle, fp);
            continue;
        }

        if (chunk->samples && chunk->count > 0) {
            char *text = transcriber_process(a->transcriber, chunk->samples, chunk->count,
                                             a->config->language, a->config->translate_to_english);
            if (text && *text) {
                g_mutex_lock(&a->accum_mutex);
                if (a->accum_text->len > 0) g_string_append_c(a->accum_text, ' ');
                g_string_append(a->accum_text, text);
                g_mutex_unlock(&a->accum_mutex);
            }
            free(text);
        }

        free(chunk->samples);
        free(chunk);
    }
    return NULL;
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

    if (final_text && *final_text) {
        PasteMethod method = PASTE_AUTO;
        if (app->config->paste_method && strcmp(app->config->paste_method, "xdotool") == 0) method = PASTE_XDOTOOL;
        else if (app->config->paste_method && strcmp(app->config->paste_method, "wtype") == 0) method = PASTE_WTYPE;
        else if (app->config->paste_method && strcmp(app->config->paste_method, "clipboard") == 0) method = PASTE_CLIPBOARD;

        paste_text_to_x11_window(final_text, method, fp->target_window);
    }
    free(final_text);

    app->state = STATE_IDLE;
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
