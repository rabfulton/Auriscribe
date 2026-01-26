#include "overlay.h"
#include "app.h"
#include <gtk/gtk.h>
#include <pango/pangocairo.h>
#include <math.h>
#include <string.h>
#include <X11/Xlib.h>

static bool overlay_use_target_window(const App *a) {
    return a && a->config && a->config->overlay_position &&
           strcmp(a->config->overlay_position, "target") == 0;
}

static bool x11_get_window_center(unsigned long win, int *cx, int *cy) {
    if (!win) return false;
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return false;

    Window w = (Window)win;
    XWindowAttributes attr;
    if (XGetWindowAttributes(dpy, w, &attr) == 0) {
        XCloseDisplay(dpy);
        return false;
    }

    Window root = RootWindow(dpy, DefaultScreen(dpy));
    Window child = 0;
    int x = 0, y = 0;
    if (XTranslateCoordinates(dpy, w, root, 0, 0, &x, &y, &child) == 0) {
        XCloseDisplay(dpy);
        return false;
    }

    *cx = x + (attr.width / 2);
    *cy = y + (attr.height / 2);
    XCloseDisplay(dpy);
    return true;
}

static void screen_get_center(int *cx, int *cy) {
    GdkDisplay *display = gdk_display_get_default();
    if (display) {
        GdkMonitor *mon = gdk_display_get_primary_monitor(display);
        if (mon) {
            GdkRectangle geo;
            gdk_monitor_get_geometry(mon, &geo);
            *cx = geo.x + geo.width / 2;
            *cy = geo.y + geo.height / 2;
            return;
        }
    }

    *cx = 0;
    *cy = 0;
}

static int overlay_pick_size_for_point(int cx, int cy) {
    int min_dim = 900;

    GdkDisplay *display = gdk_display_get_default();
    if (display) {
        GdkMonitor *mon = gdk_display_get_monitor_at_point(display, cx, cy);
        if (!mon) mon = gdk_display_get_primary_monitor(display);
        if (mon) {
            GdkRectangle geo;
            gdk_monitor_get_geometry(mon, &geo);
            min_dim = geo.width < geo.height ? geo.width : geo.height;
        }
    }

    // Size is in logical pixels; choose a % of the monitor so it's "DPI/scale aware".
    int sz = (int)lrint((double)min_dim * 0.12);
    if (sz < 140) sz = 140;
    if (sz > 260) sz = 260;
    return sz;
}

static void overlay_reposition(App *a) {
    if (!a || !a->overlay_window) return;

    int cx = 0, cy = 0;
    bool ok = false;
    if (overlay_use_target_window(a)) {
        ok = x11_get_window_center(a->target_x11_window, &cx, &cy);
    }
    if (!ok) {
        screen_get_center(&cx, &cy);
    }

    const int w = a->overlay_w;
    const int h = a->overlay_h;
    gtk_window_move(GTK_WINDOW(a->overlay_window), cx - (w / 2), cy - (h / 2));
}

static gboolean overlay_draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
    (void)widget;
    App *a = data;
    if (!a) return FALSE;

    const int w = a->overlay_w;
    const int h = a->overlay_h;
    const double cx = w / 2.0;
    const double radius = w * 0.34;
    const double margin = w * 0.10;
    const double cy = margin + radius * 1.05;

    const double t = a->overlay_phase;
    const double level = a->overlay_level_smooth; // 0..1

    // Clear.
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    // Soft background circle.
    cairo_set_source_rgba(cr, 0, 0, 0, 0.28);
    cairo_arc(cr, cx, cy, radius * 1.05, 0, 2 * M_PI);
    cairo_fill(cr);

    // Pulse ring.
    const double pulse = 1.0 + 0.05 * sin(t * 2.0 * M_PI);
    cairo_set_line_width(cr, radius * 0.10);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.22 + 0.20 * level);
    cairo_arc(cr, cx, cy, radius * pulse, 0, 2 * M_PI);
    cairo_stroke(cr);

    // Level bars (waveform).
    const int bars = 11;
    const double bar_w = radius * 0.11;
    const double gap = bar_w * 0.40;
    const double total_w = bars * bar_w + (bars - 1) * gap;
    const double start_x = cx - total_w / 2.0;
    const double base_h = radius * 0.25;
    const double max_h = radius * 0.95;

    for (int i = 0; i < bars; i++) {
        const double phase = t * 2.0 * M_PI + i * 0.60;
        const double jitter = 0.25 + 0.75 * (0.5 + 0.5 * sin(phase));
        const double amp = base_h + (max_h - base_h) * level * jitter;

        const double x = start_x + i * (bar_w + gap);
        const double y = cy - amp / 2.0;
        const double r = bar_w * 0.45;

        cairo_set_source_rgba(cr, 1, 1, 1, 0.80);
        cairo_new_path(cr);
        cairo_move_to(cr, x + r, y);
        cairo_arc(cr, x + bar_w - r, y + r, r, -M_PI_2, 0);
        cairo_arc(cr, x + bar_w - r, y + amp - r, r, 0, M_PI_2);
        cairo_arc(cr, x + r, y + amp - r, r, M_PI_2, M_PI);
        cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI_2);
        cairo_close_path(cr);
        cairo_fill(cr);
    }

    // Transcript preview below.
    const double text_top = cy + radius * 1.20;
    if (a->overlay_text && a->overlay_text->len > 0 && text_top < (double)h) {
        cairo_set_source_rgba(cr, 1, 1, 1, 0.92);

        PangoLayout *layout = pango_cairo_create_layout(cr);
        PangoFontDescription *fd = pango_font_description_from_string("Sans 14");
        pango_layout_set_font_description(layout, fd);
        pango_font_description_free(fd);

        pango_layout_set_width(layout, (int)((w - 2 * margin) * PANGO_SCALE));
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        // Show the most recent transcript (tail), and bound the on-screen height.
        pango_layout_set_height(layout, -3);
        pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_START);
        pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);

        pango_layout_set_text(layout, a->overlay_text->str, -1);

        cairo_move_to(cr, margin, text_top);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
    }

    return FALSE;
}

static gboolean overlay_tick(gpointer data) {
    App *a = data;
    if (!a || !a->overlay_window || !a->overlay_area) {
        if (a) a->overlay_tick_id = 0;
        return G_SOURCE_REMOVE;
    }

    // Hold a reference for the duration of this tick in case the overlay gets
    // hidden/destroyed during main-loop dispatch.
    GtkWidget *area = a->overlay_area;
    g_object_ref(area);

    const int lvl_i = g_atomic_int_get(&a->overlay_level_i);
    double lvl = (double)lvl_i / 1000.0;
    if (lvl < 0) lvl = 0;
    if (lvl > 1) lvl = 1;

    // Attack/decay smoothing for nicer motion.
    const double attack = 0.70;
    const double decay = 0.22;
    if (lvl > a->overlay_level_smooth) {
        a->overlay_level_smooth = a->overlay_level_smooth * (1.0 - attack) + lvl * attack;
    } else {
        a->overlay_level_smooth = a->overlay_level_smooth * (1.0 - decay) + lvl * decay;
    }

    a->overlay_phase += 1.0 / 60.0;
    if (a->overlay_phase > 1000000.0) a->overlay_phase = 0.0;

    const gint64 now_us = g_get_monotonic_time();
    if (!a->overlay_last_pos_us || (now_us - a->overlay_last_pos_us) > 250000) {
        overlay_reposition(a);
        a->overlay_last_pos_us = now_us;
    }

    if (a->debug_overlay_latency) {
        static gint64 last_log_us = 0;
        const gint64 src_us = (gint64)atomic_load(&a->overlay_level_us);
        if (src_us > 0 && (last_log_us == 0 || (now_us - last_log_us) > 500000)) {
            fprintf(stderr, "[overlay-lat] tick lvl=%.3f lag=%lldms\n",
                    lvl,
                    (long long)((now_us - src_us) / 1000));
            last_log_us = now_us;
        }
    }

    if (gtk_widget_get_visible(area)) {
        gtk_widget_queue_draw(area);
    }
    g_object_unref(area);
    return G_SOURCE_CONTINUE;
}

void overlay_show(App *a) {
    if (!a || !a->config || !a->config->overlay_enabled) return;
    if (a->overlay_window) return;

    int cx = 0, cy = 0;
    bool ok = false;
    if (overlay_use_target_window(a)) {
        ok = x11_get_window_center(a->target_x11_window, &cx, &cy);
    }
    if (!ok) {
        screen_get_center(&cx, &cy);
    }
    const int sz = overlay_pick_size_for_point(cx, cy);
    a->overlay_w = sz;
    a->overlay_h = (int)lrint((double)sz * 1.55);
    a->overlay_phase = 0.0;
    a->overlay_level_smooth = (double)g_atomic_int_get(&a->overlay_level_i) / 1000.0;
    a->overlay_last_pos_us = 0;

    GtkWidget *win = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_accept_focus(GTK_WINDOW(win), FALSE);
    gtk_window_set_focus_on_map(GTK_WINDOW(win), FALSE);
    gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_NOTIFICATION);
    gtk_widget_set_app_paintable(win, TRUE);

    GdkScreen *screen = gtk_window_get_screen(GTK_WINDOW(win));
    if (screen && gdk_screen_is_composited(screen)) {
        GdkVisual *rgba = gdk_screen_get_rgba_visual(screen);
        if (rgba) gtk_widget_set_visual(win, rgba);
    }

    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, a->overlay_w, a->overlay_h);
    g_signal_connect(area, "draw", G_CALLBACK(overlay_draw), a);

    gtk_container_add(GTK_CONTAINER(win), area);

    a->overlay_window = win;
    a->overlay_area = area;

    gtk_widget_show_all(win);
    overlay_reposition(a);

    a->overlay_tick_id = g_timeout_add(16, overlay_tick, a);
}

void overlay_hide(App *a) {
    if (!a) return;
    if (a->overlay_tick_id) {
        g_source_remove(a->overlay_tick_id);
        a->overlay_tick_id = 0;
    }
    if (a->overlay_window) {
        // Clear pointers first so a pending tick can't dereference freed widgets.
        GtkWidget *win = a->overlay_window;
        a->overlay_window = NULL;
        a->overlay_area = NULL;
        gtk_widget_destroy(win);
    }
}

void overlay_set_level(App *a, float level_0_to_1) {
    if (!a) return;
    if (level_0_to_1 < 0) level_0_to_1 = 0;
    if (level_0_to_1 > 1) level_0_to_1 = 1;
    atomic_store(&a->overlay_level_us, g_get_monotonic_time());
    g_atomic_int_set(&a->overlay_level_i, (int)lrintf(level_0_to_1 * 1000.0f));
}

void overlay_append_text(App *a, const char *text) {
    if (!a || !a->overlay_text || !text || !*text) return;
    if (a->overlay_text->len > 0 && text[0] != ' ' && text[0] != '\n' && text[0] != '\t') {
        g_string_append_c(a->overlay_text, ' ');
    }
    g_string_append(a->overlay_text, text);

    // Keep the overlay preview bounded so it doesn't grow without limit.
    const size_t max_chars = 280;
    if (a->overlay_text->len > max_chars) {
        const size_t cut = a->overlay_text->len - max_chars;
        g_string_erase(a->overlay_text, 0, (gssize)cut);
        // Trim to a word boundary-ish.
        while (a->overlay_text->len > 0 && a->overlay_text->str[0] != ' ') {
            g_string_erase(a->overlay_text, 0, 1);
        }
        while (a->overlay_text->len > 0 && a->overlay_text->str[0] == ' ') {
            g_string_erase(a->overlay_text, 0, 1);
        }
    }
}
