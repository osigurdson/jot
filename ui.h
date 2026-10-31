/* ui.h - what the editing engine (vim.c, md.c) asks of the platform, and what it offers back.
 *
 * ui_gtk.c implements this on Linux. Each platform owns its window, status bar, scrolling and
 * clipboard; the engine never touches a widget. Keep this small: anything that isn't tied to a
 * platform belongs in the engine, where the tests can reach it.
 */
#ifndef JOT_UI_H
#define JOT_UI_H

/* Keys that aren't text. Everything else reaches vim_key() as JK_CHAR plus its character. */
typedef enum {
    JK_NONE, /* no character and no meaning: a modifier, a function key */
    JK_CHAR,
    JK_ESC,
    JK_RETURN,
    JK_BACKSPACE,
    JK_TAB,
    JK_LEFT,
    JK_RIGHT,
    JK_UP,
    JK_DOWN,
    JK_HOME,
    JK_END,
    JK_PAGE_UP,
    JK_PAGE_DOWN,
    JK_DELETE,
    JK_INSERT
} JotKey;

/* ---- The platform provides these --------------------------------------- */

/* Status bar. Text is UTF-8; ui_status_ocr(NULL) hides the indexing note. */
gboolean ui_status_ready(void); /* the bar exists yet */
void ui_status_mode(const char *text, gboolean error);
void ui_status_keys(const char *text);
void ui_status_pos(const char *text);
void ui_status_ocr(const char *text);

/* Command line (: / ?). Shown in place of the mode text; takes keyboard focus while open. */
void ui_cmd_open(const char *prefix, const char *text);
void ui_cmd_close(void);
gboolean ui_cmd_is_open(void);
const char *ui_cmd_text(void);
void ui_cmd_set_text(const char *text); /* puts the caret at the end */

/* The view */
void ui_block_cursor(gboolean block);
void ui_bell(void);
void ui_scroll_mark_onscreen(JotMark *mark);
void ui_scroll_mark_to(JotMark *mark, double yalign); /* 0 top, 0.5 middle, 1 bottom */
void ui_scroll_iter_onscreen(const JotIter *it);      /* with a little context around it */
void ui_scroll_lines(const JotIter *cursor, int dir, int count);
void ui_visible_lines(int *top, int *bottom);
double ui_iter_screen_pos(const JotIter *it); /* 0 top of the view .. 1 bottom */

/* How each Markdown style looks: tags[] is indexed by the T_ values in jot.h. */
void ui_md_style(JotTag **tags);

/* The system */
void ui_quit(void);
void ui_open_folder(const char *path);
void ui_open_uri(const char *uri);

/* Reads the clipboard (possibly later) and answers with exactly one of vim_put_text,
 * vim_put_image or vim_put_none, passing `request` back. */
void ui_clipboard_read(void *request);

/* ---- The engine provides these ------------------------------------------ */

/* A key for the editor. `ch` is the character for JK_CHAR (lowercased when `ctrl`).
 * TRUE = handled; FALSE = let the platform have it (typing in insert mode, Ctrl+C...). */
gboolean vim_key(JotKey key, gunichar ch, gboolean ctrl);

/* The command line changed, was submitted, or got a key. TRUE = the key was handled. */
void vim_cmd_changed(void);
void vim_cmd_activate(void);
gboolean vim_cmd_key(JotKey key, gunichar ch, gboolean ctrl);

void vim_put_text(void *request, const char *text); /* NULL or empty: nothing to put */
void vim_put_image(void *request, char *rel);       /* takes rel; NULL: the read failed */
void vim_put_none(void *request);

#endif
