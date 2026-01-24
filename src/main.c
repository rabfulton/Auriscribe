#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>
#include "app.h"
#include <X11/Xlib.h>

static AppIndicator *indicator;

static void on_toggle_recording(GtkMenuItem *item, gpointer data) {
    (void)item; (void)data;
    app_toggle_recording();
}

static void on_settings(GtkMenuItem *item, gpointer data) {
    (void)item; (void)data;
    app_show_settings();
}

static void on_download(GtkMenuItem *item, gpointer data) {
    (void)item; (void)data;
    app_show_download();
}

static void on_quit(GtkMenuItem *item, gpointer data) {
    (void)item; (void)data;
    g_application_quit(G_APPLICATION(app->gtk_app));
}

static GtkWidget *create_menu(void) {
    GtkWidget *menu = gtk_menu_new();
    
    // Status item (non-clickable)
    app->status_item = gtk_menu_item_new_with_label("Ready");
    gtk_widget_set_sensitive(app->status_item, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), app->status_item);

    // Hotkey status (non-clickable)
    char hk_buf[256];
    if (app->hotkey_available) {
        snprintf(hk_buf, sizeof(hk_buf), "Hotkey: %s", app->config->hotkey ? app->config->hotkey : "");
    } else {
        snprintf(hk_buf, sizeof(hk_buf), "Hotkey: unavailable (bind SIGUSR2)");
    }
    app->hotkey_item = gtk_menu_item_new_with_label(hk_buf);
    gtk_widget_set_sensitive(app->hotkey_item, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), app->hotkey_item);
    
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    
    // Toggle recording
    GtkWidget *toggle = gtk_menu_item_new_with_label("Start/Stop Recording");
    g_signal_connect(toggle, "activate", G_CALLBACK(on_toggle_recording), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), toggle);
    
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    
    // Settings
    GtkWidget *settings = gtk_menu_item_new_with_label("Settings...");
    g_signal_connect(settings, "activate", G_CALLBACK(on_settings), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), settings);
    
    // Download models
    GtkWidget *download = gtk_menu_item_new_with_label("Download Models...");
    g_signal_connect(download, "activate", G_CALLBACK(on_download), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), download);
    
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    
    // Quit
    GtkWidget *quit = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(quit, "activate", G_CALLBACK(on_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit);
    
    gtk_widget_show_all(menu);
    return menu;
}

static void on_activate(GtkApplication *gtk_app, gpointer data) {
    (void)data;
    
    app_init(gtk_app);
    
    // Create tray indicator
    indicator = app_indicator_new("auriscribe", "audio-input-microphone",
                                  APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_title(indicator, "Auriscribe");
    
    app->tray_menu = create_menu();
    app_indicator_set_menu(indicator, GTK_MENU(app->tray_menu));
    
    // Hold the application (no window, just tray)
    g_application_hold(G_APPLICATION(gtk_app));
    
    if (app->hotkey_available) {
        printf("Auriscribe started. Press %s to record.\n", app->config->hotkey);
    } else {
        printf("Auriscribe started. Global hotkey unavailable.\n");
        printf("On Wayland (or if the key is already in use), bind a key to: pkill -USR2 auriscribe\n");
    }
    
    if (!transcriber_is_loaded(app->transcriber)) {
        printf("No model loaded. Use 'Download Models' from tray menu.\n");
    }
}

static void on_shutdown(GtkApplication *gtk_app, gpointer data) {
    (void)gtk_app; (void)data;
    app_cleanup();
}

int main(int argc, char *argv[]) {
    // Must be called before any other Xlib call in the process.
    // This makes global hotkey handling reliable when GTK/GDK is also using X11.
    XInitThreads();

    GtkApplication *gtk_app = gtk_application_new("org.auriscribe",
                                                   G_APPLICATION_FLAGS_NONE);
    
    g_signal_connect(gtk_app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(gtk_app, "shutdown", G_CALLBACK(on_shutdown), NULL);
    
    int status = g_application_run(G_APPLICATION(gtk_app), argc, argv);
    g_object_unref(gtk_app);
    
    return status;
}
