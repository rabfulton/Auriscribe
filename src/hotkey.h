#ifndef HOTKEY_H
#define HOTKEY_H

#include <stdbool.h>
#include <stddef.h>

typedef void (*HotkeyCallback)(void *userdata);

typedef struct Hotkey Hotkey;

Hotkey *hotkey_new(const char *keyspec);
void hotkey_set_callback(Hotkey *hk, HotkeyCallback cb, void *userdata);
bool hotkey_start(Hotkey *hk);
void hotkey_stop(Hotkey *hk);
void hotkey_free(Hotkey *hk);

// For Wayland: setup SIGUSR2 handler
void hotkey_setup_signal(HotkeyCallback cb, void *userdata);

// Best-effort check (X11 only) whether a hotkey can be grabbed.
// Returns false if no X11 display is available, the keyspec is invalid, or the grab conflicts.
bool hotkey_check_available(const char *keyspec, char *reason, size_t reason_len);

#endif
