/* Headless UI tests for jot. Run with `make check` (uses GTK's Broadway backend). */
#define JOT_NO_MAIN
#include "jot.c"
#include <pango/pangocairo.h>

static int failures;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (cond) {                                       \
            g_print("ok    ");                            \
        } else {                                          \
            g_print("FAIL  ");                            \
            failures++;                                   \
        }                                                 \
        g_print(__VA_ARGS__);                             \
        g_print("\n");                                    \
    } while (0)

static char *file_text(void) {
    char *t = NULL;
    g_file_get_contents(page_path, &t, NULL, NULL);
    return t ? t : g_strdup("<missing>");
}

static GdkTexture *make_texture(int w, int h) {
    gsize stride = (gsize)w * 4;
    guchar *px = g_malloc(stride * h);
    for (gsize i = 0; i < stride * h; i += 4) {
        px[i] = 0x33; px[i + 1] = 0x66; px[i + 2] = 0xaa; px[i + 3] = 0xff;
    }
    g_autoptr(GBytes) b = g_bytes_new_take(px, stride * h);
    return gdk_memory_texture_new(w, h, GDK_MEMORY_R8G8B8A8, b, stride);
}

/* A dark-theme "terminal screenshot" with real text in it, for OCR. */
static GdkTexture *make_text_texture(const char *text) {
    const int w = 720, h = 110;
    cairo_surface_t *cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(cs);
    cairo_set_source_rgb(cr, 0x1e / 255.0, 0x1f / 255.0, 0x24 / 255.0);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
    PangoLayout *l = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string("Monospace 14");
    pango_layout_set_font_description(l, fd);
    pango_layout_set_text(l, text, -1);
    cairo_move_to(cr, 14, 14);
    pango_cairo_show_layout(cr, l);
    cairo_surface_flush(cs);
    int stride = cairo_image_surface_get_stride(cs);
    g_autoptr(GBytes) b = g_bytes_new(cairo_image_surface_get_data(cs), (gsize)stride * h);
    GdkTexture *tex = gdk_memory_texture_new(w, h, GDK_MEMORY_DEFAULT, b, stride);
    pango_font_description_free(fd);
    g_object_unref(l);
    cairo_destroy(cr);
    cairo_surface_destroy(cs);
    return tex;
}

static void set_clipboard_bytes(const char *mime, GBytes *b) {
    g_autoptr(GdkContentProvider) p = gdk_content_provider_new_for_bytes(mime, b);
    gdk_clipboard_set_content(gtk_widget_get_clipboard(GTK_WIDGET(J.view)), p);
}

static JotImage *first_image(void) {
    GtkTextIter it;
    gtk_text_buffer_get_start_iter(J.buf, &it);
    do {
        GdkPaintable *p = gtk_text_iter_get_paintable(&it);
        if (JOT_IS_IMAGE(p))
            return JOT_IMAGE(p);
    } while (gtk_text_iter_forward_find_char(&it, is_obj_char, NULL, NULL));
    return NULL;
}

static int text_width(void) {
    GdkRectangle vis;
    gtk_text_view_get_visible_rect(J.view, &vis);
    return vis.width;
}

static int gutter_natural_width(void) {
    int min, nat;
    gtk_widget_measure(J.gutter, GTK_ORIENTATION_HORIZONTAL, -1, &min, &nat, NULL, NULL);
    return nat;
}

/* Renders the window content to a PNG when $JOT_SHOTS names a directory (for eyeballing). */
static void screenshot(const char *name) {
    const char *dir = g_getenv("JOT_SHOTS");
    if (!dir || !*dir)
        return;
    GtkWidget *w = gtk_window_get_child(J.win);
    g_autoptr(GdkPaintable) p = gtk_widget_paintable_new(w);
    g_autoptr(GtkSnapshot) snap = gtk_snapshot_new();
    gdk_paintable_snapshot(p, GDK_SNAPSHOT(snap), gtk_widget_get_width(w), gtk_widget_get_height(w));
    g_autoptr(GskRenderNode) node = gtk_snapshot_free_to_node(g_steal_pointer(&snap));
    if (!node)
        return;
    GskRenderer *r = gtk_native_get_renderer(GTK_NATIVE(J.win));
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, gtk_widget_get_width(w), gtk_widget_get_height(w));
    g_autoptr(GdkTexture) tex = gsk_renderer_render_texture(r, node, &bounds);
    g_autofree char *path = g_build_filename(dir, name, NULL);
    gdk_texture_save_to_png(tex, path);
}

static char *selected_text(void) {
    GtkTextIter s, e;
    if (!gtk_text_buffer_get_selection_bounds(J.buf, &s, &e))
        return g_strdup("");
    return gtk_text_buffer_get_text(J.buf, &s, &e, FALSE);
}

static int selection_line(void) {
    GtkTextIter s, e;
    gtk_text_buffer_get_selection_bounds(J.buf, &s, &e);
    return gtk_text_iter_get_line(&s);
}

static void select_all(void) {
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(J.buf, &s, &e);
    gtk_text_buffer_select_range(J.buf, &s, &e);
}

static char *img_note_text, *ocr_rel, *clip_text;
static int width_before_resize, waited, gutter_w3;
static int step_no;

/* Each step runs ~300ms after the previous one so async clipboard reads and layout settle. */
static gboolean step(gpointer unused) {
    (void)unused;
    g_autofree char *t = NULL;
    switch (step_no++) {
    case 0:
        gtk_widget_grab_focus(GTK_WIDGET(J.view));
        gtk_text_buffer_insert_at_cursor(J.buf, "TODO: fix flaky test\n\n", -1);
        {
            /* Rich content: html + plain. Plain must win. */
            g_autoptr(GBytes) html = g_bytes_new_static("<h1>Big</h1>", 12);
            g_autoptr(GBytes) plain = g_bytes_new_static("def foo():\n    return 42\n", 25);
            GdkContentProvider *ps[] = {gdk_content_provider_new_for_bytes("text/html", html),
                                        gdk_content_provider_new_for_bytes("text/plain;charset=utf-8", plain)};
            g_autoptr(GdkContentProvider) u = gdk_content_provider_new_union(ps, 2);
            gdk_clipboard_set_content(gtk_widget_get_clipboard(GTK_WIDGET(J.view)), u);
        }
        g_signal_emit_by_name(J.view, "paste-clipboard");
        break;
    case 1: {
        /* A HiDPI-sized screenshot. */
        g_autoptr(GdkTexture) tex = make_texture(2880, 1800);
        g_autoptr(GBytes) png = gdk_texture_save_to_png_bytes(tex);
        set_clipboard_bytes("image/png", png);
        g_signal_emit_by_name(J.view, "paste-clipboard");
        break;
    }
    case 2:
        if (!first_image()) {
            step_no--; /* async image paste still decoding; wait */
            break;
        }
        gtk_text_buffer_insert_at_cursor(J.buf, "after the image", -1);
        break;
    case 3: {
        save();
        t = file_text();
        g_autoptr(GRegex) re = g_regex_new("^TODO: fix flaky test\n\ndef foo\\(\\):\n    return 42\n"
                                           "!\\[\\]\\(assets/[0-9-]+-[0-9a-f]{6}\\.png\\)\nafter the image\n$",
                                           0, 0, NULL);
        CHECK(g_regex_match(re, t, 0, NULL), "paste: plain text + image saved to jot.md");
        img_note_text = g_strdup(t);
        JotImage *img = first_image();
        int vw = text_width();
        CHECK(img && img->max_w == vw - 14 - 28 - 4, "pasted image fits text area (max_w=%d, text area=%d)",
              img ? img->max_w : -1, vw);
        int gw = gtk_widget_get_width(J.gutter);
        CHECK(gw > 0 && vw + gw == gtk_widget_get_width(GTK_WIDGET(J.view)),
              "gutter sits beside the text area (gutter=%d, text=%d)", gw, vw);
        screenshot("shot-gutter.png");
        GtkTextIter it;
        cursor_iter(&it);
        CHECK(gtk_text_iter_get_line(&it) == 5, "cursor ends after typed text on line 5 (got %d)",
              gtk_text_iter_get_line(&it));
        break;
    }
    case 4:
        width_before_resize = text_width();
        gtk_window_unmaximize(J.win);
        gtk_window_set_default_size(J.win, 600, 600);
        break;
    case 5:
    case 6:
        break; /* let the resize land */
    case 7: {
        JotImage *img = first_image();
        int vw = text_width();
        if ((vw == width_before_resize || (img && img->max_w != vw - 14 - 28 - 4)) && waited++ < 20) {
            /* Broadway sometimes drops a resize of a mapped window; ask again. The refit itself
             * is deferred to idle, which is slow under ASan. */
            gtk_window_set_default_size(J.win, waited % 2 ? 610 : 600, 600);
            step_no--;
            break;
        }
        waited = 0;
        CHECK(img && vw != width_before_resize && img->max_w == vw - 14 - 28 - 4,
              "image refits after resize (view %d -> %d, max_w=%d)", width_before_resize, vw, img ? img->max_w : -1);
        /* cut everything, then paste it back: the image should come along */
        select_all();
        g_signal_emit_by_name(J.view, "cut-clipboard");
        t = serialize(J.buf);
        CHECK(*t == '\0', "cut empties the page");
        g_signal_emit_by_name(J.view, "paste-clipboard");
        break;
    }
    case 8: {
        save();
        t = file_text();
        CHECK(g_str_equal(t, img_note_text), "cut/paste keeps image");
        /* lone image selected -> clipboard offers a real image/png */
        GtkTextIter s, e;
        gtk_text_buffer_get_start_iter(J.buf, &s);
        gtk_text_iter_forward_find_char(&s, is_obj_char, NULL, NULL);
        e = s;
        gtk_text_iter_forward_char(&e);
        gtk_text_buffer_select_range(J.buf, &s, &e);
        g_signal_emit_by_name(J.view, "copy-clipboard");
        GdkContentFormats *f = gdk_clipboard_get_formats(gtk_widget_get_clipboard(GTK_WIDGET(J.view)));
        CHECK(gdk_content_formats_contain_mime_type(f, "image/png") &&
                  gdk_content_formats_contain_mime_type(f, "text/plain;charset=utf-8"),
              "copying a lone image offers image/png and text");
        break;
    }
    case 9:
        /* indent dance on all lines: +1, -1, -1 */
        select_all();
        indent_lines(1);
        indent_lines(-1);
        indent_lines(-1);
        save();
        t = file_text();
        CHECK(strstr(t, "def foo():\nreturn 42\n") != NULL, "indent/dedent selected lines");
        break;
    case 10: {
        /* reload from disk: exact round trip, cursor at the end */
        load_page();
        t = serialize(J.buf);
        g_autofree char *disk = file_text();
        g_autofree char *with_nl = g_strconcat(t, "\n", NULL);
        CHECK(g_str_equal(with_nl, disk), "reload round-trips exactly");
        GtkTextIter it;
        cursor_iter(&it);
        CHECK(gtk_text_iter_is_end(&it), "cursor starts at end of page");
        break;
    }
    case 11: {
        /* find: case-insensitive, next wraps, previous goes back */
        set_text("alpha FLAKY\nbeta\nflaky gamma\nflaky delta");
        GtkTextIter it;
        gtk_text_buffer_get_start_iter(J.buf, &it);
        gtk_text_buffer_place_cursor(J.buf, &it);
        open_find();
        CHECK(gtk_search_bar_get_search_mode(J.find_bar), "Ctrl+F opens find bar");
        gtk_editable_set_text(GTK_EDITABLE(J.find_entry), "flaky");
        find(TRUE, FALSE);
        t = selected_text();
        CHECK(g_str_equal(t, "FLAKY") && selection_line() == 0, "find selects first match (case-insensitive)");
        find(TRUE, TRUE);
        CHECK(selection_line() == 2, "next -> line 2 (got %d)", selection_line());
        find(TRUE, TRUE);
        find(TRUE, TRUE);
        CHECK(selection_line() == 0, "next wraps to line 0 (got %d)", selection_line());
        find(FALSE, TRUE);
        CHECK(selection_line() == 3, "previous wraps back to line 3 (got %d)", selection_line());
        gtk_editable_set_text(GTK_EDITABLE(J.find_entry), "zzz");
        find(TRUE, FALSE);
        CHECK(gtk_widget_has_css_class(GTK_WIDGET(J.find_entry), "error"), "no match marks entry red");
        gtk_search_bar_set_search_mode(J.find_bar, FALSE);
        CHECK(gtk_widget_has_focus(GTK_WIDGET(J.view)), "closing find returns focus to editor");
        break;
    }
    case 12: {
        /* external edit is picked up on focus */
        save();
        g_usleep(20000);
        g_file_set_contents(page_path, "edited in vim\n", -1, NULL);
        on_active(J.win, NULL, NULL);
        if (!gtk_window_is_active(J.win))
            load_page(); /* headless window may not be "active"; exercise the reload path directly */
        t = serialize(J.buf);
        CHECK(g_str_equal(t, "edited in vim"), "external edit reloaded: %s", t);
        break;
    }
    case 13: {
        /* auto-indent on Enter, Tab inserts spaces */
        set_text("");
        gtk_text_buffer_insert_at_cursor(J.buf, "    indented", -1);
        on_key(NULL, GDK_KEY_Return, 0, 0, NULL);
        on_key(NULL, GDK_KEY_Tab, 0, 0, NULL);
        gtk_text_buffer_insert_at_cursor(J.buf, "x", -1);
        t = serialize(J.buf);
        CHECK(g_str_equal(t, "    indented\n        x"), "enter keeps indent, tab inserts 4 spaces");
        /* an emptied page is saved as an empty file */
        set_text("x");
        select_all();
        gtk_text_buffer_delete_selection(J.buf, TRUE, TRUE);
        save();
        g_autofree char *disk = file_text();
        CHECK(g_str_equal(disk, ""), "emptied page saves as empty file");
        /* gutter widens for 4-digit line numbers, and narrows back */
        int w3 = gutter_natural_width();
        g_autoptr(GString) many = g_string_new(NULL);
        for (int i = 0; i < 1200; i++)
            g_string_append_printf(many, "line %d\n", i + 1);
        set_text(many->str);
        int w4 = gutter_natural_width();
        CHECK(JOT_GUTTER(J.gutter)->digits == 4 && w4 > w3, "gutter widens for 4 digits (%d -> %d)", w3, w4);
        set_text("");
        CHECK(JOT_GUTTER(J.gutter)->digits == 3 && gutter_natural_width() == w3, "gutter narrows back");
        gutter_w3 = w3;
        zoom(4); /* CSS applies on the next frame */
        break;
    }
    case 14:
        if (gutter_natural_width() <= gutter_w3 && waited++ < 10) {
            step_no--;
            break;
        }
        waited = 0;
        CHECK(gutter_natural_width() > gutter_w3, "gutter follows zoom (%d -> %d)", gutter_w3, gutter_natural_width());
        zoom(0);
        g_signal_emit_by_name(J.view, "paste-clipboard"); /* clipboard still holds the lone image */
        break;
    case 15: {
        GtkTextIter e;
        CHECK(first_image() != NULL, "pasting a copied lone image inserts an image");
        gtk_text_buffer_get_end_iter(J.buf, &e);
        insert_text(J.buf, &e, "\n![](assets/nope.png)\n![](assets/../escape.png)");
        t = serialize(J.buf);
        CHECK(strstr(t, "![](assets/nope.png)") && strstr(t, "![](assets/../escape.png)"),
              "missing/unsafe image refs stay as plain text");
        if (!tesseract_path) {
            g_print("skip  OCR tests: tesseract not installed\n");
            step_no = 99;
            break;
        }
        /* ---- OCR ---- */
        set_text("");
        g_autoptr(GdkTexture) tex = make_text_texture("Segmentation fault in parse_config()\n    at src/main.c:42");
        g_autoptr(GBytes) png = gdk_texture_save_to_png_bytes(tex);
        set_clipboard_bytes("image/png", png);
        g_signal_emit_by_name(J.view, "paste-clipboard");
        break;
    }
    case 16: {
        JotImage *img = first_image();
        g_autofree char *side = img ? ocr_sidecar(img->path) : NULL;
        if ((!side || !g_file_test(side, G_FILE_TEST_EXISTS)) && waited++ < 40) {
            step_no--; /* waiting for paste + tesseract */
            break;
        }
        waited = 0;
        ocr_rel = img ? g_strdup(img->path) : NULL;
        g_autofree char *ocr = NULL;
        if (side)
            g_file_get_contents(side, &ocr, NULL, NULL);
        CHECK(ocr && strstr(ocr, "Segmentation fault") && strstr(ocr, "parse_config"),
              "pasted dark screenshot is OCR'd into %s.txt: %s", ocr_rel, ocr ? ocr : "(none)");
        /* page: text, the image, more text */
        g_autofree char *page = g_strdup_printf("intro\n![](%s)\nlater: parse_config is called here", ocr_rel);
        set_text(page);
        GtkTextIter it;
        gtk_text_buffer_get_start_iter(J.buf, &it);
        gtk_text_buffer_place_cursor(J.buf, &it);
        open_find();
        gtk_editable_set_text(GTK_EDITABLE(J.find_entry), "segmentation");
        find(TRUE, FALSE);
        JotImage *sel = selected_image();
        CHECK(sel && sel->selected, "find matches text inside an image and outlines it");
        CHECK(g_str_has_prefix(gtk_label_get_text(J.find_note), "in image: Segmentation"),
              "find bar shows the matching image line: %s", gtk_label_get_text(J.find_note));
        gtk_text_buffer_get_start_iter(J.buf, &it);
        gtk_text_buffer_place_cursor(J.buf, &it);
        gtk_editable_set_text(GTK_EDITABLE(J.find_entry), "parse_config");
        find(TRUE, FALSE);
        CHECK(selected_image() != NULL, "image match comes before a later text match");
        find(TRUE, TRUE);
        t = selected_text();
        CHECK(selected_image() == NULL && g_str_equal(t, "parse_config") && selection_line() == 2,
              "next goes on to the text match");
        CHECK(*gtk_label_get_text(J.find_note) == '\0', "note clears for text matches");
        find(TRUE, TRUE);
        CHECK(selected_image() != NULL, "next wraps back around to the image");
        find(FALSE, TRUE);
        CHECK(selected_image() == NULL && selection_line() == 2, "previous wraps to the text match");
        gtk_search_bar_set_search_mode(J.find_bar, FALSE);
        CHECK(first_image() && !first_image()->selected, "outline goes away when selection moves off image");

        /* Copy Text from Image (context menu / Ctrl+Shift+C) on the selected image */
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_line(J.buf, &a, 1);
        b = a;
        gtk_text_iter_forward_char(&b);
        gtk_text_buffer_select_range(J.buf, &a, &b);
        GAction *copy = g_action_map_lookup_action(G_ACTION_MAP(J.win), "copy-image-text");
        CHECK(g_action_get_enabled(copy), "copy-image-text enabled when an image is selected");
        g_action_activate(copy, NULL);
        break;
    }
    case 17: {
        GdkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(J.view));
        GdkContentProvider *cp = gdk_clipboard_get_content(cb);
        GValue v = G_VALUE_INIT;
        g_value_init(&v, G_TYPE_STRING);
        gboolean got = cp && gdk_content_provider_get_value(cp, &v, NULL);
        clip_text = got ? g_value_dup_string(&v) : NULL;
        g_value_unset(&v);
        CHECK(clip_text && strstr(clip_text, "Segmentation fault"), "image text copied to clipboard: %s",
              clip_text ? clip_text : "(none)");
        /* nothing selected -> action disabled (and hidden in the menu) */
        GtkTextIter it;
        gtk_text_buffer_get_start_iter(J.buf, &it);
        gtk_text_buffer_place_cursor(J.buf, &it);
        GAction *copy = g_action_map_lookup_action(G_ACTION_MAP(J.win), "copy-image-text");
        CHECK(!g_action_get_enabled(copy), "copy-image-text disabled without an image selection");
        /* images already on the page are (re)read on load when their .txt is missing */
        g_autofree char *page = serialize(J.buf);
        g_file_set_contents(page_path, page, -1, NULL);
        g_autofree char *side = ocr_sidecar(ocr_rel);
        g_unlink(side);
        g_hash_table_remove(ocr_text, ocr_rel);
        load_page();
        CHECK(g_hash_table_contains(ocr_pending, ocr_rel), "loading the page queues unread images");
        break;
    }
    case 18: {
        g_autofree char *side = ocr_sidecar(ocr_rel);
        if (!g_file_test(side, G_FILE_TEST_EXISTS) && waited++ < 40) {
            step_no--;
            break;
        }
        waited = 0;
        CHECK(g_file_test(side, G_FILE_TEST_EXISTS), "unread image gets OCR'd in the background");
        /* without tesseract the image items are disabled (hidden) */
        char *saved = tesseract_path;
        tesseract_path = NULL;
        GtkTextIter a, b;
        gtk_text_buffer_get_iter_at_line(J.buf, &a, 1);
        b = a;
        gtk_text_iter_forward_char(&b);
        gtk_text_buffer_select_range(J.buf, &a, &b);
        GAction *copy = g_action_map_lookup_action(G_ACTION_MAP(J.win), "copy-image-text");
        GAction *open = g_action_map_lookup_action(G_ACTION_MAP(J.win), "open-image");
        CHECK(!g_action_get_enabled(copy) && g_action_get_enabled(open),
              "no tesseract: copy-text hidden, open-image still available");
        tesseract_path = saved;
        step_no = 99;
        break;
    }
    case 99:
        g_print("\n%s\n", failures ? "SOME TESTS FAILED" : "all tests passed");
        g_application_quit(G_APPLICATION(J.app));
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void on_test_activate(GtkApplication *app, gpointer d) {
    on_activate(app, d);
    g_timeout_add(300, step, NULL);
}

int main(void) {
    init_paths();
    g_autoptr(AdwApplication) app = adw_application_new("dev.jot.JotTest", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_test_activate), NULL);
    g_application_run(G_APPLICATION(app), 0, NULL);
    return failures ? 1 : 0;
}
