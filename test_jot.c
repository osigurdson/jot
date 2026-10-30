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

/* Types keys as the key handler would: control chars for Esc (\x1b) and Enter (\r);
 * in insert mode text is typed into the buffer; with : or / open, into the command line. */
static void feed(const char *keys) {
    for (const char *p = keys; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        char b[8] = {0};
        g_unichar_to_utf8(c, b);
        if (cmdline_is_open()) {
            if (c == '\r') {
                g_signal_emit_by_name(V.cmd, "activate");
            } else if (c == K_ESC) {
                cmdline_cancel();
            } else {
                g_autofree char *t = g_strconcat(gtk_editable_get_text(GTK_EDITABLE(V.cmd)), b, NULL);
                gtk_editable_set_text(GTK_EDITABLE(V.cmd), t);
            }
        } else if (V.mode == MODE_INSERT && c != K_ESC) {
            if (c == '\r')
                on_key(NULL, GDK_KEY_Return, 0, 0, NULL);
            else if (c == '\t')
                on_key(NULL, GDK_KEY_Tab, 0, 0, NULL);
            else
                gtk_text_buffer_insert_interactive_at_cursor(J.buf, b, -1, TRUE);
        } else {
            vim_input(c);
        }
    }
}

/* Sets the page and puts the (normal mode) cursor at line, col. */
static void page_at(const char *text, int line, int col) {
    if (V.mode == MODE_INSERT)
        leave_insert();
    if (is_visual())
        exit_visual(-1);
    set_text(text);
    GtkTextIter it;
    iter_line_col(&it, line, col);
    set_cursor(&it);
    g_string_truncate(V.pending, 0);
}

static int cur_line(void) {
    GtkTextIter it;
    get_cursor(&it);
    return gtk_text_iter_get_line(&it);
}

static int cur_col(void) {
    GtkTextIter it;
    get_cursor(&it);
    return gtk_text_iter_get_line_offset(&it);
}

static char *page_text(void) {
    return serialize(J.buf);
}

#define CHECK_PAGE(expect, ...)                                                                      \
    do {                                                                                             \
        g_autofree char *_t = page_text();                                                           \
        CHECK(g_str_equal(_t, expect), __VA_ARGS__);                                                 \
        if (!g_str_equal(_t, expect))                                                                \
            g_print("      got: \"%s\"\n", g_strescape(_t, NULL));                                   \
    } while (0)

#define CHECK_POS(l, c, what) CHECK(cur_line() == (l) && cur_col() == (c), "%s -> %d,%d (got %d,%d)", what, l, c, cur_line(), cur_col())

/* Whether md tag t covers the char at line, col (after styling catches up). */
static gboolean has_tag(int line, int col, int t) {
    md_flush(NULL);
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line_offset(J.buf, &it, line, col);
    return gtk_text_iter_has_tag(&it, M.tag[t]);
}

static gboolean plain(int line, int col) {
    for (int t = 0; t < N_TAGS; t++)
        if (has_tag(line, col, t))
            return FALSE;
    return TRUE;
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
        /* / search: smartcase, n wraps, N goes back, ? searches backward */
        page_at("alpha FLAKY\nbeta\nflaky gamma\nflaky delta", 0, 0);
        feed("/flaky\r");
        t = selected_text();
        CHECK(g_str_equal(t, "FLAKY") && selection_line() == 0, "/flaky finds FLAKY (lowercase = ignore case)");
        CHECK(!gtk_widget_has_focus(GTK_WIDGET(V.cmd)) && gtk_widget_has_focus(GTK_WIDGET(J.view)),
              "Enter closes the prompt, focus back in editor");
        feed("n");
        CHECK(cur_line() == 2, "n -> line 2 (got %d)", cur_line());
        feed("n");
        feed("n");
        CHECK(cur_line() == 0, "n wraps to line 0 (got %d)", cur_line());
        CHECK(g_str_has_prefix(gtk_label_get_text(V.mode_label), "search hit BOTTOM"), "wrap message shown");
        feed("N");
        CHECK(cur_line() == 3, "N wraps back to line 3 (got %d)", cur_line());
        feed("/FLAKY\r");
        CHECK(cur_line() == 0, "/FLAKY is case-sensitive (smartcase) -> line 0 (got %d)", cur_line());
        page_at("alpha FLAKY\nbeta\nflaky gamma\nflaky delta", 3, 3);
        feed("?gamma\r");
        CHECK_POS(2, 6, "?gamma searches backward");
        feed("/zzz\r");
        CHECK(gtk_widget_has_css_class(GTK_WIDGET(V.mode_label), "error") &&
                  strstr(gtk_label_get_text(V.mode_label), "E486"),
              "no match -> E486 error");
        CHECK_POS(2, 6, "failed search leaves cursor");
        feed("/bet");
        CHECK(selection_line() == 1, "incremental search highlights while typing");
        feed("\x1b");
        CHECK_POS(2, 6, "Esc in the prompt cancels and restores the cursor");
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
        page_at("", 0, 0);
        feed("i    indented\r\tx\x1b");
        t = serialize(J.buf);
        CHECK(g_str_equal(t, "    indented\n        x"), "insert mode: enter keeps indent, tab inserts 4 spaces");
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
            step_no = 19;
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
        if ((!side || !g_file_test(side, G_FILE_TEST_EXISTS) || g_hash_table_contains(ocr_pending, img->path)) &&
            waited++ < 40) {
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
        page_at(page, 0, 0);
        feed("/segmentation\r");
        JotImage *sel = V.hl_image;
        CHECK(sel && sel->selected && !gtk_text_buffer_get_has_selection(J.buf),
              "/ finds text inside an image and outlines it (without a selection tint)");
        CHECK(g_str_has_prefix(gtk_label_get_text(V.mode_label), "in image: Segmentation"),
              "status line shows the matching image line: %s", gtk_label_get_text(V.mode_label));
        {
            g_autofree char *tsv = ocr_sidecar_ext(ocr_rel, ".tsv");
            CHECK(g_file_test(tsv, G_FILE_TEST_EXISTS), "word positions saved in %s.tsv", ocr_rel);
        }
        /* The image is 720x110; "Segmentation" is the first word of the first line, at x=14 y=14. */
        CHECK(sel && sel->hl && sel->hl->len == 1, "the matched word is highlighted (%u rects)",
              sel && sel->hl ? sel->hl->len : 0);
        if (sel && sel->hl && sel->hl->len) {
            graphene_rect_t *r = &g_array_index(sel->hl, graphene_rect_t, 0);
            CHECK(r->origin.x > 5 && r->origin.x < 30 && r->origin.y > 5 && r->origin.y < 30 && r->size.width > 80 &&
                      r->size.width < 200 && r->size.height > 10 && r->size.height < 40,
                  "highlight sits on the word (%.0f,%.0f %.0fx%.0f)", r->origin.x, r->origin.y, r->size.width,
                  r->size.height);
        }
        page_at(page, 0, 0);
        feed("/fault in parse\r");
        sel = V.hl_image;
        CHECK(sel && sel->hl && sel->hl->len == 1, "a phrase across words is one highlight");
        if (sel && sel->hl && sel->hl->len) {
            graphene_rect_t *r = &g_array_index(sel->hl, graphene_rect_t, 0);
            CHECK(r->size.width > 120 && r->origin.x > 100, "...spanning the words (%.0f,%.0f %.0fx%.0f)",
                  r->origin.x, r->origin.y, r->size.width, r->size.height);
        }
        page_at(page, 0, 0);
        feed("/main.c\r");
        sel = V.hl_image;
        CHECK(sel && sel->hl && sel->hl->len == 1 && g_array_index(sel->hl, graphene_rect_t, 0).origin.y > 30,
              "a match on the second line is highlighted there");
        feed("j");
        CHECK(first_image() && !first_image()->hl, "highlight clears when the cursor moves on");
        page_at(page, 0, 0);
        feed("/parse_config\r");
        CHECK(V.hl_image != NULL, "image match comes before a later text match");
        CHECK(V.hl_image && V.hl_image->hl, "image match is highlighted");
        feed("n");
        t = selected_text();
        CHECK(V.hl_image == NULL && g_str_equal(t, "parse_config") && selection_line() == 2,
              "n goes on to the text match");
        CHECK(first_image() && !first_image()->hl, "...and the image highlight goes away");
        feed("n");
        CHECK(V.hl_image != NULL && cur_line() == 1, "n wraps back around to the image");
        feed("N");
        CHECK(V.hl_image == NULL && selection_line() == 2, "N wraps back to the text match");
        feed("gg");
        CHECK(first_image() && !first_image()->selected, "outline goes away when the cursor moves off the image");

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
        break;
    }
    case 19: {
        /* ---- vim: motions ---- */
        page_at("hello world foo\nsecond line here\n\nlast", 0, 0);
        feed("w");
        CHECK_POS(0, 6, "w");
        feed("e");
        CHECK_POS(0, 10, "e");
        feed("b");
        CHECK_POS(0, 6, "b");
        feed("$");
        CHECK_POS(0, 14, "$ (on last char)");
        feed("0");
        CHECK_POS(0, 0, "0");
        feed("3l");
        CHECK_POS(0, 3, "3l");
        feed("j");
        CHECK_POS(1, 3, "j keeps column");
        feed("jj");
        CHECK_POS(3, 3, "j through an empty line keeps the wanted column");
        feed("gg");
        CHECK_POS(0, 0, "gg");
        feed("G");
        CHECK_POS(3, 0, "G");
        feed("2G");
        CHECK_POS(1, 0, "2G");
        feed("gg}");
        CHECK_POS(2, 0, "} stops at the blank line");
        feed("gg");
        feed("fo");
        CHECK_POS(0, 4, "fo");
        feed(";");
        CHECK_POS(0, 7, "; repeats f");
        feed(",");
        CHECK_POS(0, 4, ", reverses");
        feed("tw");
        CHECK_POS(0, 5, "tw stops before w");
        feed("ll");
        CHECK_POS(0, 7, "l");
        feed("10l");
        CHECK_POS(0, 14, "l stops at the last char");
        feed("hh");
        page_at("if (a[1] == b) { x(); }", 0, 3);
        feed("%");
        CHECK_POS(0, 13, "% jumps to the matching bracket");
        feed("%");
        CHECK_POS(0, 3, "% jumps back");

        /* ---- operators ---- */
        page_at("hello world foo", 0, 0);
        feed("dw");
        CHECK_PAGE("world foo", "dw");
        feed("d2w");
        CHECK_PAGE("", "d2w");
        page_at("hello world foo", 0, 6);
        feed("D");
        CHECK_PAGE("hello ", "D");
        CHECK_POS(0, 5, "D leaves cursor on the last char");
        page_at("hello world", 0, 0);
        feed("cwbye\x1b");
        CHECK_PAGE("bye world", "cw (changes to end of word)");
        CHECK(V.mode == MODE_NORMAL && cur_col() == 2, "Esc returns to normal mode, cursor steps back");
        feed("u");
        CHECK_PAGE("hello world", "u undoes the whole change (cw + typing) at once");
        feed("\x12");
        CHECK_PAGE("bye world", "Ctrl+R redoes it");
        page_at("say \"hi there\" now", 0, 7);
        feed("ci\"yo\x1b");
        CHECK_PAGE("say \"yo\" now", "ci\" changes inside quotes");
        page_at("f(a, (b), c)", 0, 2);
        feed("di(");
        CHECK_PAGE("f()", "di( deletes inside the enclosing parens");
        page_at("f(a, (b), c)", 0, 6);
        feed("da(");
        CHECK_PAGE("f(a, , c)", "da( deletes the inner parens");
        page_at("one two three", 0, 5);
        feed("diw");
        CHECK_PAGE("one  three", "diw");
        page_at("one two three", 0, 5);
        feed("daw");
        CHECK_PAGE("one three", "daw takes the trailing space");
        page_at("abcdef", 0, 1);
        feed("3x");
        CHECK_PAGE("aef", "3x");
        feed("X");
        CHECK_PAGE("ef", "X");
        page_at("abc", 0, 1);
        feed("rZ");
        CHECK_PAGE("aZc", "r");
        feed("~");
        CHECK_PAGE("azc", "~ toggles case");
        CHECK_POS(0, 2, "~ advances");
        page_at("one\n   two\nthree", 0, 0);
        feed("J");
        CHECK_PAGE("one two\nthree", "J joins with one space");
        CHECK_POS(0, 3, "J puts the cursor at the join");
        page_at("a\nb", 0, 0);
        feed(">>");
        CHECK_PAGE("    a\nb", ">>");
        feed("<<");
        CHECK_PAGE("a\nb", "<<");
        feed("2>>");
        CHECK_PAGE("    a\n    b", "2>>");

        /* ---- lines ---- */
        page_at("one\ntwo\nthree", 1, 1);
        feed("dd");
        CHECK_PAGE("one\nthree", "dd");
        CHECK_POS(1, 0, "dd leaves cursor on the next line");
        feed("dd");
        CHECK_PAGE("one", "dd on the last line takes the newline before it");
        page_at("one\ntwo\nthree\nfour", 0, 0);
        feed("2dd");
        CHECK_PAGE("three\nfour", "2dd");
        page_at("one\ntwo\nthree", 0, 0);
        feed("dj");
        CHECK_PAGE("three", "dj is linewise");
        page_at("  one\ntwo", 0, 3);
        feed("cc");
        CHECK(V.mode == MODE_INSERT, "cc enters insert mode");
        feed("x\x1b");
        CHECK_PAGE("  x\ntwo", "cc keeps the indent");
        page_at("  one", 0, 4);
        feed("o");
        feed("next\x1b");
        CHECK_PAGE("  one\n  next", "o opens an indented line below");
        feed("Otop\x1b");
        CHECK_PAGE("  one\n  top\n  next", "O opens a line above");
        page_at("mid", 0, 1);
        feed("Ax\x1bIy\x1b");
        CHECK_PAGE("ymidx", "A and I");
        page_at("ab", 0, 0);
        feed("ax\x1b");
        CHECK_PAGE("axb", "a appends after the cursor char");

        /* ---- visual ---- */
        page_at("one two\nthree four", 0, 4);
        feed("vjd");
        CHECK_PAGE("one  four", "v j d deletes charwise across lines (inclusive)");
        page_at("one\ntwo\nthree", 0, 1);
        feed("Vjd");
        CHECK_PAGE("three", "V j d deletes whole lines");
        page_at("one two three", 0, 4);
        feed("viwU");
        CHECK_PAGE("one TWO three", "viw U uppercases the word");
        CHECK(V.mode == MODE_NORMAL, "operator ends visual mode");
        page_at("a\nb\nc", 0, 0);
        feed("Vj>");
        CHECK_PAGE("    a\n    b\nc", "V j > indents the lines");
        page_at("one two", 0, 0);
        feed("ve");
        CHECK(V.mode == MODE_VISUAL, "v enters visual mode");
        CHECK(g_str_equal(gtk_label_get_text(V.mode_label), "-- VISUAL --"), "status shows -- VISUAL --");
        t = selected_text();
        CHECK(g_str_equal(t, "one"), "v e selects inclusively: %s", t);
        feed("\x1b");
        CHECK(V.mode == MODE_NORMAL && !gtk_text_buffer_get_has_selection(J.buf), "Esc leaves visual mode");

        /* ---- ex commands ---- */
        page_at("foo one\nfoo two foo\nthree", 0, 0);
        feed(":%s/foo/bar/g\r");
        CHECK_PAGE("bar one\nbar two bar\nthree", ":%%s/foo/bar/g");
        CHECK(strstr(gtk_label_get_text(V.mode_label), "3 substitutions on 2 lines") != NULL,
              "substitute reports: %s", gtk_label_get_text(V.mode_label));
        feed(":s/bar/baz/\r");
        CHECK_PAGE("bar one\nbaz two bar\nthree", ":s on the current (last changed) line, first match only");
        feed(":%s/\\(t\\w\\+\\)/<\\1>/\r");
        CHECK_PAGE("bar one\nbaz <two> bar\n<three>", "vim-style groups and \\1 in :s");
        feed(":1\r");
        CHECK_POS(0, 0, ":1 goes to line 1");
        feed(":$\r");
        CHECK(cur_line() == 2, ":$ goes to the last line");
        feed(":set nonu\r");
        CHECK(!J.line_numbers && !gtk_widget_get_parent(J.gutter), ":set nonu hides line numbers");
        feed(":set nu\r");
        CHECK(J.line_numbers && gtk_widget_is_ancestor(J.gutter, GTK_WIDGET(J.view)), ":set nu shows them");
        feed(":w\r");
        CHECK(strstr(gtk_label_get_text(V.mode_label), "written") != NULL, ":w reports written: %s",
              gtk_label_get_text(V.mode_label));
        feed(":bogus\r");
        CHECK(strstr(gtk_label_get_text(V.mode_label), "E492") != NULL, "unknown command -> E492");
        feed(":2,3d\r");
        CHECK_PAGE("bar one", ":2,3d deletes lines");

        /* ---- keys ---- */
        page_at("abc", 0, 0);
        CHECK(vim_key(GDK_KEY_q, 0) && vim_key(GDK_KEY_z, 0), "normal mode swallows printable keys");
        g_string_truncate(V.pending, 0);
        CHECK_PAGE("abc", "...without inserting them");
        vim_key(GDK_KEY_Right, 0);
        CHECK_POS(0, 1, "arrow keys move");
        vim_key(GDK_KEY_i, 0);
        CHECK(V.mode == MODE_INSERT && g_str_equal(gtk_label_get_text(V.mode_label), "-- INSERT --"),
              "i -> insert mode, status says so");
        CHECK(!vim_key(GDK_KEY_q, 0), "insert mode lets keys through to the text view");
        CHECK(!gtk_text_view_get_overwrite(J.view), "insert mode: line cursor");
        vim_key(GDK_KEY_Escape, 0);
        CHECK(V.mode == MODE_NORMAL && gtk_text_view_get_overwrite(J.view), "Esc -> normal mode, block cursor");
        CHECK(!vim_key(GDK_KEY_c, GDK_CONTROL_MASK), "Ctrl+C is left to GTK (copy)");
        CHECK(g_str_equal(gtk_label_get_text(V.pos_label), "1,1"), "status shows line,col: %s",
              gtk_label_get_text(V.pos_label));
        feed("2d");
        CHECK(g_str_equal(gtk_label_get_text(V.showcmd_label), "2d"), "pending keys are shown");
        feed("\x1b");

        /* ---- images: the cursor selects them ---- */
        {
            g_autoptr(GdkTexture) itex = make_texture(60, 30);
            g_autofree char *rel = save_texture(itex);
            g_autofree char *pg = g_strdup_printf("above\n![](%s)\nbelow", rel);
            page_at(pg, 0, 0);
            JotImage *img = first_image();
            CHECK(img && !img->selected && gtk_text_view_get_cursor_visible(J.view), "image not selected from line 1");
            feed("j");
            CHECK(img && current_image() == img && img->selected, "j onto an image line selects (outlines) it");
            CHECK(!gtk_text_view_get_cursor_visible(J.view), "...and the outline replaces the text cursor");
            GAction *open = g_action_map_lookup_action(G_ACTION_MAP(J.win), "open-image");
            CHECK(g_action_get_enabled(open), "image actions work on the image under the cursor");
            feed("yy");
            GdkContentFormats *f = gdk_clipboard_get_formats(gtk_widget_get_clipboard(GTK_WIDGET(J.view)));
            CHECK(gdk_content_formats_contain_mime_type(f, "image/png"), "yy on an image line copies the image");
            feed("j");
            CHECK(!img->selected && gtk_text_view_get_cursor_visible(J.view) && !g_action_get_enabled(open),
                  "moving off the image deselects it");
            feed("ki");
            CHECK(!img->selected, "insert mode doesn't select the image");
            feed("\x1b");
            CHECK(img->selected, "back in normal mode on the image, it's selected again");
            feed("dd");
            CHECK_PAGE("above\nbelow", "dd deletes the image line");
        }

        /* ---- put (reads the clipboard asynchronously; checked next step) ---- */
        page_at("one\ntwo\nthree", 0, 0);
        feed("yyjp");
        break;
    }
    case 20: {
        CHECK_PAGE("one\ntwo\none\nthree", "yy j p puts the line below");
        CHECK_POS(2, 0, "p puts the cursor on the new line");
        page_at("abc", 0, 0);
        feed("ylp");
        break;
    }
    case 21: {
        CHECK_PAGE("aabc", "yl p puts after the cursor char");
        page_at("one two", 0, 0);
        feed("vey");
        CHECK(V.mode == MODE_NORMAL, "v e y leaves visual mode");
        feed("$p");
        break;
    }
    case 22: {
        CHECK_PAGE("one twoone", "visual yank + $p");
        /* p of an image (screenshot on the clipboard) goes on its own line below */
        page_at("above\nbelow", 0, 2);
        g_autoptr(GdkTexture) tex = make_texture(40, 20);
        g_autoptr(GBytes) png = gdk_texture_save_to_png_bytes(tex);
        set_clipboard_bytes("image/png", png);
        feed("p");
        break;
    }
    case 23: {
        if (!first_image() && waited++ < 20) {
            step_no--;
            break;
        }
        waited = 0;
        g_autofree char *txt = page_text();
        g_autoptr(GRegex) re = g_regex_new("^above\n!\\[\\]\\(assets/[^)]+\\)\nbelow$", 0, 0, NULL);
        CHECK(g_regex_match(re, txt, 0, NULL), "p of a screenshot puts it on its own line below");
        break;
    }
    case 24: {
        /* ---- Markdown styling ---- */
        CHECK(M.enabled, "markdown styling is on by default");
        page_at("# Title here\n"
                "**bold** and *it* and `co*de*` and _u_\n"
                "a*b*c snake_case_name **kwargs 2 * 3 * 4 __init__\n"
                "- item\n"
                "12. numbered\n"
                "- [x] done thing\n"
                "- [ ] open thing\n"
                "> quoted\n"
                "see [docs](https://example.com/a) or https://gnome.org.\n"
                "---",
                0, 0);
        CHECK(has_tag(0, 0, T_DIM) && !has_tag(0, 0, T_HEADING), "# marker dimmed");
        CHECK(has_tag(0, 2, T_HEADING) && has_tag(0, 11, T_HEADING), "heading text styled");
        CHECK(has_tag(1, 0, T_DIM) && has_tag(1, 2, T_BOLD) && !has_tag(1, 2, T_DIM) && has_tag(1, 7, T_DIM),
              "**bold**: bold with dim markers");
        CHECK(has_tag(1, 13, T_DIM) && has_tag(1, 14, T_ITALIC) && has_tag(1, 16, T_DIM), "*it*: italic");
        CHECK(has_tag(1, 22, T_CODE) && has_tag(1, 22, T_DIM) && has_tag(1, 24, T_CODE) && !has_tag(1, 25, T_ITALIC),
              "`code`: chip, dim backticks, nothing styled inside");
        CHECK(has_tag(1, 36, T_ITALIC), "_u_: italic");
        CHECK(plain(2, 2) && plain(2, 6) && plain(2, 12) && plain(2, 25) && plain(2, 27) && plain(2, 36) &&
                  plain(2, 38) && plain(2, 44),
              "conservative: a*b*c, snake_case_name, **kwargs, 2 * 3 * 4, __init__ stay plain");
        CHECK(has_tag(3, 0, T_BULLET) && plain(3, 2), "- bullet colored, item text plain");
        CHECK(has_tag(4, 0, T_BULLET) && has_tag(4, 2, T_BULLET) && plain(4, 4), "12. numbered marker colored");
        CHECK(has_tag(5, 2, T_BULLET) && has_tag(5, 7, T_DONE), "- [x] done: box colored, text struck through");
        CHECK(has_tag(6, 2, T_BULLET) && !has_tag(6, 7, T_DONE), "- [ ] open: not struck through");
        CHECK(has_tag(7, 0, T_QUOTE_MARK) && has_tag(7, 3, T_QUOTE), "> quote");
        CHECK(has_tag(8, 5, T_LINK) && has_tag(8, 4, T_DIM) && has_tag(8, 12, T_DIM) && !has_tag(8, 12, T_LINK),
              "[docs](url): text is the link, the rest dimmed");
        CHECK(has_tag(8, 37, T_LINK) && has_tag(8, 53, T_LINK) && !has_tag(8, 54, T_LINK),
              "bare URL is a link, trailing period excluded");
        CHECK(has_tag(9, 0, T_DIM), "--- rule dimmed");
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_line_offset(J.buf, &it, 8, 6);
        g_autofree char *u1 = md_url_at(&it);
        gtk_text_buffer_get_iter_at_line_offset(J.buf, &it, 8, 45);
        g_autofree char *u2 = md_url_at(&it);
        CHECK(u1 && g_str_equal(u1, "https://example.com/a") && u2 && g_str_equal(u2, "https://gnome.org"),
              "gx/Ctrl+click find the link's URL (%s, %s)", u1 ? u1 : "-", u2 ? u2 : "-");
        g_autofree char *saved = page_text();
        CHECK_PAGE(saved, "styling never changes the text");

        /* code blocks, and fences opening/closing as they're edited */
        page_at("before\n```\n# not a heading\n*not italic*\n```\n# after", 0, 0);
        CHECK(has_tag(1, 0, T_CODEBLOCK) && has_tag(1, 0, T_DIM), "fence line: code block, dimmed");
        CHECK(has_tag(2, 0, T_CODEBLOCK) && !has_tag(2, 2, T_HEADING) && !has_tag(3, 1, T_ITALIC),
              "nothing styled inside a code block");
        CHECK(!has_tag(5, 0, T_CODEBLOCK) && has_tag(5, 2, T_HEADING), "after the closing fence: styled again");
        feed("jdd");
        CHECK(has_tag(1, 2, T_HEADING) && !has_tag(1, 0, T_CODEBLOCK), "deleting the opening fence ends the block");
        CHECK(has_tag(3, 0, T_CODEBLOCK) && has_tag(4, 0, T_CODEBLOCK), "...and the old closing fence opens a new one");
        feed("O```\x1b");
        CHECK(has_tag(2, 0, T_CODEBLOCK) && !has_tag(2, 2, T_HEADING) && !has_tag(5, 0, T_CODEBLOCK) &&
                  has_tag(5, 2, T_HEADING),
              "typing the fence back restores the block");
        feed("o`x`\x1b");
        CHECK(has_tag(3, 1, T_CODEBLOCK) && !has_tag(3, 1, T_CODE), "inline code inside a block isn't a chip");

        /* ]] [[ jump between headings, skipping code blocks */
        page_at("intro\n# One\ntext\n```\n# fake\n```\n## Two\nend", 0, 0);
        feed("]]");
        CHECK(cur_line() == 1, "]] -> next heading (line %d)", cur_line() + 1);
        feed("]]");
        CHECK(cur_line() == 6, "]] skips # lines inside code (line %d)", cur_line() + 1);
        feed("[[");
        CHECK(cur_line() == 1, "[[ -> previous heading (line %d)", cur_line() + 1);
        feed("G[[");
        CHECK(cur_line() == 6, "[[ from the end");

        /* the switch */
        page_at("# Title", 0, 0);
        feed(":set nomd\r");
        CHECK(!M.enabled && plain(0, 0) && plain(0, 3), ":set nomd removes the styling");
        feed("A!\x1b");
        CHECK(plain(0, 3), "...and it stays off while editing");
        feed(":set md\r");
        CHECK(M.enabled && has_tag(0, 3, T_HEADING), ":set md brings it back");
        break;
    }
    case 25: {
        /* ---- page folders ---- */
        const char *env = g_getenv("JOT_DIR");
        g_autofree char *home = g_strdup(g_get_home_dir());
        g_autofree char *root = jot_root();
        g_autofree char *d0 = resolve_dir(NULL);
        g_autofree char *d1 = resolve_dir("work");
        g_autofree char *d2 = resolve_dir("~/x/y");
        g_autofree char *d3 = resolve_dir("./scratch");
        g_autofree char *cwd = g_get_current_dir();
        g_autofree char *want1 = g_build_filename(root, "work", NULL);
        g_autofree char *want2 = g_build_filename(home, "x", "y", NULL);
        g_autofree char *want3 = g_build_filename(cwd, "scratch", NULL);
        CHECK(g_str_equal(d0, env), "jot with no argument uses $JOT_DIR when set");
        CHECK(g_str_equal(d1, want1), "jot work -> %s", d1);
        CHECK(g_str_equal(d2, want2), "jot ~/x/y -> %s", d2);
        CHECK(g_str_equal(d3, want3), "jot ./scratch -> %s", d3);
        CHECK(g_str_has_suffix(root, "/jot"), "pages live under .../jot: %s", root);
        g_autofree char *id0 = app_id_for(d0);
        g_autofree char *id1 = app_id_for(d1);
        g_autofree char *id1b = app_id_for(want1);
        CHECK(g_str_equal(id0, APP_ID), "default page uses the plain app ID");
        CHECK(!g_str_equal(id1, id0) && g_str_equal(id1, id1b) && g_application_id_is_valid(id1),
              "other pages get their own stable, valid app ID (%s)", id1);

        g_autofree char *agents = g_build_filename(notes_dir, "AGENTS.md", NULL);
        g_autofree char *claude = g_build_filename(notes_dir, "CLAUDE.md", NULL);
        g_autofree char *link = g_file_read_link(claude, NULL);
        g_autofree char *ctext = NULL;
        g_file_get_contents(claude, &ctext, NULL, NULL);
        CHECK(g_file_test(agents, G_FILE_TEST_IS_REGULAR), "a new page gets AGENTS.md");
        CHECK(link && g_str_equal(link, "AGENTS.md") && ctext && g_str_equal(ctext, agents_md),
              "...and CLAUDE.md, a symlink to it");
        g_file_set_contents(agents, "my own notes for agents\n", -1, NULL);
        write_agent_files();
        g_autofree char *atext = NULL;
        g_file_get_contents(agents, &atext, NULL, NULL);
        CHECK(g_str_equal(atext, "my own notes for agents\n"), "existing agent files are never overwritten");
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
    init_paths(NULL);
    g_autoptr(AdwApplication) app = adw_application_new("dev.jot.JotTest", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_test_activate), NULL);
    g_application_run(G_APPLICATION(app), 0, NULL);
    return failures ? 1 : 0;
}
