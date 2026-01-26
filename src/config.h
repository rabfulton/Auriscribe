#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

typedef struct {
    char *model_id;
    char *model_path;
    char *hotkey;
    char *language;
    char *paste_method;
    char *microphone;
    bool push_to_talk;
    bool translate_to_english;
    float vad_threshold;
    bool autostart;
    bool overlay_enabled;
    char *overlay_position; // "screen" or "target"
    bool paste_each_chunk;  // X11 only; paste on VAD pause
    char *chunk_output;     // "target", "overlay", or "both"
} Config;

// Get XDG paths
const char *config_get_dir(void);
const char *config_get_data_dir(void);
const char *config_get_models_dir(void);

// Load/save
Config *config_load(void);
void config_save(const Config *cfg);
void config_free(Config *cfg);

// Defaults
Config *config_new_default(void);

#endif
