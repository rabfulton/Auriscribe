#ifndef APP_H
#define APP_H

#include <gtk/gtk.h>
#include "config.h"
#include "audio.h"
#include "vad.h"
#include "transcribe.h"
#include "hotkey.h"
#include <stdatomic.h>

typedef enum {
    STATE_IDLE,
    STATE_RECORDING,
    STATE_PROCESSING
} AppState;

typedef struct App {
    GtkApplication *gtk_app;
    Config *config;
    
    AudioCapture *audio;
    VAD *vad;
    Transcriber *transcriber;
    Hotkey *hotkey;

    bool hotkey_available;
    bool shutting_down;
    bool in_settings;
    gint hotkey_toggle_queued;
    gint64 last_hotkey_us;
    
    AppState state;

    // Model lifecycle (on-demand load + unload after idle)
    guint model_unload_timeout_id;
    gint64 model_last_used_us;

    // Current utterance buffer (grows dynamically)
    float *rec_buffer;
    size_t rec_count;
    size_t rec_capacity;
    unsigned long target_x11_window;
    float vad_accum[480];
    size_t vad_accum_count;

    // Chunking + background transcription
    GAsyncQueue *chunk_queue;
    GThread *worker_thread;
    GMutex accum_mutex;
    GString *accum_text;
    bool stop_requested;
    
    // UI elements
    GtkWidget *tray_menu;
    GtkWidget *status_item;
    GtkWidget *hotkey_item;

    // Debug
    bool debug_chunking;
    bool debug_last_vad_speech;
    uint64_t debug_audio_cb_count;
    bool debug_overlay_latency;
    float debug_prev_overlay_lvl;

    // Recording overlay (optional)
    GtkWidget *overlay_window;
    GtkWidget *overlay_area;
    guint overlay_tick_id;
    gint overlay_level_i; // atomic 0..1000
    atomic_long overlay_level_us; // last time overlay_level_i updated (monotonic us)
    double overlay_level_smooth;
    double overlay_phase;
    gint64 overlay_last_pos_us;
    int overlay_w;
    int overlay_h;
} App;

extern App *app;

void app_init(GtkApplication *gtk_app);
void app_cleanup(void);

void app_toggle_recording(void);
void app_start_recording(void);
void app_stop_recording(void);

void app_show_settings(void);
void app_show_download(void);

// Tray icon state (implemented in src/main.c)
void tray_set_recording(bool recording);

#endif
