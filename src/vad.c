#include "vad.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define PREFILL_FRAMES 10
#define HANGOVER_FRAMES 15
#define ONSET_FRAMES 2

struct VAD {
    float threshold;
    
    // Ring buffer for prefill
    float *prefill_buf;
    size_t prefill_size;
    size_t prefill_pos;
    size_t prefill_count;
    
    // State
    bool in_speech;
    int onset_counter;
    int hangover_counter;
};

VAD *vad_new_energy(float threshold) {
    VAD *vad = calloc(1, sizeof(VAD));
    vad->threshold = threshold;
    vad->prefill_size = PREFILL_FRAMES * 480;  // ~30ms frames at 16kHz
    vad->prefill_buf = calloc(vad->prefill_size, sizeof(float));
    return vad;
}

static float compute_rms(const float *samples, size_t count) {
    if (count == 0) return 0;
    float sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum += samples[i] * samples[i];
    }
    return sqrtf(sum / count);
}

static void prefill_push(VAD *vad, const float *samples, size_t count) {
    for (size_t i = 0; i < count; i++) {
        vad->prefill_buf[vad->prefill_pos] = samples[i];
        vad->prefill_pos = (vad->prefill_pos + 1) % vad->prefill_size;
        if (vad->prefill_count < vad->prefill_size) {
            vad->prefill_count++;
        }
    }
}

static size_t prefill_get(VAD *vad, float **out) {
    if (vad->prefill_count == 0) {
        *out = NULL;
        return 0;
    }
    
    *out = malloc(vad->prefill_count * sizeof(float));
    size_t start = (vad->prefill_pos + vad->prefill_size - vad->prefill_count) % vad->prefill_size;
    
    for (size_t i = 0; i < vad->prefill_count; i++) {
        (*out)[i] = vad->prefill_buf[(start + i) % vad->prefill_size];
    }
    
    return vad->prefill_count;
}

VADResult vad_process(VAD *vad, const float *samples, size_t count) {
    VADResult result = {0};
    
    float rms = compute_rms(samples, count);
    bool is_voice = rms > vad->threshold;
    
    prefill_push(vad, samples, count);
    
    if (!vad->in_speech && is_voice) {
        vad->onset_counter++;
        if (vad->onset_counter >= ONSET_FRAMES) {
            vad->in_speech = true;
            vad->hangover_counter = HANGOVER_FRAMES;
            vad->onset_counter = 0;
            
            // Return prefill buffer + current frame
            float *prefill;
            size_t prefill_count = prefill_get(vad, &prefill);
            
            result.count = prefill_count;
            result.samples = prefill;
            result.is_speech = true;
        }
    } else if (vad->in_speech && is_voice) {
        vad->hangover_counter = HANGOVER_FRAMES;
        
        result.samples = malloc(count * sizeof(float));
        memcpy(result.samples, samples, count * sizeof(float));
        result.count = count;
        result.is_speech = true;
    } else if (vad->in_speech && !is_voice) {
        if (vad->hangover_counter > 0) {
            vad->hangover_counter--;
            
            result.samples = malloc(count * sizeof(float));
            memcpy(result.samples, samples, count * sizeof(float));
            result.count = count;
            result.is_speech = true;
        } else {
            vad->in_speech = false;
            result.is_speech = false;
            result.speech_ended = true;
        }
    } else {
        vad->onset_counter = 0;
        result.is_speech = false;
    }
    
    return result;
}

void vad_reset(VAD *vad) {
    vad->in_speech = false;
    vad->onset_counter = 0;
    vad->hangover_counter = 0;
    vad->prefill_pos = 0;
    vad->prefill_count = 0;
    memset(vad->prefill_buf, 0, vad->prefill_size * sizeof(float));
}

void vad_free(VAD *vad) {
    if (!vad) return;
    free(vad->prefill_buf);
    free(vad);
}
