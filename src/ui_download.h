#ifndef UI_DOWNLOAD_H
#define UI_DOWNLOAD_H

#include <gtk/gtk.h>

typedef void (*ModelDownloadedCallback)(const char *model_id, const char *model_path, void *userdata);

void download_dialog_show(GtkWindow *parent, ModelDownloadedCallback cb, void *userdata);

#endif
