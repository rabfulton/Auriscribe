#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/pulseaudio.h>

struct AudioCapture {
    pa_simple *pa;
    char *device;
    AudioCallback callback;
    void *userdata;
    pthread_t thread;
    volatile bool running;
};

// 40ms frames at 16kHz. The app layer aggregates into 30ms frames for VAD.
// (Overlay uses per-callback level updates.)
#define AUDIO_FRAME_SAMPLES 640

static void *capture_thread(void *arg) {
    AudioCapture *ac = arg;
    int16_t buf[AUDIO_FRAME_SAMPLES];
    float fbuf[AUDIO_FRAME_SAMPLES];
    int err;

    while (ac->running) {
        if (pa_simple_read(ac->pa, buf, sizeof(buf), &err) < 0) {
            fprintf(stderr, "PulseAudio read error: %s\n", pa_strerror(err));
            break;
        }

        // Convert int16 to float
        size_t count = sizeof(buf) / sizeof(buf[0]);
        for (size_t i = 0; i < count; i++) {
            fbuf[i] = buf[i] / 32768.0f;
        }

        if (ac->callback) {
            ac->callback(fbuf, count, ac->userdata);
        }
    }

    return NULL;
}

AudioCapture *audio_capture_new(const char *device) {
    AudioCapture *ac = calloc(1, sizeof(AudioCapture));
    ac->device = device ? strdup(device) : NULL;
    return ac;
}

void audio_capture_set_callback(AudioCapture *ac, AudioCallback cb, void *userdata) {
    ac->callback = cb;
    ac->userdata = userdata;
}

bool audio_capture_start(AudioCapture *ac) {
    if (ac->running) return true;

    pa_sample_spec ss = {
        .format = PA_SAMPLE_S16LE,
        .rate = SAMPLE_RATE,
        .channels = 1
    };

    int err;
    pa_buffer_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.maxlength = (uint32_t)-1;
    attr.tlength   = (uint32_t)-1;
    attr.prebuf    = (uint32_t)-1;
    attr.minreq    = (uint32_t)-1;
    // Hint to the server that we want small, low-latency capture chunks.
    attr.fragsize  = (uint32_t)(AUDIO_FRAME_SAMPLES * sizeof(int16_t));

    ac->pa = pa_simple_new(NULL, "auriscribe", PA_STREAM_RECORD,
                           ac->device, "Speech Input", &ss, NULL, &attr, &err);

    if (!ac->pa) {
        fprintf(stderr, "PulseAudio connection failed: %s\n", pa_strerror(err));
        return false;
    }

    ac->running = true;
    pthread_create(&ac->thread, NULL, capture_thread, ac);
    return true;
}

void audio_capture_stop(AudioCapture *ac) {
    if (!ac->running) return;

    ac->running = false;
    pthread_join(ac->thread, NULL);

    if (ac->pa) {
        pa_simple_free(ac->pa);
        ac->pa = NULL;
    }
}

void audio_capture_free(AudioCapture *ac) {
    if (!ac) return;
    audio_capture_stop(ac);
    free(ac->device);
    free(ac);
}

// Device enumeration using PulseAudio async API
static pa_context *ctx;
static pa_mainloop *ml;
static AudioDevice *devices;
static int device_count;
static int device_capacity;

static void source_info_cb(pa_context *c, const pa_source_info *i, int eol, void *userdata) {
    (void)c; (void)userdata;
    if (eol) {
        pa_mainloop_quit(ml, 0);
        return;
    }

    // Skip monitors (output devices)
    if (i->monitor_of_sink != PA_INVALID_INDEX) return;

    if (device_count >= device_capacity) {
        device_capacity = device_capacity ? device_capacity * 2 : 8;
        devices = realloc(devices, device_capacity * sizeof(AudioDevice));
    }

    devices[device_count].name = strdup(i->name);
    devices[device_count].description = strdup(i->description);
    devices[device_count].is_default = false;  // Set later
    device_count++;
}

static void context_state_cb(pa_context *c, void *userdata) {
    (void)userdata;
    if (pa_context_get_state(c) == PA_CONTEXT_READY) {
        pa_context_get_source_info_list(c, source_info_cb, NULL);
    }
}

AudioDevice *audio_list_devices(int *count) {
    devices = NULL;
    device_count = 0;
    device_capacity = 0;

    ml = pa_mainloop_new();
    pa_mainloop_api *api = pa_mainloop_get_api(ml);
    ctx = pa_context_new(api, "auriscribe-enum");

    pa_context_set_state_callback(ctx, context_state_cb, NULL);
    pa_context_connect(ctx, NULL, PA_CONTEXT_NOFLAGS, NULL);

    pa_mainloop_run(ml, NULL);

    pa_context_disconnect(ctx);
    pa_context_unref(ctx);
    pa_mainloop_free(ml);

    *count = device_count;
    return devices;
}

void audio_free_devices(AudioDevice *devs, int count) {
    for (int i = 0; i < count; i++) {
        free(devs[i].name);
        free(devs[i].description);
    }
    free(devs);
}
