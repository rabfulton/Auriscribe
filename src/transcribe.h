#ifndef TRANSCRIBE_H
#define TRANSCRIBE_H

#include <stddef.h>
#include <stdbool.h>

typedef enum {
    ENGINE_NONE,
    ENGINE_WHISPER,
    ENGINE_PARAKEET
} EngineType;

typedef struct Transcriber Transcriber;

Transcriber *transcriber_new(void);
bool transcriber_load(Transcriber *t, EngineType type, const char *model_path);
bool transcriber_load_async(Transcriber *t, EngineType type, const char *model_path);
void transcriber_unload(Transcriber *t);
void transcriber_free(Transcriber *t);

bool transcriber_is_loaded(Transcriber *t);
bool transcriber_is_loading(Transcriber *t);
EngineType transcriber_get_type(Transcriber *t);

// Returns allocated string, caller must free
char *transcriber_process(Transcriber *t, const float *samples, size_t count,
                          const char *language, bool translate);
char *transcriber_process_ex(Transcriber *t, const float *samples, size_t count,
                             const char *language, bool translate,
                             const char *initial_prompt,
                             char **error_out);

#endif
