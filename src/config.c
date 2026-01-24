#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <json-c/json.h>

static char config_dir[512];
static char data_dir[512];
static char models_dir[512];

static void build_paths(const char *app, char *out_config_dir, size_t out_config_len,
                        char *out_data_dir, size_t out_data_len) {
    const char *home = getenv("HOME");
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    const char *xdg_data = getenv("XDG_DATA_HOME");

    if (xdg_config)
        snprintf(out_config_dir, out_config_len, "%s/%s", xdg_config, app);
    else
        snprintf(out_config_dir, out_config_len, "%s/.config/%s", home, app);

    if (xdg_data)
        snprintf(out_data_dir, out_data_len, "%s/%s", xdg_data, app);
    else
        snprintf(out_data_dir, out_data_len, "%s/.local/share/%s", home, app);
}

static void ensure_dirs(void) {
    char legacy_config_dir[512] = {0};
    char legacy_data_dir[512] = {0};

    build_paths("auriscribe", config_dir, sizeof(config_dir), data_dir, sizeof(data_dir));
    build_paths("xfce-whisper", legacy_config_dir, sizeof(legacy_config_dir), legacy_data_dir, sizeof(legacy_data_dir));

    // If the new config dir doesn't exist but legacy one does, keep reading legacy config.
    //
    // Data dir intentionally does NOT fall back to legacy:
    // old whisper.cpp models can be incompatible with the vendored whisper.cpp version and
    // lead to confusing "invalid model" / "not all tensors loaded" failures.
    struct stat st;
    if (stat(config_dir, &st) != 0 && stat(legacy_config_dir, &st) == 0) {
        snprintf(config_dir, sizeof(config_dir), "%s", legacy_config_dir);
    }

    snprintf(models_dir, sizeof(models_dir), "%s/models", data_dir);

    mkdir(config_dir, 0755);
    mkdir(data_dir, 0755);
    mkdir(models_dir, 0755);
}

const char *config_get_dir(void) {
    if (!config_dir[0]) ensure_dirs();
    return config_dir;
}

const char *config_get_data_dir(void) {
    if (!data_dir[0]) ensure_dirs();
    return data_dir;
}

const char *config_get_models_dir(void) {
    if (!models_dir[0]) ensure_dirs();
    return models_dir;
}

static char *strdup_safe(const char *s) {
    return s ? strdup(s) : NULL;
}

Config *config_new_default(void) {
    Config *cfg = calloc(1, sizeof(Config));
    cfg->model_id = strdup("medium.en-q5_0");
    cfg->hotkey = strdup("<Super>space");
    cfg->language = strdup("en");
    cfg->paste_method = strdup("auto");
    cfg->push_to_talk = false;
    cfg->translate_to_english = false;
    cfg->vad_threshold = 0.02f;
    cfg->autostart = false;
    return cfg;
}

void config_free(Config *cfg) {
    if (!cfg) return;
    free(cfg->model_id);
    free(cfg->model_path);
    free(cfg->hotkey);
    free(cfg->language);
    free(cfg->paste_method);
    free(cfg->microphone);
    free(cfg);
}

Config *config_load(void) {
    char path[600];
    snprintf(path, sizeof(path), "%s/settings.json", config_get_dir());

    FILE *f = fopen(path, "r");
    if (!f) return config_new_default();

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(len + 1);
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);

    struct json_object *root = json_tokener_parse(buf);
    free(buf);

    if (!root) return config_new_default();

    Config *cfg = calloc(1, sizeof(Config));
    struct json_object *val;

    if (json_object_object_get_ex(root, "model_id", &val))
        cfg->model_id = strdup(json_object_get_string(val));
    if (json_object_object_get_ex(root, "model_path", &val))
        cfg->model_path = strdup_safe(json_object_get_string(val));
    if (json_object_object_get_ex(root, "hotkey", &val))
        cfg->hotkey = strdup(json_object_get_string(val));
    if (json_object_object_get_ex(root, "language", &val))
        cfg->language = strdup(json_object_get_string(val));
    if (json_object_object_get_ex(root, "paste_method", &val))
        cfg->paste_method = strdup(json_object_get_string(val));
    if (json_object_object_get_ex(root, "microphone", &val))
        cfg->microphone = strdup_safe(json_object_get_string(val));
    if (json_object_object_get_ex(root, "push_to_talk", &val))
        cfg->push_to_talk = json_object_get_boolean(val);
    if (json_object_object_get_ex(root, "translate_to_english", &val))
        cfg->translate_to_english = json_object_get_boolean(val);
    if (json_object_object_get_ex(root, "vad_threshold", &val))
        cfg->vad_threshold = (float)json_object_get_double(val);
    if (json_object_object_get_ex(root, "autostart", &val))
        cfg->autostart = json_object_get_boolean(val);

    json_object_put(root);

    // Fill defaults for missing fields
    if (!cfg->model_id) cfg->model_id = strdup("whisper-small");
    if (!cfg->hotkey) cfg->hotkey = strdup("<Super>space");
    if (!cfg->language) cfg->language = strdup("en");
    if (!cfg->paste_method) cfg->paste_method = strdup("auto");
    if (cfg->vad_threshold == 0) cfg->vad_threshold = 0.02f;

    return cfg;
}

void config_save(const Config *cfg) {
    struct json_object *root = json_object_new_object();

    json_object_object_add(root, "model_id", json_object_new_string(cfg->model_id ? cfg->model_id : ""));
    if (cfg->model_path)
        json_object_object_add(root, "model_path", json_object_new_string(cfg->model_path));
    json_object_object_add(root, "hotkey", json_object_new_string(cfg->hotkey ? cfg->hotkey : ""));
    json_object_object_add(root, "language", json_object_new_string(cfg->language ? cfg->language : ""));
    json_object_object_add(root, "paste_method", json_object_new_string(cfg->paste_method ? cfg->paste_method : ""));
    if (cfg->microphone)
        json_object_object_add(root, "microphone", json_object_new_string(cfg->microphone));
    json_object_object_add(root, "push_to_talk", json_object_new_boolean(cfg->push_to_talk));
    json_object_object_add(root, "translate_to_english", json_object_new_boolean(cfg->translate_to_english));
    json_object_object_add(root, "vad_threshold", json_object_new_double(cfg->vad_threshold));
    json_object_object_add(root, "autostart", json_object_new_boolean(cfg->autostart));

    char path[600];
    snprintf(path, sizeof(path), "%s/settings.json", config_get_dir());

    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n", json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY));
        fclose(f);
    }

    json_object_put(root);
}
