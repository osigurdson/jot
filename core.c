/* core.c - page text logic shared by every platform: text <-> buffer, images on their own lines,
 * indenting, and search across text and screenshots. Uses only what lite.h also provides. */
#include "jot.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Image lines                                                                */
/* ------------------------------------------------------------------------- */

/* If line is exactly an image reference, returns the "assets/..." path. */
static char *match_img_line(const char *line) {
    g_autofree char *s = g_strstrip(g_strdup(line));
    static GRegex *img_re;
    if (!img_re)
        img_re = g_regex_new("^!\\[[^\\]]*\\]\\((assets/[^)\\s/]+)\\)$", 0, 0, NULL);
    g_autoptr(GMatchInfo) m = NULL;
    return g_regex_match(img_re, s, 0, &m) ? g_match_info_fetch(m, 1) : NULL;
}

/* ------------------------------------------------------------------------- */
/* Buffer <-> text                                                            */
/* ------------------------------------------------------------------------- */

char *serialize_range(GtkTextBuffer *buf, const GtkTextIter *start, const GtkTextIter *end) {
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

char *serialize(GtkTextBuffer *buf) {
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(buf, &s, &e);
    return serialize_range(buf, &s, &e);
}

/* Inserts plain text at it; ![](assets/..) lines whose file exists become images. */
void insert_text(GtkTextBuffer *buf, GtkTextIter *it, const char *raw) {
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

void cursor_iter(GtkTextIter *it) {
    gtk_text_buffer_get_iter_at_mark(J.buf, it, gtk_text_buffer_get_insert(J.buf));
}

gboolean is_obj_char(gunichar ch, gpointer unused) {
    (void)unused;
    return ch == 0xFFFC;
}

/* Inserts images, each on its own line, at `at` (or replacing the selection when NULL). */
void insert_images(GPtrArray *rels, const GtkTextIter *at) {
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

/* ------------------------------------------------------------------------- */
/* Insert mode keys                                                           */
/* ------------------------------------------------------------------------- */

void indent_range(int first, int last, int direction) {
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

/* Types text, replacing the selection. */
void edit_type(const char *text) {
    replace_selection(text);
}

/* Enter: a new line that keeps the current line's indentation. */
void edit_return(void) {
    GtkTextIter it, ls;
    cursor_iter(&it);
    ls = it;
    gtk_text_iter_set_line_offset(&ls, 0);
    g_autofree char *line = gtk_text_buffer_get_text(J.buf, &ls, &it, TRUE);
    gsize ws = strspn(line, " \t");
    g_autofree char *ins = g_strdup_printf("\n%.*s", (int)ws, line);
    replace_selection(ins);
    ui_scroll_mark_onscreen(gtk_text_buffer_get_insert(J.buf));
}

/* Tab: indents the selected lines when several are selected, else types the indent. */
void edit_tab(gboolean shift) {
    GtkTextIter s, e;
    gboolean sel = gtk_text_buffer_get_selection_bounds(J.buf, &s, &e);
    if (shift || (sel && gtk_text_iter_get_line(&s) != gtk_text_iter_get_line(&e)))
        indent_lines(shift ? -1 : 1);
    else
        replace_selection(INDENT);
}

/* ------------------------------------------------------------------------- */
/* Search                                                                     */
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

/* Finds q from `from` (inclusive going forward; before it going backward), in the text or in an
 * image's OCR text, wrapping around the end of the page. */
gboolean find_match(const char *q, gboolean case_sensitive, const GtkTextIter *from_in, gboolean forward,
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
