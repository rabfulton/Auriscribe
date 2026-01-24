#ifndef UI_SETTINGS_H
#define UI_SETTINGS_H

#include <gtk/gtk.h>
#include "config.h"

void settings_dialog_show(GtkWindow *parent, Config *cfg);

#endif
