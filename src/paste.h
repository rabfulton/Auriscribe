#ifndef PASTE_H
#define PASTE_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    PASTE_AUTO,
    PASTE_XDOTOOL,
    PASTE_WTYPE,
    PASTE_CLIPBOARD
} PasteMethod;

bool paste_text(const char *text, PasteMethod method);
bool paste_text_to_x11_window(const char *text, PasteMethod method, unsigned long window);
PasteMethod paste_detect_best(void);
const char *paste_method_name(PasteMethod method);

#endif
