#include "paste.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

static bool is_wayland(void) {
    return getenv("WAYLAND_DISPLAY") != NULL;
}

static bool command_exists(const char *cmd) {
    char buf[256];
    snprintf(buf, sizeof(buf), "which %s >/dev/null 2>&1", cmd);
    return system(buf) == 0;
}

PasteMethod paste_detect_best(void) {
    if (is_wayland()) {
        if (command_exists("wtype")) return PASTE_WTYPE;
        if (command_exists("dotool")) return PASTE_XDOTOOL;  // dotool works on both
    } else {
        if (command_exists("xdotool")) return PASTE_XDOTOOL;
    }
    return PASTE_CLIPBOARD;
}

const char *paste_method_name(PasteMethod method) {
    switch (method) {
        case PASTE_XDOTOOL: return "xdotool";
        case PASTE_WTYPE: return "wtype";
        case PASTE_CLIPBOARD: return "clipboard";
        default: return "auto";
    }
}

static bool run_with_timeout(char *const argv[], int timeout_ms) {
    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(1);
    }
    if (pid < 0) return false;

    int status = 0;
    const int sleep_us = 10000; // 10ms
    int waited_ms = 0;

    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) return false;

        if (timeout_ms > 0 && waited_ms >= timeout_ms) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return false;
        }

        usleep(sleep_us);
        waited_ms += sleep_us / 1000;
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool paste_xdotool(const char *text) {
    char *argv[] = { "xdotool", "type", "--clearmodifiers", "--", (char *) text, NULL };
    // Typing can take time for long text; avoid freezing forever if xdotool hangs.
    return run_with_timeout(argv, 30000);
}

static bool xdotool_activate_window(unsigned long window) {
    if (!window) return true;
    char idbuf[32];
    snprintf(idbuf, sizeof(idbuf), "%lu", window);
    // Do not use --sync here; it can block indefinitely if the window can't be focused.
    char *argv[] = { "xdotool", "windowactivate", idbuf, NULL };
    return run_with_timeout(argv, 1500);
}

static bool paste_wtype(const char *text) {
    char *argv[] = { "wtype", "--", (char *) text, NULL };
    return run_with_timeout(argv, 30000);
}

static bool paste_clipboard(const char *text) {
    // Use xclip or wl-copy depending on session
    const char *cmd = is_wayland() ? "wl-copy" : "xclip -selection clipboard";
    
    FILE *p = popen(cmd, "w");
    if (!p) return false;
    
    fputs(text, p);
    int ret = pclose(p);
    
    if (ret != 0) return false;
    
    // Simulate Ctrl+V
    if (is_wayland()) {
        return system("wtype -M ctrl v -m ctrl") == 0;
    } else {
        return system("xdotool key --clearmodifiers ctrl+v") == 0;
    }
}

bool paste_text(const char *text, PasteMethod method) {
    return paste_text_to_x11_window(text, method, 0);
}

bool paste_text_to_x11_window(const char *text, PasteMethod method, unsigned long window) {
    if (!text || !*text) return true;
    
    if (method == PASTE_AUTO) {
        method = paste_detect_best();
    }
    
    switch (method) {
        case PASTE_XDOTOOL:
            if (!is_wayland()) {
                if (!xdotool_activate_window(window)) return false;
            }
            return paste_xdotool(text);
        case PASTE_WTYPE:
            return paste_wtype(text);
        case PASTE_CLIPBOARD:
            if (!is_wayland()) {
                if (!xdotool_activate_window(window)) return false;
            }
            return paste_clipboard(text);
        default:
            return false;
    }
}
