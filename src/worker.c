#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
// whisper.cpp header (vendored)
#include "whisper.h"
#include "ggml-backend.h"

static const char *env_get(const char *preferred, const char *legacy) {
    const char *v = preferred ? getenv(preferred) : NULL;
    if (v && *v) return v;
    v = legacy ? getenv(legacy) : NULL;
    if (v && *v) return v;
    return NULL;
}

static uint64_t fnv1a64_update(uint64_t h, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t hash_file_fnv1a64(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    uint8_t buf[8192];
    uint64_t h = 1469598103934665603ULL;
    for (;;) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r == 0) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return 0;
        }
        h = fnv1a64_update(h, buf, (size_t)r);
    }
    close(fd);
    return h;
}

static bool ensure_dir(const char *path) {
    if (!path || !*path) return false;
    if (mkdir(path, 0700) == 0) return true;
    return errno == EEXIST;
}

static char *cache_dir(void) {
    const char *xdg = getenv("XDG_CACHE_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) {
        return strdup(xdg);
    }
    if (home && *home) {
        size_t n = strlen(home) + strlen("/.cache") + 1;
        char *p = malloc(n);
        if (!p) return NULL;
        snprintf(p, n, "%s/.cache", home);
        return p;
    }
    return NULL;
}

static int warmup_vulkan(void) {
    if (env_get("AURISCRIBE_NO_GPU", "XFCE_WHISPER_NO_GPU")) {
        return 0;
    }

    if (ggml_cpu_has_vulkan() != 1) {
        return 0;
    }

    const char *gpu_device_s = env_get("AURISCRIBE_GPU_DEVICE", "XFCE_WHISPER_GPU_DEVICE");
    size_t gpu_device = 0;
    if (gpu_device_s && *gpu_device_s) gpu_device = (size_t)atoi(gpu_device_s);

    uint64_t exe_hash = hash_file_fnv1a64("/proc/self/exe");
    const char *icd = env_get("AURISCRIBE_VK_ICD_FILENAMES", "XFCE_WHISPER_VK_ICD_FILENAMES");
    uint64_t icd_hash = 1469598103934665603ULL;
    if (icd && *icd) icd_hash = fnv1a64_update(icd_hash, icd, strlen(icd));

    char *base = cache_dir();
    if (!base) {
        fprintf(stderr, "vulkan-warmup: cannot resolve cache dir\n");
        return 0;
    }

    size_t appdir_n = strlen(base) + strlen("/auriscribe") + 1;
    char *appdir = malloc(appdir_n);
    if (!appdir) {
        free(base);
        return 0;
    }
    snprintf(appdir, appdir_n, "%s/auriscribe", base);
    free(base);

    if (!ensure_dir(appdir)) {
        free(appdir);
        return 0;
    }

    char stamp[512];
    snprintf(stamp, sizeof(stamp),
             "%s/vk-warmup-%016llx-dev%zu-icd%016llx.stamp",
             appdir,
             (unsigned long long)exe_hash,
             gpu_device,
             (unsigned long long)icd_hash);
    free(appdir);

    if (access(stamp, F_OK) == 0) {
        return 0;
    }

    void (*ggml_vk_instance_init_fn)(void) = (void (*)(void))dlsym(RTLD_DEFAULT, "ggml_vk_instance_init");
    ggml_backend_t (*ggml_backend_vk_init_fn)(size_t) = (ggml_backend_t (*)(size_t))dlsym(RTLD_DEFAULT, "ggml_backend_vk_init");

    if (!ggml_vk_instance_init_fn || !ggml_backend_vk_init_fn) {
        return 0;
    }

    ggml_vk_instance_init_fn();
    ggml_backend_t backend = ggml_backend_vk_init_fn(gpu_device);
    if (!backend) {
        fprintf(stderr, "vulkan-warmup: ggml_backend_vk_init failed\n");
        return 0;
    }

    ggml_backend_free(backend);

    int fd = open(stamp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        const char msg[] = "ok\n";
        (void)write(fd, msg, sizeof(msg) - 1);
        close(fd);
    }

    return 0;
}

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

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--warmup-vulkan") == 0) {
        return warmup_vulkan();
    }

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
