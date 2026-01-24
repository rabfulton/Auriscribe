#ifndef VAD_H
#define VAD_H

#include <stddef.h>
#include <stdbool.h>

typedef struct VAD VAD;

// Energy-based VAD (no external dependencies)
VAD *vad_new_energy(float threshold);

// Process audio frame, returns speech samples (may be empty)
// Caller must free returned buffer
typedef struct {
    float *samples;
    size_t count;
    bool is_speech;
    bool speech_ended; // true on transition from speech -> silence
} VADResult;

VADResult vad_process(VAD *vad, const float *samples, size_t count);
void vad_reset(VAD *vad);
void vad_free(VAD *vad);

#endif
