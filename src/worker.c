#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
// whisper.cpp header (vendored)
#include "whisper.h"

static bool read_exact(int fd, void *buf, size_t n) {
    uint8_t *p = (uint8_t *)buf;
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, p + off, n - off);
        if (r == 0) return false;
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += (size_t)r;
    }
    return true;
}

static bool write_exact(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += (size_t)w;
    }
    return true;
}

static bool read_u32(int fd, uint32_t *out) {
    uint8_t b[4];
    if (!read_exact(fd, b, sizeof(b))) return false;
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return true;
}

static bool write_u32(int fd, uint32_t v) {
    uint8_t b[4] = {
        (uint8_t)(v & 0xff),
        (uint8_t)((v >> 8) & 0xff),
        (uint8_t)((v >> 16) & 0xff),
        (uint8_t)((v >> 24) & 0xff),
    };
    return write_exact(fd, b, sizeof(b));
}

static bool read_u8(int fd, uint8_t *out) {
    return read_exact(fd, out, 1);
}

static bool write_u8(int fd, uint8_t v) {
    return write_exact(fd, &v, 1);
}

static bool write_msg(int fd, char type, const char *s) {
    const char magic[4] = { 'A', 'U', 'R', '1' };
    if (!write_exact(fd, magic, 4)) return false;
    if (!write_u8(fd, (uint8_t)type)) return false;
    const uint32_t n = s ? (uint32_t)strlen(s) : 0;
    if (!write_u32(fd, n)) return false;
    if (n && !write_exact(fd, s, n)) return false;
    return true;
}

static char *read_bytes_str(int fd, uint32_t n) {
    char *s = calloc(1, (size_t)n + 1);
    if (!s) return NULL;
    if (n && !read_exact(fd, s, n)) {
        free(s);
        return NULL;
    }
    s[n] = '\0';
    return s;
}

static char *trim_leading_space(char *s) {
    if (!s) return NULL;
    size_t i = 0;
    while (s[i] == ' ') i++;
    if (i == 0) return s;
    memmove(s, s + i, strlen(s + i) + 1);
    return s;
}

static char *whisper_run(struct whisper_context *ctx, const float *samples, int n_samples,
                         const char *language, bool translate, int n_threads) {
    struct whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.n_threads = n_threads;
    params.print_progress = false;
    params.print_special = false;
    params.print_realtime = false;
    params.print_timestamps = false;
    params.translate = translate;
    params.single_segment = true;
    params.no_context = true;

    if (language && *language) {
        params.language = language;
        params.detect_language = false;
    } else {
        params.detect_language = true;
        params.language = NULL;
    }

    if (whisper_full(ctx, params, samples, n_samples) != 0) {
        return NULL;
    }

    const int n_segments = whisper_full_n_segments(ctx);
    if (n_segments <= 0) return strdup("");

    size_t total_len = 0;
    for (int i = 0; i < n_segments; i++) {
        const char *t = whisper_full_get_segment_text(ctx, i);
        if (t) total_len += strlen(t);
    }

    char *out = malloc(total_len + 1);
    if (!out) return NULL;
    out[0] = '\0';
    for (int i = 0; i < n_segments; i++) {
        const char *t = whisper_full_get_segment_text(ctx, i);
        if (t) strcat(out, t);
    }

    return trim_leading_space(out);
}

int main(void) {
    const int in_fd = STDIN_FILENO;
    const int out_fd = STDOUT_FILENO;

    struct whisper_context *ctx = NULL;

    for (;;) {
        char magic[4];
        if (!read_exact(in_fd, magic, 4)) break;
        if (memcmp(magic, "AURI", 4) != 0) {
            (void)write_msg(out_fd, 'E', "Bad magic");
            break;
        }

        uint8_t cmd = 0;
        if (!read_u8(in_fd, &cmd)) break;

        if (cmd == 'Q') {
            (void)write_msg(out_fd, 'O', "bye");
            break;
        }

        if (cmd == 'U') {
            if (ctx) {
                whisper_free(ctx);
                ctx = NULL;
            }
            (void)write_msg(out_fd, 'O', "unloaded");
            continue;
        }

        if (cmd == 'L') {
            uint32_t path_len = 0;
            uint32_t threads = 0;
            uint32_t gpu_device = 0;
            uint8_t use_gpu = 1;

            if (!read_u32(in_fd, &path_len)) break;
            char *path = read_bytes_str(in_fd, path_len);
            if (!path) {
                (void)write_msg(out_fd, 'E', "Out of memory");
                continue;
            }
            if (!read_u32(in_fd, &threads) || !read_u32(in_fd, &gpu_device) || !read_u8(in_fd, &use_gpu)) {
                free(path);
                break;
            }

            if (ctx) {
                whisper_free(ctx);
                ctx = NULL;
            }

            struct whisper_context_params cparams = whisper_context_default_params();
            cparams.use_gpu = use_gpu ? true : false;
            cparams.gpu_device = (int)gpu_device;

            ctx = whisper_init_from_file_with_params(path, cparams);
            free(path);

            if (!ctx) {
                (void)write_msg(out_fd, 'E', "Failed to load model");
                continue;
            }

            (void)write_msg(out_fd, 'O', "loaded");
            continue;
        }

        if (cmd == 'T') {
            if (!ctx) {
                (void)write_msg(out_fd, 'E', "No model loaded");
                continue;
            }

            uint32_t n_samples_u32 = 0;
            uint32_t lang_len = 0;
            uint8_t translate = 0;
            uint32_t n_threads = 0;

            if (!read_u32(in_fd, &n_samples_u32)) break;
            if (!read_u32(in_fd, &lang_len)) break;
            char *lang = read_bytes_str(in_fd, lang_len);
            if (!lang && lang_len != 0) {
                (void)write_msg(out_fd, 'E', "Out of memory");
                continue;
            }
            if (!read_u8(in_fd, &translate) || !read_u32(in_fd, &n_threads)) {
                free(lang);
                break;
            }

            const size_t n_samples = (size_t)n_samples_u32;
            float *samples = NULL;
            if (n_samples > 0) {
                samples = malloc(n_samples * sizeof(float));
                if (!samples) {
                    free(lang);
                    (void)write_msg(out_fd, 'E', "Out of memory");
                    continue;
                }
                if (!read_exact(in_fd, samples, n_samples * sizeof(float))) {
                    free(samples);
                    free(lang);
                    break;
                }
            }

            char *text = whisper_run(ctx, samples, (int)n_samples_u32, lang, translate != 0, (int)n_threads);
            free(samples);
            free(lang);

            if (!text) {
                (void)write_msg(out_fd, 'E', "Transcription failed");
                continue;
            }

            const char magic2[4] = { 'A', 'U', 'R', '1' };
            (void)write_exact(out_fd, magic2, 4);
            (void)write_u8(out_fd, (uint8_t)'R');
            (void)write_u32(out_fd, (uint32_t)strlen(text));
            (void)write_exact(out_fd, text, strlen(text));
            free(text);
            continue;
        }

        (void)write_msg(out_fd, 'E', "Unknown command");
        break;
    }

    if (ctx) whisper_free(ctx);
    return 0;
}
