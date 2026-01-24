#include "ui_settings.h"
#include "audio.h"
#include "paste.h"
#include "config.h"
#include "hotkey.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    Config *cfg;
    GtkWidget *dialog;
    GtkWidget *mic_combo;
    GtkWidget *hotkey_entry;
    GtkWidget *hotkey_status;
    GtkWidget *hotkey_capture_btn;
    GtkWidget *language_combo;
    GtkWidget *paste_combo;
    GtkWidget *vad_scale;
    GtkWidget *ptt_check;
    GtkWidget *translate_check;
    GtkWidget *autostart_check;
    GtkWidget *overlay_check;
    GtkWidget *overlay_pos_combo;
    GtkWidget *model_path_entry;
    bool capturing_hotkey;
} SettingsDialog;

static void autostart_set_enabled(bool enabled) {
    const char *home = getenv("HOME");
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    if (!home && !xdg_config) return;

    char autostart_dir[512];
    if (xdg_config) {
        snprintf(autostart_dir, sizeof(autostart_dir), "%s/autostart", xdg_config);
    } else {
        snprintf(autostart_dir, sizeof(autostart_dir), "%s/.config/autostart", home);
    }

    mkdir(autostart_dir, 0755);

    char path[600];
    snprintf(path, sizeof(path), "%s/auriscribe.desktop", autostart_dir);

    if (!enabled) {
        unlink(path);
        return;
    }

    FILE *f = fopen(path, "w");
    if (!f) return;

    fprintf(f,
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=Auriscribe\n"
            "Exec=auriscribe\n"
            "X-GNOME-Autostart-enabled=true\n");
    fclose(f);
}

static void populate_microphones(SettingsDialog *sd) {
    int count = 0;
    AudioDevice *devices = audio_list_devices(&count);
    
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(sd->mic_combo));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sd->mic_combo), NULL, "(Default)");
    
    int active = 0;
    for (int i = 0; i < count; i++) {
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sd->mic_combo), 
                                  devices[i].name, devices[i].description);
        if (sd->cfg->microphone && strcmp(sd->cfg->microphone, devices[i].name) == 0) {
            active = i + 1;
        }
    }
    
    gtk_combo_box_set_active(GTK_COMBO_BOX(sd->mic_combo), active);
    audio_free_devices(devices, count);
}

static void update_hotkey_status(SettingsDialog *sd) {
    if (!sd || !sd->hotkey_status) return;

    const char *keyspec = gtk_entry_get_text(GTK_ENTRY(sd->hotkey_entry));
    char reason[128];
    const bool ok = hotkey_check_available(keyspec, reason, sizeof(reason));

    char buf[256];
    if (ok) {
        snprintf(buf, sizeof(buf), "Hotkey status: %s", reason);
    } else {
        snprintf(buf, sizeof(buf), "Hotkey status: %s (try <Control>space)", reason);
    }
    gtk_label_set_text(GTK_LABEL(sd->hotkey_status), buf);
}

static void on_hotkey_entry_changed(GtkEditable *editable, gpointer data) {
    (void)editable;
    SettingsDialog *sd = data;
    update_hotkey_status(sd);
}

static void set_hotkey_capture_state(SettingsDialog *sd, bool enabled) {
    sd->capturing_hotkey = enabled;
    gtk_button_set_label(GTK_BUTTON(sd->hotkey_capture_btn), enabled ? "Press keys..." : "Capture...");
    if (enabled) {
        gtk_label_set_text(GTK_LABEL(sd->hotkey_status), "Press desired key combination (Esc to cancel)");
    } else {
        update_hotkey_status(sd);
    }
}

static gboolean on_dialog_key_press(GtkWidget *widget, GdkEventKey *event, gpointer data) {
    (void)widget;
    SettingsDialog *sd = data;
    if (!sd || !sd->capturing_hotkey) return FALSE;

    if (event->keyval == GDK_KEY_Escape) {
        set_hotkey_capture_state(sd, false);
        return TRUE;
    }

    switch (event->keyval) {
        case GDK_KEY_Control_L:
        case GDK_KEY_Control_R:
        case GDK_KEY_Shift_L:
        case GDK_KEY_Shift_R:
        case GDK_KEY_Alt_L:
        case GDK_KEY_Alt_R:
        case GDK_KEY_Super_L:
        case GDK_KEY_Super_R:
        case GDK_KEY_Meta_L:
        case GDK_KEY_Meta_R:
            return TRUE;
        default:
            break;
    }

    char keybuf[64];
    const char *name = gdk_keyval_name(event->keyval);
    if (event->keyval == GDK_KEY_space) name = "space";
    if (!name || !*name) return TRUE;

    snprintf(keybuf, sizeof(keybuf), "%s", name);

    char spec[256];
    spec[0] = '\0';

    if (event->state & GDK_CONTROL_MASK) strcat(spec, "<Control>");
    if (event->state & GDK_SHIFT_MASK)   strcat(spec, "<Shift>");
    if (event->state & GDK_MOD1_MASK)    strcat(spec, "<Alt>");
    if (event->state & GDK_SUPER_MASK)   strcat(spec, "<Super>");

    strncat(spec, keybuf, sizeof(spec) - strlen(spec) - 1);

    gtk_entry_set_text(GTK_ENTRY(sd->hotkey_entry), spec);
    set_hotkey_capture_state(sd, false);
    return TRUE;
}

static void on_hotkey_capture_clicked(GtkButton *button, gpointer data) {
    (void)button;
    SettingsDialog *sd = data;
    set_hotkey_capture_state(sd, !sd->capturing_hotkey);
}

static void settings_apply(SettingsDialog *sd) {
    // Save microphone
    free(sd->cfg->microphone);
    const char *mic_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(sd->mic_combo));
    sd->cfg->microphone = mic_id ? strdup(mic_id) : NULL;

    // Save hotkey
    free(sd->cfg->hotkey);
    sd->cfg->hotkey = strdup(gtk_entry_get_text(GTK_ENTRY(sd->hotkey_entry)));

    // Save language
    free(sd->cfg->language);
    const char *lang = gtk_combo_box_get_active_id(GTK_COMBO_BOX(sd->language_combo));
    sd->cfg->language = strdup(lang ? lang : "en");

    // Save paste method
    free(sd->cfg->paste_method);
    const char *paste = gtk_combo_box_get_active_id(GTK_COMBO_BOX(sd->paste_combo));
    sd->cfg->paste_method = strdup(paste ? paste : "auto");

    // Save VAD threshold
    sd->cfg->vad_threshold = (float)gtk_range_get_value(GTK_RANGE(sd->vad_scale));

    // Save checkboxes
    sd->cfg->push_to_talk = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sd->ptt_check));
    sd->cfg->translate_to_english = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sd->translate_check));
    sd->cfg->autostart = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sd->autostart_check));

    // Save overlay
    sd->cfg->overlay_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sd->overlay_check));
    free(sd->cfg->overlay_position);
    const char *pos = gtk_combo_box_get_active_id(GTK_COMBO_BOX(sd->overlay_pos_combo));
    sd->cfg->overlay_position = strdup(pos ? pos : "screen");

    // Save model path
    free(sd->cfg->model_path);
    const char *path = gtk_entry_get_text(GTK_ENTRY(sd->model_path_entry));
    sd->cfg->model_path = (path && *path) ? strdup(path) : NULL;

    config_save(sd->cfg);
    autostart_set_enabled(sd->cfg->autostart);
}

static void on_browse_model(GtkButton *button, gpointer data) {
    (void)button;
    SettingsDialog *sd = data;
    
    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Select Model File",
        GTK_WINDOW(sd->dialog),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open", GTK_RESPONSE_ACCEPT,
        NULL);
    
    // Set filter for .bin files
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Whisper Models (*.bin)");
    gtk_file_filter_add_pattern(filter, "*.bin");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);
    
    // Start in models directory
    char path[512];
    snprintf(path, sizeof(path), "%s", config_get_models_dir());
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), path);
    
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        gtk_entry_set_text(GTK_ENTRY(sd->model_path_entry), filename);
        g_free(filename);
    }
    
    gtk_widget_destroy(chooser);
}

static GtkWidget *create_label(const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    return label;
}

void settings_dialog_show(GtkWindow *parent, Config *cfg) {
    SettingsDialog *sd = calloc(1, sizeof(SettingsDialog));
    sd->cfg = cfg;
    
    sd->dialog = gtk_dialog_new_with_buttons(
        "Auriscribe Settings",
        parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Save", GTK_RESPONSE_OK,
        NULL);
    
    gtk_window_set_default_size(GTK_WINDOW(sd->dialog), 450, -1);
    
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(sd->dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);
    gtk_box_set_spacing(GTK_BOX(content), 12);
    
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_box_pack_start(GTK_BOX(content), grid, TRUE, TRUE, 0);
    
    int row = 0;
    
    // Model path
    gtk_grid_attach(GTK_GRID(grid), create_label("Model:"), 0, row, 1, 1);
    GtkWidget *model_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    sd->model_path_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(sd->model_path_entry), cfg->model_path ? cfg->model_path : "");
    gtk_widget_set_hexpand(sd->model_path_entry, TRUE);
    gtk_box_pack_start(GTK_BOX(model_box), sd->model_path_entry, TRUE, TRUE, 0);
    GtkWidget *browse_btn = gtk_button_new_with_label("Browse...");
    g_signal_connect(browse_btn, "clicked", G_CALLBACK(on_browse_model), sd);
    gtk_box_pack_start(GTK_BOX(model_box), browse_btn, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), model_box, 1, row++, 1, 1);
    
    // Microphone
    gtk_grid_attach(GTK_GRID(grid), create_label("Microphone:"), 0, row, 1, 1);
    sd->mic_combo = gtk_combo_box_text_new();
    gtk_widget_set_hexpand(sd->mic_combo, TRUE);
    gtk_grid_attach(GTK_GRID(grid), sd->mic_combo, 1, row++, 1, 1);
    populate_microphones(sd);
    
    // Hotkey
    gtk_grid_attach(GTK_GRID(grid), create_label("Hotkey:"), 0, row, 1, 1);
    GtkWidget *hk_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    sd->hotkey_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(sd->hotkey_entry), cfg->hotkey ? cfg->hotkey : "<Super>space");
    gtk_widget_set_hexpand(sd->hotkey_entry, TRUE);
    gtk_box_pack_start(GTK_BOX(hk_box), sd->hotkey_entry, TRUE, TRUE, 0);

    sd->hotkey_capture_btn = gtk_button_new_with_label("Capture...");
    g_signal_connect(sd->hotkey_capture_btn, "clicked", G_CALLBACK(on_hotkey_capture_clicked), sd);
    gtk_box_pack_start(GTK_BOX(hk_box), sd->hotkey_capture_btn, FALSE, FALSE, 0);

    gtk_grid_attach(GTK_GRID(grid), hk_box, 1, row++, 1, 1);

    sd->hotkey_status = gtk_label_new("");
    gtk_widget_set_halign(sd->hotkey_status, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), sd->hotkey_status, 1, row++, 1, 1);
    
    // Language
    gtk_grid_attach(GTK_GRID(grid), create_label("Language:"), 0, row, 1, 1);
    sd->language_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sd->language_combo), "auto", "Auto-detect");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sd->language_combo), "en", "English");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sd->language_combo), "es", "Spanish");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sd->language_combo), "fr", "French");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sd->language_combo), "de", "German");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sd->language_combo), "it", "Italian");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sd->language_combo), "pt", "Portuguese");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sd->language_combo), "ru", "Russian");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sd->language_combo), "zh", "Chinese");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sd->language_combo), "ja", "Japanese");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(sd->language_combo), cfg->language ? cfg->language : "en");
    gtk_grid_attach(GTK_GRID(grid), sd->language_combo, 1, row++, 1, 1);
    
    // Paste method
    gtk_grid_attach(GTK_GRID(grid), create_label("Paste method:"), 0, row, 1, 1);
    sd->paste_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sd->paste_combo), "auto", "Auto-detect");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sd->paste_combo), "xdotool", "xdotool (X11)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sd->paste_combo), "wtype", "wtype (Wayland)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sd->paste_combo), "clipboard", "Clipboard");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(sd->paste_combo), cfg->paste_method ? cfg->paste_method : "auto");
    gtk_grid_attach(GTK_GRID(grid), sd->paste_combo, 1, row++, 1, 1);
    
    // VAD threshold
    gtk_grid_attach(GTK_GRID(grid), create_label("VAD sensitivity:"), 0, row, 1, 1);
    sd->vad_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.005, 0.1, 0.005);
    gtk_range_set_value(GTK_RANGE(sd->vad_scale), cfg->vad_threshold);
    gtk_scale_set_value_pos(GTK_SCALE(sd->vad_scale), GTK_POS_RIGHT);
    gtk_widget_set_hexpand(sd->vad_scale, TRUE);
    gtk_grid_attach(GTK_GRID(grid), sd->vad_scale, 1, row++, 1, 1);
    
    // Checkboxes
    sd->ptt_check = gtk_check_button_new_with_label("Push-to-talk mode");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sd->ptt_check), cfg->push_to_talk);
    gtk_grid_attach(GTK_GRID(grid), sd->ptt_check, 0, row++, 2, 1);
    
    sd->translate_check = gtk_check_button_new_with_label("Translate to English");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sd->translate_check), cfg->translate_to_english);
    gtk_grid_attach(GTK_GRID(grid), sd->translate_check, 0, row++, 2, 1);

    sd->autostart_check = gtk_check_button_new_with_label("Start Auriscribe on login");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sd->autostart_check), cfg->autostart);
    gtk_grid_attach(GTK_GRID(grid), sd->autostart_check, 0, row++, 2, 1);

    // Recording overlay
    sd->overlay_check = gtk_check_button_new_with_label("Show recording overlay");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sd->overlay_check), cfg->overlay_enabled);
    gtk_grid_attach(GTK_GRID(grid), sd->overlay_check, 0, row++, 2, 1);

    gtk_grid_attach(GTK_GRID(grid), create_label("Overlay position:"), 0, row, 1, 1);
    sd->overlay_pos_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sd->overlay_pos_combo), "screen", "Screen center");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sd->overlay_pos_combo), "target", "Target window center (X11)");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(sd->overlay_pos_combo), cfg->overlay_position ? cfg->overlay_position : "screen");
    gtk_grid_attach(GTK_GRID(grid), sd->overlay_pos_combo, 1, row++, 1, 1);
    
    g_signal_connect(sd->hotkey_entry, "changed", G_CALLBACK(on_hotkey_entry_changed), sd);
    g_signal_connect(sd->dialog, "key-press-event", G_CALLBACK(on_dialog_key_press), sd);
    update_hotkey_status(sd);

    gtk_widget_show_all(sd->dialog);

    gint resp = gtk_dialog_run(GTK_DIALOG(sd->dialog));
    if (resp == GTK_RESPONSE_OK) {
        const char *keyspec = gtk_entry_get_text(GTK_ENTRY(sd->hotkey_entry));
        char reason[128];
        if (!hotkey_check_available(keyspec, reason, sizeof(reason))) {
            GtkWidget *msg = gtk_message_dialog_new(
                GTK_WINDOW(sd->dialog),
                GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                GTK_MESSAGE_WARNING,
                GTK_BUTTONS_OK_CANCEL,
                "The selected hotkey may not work: %s\n\nSave anyway?",
                reason);
            gint confirm = gtk_dialog_run(GTK_DIALOG(msg));
            gtk_widget_destroy(msg);
            if (confirm == GTK_RESPONSE_OK) {
                settings_apply(sd);
            }
        } else {
            settings_apply(sd);
        }
    }

    gtk_widget_destroy(sd->dialog);
    free(sd);
}
