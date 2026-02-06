#include "transcribe.h"
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *env_get(const char *preferred, const char *legacy) {
    const char *v = preferred ? getenv(preferred) : NULL;
    if (v && *v) return v;
    v = legacy ? getenv(legacy) : NULL;
    if (v && *v) return v;
    return NULL;
}

static int transcriber_default_threads(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    return (int)n;
}

static int transcriber_threads(void) {
    const char *env = env_get("AURISCRIBE_THREADS", "XFCE_WHISPER_THREADS");
    if (!env || !*env) return transcriber_default_threads();
    int n = atoi(env);
    if (n < 1) n = 1;
    if (n > 64) n = 64;
    return n;
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

static bool write_u32(int fd, uint32_t v) {
    uint8_t b[4] = {
        (uint8_t)(v & 0xff),
        (uint8_t)((v >> 8) & 0xff),
        (uint8_t)((v >> 16) & 0xff),
        (uint8_t)((v >> 24) & 0xff),
    };
    return write_exact(fd, b, sizeof(b));
}

static bool read_u32(int fd, uint32_t *out) {
    uint8_t b[4];
    if (!read_exact(fd, b, sizeof(b))) return false;
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return true;
}

static bool write_u8(int fd, uint8_t v) {
    return write_exact(fd, &v, 1);
}

static bool read_u8(int fd, uint8_t *out) {
    return read_exact(fd, out, 1);
}

static bool read_msg(int fd, char *type_out, char **payload_out) {
    *payload_out = NULL;
    char magic[4];
    if (!read_exact(fd, magic, 4)) return false;
    if (memcmp(magic, "AUR1", 4) != 0) return false;
    uint8_t t = 0;
    if (!read_u8(fd, &t)) return false;
    uint32_t n = 0;
    if (!read_u32(fd, &n)) return false;
    char *s = calloc(1, (size_t)n + 1);
    if (!s) return false;
    if (n && !read_exact(fd, s, n)) {
        free(s);
        return false;
    }
    s[n] = '\0';
    *type_out = (char)t;
    *payload_out = s;
    return true;
}

static bool send_magic_cmd(int fd, char cmd) {
    const char magic[4] = { 'A', 'U', 'R', 'I' };
    if (!write_exact(fd, magic, 4)) return false;
    return write_u8(fd, (uint8_t)cmd);
}

struct Transcriber {
    EngineType type;
    pid_t worker_pid;
    int to_worker_fd;
    int from_worker_fd;
    int err_fd;
    bool loaded;
    bool loading;
    bool load_failed;
};

static void transcriber_kill_worker(Transcriber *t) {
    if (!t) return;
    if (t->to_worker_fd != -1) close(t->to_worker_fd);
    if (t->from_worker_fd != -1) close(t->from_worker_fd);
    if (t->err_fd != -1) close(t->err_fd);
    t->to_worker_fd = -1;
    t->from_worker_fd = -1;
    t->err_fd = -1;

    if (t->worker_pid > 0) {
        // Try graceful.
        kill(t->worker_pid, SIGTERM);
        for (int i = 0; i < 50; i++) { // ~500ms
            int status = 0;
            pid_t r = waitpid(t->worker_pid, &status, WNOHANG);
            if (r == t->worker_pid) break;
            usleep(10000);
        }
        kill(t->worker_pid, SIGKILL);
        (void)waitpid(t->worker_pid, NULL, 0);
        t->worker_pid = 0;
    }
    t->loaded = false;
    t->loading = false;
    t->load_failed = false;
    t->type = ENGINE_NONE;
}

static bool transcriber_start_worker(Transcriber *t) {
    int to_child[2] = {-1, -1};
    int from_child[2] = {-1, -1};
    int err_child[2] = {-1, -1};
    if (pipe(to_child) != 0) return false;
    if (pipe(from_child) != 0) {
        close(to_child[0]); close(to_child[1]);
        return false;
    }
    if (pipe(err_child) != 0) {
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        return false;
    }

    pid_t pid = fork();
    if (pid == 0) {
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        dup2(err_child[1], STDERR_FILENO);
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        close(err_child[0]); close(err_child[1]);

        // Dev (run from build dir) + installed (in PATH).
        execl("./auriscribe-worker", "auriscribe-worker", NULL);
        execlp("auriscribe-worker", "auriscribe-worker", NULL);
        _exit(127);
    }

    if (pid < 0) {
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        close(err_child[0]); close(err_child[1]);
        return false;
    }

    close(to_child[0]);
    close(from_child[1]);
    close(err_child[1]);

    t->worker_pid = pid;
    t->to_worker_fd = to_child[1];
    t->from_worker_fd = from_child[0];
    t->err_fd = err_child[0];
    (void)fcntl(t->err_fd, F_SETFL, fcntl(t->err_fd, F_GETFL, 0) | O_NONBLOCK);
    return true;
}

static char *read_worker_stderr_nonblocking(int fd) {
    if (fd < 0) return NULL;
    char buf[4096];
    size_t cap = 0;
    size_t len = 0;
    char *out = NULL;

    for (;;) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r == 0) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if ((size_t)r > 0) {
            if (len + (size_t)r + 1 > cap) {
                size_t ncap = cap ? cap * 2 : 8192;
                while (ncap < len + (size_t)r + 1) ncap *= 2;
                char *p = realloc(out, ncap);
                if (!p) break;
                out = p;
                cap = ncap;
            }
            memcpy(out + len, buf, (size_t)r);
            len += (size_t)r;
        }
    }

    if (!out) return NULL;
    out[len] = '\0';

    // Keep the tail (most relevant error lines).
    const size_t max = 2000;
    if (len > max) {
        size_t start = len - max;
        memmove(out, out + start, max);
        out[max] = '\0';
    }
    return out;
}

Transcriber *transcriber_new(void) {
    Transcriber *t = calloc(1, sizeof(*t));
    t->type = ENGINE_NONE;
    t->worker_pid = 0;
    t->to_worker_fd = -1;
    t->from_worker_fd = -1;
    t->err_fd = -1;
    t->loaded = false;
    t->loading = false;
    t->load_failed = false;
    return t;
}

bool transcriber_load(Transcriber *t, EngineType type, const char *model_path) {
    transcriber_unload(t);
    if (!t || type != ENGINE_WHISPER) return false;
    if (!model_path || !*model_path) return false;

    if (!transcriber_start_worker(t)) {
        fprintf(stderr, "Failed to start auriscribe-worker\n");
        transcriber_kill_worker(t);
        return false;
    }

    const bool no_gpu = env_get("AURISCRIBE_NO_GPU", "XFCE_WHISPER_NO_GPU") != NULL;
    const char *gpu_device_s = env_get("AURISCRIBE_GPU_DEVICE", "XFCE_WHISPER_GPU_DEVICE");
    uint32_t gpu_device = 0;
    if (gpu_device_s && *gpu_device_s) gpu_device = (uint32_t)atoi(gpu_device_s);

    if (!send_magic_cmd(t->to_worker_fd, 'L')) {
        transcriber_kill_worker(t);
        return false;
    }
    const uint32_t path_len = (uint32_t)strlen(model_path);
    if (!write_u32(t->to_worker_fd, path_len) ||
        !write_exact(t->to_worker_fd, model_path, path_len) ||
        !write_u32(t->to_worker_fd, (uint32_t)transcriber_threads()) ||
        !write_u32(t->to_worker_fd, gpu_device) ||
        !write_u8(t->to_worker_fd, no_gpu ? 0 : 1)) {
        transcriber_kill_worker(t);
        return false;
    }

    char resp_type = 0;
    char *payload = NULL;
    if (!read_msg(t->from_worker_fd, &resp_type, &payload)) {
        transcriber_kill_worker(t);
        return false;
    }
    const bool ok = (resp_type == 'O');
    if (!ok) {
        fprintf(stderr, "Worker load failed: %s\n", payload ? payload : "");
        free(payload);
        transcriber_kill_worker(t);
        return false;
    }
    free(payload);

    t->type = ENGINE_WHISPER;
    t->loaded = true;
    return true;
}

bool transcriber_load_async(Transcriber *t, EngineType type, const char *model_path) {
    transcriber_unload(t);
    if (!t || type != ENGINE_WHISPER) return false;
    if (!model_path || !*model_path) return false;

    if (!transcriber_start_worker(t)) {
        fprintf(stderr, "Failed to start auriscribe-worker\n");
        transcriber_kill_worker(t);
        return false;
    }

    const bool no_gpu = env_get("AURISCRIBE_NO_GPU", "XFCE_WHISPER_NO_GPU") != NULL;
    const char *gpu_device_s = env_get("AURISCRIBE_GPU_DEVICE", "XFCE_WHISPER_GPU_DEVICE");
    uint32_t gpu_device = 0;
    if (gpu_device_s && *gpu_device_s) gpu_device = (uint32_t)atoi(gpu_device_s);

    if (!send_magic_cmd(t->to_worker_fd, 'L')) {
        transcriber_kill_worker(t);
        return false;
    }
    const uint32_t path_len = (uint32_t)strlen(model_path);
    if (!write_u32(t->to_worker_fd, path_len) ||
        !write_exact(t->to_worker_fd, model_path, path_len) ||
        !write_u32(t->to_worker_fd, (uint32_t)transcriber_threads()) ||
        !write_u32(t->to_worker_fd, gpu_device) ||
        !write_u8(t->to_worker_fd, no_gpu ? 0 : 1)) {
        transcriber_kill_worker(t);
        return false;
    }

    t->type = ENGINE_WHISPER;
    t->loaded = false;
    t->loading = true;
    t->load_failed = false;
    return true;
}

void transcriber_unload(Transcriber *t) {
    if (!t) return;

    if (t->worker_pid <= 0) {
        t->loaded = false;
        t->loading = false;
        t->load_failed = false;
        t->type = ENGINE_NONE;
        return;
    }

    (void)send_magic_cmd(t->to_worker_fd, 'Q');
    char resp_type = 0;
    char *payload = NULL;
    (void)read_msg(t->from_worker_fd, &resp_type, &payload);
    free(payload);

    // Close pipes; reap child.
    if (t->to_worker_fd != -1) close(t->to_worker_fd);
    if (t->from_worker_fd != -1) close(t->from_worker_fd);
    if (t->err_fd != -1) close(t->err_fd);
    t->to_worker_fd = -1;
    t->from_worker_fd = -1;
    t->err_fd = -1;
    (void)waitpid(t->worker_pid, NULL, 0);
    t->worker_pid = 0;
    t->loaded = false;
    t->loading = false;
    t->load_failed = false;
    t->type = ENGINE_NONE;
}

void transcriber_free(Transcriber *t) {
    if (!t) return;
    transcriber_kill_worker(t);
    free(t);
}

bool transcriber_is_loaded(Transcriber *t) {
    return t && t->loaded && t->worker_pid > 0;
}

bool transcriber_is_loading(Transcriber *t) {
    return t && t->loading;
}

bool transcriber_is_active(Transcriber *t) {
    return t && t->worker_pid > 0;
}

EngineType transcriber_get_type(Transcriber *t) {
    return t ? t->type : ENGINE_NONE;
}

char *transcriber_process(Transcriber *t, const float *samples, size_t count,
                          const char *language, bool translate) {
    return transcriber_process_ex(t, samples, count, language, translate, NULL, NULL);
}

char *transcriber_process_ex(Transcriber *t, const float *samples, size_t count,
                             const char *language, bool translate,
                             const char *initial_prompt,
                             char **error_out) {
    if (error_out) *error_out = NULL;
    if (!t) return NULL;

    if (t->loading && !t->loaded) {
        char resp_type = 0;
        char *payload = NULL;
        if (!read_msg(t->from_worker_fd, &resp_type, &payload)) {
            free(payload);
            t->loading = false;
            t->load_failed = true;
            transcriber_kill_worker(t);
            if (error_out) *error_out = strdup("Failed to load model (worker communication error)");
            return NULL;
        }
        const bool ok = (resp_type == 'O');
        if (!ok && error_out) {
            *error_out = payload ? payload : strdup("Failed to load model");
            payload = NULL;
        }
        free(payload);
        t->loading = false;
        t->loaded = ok;
        t->load_failed = !ok;
        if (!ok) {
            transcriber_kill_worker(t);
            return NULL;
        }
    }

    if (!transcriber_is_loaded(t)) return NULL;
    if (t->type != ENGINE_WHISPER) return NULL;

    if (!send_magic_cmd(t->to_worker_fd, 'T')) {
        transcriber_kill_worker(t);
        if (error_out) *error_out = strdup("Worker communication error");
        return NULL;
    }

    const uint32_t n_samples = (uint32_t)count;
    const char *lang = (language && strcmp(language, "auto") != 0) ? language : "";
    const uint32_t lang_len = (uint32_t)strlen(lang);
    const char *prompt = initial_prompt ? initial_prompt : "";
    const uint32_t prompt_len = (uint32_t)strlen(prompt);

    if (!write_u32(t->to_worker_fd, n_samples) ||
        !write_u32(t->to_worker_fd, lang_len) ||
        (lang_len && !write_exact(t->to_worker_fd, lang, lang_len)) ||
        !write_u32(t->to_worker_fd, prompt_len) ||
        (prompt_len && !write_exact(t->to_worker_fd, prompt, prompt_len)) ||
        !write_u8(t->to_worker_fd, translate ? 1 : 0) ||
        !write_u32(t->to_worker_fd, (uint32_t)transcriber_threads()) ||
        (n_samples && !write_exact(t->to_worker_fd, samples, (size_t)n_samples * sizeof(float)))) {
        transcriber_kill_worker(t);
        if (error_out) *error_out = strdup("Worker communication error");
        return NULL;
    }

    char resp_type = 0;
    char *payload = NULL;
    if (!read_msg(t->from_worker_fd, &resp_type, &payload)) {
        transcriber_kill_worker(t);
        if (error_out) *error_out = strdup("Worker communication error");
        return NULL;
    }

    if (resp_type == 'R') {
        return payload; // already allocated
    }

    if (error_out) {
        char *stderr_tail = read_worker_stderr_nonblocking(t->err_fd);
        if (stderr_tail && *stderr_tail) {
            const char *p = payload ? payload : "Transcription failed";
            const size_t n = strlen(p) + strlen("\n\n") + strlen(stderr_tail) + 1;
            char *msg = malloc(n);
            if (msg) {
                snprintf(msg, n, "%s\n\n%s", p, stderr_tail);
                free(payload);
                free(stderr_tail);
                *error_out = msg;
            } else {
                free(stderr_tail);
                *error_out = payload;
            }
        } else {
            free(stderr_tail);
            *error_out = payload; // caller frees
        }
        return NULL;
    }

    fprintf(stderr, "Worker error: %s\n", payload ? payload : "");
    free(payload);
    return NULL;
}
