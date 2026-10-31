/* jot.h - what jot.c, md.c (Markdown styling) and vim.c (modal editing) share. */
#ifndef JOT_H
#define JOT_H

#ifdef JOT_LITE
#include "lite.h"
#else
#include <adwaita.h>
#endif

#include "ui.h"

#define APP_ID "dev.jot.Jot"
#define OBJ_UTF8 "\xef\xbf\xbc" /* U+FFFC: what GtkTextBuffer reports for an embedded image */
#define INDENT "    "

/* ------------------------------------------------------------------------- */
/* jot.c                                                                      */
/* ------------------------------------------------------------------------- */

extern char *notes_dir, *page_path;

#ifdef JOT_LITE

/* An image in the page; the platform owns everything past `path` (pixels, its fitted size...). */
typedef struct JotImage {
    int refs;
    char *path; /* "assets/x.png", relative to notes_dir */
    gboolean selected;
    void *platform;
} JotImage;

#define JOT_IS_IMAGE(p) ((p) != NULL)
#define JOT_IMAGE(p) (p)
#define GDK_PAINTABLE(p) (p)
static inline JotImage *g_object_ref(JotImage *img) {
    lite_image_ref(img);
    return img;
}
#define g_object_unref lite_image_unref
#define g_clear_object(pp) g_clear_pointer(pp, lite_image_unref)

typedef struct {
    GtkTextBuffer *buf;
    gboolean line_numbers;
} Jot;

#else

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

/* The window and the page (single window per process, so this is a global). */
typedef struct {
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
} Jot;

#endif

extern Jot J;

/* A search match, in the text or in an image's OCR text. */
typedef struct {
    GtkTextIter start, end;
    JotImage *img; /* set when the match is in an image's OCR text */
    char *line;    /* ...and this is the matching line of that text (owned) */
    gboolean wrapped;
} Match;

/* Images, text, saving */
void toast(const char *msg);
void cursor_iter(GtkTextIter *it);
char *serialize(GtkTextBuffer *buf);
char *serialize_range(GtkTextBuffer *buf, const GtkTextIter *start, const GtkTextIter *end);
void insert_text(GtkTextBuffer *buf, GtkTextIter *it, const char *raw);
void insert_images(GPtrArray *rels, const GtkTextIter *at);
JotImage *load_image(const char *rel);
const char *ocr_lookup(const char *rel); /* an image's text, NULL if not read yet */
void ocr_request(const char *rel);
gboolean is_obj_char(gunichar ch, gpointer unused);
void after_insert(void);
void open_image(JotImage *img);
void image_set_highlight(JotImage *img, const char *q);
void update_image_selection(void);
void clipboard_set(const char *text, JotImage *lone_image);
void save(void);
void load_page(void);
guint ocr_pending_count(void); /* images queued for, or in, OCR */

/* Editing, view */
void indent_range(int first, int last, int direction);
void edit_type(const char *text);
void edit_return(void);
void edit_tab(gboolean shift);
void set_line_numbers(gboolean show);
gboolean find_match(const char *q, gboolean case_sensitive, const GtkTextIter *from, gboolean forward, Match *m);

/* ------------------------------------------------------------------------- */
/* md.c                                                                       */
/* ------------------------------------------------------------------------- */

void md_init(void);
gboolean md_enabled(void);
void md_set_enabled(gboolean on);
gboolean md_open_url_at(const GtkTextIter *it);
int md_heading_line(int line, int dir);

/* ------------------------------------------------------------------------- */
/* vim.c                                                                      */
/* ------------------------------------------------------------------------- */

void vim_init(void);
gboolean vim_image_cursor_mode(void);
void vim_ocr_changed(gboolean finished); /* the OCR queue changed; finished = a read completed */

#ifndef JOT_LITE
/* ui_gtk.c and jot.c, Linux only (the rest of ui_gtk.c is in ui.h) */
GtkWidget *ui_build_bar(void);
gboolean ui_key(guint keyval, GdkModifierType mods);
char *save_texture(GdkTexture *tex);
#endif

#endif
