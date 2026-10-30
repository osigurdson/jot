/* jot.h - what jot.c, md.c (Markdown styling) and vim.c (modal editing) share. */
#ifndef JOT_H
#define JOT_H

#include <adwaita.h>

#define APP_ID "dev.jot.Jot"
#define OBJ_UTF8 "\xef\xbf\xbc" /* U+FFFC: what GtkTextBuffer reports for an embedded image */
#define INDENT "    "

/* ------------------------------------------------------------------------- */
/* jot.c                                                                      */
/* ------------------------------------------------------------------------- */

extern char *notes_dir, *page_path;

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
char *save_texture(GdkTexture *tex);
void insert_images(GPtrArray *rels, const GtkTextIter *at);
void after_insert(void);
void open_image(JotImage *img);
void image_set_highlight(JotImage *img, const char *q);
void update_image_selection(void);
void clipboard_set(const char *text, JotImage *lone_image);
void save(void);
void load_page(void);

/* Editing, view */
void indent_range(int first, int last, int direction);
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

GtkWidget *vim_build_bar(void);
void vim_init(void);
gboolean vim_key(guint keyval, GdkModifierType mods);
gboolean vim_image_cursor_mode(void);

#endif
