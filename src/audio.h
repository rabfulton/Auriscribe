#ifndef AUDIO_H
#define AUDIO_H

#include <stddef.h>
#include <stdbool.h>

#define SAMPLE_RATE 16000

typedef void (*AudioCallback)(const float *samples, size_t count, void *userdata);

typedef struct AudioCapture AudioCapture;

AudioCapture *audio_capture_new(const char *device);
void audio_capture_set_callback(AudioCapture *ac, AudioCallback cb, void *userdata);
bool audio_capture_start(AudioCapture *ac);
void audio_capture_stop(AudioCapture *ac);
void audio_capture_free(AudioCapture *ac);

// Device enumeration
typedef struct {
    char *name;
    char *description;
    bool is_default;
} AudioDevice;

AudioDevice *audio_list_devices(int *count);
void audio_free_devices(AudioDevice *devices, int count);

#endif
