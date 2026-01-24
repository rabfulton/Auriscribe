#include "transcribe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// whisper.cpp header
#include "whisper.h"

static int transcriber_default_threads(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    return (int)n;
}

static int transcriber_threads(void) {
    const char *env = getenv("XFCE_WHISPER_THREADS");
    if (!env || !*env) return transcriber_default_threads();
    int n = atoi(env);
    if (n < 1) n = 1;
    if (n > 64) n = 64;
    return n;
}

struct Transcriber {
    EngineType type;
    struct whisper_context *whisper_ctx;
    // Parakeet would go here with ONNX runtime
};

Transcriber *transcriber_new(void) {
    return calloc(1, sizeof(Transcriber));
}

bool transcriber_load(Transcriber *t, EngineType type, const char *model_path) {
    transcriber_unload(t);
    
    if (type == ENGINE_WHISPER) {
        struct whisper_context_params params = whisper_context_default_params();

        // Prefer GPU (Vulkan/CUDA/Metal/etc.) when whisper.cpp was built with it.
        // Set XFCE_WHISPER_NO_GPU=1 to force CPU.
        if (getenv("XFCE_WHISPER_NO_GPU")) {
            params.use_gpu = false;
        }

        const char *gpu_device = getenv("XFCE_WHISPER_GPU_DEVICE");
        if (gpu_device && *gpu_device) {
            params.gpu_device = atoi(gpu_device);
        }

        t->whisper_ctx = whisper_init_from_file_with_params(model_path, params);
        if (!t->whisper_ctx) {
            fprintf(stderr, "Failed to load whisper model: %s\n", model_path);
            return false;
        }
        t->type = ENGINE_WHISPER;
        return true;
    }
    
    if (type == ENGINE_PARAKEET) {
        // TODO: Implement Parakeet with ONNX Runtime
        fprintf(stderr, "Parakeet not yet implemented\n");
        return false;
    }
    
    return false;
}

void transcriber_unload(Transcriber *t) {
    if (t->whisper_ctx) {
        whisper_free(t->whisper_ctx);
        t->whisper_ctx = NULL;
    }
    t->type = ENGINE_NONE;
}

void transcriber_free(Transcriber *t) {
    if (!t) return;
    transcriber_unload(t);
    free(t);
}

bool transcriber_is_loaded(Transcriber *t) {
    return t && t->type != ENGINE_NONE;
}

EngineType transcriber_get_type(Transcriber *t) {
    return t ? t->type : ENGINE_NONE;
}

char *transcriber_process(Transcriber *t, const float *samples, size_t count,
                          const char *language, bool translate) {
    if (!t || t->type == ENGINE_NONE) return NULL;
    
    if (t->type == ENGINE_WHISPER) {
        struct whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        
        params.n_threads = transcriber_threads();
        params.print_progress = false;
        params.print_special = false;
        params.print_realtime = false;
        params.print_timestamps = false;
        params.translate = translate;
        params.single_segment = true;
        params.no_context = true;
        
        if (language && strcmp(language, "auto") != 0) {
            params.language = language;
            params.detect_language = false;
        } else {
            params.detect_language = true;
            params.language = NULL;
        }
        
        if (whisper_full(t->whisper_ctx, params, samples, (int)count) != 0) {
            fprintf(stderr, "Whisper transcription failed\n");
            return NULL;
        }
        
        int n_segments = whisper_full_n_segments(t->whisper_ctx);
        if (n_segments == 0) return strdup("");
        
        // Concatenate all segments
        size_t total_len = 0;
        for (int i = 0; i < n_segments; i++) {
            const char *text = whisper_full_get_segment_text(t->whisper_ctx, i);
            if (text) total_len += strlen(text);
        }
        
        char *result = malloc(total_len + 1);
        result[0] = '\0';
        
        for (int i = 0; i < n_segments; i++) {
            const char *text = whisper_full_get_segment_text(t->whisper_ctx, i);
            if (text) strcat(result, text);
        }
        
        // Trim leading space that whisper often adds
        char *trimmed = result;
        while (*trimmed == ' ') trimmed++;
        if (trimmed != result) {
            memmove(result, trimmed, strlen(trimmed) + 1);
        }
        
        return result;
    }
    
    return NULL;
}
