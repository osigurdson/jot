/* buf.h - the page in memory: text, images, cursor, selection, undo, styles. The engine (vim.c,
 * md.c, core.c) edits the page only through this. buf.c implements it; on Linux, while the editor
 * is GTK's text view, buf_gtk.h maps the same names onto GtkTextBuffer.
 *
 * Offsets count characters. An iterator is a position; edits update the ones passed to them.
 */
#ifndef JOT_BUF_H
#define JOT_BUF_H

typedef struct JotBuf JotBuf;
typedef struct JotMark JotMark;
typedef struct JotTag JotTag;
typedef struct JotImage JotImage; /* an embedded image: U+FFFC in the text */

typedef struct {
    JotBuf *buf;
    int off;
} JotIter;

typedef enum {
    JB_SEARCH_TEXT_ONLY = 1,
    JB_SEARCH_CASE_INSENSITIVE = 2
} JotSearchFlags;

/* The buffer keeps images alive with these; the platform owns the image type. */
JotImage *jot_image_ref(JotImage *img);
void jot_image_unref(JotImage *img);

JotBuf *jb_new(void);
void jb_free(JotBuf *b);

JotTag *jb_create_tag(JotBuf *b, const char *name);
void jb_apply_tag(JotBuf *b, JotTag *t, const JotIter *s, const JotIter *e);
void jb_remove_tag(JotBuf *b, JotTag *t, const JotIter *s, const JotIter *e);
unsigned jb_tags_at(JotBuf *b, int offset); /* bit n = the nth tag created */
int jb_copy_line(JotBuf *b, int line, gunichar *ch, unsigned *tags, int max);

extern gboolean jb_merge_deletes; /* test_engine.c only, see buf.c */
void jb_begin_user_action(JotBuf *b);
void jb_end_user_action(JotBuf *b);
void jb_begin_irreversible_action(JotBuf *b);
void jb_end_irreversible_action(JotBuf *b);
gboolean jb_get_can_undo(JotBuf *b);
gboolean jb_get_can_redo(JotBuf *b);
void jb_undo(JotBuf *b);
void jb_redo(JotBuf *b);

void jb_insert(JotBuf *b, JotIter *it, const char *text, int len);
void jb_insert_at_cursor(JotBuf *b, const char *text, int len);
void jb_insert_image(JotBuf *b, JotIter *it, JotImage *p);
void jb_delete(JotBuf *b, JotIter *s, JotIter *e);
gboolean jb_delete_selection(JotBuf *b, gboolean interactive, gboolean editable);
void jb_set_text(JotBuf *b, const char *text, int len);
char *jb_get_text(JotBuf *b, const JotIter *s, const JotIter *e, gboolean hidden);
char *jb_get_slice(JotBuf *b, const JotIter *s, const JotIter *e, gboolean hidden);

int jb_get_char_count(JotBuf *b);
int jb_get_line_count(JotBuf *b);
int jb_line_len(JotBuf *b, int line); /* characters, without the newline */
void jb_get_start_iter(JotBuf *b, JotIter *it);
void jb_get_end_iter(JotBuf *b, JotIter *it);
void jb_get_bounds(JotBuf *b, JotIter *s, JotIter *e);
gboolean jb_get_iter_at_line(JotBuf *b, JotIter *it, int line);
void jb_get_iter_at_offset(JotBuf *b, JotIter *it, int offset);
void jb_get_iter_at_mark(JotBuf *b, JotIter *it, JotMark *m);

JotMark *jb_create_mark(JotBuf *b, const char *name, const JotIter *at, gboolean left_gravity);
void jb_move_mark(JotBuf *b, JotMark *m, const JotIter *at);
JotMark *jb_get_insert(JotBuf *b);
void jb_place_cursor(JotBuf *b, const JotIter *at);
void jb_select_range(JotBuf *b, const JotIter *ins, const JotIter *bound);
gboolean jb_get_selection_bounds(JotBuf *b, JotIter *s, JotIter *e);
gboolean jb_get_has_selection(JotBuf *b);

gunichar jb_iter_get_char(const JotIter *it);
JotImage *jb_iter_get_image(const JotIter *it);
int jb_iter_get_offset(const JotIter *it);
int jb_iter_get_line(const JotIter *it);
int jb_iter_get_line_offset(const JotIter *it);
void jb_iter_set_line_offset(JotIter *it, int col);
gboolean jb_iter_is_start(const JotIter *it);
gboolean jb_iter_is_end(const JotIter *it);
gboolean jb_iter_starts_line(const JotIter *it);
gboolean jb_iter_ends_line(const JotIter *it);
gboolean jb_iter_equal(const JotIter *a, const JotIter *b);
int jb_iter_compare(const JotIter *a, const JotIter *b);
gboolean jb_iter_forward_char(JotIter *it);
gboolean jb_iter_backward_char(JotIter *it);
gboolean jb_iter_forward_chars(JotIter *it, int n);
gboolean jb_iter_forward_line(JotIter *it);
gboolean jb_iter_forward_to_line_end(JotIter *it);
gboolean jb_iter_forward_find_char(JotIter *it, gboolean (*pred)(gunichar, gpointer), gpointer data, const JotIter *limit);
gboolean jb_iter_backward_find_char(JotIter *it, gboolean (*pred)(gunichar, gpointer), gpointer data, const JotIter *limit);
gboolean jb_iter_forward_search(const JotIter *it, const char *q, JotSearchFlags f, JotIter *ms, JotIter *me, const JotIter *limit);
gboolean jb_iter_backward_search(const JotIter *it, const char *q, JotSearchFlags f, JotIter *ms, JotIter *me, const JotIter *limit);

/* Signals: "insert-text", "delete-range", "changed", "mark-set". */
void jb_connect_mode(JotBuf *b, const char *name, void (*fn)(void), gpointer data, int mode);
#define jb_connect(o, name, fn, data) jb_connect_mode(o, name, G_CALLBACK(fn), data, 0)
#define jb_connect_after(o, name, fn, data) jb_connect_mode(o, name, G_CALLBACK(fn), data, 1)
#define jb_connect_swapped(o, name, fn, data) jb_connect_mode(o, name, G_CALLBACK(fn), data, 2)

#endif
