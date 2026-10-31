/* ui_gtk.c - the Linux side of ui.h: status bar, command line, scrolling, clipboard. */
#include "jot.h"

#include <string.h>

static struct {
    GtkStack *stack;
    GtkLabel *mode_label, *showcmd_label, *ocr_label, *pos_label, *cmd_prefix;
    GtkText *cmd;
} U;

/* ------------------------------------------------------------------------- */
/* Status bar                                                                 */
/* ------------------------------------------------------------------------- */

gboolean ui_status_ready(void) {
    return U.mode_label != NULL;
}

void ui_status_mode(const char *text, gboolean error) {
    gtk_label_set_text(U.mode_label, text);
    if (error)
        gtk_widget_add_css_class(GTK_WIDGET(U.mode_label), "error");
    else
        gtk_widget_remove_css_class(GTK_WIDGET(U.mode_label), "error");
}

void ui_status_keys(const char *text) {
    gtk_label_set_text(U.showcmd_label, text);
}

void ui_status_pos(const char *text) {
    gtk_label_set_text(U.pos_label, text);
}

void ui_status_ocr(const char *text) {
    if (!U.ocr_label)
        return;
    if (text)
        gtk_label_set_text(U.ocr_label, text);
    gtk_widget_set_visible(GTK_WIDGET(U.ocr_label), text != NULL);
}

/* ------------------------------------------------------------------------- */
/* Command line                                                               */
/* ------------------------------------------------------------------------- */

void ui_cmd_set_text(const char *text) {
    gtk_editable_set_text(GTK_EDITABLE(U.cmd), text);
    gtk_editable_set_position(GTK_EDITABLE(U.cmd), -1);
}

void ui_cmd_open(const char *prefix, const char *text) {
    gtk_label_set_text(U.cmd_prefix, prefix);
    gtk_editable_set_text(GTK_EDITABLE(U.cmd), text);
    gtk_stack_set_visible_child_name(U.stack, "cmd");
    gtk_widget_grab_focus(GTK_WIDGET(U.cmd));
    gtk_editable_set_position(GTK_EDITABLE(U.cmd), -1);
}

void ui_cmd_close(void) {
    gtk_stack_set_visible_child_name(U.stack, "status");
    gtk_widget_grab_focus(GTK_WIDGET(J.view));
}

gboolean ui_cmd_is_open(void) {
    return g_str_equal(gtk_stack_get_visible_child_name(U.stack), "cmd");
}

const char *ui_cmd_text(void) {
    return gtk_editable_get_text(GTK_EDITABLE(U.cmd));
}

static void on_cmd_changed(GtkEditable *e, gpointer unused) {
    (void)e, (void)unused;
    vim_cmd_changed();
}

static void on_cmd_activate(GtkText *t, gpointer unused) {
    (void)t, (void)unused;
    vim_cmd_activate();
}

/* ------------------------------------------------------------------------- */
/* Keys                                                                       */
/* ------------------------------------------------------------------------- */

static JotKey vim_key_of(guint keyval) {
    switch (keyval) {
    case GDK_KEY_Escape: return JK_ESC;
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: return JK_RETURN;
    case GDK_KEY_BackSpace: return JK_BACKSPACE;
    case GDK_KEY_Tab: case GDK_KEY_ISO_Left_Tab: return JK_TAB;
    case GDK_KEY_Left: case GDK_KEY_KP_Left: return JK_LEFT;
    case GDK_KEY_Right: case GDK_KEY_KP_Right: return JK_RIGHT;
    case GDK_KEY_Up: case GDK_KEY_KP_Up: return JK_UP;
    case GDK_KEY_Down: case GDK_KEY_KP_Down: return JK_DOWN;
    case GDK_KEY_Home: case GDK_KEY_KP_Home: return JK_HOME;
    case GDK_KEY_End: case GDK_KEY_KP_End: return JK_END;
    case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down: return JK_PAGE_DOWN;
    case GDK_KEY_Page_Up: case GDK_KEY_KP_Page_Up: return JK_PAGE_UP;
    case GDK_KEY_Delete: case GDK_KEY_KP_Delete: return JK_DELETE;
    case GDK_KEY_Insert: return JK_INSERT;
    default: return gdk_keyval_to_unicode(keyval) ? JK_CHAR : JK_NONE;
    }
}

/* Called first from jot.c's key handler. */
gboolean ui_key(guint keyval, GdkModifierType mods) {
    mods &= gtk_accelerator_get_default_mod_mask();
    if (mods & (GDK_ALT_MASK | GDK_SUPER_MASK))
        return FALSE;
    gboolean ctrl = (mods & GDK_CONTROL_MASK) != 0;
    return vim_key(vim_key_of(keyval), gdk_keyval_to_unicode(ctrl ? gdk_keyval_to_lower(keyval) : keyval), ctrl);
}

static gboolean on_cmd_key(GtkEventControllerKey *c, guint keyval, guint code, GdkModifierType mods, gpointer unused) {
    (void)c, (void)code, (void)unused;
    gboolean ctrl = (mods & GDK_CONTROL_MASK) != 0;
    return vim_cmd_key(vim_key_of(keyval), gdk_keyval_to_unicode(keyval), ctrl);
}

/* ------------------------------------------------------------------------- */
/* The view                                                                   */
/* ------------------------------------------------------------------------- */

void ui_block_cursor(gboolean block) {
    gtk_text_view_set_overwrite(J.view, block);
}

void ui_bell(void) {
    gtk_widget_error_bell(GTK_WIDGET(J.view));
}

void ui_scroll_mark_onscreen(GtkTextMark *mark) {
    gtk_text_view_scroll_mark_onscreen(J.view, mark);
}

void ui_scroll_mark_to(GtkTextMark *mark, double yalign) {
    gtk_text_view_scroll_to_mark(J.view, mark, 0, TRUE, 0, yalign);
}

void ui_scroll_iter_onscreen(const GtkTextIter *it) {
    GtkTextIter i = *it;
    gtk_text_view_scroll_to_iter(J.view, &i, 0.1, FALSE, 0, 0);
}

void ui_scroll_lines(const GtkTextIter *cursor, int dir, int count) {
    GtkAdjustment *adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(J.view));
    int y, h;
    gtk_text_view_get_line_yrange(J.view, cursor, &y, &h);
    double step = MAX(h, 16) * MAX(count, 1);
    gtk_adjustment_set_value(adj, gtk_adjustment_get_value(adj) + dir * step);
}

void ui_visible_lines(int *top, int *bottom) {
    GdkRectangle vis;
    gtk_text_view_get_visible_rect(J.view, &vis);
    GtkTextIter t, b;
    gtk_text_view_get_line_at_y(J.view, &t, vis.y, NULL);
    gtk_text_view_get_line_at_y(J.view, &b, vis.y + vis.height - 1, NULL);
    *top = gtk_text_iter_get_line(&t);
    *bottom = gtk_text_iter_get_line(&b);
}

double ui_iter_screen_pos(const GtkTextIter *it) {
    GdkRectangle vis;
    gtk_text_view_get_visible_rect(J.view, &vis);
    int y, h;
    gtk_text_view_get_line_yrange(J.view, it, &y, &h);
    return CLAMP((double)(y - vis.y) / MAX(vis.height, 1), 0.0, 1.0);
}

/* ------------------------------------------------------------------------- */
/* The system                                                                 */
/* ------------------------------------------------------------------------- */

void ui_quit(void) {
    gtk_window_close(J.win);
}

void ui_open_folder(const char *path) {
    g_autoptr(GFile) f = g_file_new_for_path(path);
    g_autoptr(GtkFileLauncher) l = gtk_file_launcher_new(f);
    gtk_file_launcher_launch(l, J.win, NULL, NULL, NULL);
}

static void url_launched(GObject *src, GAsyncResult *res, gpointer unused) {
    (void)unused;
    g_autoptr(GError) err = NULL;
    if (!gtk_uri_launcher_launch_finish(GTK_URI_LAUNCHER(src), res, &err) &&
        !g_error_matches(err, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)) {
        g_autofree char *msg = g_strdup_printf("Couldn't open link: %s", err->message);
        g_printerr("jot: %s\n", msg);
        toast(msg);
    }
}

void ui_open_uri(const char *uri) {
    g_autoptr(GtkUriLauncher) l = gtk_uri_launcher_new(uri);
    gtk_uri_launcher_launch(l, J.win, NULL, url_launched, NULL);
}

/* ------------------------------------------------------------------------- */
/* Clipboard                                                                  */
/* ------------------------------------------------------------------------- */

static void read_text_done(GObject *src, GAsyncResult *res, gpointer request) {
    g_autofree char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, NULL);
    vim_put_text(request, text);
}

static void read_image_done(GObject *src, GAsyncResult *res, gpointer request) {
    g_autoptr(GdkTexture) tex = gdk_clipboard_read_texture_finish(GDK_CLIPBOARD(src), res, NULL);
    vim_put_image(request, tex ? save_texture(tex) : NULL);
}

void ui_clipboard_read(void *request) {
    GdkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(J.view));
    GdkContentFormats *f = gdk_clipboard_get_formats(cb);
    gboolean has_text = gdk_content_formats_contain_gtype(f, G_TYPE_STRING);
    gboolean has_image = gdk_content_formats_contain_gtype(f, GDK_TYPE_TEXTURE);
    gsize n = 0;
    const char *const *mimes = gdk_content_formats_get_mime_types(f, &n);
    for (gsize i = 0; i < n; i++) {
        has_text |= g_str_has_prefix(mimes[i], "text/plain");
        has_image |= g_str_has_prefix(mimes[i], "image/");
    }
    if (has_text)
        gdk_clipboard_read_text_async(cb, NULL, read_text_done, request);
    else if (has_image)
        gdk_clipboard_read_texture_async(cb, NULL, read_image_done, request);
    else
        vim_put_none(request);
}

/* ------------------------------------------------------------------------- */
/* Colors (follow light/dark and the accent color)                            */
/* ------------------------------------------------------------------------- */

static JotTag **md_tags;

static void md_colors(void) {
    AdwStyleManager *sm = adw_style_manager_get_default();
    gboolean dark = adw_style_manager_get_dark(sm);
    GdkRGBA accent;
    adw_accent_color_to_standalone_rgba(adw_style_manager_get_accent_color(sm), dark, &accent);
    GdkRGBA ink = dark ? (GdkRGBA){1, 1, 1, 1} : (GdkRGBA){0, 0, 0, 1};
    GdkRGBA dim = ink, quote = ink, chip = ink, block = ink;
    dim.alpha = 0.40f;
    quote.alpha = 0.70f;
    chip.alpha = dark ? 0.10f : 0.07f;
    block.alpha = dark ? 0.06f : 0.04f;
    g_object_set(md_tags[T_DIM], "foreground-rgba", &dim, NULL);
    g_object_set(md_tags[T_DONE], "foreground-rgba", &dim, NULL);
    g_object_set(md_tags[T_QUOTE], "foreground-rgba", &quote, NULL);
    g_object_set(md_tags[T_CODE], "background-rgba", &chip, NULL);
    g_object_set(md_tags[T_CODEBLOCK], "paragraph-background-rgba", &block, NULL);
    g_object_set(md_tags[T_HEADING], "foreground-rgba", &accent, NULL);
    g_object_set(md_tags[T_BULLET], "foreground-rgba", &accent, NULL);
    g_object_set(md_tags[T_QUOTE_MARK], "foreground-rgba", &accent, NULL);
    g_object_set(md_tags[T_LINK], "foreground-rgba", &accent, NULL);
}

static void on_md_style(AdwStyleManager *sm, GParamSpec *ps, gpointer unused) {
    (void)sm, (void)ps, (void)unused;
    md_colors();
}

void ui_md_style(JotTag **tags) {
    md_tags = tags;
    g_object_set(md_tags[T_HEADING], "weight", PANGO_WEIGHT_BOLD, NULL);
    g_object_set(md_tags[T_BOLD], "weight", PANGO_WEIGHT_BOLD, NULL);
    g_object_set(md_tags[T_ITALIC], "style", PANGO_STYLE_ITALIC, NULL);
    g_object_set(md_tags[T_LINK], "underline", PANGO_UNDERLINE_SINGLE, NULL);
    g_object_set(md_tags[T_DONE], "strikethrough", TRUE, NULL);
    md_colors();
    AdwStyleManager *sm = adw_style_manager_get_default();
    g_signal_connect(sm, "notify::dark", G_CALLBACK(on_md_style), NULL);
    g_signal_connect(sm, "notify::accent-color", G_CALLBACK(on_md_style), NULL);
}

/* ------------------------------------------------------------------------- */
/* Setup                                                                      */
/* ------------------------------------------------------------------------- */

/* The bottom bar: mode / messages on the left (or the : / ? command line), pending keys and
 * line,col on the right. */
GtkWidget *ui_build_bar(void) {
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_widget_add_css_class(bar, "jot-statusline");
    gtk_widget_add_css_class(bar, "monospace"); /* the system monospace font, like the editor */

    U.stack = GTK_STACK(gtk_stack_new());
    gtk_widget_set_hexpand(GTK_WIDGET(U.stack), TRUE);
    U.mode_label = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_xalign(U.mode_label, 0);
    gtk_label_set_ellipsize(U.mode_label, PANGO_ELLIPSIZE_END);
    gtk_stack_add_named(U.stack, GTK_WIDGET(U.mode_label), "status");

    GtkWidget *cmdbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    U.cmd_prefix = GTK_LABEL(gtk_label_new(":"));
    U.cmd = GTK_TEXT(gtk_text_new());
    gtk_widget_set_hexpand(GTK_WIDGET(U.cmd), TRUE);
    g_signal_connect(U.cmd, "activate", G_CALLBACK(on_cmd_activate), NULL);
    g_signal_connect(U.cmd, "changed", G_CALLBACK(on_cmd_changed), NULL);
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_cmd_key), NULL);
    gtk_widget_add_controller(GTK_WIDGET(U.cmd), keys);
    gtk_box_append(GTK_BOX(cmdbox), GTK_WIDGET(U.cmd_prefix));
    gtk_box_append(GTK_BOX(cmdbox), GTK_WIDGET(U.cmd));
    gtk_stack_add_named(U.stack, cmdbox, "cmd");
    gtk_stack_set_visible_child_name(U.stack, "status");

    U.showcmd_label = GTK_LABEL(gtk_label_new(NULL));
    gtk_widget_add_css_class(GTK_WIDGET(U.showcmd_label), "dim-label");
    U.pos_label = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_width_chars(U.pos_label, 10);
    gtk_label_set_xalign(U.pos_label, 1);

    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(U.stack));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(U.showcmd_label));
    U.ocr_label = GTK_LABEL(gtk_label_new(NULL));
    gtk_widget_add_css_class(GTK_WIDGET(U.ocr_label), "dim-label");
    gtk_widget_set_visible(GTK_WIDGET(U.ocr_label), FALSE);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(U.ocr_label));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(U.pos_label));
    return bar;
}
