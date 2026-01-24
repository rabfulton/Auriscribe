#include "ui_download.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <curl/curl.h>
#include "ggml.h"

typedef struct {
    const char *id;
    const char *name;
    const char *filename;
    size_t size_mb;
    const char *hf_repo;
    const char *hf_revision;
    const char *hf_path;
} ModelInfo;

// Default model store (override at runtime with XFCE_WHISPER_HF_REPO).
// We follow whisper.cpp's models/download-ggml-model.sh URL pattern:
//   https://huggingface.co/<repo>/resolve/main/ggml-<model>.bin
#define DEFAULT_HF_REPO "ggerganov/whisper.cpp"

static const ModelInfo models[] = {
    // One-click presets (Hugging Face), aligned to whisper.cpp's naming.
    // File name saved locally is the same as whisper.cpp expects: ggml-<model>.bin
    {"tiny.en",         "Tiny (English)",           "ggml-tiny.en.bin",           75,   NULL, "main", "ggml-tiny.en.bin"},
    {"base.en",         "Base (English)",           "ggml-base.en.bin",           142,  NULL, "main", "ggml-base.en.bin"},
    {"small.en",        "Small (English)",          "ggml-small.en.bin",          487,  NULL, "main", "ggml-small.en.bin"},
    {"small",           "Small (Multilingual)",     "ggml-small.bin",             487,  NULL, "main", "ggml-small.bin"},
    {"medium.en",       "Medium (English)",         "ggml-medium.en.bin",         1500, NULL, "main", "ggml-medium.en.bin"},
    {"large-v3-turbo",  "Large-v3 Turbo",           "ggml-large-v3-turbo.bin",    1600, NULL, "main", "ggml-large-v3-turbo.bin"},
    {"large-v3-turbo-q5_0", "Large-v3 Turbo (Q5_0)", "ggml-large-v3-turbo-q5_0.bin", 1200, NULL, "main", "ggml-large-v3-turbo-q5_0.bin"},
    {NULL, NULL, NULL, 0, NULL, NULL, NULL}
};

typedef struct {
    GtkWidget *dialog;
    GtkWidget *model_combo;
    GtkWidget *progress;
    GtkWidget *status_label;
    GtkWidget *download_btn;
    pthread_t thread;
    volatile bool downloading;
    volatile bool cancel;
    int selected_model;
    volatile gint64 downloaded;
    volatile gint64 total;
    volatile gint64 last_update_us;
    volatile gint64 last_downloaded;
    guint progress_timer_id;

    char *active_url;
    char *active_filename;

    volatile long http_code;
    char curl_err[CURL_ERROR_SIZE];
    bool success;
    char *error_message;

    ModelDownloadedCallback downloaded_cb;
    void *downloaded_userdata;
} DownloadDialog;

static char *hf_build_url(const char *repo, const char *revision, const char *path) {
    if (!repo || !*repo || !path || !*path) return NULL;
    const char *rev = (revision && *revision) ? revision : "main";
    size_t n = strlen("https://huggingface.co/") + strlen(repo) +
               strlen("/resolve/") + strlen(rev) + 1 + strlen(path) +
               strlen("?download=true") + 1;
    char *url = malloc(n);
    if (!url) return NULL;
    snprintf(url, n, "https://huggingface.co/%s/resolve/%s/%s?download=true", repo, rev, path);
    return url;
}

static char *model_to_url(const ModelInfo *model) {
    if (!model) return NULL;

    const char *repo = getenv("XFCE_WHISPER_HF_REPO");
    if (!repo || !*repo) repo = model->hf_repo ? model->hf_repo : DEFAULT_HF_REPO;

    return hf_build_url(repo, model->hf_revision, model->hf_path);
}

static size_t write_callback(void *ptr, size_t size, size_t nmemb, void *stream) {
    return fwrite(ptr, size, nmemb, (FILE *)stream);
}

static int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow) {
    (void)ultotal; (void)ulnow;
    DownloadDialog *dd = clientp;
    
    if (dd->cancel) return 1;  // Abort
    
    dd->downloaded = (gint64)dlnow;
    dd->total = (gint64)dltotal;
    
    return 0;
}

static gboolean update_progress(gpointer data) {
    DownloadDialog *dd = data;

    if (!dd->downloading) {
        dd->progress_timer_id = 0;
        return G_SOURCE_REMOVE;
    }

    const gint64 total = dd->total;
    const gint64 downloaded = dd->downloaded;
    const gint64 now_us = g_get_monotonic_time();

    if (total > 0 && downloaded >= 0) {
        double fraction = (double)downloaded / (double)total;
        if (fraction < 0) fraction = 0;
        if (fraction > 1) fraction = 1;
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(dd->progress), fraction);
        
        char buf[64];
        snprintf(buf, sizeof(buf), "%.1f / %.1f MB", 
                 downloaded / 1048576.0, total / 1048576.0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(dd->progress), buf);

        if (dd->last_update_us != 0 && now_us > dd->last_update_us) {
            const double dt = (now_us - dd->last_update_us) / 1000000.0;
            const double db = (double)(downloaded - dd->last_downloaded);
            const double mb_s = dt > 0 ? (db / 1048576.0) / dt : 0;
            char status[128];
            snprintf(status, sizeof(status), "Downloading... %.1f%% (%.1f MB/s)",
                     fraction * 100.0, mb_s);
            gtk_label_set_text(GTK_LABEL(dd->status_label), status);
        }
        dd->last_update_us = now_us;
        dd->last_downloaded = downloaded;
    } else {
        // No content length yet; show pulsing bar.
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(dd->progress));
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(dd->progress), "Downloading...");
        gtk_label_set_text(GTK_LABEL(dd->status_label), "Downloading... (unknown size)");
    }
    
    return G_SOURCE_CONTINUE;
}

static gboolean download_done_ui(gpointer data) {
    DownloadDialog *dd = data;

    gtk_button_set_label(GTK_BUTTON(dd->download_btn), "Download");
    gtk_widget_set_sensitive(dd->model_combo, TRUE);
    gtk_widget_set_sensitive(dd->download_btn, TRUE);

    if (dd->cancel) {
        gtk_label_set_text(GTK_LABEL(dd->status_label), "Canceled");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(dd->progress), 0.0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(dd->progress), "Canceled");
    } else if (!dd->success) {
        const char *msg = dd->error_message ? dd->error_message : "Download failed";
        gtk_label_set_text(GTK_LABEL(dd->status_label), msg);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(dd->progress), 0.0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(dd->progress), "Failed");
    } else {
        gtk_label_set_text(GTK_LABEL(dd->status_label), "Download complete");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(dd->progress), 1.0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(dd->progress), "Done");

        if (dd->downloaded_cb && dd->active_filename && dd->selected_model >= 0) {
            const ModelInfo *model = &models[dd->selected_model];
            if (model && model->id) {
                char path[512];
                snprintf(path, sizeof(path), "%s/%s", config_get_models_dir(), dd->active_filename);
                dd->downloaded_cb(model->id, path, dd->downloaded_userdata);
            }
        }
    }

    free(dd->error_message);
    dd->error_message = NULL;

    return G_SOURCE_REMOVE;
}

static bool file_has_ggml_magic(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint32_t magic = 0;
    const size_t n = fread(&magic, 1, sizeof(magic), f);
    fclose(f);
    if (n != sizeof(magic)) return false;
    return magic == GGML_FILE_MAGIC;
}

static void *download_thread(void *arg) {
    DownloadDialog *dd = arg;
    const ModelInfo *model = &models[dd->selected_model];

    const char *url = dd->active_url;
    const char *filename = dd->active_filename ? dd->active_filename : model->filename;
    if (!url || !*url || !filename || !*filename) {
        dd->downloading = false;
        dd->cancel = true;
        g_idle_add(download_done_ui, dd);
        return NULL;
    }

    dd->success = false;
    free(dd->error_message);
    dd->error_message = NULL;
    dd->http_code = 0;
    dd->curl_err[0] = '\0';
    
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", config_get_models_dir(), filename);
    
    FILE *f = fopen(path, "wb");
    if (!f) {
        dd->downloading = false;
        dd->cancel = true;
        dd->error_message = strdup("Failed to write file (check permissions)");
        g_idle_add(download_done_ui, dd);
        return NULL;
    }
    
    CURL *curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, dd->curl_err);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, dd);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "auriscribe/1.0");
    
    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &dd->http_code);
    
    fclose(f);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK || dd->cancel || dd->http_code < 200 || dd->http_code >= 300) {
        unlink(path);  // Remove partial file
        if (!dd->cancel) {
            char buf[320];
            if (dd->http_code && (dd->http_code < 200 || dd->http_code >= 300)) {
                snprintf(buf, sizeof(buf), "Download failed (HTTP %ld)", dd->http_code);
            } else if (dd->curl_err[0]) {
                snprintf(buf, sizeof(buf), "Download failed (curl: %.200s)", dd->curl_err);
            } else {
                snprintf(buf, sizeof(buf), "Download failed (%s)", curl_easy_strerror(res));
            }
            dd->error_message = strdup(buf);
        }
    } else if (!file_has_ggml_magic(path)) {
        unlink(path);
        dd->error_message = strdup("Downloaded file is not a valid whisper.cpp model (wrong file/source)");
    } else {
        dd->success = true;
    }
    
    dd->downloading = false;
    g_idle_add(download_done_ui, dd);
    
    return NULL;
}

static void on_model_changed(GtkComboBox *box, gpointer data) {
    (void)box;
    DownloadDialog *dd = data;
    gtk_label_set_text(GTK_LABEL(dd->status_label), "Select a model and click Download");
}

static void on_download_clicked(GtkButton *button, gpointer data) {
    (void)button;
    DownloadDialog *dd = data;
    
    if (dd->downloading) {
        dd->cancel = true;
        return;
    }
    
    dd->selected_model = gtk_combo_box_get_active(GTK_COMBO_BOX(dd->model_combo));
    if (dd->selected_model < 0) return;

    const ModelInfo *model = &models[dd->selected_model];

    free(dd->active_url);
    free(dd->active_filename);
    dd->active_url = NULL;
    dd->active_filename = NULL;

    dd->active_url = model_to_url(model);
    if (!dd->active_url) {
        gtk_label_set_text(GTK_LABEL(dd->status_label), "Failed to build model URL");
        return;
    }
    dd->active_filename = model->filename ? strdup(model->filename) : NULL;

    dd->downloading = true;
    dd->cancel = false;
    dd->downloaded = 0;
    dd->total = 0;
    dd->last_update_us = 0;
    dd->last_downloaded = 0;

    gtk_button_set_label(GTK_BUTTON(dd->download_btn), "Cancel");
    gtk_label_set_text(GTK_LABEL(dd->status_label), "Downloading...");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(dd->progress), 0.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(dd->progress), "Starting...");
    gtk_widget_set_sensitive(dd->model_combo, FALSE);

    if (dd->progress_timer_id == 0) {
        dd->progress_timer_id = g_timeout_add(100, update_progress, dd);
    }
    
    pthread_create(&dd->thread, NULL, download_thread, dd);
}

static void on_dialog_response(GtkDialog *dialog, gint response_id, gpointer data) {
    (void)response_id;
    DownloadDialog *dd = data;
    
    if (dd->downloading) {
        dd->cancel = true;
        pthread_join(dd->thread, NULL);
    }

    if (dd->progress_timer_id) {
        g_source_remove(dd->progress_timer_id);
        dd->progress_timer_id = 0;
    }

    free(dd->active_url);
    free(dd->active_filename);
    
    gtk_widget_destroy(GTK_WIDGET(dialog));
    free(dd);
}

void download_dialog_show(GtkWindow *parent, ModelDownloadedCallback cb, void *userdata) {
    DownloadDialog *dd = calloc(1, sizeof(DownloadDialog));
    dd->downloaded_cb = cb;
    dd->downloaded_userdata = userdata;
    
    dd->dialog = gtk_dialog_new_with_buttons(
        "Download Models",
        parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Close", GTK_RESPONSE_CLOSE,
        NULL);
    
    gtk_window_set_default_size(GTK_WINDOW(dd->dialog), 400, -1);
    
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dd->dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);
    gtk_box_set_spacing(GTK_BOX(content), 12);
    
    // Model selector
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(content), hbox, FALSE, FALSE, 0);
    
    dd->model_combo = gtk_combo_box_text_new();
    for (int i = 0; models[i].id; i++) {
        char buf[128];
        if (models[i].size_mb) {
            snprintf(buf, sizeof(buf), "%s (%zu MB)", models[i].name, models[i].size_mb);
        } else {
            snprintf(buf, sizeof(buf), "%s", models[i].name);
        }
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(dd->model_combo), buf);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(dd->model_combo), 0);
    gtk_widget_set_hexpand(dd->model_combo, TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), dd->model_combo, TRUE, TRUE, 0);
    g_signal_connect(dd->model_combo, "changed", G_CALLBACK(on_model_changed), dd);
    
    dd->download_btn = gtk_button_new_with_label("Download");
    g_signal_connect(dd->download_btn, "clicked", G_CALLBACK(on_download_clicked), dd);
    gtk_box_pack_start(GTK_BOX(hbox), dd->download_btn, FALSE, FALSE, 0);

    // Progress bar
    dd->progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(dd->progress), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(dd->progress), "Ready");
    gtk_box_pack_start(GTK_BOX(content), dd->progress, FALSE, FALSE, 0);
    
    // Status label
    dd->status_label = gtk_label_new("Select a model and click Download");
    gtk_widget_set_halign(dd->status_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(content), dd->status_label, FALSE, FALSE, 0);

    // Source info
    const char *repo = getenv("XFCE_WHISPER_HF_REPO");
    if (!repo || !*repo) repo = DEFAULT_HF_REPO;
    char src_info[256];
    snprintf(src_info, sizeof(src_info), "Source: Hugging Face (%s)", repo);
    GtkWidget *src_label = gtk_label_new(src_info);
    gtk_widget_set_halign(src_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(content), src_label, FALSE, FALSE, 0);
    
    // Models directory info
    char info[256];
    snprintf(info, sizeof(info), "Models saved to: %s", config_get_models_dir());
    GtkWidget *info_label = gtk_label_new(info);
    gtk_widget_set_halign(info_label, GTK_ALIGN_START);
    gtk_label_set_selectable(GTK_LABEL(info_label), TRUE);
    gtk_box_pack_start(GTK_BOX(content), info_label, FALSE, FALSE, 0);
    
    g_signal_connect(dd->dialog, "response", G_CALLBACK(on_dialog_response), dd);
    
    gtk_widget_show_all(dd->dialog);
}
