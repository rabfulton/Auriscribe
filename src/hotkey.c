#include "hotkey.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>

static int grab_error = 0;

static bool hotkey_debug_enabled(void) {
    const char *v = getenv("XFCE_WHISPER_DEBUG_HOTKEY");
    return v && *v && strcmp(v, "0") != 0;
}

static unsigned int modifier_mask_for_keysym(Display *dpy, KeySym keysym) {
    XModifierKeymap *map = XGetModifierMapping(dpy);
    if (!map) return 0;

    static const unsigned int mod_masks[8] = {
        ShiftMask, LockMask, ControlMask, Mod1Mask, Mod2Mask, Mod3Mask, Mod4Mask, Mod5Mask
    };

    unsigned int result = 0;
    for (int mod_index = 0; mod_index < 8; mod_index++) {
        for (int k = 0; k < map->max_keypermod; k++) {
            KeyCode kc = map->modifiermap[mod_index * map->max_keypermod + k];
            if (!kc) continue;

            // XKeycodeToKeysym is deprecated but is sufficient here and avoids extra deps.
            KeySym ks = XKeycodeToKeysym(dpy, kc, 0);
            if (ks == keysym) {
                result = mod_masks[mod_index];
                break;
            }
        }
        if (result) break;
    }

    XFreeModifiermap(map);
    return result;
}

static void add_unique_mask(unsigned int *masks, int *count, unsigned int mask) {
    for (int i = 0; i < *count; i++) {
        if (masks[i] == mask) return;
    }
    masks[*count] = mask;
    (*count)++;
}

static int x_error_handler(Display *d, XErrorEvent *e) {
    (void)d;
    if (e->error_code == BadAccess) {
        grab_error = 1;
        fprintf(stderr, "Warning: Hotkey already grabbed by another application\n");
    }
    return 0;
}

static int x_error_handler_silent(Display *d, XErrorEvent *e) {
    (void)d;
    if (e->error_code == BadAccess) {
        grab_error = 1;
    }
    return 0;
}

struct Hotkey {
    char *keyspec;
    HotkeyCallback callback;
    void *userdata;
    
    Display *display;
    Window root;
    KeyCode keycode;
    unsigned int modifiers;

    bool grabbed;
    unsigned int ignore_mod_masks[16];
    int n_ignore_mod_masks;
    
    pthread_t thread;
    volatile bool running;
};

static void debug_dump_ignore_masks(const Hotkey *hk) {
    if (!hotkey_debug_enabled()) return;
    fprintf(stderr, "Hotkey debug: base modifiers=0x%x keycode=%u\n", hk->modifiers, hk->keycode);
    fprintf(stderr, "Hotkey debug: ignore modifier masks (%d):", hk->n_ignore_mod_masks);
    for (int i = 0; i < hk->n_ignore_mod_masks; i++) {
        fprintf(stderr, " 0x%x", hk->ignore_mod_masks[i]);
    }
    fprintf(stderr, "\n");
}

static void hotkey_compute_ignore_masks(Hotkey *hk) {
    hk->n_ignore_mod_masks = 0;
    add_unique_mask(hk->ignore_mod_masks, &hk->n_ignore_mod_masks, 0);

    const unsigned int numlock = modifier_mask_for_keysym(hk->display, XK_Num_Lock);
    const unsigned int scrolllock = modifier_mask_for_keysym(hk->display, XK_Scroll_Lock);

    // Always ignore CapsLock (LockMask is fixed).
    add_unique_mask(hk->ignore_mod_masks, &hk->n_ignore_mod_masks, LockMask);

    if (numlock) {
        add_unique_mask(hk->ignore_mod_masks, &hk->n_ignore_mod_masks, numlock);
        add_unique_mask(hk->ignore_mod_masks, &hk->n_ignore_mod_masks, numlock | LockMask);
    }
    if (scrolllock) {
        add_unique_mask(hk->ignore_mod_masks, &hk->n_ignore_mod_masks, scrolllock);
        add_unique_mask(hk->ignore_mod_masks, &hk->n_ignore_mod_masks, scrolllock | LockMask);
    }
    if (numlock && scrolllock) {
        add_unique_mask(hk->ignore_mod_masks, &hk->n_ignore_mod_masks, numlock | scrolllock);
        add_unique_mask(hk->ignore_mod_masks, &hk->n_ignore_mod_masks, numlock | scrolllock | LockMask);
    }
}

// Signal handler globals
static HotkeyCallback signal_callback;
static void *signal_userdata;

static void signal_handler(int sig) {
    (void)sig;
    // Note: write() is async-signal-safe, printf is not
    const char msg[] = "SIGUSR2 received\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    if (signal_callback) {
        signal_callback(signal_userdata);
    }
}

void hotkey_setup_signal(HotkeyCallback cb, void *userdata) {
    signal_callback = cb;
    signal_userdata = userdata;
    
    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigaction(SIGUSR2, &sa, NULL);
}

static bool parse_keyspec(Hotkey *hk, const char *spec) {
    hk->modifiers = 0;
    
    // Parse modifiers
    if (strstr(spec, "<Super>")) {
        unsigned int super_mask = 0;
        super_mask |= modifier_mask_for_keysym(hk->display, XK_Super_L);
        super_mask |= modifier_mask_for_keysym(hk->display, XK_Super_R);
        hk->modifiers |= super_mask ? super_mask : Mod4Mask;
    }
    if (strstr(spec, "<Mod4>")) {
        hk->modifiers |= Mod4Mask;
    }
    if (strstr(spec, "<Control>") || strstr(spec, "<Ctrl>")) {
        hk->modifiers |= ControlMask;
    }
    if (strstr(spec, "<Alt>")) {
        unsigned int alt_mask = 0;
        alt_mask |= modifier_mask_for_keysym(hk->display, XK_Alt_L);
        alt_mask |= modifier_mask_for_keysym(hk->display, XK_Alt_R);
        hk->modifiers |= alt_mask ? alt_mask : Mod1Mask;
    }
    if (strstr(spec, "<Mod1>")) {
        hk->modifiers |= Mod1Mask;
    }
    if (strstr(spec, "<Shift>")) {
        hk->modifiers |= ShiftMask;
    }
    
    // Find key name (last part after >)
    const char *key = strrchr(spec, '>');
    if (key) {
        key++;
    } else {
        key = spec;
    }
    
    // Convert to keysym
    KeySym keysym = XStringToKeysym(key);
    if (keysym == NoSymbol) {
        // Try common names
        if (strcasecmp(key, "space") == 0) keysym = XK_space;
        else if (strcasecmp(key, "return") == 0) keysym = XK_Return;
        else if (strcasecmp(key, "escape") == 0) keysym = XK_Escape;
        else {
            fprintf(stderr, "Unknown key: %s\n", key);
            return false;
        }
    }
    
    hk->keycode = XKeysymToKeycode(hk->display, keysym);
    return hk->keycode != 0;
}

static void *hotkey_thread(void *arg) {
    Hotkey *hk = arg;
    XEvent ev;
    
    while (hk->running) {
        while (XPending(hk->display)) {
            XNextEvent(hk->display, &ev);
            if (ev.type == KeyPress) {
                if (hotkey_debug_enabled()) {
                    KeySym ks = XKeycodeToKeysym(hk->display, (KeyCode)ev.xkey.keycode, 0);
                    const char *ks_name = ks ? XKeysymToString(ks) : NULL;
                    fprintf(stderr, "Hotkey debug: KeyPress keycode=%u state=0x%x keysym=%s\n",
                            ev.xkey.keycode, ev.xkey.state, ks_name ? ks_name : "(null)");
                }
                printf("Hotkey pressed!\n");
                if (hk->callback) {
                    hk->callback(hk->userdata);
                }
            }
        }
        usleep(10000);  // 10ms
    }
    
    return NULL;
}

Hotkey *hotkey_new(const char *keyspec) {
    Hotkey *hk = calloc(1, sizeof(Hotkey));
    hk->keyspec = strdup(keyspec);
    return hk;
}

void hotkey_set_callback(Hotkey *hk, HotkeyCallback cb, void *userdata) {
    hk->callback = cb;
    hk->userdata = userdata;
}

bool hotkey_start(Hotkey *hk) {
    if (hk->running) return true;
    
    // Check if X11 is available
    hk->display = XOpenDisplay(NULL);
    if (!hk->display) {
        fprintf(stderr, "Cannot open X display. Use SIGUSR2 for Wayland.\n");
        return false;
    }
    
    hk->root = DefaultRootWindow(hk->display);

    if (!parse_keyspec(hk, hk->keyspec)) {
        fprintf(stderr, "Failed to parse hotkey: %s\n", hk->keyspec);
        XCloseDisplay(hk->display);
        hk->display = NULL;
        return false;
    }

    hotkey_compute_ignore_masks(hk);

    debug_dump_ignore_masks(hk);
    
    // Set error handler to detect grab failures
    grab_error = 0;
    XErrorHandler old_handler = XSetErrorHandler(x_error_handler);
    
    // Grab the key with error handling (base modifiers without lock modifiers).
    XGrabKey(hk->display, hk->keycode, hk->modifiers, hk->root, False, GrabModeAsync, GrabModeAsync);
    
    // Sync to check for errors
    XSync(hk->display, False);
    
    XSetErrorHandler(old_handler);
    
    if (grab_error) {
        fprintf(stderr, "Hotkey %s is already in use. Try a different key.\n", hk->keyspec);
        fprintf(stderr, "You can also use SIGUSR2: pkill -USR2 auriscribe\n");
        XCloseDisplay(hk->display);
        hk->display = NULL;
        return false;
    }
    
    hk->grabbed = true;

    // Grab additional variants with lock modifiers (ignore errors for these).
    XSetErrorHandler(x_error_handler);
    for (int i = 0; i < hk->n_ignore_mod_masks; i++) {
        const unsigned int mods = hk->modifiers | hk->ignore_mod_masks[i];
        XGrabKey(hk->display, hk->keycode, mods, hk->root, False, GrabModeAsync, GrabModeAsync);
    }
    XSync(hk->display, False);
    XSetErrorHandler(old_handler);
    
    XSelectInput(hk->display, hk->root, KeyPressMask);
    
    hk->running = true;
    pthread_create(&hk->thread, NULL, hotkey_thread, hk);
    
    printf("Hotkey registered: %s (keycode=%d, modifiers=0x%x)\n", 
           hk->keyspec, hk->keycode, hk->modifiers);
    fflush(stdout);
    
    return true;
}

void hotkey_stop(Hotkey *hk) {
    if (!hk->running) return;
    
    hk->running = false;
    pthread_join(hk->thread, NULL);
    
    if (hk->display) {
        if (hk->grabbed) {
            for (int i = 0; i < hk->n_ignore_mod_masks; i++) {
                const unsigned int mods = hk->modifiers | hk->ignore_mod_masks[i];
                XUngrabKey(hk->display, hk->keycode, mods, hk->root);
            }
        }
        XCloseDisplay(hk->display);
        hk->display = NULL;
    }
}

void hotkey_free(Hotkey *hk) {
    if (!hk) return;
    hotkey_stop(hk);
    free(hk->keyspec);
    free(hk);
}

bool hotkey_check_available(const char *keyspec, char *reason, size_t reason_len) {
    if (reason && reason_len) reason[0] = '\0';

    Display *display = XOpenDisplay(NULL);
    if (!display) {
        if (reason && reason_len) {
            snprintf(reason, reason_len, "No X11 display (Wayland?)");
        }
        return false;
    }

    Hotkey hk = {0};
    hk.display = display;
    hk.root = DefaultRootWindow(display);
    hk.keyspec = (char *)keyspec;

    if (!parse_keyspec(&hk, keyspec)) {
        if (reason && reason_len) {
            snprintf(reason, reason_len, "Invalid hotkey");
        }
        XCloseDisplay(display);
        return false;
    }

    hotkey_compute_ignore_masks(&hk);

    grab_error = 0;
    XErrorHandler old_handler = XSetErrorHandler(x_error_handler_silent);

    for (int i = 0; i < hk.n_ignore_mod_masks; i++) {
        const unsigned int mods = hk.modifiers | hk.ignore_mod_masks[i];
        XGrabKey(display, hk.keycode, mods, hk.root, False, GrabModeAsync, GrabModeAsync);
    }
    XSync(display, False);

    for (int i = 0; i < hk.n_ignore_mod_masks; i++) {
        const unsigned int mods = hk.modifiers | hk.ignore_mod_masks[i];
        XUngrabKey(display, hk.keycode, mods, hk.root);
    }
    XSync(display, False);

    XSetErrorHandler(old_handler);
    XCloseDisplay(display);

    if (grab_error) {
        if (reason && reason_len) {
            snprintf(reason, reason_len, "In use / reserved");
        }
        return false;
    }

    if (reason && reason_len) {
        snprintf(reason, reason_len, "Available");
    }
    return true;
}
