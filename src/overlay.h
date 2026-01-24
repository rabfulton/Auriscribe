#ifndef OVERLAY_H
#define OVERLAY_H

#include <stdbool.h>

typedef struct App App;

void overlay_show(App *app);
void overlay_hide(App *app);
void overlay_set_level(App *app, float level_0_to_1);

#endif

