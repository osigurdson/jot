/* jot - a tiny native (GTK4 / libadwaita) scratch page for programmers.
 *
 * One page of notes, stored as plain text in ~/notes/jot.md (or $JOT_DIR/jot.md).
 * Pasted images are saved to <notes>/assets/ and stored as ![](assets/<file>) lines; if
 * tesseract is installed their text is read in the background so search can find it.
 * Editing is vim-style (modal); see vim.c.
 * Everything is monospace, paste is always plain text, nothing touches the network.
 */
#include <adwaita.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>

#define APP_ID "dev.jot.Jot"
#define OBJ_UTF8 "\xef\xbf\xbc" /* U+FFFC: what GtkTextBuffer reports for an embedded image */
#define INDENT "    "
#define SAVE_DELAY_MS 400
#define DEFAULT_FONT_PT 11

static char *notes_dir, *assets_dir, *page_path, *state_path;
static GRegex *img_re;
static GHashTable *textures; /* absolute path -> GdkTexture */

/* ------------------------------------------------------------------------- */
/* Inline images                                                              */
/* ------------------------------------------------------------------------- */

/* A texture shown at its true on-screen size (HiDPI aware), shrunk to fit the view. */
#define JOT_TYPE_IMAGE (jot_image_get_type())
G_DECLARE_FINAL_TYPE(JotImage, jot_image, JOT, IMAGE, GObject)

struct _JotImage {
    GObject parent;
    GdkTexture *texture;
    char *path; /* "assets/x.png", relative to notes_dir */
    double scale;
    int max_w;
    gboolean selected; /* drawn with an accent outline (selection, find match) */
    GArray *hl;        /* graphene_rect_t in texture pixels: search matches in the image's text */
};

/* (scale, max width) from the most recent layout; new images start with these until refit. */
static double last_scale = 1.0;
static int last_max_w = 800;

static void jot_image_paintable_init(GdkPaintableInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(JotImage, jot_image, G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(GDK_TYPE_PAINTABLE, jot_image_paintable_init))

static void jot_image_size(JotImage *self, int *w, int *h) {
    double fw = gdk_texture_get_width(self->texture) / self->scale;
    double fh = gdk_texture_get_height(self->texture) / self->scale;
    if (fw > self->max_w) {
        fh = fh * self->max_w / fw;
        fw = self->max_w;
    }
    *w = MAX(1, (int)round(fw));
    *h = MAX(1, (int)round(fh));
}

static int jot_image_intrinsic_width(GdkPaintable *p) {
    int w, h;
    jot_image_size(JOT_IMAGE(p), &w, &h);
    return w;
}

static int jot_image_intrinsic_height(GdkPaintable *p) {
    int w, h;
    jot_image_size(JOT_IMAGE(p), &w, &h);
    return h;
}

/* Search matches inside the image: the rest of the image dims, and each match is a glowing
 * yellow highlighter mark over the original pixels (like a PDF viewer's find). */
static void draw_highlights(GtkSnapshot *s, JotImage *img, double w, double h) {
    const GdkRGBA dim = {0, 0, 0, 0.30f};
    const GdkRGBA fill = {0.965f, 0.827f, 0.176f, 0.50f}; /* Adwaita yellow 3 */
    const GdkRGBA ring = {0.898f, 0.647f, 0.039f, 0.95f}; /* yellow 5 */
    const GdkRGBA glow = {0.965f, 0.827f, 0.176f, 0.60f};
    double sx = w / gdk_texture_get_width(img->texture), sy = h / gdk_texture_get_height(img->texture);
    graphene_rect_t full = GRAPHENE_RECT_INIT(0, 0, w, h);
    gtk_snapshot_append_color(s, &dim, &full);
    for (guint i = 0; i < img->hl->len; i++) {
        const graphene_rect_t *t = &g_array_index(img->hl, graphene_rect_t, i);
        const double pad = 3;
        graphene_rect_t r = GRAPHENE_RECT_INIT(t->origin.x * sx - pad, t->origin.y * sy - pad,
                                               t->size.width * sx + 2 * pad, t->size.height * sy + 2 * pad);
        if (!graphene_rect_intersection(&r, &full, &r))
            continue;
        GskRoundedRect rr;
        gsk_rounded_rect_init_from_rect(&rr, &r, 4);
        gtk_snapshot_append_outset_shadow(s, &rr, &glow, 0, 0, 1, 8);
        gtk_snapshot_push_rounded_clip(s, &rr);
        gtk_snapshot_append_scaled_texture(s, img->texture, GSK_SCALING_FILTER_TRILINEAR, &full);
        gtk_snapshot_append_color(s, &fill, &r);
        gtk_snapshot_pop(s);
        gtk_snapshot_append_inset_shadow(s, &rr, &ring, 0, 0, 1.5f, 0);
    }
}

static void jot_image_snapshot(GdkPaintable *p, GdkSnapshot *gs, double w, double h) {
    GtkSnapshot *s = GTK_SNAPSHOT(gs);
    graphene_rect_t r = GRAPHENE_RECT_INIT(0, 0, w, h);
    gtk_snapshot_append_scaled_texture(s, JOT_IMAGE(p)->texture, GSK_SCALING_FILTER_TRILINEAR, &r);
    gboolean highlighted = JOT_IMAGE(p)->hl && JOT_IMAGE(p)->hl->len;
    if (highlighted)
        draw_highlights(s, JOT_IMAGE(p), w, h);
    const GdkRGBA grey = {0.5f, 0.5f, 0.5f, 0.35f};
    const graphene_rect_t edges[] = {
        GRAPHENE_RECT_INIT(0, 0, w, 1), GRAPHENE_RECT_INIT(0, h - 1, w, 1),
        GRAPHENE_RECT_INIT(0, 0, 1, h), GRAPHENE_RECT_INIT(w - 1, 0, 1, h),
    };
    for (guint i = 0; i < G_N_ELEMENTS(edges); i++)
        gtk_snapshot_append_color(s, &grey, &edges[i]);
    if (JOT_IMAGE(p)->selected) {
        GdkRGBA *accent = adw_style_manager_get_accent_color_rgba(adw_style_manager_get_default());
        const graphene_rect_t thick[] = {
            GRAPHENE_RECT_INIT(0, 0, w, 3), GRAPHENE_RECT_INIT(0, h - 3, w, 3),
            GRAPHENE_RECT_INIT(0, 0, 3, h), GRAPHENE_RECT_INIT(w - 3, 0, 3, h),
        };
        for (guint i = 0; i < G_N_ELEMENTS(thick); i++)
            gtk_snapshot_append_color(s, accent, &thick[i]);
        gdk_rgba_free(accent);
    }
}

static void jot_image_paintable_init(GdkPaintableInterface *iface) {
    iface->snapshot = jot_image_snapshot;
    iface->get_intrinsic_width = jot_image_intrinsic_width;
    iface->get_intrinsic_height = jot_image_intrinsic_height;
}

static void jot_image_finalize(GObject *o) {
    JotImage *self = JOT_IMAGE(o);
    g_clear_object(&self->texture);
    g_clear_pointer(&self->hl, g_array_unref);
    g_free(self->path);
    G_OBJECT_CLASS(jot_image_parent_class)->finalize(o);
}

static void jot_image_class_init(JotImageClass *klass) {
    G_OBJECT_CLASS(klass)->finalize = jot_image_finalize;
}

static void jot_image_init(JotImage *self) {
    self->scale = last_scale;
    self->max_w = last_max_w;
}

static JotImage *jot_image_new(GdkTexture *tex, const char *rel) {
    JotImage *self = g_object_new(JOT_TYPE_IMAGE, NULL);
    self->texture = g_object_ref(tex);
    self->path = g_strdup(rel);
    return self;
}

static void jot_image_fit(JotImage *self, double scale, int max_w) {
    if (scale == self->scale && max_w == self->max_w)
        return;
    self->scale = scale;
    self->max_w = max_w;
    gdk_paintable_invalidate_size(GDK_PAINTABLE(self));
}

/* rel is "assets/x.png". Returns the (cached, borrowed) texture, or NULL if missing/bad. */
static GdkTexture *load_texture(const char *rel) {
    const char *name = rel + strlen("assets/");
    if (!g_str_has_prefix(rel, "assets/") || !*name || strchr(name, '/') ||
        g_str_equal(name, ".") || g_str_equal(name, ".."))
        return NULL;
    g_autofree char *abs = g_build_filename(assets_dir, name, NULL);
    GdkTexture *tex = g_hash_table_lookup(textures, abs);
    if (!tex) {
        if (!g_file_test(abs, G_FILE_TEST_IS_REGULAR))
            return NULL;
        tex = gdk_texture_new_from_filename(abs, NULL);
        if (!tex)
            return NULL;
        g_hash_table_insert(textures, g_strdup(abs), tex);
    }
    return tex;
}

/* Returns a new JotImage for rel, or NULL if the file is missing/bad. */
static JotImage *load_image(const char *rel) {
    GdkTexture *tex = load_texture(rel);
    return tex ? jot_image_new(tex, rel) : NULL;
}

static char *asset_name(const char *ext) {
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree char *stamp = g_date_time_format(now, "%Y%m%d-%H%M%S");
    return g_strdup_printf("%s-%06x%s", stamp, g_random_int() & 0xffffff, ext);
}

/* Saves a texture into assets/ as PNG. Returns "assets/x.png" or NULL. */
static char *save_texture(GdkTexture *tex) {
    g_autofree char *name = asset_name(".png");
    char *abs = g_build_filename(assets_dir, name, NULL);
    if (!gdk_texture_save_to_png(tex, abs)) {
        g_free(abs);
        return NULL;
    }
    g_hash_table_insert(textures, abs, g_object_ref(tex));
    return g_strconcat("assets/", name, NULL);
}

/* Copies an image file into assets/. Returns "assets/x.ext" or NULL. */
static char *import_image_file(const char *path) {
    static const char *exts[] = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp"};
    const char *dot = strrchr(path, '.');
    if (!dot)
        return NULL;
    g_autofree char *ext = g_ascii_strdown(dot, -1);
    gboolean ok = FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(exts); i++)
        ok |= g_str_equal(ext, exts[i]);
    if (!ok)
        return NULL;
    g_autofree char *name = asset_name(g_str_equal(ext, ".jpeg") ? ".jpg" : ext);
    g_autofree char *dest = g_build_filename(assets_dir, name, NULL);
    g_autoptr(GFile) src = g_file_new_for_path(path);
    g_autoptr(GFile) dst = g_file_new_for_path(dest);
    if (!g_file_copy(src, dst, G_FILE_COPY_NONE, NULL, NULL, NULL, NULL))
        return NULL;
    return g_strconcat("assets/", name, NULL);
}

/* If line is exactly an image reference, returns the "assets/..." path. */
static char *match_img_line(const char *line) {
    g_autofree char *s = g_strstrip(g_strdup(line));
    g_autoptr(GMatchInfo) m = NULL;
    return g_regex_match(img_re, s, 0, &m) ? g_match_info_fetch(m, 1) : NULL;
}

/* ------------------------------------------------------------------------- */
/* Buffer <-> text                                                            */
/* ------------------------------------------------------------------------- */

static char *serialize_range(GtkTextBuffer *buf, const GtkTextIter *start, const GtkTextIter *end) {
    g_autofree char *text = gtk_text_buffer_get_slice(buf, start, end, TRUE);
    GString *out = g_string_new(NULL);
    int offset = gtk_text_iter_get_offset(start);
    gboolean after_img = FALSE;
    const char *p = text;
    for (;;) {
        const char *obj = strstr(p, OBJ_UTF8);
        gsize len = obj ? (gsize)(obj - p) : strlen(p);
        if (after_img && len > 0 && p[0] != '\n')
            g_string_append_c(out, '\n');
        g_string_append_len(out, p, len);
        offset += g_utf8_strlen(p, len);
        if (!obj)
            break;
        GtkTextIter oi;
        gtk_text_buffer_get_iter_at_offset(buf, &oi, offset);
        GdkPaintable *pt = gtk_text_iter_get_paintable(&oi);
        after_img = FALSE;
        if (JOT_IS_IMAGE(pt)) {
            if (out->len && out->str[out->len - 1] != '\n')
                g_string_append_c(out, '\n');
            g_string_append_printf(out, "![](%s)", JOT_IMAGE(pt)->path);
            after_img = TRUE;
        }
        offset++;
        p = obj + strlen(OBJ_UTF8);
    }
    return g_string_free(out, FALSE);
}

static char *serialize(GtkTextBuffer *buf) {
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(buf, &s, &e);
    return serialize_range(buf, &s, &e);
}

/* Inserts plain text at it; ![](assets/..) lines whose file exists become images. */
static void insert_text(GtkTextBuffer *buf, GtkTextIter *it, const char *raw) {
    g_autoptr(GString) t = g_string_new(raw);
    g_string_replace(t, "\r\n", "\n", 0);
    g_string_replace(t, OBJ_UTF8, "", 0);
    if (!strstr(t->str, "![")) {
        gtk_text_buffer_insert(buf, it, t->str, (int)t->len);
        return;
    }
    g_auto(GStrv) lines = g_strsplit(t->str, "\n", -1);
    g_autoptr(GString) pending = g_string_new(NULL);
    for (int i = 0; lines[i]; i++) {
        if (i)
            g_string_append_c(pending, '\n');
        g_autofree char *rel = match_img_line(lines[i]);
        JotImage *img = rel ? load_image(rel) : NULL;
        if (!img) {
            g_string_append(pending, lines[i]);
            continue;
        }
        gtk_text_buffer_insert(buf, it, pending->str, (int)pending->len);
        g_string_truncate(pending, 0);
        gtk_text_buffer_insert_paintable(buf, it, GDK_PAINTABLE(img));
        g_object_unref(img);
    }
    gtk_text_buffer_insert(buf, it, pending->str, (int)pending->len);
}

/* ------------------------------------------------------------------------- */
/* The page on disk + window state (single window app, so this is a global)   */
/* ------------------------------------------------------------------------- */

static struct {
    GtkApplication *app;
    GtkWindow *win;
    GtkTextView *view;
    GtkTextBuffer *buf;
    AdwToastOverlay *toasts;
    GtkCssProvider *css;
    GtkWidget *gutter;
    gboolean line_numbers;
    GKeyFile *state;
    char *last_saved;
    gint64 mtime;
    gboolean loading;
    guint save_src;
    int font_size;
} J;

/* Modification time in microseconds, or -1 if the file doesn't exist. */
static gint64 file_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    return (gint64)st.st_mtim.tv_sec * G_USEC_PER_SEC + st.st_mtim.tv_nsec / 1000;
}

/* The page's text (one trailing newline dropped), or "" if there's no file yet. */
static char *read_page(void) {
    char *text = NULL;
    gsize len;
    if (!g_file_get_contents(page_path, &text, &len, NULL))
        return g_strdup("");
    if (len && text[len - 1] == '\n')
        text[len - 1] = '\0';
    return text;
}

static void toast(const char *msg) {
    AdwToast *t = adw_toast_new(msg);
    adw_toast_set_timeout(t, 4);
    adw_toast_overlay_add_toast(J.toasts, t);
}

static void cursor_iter(GtkTextIter *it) {
    gtk_text_buffer_get_iter_at_mark(J.buf, it, gtk_text_buffer_get_insert(J.buf));
}

static gboolean is_obj_char(gunichar ch, gpointer unused) {
    (void)unused;
    return ch == 0xFFFC;
}

static gboolean fit_images(gpointer unused) {
    (void)unused;
    GtkWidget *v = GTK_WIDGET(J.view);
    GdkRectangle vis; /* the text area: excludes the gutter, includes the margins */
    gtk_text_view_get_visible_rect(J.view, &vis);
    if (vis.width <= 0)
        return G_SOURCE_REMOVE;
    int max_w = MAX(50, vis.width - gtk_text_view_get_left_margin(J.view) - gtk_text_view_get_right_margin(J.view) - 4);
    GtkNative *native = gtk_widget_get_native(v);
    GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;
    double scale = surface ? gdk_surface_get_scale(surface) : gtk_widget_get_scale_factor(v);
    last_scale = scale;
    last_max_w = max_w;

    GtkTextIter it;
    gtk_text_buffer_get_start_iter(J.buf, &it);
    do {
        GdkPaintable *p = gtk_text_iter_get_paintable(&it);
        if (JOT_IS_IMAGE(p))
            jot_image_fit(JOT_IMAGE(p), scale, max_w);
    } while (gtk_text_iter_forward_find_char(&it, is_obj_char, NULL, NULL));
    return G_SOURCE_REMOVE;
}

static void set_text(const char *text) {
    GtkTextIter it;
    J.loading = TRUE;
    gtk_text_buffer_begin_irreversible_action(J.buf);
    gtk_text_buffer_set_text(J.buf, "", 0);
    gtk_text_buffer_get_start_iter(J.buf, &it);
    insert_text(J.buf, &it, text);
    gtk_text_buffer_end_irreversible_action(J.buf);
    J.loading = FALSE;
    g_free(J.last_saved);
    J.last_saved = serialize(J.buf);
    fit_images(NULL);
}

static void save(void) {
    g_clear_handle_id(&J.save_src, g_source_remove);
    g_autofree char *text = serialize(J.buf);
    if (g_str_equal(text, J.last_saved))
        return;
    g_autofree char *contents = *text ? g_strconcat(text, "\n", NULL) : g_strdup("");
    g_autoptr(GError) err = NULL;
    if (!g_file_set_contents(page_path, contents, -1, &err)) {
        g_autofree char *msg = g_strdup_printf("Save failed: %s", err->message);
        toast(msg);
        return;
    }
    J.mtime = file_mtime(page_path);
    g_free(J.last_saved);
    J.last_saved = g_steal_pointer(&text);
}

static gboolean save_timeout(gpointer unused) {
    (void)unused;
    J.save_src = 0;
    save();
    return G_SOURCE_REMOVE;
}

static void on_changed(GtkTextBuffer *buf, gpointer unused) {
    (void)buf, (void)unused;
    if (J.loading)
        return;
    g_clear_handle_id(&J.save_src, g_source_remove);
    J.save_src = g_timeout_add(SAVE_DELAY_MS, save_timeout, NULL);
}

static void ocr_request_all(void);

/* Loads the page and puts the cursor at the end, ready for the next note. */
static void load_page(void) {
    g_autofree char *text = read_page();
    J.mtime = file_mtime(page_path);
    set_text(text);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(J.buf, &end);
    gtk_text_buffer_place_cursor(J.buf, &end);
    gtk_text_view_scroll_to_mark(J.view, gtk_text_buffer_get_insert(J.buf), 0, FALSE, 0, 0);
    ocr_request_all();
}

/* Pick up edits made outside jot (vim, sed, git checkout, ...); save when focus leaves. */
static void on_active(GtkWindow *win, GParamSpec *ps, gpointer unused) {
    (void)ps, (void)unused;
    if (!gtk_window_is_active(win)) {
        save();
        return;
    }
    gint64 mtime = file_mtime(page_path);
    g_autofree char *current = serialize(J.buf);
    if (mtime < 0 || mtime == J.mtime || !g_str_equal(current, J.last_saved))
        return;
    GtkTextIter it;
    cursor_iter(&it);
    int offset = gtk_text_iter_get_offset(&it);
    g_autofree char *text = read_page();
    J.mtime = mtime;
    set_text(text);
    gtk_text_buffer_get_iter_at_offset(J.buf, &it, offset);
    gtk_text_buffer_place_cursor(J.buf, &it);
    ocr_request_all();
}

static void save_state(void) {
    GKeyFile *k = J.state;
    int w, h;
    gtk_window_get_default_size(J.win, &w, &h);
    g_key_file_set_integer(k, "window", "width", w);
    g_key_file_set_integer(k, "window", "height", h);
    g_key_file_set_boolean(k, "window", "maximized", gtk_window_is_maximized(J.win));
    g_key_file_set_integer(k, "window", "font_size", J.font_size);
    g_key_file_set_boolean(k, "window", "line_numbers", J.line_numbers);
    g_autofree char *dir = g_path_get_dirname(state_path);
    g_mkdir_with_parents(dir, 0755);
    g_key_file_save_to_file(k, state_path, NULL);
}

static gboolean on_close(GtkWindow *win, gpointer unused) {
    (void)win, (void)unused;
    save();
    save_state();
    return FALSE;
}

/* ------------------------------------------------------------------------- */
/* OCR: Tesseract in the background, one image at a time                      */
/* ------------------------------------------------------------------------- */

/* The text of assets/x.png is cached next to it as assets/x.png.txt, so each image is read once
 * (and grep finds text inside screenshots), with each word's position in assets/x.png.tsv (for
 * highlighting search matches). Without tesseract the feature quietly disappears. */
static char *tesseract_path;
static GHashTable *ocr_text;    /* rel -> recognized text ("" = none) */
static GHashTable *ocr_pending; /* rels queued or running */
static GHashTable *ocr_failed;  /* rels tesseract couldn't read this session (not retried) */
static GHashTable *ocr_words;   /* rel -> GPtrArray of OcrWord, parsed from the .tsv */
static GQueue ocr_queue = G_QUEUE_INIT;
static gboolean ocr_running;
static char *ocr_copy_wanted; /* rel whose text goes to the clipboard once it's ready */

static char *ocr_sidecar_ext(const char *rel, const char *ext) {
    g_autofree char *abs = g_build_filename(notes_dir, rel, NULL);
    return g_strconcat(abs, ext, NULL);
}

static char *ocr_sidecar(const char *rel) {
    return ocr_sidecar_ext(rel, ".txt");
}

/* The recognized text for rel, or NULL if it hasn't been read (yet). */
static const char *ocr_lookup(const char *rel) {
    const char *t = g_hash_table_lookup(ocr_text, rel);
    if (t)
        return t;
    g_autofree char *side = ocr_sidecar(rel);
    char *text;
    if (!g_file_get_contents(side, &text, NULL, NULL))
        return NULL;
    g_hash_table_insert(ocr_text, g_strdup(rel), text);
    return text;
}

/* Tesseract does best on dark text on a light background with ~20px+ glyphs: grayscale,
 * invert dark themes, and upscale 2x (unless already huge). Output is a binary PGM. */
static GBytes *ocr_prepare(GdkTexture *tex) {
    int w = gdk_texture_get_width(tex), h = gdk_texture_get_height(tex);
    g_autoptr(GdkTextureDownloader) d = gdk_texture_downloader_new(tex);
    gdk_texture_downloader_set_format(d, GDK_MEMORY_R8G8B8A8);
    gsize stride;
    g_autoptr(GBytes) px = gdk_texture_downloader_download_bytes(d, &stride);
    const guchar *src = g_bytes_get_data(px, NULL);

    g_autofree guchar *gray = g_malloc((gsize)w * h);
    guint64 sum = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const guchar *p = src + y * stride + x * 4;
            double a = p[3] / 255.0;
            double lum = 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
            guchar v = (guchar)(lum * a + 255 * (1 - a) + 0.5); /* transparent -> white */
            gray[(gsize)y * w + x] = v;
            sum += v;
        }
    }
    gboolean dark = sum < (guint64)w * h * 128;

    int k = (w * 2 <= 5000 && h * 2 <= 5000) ? 2 : 1;
    int ow = w * k, oh = h * k;
    g_autofree char *header = g_strdup_printf("P5\n%d %d\n255\n", ow, oh);
    gsize hlen = strlen(header);
    guchar *out = g_malloc(hlen + (gsize)ow * oh);
    memcpy(out, header, hlen);
    guchar *dst = out + hlen;
    for (int y = 0; y < oh; y++) {
        double sy = CLAMP((y + 0.5) / k - 0.5, 0, h - 1);
        int y0 = (int)sy, y1 = MIN(y0 + 1, h - 1);
        double fy = sy - y0;
        for (int x = 0; x < ow; x++) {
            double sx = CLAMP((x + 0.5) / k - 0.5, 0, w - 1);
            int x0 = (int)sx, x1 = MIN(x0 + 1, w - 1);
            double fx = sx - x0;
            double v = (gray[(gsize)y0 * w + x0] * (1 - fx) + gray[(gsize)y0 * w + x1] * fx) * (1 - fy) +
                       (gray[(gsize)y1 * w + x0] * (1 - fx) + gray[(gsize)y1 * w + x1] * fx) * fy;
            guchar g = (guchar)(v + 0.5);
            dst[(gsize)y * ow + x] = dark ? 255 - g : g;
        }
    }
    return g_bytes_new_take(out, hlen + (gsize)ow * oh);
}

/* Trims trailing spaces, the trailing form feed, and runs of blank lines. */
static char *ocr_clean(const char *raw, gsize len) {
    g_autofree char *text = g_strndup(raw, len);
    g_strdelimit(text, "\f", '\n');
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    GString *out = g_string_new(NULL);
    int blanks = 0;
    for (int i = 0; lines[i]; i++) {
        g_strchomp(lines[i]);
        if (!*lines[i]) {
            blanks++;
            continue;
        }
        if (out->len)
            g_string_append(out, blanks ? "\n\n" : "\n");
        g_string_append(out, lines[i]);
        blanks = 0;
    }
    return g_string_free(out, FALSE);
}

static void copy_image_text(const char *rel);
static void ocr_pump(void);

static void ocr_done(GObject *src, GAsyncResult *res, gpointer data) {
    char *rel = data;
    g_autoptr(GError) err = NULL;
    GSubprocess *proc = G_SUBPROCESS(src);
    g_autofree char *side = ocr_sidecar(rel);
    char *raw = NULL;
    gsize len = 0;
    if (g_subprocess_communicate_finish(proc, res, NULL, NULL, &err) && g_subprocess_get_successful(proc) &&
        g_file_get_contents(side, &raw, &len, NULL)) {
        char *text = ocr_clean(raw, len); /* tesseract wrote x.png.txt and x.png.tsv; tidy the .txt */
        g_file_set_contents(side, text, -1, NULL);
        g_hash_table_insert(ocr_text, g_strdup(rel), text);
        g_hash_table_remove(ocr_words, rel);
    } else {
        /* Don't retry this session; no sidecar, so it's tried again next launch. */
        g_hash_table_insert(ocr_text, g_strdup(rel), g_strdup(""));
        g_hash_table_add(ocr_failed, g_strdup(rel));
    }
    g_free(raw);
    g_hash_table_remove(ocr_pending, rel);
    if (ocr_copy_wanted && g_str_equal(ocr_copy_wanted, rel)) {
        g_clear_pointer(&ocr_copy_wanted, g_free);
        copy_image_text(rel);
    }
    g_free(rel);
    ocr_running = FALSE;
    ocr_pump();
}

static void ocr_pump(void) {
    while (!ocr_running && !g_queue_is_empty(&ocr_queue)) {
        g_autofree char *rel = g_queue_pop_head(&ocr_queue);
        GdkTexture *tex = load_texture(rel);
        g_autoptr(GError) err = NULL;
        g_autofree char *outbase = g_build_filename(notes_dir, rel, NULL); /* -> x.png.txt, x.png.tsv */
        g_autoptr(GSubprocess) proc =
            tex ? g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                       G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                   &err, tesseract_path, "stdin", outbase, "-l", "eng", "-c",
                                   "preserve_interword_spaces=1", "txt", "tsv", NULL)
                : NULL;
        if (!proc) {
            g_hash_table_insert(ocr_text, g_strdup(rel), g_strdup(""));
            g_hash_table_add(ocr_failed, g_strdup(rel));
            g_hash_table_remove(ocr_pending, rel);
            continue;
        }
        g_autoptr(GBytes) pgm = ocr_prepare(tex);
        ocr_running = TRUE;
        g_subprocess_communicate_async(proc, pgm, NULL, ocr_done, g_steal_pointer(&rel));
    }
}

static void ocr_request(const char *rel) {
    if (!tesseract_path || g_hash_table_contains(ocr_pending, rel) || g_hash_table_contains(ocr_failed, rel))
        return;
    g_autofree char *tsv = ocr_sidecar_ext(rel, ".tsv");
    if (ocr_lookup(rel) && g_file_test(tsv, G_FILE_TEST_EXISTS))
        return; /* already read (older versions didn't keep word positions; those are re-read) */
    g_hash_table_add(ocr_pending, g_strdup(rel));
    g_queue_push_tail(&ocr_queue, g_strdup(rel));
    ocr_pump();
}

/* Queues every image on the page that hasn't been read yet. */
static void ocr_request_all(void) {
    GtkTextIter it;
    gtk_text_buffer_get_start_iter(J.buf, &it);
    do {
        GdkPaintable *p = gtk_text_iter_get_paintable(&it);
        if (JOT_IS_IMAGE(p))
            ocr_request(JOT_IMAGE(p)->path);
    } while (gtk_text_iter_forward_find_char(&it, is_obj_char, NULL, NULL));
}

static void copy_image_text(const char *rel) {
    const char *text = ocr_lookup(rel);
    if (!text) {
        if (!tesseract_path) {
            toast("Install tesseract to read text from images");
            return;
        }
        g_free(ocr_copy_wanted);
        ocr_copy_wanted = g_strdup(rel);
        toast("Reading text from image…");
        ocr_request(rel);
        return;
    }
    if (!*text) {
        toast("No text found in image");
        return;
    }
    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(J.view)), text);
    toast("Copied text from image");
}

typedef struct {
    int line;         /* tesseract's text line, numbered through the image */
    double x, y, w, h; /* in texture pixels */
    char *text;
} OcrWord;

static void ocr_word_free(OcrWord *w) {
    g_free(w->text);
    g_free(w);
}

/* The image's words with their positions, parsed from tesseract's TSV (cached). */
static GPtrArray *ocr_words_for(JotImage *img) {
    GPtrArray *words = g_hash_table_lookup(ocr_words, img->path);
    if (words)
        return words;
    g_autofree char *path = ocr_sidecar_ext(img->path, ".tsv");
    g_autofree char *data = NULL;
    if (!g_file_get_contents(path, &data, NULL, NULL))
        return NULL;
    words = g_ptr_array_new_with_free_func((GDestroyNotify)ocr_word_free);
    double scale = 1; /* tesseract saw the preprocessed (upscaled) image */
    int line = -1;
    g_auto(GStrv) rows = g_strsplit(data, "\n", -1);
    for (int i = 0; rows[i]; i++) {
        /* level page block par line word left top width height conf text */
        g_auto(GStrv) f = g_strsplit(rows[i], "\t", 12);
        if (g_strv_length(f) < 12)
            continue;
        int level = atoi(f[0]);
        if (level == 1 && g_ascii_strtod(f[8], NULL) > 0) {
            scale = gdk_texture_get_width(img->texture) / g_ascii_strtod(f[8], NULL);
        } else if (level == 4) {
            line++;
        } else if (level == 5 && *g_strstrip(f[11])) {
            OcrWord *w = g_new0(OcrWord, 1);
            w->line = line;
            w->x = g_ascii_strtod(f[6], NULL) * scale;
            w->y = g_ascii_strtod(f[7], NULL) * scale;
            w->w = g_ascii_strtod(f[8], NULL) * scale;
            w->h = g_ascii_strtod(f[9], NULL) * scale;
            w->text = g_strdup(f[11]);
            g_ptr_array_add(words, w);
        }
    }
    g_hash_table_insert(ocr_words, g_strdup(img->path), words);
    return words;
}

/* Rects (texture pixels) over each occurrence of qfold (casefolded) in the image's text. A match
 * within a word covers that part of it; a phrase across words on a line becomes one rect. */
static GArray *ocr_highlights(JotImage *img, const char *qfold) {
    GArray *out = g_array_new(FALSE, FALSE, sizeof(graphene_rect_t));
    GPtrArray *words = ocr_words_for(img);
    if (!words || !*qfold)
        return out;
    for (guint i = 0; i < words->len;) {
        int line = ((OcrWord *)words->pdata[i])->line;
        guint j = i;
        while (j < words->len && ((OcrWord *)words->pdata[j])->line == line)
            j++;
        /* the line as casefolded words joined by spaces, remembering where each word sits */
        GString *l = g_string_new(NULL);
        g_autofree int *ws = g_new(int, j - i);
        g_autofree int *we = g_new(int, j - i);
        for (guint k = i; k < j; k++) {
            if (k > i)
                g_string_append_c(l, ' ');
            g_autofree char *fold = g_utf8_casefold(((OcrWord *)words->pdata[k])->text, -1);
            ws[k - i] = (int)l->len;
            g_string_append(l, fold);
            we[k - i] = (int)l->len;
        }
        for (const char *m = strstr(l->str, qfold); m; m = strstr(m + 1, qfold)) {
            int ms = (int)(m - l->str), me = ms + (int)strlen(qfold);
            graphene_rect_t u;
            gboolean have = FALSE;
            for (guint k = i; k < j; k++) {
                int a = MAX(ms, ws[k - i]), b = MIN(me, we[k - i]);
                if (a >= b)
                    continue;
                const OcrWord *w = words->pdata[k];
                const char *wp = l->str + ws[k - i];
                double len = MAX(1, g_utf8_strlen(wp, we[k - i] - ws[k - i]));
                double fa = g_utf8_strlen(wp, a - ws[k - i]) / len, fb = g_utf8_strlen(wp, b - ws[k - i]) / len;
                graphene_rect_t r = GRAPHENE_RECT_INIT(w->x + w->w * fa, w->y, w->w * (fb - fa), w->h);
                if (have)
                    graphene_rect_union(&u, &r, &u);
                else
                    u = r;
                have = TRUE;
            }
            if (have)
                g_array_append_val(out, u);
        }
        g_string_free(l, TRUE);
        i = j;
    }
    return out;
}

/* Highlights where q occurs in the image's text (NULL clears). */
static void image_set_highlight(JotImage *img, const char *q) {
    g_clear_pointer(&img->hl, g_array_unref);
    if (q && *q) {
        g_autofree char *fold = g_utf8_casefold(q, -1);
        img->hl = ocr_highlights(img, fold);
        if (!img->hl->len)
            g_clear_pointer(&img->hl, g_array_unref);
    }
    gdk_paintable_invalidate_contents(GDK_PAINTABLE(img));
}

/* The image, if the selection is exactly one image. */
static JotImage *selected_image(void) {
    GtkTextIter s, e;
    if (!gtk_text_buffer_get_selection_bounds(J.buf, &s, &e) ||
        gtk_text_iter_get_offset(&e) - gtk_text_iter_get_offset(&s) != 1)
        return NULL;
    GdkPaintable *p = gtk_text_iter_get_paintable(&s);
    return JOT_IS_IMAGE(p) ? JOT_IMAGE(p) : NULL;
}

static gboolean vim_image_cursor_mode(void); /* vim.c */

/* The image being acted on: one selected by itself, or (outside insert mode) the one the cursor
 * is on, which is how you "select" an image with the keyboard or a click. */
static JotImage *current_image(void) {
    JotImage *img = selected_image();
    if (img || gtk_text_buffer_get_has_selection(J.buf) || !vim_image_cursor_mode())
        return img;
    GtkTextIter it;
    cursor_iter(&it);
    GdkPaintable *p = gtk_text_iter_get_paintable(&it);
    return JOT_IS_IMAGE(p) ? JOT_IMAGE(p) : NULL;
}

/* Outlines the current image and enables the image actions (context menu, Ctrl+Shift+C). The
 * text cursor is hidden while it's on an image: the outline is the cursor. */
static void update_image_selection(void) {
    static JotImage *outlined;
    JotImage *img = current_image();
    if (img != outlined) {
        if (outlined) {
            outlined->selected = FALSE;
            image_set_highlight(outlined, NULL); /* search highlights go when the cursor moves on */
        }
        g_set_object(&outlined, img);
        if (img) {
            img->selected = TRUE;
            gdk_paintable_invalidate_contents(GDK_PAINTABLE(img));
        }
    }
    gtk_text_view_set_cursor_visible(J.view, !img || selected_image() == img);
    if (!J.win)
        return;
    GAction *copy = g_action_map_lookup_action(G_ACTION_MAP(J.win), "copy-image-text");
    GAction *open = g_action_map_lookup_action(G_ACTION_MAP(J.win), "open-image");
    if (copy)
        g_simple_action_set_enabled(G_SIMPLE_ACTION(copy), img && tesseract_path);
    if (open)
        g_simple_action_set_enabled(G_SIMPLE_ACTION(open), img != NULL);
}

static void on_selection_mark_set(GtkTextBuffer *buf, GtkTextIter *it, GtkTextMark *mark, gpointer unused) {
    (void)it, (void)unused;
    if (mark == gtk_text_buffer_get_insert(buf) || mark == gtk_text_buffer_get_selection_bound(buf))
        update_image_selection();
}

/* ------------------------------------------------------------------------- */
/* Images in the buffer                                                       */
/* ------------------------------------------------------------------------- */

static void after_insert(void) {
    fit_images(NULL);
    gtk_text_view_scroll_mark_onscreen(J.view, gtk_text_buffer_get_insert(J.buf));
}

/* Inserts images, each on its own line, at `at` (or replacing the selection when NULL). */
static void insert_images(GPtrArray *rels, const GtkTextIter *at) {
    GtkTextIter it;
    gtk_text_buffer_begin_user_action(J.buf);
    if (at) {
        it = *at;
    } else {
        gtk_text_buffer_delete_selection(J.buf, TRUE, TRUE);
        cursor_iter(&it);
    }
    for (guint i = 0; i < rels->len; i++) {
        JotImage *img = load_image(rels->pdata[i]);
        if (!img)
            continue;
        if (!gtk_text_iter_starts_line(&it))
            gtk_text_buffer_insert(J.buf, &it, "\n", 1);
        gtk_text_buffer_insert_paintable(J.buf, &it, GDK_PAINTABLE(img));
        ocr_request(img->path);
        g_object_unref(img);
        if (gtk_text_iter_is_end(&it) || gtk_text_iter_get_char(&it) != '\n')
            gtk_text_buffer_insert(J.buf, &it, "\n", 1);
        else
            gtk_text_iter_forward_char(&it);
    }
    gtk_text_buffer_place_cursor(J.buf, &it);
    gtk_text_buffer_end_user_action(J.buf);
    after_insert();
}

static void insert_files(GSList *files, const GtkTextIter *at) {
    g_autoptr(GPtrArray) rels = g_ptr_array_new_with_free_func(g_free);
    for (GSList *l = files; l; l = l->next) {
        g_autofree char *path = g_file_get_path(l->data);
        char *rel = path ? import_image_file(path) : NULL;
        if (rel)
            g_ptr_array_add(rels, rel);
    }
    if (rels->len)
        insert_images(rels, at);
    else if (files)
        toast("Only image files can be added");
}

static void insert_texture(GdkTexture *tex, const GtkTextIter *at) {
    g_autoptr(GPtrArray) rels = g_ptr_array_new_with_free_func(g_free);
    char *rel = save_texture(tex);
    if (!rel) {
        toast("Couldn't save image");
        return;
    }
    g_ptr_array_add(rels, rel);
    insert_images(rels, at);
}

/* ------------------------------------------------------------------------- */
/* Clipboard                                                                  */
/* ------------------------------------------------------------------------- */

static void paste_text_done(GObject *src, GAsyncResult *res, gpointer unused) {
    (void)unused;
    g_autofree char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, NULL);
    if (!text || !*text)
        return;
    GtkTextIter it;
    gtk_text_buffer_begin_user_action(J.buf);
    gtk_text_buffer_delete_selection(J.buf, TRUE, TRUE);
    cursor_iter(&it);
    insert_text(J.buf, &it, text);
    gtk_text_buffer_end_user_action(J.buf);
    after_insert();
}

static void paste_texture_done(GObject *src, GAsyncResult *res, gpointer unused) {
    (void)unused;
    g_autoptr(GError) err = NULL;
    g_autoptr(GdkTexture) tex = gdk_clipboard_read_texture_finish(GDK_CLIPBOARD(src), res, &err);
    if (!tex) {
        g_autofree char *msg = g_strdup_printf("Couldn't read image: %s", err ? err->message : "?");
        toast(msg);
        return;
    }
    insert_texture(tex, NULL);
}

static void paste_files_done(GObject *src, GAsyncResult *res, gpointer unused) {
    (void)unused;
    const GValue *v = gdk_clipboard_read_value_finish(GDK_CLIPBOARD(src), res, NULL);
    if (v && G_VALUE_HOLDS(v, GDK_TYPE_FILE_LIST)) {
        g_autoptr(GSList) files = gdk_file_list_get_files(g_value_get_boxed(v));
        insert_files(files, NULL);
    }
}

static void on_paste(GtkTextView *view, gpointer unused) {
    (void)unused;
    g_signal_stop_emission_by_name(view, "paste-clipboard");
    GdkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(view));
    GdkContentFormats *f = gdk_clipboard_get_formats(cb);
    gboolean has_text = gdk_content_formats_contain_gtype(f, G_TYPE_STRING);
    gboolean has_image = gdk_content_formats_contain_gtype(f, GDK_TYPE_TEXTURE);
    gsize n = 0;
    const char *const *mimes = gdk_content_formats_get_mime_types(f, &n);
    for (gsize i = 0; i < n; i++) {
        has_text |= g_str_has_prefix(mimes[i], "text/plain");
        has_image |= g_str_has_prefix(mimes[i], "image/");
    }
    /* Text wins when offered, so copying from apps that also attach a bitmap still pastes text. */
    if (has_text)
        gdk_clipboard_read_text_async(cb, NULL, paste_text_done, NULL);
    else if (has_image)
        gdk_clipboard_read_texture_async(cb, NULL, paste_texture_done, NULL);
    else if (gdk_content_formats_contain_gtype(f, GDK_TYPE_FILE_LIST))
        gdk_clipboard_read_value_async(cb, GDK_TYPE_FILE_LIST, G_PRIORITY_DEFAULT, NULL, paste_files_done, NULL);
}

/* Puts text on the clipboard; a lone image also goes on as a real image, for other apps. */
static void clipboard_set(const char *text, JotImage *lone_image) {
    GdkContentProvider *providers[2];
    guint n = 0;
    if (lone_image) {
        g_autoptr(GBytes) png = gdk_texture_save_to_png_bytes(lone_image->texture);
        providers[n++] = gdk_content_provider_new_for_bytes("image/png", png);
    }
    g_autoptr(GBytes) bytes = g_bytes_new(text, strlen(text));
    providers[n++] = gdk_content_provider_new_for_bytes("text/plain;charset=utf-8", bytes);
    g_autoptr(GdkContentProvider) u = gdk_content_provider_new_union(providers, n);
    gdk_clipboard_set_content(gtk_widget_get_clipboard(GTK_WIDGET(J.view)), u);
}

static void copy_selection(GtkTextView *view, gboolean cut) {
    (void)view;
    GtkTextIter s, e;
    if (!gtk_text_buffer_get_selection_bounds(J.buf, &s, &e))
        return;
    g_autofree char *text = serialize_range(J.buf, &s, &e);
    GdkPaintable *p = gtk_text_iter_get_paintable(&s);
    gboolean lone = gtk_text_iter_get_offset(&e) - gtk_text_iter_get_offset(&s) == 1 && JOT_IS_IMAGE(p);
    clipboard_set(text, lone ? JOT_IMAGE(p) : NULL);
    if (cut) {
        gtk_text_buffer_begin_user_action(J.buf);
        gtk_text_buffer_delete_selection(J.buf, TRUE, TRUE);
        gtk_text_buffer_end_user_action(J.buf);
    }
}

static void on_copy(GtkTextView *view, gpointer unused) {
    (void)unused;
    g_signal_stop_emission_by_name(view, "copy-clipboard");
    copy_selection(view, FALSE);
}

static void on_cut(GtkTextView *view, gpointer unused) {
    (void)unused;
    g_signal_stop_emission_by_name(view, "cut-clipboard");
    copy_selection(view, TRUE);
}

/* ------------------------------------------------------------------------- */
/* Mouse, drag & drop                                                         */
/* ------------------------------------------------------------------------- */

static gboolean iter_at_point(double x, double y, GtkTextIter *it) {
    int bx, by;
    gtk_text_view_window_to_buffer_coords(J.view, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
    return gtk_text_view_get_iter_at_location(J.view, it, bx, by);
}

static gboolean on_drop(GtkDropTarget *t, const GValue *value, double x, double y, gpointer unused) {
    (void)t, (void)unused;
    GtkTextIter it;
    gboolean ok = iter_at_point(x, y, &it);
    if (G_VALUE_HOLDS(value, GDK_TYPE_TEXTURE)) {
        insert_texture(g_value_get_object(value), ok ? &it : NULL);
    } else if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
        g_autoptr(GSList) files = gdk_file_list_get_files(g_value_get_boxed(value));
        insert_files(files, ok ? &it : NULL);
    }
    return TRUE;
}

/* Opens an image in the system image viewer (zoom, pan, copy...). */
static void open_image(JotImage *img) {
    g_autofree char *abs = g_build_filename(notes_dir, img->path, NULL);
    g_autoptr(GFile) file = g_file_new_for_path(abs);
    g_autoptr(GtkFileLauncher) l = gtk_file_launcher_new(file);
    gtk_file_launcher_launch(l, J.win, NULL, NULL, NULL);
}

/* The image under a point. GTK maps a point on an image's right half to the position after it,
 * so check the characters on both sides against their actual rectangles. */
static JotImage *image_at_point(double x, double y, GtkTextIter *it) {
    int bx, by;
    gtk_text_view_window_to_buffer_coords(J.view, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
    GtkTextIter at;
    gtk_text_view_get_iter_at_location(J.view, &at, bx, by);
    for (int i = 0; i < 2; i++) {
        GtkTextIter c = at;
        if (i == 1 && !gtk_text_iter_backward_char(&c))
            break;
        GdkPaintable *p = gtk_text_iter_get_paintable(&c);
        GdkRectangle r;
        gtk_text_view_get_iter_location(J.view, &c, &r);
        if (JOT_IS_IMAGE(p) && bx >= r.x && bx < r.x + r.width && by >= r.y && by < r.y + r.height) {
            *it = c;
            return JOT_IMAGE(p);
        }
    }
    return NULL;
}

/* Click an image to put the cursor on it (which selects it); double-click opens it. Clicking
 * the right half of an image would otherwise put the cursor after it. */
static void on_click(GtkGestureClick *g, int n_press, double x, double y, gpointer unused) {
    (void)unused;
    GtkTextIter it;
    JotImage *img = image_at_point(x, y, &it);
    if (!img)
        return;
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    gtk_widget_grab_focus(GTK_WIDGET(J.view));
    gtk_text_buffer_place_cursor(J.buf, &it);
    if (n_press == 2)
        open_image(img);
}

/* Right-click an image: put the cursor on it, so the context menu's image items act on it. */
static void on_right_click(GtkGestureClick *g, int n_press, double x, double y, gpointer unused) {
    (void)g, (void)n_press, (void)unused;
    GtkTextIter it;
    if (image_at_point(x, y, &it) && !current_image())
        gtk_text_buffer_place_cursor(J.buf, &it);
}

/* ------------------------------------------------------------------------- */
/* Keyboard editing                                                           */
/* ------------------------------------------------------------------------- */

static void indent_range(int first, int last, int direction) {
    gtk_text_buffer_begin_user_action(J.buf);
    for (int ln = first; ln <= last; ln++) {
        GtkTextIter it, end;
        if (!gtk_text_buffer_get_iter_at_line(J.buf, &it, ln))
            break;
        if (direction > 0) {
            if (!gtk_text_iter_ends_line(&it))
                gtk_text_buffer_insert(J.buf, &it, INDENT, -1);
            continue;
        }
        end = it;
        int n = 0;
        while (n < (int)strlen(INDENT) && gtk_text_iter_get_char(&end) == ' ') {
            gtk_text_iter_forward_char(&end);
            n++;
        }
        if (n == 0 && gtk_text_iter_get_char(&end) == '\t')
            gtk_text_iter_forward_char(&end);
        gtk_text_buffer_delete(J.buf, &it, &end);
    }
    gtk_text_buffer_end_user_action(J.buf);
}

/* Indents/dedents the selected lines, or the cursor's line. */
static void indent_lines(int direction) {
    GtkTextIter s, e;
    int first, last;
    if (gtk_text_buffer_get_selection_bounds(J.buf, &s, &e)) {
        first = gtk_text_iter_get_line(&s);
        last = gtk_text_iter_get_line(&e);
        if (gtk_text_iter_starts_line(&e) && last > first)
            last--;
    } else {
        cursor_iter(&s);
        first = last = gtk_text_iter_get_line(&s);
    }
    indent_range(first, last, direction);
}

static void replace_selection(const char *text) {
    gtk_text_buffer_begin_user_action(J.buf);
    gtk_text_buffer_delete_selection(J.buf, TRUE, TRUE);
    gtk_text_buffer_insert_at_cursor(J.buf, text, -1);
    gtk_text_buffer_end_user_action(J.buf);
}

static gboolean vim_key(guint keyval, GdkModifierType mods);

static gboolean on_key(GtkEventControllerKey *c, guint keyval, guint code, GdkModifierType mods, gpointer unused) {
    (void)c, (void)code, (void)unused;
    if (vim_key(keyval, mods))
        return TRUE; /* normal/visual mode, or Esc & friends in insert mode */
    /* Insert mode from here on. */
    mods &= gtk_accelerator_get_default_mod_mask();
    if ((keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab) && !(mods & ~GDK_SHIFT_MASK)) {
        gboolean shift = (mods & GDK_SHIFT_MASK) || keyval == GDK_KEY_ISO_Left_Tab;
        GtkTextIter s, e;
        gboolean sel = gtk_text_buffer_get_selection_bounds(J.buf, &s, &e);
        if (shift || (sel && gtk_text_iter_get_line(&s) != gtk_text_iter_get_line(&e)))
            indent_lines(shift ? -1 : 1);
        else
            replace_selection(INDENT);
        return TRUE;
    }
    if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) && !mods) {
        /* Keep the current line's indentation. */
        GtkTextIter it, ls;
        cursor_iter(&it);
        ls = it;
        gtk_text_iter_set_line_offset(&ls, 0);
        g_autofree char *line = gtk_text_buffer_get_text(J.buf, &ls, &it, TRUE);
        gsize ws = strspn(line, " \t");
        g_autofree char *ins = g_strdup_printf("\n%.*s", (int)ws, line);
        replace_selection(ins);
        gtk_text_view_scroll_mark_onscreen(J.view, gtk_text_buffer_get_insert(J.buf));
        return TRUE;
    }
    return FALSE;
}

static void apply_font(void) {
    g_autofree char *css = g_strdup_printf("textview.jot { font-size: %dpt; }\n"
                                           ".jot-statusline { font-size: %dpt;"
                                           " padding: 3px 12px; }\n"
                                           ".jot-statusline text { padding: 0; min-height: 0; }\n",
                                           J.font_size, J.font_size);
    gtk_css_provider_load_from_string(J.css, css);
    if (J.gutter)
        gtk_widget_queue_resize(J.gutter); /* digit width changed */
}

static void zoom(int d) {
    J.font_size = d == 0 ? DEFAULT_FONT_PT : CLAMP(J.font_size + d, 7, 32);
    apply_font();
}

/* ------------------------------------------------------------------------- */
/* Line-number gutter                                                         */
/* ------------------------------------------------------------------------- */

/* Numbers logical lines (as in jot.md), at the top of each line; wrapped lines get one number. */
#define JOT_TYPE_GUTTER (jot_gutter_get_type())
G_DECLARE_FINAL_TYPE(JotGutter, jot_gutter, JOT, GUTTER, GtkWidget)

struct _JotGutter {
    GtkWidget parent;
    int digits;
};

G_DEFINE_FINAL_TYPE(JotGutter, jot_gutter, GTK_TYPE_WIDGET)

#define GUTTER_PAD_LEFT 16
#define GUTTER_PAD_RIGHT 12
#define GUTTER_MIN_DIGITS 3

static void jot_gutter_measure(GtkWidget *w, GtkOrientation o, int for_size, int *min, int *nat, int *min_base,
                               int *nat_base) {
    (void)for_size;
    *min_base = *nat_base = -1;
    if (o == GTK_ORIENTATION_VERTICAL) {
        *min = *nat = 0;
        return;
    }
    g_autoptr(PangoLayout) l = gtk_widget_create_pango_layout(w, "0");
    int cw, ch;
    pango_layout_get_pixel_size(l, &cw, &ch);
    *min = *nat = GUTTER_PAD_LEFT + JOT_GUTTER(w)->digits * cw + GUTTER_PAD_RIGHT;
}

static void jot_gutter_snapshot(GtkWidget *w, GtkSnapshot *s) {
    GtkTextView *v = J.view;
    int width = gtk_widget_get_width(w), height = gtk_widget_get_height(w);
    GdkRGBA fg, dim, rule;
    gtk_widget_get_color(w, &fg);
    dim = rule = fg;
    dim.alpha *= 0.4f;
    rule.alpha *= 0.1f;
    gtk_snapshot_append_color(s, &rule, &GRAPHENE_RECT_INIT(width - 1, 0, 1, height));

    GdkRectangle vis;
    GtkTextIter it, cur;
    gtk_text_view_get_visible_rect(v, &vis);
    gtk_text_view_get_line_at_y(v, &it, vis.y, NULL);
    cursor_iter(&cur);
    int cur_line = gtk_text_iter_get_line(&cur);
    int count = gtk_text_buffer_get_line_count(J.buf);
    g_autoptr(PangoLayout) layout = gtk_widget_create_pango_layout(w, NULL);
    for (int line = gtk_text_iter_get_line(&it); line < count; line++) {
        GtkTextIter li;
        int y, h, wx, wy, tw, th;
        gtk_text_buffer_get_iter_at_line(J.buf, &li, line);
        gtk_text_view_get_line_yrange(v, &li, &y, &h);
        if (y > vis.y + vis.height)
            break;
        gtk_text_view_buffer_to_window_coords(v, GTK_TEXT_WINDOW_LEFT, 0, y, &wx, &wy);
        char num[16];
        g_snprintf(num, sizeof num, "%d", line + 1);
        pango_layout_set_text(layout, num, -1);
        pango_layout_get_pixel_size(layout, &tw, &th);
        gtk_snapshot_save(s);
        gtk_snapshot_translate(s, &GRAPHENE_POINT_INIT(width - GUTTER_PAD_RIGHT - tw, wy));
        gtk_snapshot_append_layout(s, layout, line == cur_line ? &fg : &dim);
        gtk_snapshot_restore(s);
    }
}

static void jot_gutter_class_init(JotGutterClass *klass) {
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
    wc->measure = jot_gutter_measure;
    wc->snapshot = jot_gutter_snapshot;
    gtk_widget_class_set_css_name(wc, "jotgutter");
}

static void jot_gutter_init(JotGutter *self) {
    self->digits = GUTTER_MIN_DIGITS;
}

static int count_digits(int n) {
    int d = 1;
    while (n >= 10) {
        n /= 10;
        d++;
    }
    return d;
}

static void gutter_redraw(void) {
    gtk_widget_queue_draw(J.gutter);
}

static void on_gutter_buffer_changed(GtkTextBuffer *buf, gpointer unused) {
    (void)unused;
    JotGutter *g = JOT_GUTTER(J.gutter);
    int d = MAX(GUTTER_MIN_DIGITS, count_digits(gtk_text_buffer_get_line_count(buf)));
    if (d != g->digits) {
        g->digits = d;
        gtk_widget_queue_resize(J.gutter);
        g_idle_add(fit_images, NULL); /* text area width changed */
    }
    gutter_redraw();
}

static void on_gutter_mark_set(GtkTextBuffer *buf, GtkTextIter *it, GtkTextMark *mark, gpointer unused) {
    (void)it, (void)unused;
    if (mark == gtk_text_buffer_get_insert(buf))
        gutter_redraw(); /* current-line highlight */
}

static void set_line_numbers(gboolean show) {
    J.line_numbers = show;
    gtk_text_view_set_gutter(J.view, GTK_TEXT_WINDOW_LEFT, show ? J.gutter : NULL);
    g_idle_add(fit_images, NULL);
}

static void on_gutter_scroll(GtkAdjustment *adj, gpointer unused) {
    (void)adj, (void)unused;
    gutter_redraw();
}

/* ------------------------------------------------------------------------- */
/* Find in page                                                               */
/* ------------------------------------------------------------------------- */

/* The first line of an image's OCR text containing qfold (casefolded), or NULL. */
static char *image_match_line(JotImage *img, const char *qfold) {
    const char *text = ocr_lookup(img->path);
    if (!text || !*text)
        return NULL;
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        g_autofree char *fold = g_utf8_casefold(lines[i], -1);
        if (strstr(fold, qfold))
            return g_strdup(g_strstrip(lines[i]));
    }
    return NULL;
}

/* Nearest image from `from` (inclusive going forward) towards `limit` whose text matches. */
static JotImage *find_in_images(const GtkTextIter *from, const GtkTextIter *limit, gboolean forward,
                                const char *qfold, GtkTextIter *at, char **line) {
    GtkTextIter it = *from;
    for (;;) {
        if (forward) {
            if (gtk_text_iter_is_end(&it) || (limit && gtk_text_iter_compare(&it, limit) >= 0))
                return NULL;
            if (gtk_text_iter_get_char(&it) != 0xFFFC &&
                !gtk_text_iter_forward_find_char(&it, is_obj_char, NULL, limit))
                return NULL;
        } else if (!gtk_text_iter_backward_find_char(&it, is_obj_char, NULL, limit)) {
            return NULL;
        }
        GdkPaintable *p = gtk_text_iter_get_paintable(&it);
        if (JOT_IS_IMAGE(p) && (*line = image_match_line(JOT_IMAGE(p), qfold))) {
            *at = it;
            return JOT_IMAGE(p);
        }
        if (forward)
            gtk_text_iter_forward_char(&it);
    }
}

typedef struct {
    GtkTextIter start, end;
    JotImage *img; /* set when the match is in an image's OCR text */
    char *line;    /* ...and this is the matching line of that text (owned) */
    gboolean wrapped;
} Match;

/* Finds q from `from` (inclusive going forward; before it going backward), in the text or in an
 * image's OCR text, wrapping around the end of the page. */
static gboolean find_match(const char *q, gboolean case_sensitive, const GtkTextIter *from_in, gboolean forward,
                           Match *m) {
    if (!*q)
        return FALSE;
    g_autofree char *qfold = g_utf8_casefold(q, -1);
    GtkTextSearchFlags f = GTK_TEXT_SEARCH_TEXT_ONLY | (case_sensitive ? 0 : GTK_TEXT_SEARCH_CASE_INSENSITIVE);
    GtkTextIter from = *from_in, ms, me, at;
    for (int attempt = 0; attempt < 2; attempt++) {
        gboolean ok = forward ? gtk_text_iter_forward_search(&from, q, f, &ms, &me, NULL)
                              : gtk_text_iter_backward_search(&from, q, f, &ms, &me, NULL);
        /* An image between here and the text match is closer. */
        char *line = NULL;
        JotImage *img = find_in_images(&from, ok ? (forward ? &ms : &me) : NULL, forward, qfold, &at, &line);
        m->wrapped = attempt == 1;
        if (img) {
            m->start = m->end = at;
            gtk_text_iter_forward_char(&m->end);
            m->img = img;
            m->line = line;
            return TRUE;
        }
        if (ok) {
            m->start = ms;
            m->end = me;
            m->img = NULL;
            m->line = NULL;
            return TRUE;
        }
        if (forward)
            gtk_text_buffer_get_start_iter(J.buf, &from);
        else
            gtk_text_buffer_get_end_iter(J.buf, &from);
    }
    return FALSE;
}

#include "vim.c" /* vim-style modal editing; a unity build, so it shares this file's statics */

/* ------------------------------------------------------------------------- */
/* Actions                                                                    */
/* ------------------------------------------------------------------------- */

#define ACTION(name) static void act_##name(GSimpleAction *a, GVariant *p, gpointer d)
#define UNUSED (void)a, (void)p, (void)d

ACTION(copy_image_text) {
    UNUSED;
    JotImage *img = current_image();
    if (img)
        copy_image_text(img->path);
}
ACTION(open_image) {
    UNUSED;
    JotImage *img = current_image();
    if (img)
        open_image(img);
}
ACTION(save) { UNUSED; save(); }
ACTION(zoom_in) { UNUSED; zoom(1); }
ACTION(zoom_out) { UNUSED; zoom(-1); }
ACTION(zoom_reset) { UNUSED; zoom(0); }
ACTION(close) { UNUSED; gtk_window_close(J.win); }
ACTION(open_folder) {
    UNUSED;
    g_autoptr(GFile) f = g_file_new_for_path(notes_dir);
    g_autoptr(GtkFileLauncher) l = gtk_file_launcher_new(f);
    gtk_file_launcher_launch(l, J.win, NULL, NULL, NULL);
}
ACTION(quit) {
    UNUSED;
    if (J.win)
        gtk_window_close(J.win);
    g_application_quit(G_APPLICATION(J.app));
}

static const GActionEntry win_actions[] = {
    {"copy-image-text", act_copy_image_text, NULL, NULL, NULL, {0}},
    {"open-image", act_open_image, NULL, NULL, NULL, {0}},
    {"save", act_save, NULL, NULL, NULL, {0}},
    {"zoom-in", act_zoom_in, NULL, NULL, NULL, {0}},
    {"zoom-out", act_zoom_out, NULL, NULL, NULL, {0}},
    {"zoom-reset", act_zoom_reset, NULL, NULL, NULL, {0}},
    {"close", act_close, NULL, NULL, NULL, {0}},
    {"open-folder", act_open_folder, NULL, NULL, NULL, {0}},
};

static const struct {
    const char *action;
    const char *accels[4];
} accels[] = {
    {"win.copy-image-text", {"<Ctrl><Shift>c", NULL}},
    {"win.save", {"<Ctrl>s", NULL}},
    {"win.zoom-in", {"<Ctrl>plus", "<Ctrl>equal", "<Ctrl>KP_Add", NULL}},
    {"win.zoom-out", {"<Ctrl>minus", "<Ctrl>KP_Subtract", NULL}},
    {"win.zoom-reset", {"<Ctrl>0", NULL}},
    {"app.quit", {"<Ctrl>q", NULL}},
};

/* ------------------------------------------------------------------------- */
/* UI                                                                         */
/* ------------------------------------------------------------------------- */

static int state_int(const char *key, int fallback) {
    g_autoptr(GError) err = NULL;
    int v = g_key_file_get_integer(J.state, "window", key, &err);
    return err ? fallback : v;
}

static gboolean state_bool(const char *key, gboolean fallback) {
    g_autoptr(GError) err = NULL;
    gboolean v = g_key_file_get_boolean(J.state, "window", key, &err);
    return err ? fallback : v;
}

static void on_page_size(GtkAdjustment *adj, GParamSpec *ps, gpointer unused) {
    (void)adj, (void)ps, (void)unused;
    g_idle_add(fit_images, NULL);
}

static GtkWidget *build_editor(void) {
    J.view = GTK_TEXT_VIEW(gtk_text_view_new());
    GtkTextView *v = J.view;
    gtk_text_view_set_monospace(v, TRUE);
    gtk_text_view_set_wrap_mode(v, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(v, 14);
    gtk_text_view_set_right_margin(v, 28);
    gtk_text_view_set_top_margin(v, 18);
    gtk_text_view_set_bottom_margin(v, 300);
    gtk_text_view_set_pixels_below_lines(v, 2);
    gtk_widget_add_css_class(GTK_WIDGET(v), "jot");
    J.buf = gtk_text_view_get_buffer(v);
    g_signal_connect(J.buf, "changed", G_CALLBACK(on_changed), NULL);

    J.gutter = g_object_new(JOT_TYPE_GUTTER, NULL);
    J.line_numbers = TRUE;
    g_object_ref_sink(J.gutter); /* kept while hidden by :set nonu */
    gtk_text_view_set_gutter(v, GTK_TEXT_WINDOW_LEFT, J.gutter);
    g_signal_connect(J.buf, "changed", G_CALLBACK(on_gutter_buffer_changed), NULL);
    g_signal_connect(J.buf, "mark-set", G_CALLBACK(on_gutter_mark_set), NULL);

    g_signal_connect(v, "paste-clipboard", G_CALLBACK(on_paste), NULL);
    g_signal_connect(v, "copy-clipboard", G_CALLBACK(on_copy), NULL);
    g_signal_connect(v, "cut-clipboard", G_CALLBACK(on_cut), NULL);

    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), NULL);
    gtk_widget_add_controller(GTK_WIDGET(v), keys);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(on_click), NULL);
    gtk_widget_add_controller(GTK_WIDGET(v), GTK_EVENT_CONTROLLER(click));

    GtkGesture *rclick = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rclick), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(rclick), GTK_PHASE_CAPTURE);
    g_signal_connect(rclick, "pressed", G_CALLBACK(on_right_click), NULL);
    gtk_widget_add_controller(GTK_WIDGET(v), GTK_EVENT_CONTROLLER(rclick));

    /* Image items in the right-click menu, shown only when an image is selected. */
    g_autoptr(GMenu) extra = g_menu_new();
    const char *items[][2] = {{"Copy Text from Image", "win.copy-image-text"}, {"Open Image", "win.open-image"}};
    for (guint i = 0; i < G_N_ELEMENTS(items); i++) {
        g_autoptr(GMenuItem) item = g_menu_item_new(items[i][0], items[i][1]);
        g_menu_item_set_attribute(item, "hidden-when", "s", "action-disabled");
        g_menu_append_item(extra, item);
    }
    gtk_text_view_set_extra_menu(v, G_MENU_MODEL(extra));
    g_signal_connect(J.buf, "mark-set", G_CALLBACK(on_selection_mark_set), NULL);

    GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_COPY);
    GType types[] = {GDK_TYPE_FILE_LIST, GDK_TYPE_TEXTURE};
    gtk_drop_target_set_gtypes(drop, types, G_N_ELEMENTS(types));
    g_signal_connect(drop, "drop", G_CALLBACK(on_drop), NULL);
    gtk_widget_add_controller(GTK_WIDGET(v), GTK_EVENT_CONTROLLER(drop));

    J.css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(J.css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    apply_font();

    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), GTK_WIDGET(v));
    /* The horizontal adjustment's page size tracks the view width: refit images on resize.
     * (Connected after set_child, which swaps in the scroller's adjustment.) */
    g_signal_connect(gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroller)), "notify::page-size",
                     G_CALLBACK(on_page_size), NULL);
    /* Line positions move on scroll, and as the view lays out lines (which changes "upper"). */
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller));
    g_signal_connect(vadj, "value-changed", G_CALLBACK(on_gutter_scroll), NULL);
    g_signal_connect(vadj, "changed", G_CALLBACK(on_gutter_scroll), NULL);
    return scroller;
}

static GtkWidget *build_header(void) {
    const char *home = g_get_home_dir();
    g_autofree char *pretty = g_str_has_prefix(page_path, home) ? g_strconcat("~", page_path + strlen(home), NULL)
                                                                 : g_strdup(page_path);
    GtkWidget *header = adw_header_bar_new();
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), adw_window_title_new("jot", pretty));

    g_autoptr(GMenu) menu = g_menu_new();
    g_menu_append(menu, "Open notes folder", "win.open-folder");
    g_menu_append(menu, "Zoom in", "win.zoom-in");
    g_menu_append(menu, "Zoom out", "win.zoom-out");
    g_menu_append(menu, "Reset zoom", "win.zoom-reset");
    GtkWidget *menu_btn = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_btn), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_btn), G_MENU_MODEL(menu));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu_btn);

    return header;
}

static void on_activate(GtkApplication *app, gpointer unused) {
    (void)unused;
    if (J.win) {
        gtk_window_present(J.win);
        return;
    }
    J.app = app;
    J.state = g_key_file_new();
    g_key_file_load_from_file(J.state, state_path, G_KEY_FILE_NONE, NULL);
    J.font_size = state_int("font_size", DEFAULT_FONT_PT);

    J.win = GTK_WINDOW(adw_application_window_new(app));
    gtk_window_set_title(J.win, "jot");
    gtk_window_set_default_size(J.win, state_int("width", 900), state_int("height", 750));
    if (state_bool("maximized", FALSE))
        gtk_window_maximize(J.win);

    J.toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(J.toasts, build_editor());
    GtkWidget *tv = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), build_header());
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(tv), vim_build_bar());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), GTK_WIDGET(J.toasts));
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(J.win), tv);

    g_action_map_add_action_entries(G_ACTION_MAP(J.win), win_actions, G_N_ELEMENTS(win_actions), NULL);
    static const GActionEntry app_actions[] = {{"quit", act_quit, NULL, NULL, NULL, {0}}};
    g_action_map_add_action_entries(G_ACTION_MAP(app), app_actions, G_N_ELEMENTS(app_actions), NULL);
    for (guint i = 0; i < G_N_ELEMENTS(accels); i++)
        gtk_application_set_accels_for_action(app, accels[i].action, accels[i].accels);

    g_signal_connect(J.win, "close-request", G_CALLBACK(on_close), NULL);
    g_signal_connect(J.win, "notify::is-active", G_CALLBACK(on_active), NULL);

    if (!state_bool("line_numbers", TRUE))
        set_line_numbers(FALSE);
    load_page();
    vim_init();
    update_image_selection();
    gtk_window_present(J.win);
    gtk_widget_grab_focus(GTK_WIDGET(J.view));
}

static void init_paths(void) {
    const char *env = g_getenv("JOT_DIR");
    if (env && *env)
        notes_dir = g_str_has_prefix(env, "~/") ? g_build_filename(g_get_home_dir(), env + 2, NULL)
                                                : g_canonicalize_filename(env, NULL);
    else
        notes_dir = g_build_filename(g_get_home_dir(), "notes", NULL);
    assets_dir = g_build_filename(notes_dir, "assets", NULL);
    page_path = g_build_filename(notes_dir, "jot.md", NULL);
    state_path = g_build_filename(g_get_user_config_dir(), "jot", "state.ini", NULL);
    g_mkdir_with_parents(assets_dir, 0755);
    img_re = g_regex_new("^!\\[[^\\]]*\\]\\((assets/[^)\\s/]+)\\)$", 0, 0, NULL);
    textures = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    ocr_text = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    ocr_pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    ocr_failed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    ocr_words = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
    tesseract_path = g_find_program_in_path("tesseract");
}

#ifndef JOT_NO_MAIN
int main(int argc, char **argv) {
    init_paths();
    g_autoptr(AdwApplication) app = adw_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    return g_application_run(G_APPLICATION(app), argc, argv);
}
#endif
