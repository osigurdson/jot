/* md.c - light Markdown styling for jot.
 *
 * Color and weight only: same font, same size, and every character stays visible (markers are
 * dimmed, never hidden), so vim motions see exactly what's on screen. The rules are deliberately
 * conservative because scratch notes are full of pasted code and logs: emphasis needs word
 * boundaries (a*b*c, snake_case, **kwargs stay plain), nothing is styled inside code, and image
 * lines are left alone. `:set nomd` turns it off.
 */
#include "jot.h"

#include <string.h>

static struct {
    gboolean enabled;
    JotTag *tag[N_TAGS];
    int first, last; /* dirty lines; first < 0 = nothing to do */
    gboolean to_end; /* a code fence changed: everything below may change */
    guint idle;
    GRegex *heading, *rule, *quote, *list, *code, *link, *url, *bold, *italic;
} M = {.first = -1};

/* ------------------------------------------------------------------------- */
/* Lines                                                                      */
/* ------------------------------------------------------------------------- */

static gboolean is_fence(const char *t) {
    int i = 0;
    while (i < 3 && t[i] == ' ')
        i++;
    return g_str_has_prefix(t + i, "```") || g_str_has_prefix(t + i, "~~~");
}

/* Whether a buffer line is a code fence, reading just its first few chars. */
static gboolean line_is_fence(int line) {
    JotIter it;
    if (!jb_get_iter_at_line(J.buf, &it, line))
        return FALSE;
    char head[8] = {0};
    for (int i = 0; i < 6 && !jb_iter_ends_line(&it); i++) {
        gunichar c = jb_iter_get_char(&it);
        head[i] = c < 128 ? (char)c : '?';
        jb_iter_forward_char(&it);
    }
    return is_fence(head);
}

/* Whether `line` starts inside a fenced code block. */
static gboolean in_code_at(int line) {
    gboolean in_code = FALSE;
    for (int l = 0; l < line; l++)
        if (line_is_fence(l))
            in_code = !in_code;
    return in_code;
}

static void line_bounds(int line, JotIter *s, JotIter *e, JotIter *next) {
    jb_get_iter_at_line(J.buf, s, line);
    *e = *s;
    if (!jb_iter_ends_line(e))
        jb_iter_forward_to_line_end(e);
    *next = *e;
    jb_iter_forward_char(next); /* the newline, so paragraph backgrounds cover empty lines */
}

/* Tags bytes [from, to) of `text`, the line starting at `ls`. */
static void tag_bytes(const JotIter *ls, const char *text, int from, int to, int t) {
    if (to <= from)
        return;
    JotIter a = *ls, b = *ls;
    jb_iter_forward_chars(&a, (int)g_utf8_pointer_to_offset(text, text + from));
    jb_iter_forward_chars(&b, (int)g_utf8_pointer_to_offset(text, text + to));
    jb_apply_tag(J.buf, M.tag[t], &a, &b);
}

static gboolean masked(const gboolean *mask, int a, int b) {
    for (int i = a; i < b; i++)
        if (mask[i])
            return TRUE;
    return FALSE;
}

static void mask_set(gboolean *mask, int a, int b) {
    for (int i = a; i < b; i++)
        mask[i] = TRUE;
}

/* Inline markup in text[from..]: `code`, [links](url), bare URLs, **bold**, *italic*. */
static void md_inline(const JotIter *ls, const char *text, int from) {
    int len = (int)strlen(text);
    g_autofree gboolean *mask = g_new0(gboolean, len + 1);
    GMatchInfo *mi = NULL;
    int a, b, c, d;

    g_regex_match_full(M.code, text, -1, from, 0, &mi, NULL);
    for (; g_match_info_matches(mi); g_match_info_next(mi, NULL)) {
        g_match_info_fetch_pos(mi, 0, &a, &b);
        g_match_info_fetch_pos(mi, 1, &c, &d); /* the backtick run */
        tag_bytes(ls, text, a, b, T_CODE);
        tag_bytes(ls, text, a, a + (d - c), T_DIM);
        tag_bytes(ls, text, b - (d - c), b, T_DIM);
        mask_set(mask, a, b);
    }
    g_clear_pointer(&mi, g_match_info_unref);

    g_regex_match_full(M.link, text, -1, from, 0, &mi, NULL);
    for (; g_match_info_matches(mi); g_match_info_next(mi, NULL)) {
        g_match_info_fetch_pos(mi, 0, &a, &b);
        if (masked(mask, a, b))
            continue;
        g_match_info_fetch_pos(mi, 1, &c, &d); /* the link text */
        tag_bytes(ls, text, c, d, T_LINK);
        tag_bytes(ls, text, a, c, T_DIM);
        tag_bytes(ls, text, d, b, T_DIM);
        mask_set(mask, a, b);
    }
    g_clear_pointer(&mi, g_match_info_unref);

    g_regex_match_full(M.url, text, -1, from, 0, &mi, NULL);
    for (; g_match_info_matches(mi); g_match_info_next(mi, NULL)) {
        g_match_info_fetch_pos(mi, 0, &a, &b);
        while (b > a && strchr(".,;:!?", text[b - 1])) /* "see https://x.org." */
            b--;
        if (masked(mask, a, b))
            continue;
        tag_bytes(ls, text, a, b, T_LINK);
        mask_set(mask, a, b);
    }
    g_clear_pointer(&mi, g_match_info_unref);

    g_regex_match_full(M.bold, text, -1, from, 0, &mi, NULL);
    for (; g_match_info_matches(mi); g_match_info_next(mi, NULL)) {
        g_match_info_fetch_pos(mi, 0, &a, &b);
        if (masked(mask, a, b))
            continue;
        tag_bytes(ls, text, a + 2, b - 2, T_BOLD);
        tag_bytes(ls, text, a, a + 2, T_DIM);
        tag_bytes(ls, text, b - 2, b, T_DIM);
        mask_set(mask, a, a + 2); /* markers only: *italic* may sit inside **bold** */
        mask_set(mask, b - 2, b);
    }
    g_clear_pointer(&mi, g_match_info_unref);

    g_regex_match_full(M.italic, text, -1, from, 0, &mi, NULL);
    for (; g_match_info_matches(mi); g_match_info_next(mi, NULL)) {
        g_match_info_fetch_pos(mi, 0, &a, &b);
        if (masked(mask, a, a + 1) || masked(mask, b - 1, b))
            continue;
        tag_bytes(ls, text, a + 1, b - 1, T_ITALIC);
        tag_bytes(ls, text, a, a + 1, T_DIM);
        tag_bytes(ls, text, b - 1, b, T_DIM);
    }
    g_clear_pointer(&mi, g_match_info_unref);
}

/* Restyles one line; returns whether the next line starts inside a code block. */
static gboolean md_line(int line, gboolean in_code) {
    JotIter s, e, n;
    line_bounds(line, &s, &e, &n);
    for (int t = 0; t < N_TAGS; t++)
        jb_remove_tag(J.buf, M.tag[t], &s, &n);
    g_autofree char *text = jb_get_slice(J.buf, &s, &e, TRUE);
    if (is_fence(text)) {
        jb_apply_tag(J.buf, M.tag[T_CODEBLOCK], &s, &n);
        jb_apply_tag(J.buf, M.tag[T_DIM], &s, &e);
        return !in_code;
    }
    if (in_code) {
        jb_apply_tag(J.buf, M.tag[T_CODEBLOCK], &s, &n);
        return TRUE;
    }
    if (strstr(text, OBJ_UTF8))
        return FALSE; /* image lines stay as they are */

    GMatchInfo *mi = NULL;
    int a, b, c, d, body = 0;
    if (g_regex_match(M.heading, text, 0, &mi)) {
        g_match_info_fetch_pos(mi, 1, &a, &b);
        g_match_info_fetch_pos(mi, 2, &c, &d);
        tag_bytes(&s, text, a, b, T_DIM);
        tag_bytes(&s, text, c, d, T_HEADING);
        g_match_info_unref(mi);
        md_inline(&s, text, c);
        return FALSE;
    }
    g_clear_pointer(&mi, g_match_info_unref);
    if (g_regex_match(M.rule, text, 0, NULL)) {
        tag_bytes(&s, text, 0, (int)strlen(text), T_DIM);
        return FALSE;
    }
    if (g_regex_match(M.quote, text, 0, &mi)) {
        g_match_info_fetch_pos(mi, 1, &a, &b); /* the > marks */
        g_match_info_fetch_pos(mi, 0, &c, &body);
        tag_bytes(&s, text, a, b, T_QUOTE_MARK);
        tag_bytes(&s, text, body, (int)strlen(text), T_QUOTE);
    }
    g_clear_pointer(&mi, g_match_info_unref);
    if (g_regex_match_full(M.list, text, -1, body, G_REGEX_MATCH_ANCHORED, &mi, NULL)) {
        g_match_info_fetch_pos(mi, 1, &a, &b); /* - * + or 1. 1) */
        g_match_info_fetch_pos(mi, 0, &c, &body);
        tag_bytes(&s, text, a, b, T_BULLET);
        /* - [ ] task / - [x] done */
        const char *p = text + body;
        if (p[0] == '[' && p[1] && strchr(" xX", p[1]) && p[2] == ']' && (p[3] == ' ' || !p[3])) {
            tag_bytes(&s, text, body, body + 3, T_BULLET);
            if (p[1] != ' ')
                tag_bytes(&s, text, body + 3, (int)strlen(text), T_DONE);
            body += 3;
        }
    }
    g_clear_pointer(&mi, g_match_info_unref);
    md_inline(&s, text, body);
    return FALSE;
}

/* ------------------------------------------------------------------------- */
/* Keeping it up to date                                                      */
/* ------------------------------------------------------------------------- */

static gboolean md_flush(gpointer unused) {
    (void)unused;
    M.idle = 0;
    if (!M.enabled || M.first < 0)
        return G_SOURCE_REMOVE;
    int n = jb_get_line_count(J.buf);
    int first = MIN(M.first, n - 1), last = M.to_end ? n - 1 : MIN(M.last, n - 1);
    gboolean in_code = in_code_at(first);
    for (int line = first; line <= last; line++)
        in_code = md_line(line, in_code);
    M.first = M.last = -1;
    M.to_end = FALSE;
    return G_SOURCE_REMOVE;
}

/* Marks lines for restyling; runs before the next redraw, so there's no unstyled flash. */
static void md_dirty(int first, int last, gboolean to_end) {
    if (!M.enabled)
        return;
    M.first = M.first < 0 ? first : MIN(M.first, first);
    M.last = MAX(M.last, last);
    M.to_end |= to_end;
    if (!M.idle)
        M.idle = g_idle_add_full(G_PRIORITY_HIGH_IDLE, md_flush, NULL, NULL);
}

static void on_md_insert(JotBuf *buf, JotIter *end, char *text, int len, gpointer unused) {
    (void)buf, (void)unused;
    int last = jb_iter_get_line(end), first = last;
    for (int i = 0; i < len; i++)
        first -= text[i] == '\n';
    gboolean fence = FALSE;
    for (int l = first; l <= last && !fence; l++)
        fence = line_is_fence(l);
    md_dirty(first, last, fence);
}

static void on_md_delete(JotBuf *buf, JotIter *s, JotIter *e, gpointer unused) {
    (void)buf, (void)unused;
    /* before the delete: did it touch a fence line? */
    gboolean fence = FALSE;
    for (int l = jb_iter_get_line(s); l <= jb_iter_get_line(e) && !fence; l++)
        fence = line_is_fence(l);
    int line = jb_iter_get_line(s);
    md_dirty(line, line, fence);
}

/* After the delete (lines have collapsed): the joined line may now be a fence. */
static void on_md_deleted(JotBuf *buf, JotIter *s, JotIter *e, gpointer unused) {
    (void)buf, (void)e, (void)unused;
    int line = jb_iter_get_line(s);
    md_dirty(line, line, line_is_fence(line));
}

gboolean md_enabled(void) {
    return M.enabled;
}

void md_set_enabled(gboolean on) {
    M.enabled = on;
    JotIter s, e;
    jb_get_bounds(J.buf, &s, &e);
    if (!on) {
        for (int t = 0; t < N_TAGS; t++)
            jb_remove_tag(J.buf, M.tag[t], &s, &e);
        return;
    }
    md_dirty(0, 0, TRUE);
    md_flush(NULL);
}

void md_init(void) {
    static const char *names[N_TAGS] = {"md-codeblock", "md-code",   "md-quote", "md-heading",
                                        "md-bold",      "md-italic", "md-bullet", "md-quote-mark",
                                        "md-link",      "md-done",   "md-dim"};
    for (int t = 0; t < N_TAGS; t++) /* created in priority order */
        M.tag[t] = jb_create_tag(J.buf, names[t]);
    ui_md_style(M.tag);

#define RE(p) g_regex_new(p, G_REGEX_OPTIMIZE, 0, NULL)
    M.heading = RE("^(#{1,6})[ \\t]+(.*\\S)");
    M.rule = RE("^ {0,3}([-*_])(?:[ \\t]*\\1){2,}[ \\t]*$");
    M.quote = RE("^[ \\t]*(>+)[ \\t]?");
    M.list = RE("[ \\t]*([-*+]|\\d{1,9}[.)])[ \\t]+");
    M.code = RE("(`+)(?!`)(.+?)(?<!`)\\1(?!`)");
    M.link = RE("\\[([^\\]]+)\\]\\(([^)\\s]+)\\)");
    M.url = RE("\\bhttps?://[^\\s<>\"'`)\\]]+");
    /* **bold** only: __x__ would catch Python's __init__ and friends */
    M.bold = RE("(?<![\\w*])\\*\\*(?=\\S)(.+?)(?<=\\S)\\*\\*(?![\\w*])");
    M.italic = RE("(?<![\\w*])\\*(?=[^\\s*])(.+?)(?<=[^\\s*])\\*(?![\\w*])|(?<![\\w_])_(?=[^\\s_])(.+?)(?<=[^\\s_])_(?![\\w_])");
#undef RE

    jb_connect_after(J.buf, "insert-text", G_CALLBACK(on_md_insert), NULL);
    jb_connect(J.buf, "delete-range", G_CALLBACK(on_md_delete), NULL);
    jb_connect_after(J.buf, "delete-range", G_CALLBACK(on_md_deleted), NULL);
}

/* ------------------------------------------------------------------------- */
/* Links and headings                                                         */
/* ------------------------------------------------------------------------- */

/* The URL of a [link](url) or bare URL at `it`, or NULL. */
static char *md_url_at(const JotIter *it) {
    JotIter s, e, n;
    line_bounds(jb_iter_get_line(it), &s, &e, &n);
    g_autofree char *text = jb_get_slice(J.buf, &s, &e, TRUE);
    int col = jb_iter_get_line_offset(it);
    GRegex *res[] = {M.link, M.url};
    for (int r = 0; r < 2; r++) {
        GMatchInfo *mi = NULL;
        g_regex_match(res[r], text, 0, &mi);
        for (; g_match_info_matches(mi); g_match_info_next(mi, NULL)) {
            int a, b;
            g_match_info_fetch_pos(mi, 0, &a, &b);
            if (r == 1)
                while (b > a && strchr(".,;:!?", text[b - 1]))
                    b--;
            long ca = g_utf8_pointer_to_offset(text, text + a), cb = g_utf8_pointer_to_offset(text, text + b);
            if (col >= ca && col < cb) {
                char *url = r == 0 ? g_match_info_fetch(mi, 2) : g_strndup(text + a, b - a);
                g_match_info_unref(mi);
                return url;
            }
        }
        g_match_info_unref(mi);
    }
    return NULL;
}

/* Opens the link at `it` in the browser; FALSE if there's none. */
gboolean md_open_url_at(const JotIter *it) {
    g_autofree char *url = md_url_at(it);
    if (!url)
        return FALSE;
    ui_open_uri(url);
    return TRUE;
}

/* The next (dir > 0) or previous heading line from `line`, skipping code blocks; -1 if none. */
int md_heading_line(int line, int dir) {
    int n = jb_get_line_count(J.buf), found = -1;
    gboolean in_code = FALSE;
    for (int l = 0; l < n; l++) {
        if (line_is_fence(l)) {
            in_code = !in_code;
            continue;
        }
        if (in_code || (dir > 0 ? l <= line : l >= line))
            continue;
        JotIter s, e, nx;
        line_bounds(l, &s, &e, &nx);
        if (jb_iter_get_char(&s) != '#')
            continue;
        g_autofree char *text = jb_get_slice(J.buf, &s, &e, TRUE);
        if (!g_regex_match(M.heading, text, 0, NULL))
            continue;
        found = l;
        if (dir > 0)
            break; /* first one after; going backward, keep the last one before */
    }
    return found;
}
