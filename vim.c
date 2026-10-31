/* vim.c - vim-style modal editing for jot.
 *
 * Keys arrive as unichar "tokens": printable characters as themselves; Esc, Enter, Backspace,
 * Tab and Ctrl+letter as their ASCII control codes. Normal/visual mode keys accumulate in
 * V.pending until they form a complete command ("d", "d2", "d2w" -> run).
 *
 * Yank/delete/put use the system clipboard (like clipboard=unnamedplus), so `p` also puts a
 * screenshot. Not implemented: `.` repeat, macros, marks, named registers, block visual.
 */
#include "jot.h"

#include <string.h>

#define K_ESC 0x1b
#define K_CR '\r'
#define K_BS 0x08
#define K_TAB '\t'
#define CTRL(c) ((gunichar)(c) & 0x1f)

typedef enum { MODE_NORMAL, MODE_INSERT, MODE_VISUAL, MODE_VISUAL_LINE } Mode;
typedef enum { R_DONE, R_MORE, R_BAD } Res; /* command complete / needs more keys / invalid */

typedef struct {
    int s, e; /* char offsets, e exclusive */
    gboolean linewise;
} Range;

typedef struct {
    gboolean linewise, inclusive, jk, dollar;
} Motion;

static struct {
    Mode mode;
    GString *pending;
    int want_col;               /* sticky column for j/k: -1 = take it from the cursor, G_MAXINT = $ */
    GtkTextMark *anchor, *vcur; /* visual mode: where it started, and the cursor end */
    char *reg;                  /* last yank/delete, to tell linewise text apart on the clipboard */
    gboolean reg_linewise;
    char *last_search;
    gboolean last_search_fwd;
    gunichar last_f, last_f_cmd; /* for ; and , */
    gboolean in_insert_group;    /* an insert session (incl. the c that started it) is one undo step */
    gboolean search_hl;          /* the selection is only a search-match highlight */
    gboolean search_retry, search_retry_fwd; /* a search failed while images were being read */
    JotImage *hl_image;          /* image whose text matched the search: outlined + words highlighted */
    int vis_first, vis_last;     /* lines of the last visual selection, for :'<,'> */
    /* status line / command line */
    GtkStack *stack;
    GtkLabel *mode_label, *showcmd_label, *ocr_label, *pos_label, *cmd_prefix;
    GtkText *cmd;
    gunichar cmd_kind; /* ':' '/' '?' */
    int search_origin;
    GPtrArray *history;
    guint hist_pos;
    gboolean message_shown;
} V;

static Res vim_exec(const char *keys);

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

static gunichar next_char(const char **p) {
    gunichar c = g_utf8_get_char(*p);
    *p = g_utf8_next_char(*p);
    return c;
}

/* A count: digits not starting with 0 (a lone 0 is the motion). 0 = no count. */
static int parse_count(const char **pp) {
    const char *p = *pp;
    int n = 0;
    if (*p >= '1' && *p <= '9') {
        for (; g_ascii_isdigit(*p); p++)
            n = MIN(n * 10 + (*p - '0'), 99999);
    }
    *pp = p;
    return n;
}

static gboolean is_visual(void) {
    return V.mode == MODE_VISUAL || V.mode == MODE_VISUAL_LINE;
}

static int offset_of(const GtkTextIter *it) {
    return gtk_text_iter_get_offset(it);
}

static void iter_at(GtkTextIter *it, int offset) {
    gtk_text_buffer_get_iter_at_offset(J.buf, it, offset);
}

static int line_count(void) {
    return gtk_text_buffer_get_line_count(J.buf);
}

static void iter_line(GtkTextIter *it, int line) {
    gtk_text_buffer_get_iter_at_line(J.buf, it, CLAMP(line, 0, line_count() - 1));
}

static void to_line_end(GtkTextIter *it) {
    if (!gtk_text_iter_ends_line(it))
        gtk_text_iter_forward_to_line_end(it);
}

static int line_length(int line) {
    GtkTextIter it;
    iter_line(&it, line);
    to_line_end(&it);
    return gtk_text_iter_get_line_offset(&it);
}

static void first_nonblank(GtkTextIter *it) {
    gtk_text_iter_set_line_offset(it, 0);
    while (!gtk_text_iter_ends_line(it) && (gtk_text_iter_get_char(it) == ' ' || gtk_text_iter_get_char(it) == '\t'))
        gtk_text_iter_forward_char(it);
}

/* Line `line`, column `col`; in normal mode the cursor can't sit on the newline. */
static void iter_line_col(GtkTextIter *it, int line, int col) {
    int len = line_length(line);
    int max = (V.mode != MODE_INSERT && len > 0) ? len - 1 : len;
    iter_line(it, line);
    gtk_text_iter_set_line_offset(it, CLAMP(col, 0, max));
}

static void clamp_normal(GtkTextIter *it) {
    if (gtk_text_iter_ends_line(it) && !gtk_text_iter_starts_line(it))
        gtk_text_iter_backward_char(it);
}

/* The vim cursor: the insert mark, or the moving end of a visual selection. */
static void get_cursor(GtkTextIter *it) {
    if (is_visual())
        gtk_text_buffer_get_iter_at_mark(J.buf, it, V.vcur);
    else
        cursor_iter(it);
}

static gboolean smartcase(const char *q) {
    for (const char *p = q; *p; p = g_utf8_next_char(p))
        if (g_unichar_isupper(g_utf8_get_char(p)))
            return TRUE;
    return FALSE;
}

/* ------------------------------------------------------------------------- */
/* Status line                                                                */
/* ------------------------------------------------------------------------- */

static void vim_message(const char *msg, gboolean error) {
    gtk_label_set_text(V.mode_label, msg);
    if (error)
        gtk_widget_add_css_class(GTK_WIDGET(V.mode_label), "error");
    else
        gtk_widget_remove_css_class(GTK_WIDGET(V.mode_label), "error");
    V.message_shown = TRUE;
}

static void update_status(void) {
    if (!V.mode_label)
        return;
    if (!V.message_shown) {
        static const char *names[] = {"", "-- INSERT --", "-- VISUAL --", "-- VISUAL LINE --"};
        gtk_label_set_text(V.mode_label, names[V.mode]);
        gtk_widget_remove_css_class(GTK_WIDGET(V.mode_label), "error");
    }
    GString *sc = g_string_new(NULL);
    for (const char *p = V.pending->str; *p;) {
        gunichar c = next_char(&p);
        if (c < 0x20)
            g_string_append_printf(sc, "^%c", (char)(c + '@'));
        else
            g_string_append_unichar(sc, c);
    }
    gtk_label_set_text(V.showcmd_label, sc->str);
    g_string_free(sc, TRUE);
    GtkTextIter it;
    get_cursor(&it);
    char pos[48];
    g_snprintf(pos, sizeof pos, "%d,%d", gtk_text_iter_get_line(&it) + 1, gtk_text_iter_get_line_offset(&it) + 1);
    gtk_label_set_text(V.pos_label, pos);
}

/* ------------------------------------------------------------------------- */
/* Modes                                                                      */
/* ------------------------------------------------------------------------- */

static void set_mode(Mode m) {
    V.mode = m;
    gtk_text_view_set_overwrite(J.view, m == MODE_NORMAL); /* block cursor */
    update_status();
    update_image_selection(); /* the cursor selects an image in normal mode only */
}

gboolean vim_image_cursor_mode(void) {
    return V.mode == MODE_NORMAL;
}

static void set_cursor(GtkTextIter *it) {
    if (is_visual()) {
        clamp_normal(it);
        gtk_text_buffer_move_mark(J.buf, V.vcur, it);
        GtkTextIter a, c, s, e;
        gtk_text_buffer_get_iter_at_mark(J.buf, &a, V.anchor);
        gtk_text_buffer_get_iter_at_mark(J.buf, &c, V.vcur);
        gboolean fwd = gtk_text_iter_compare(&c, &a) >= 0;
        s = fwd ? a : c;
        e = fwd ? c : a;
        if (V.mode == MODE_VISUAL_LINE) {
            gtk_text_iter_set_line_offset(&s, 0);
            to_line_end(&e);
            gtk_text_iter_forward_char(&e);
        } else if (!gtk_text_iter_is_end(&e)) {
            gtk_text_iter_forward_char(&e); /* inclusive */
        }
        gtk_text_buffer_select_range(J.buf, fwd ? &e : &s, fwd ? &s : &e);
        gtk_text_view_scroll_mark_onscreen(J.view, V.vcur);
    } else {
        if (V.mode == MODE_NORMAL)
            clamp_normal(it);
        gtk_text_buffer_place_cursor(J.buf, it);
        gtk_text_view_scroll_mark_onscreen(J.view, gtk_text_buffer_get_insert(J.buf));
    }
    update_status();
}

static void begin_insert_group(void) {
    if (!V.in_insert_group) {
        gtk_text_buffer_begin_user_action(J.buf);
        V.in_insert_group = TRUE;
    }
}

static void enter_insert(GtkTextIter *at) {
    begin_insert_group();
    gtk_text_buffer_place_cursor(J.buf, at);
    set_mode(MODE_INSERT);
    gtk_text_view_scroll_mark_onscreen(J.view, gtk_text_buffer_get_insert(J.buf));
}

static void leave_insert(void) {
    if (V.in_insert_group) {
        gtk_text_buffer_end_user_action(J.buf);
        V.in_insert_group = FALSE;
    }
    set_mode(MODE_NORMAL);
    GtkTextIter it;
    cursor_iter(&it);
    if (!gtk_text_iter_starts_line(&it))
        gtk_text_iter_backward_char(&it); /* like vim, Esc steps back onto the last typed char */
    set_cursor(&it);
}

static void enter_visual(Mode m, const GtkTextIter *anchor, const GtkTextIter *cur) {
    if (!V.anchor) {
        V.anchor = gtk_text_buffer_create_mark(J.buf, NULL, anchor, TRUE);
        V.vcur = gtk_text_buffer_create_mark(J.buf, NULL, cur, TRUE);
    }
    gtk_text_buffer_move_mark(J.buf, V.anchor, anchor);
    set_mode(m);
    GtkTextIter c = *cur;
    set_cursor(&c);
}

/* Leaves visual mode, putting the cursor at `offset` (or where the visual cursor was if < 0). */
static void exit_visual(int offset) {
    GtkTextIter it, a;
    gtk_text_buffer_get_iter_at_mark(J.buf, &it, V.vcur);
    gtk_text_buffer_get_iter_at_mark(J.buf, &a, V.anchor);
    V.vis_first = MIN(gtk_text_iter_get_line(&it), gtk_text_iter_get_line(&a));
    V.vis_last = MAX(gtk_text_iter_get_line(&it), gtk_text_iter_get_line(&a));
    if (offset >= 0)
        iter_at(&it, offset);
    set_mode(MODE_NORMAL);
    set_cursor(&it);
}

static void visual_range(Range *r) {
    GtkTextIter a, c;
    gtk_text_buffer_get_iter_at_mark(J.buf, &a, V.anchor);
    gtk_text_buffer_get_iter_at_mark(J.buf, &c, V.vcur);
    if (gtk_text_iter_compare(&a, &c) > 0) {
        GtkTextIter t = a;
        a = c;
        c = t;
    }
    if (V.mode == MODE_VISUAL_LINE) {
        gtk_text_iter_set_line_offset(&a, 0);
        to_line_end(&c);
        gtk_text_iter_forward_char(&c);
    } else if (!gtk_text_iter_is_end(&c)) {
        gtk_text_iter_forward_char(&c);
    }
    r->s = offset_of(&a);
    r->e = offset_of(&c);
    r->linewise = V.mode == MODE_VISUAL_LINE;
}

/* ------------------------------------------------------------------------- */
/* Words, characters, brackets                                                */
/* ------------------------------------------------------------------------- */

/* 0 = blank (incl. newline/end), 1 = punctuation (or image, or any non-blank for WORDs), 2 = word char */
static int cls_at(const GtkTextIter *it, gboolean big) {
    gunichar c = gtk_text_iter_get_char(it);
    if (c == 0 || g_unichar_isspace(c))
        return 0;
    if (big)
        return 1;
    return (c == '_' || g_unichar_isalnum(c)) ? 2 : 1;
}

static gboolean empty_line(const GtkTextIter *it) {
    return gtk_text_iter_starts_line(it) && gtk_text_iter_ends_line(it);
}

static void word_fwd(GtkTextIter *it, gboolean big) {
    int c = cls_at(it, big);
    if (c)
        while (!gtk_text_iter_is_end(it) && cls_at(it, big) == c)
            gtk_text_iter_forward_char(it);
    while (!gtk_text_iter_is_end(it) && cls_at(it, big) == 0) {
        gboolean nl = gtk_text_iter_get_char(it) == '\n';
        gtk_text_iter_forward_char(it);
        if (nl && empty_line(it))
            return; /* an empty line counts as a word */
    }
}

static void word_end(GtkTextIter *it, gboolean big) {
    gtk_text_iter_forward_char(it);
    while (!gtk_text_iter_is_end(it) && cls_at(it, big) == 0)
        gtk_text_iter_forward_char(it);
    int c = cls_at(it, big);
    if (!c)
        return;
    GtkTextIter nx = *it;
    while (gtk_text_iter_forward_char(&nx) && cls_at(&nx, big) == c)
        *it = nx;
}

static void word_back(GtkTextIter *it, gboolean big) {
    if (!gtk_text_iter_backward_char(it))
        return;
    while (cls_at(it, big) == 0 && !gtk_text_iter_is_start(it)) {
        if (empty_line(it))
            return;
        gtk_text_iter_backward_char(it);
    }
    int c = cls_at(it, big);
    GtkTextIter pv = *it;
    while (c && gtk_text_iter_backward_char(&pv) && cls_at(&pv, big) == c)
        *it = pv;
}

static gboolean at_word_end(const GtkTextIter *it, gboolean big) {
    GtkTextIter nx = *it;
    gtk_text_iter_forward_char(&nx);
    return cls_at(&nx, big) != cls_at(it, big);
}

/* f/F/t/T: n-th occurrence of ch on this line. */
static gboolean find_char(GtkTextIter *it, gunichar cmd, gunichar ch, int n, gboolean repeat) {
    gboolean fwd = cmd == 'f' || cmd == 't';
    gboolean till = cmd == 't' || cmd == 'T';
    GtkTextIter t = *it;
    if (till && repeat) /* don't get stuck right before the char we stopped at */
        fwd ? gtk_text_iter_forward_char(&t) : gtk_text_iter_backward_char(&t);
    for (int i = 0; i < n; i++) {
        do {
            if (fwd ? (gtk_text_iter_ends_line(&t) || !gtk_text_iter_forward_char(&t))
                    : (gtk_text_iter_starts_line(&t) || !gtk_text_iter_backward_char(&t)))
                return FALSE;
            if (gtk_text_iter_ends_line(&t) && fwd)
                return FALSE;
        } while (gtk_text_iter_get_char(&t) != ch);
    }
    if (till)
        fwd ? gtk_text_iter_backward_char(&t) : gtk_text_iter_forward_char(&t);
    *it = t;
    return TRUE;
}

static gunichar bracket_partner(gunichar c, gboolean *open) {
    static const char pairs[] = "()[]{}<>";
    const char *p = strchr(pairs, (int)c);
    if (!c || !p)
        return 0;
    gsize i = p - pairs;
    *open = i % 2 == 0;
    return (gunichar)pairs[*open ? i + 1 : i - 1];
}

/* From the bracket at `it`, find its partner (scanning with nesting). */
static gboolean match_bracket(GtkTextIter *it) {
    gboolean open;
    gunichar c = gtk_text_iter_get_char(it);
    gunichar partner = bracket_partner(c, &open);
    if (!partner || c == '<' || c == '>')
        return FALSE;
    GtkTextIter t = *it;
    int depth = 0;
    while (open ? gtk_text_iter_forward_char(&t) : gtk_text_iter_backward_char(&t)) {
        gunichar d = gtk_text_iter_get_char(&t);
        if (d == c)
            depth++;
        else if (d == partner && depth-- == 0) {
            *it = t;
            return TRUE;
        }
    }
    return FALSE;
}

/* The unmatched `open` bracket enclosing `it` (or at it). */
static gboolean enclosing_open(GtkTextIter *it, gunichar open, gunichar close) {
    if (gtk_text_iter_get_char(it) == open)
        return TRUE;
    GtkTextIter t = *it;
    int depth = 0;
    while (gtk_text_iter_backward_char(&t)) {
        gunichar d = gtk_text_iter_get_char(&t);
        if (d == close)
            depth++;
        else if (d == open && depth-- == 0) {
            *it = t;
            return TRUE;
        }
    }
    return FALSE;
}

/* ------------------------------------------------------------------------- */
/* Search                                                                     */
/* ------------------------------------------------------------------------- */

/* count-th match of q after (forward) or before `start`. */
static gboolean do_search(const char *q, gboolean fwd, int count, const GtkTextIter *start, Match *m) {
    GtkTextIter from = *start;
    gboolean wrapped = FALSE;
    for (int i = 0; i < MAX(count, 1); i++) {
        GtkTextIter f = from;
        if (fwd && !gtk_text_iter_is_end(&f))
            gtk_text_iter_forward_char(&f);
        if (i)
            g_clear_pointer(&m->line, g_free);
        if (!find_match(q, smartcase(q), &f, fwd, m))
            return FALSE;
        wrapped |= m->wrapped;
        from = m->start;
    }
    m->wrapped = wrapped;
    return TRUE;
}

/* Moves to a match: the cursor goes to its start and (outside visual mode) the match is
 * highlighted by selecting it until the next key. */
/* An image match isn't selected (GTK would tint the whole image with the selection color); the
 * cursor goes on the image and jot outlines it and highlights the matching words itself. */
static void clear_image_mark(void) {
    if (V.hl_image) {
        image_set_highlight(V.hl_image, NULL);
        g_clear_object(&V.hl_image);
    }
}

/* The cursor goes on the image (which outlines it); the matching words get highlighted. */
static void mark_image(JotImage *img, const GtkTextIter *at, const char *q) {
    clear_image_mark();
    gtk_text_buffer_place_cursor(J.buf, at);
    V.hl_image = g_object_ref(img);
    image_set_highlight(img, q);
}

static void show_match(Match *m, const char *q) {
    if (is_visual()) {
        set_cursor(&m->start);
    } else {
        if (m->img)
            mark_image(m->img, &m->start, q);
        else
            gtk_text_buffer_select_range(J.buf, &m->start, &m->end);
        V.search_hl = TRUE;
        gtk_text_view_scroll_mark_onscreen(J.view, gtk_text_buffer_get_insert(J.buf));
        update_status();
    }
    gtk_text_view_scroll_to_iter(J.view, &m->start, 0.1, FALSE, 0, 0);
    if (m->img) {
        /* OCR keeps runs of spaces (indentation, columns); squeeze them for the status line */
        g_autoptr(GRegex) spaces = g_regex_new("\\s{2,}", 0, 0, NULL);
        g_autofree char *line = g_regex_replace_literal(spaces, m->line, -1, 0, "  ", 0, NULL);
        g_autofree char *msg = g_strdup_printf("in image: %s", line);
        vim_message(msg, FALSE);
    } else if (m->wrapped) {
        vim_message(V.last_search_fwd ? "search hit BOTTOM, continuing at TOP" : "search hit TOP, continuing at BOTTOM",
                    TRUE);
    }
    g_clear_pointer(&m->line, g_free);
}

static void search_jump(gboolean fwd, int count) {
    if (!V.last_search) {
        vim_message("E35: No previous regular expression", TRUE);
        return;
    }
    GtkTextIter cur;
    get_cursor(&cur);
    Match m = {0};
    if (!do_search(V.last_search, fwd, count, &cur, &m)) {
        guint reading = ocr_pending_count();
        g_autofree char *msg = reading ? g_strdup_printf("E486: Pattern not found: %s (still indexing %u image%s)",
                                                         V.last_search, reading, reading == 1 ? "" : "s")
                                       : g_strdup_printf("E486: Pattern not found: %s", V.last_search);
        vim_message(msg, TRUE);
        V.search_retry = reading > 0; /* it may be in an image that's still being read */
        V.search_retry_fwd = fwd;
        return;
    }
    show_match(&m, V.last_search);
}

/* The word (or WORD-ish non-blank run) under the cursor, for * and #. */
static char *word_under_cursor(void) {
    GtkTextIter s, e;
    get_cursor(&s);
    while (!gtk_text_iter_ends_line(&s) && cls_at(&s, FALSE) != 2)
        gtk_text_iter_forward_char(&s);
    if (cls_at(&s, FALSE) != 2)
        return NULL;
    e = s;
    GtkTextIter pv = s;
    while (gtk_text_iter_backward_char(&pv) && cls_at(&pv, FALSE) == 2)
        s = pv;
    while (!gtk_text_iter_ends_line(&e) && cls_at(&e, FALSE) == 2)
        gtk_text_iter_forward_char(&e);
    return gtk_text_buffer_get_text(J.buf, &s, &e, FALSE);
}

/* ------------------------------------------------------------------------- */
/* Motions and text objects                                                   */
/* ------------------------------------------------------------------------- */

/* Moves `it` by the motion at *pp. `count` is 0 when none was typed. `op` is the pending
 * operator (d, c, y, <, >) or 0. */
static Res motion(const char **pp, int count, gunichar op, GtkTextIter *it, Motion *mi) {
    const char *p = *pp;
    if (!*p)
        return R_MORE;
    gunichar c = next_char(&p);
    int n = MAX(count, 1);
    GtkTextIter start = *it;
    memset(mi, 0, sizeof *mi);
    switch (c) {
    case 'h':
    case K_BS: /* also Ctrl+H */
        for (int i = 0; i < n && !gtk_text_iter_starts_line(it); i++)
            gtk_text_iter_backward_char(it);
        break;
    case 'l':
    case ' ':
        for (int i = 0; i < n && !gtk_text_iter_ends_line(it); i++)
            gtk_text_iter_forward_char(it);
        break;
    case 'j':
    case 'k':
    case CTRL('n'):
    case CTRL('p'):
    case '+':
    case '-':
    case K_CR:
    case '_': {
        int dir = (c == 'k' || c == '-' || c == CTRL('p')) ? -1 : 1;
        int cur = gtk_text_iter_get_line(it);
        int line = c == '_' ? cur + n - 1 : cur + dir * n;
        if (line < 0 || line >= line_count())
            return R_BAD;
        if (c == 'j' || c == 'k' || c == CTRL('n') || c == CTRL('p')) {
            if (V.want_col < 0)
                V.want_col = gtk_text_iter_get_line_offset(it);
            iter_line_col(it, line, V.want_col);
            mi->jk = TRUE;
        } else {
            iter_line(it, line);
            first_nonblank(it);
        }
        mi->linewise = TRUE;
        break;
    }
    case '0':
        gtk_text_iter_set_line_offset(it, 0);
        break;
    case '^':
        first_nonblank(it);
        break;
    case '$':
        if (n > 1)
            iter_line(it, gtk_text_iter_get_line(it) + n - 1);
        to_line_end(it);
        if (!gtk_text_iter_starts_line(it)) {
            gtk_text_iter_backward_char(it);
            mi->inclusive = TRUE;
        }
        mi->dollar = TRUE;
        break;
    case 'w':
    case 'W':
        for (int i = 0; i < n; i++)
            word_fwd(it, c == 'W');
        /* dw on the last word of a line stops at the line end, not the next line's word. */
        if (op && gtk_text_iter_get_line(it) > gtk_text_iter_get_line(&start) && gtk_text_iter_starts_line(it) &&
            !gtk_text_iter_is_end(it))
            gtk_text_iter_backward_char(it);
        break;
    case 'e':
    case 'E':
        for (int i = 0; i < n; i++)
            word_end(it, c == 'E');
        mi->inclusive = TRUE;
        break;
    case 'b':
    case 'B':
        for (int i = 0; i < n; i++)
            word_back(it, c == 'B');
        break;
    case 'G':
        iter_line(it, count ? count - 1 : line_count() - 1);
        first_nonblank(it);
        mi->linewise = TRUE;
        break;
    case 'g': {
        if (!*p)
            return R_MORE;
        gunichar d = next_char(&p);
        if (d == 'g') {
            iter_line(it, count ? count - 1 : 0);
            first_nonblank(it);
            mi->linewise = TRUE;
        } else if (d == 'j' || d == 'k') {
            const char *jk = d == 'j' ? "j" : "k";
            Res r = motion(&jk, count, op, it, mi);
            if (r != R_DONE)
                return r;
        } else if (d == '_') {
            iter_line(it, gtk_text_iter_get_line(it) + n - 1);
            to_line_end(it);
            while (!gtk_text_iter_starts_line(it)) {
                gtk_text_iter_backward_char(it);
                if (cls_at(it, TRUE))
                    break;
            }
            mi->inclusive = TRUE;
        } else {
            return R_BAD;
        }
        break;
    }
    case 'f':
    case 'F':
    case 't':
    case 'T': {
        if (!*p)
            return R_MORE;
        gunichar t = next_char(&p);
        if (t == K_ESC || !find_char(it, c, t, n, FALSE))
            return R_BAD;
        V.last_f = t;
        V.last_f_cmd = c;
        mi->inclusive = c == 'f' || c == 't';
        break;
    }
    case ';':
    case ',': {
        if (!V.last_f)
            return R_BAD;
        gunichar cmd = V.last_f_cmd;
        if (c == ',')
            cmd = cmd == 'f' ? 'F' : cmd == 'F' ? 'f' : cmd == 't' ? 'T' : 't';
        if (!find_char(it, cmd, V.last_f, n, TRUE))
            return R_BAD;
        mi->inclusive = cmd == 'f' || cmd == 't';
        break;
    }
    case '}':
    case '{': {
        gboolean fwd = c == '}';
        for (int i = 0; i < n; i++) {
            int line = gtk_text_iter_get_line(it), last = line_count() - 1;
            /* skip blank lines we're on, then find the next blank line */
            while ((fwd ? line < last : line > 0) && line_length(line) == 0)
                line += fwd ? 1 : -1;
            while ((fwd ? line < last : line > 0) && line_length(line) != 0)
                line += fwd ? 1 : -1;
            iter_line(it, line);
            if (fwd && line == last)
                to_line_end(it);
        }
        break;
    }
    case ']':
    case '[': { /* ]] [[: next / previous Markdown heading */
        if (!*p)
            return R_MORE;
        if (next_char(&p) != c)
            return R_BAD;
        for (int i = 0; i < n; i++) {
            int line = md_heading_line(gtk_text_iter_get_line(it), c == ']' ? 1 : -1);
            if (line < 0) { /* none: like vim, go to the end / start */
                if (c == ']')
                    gtk_text_buffer_get_end_iter(J.buf, it);
                else
                    gtk_text_buffer_get_start_iter(J.buf, it);
                gtk_text_iter_set_line_offset(it, 0);
                break;
            }
            iter_line(it, line);
        }
        break;
    }
    case '%': {
        GtkTextIter t = *it;
        gboolean open;
        while (!gtk_text_iter_ends_line(&t) && !bracket_partner(gtk_text_iter_get_char(&t), &open))
            gtk_text_iter_forward_char(&t);
        if (!match_bracket(&t))
            return R_BAD;
        *it = t;
        mi->inclusive = TRUE;
        break;
    }
    case 'H':
    case 'M':
    case 'L': {
        GdkRectangle vis;
        gtk_text_view_get_visible_rect(J.view, &vis);
        GtkTextIter top, bot;
        gtk_text_view_get_line_at_y(J.view, &top, vis.y, NULL);
        gtk_text_view_get_line_at_y(J.view, &bot, vis.y + vis.height - 1, NULL);
        int t = gtk_text_iter_get_line(&top), b = gtk_text_iter_get_line(&bot);
        int line = c == 'H' ? MIN(t + n - 1, b) : c == 'L' ? MAX(b - n + 1, t) : (t + b) / 2;
        iter_line(it, line);
        first_nonblank(it);
        mi->linewise = TRUE;
        break;
    }
    case 'n':
    case 'N': {
        if (!V.last_search)
            return R_BAD;
        Match m = {0};
        if (!do_search(V.last_search, (c == 'n') == V.last_search_fwd, n, it, &m))
            return R_BAD;
        *it = m.start;
        g_free(m.line);
        break;
    }
    default:
        return R_BAD;
    }
    *pp = p;
    return R_DONE;
}

static void make_range(GtkTextIter a, GtkTextIter b, const Motion *mi, Range *r) {
    if (gtk_text_iter_compare(&a, &b) > 0) {
        GtkTextIter t = a;
        a = b;
        b = t;
    }
    if (mi->linewise) {
        gtk_text_iter_set_line_offset(&a, 0);
        to_line_end(&b);
        gtk_text_iter_forward_char(&b); /* the newline (no-op at the end) */
    } else if (mi->inclusive && !gtk_text_iter_is_end(&b)) {
        gtk_text_iter_forward_char(&b);
    }
    r->s = offset_of(&a);
    r->e = offset_of(&b);
    r->linewise = mi->linewise;
}

/* Chars of the cursor's line, for quote objects. */
static gunichar *line_chars(int line, glong *len) {
    GtkTextIter s, e;
    iter_line(&s, line);
    e = s;
    to_line_end(&e);
    g_autofree char *text = gtk_text_buffer_get_slice(J.buf, &s, &e, TRUE);
    return g_utf8_to_ucs4_fast(text, -1, len);
}

/* iw aw iW aW i" a" i' a' i` a` i( a( ib ab i[ a[ i{ a{ iB aB i< a< ip ap. *pp is after the i/a. */
static Res text_object(const char **pp, gunichar ia, Range *r) {
    if (!**pp)
        return R_MORE;
    gunichar o = next_char(pp);
    GtkTextIter cur, s, e;
    get_cursor(&cur);
    s = e = cur;
    r->linewise = FALSE;
    switch (o) {
    case 'w':
    case 'W': {
        gboolean big = o == 'W';
        if (empty_line(&cur))
            return R_BAD;
        int c = cls_at(&cur, big);
        GtkTextIter pv = s;
        while (!gtk_text_iter_starts_line(&s) && gtk_text_iter_backward_char(&pv) && cls_at(&pv, big) == c &&
               gtk_text_iter_get_char(&pv) != '\n')
            s = pv;
        while (!gtk_text_iter_ends_line(&e) && cls_at(&e, big) == c)
            gtk_text_iter_forward_char(&e);
        if (ia == 'a') {
            if (c) { /* plus trailing blanks, or leading ones if there are none */
                GtkTextIter t = e;
                while (!gtk_text_iter_ends_line(&t) && cls_at(&t, big) == 0)
                    gtk_text_iter_forward_char(&t);
                if (!gtk_text_iter_equal(&t, &e)) {
                    e = t;
                } else {
                    pv = s;
                    while (!gtk_text_iter_starts_line(&s) && gtk_text_iter_backward_char(&pv) && cls_at(&pv, big) == 0)
                        s = pv;
                }
            } else { /* blanks plus the following word */
                int c2 = cls_at(&e, big);
                while (c2 && !gtk_text_iter_ends_line(&e) && cls_at(&e, big) == c2)
                    gtk_text_iter_forward_char(&e);
            }
        }
        break;
    }
    case '"':
    case '\'':
    case '`': {
        glong len;
        g_autofree gunichar *chars = line_chars(gtk_text_iter_get_line(&cur), &len);
        int col = gtk_text_iter_get_line_offset(&cur), qa = -1, qb = -1;
        int pos[512], np = 0;
        for (glong i = 0; i < len && np < 512; i++)
            if (chars[i] == o && (i == 0 || chars[i - 1] != '\\'))
                pos[np++] = (int)i;
        for (int i = 0; i + 1 < np; i += 2) {
            if ((pos[i] <= col && col <= pos[i + 1]) || col < pos[i]) {
                qa = pos[i];
                qb = pos[i + 1];
                break;
            }
        }
        if (qa < 0)
            return R_BAD;
        gtk_text_iter_set_line_offset(&s, ia == 'i' ? qa + 1 : qa);
        gtk_text_iter_set_line_offset(&e, ia == 'i' ? qb : qb + 1);
        break;
    }
    case '(':
    case ')':
    case 'b':
    case '[':
    case ']':
    case '{':
    case '}':
    case 'B':
    case '<':
    case '>': {
        gunichar open = (o == '(' || o == ')' || o == 'b')   ? '('
                        : (o == '[' || o == ']')             ? '['
                        : (o == '{' || o == '}' || o == 'B') ? '{'
                                                             : '<';
        gboolean is_open;
        gunichar close = bracket_partner(open, &is_open);
        if (!enclosing_open(&s, open, close))
            return R_BAD;
        e = s;
        int depth = 0;
        gboolean found = FALSE;
        while (gtk_text_iter_forward_char(&e)) {
            gunichar d = gtk_text_iter_get_char(&e);
            if (d == open)
                depth++;
            else if (d == close && depth-- == 0) {
                found = TRUE;
                break;
            }
        }
        if (!found)
            return R_BAD;
        if (ia == 'i')
            gtk_text_iter_forward_char(&s);
        else
            gtk_text_iter_forward_char(&e);
        break;
    }
    case 'p': {
        int line = gtk_text_iter_get_line(&cur), first = line, last = line;
        gboolean blank = line_length(line) == 0;
        while (first > 0 && (line_length(first - 1) == 0) == blank)
            first--;
        while (last < line_count() - 1 && (line_length(last + 1) == 0) == blank)
            last++;
        if (ia == 'a')
            while (last < line_count() - 1 && (line_length(last + 1) == 0) != blank)
                last++;
        iter_line(&s, first);
        iter_line(&e, last);
        to_line_end(&e);
        gtk_text_iter_forward_char(&e);
        r->linewise = TRUE;
        break;
    }
    default:
        return R_BAD;
    }
    r->s = offset_of(&s);
    r->e = offset_of(&e);
    return r->e >= r->s ? R_DONE : R_BAD;
}

/* ------------------------------------------------------------------------- */
/* Operators                                                                  */
/* ------------------------------------------------------------------------- */

static void yank_range(const Range *r) {
    GtkTextIter s, e;
    iter_at(&s, r->s);
    iter_at(&e, r->e);
    char *text = serialize_range(J.buf, &s, &e);
    if (r->linewise && !g_str_has_suffix(text, "\n")) {
        char *t = g_strconcat(text, "\n", NULL);
        g_free(text);
        text = t;
    }
    g_free(V.reg);
    V.reg = text;
    V.reg_linewise = r->linewise;
    GdkPaintable *p = gtk_text_iter_get_paintable(&s);
    gboolean lone = JOT_IS_IMAGE(p) && r->e - r->s <= (r->linewise ? 2 : 1);
    clipboard_set(text, lone ? JOT_IMAGE(p) : NULL);
}

static void delete_range(const Range *r, gboolean yank) {
    if (yank)
        yank_range(r);
    GtkTextIter s, e;
    iter_at(&s, r->s);
    iter_at(&e, r->e);
    /* Deleting the last lines also takes the newline before them. */
    if (r->linewise && gtk_text_iter_is_end(&e) && !gtk_text_iter_is_start(&s) && !gtk_text_iter_starts_line(&e))
        gtk_text_iter_backward_char(&s);
    else if (r->linewise && gtk_text_iter_is_end(&e) && !gtk_text_iter_is_start(&s) &&
             gtk_text_iter_get_char(&e) == 0 && r->e > r->s) {
        GtkTextIter pv = e;
        gtk_text_iter_backward_char(&pv);
        if (gtk_text_iter_get_char(&pv) != '\n')
            gtk_text_iter_backward_char(&s);
    }
    gtk_text_buffer_delete(J.buf, &s, &e);
}

static void set_case(const Range *r, gunichar how) {
    GtkTextIter s, e;
    iter_at(&s, r->s);
    iter_at(&e, r->e);
    g_autofree char *text = gtk_text_buffer_get_slice(J.buf, &s, &e, TRUE);
    if (strstr(text, OBJ_UTF8))
        return;
    GString *out = g_string_new(NULL);
    for (const char *p = text; *p;) {
        gunichar c = next_char(&p);
        if (how == 'u')
            c = g_unichar_tolower(c);
        else if (how == 'U')
            c = g_unichar_toupper(c);
        else
            c = g_unichar_isupper(c) ? g_unichar_tolower(c) : g_unichar_toupper(c);
        g_string_append_unichar(out, c);
    }
    gtk_text_buffer_begin_user_action(J.buf);
    gtk_text_buffer_delete(J.buf, &s, &e);
    gtk_text_buffer_insert(J.buf, &s, out->str, (int)out->len);
    gtk_text_buffer_end_user_action(J.buf);
    g_string_free(out, TRUE);
}

static void apply_op(gunichar op, Range *r) {
    GtkTextIter it, s, e;
    iter_at(&s, r->s);
    iter_at(&e, r->e);
    int first = gtk_text_iter_get_line(&s);
    int last = gtk_text_iter_get_line(&e);
    if (r->e > r->s && gtk_text_iter_starts_line(&e) && last > first)
        last--;
    switch (op) {
    case 'y': {
        yank_range(r);
        GtkTextIter cur;
        get_cursor(&cur);
        if (!r->linewise || gtk_text_iter_get_line(&cur) > first) {
            it = s;
            if (r->linewise)
                iter_line_col(&it, first, gtk_text_iter_get_line_offset(&cur));
            set_cursor(&it);
        }
        if (r->linewise && last > first + 1) {
            g_autofree char *msg = g_strdup_printf("%d lines yanked", last - first + 1);
            vim_message(msg, FALSE);
        }
        break;
    }
    case 'd':
        gtk_text_buffer_begin_user_action(J.buf);
        delete_range(r, TRUE);
        gtk_text_buffer_end_user_action(J.buf);
        if (r->linewise) {
            iter_line(&it, first);
            first_nonblank(&it);
        } else {
            iter_at(&it, r->s);
        }
        set_cursor(&it);
        break;
    case 'c': {
        begin_insert_group();
        Range d = *r;
        if (r->linewise) { /* cc / cj: keep one line, and its indent */
            yank_range(r);
            iter_line(&s, first);
            first_nonblank(&s);
            iter_line(&e, last);
            to_line_end(&e);
            d.s = offset_of(&s);
            d.e = offset_of(&e);
            d.linewise = FALSE;
            delete_range(&d, FALSE);
        } else {
            delete_range(&d, TRUE);
        }
        iter_at(&it, d.s);
        enter_insert(&it);
        break;
    }
    case '>':
    case '<':
        indent_range(first, last, op == '>' ? 1 : -1);
        iter_line(&it, first);
        first_nonblank(&it);
        set_cursor(&it);
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Put (p, P)                                                                 */
/* ------------------------------------------------------------------------- */

typedef struct {
    gboolean after;
    int count;
    int replace_s, replace_e; /* visual p: the selection to replace, else -1 */
    gboolean replace_linewise;
} PutReq;

static void put_text(PutReq *pr, const char *text, gboolean linewise) {
    GString *t = g_string_new(NULL);
    for (int i = 0; i < MAX(pr->count, 1); i++)
        g_string_append(t, text);
    gtk_text_buffer_begin_user_action(J.buf);
    GtkTextIter cur, at;
    if (pr->replace_s >= 0) {
        Range r = {pr->replace_s, pr->replace_e, pr->replace_linewise};
        delete_range(&r, FALSE);
        iter_at(&cur, MIN(pr->replace_s, gtk_text_buffer_get_char_count(J.buf)));
        if (pr->replace_linewise && !linewise)
            g_string_append_c(t, '\n');
        if (!pr->replace_linewise && linewise && !gtk_text_iter_starts_line(&cur))
            g_string_prepend_c(t, '\n');
        linewise = linewise || pr->replace_linewise;
        if (pr->replace_linewise && !gtk_text_iter_starts_line(&cur)) {
            gtk_text_buffer_insert(J.buf, &cur, "\n", 1);
            if (g_str_has_suffix(t->str, "\n"))
                g_string_truncate(t, t->len - 1);
        }
        at = cur;
        int off = offset_of(&at);
        insert_text(J.buf, &at, t->str);
        iter_at(&cur, off);
        if (linewise)
            first_nonblank(&cur);
    } else if (linewise) {
        if (!g_str_has_suffix(t->str, "\n"))
            g_string_append_c(t, '\n');
        get_cursor(&cur);
        at = cur;
        if (pr->after) {
            if (!gtk_text_iter_forward_line(&at) && !gtk_text_iter_starts_line(&at)) {
                /* after the last line, which has no newline yet */
                gtk_text_buffer_insert(J.buf, &at, "\n", 1);
                g_string_truncate(t, t->len - 1);
            }
        } else {
            gtk_text_iter_set_line_offset(&at, 0);
        }
        int off = offset_of(&at);
        insert_text(J.buf, &at, t->str);
        iter_at(&cur, off);
        first_nonblank(&cur);
    } else {
        get_cursor(&at);
        if (pr->after && !gtk_text_iter_ends_line(&at))
            gtk_text_iter_forward_char(&at);
        insert_text(J.buf, &at, t->str);
        cur = at;
        gtk_text_iter_backward_char(&cur);
    }
    gtk_text_buffer_end_user_action(J.buf);
    if (is_visual())
        exit_visual(offset_of(&cur));
    else
        set_cursor(&cur);
    after_insert();
    g_string_free(t, TRUE);
}

static void put_text_done(GObject *src, GAsyncResult *res, gpointer data) {
    PutReq *pr = data;
    g_autofree char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, NULL);
    if (text && *text) {
        gboolean linewise = (V.reg && g_str_equal(text, V.reg)) ? V.reg_linewise : g_str_has_suffix(text, "\n");
        put_text(pr, text, linewise);
    }
    g_free(pr);
}

static void put_image_done(GObject *src, GAsyncResult *res, gpointer data) {
    PutReq *pr = data;
    g_autoptr(GdkTexture) tex = gdk_clipboard_read_texture_finish(GDK_CLIPBOARD(src), res, NULL);
    char *rel = tex ? save_texture(tex) : NULL;
    if (rel) {
        if (pr->replace_s >= 0) {
            Range r = {pr->replace_s, pr->replace_e, pr->replace_linewise};
            gtk_text_buffer_begin_user_action(J.buf);
            delete_range(&r, FALSE);
            gtk_text_buffer_end_user_action(J.buf);
            exit_visual(MIN(pr->replace_s, gtk_text_buffer_get_char_count(J.buf)));
        }
        /* Images are line-level: put it on its own line below (p) or above (P). */
        GtkTextIter at;
        cursor_iter(&at);
        if (pr->after && pr->replace_s < 0) {
            if (!gtk_text_iter_forward_line(&at))
                gtk_text_buffer_get_end_iter(J.buf, &at);
        } else {
            gtk_text_iter_set_line_offset(&at, 0);
        }
        g_autoptr(GPtrArray) rels = g_ptr_array_new_with_free_func(g_free);
        for (int i = 0; i < MAX(pr->count, 1); i++)
            g_ptr_array_add(rels, g_strdup(rel));
        insert_images(rels, &at);
        GtkTextIter it;
        cursor_iter(&it);
        set_cursor(&it);
        g_free(rel);
    } else {
        vim_message("Couldn't read image from clipboard", TRUE);
    }
    g_free(pr);
}

static void put(gboolean after, int count) {
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
    PutReq *pr = g_new0(PutReq, 1);
    pr->after = after;
    pr->count = count;
    pr->replace_s = pr->replace_e = -1;
    if (is_visual()) {
        Range r;
        visual_range(&r);
        pr->replace_s = r.s;
        pr->replace_e = r.e;
        pr->replace_linewise = r.linewise;
    }
    if (has_text) {
        gdk_clipboard_read_text_async(cb, NULL, put_text_done, pr);
    } else if (has_image) {
        gdk_clipboard_read_texture_async(cb, NULL, put_image_done, pr);
    } else {
        g_free(pr);
        vim_message("E353: Nothing in register +", TRUE);
    }
}

/* ------------------------------------------------------------------------- */
/* Other normal-mode commands                                                 */
/* ------------------------------------------------------------------------- */

static void open_line(gboolean below) {
    GtkTextIter it, ls;
    cursor_iter(&it);
    ls = it;
    first_nonblank(&ls);
    GtkTextIter l0 = ls;
    gtk_text_iter_set_line_offset(&l0, 0);
    g_autofree char *indent = gtk_text_buffer_get_text(J.buf, &l0, &ls, FALSE);
    if (gtk_text_iter_get_char(&ls) == 0xFFFC)
        indent[0] = '\0';
    begin_insert_group();
    if (below) {
        to_line_end(&it);
        g_autofree char *ins = g_strconcat("\n", indent, NULL);
        gtk_text_buffer_insert(J.buf, &it, ins, -1);
    } else {
        gtk_text_iter_set_line_offset(&it, 0);
        g_autofree char *ins = g_strconcat(indent, "\n", NULL);
        gtk_text_buffer_insert(J.buf, &it, ins, -1);
        gtk_text_iter_backward_char(&it);
    }
    enter_insert(&it);
}

/* J: joins `joins` times at `line`, with a space unless the next line starts with ')'. */
static gboolean join_lines(int line, int joins) {
    gboolean any = FALSE;
    int join_at = 0;
    gtk_text_buffer_begin_user_action(J.buf);
    GtkTextIter e;
    for (int i = 0; i < joins && line < line_count() - 1; i++) {
        iter_line(&e, line);
        to_line_end(&e);
        GtkTextIter t = e;
        gtk_text_iter_forward_char(&t);
        while (!gtk_text_iter_ends_line(&t) && (gtk_text_iter_get_char(&t) == ' ' || gtk_text_iter_get_char(&t) == '\t'))
            gtk_text_iter_forward_char(&t);
        gboolean space = !gtk_text_iter_starts_line(&e) && !gtk_text_iter_ends_line(&t) &&
                         gtk_text_iter_get_char(&t) != ')';
        GtkTextIter pv = e;
        if (space && gtk_text_iter_backward_char(&pv) && g_unichar_isspace(gtk_text_iter_get_char(&pv)))
            space = FALSE;
        gtk_text_buffer_delete(J.buf, &e, &t);
        join_at = offset_of(&e); /* the cursor ends up at the (last) join */
        if (space)
            gtk_text_buffer_insert(J.buf, &e, " ", 1);
        any = TRUE;
    }
    gtk_text_buffer_end_user_action(J.buf);
    if (any) {
        iter_at(&e, join_at);
        set_cursor(&e);
    }
    return any;
}

/* Ctrl+D/U (half page) and Ctrl+F/B (page): move the cursor and scroll with it. */
static void page_move(int dir, gboolean half, int count) {
    GdkRectangle vis;
    gtk_text_view_get_visible_rect(J.view, &vis);
    GtkTextIter top, bot, cur;
    gtk_text_view_get_line_at_y(J.view, &top, vis.y, NULL);
    gtk_text_view_get_line_at_y(J.view, &bot, vis.y + vis.height - 1, NULL);
    int page = MAX(1, gtk_text_iter_get_line(&bot) - gtk_text_iter_get_line(&top));
    int lines = (half ? MAX(1, page / 2) : MAX(1, page - 2)) * MAX(count, 1);
    get_cursor(&cur);
    int y, h;
    gtk_text_view_get_line_yrange(J.view, &cur, &y, &h);
    double rel = CLAMP((double)(y - vis.y) / MAX(vis.height, 1), 0.0, 1.0);
    if (V.want_col < 0)
        V.want_col = gtk_text_iter_get_line_offset(&cur);
    int line = CLAMP(gtk_text_iter_get_line(&cur) + dir * lines, 0, line_count() - 1);
    GtkTextIter it;
    iter_line_col(&it, line, V.want_col);
    set_cursor(&it);
    GtkTextMark *m = is_visual() ? V.vcur : gtk_text_buffer_get_insert(J.buf);
    gtk_text_view_scroll_to_mark(J.view, m, 0, TRUE, 0, half ? rel : (dir > 0 ? 0.0 : 1.0));
}

static void scroll_lines(int dir, int count) {
    GtkAdjustment *adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(J.view));
    GtkTextIter cur;
    get_cursor(&cur);
    int y, h;
    gtk_text_view_get_line_yrange(J.view, &cur, &y, &h);
    double step = MAX(h, 16) * MAX(count, 1);
    gtk_adjustment_set_value(adj, gtk_adjustment_get_value(adj) + dir * step);
}

/* ------------------------------------------------------------------------- */
/* The command line (: / ?)                                                   */
/* ------------------------------------------------------------------------- */

static void cmdline_open(gunichar kind, const char *prefill) {
    V.cmd_kind = kind;
    char pre[8] = {0};
    g_unichar_to_utf8(kind, pre);
    gtk_label_set_text(V.cmd_prefix, pre);
    gtk_editable_set_text(GTK_EDITABLE(V.cmd), prefill ? prefill : "");
    GtkTextIter it;
    get_cursor(&it);
    V.search_origin = offset_of(&it);
    V.hist_pos = V.history->len;
    gtk_stack_set_visible_child_name(V.stack, "cmd");
    gtk_widget_grab_focus(GTK_WIDGET(V.cmd));
    gtk_editable_set_position(GTK_EDITABLE(V.cmd), -1);
}

static gboolean cmdline_is_open(void) {
    return g_str_equal(gtk_stack_get_visible_child_name(V.stack), "cmd");
}

static void cmdline_close(void) {
    gtk_stack_set_visible_child_name(V.stack, "status");
    gtk_widget_grab_focus(GTK_WIDGET(J.view));
    update_status();
}

static void restore_origin(void) {
    clear_image_mark();
    GtkTextIter it;
    iter_at(&it, V.search_origin);
    if (is_visual())
        set_cursor(&it);
    else
        gtk_text_buffer_place_cursor(J.buf, &it);
}

static void cmdline_cancel(void) {
    if (V.cmd_kind != ':')
        restore_origin();
    cmdline_close();
}

/* Incremental search: highlight the match as the pattern is typed. */
static void on_cmd_changed(GtkEditable *e, gpointer unused) {
    (void)unused;
    if (V.cmd_kind == ':' || !cmdline_is_open())
        return;
    const char *q = gtk_editable_get_text(e);
    GtkTextIter from;
    iter_at(&from, V.search_origin);
    Match m = {0};
    if (*q && do_search(q, V.cmd_kind == '/', 1, &from, &m)) {
        clear_image_mark();
        if (is_visual())
            set_cursor(&m.start);
        else if (m.img)
            mark_image(m.img, &m.start, q);
        else
            gtk_text_buffer_select_range(J.buf, &m.start, &m.end);
        gtk_text_view_scroll_to_iter(J.view, &m.start, 0.1, FALSE, 0, 0);
        g_free(m.line);
    } else {
        restore_origin();
    }
}

static void search_commit(const char *q, gboolean fwd) {
    if (*q) {
        g_free(V.last_search);
        V.last_search = g_strdup(q);
    }
    V.last_search_fwd = fwd;
    restore_origin();
    search_jump(fwd, 1);
}

static void ex_command(const char *raw);

static void on_cmd_activate(GtkText *t, gpointer unused) {
    (void)unused;
    g_autofree char *text = g_strdup(gtk_editable_get_text(GTK_EDITABLE(t)));
    gunichar kind = V.cmd_kind;
    if (*text) {
        char pre[8] = {0};
        g_unichar_to_utf8(kind, pre);
        g_ptr_array_add(V.history, g_strconcat(pre, text, NULL));
    }
    cmdline_close();
    if (kind == ':')
        ex_command(text);
    else
        search_commit(text, kind == '/');
}

/* Up/Down: history of the same kind (: or search). */
static void history_step(int dir) {
    char pre[8] = {0};
    g_unichar_to_utf8(V.cmd_kind, pre);
    for (int i = (int)V.hist_pos + dir; i >= 0 && i <= (int)V.history->len; i += dir) {
        if (i == (int)V.history->len) {
            V.hist_pos = i;
            gtk_editable_set_text(GTK_EDITABLE(V.cmd), "");
            return;
        }
        const char *h = V.history->pdata[i];
        if (g_str_has_prefix(h, pre)) {
            V.hist_pos = i;
            gtk_editable_set_text(GTK_EDITABLE(V.cmd), h + strlen(pre));
            gtk_editable_set_position(GTK_EDITABLE(V.cmd), -1);
            return;
        }
    }
}

static gboolean on_cmd_key(GtkEventControllerKey *c, guint keyval, guint code, GdkModifierType mods, gpointer unused) {
    (void)c, (void)code, (void)unused;
    gboolean ctrl = (mods & GDK_CONTROL_MASK) != 0;
    if (keyval == GDK_KEY_Escape || (ctrl && (keyval == GDK_KEY_bracketleft || keyval == GDK_KEY_c))) {
        cmdline_cancel();
        return TRUE;
    }
    if (keyval == GDK_KEY_BackSpace && !*gtk_editable_get_text(GTK_EDITABLE(V.cmd))) {
        cmdline_cancel();
        return TRUE;
    }
    if (keyval == GDK_KEY_Up || keyval == GDK_KEY_Down) {
        history_step(keyval == GDK_KEY_Up ? -1 : 1);
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------------- */
/* Ex commands                                                                */
/* ------------------------------------------------------------------------- */

static gboolean parse_addr(const char **pp, int *line) {
    const char *p = *pp;
    GtkTextIter cur;
    get_cursor(&cur);
    if (g_ascii_isdigit(*p)) {
        *line = (int)strtol(p, (char **)&p, 10) - 1;
    } else if (*p == '.') {
        *line = gtk_text_iter_get_line(&cur);
        p++;
    } else if (*p == '$') {
        *line = line_count() - 1;
        p++;
    } else if (p[0] == '\'' && (p[1] == '<' || p[1] == '>')) {
        *line = p[1] == '<' ? V.vis_first : V.vis_last;
        p += 2;
    } else if (*p != '+' && *p != '-') {
        return FALSE;
    } else {
        *line = gtk_text_iter_get_line(&cur);
    }
    while (*p == '+' || *p == '-') {
        int sign = *p++ == '+' ? 1 : -1;
        int k = g_ascii_isdigit(*p) ? (int)strtol(p, (char **)&p, 10) : 1;
        *line += sign * k;
    }
    *pp = p;
    return TRUE;
}

/* [range]: % | addr | addr,addr. Returns whether one was given. */
static gboolean parse_range(const char **pp, int *first, int *last) {
    const char *p = *pp;
    GtkTextIter cur;
    get_cursor(&cur);
    *first = *last = gtk_text_iter_get_line(&cur);
    gboolean given = FALSE;
    if (*p == '%') {
        *first = 0;
        *last = line_count() - 1;
        p++;
        given = TRUE;
    } else if (parse_addr(&p, first)) {
        *last = *first;
        given = TRUE;
        if (*p == ',' || *p == ';') {
            p++;
            if (!parse_addr(&p, last))
                *last = *first;
        }
    }
    if (*first > *last) {
        int t = *first;
        *first = *last;
        *last = t;
    }
    *first = CLAMP(*first, 0, line_count() - 1);
    *last = CLAMP(*last, 0, line_count() - 1);
    *pp = p;
    return given;
}

/* vim "magic" pattern -> PCRE: \( \) \| \+ \? \{ \< \> are special, their bare forms literal. */
static char *vim_to_pcre(const char *pat) {
    GString *out = g_string_new(NULL);
    for (const char *p = pat; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case '(': case ')': case '|': case '+': case '?': case '{': case '}':
                g_string_append_c(out, *p);
                break;
            case '<': case '>':
                g_string_append(out, "\\b");
                break;
            default:
                g_string_append_c(out, '\\');
                g_string_append_c(out, *p);
            }
        } else if (strchr("()|+?{}", *p)) {
            g_string_append_c(out, '\\');
            g_string_append_c(out, *p);
        } else {
            g_string_append_c(out, *p);
        }
    }
    return g_string_free(out, FALSE);
}

/* vim replacement -> GRegex: & is the whole match (\& a literal &); \1..\9 and \n carry over. */
static char *vim_to_replacement(const char *rep) {
    GString *out = g_string_new(NULL);
    for (const char *p = rep; *p; p++) {
        if (*p == '\\' && p[1] == '&') {
            g_string_append_c(out, '&');
            p++;
        } else if (*p == '\\' && p[1]) {
            g_string_append_c(out, '\\');
            g_string_append_c(out, *++p);
        } else if (*p == '&') {
            g_string_append(out, "\\0");
        } else {
            g_string_append_c(out, *p);
        }
    }
    return g_string_free(out, FALSE);
}

typedef struct {
    const char *rep;
    gboolean global;
    int n;
} SubCtx;

static gboolean sub_eval(const GMatchInfo *mi, GString *res, gpointer data) {
    SubCtx *c = data;
    g_autofree char *x = g_match_info_expand_references(mi, c->rep, NULL);
    if (x)
        g_string_append(res, x);
    c->n++;
    return !c->global; /* stop after the first match unless /g */
}

static void substitute(const char *pat, const char *vim_rep, const char *flags, int first, int last) {
    if (!*pat)
        pat = V.last_search;
    if (!pat) {
        vim_message("E35: No previous regular expression", TRUE);
        return;
    }
    g_autofree char *pcre = vim_to_pcre(pat);
    g_autoptr(GError) err = NULL;
    g_autoptr(GRegex) re = g_regex_new(pcre, strchr(flags, 'i') ? G_REGEX_CASELESS : 0, 0, &err);
    if (!re) {
        g_autofree char *msg = g_strdup_printf("E486: Bad pattern: %s", err->message);
        vim_message(msg, TRUE);
        return;
    }
    g_autofree char *rep = vim_to_replacement(vim_rep);
    SubCtx ctx = {rep, strchr(flags, 'g') != NULL, 0};
    int lines_changed = 0, last_changed = -1;
    gtk_text_buffer_begin_user_action(J.buf);
    for (int line = first; line <= last; line++) {
        GtkTextIter s, e;
        iter_line(&s, line);
        e = s;
        to_line_end(&e);
        g_autofree char *text = gtk_text_buffer_get_slice(J.buf, &s, &e, TRUE);
        if (strstr(text, OBJ_UTF8))
            continue; /* image lines are left alone */
        int before = ctx.n;
        g_autofree char *out = g_regex_replace_eval(re, text, -1, 0, 0, sub_eval, &ctx, NULL);
        if (out && ctx.n > before && !g_str_equal(out, text)) {
            gtk_text_buffer_delete(J.buf, &s, &e);
            gtk_text_buffer_insert(J.buf, &s, out, -1);
            lines_changed++;
            last_changed = line;
            int added = 0; /* the replacement may have added lines (\n) */
            for (const char *q = out; *q; q++)
                added += *q == '\n';
            line += added;
            last += added;
        }
    }
    gtk_text_buffer_end_user_action(J.buf);
    if (!ctx.n) {
        g_autofree char *msg = g_strdup_printf("E486: Pattern not found: %s", pat);
        vim_message(msg, TRUE);
        return;
    }
    if (last_changed >= 0) {
        GtkTextIter it;
        iter_line(&it, last_changed);
        first_nonblank(&it);
        set_cursor(&it);
    }
    if (lines_changed > 1 || ctx.n > 1) {
        g_autofree char *msg = g_strdup_printf("%d substitution%s on %d line%s", ctx.n, ctx.n == 1 ? "" : "s",
                                               lines_changed, lines_changed == 1 ? "" : "s");
        vim_message(msg, FALSE);
    }
}

/* :[range]s/pat/rep/[gi] -- p points at the 's'. */
static void ex_substitute(const char *p, int first, int last) {
    char delim = p[1];
    if (!delim || g_ascii_isalnum(delim) || delim == '\\' || delim == ' ') {
        vim_message("E146: Regular expressions can't be delimited by letters", TRUE);
        return;
    }
    GString *parts[3] = {g_string_new(NULL), g_string_new(NULL), g_string_new(NULL)};
    int part = 0;
    for (p += 2; *p; p++) {
        if (*p == '\\' && p[1] == delim && part < 2) {
            g_string_append_c(parts[part], delim);
            p++;
        } else if (*p == delim && part < 2) {
            part++;
        } else {
            g_string_append_c(parts[part], *p);
        }
    }
    substitute(parts[0]->str, parts[1]->str, parts[2]->str, first, last);
    for (int i = 0; i < 3; i++)
        g_string_free(parts[i], TRUE);
}

static void ex_command(const char *raw) {
    g_autofree char *cmd = g_strstrip(g_strdup(raw));
    const char *p = cmd;
    while (*p == ':' || *p == ' ')
        p++;
    int first, last;
    gboolean ranged = parse_range(&p, &first, &last);
    while (*p == ' ')
        p++;
    GtkTextIter it;
    if (!*p) {
        if (ranged) { /* :42 */
            iter_line(&it, last);
            first_nonblank(&it);
            set_cursor(&it);
        }
        return;
    }
    if (p[0] == 's' && p[1] && !g_ascii_isalpha(p[1])) {
        ex_substitute(p, first, last);
        return;
    }
    static const char *const w_cmds[] = {"w", "w!", "write", "up", "update", NULL};
    static const char *const q_cmds[] = {"q", "q!", "quit", "qa", "qa!", "qall", "wq", "wq!", "x", "x!",
                                         "xit", "wqa", "wqa!", "xa", "exit", NULL};
    gboolean is_w = FALSE, is_q = FALSE;
    for (int i = 0; w_cmds[i]; i++)
        is_w |= g_str_equal(p, w_cmds[i]);
    for (int i = 0; q_cmds[i]; i++)
        is_q |= g_str_equal(p, q_cmds[i]);
    if (is_w) {
        save();
        g_autofree char *text = serialize(J.buf);
        g_autofree char *base = g_path_get_basename(page_path);
        int lines = *text ? line_count() : 0;
        g_autofree char *msg = g_strdup_printf("\"%s\" %dL, %zuB written", base, lines, *text ? strlen(text) + 1 : 0);
        vim_message(msg, FALSE);
    } else if (is_q) {
        save();
        gtk_window_close(J.win);
    } else if (g_str_equal(p, "Ex") || g_str_equal(p, "Explore") || g_str_equal(p, "E")) {
        /* vim's file browser; here, the notes folder in the system file manager */
        g_autoptr(GFile) f = g_file_new_for_path(notes_dir);
        g_autoptr(GtkFileLauncher) l = gtk_file_launcher_new(f);
        gtk_file_launcher_launch(l, J.win, NULL, NULL, NULL);
    } else if (g_str_equal(p, "noh") || g_str_equal(p, "nohlsearch")) {
        cursor_iter(&it);
        gtk_text_buffer_place_cursor(J.buf, &it);
    } else if (g_str_equal(p, "e") || g_str_equal(p, "e!") || g_str_equal(p, "edit") || g_str_equal(p, "edit!")) {
        cursor_iter(&it);
        int line = gtk_text_iter_get_line(&it);
        load_page();
        iter_line(&it, line);
        first_nonblank(&it);
        set_cursor(&it);
    } else if (g_str_has_prefix(p, "set ") || g_str_has_prefix(p, "se ")) {
        const char *opt = strchr(p, ' ') + 1;
        while (*opt == ' ')
            opt++;
        if (g_str_equal(opt, "nu") || g_str_equal(opt, "number"))
            set_line_numbers(TRUE);
        else if (g_str_equal(opt, "nonu") || g_str_equal(opt, "nonumber"))
            set_line_numbers(FALSE);
        else if (g_str_equal(opt, "nu!") || g_str_equal(opt, "invnu") || g_str_equal(opt, "number!"))
            set_line_numbers(!J.line_numbers);
        else if (g_str_equal(opt, "md") || g_str_equal(opt, "markdown"))
            md_set_enabled(TRUE);
        else if (g_str_equal(opt, "nomd") || g_str_equal(opt, "nomarkdown"))
            md_set_enabled(FALSE);
        else if (g_str_equal(opt, "md!") || g_str_equal(opt, "invmd") || g_str_equal(opt, "markdown!"))
            md_set_enabled(!md_enabled());
        else {
            g_autofree char *msg = g_strdup_printf("E518: Unknown option: %s", opt);
            vim_message(msg, TRUE);
        }
    } else if (g_str_equal(p, "d") || g_str_equal(p, "delete") || g_str_equal(p, "y") || g_str_equal(p, "yank") ||
               g_str_equal(p, ">") || g_str_equal(p, "<") || g_str_equal(p, "j") || g_str_equal(p, "join")) {
        GtkTextIter s, e;
        iter_line(&s, first);
        iter_line(&e, last);
        to_line_end(&e);
        gtk_text_iter_forward_char(&e);
        Range r = {offset_of(&s), offset_of(&e), TRUE};
        if (*p == 'j')
            join_lines(first, MAX(1, last - first));
        else
            apply_op(*p == 'y' ? 'y' : *p == 'd' ? 'd' : *p, &r);
    } else if (g_str_equal(p, "u") || g_str_equal(p, "undo")) {
        vim_exec("u");
    } else if (g_str_equal(p, "red") || g_str_equal(p, "redo")) {
        const char k[] = {(char)CTRL('r'), 0};
        vim_exec(k);
    } else {
        g_autofree char *msg = g_strdup_printf("E492: Not an editor command: %s", p);
        vim_message(msg, TRUE);
    }
}

/* ------------------------------------------------------------------------- */
/* The command interpreter                                                    */
/* ------------------------------------------------------------------------- */

/* Runs "<count><keys>" built from a template, e.g. D -> "3d$". */
static Res exec_with_count(int count, const char *keys) {
    g_autofree char *s = count ? g_strdup_printf("%d%s", count, keys) : g_strdup(keys);
    return vim_exec(s);
}

static Res visual_command(gunichar c, const char **pp, int count) {
    Range r;
    visual_range(&r);
    GtkTextIter it;
    switch (c) {
    case 'd':
    case 'x':
    case K_BS:
    case 'X':
    case 'D':
        if (c == 'X' || c == 'D') { /* linewise */
            GtkTextIter s, e;
            iter_at(&s, r.s);
            iter_at(&e, r.e > r.s ? r.e - 1 : r.e);
            gtk_text_iter_set_line_offset(&s, 0);
            to_line_end(&e);
            gtk_text_iter_forward_char(&e);
            r = (Range){offset_of(&s), offset_of(&e), TRUE};
        }
        exit_visual(r.s);
        apply_op('d', &r);
        return R_DONE;
    case 'y':
    case 'Y':
        exit_visual(r.s);
        apply_op('y', &r);
        GtkTextIter s;
        iter_at(&s, r.s);
        set_cursor(&s);
        return R_DONE;
    case 'c':
    case 's':
        exit_visual(r.s);
        apply_op('c', &r);
        return R_DONE;
    case '>':
    case '<':
        exit_visual(r.s);
        apply_op(c, &r);
        return R_DONE;
    case '~':
    case 'u':
    case 'U':
        set_case(&r, c);
        exit_visual(r.s);
        return R_DONE;
    case 'J': {
        iter_at(&it, r.s);
        int first = gtk_text_iter_get_line(&it);
        iter_at(&it, MAX(r.s, r.e - 1));
        int last = gtk_text_iter_get_line(&it);
        exit_visual(r.s);
        join_lines(first, MAX(1, last - first));
        return R_DONE;
    }
    case 'p':
    case 'P':
    case CTRL('v'):
        put(TRUE, count);
        return R_DONE;
    case 'o': {
        GtkTextIter a, v;
        gtk_text_buffer_get_iter_at_mark(J.buf, &a, V.anchor);
        gtk_text_buffer_get_iter_at_mark(J.buf, &v, V.vcur);
        gtk_text_buffer_move_mark(J.buf, V.anchor, &v);
        set_cursor(&a);
        return R_DONE;
    }
    case 'v':
    case 'V': {
        Mode m = c == 'v' ? MODE_VISUAL : MODE_VISUAL_LINE;
        if (V.mode == m) {
            exit_visual(-1);
        } else {
            set_mode(m);
            get_cursor(&it);
            set_cursor(&it);
        }
        return R_DONE;
    }
    case ':':
        exit_visual(-1);
        cmdline_open(':', "'<,'>");
        return R_DONE;
    case 'i':
    case 'a': {
        Range tr;
        Res res = text_object(pp, c, &tr);
        if (res != R_DONE)
            return res;
        GtkTextIter s, e;
        iter_at(&s, tr.s);
        iter_at(&e, tr.e > tr.s ? tr.e - 1 : tr.e);
        if (tr.linewise && V.mode != MODE_VISUAL_LINE)
            set_mode(MODE_VISUAL_LINE);
        gtk_text_buffer_move_mark(J.buf, V.anchor, &s);
        set_cursor(&e);
        return R_DONE;
    }
    }
    return R_BAD; /* not visual-specific: try it as a motion */
}

static Res vim_exec(const char *keys) {
    const char *p = keys;
    int count = parse_count(&p);
    if (!*p)
        return R_MORE;
    const char *cmd_start = p;
    gunichar c = next_char(&p);
    int n = MAX(count, 1);
    GtkTextIter cur, it;
    get_cursor(&cur);
    gboolean vis = is_visual();

    if (c == K_ESC) {
        if (vis)
            exit_visual(-1);
        return R_DONE;
    }

    if (vis) {
        Res r = visual_command(c, &p, count);
        if (r != R_BAD)
            return r;
    } else if (c == 'd' || c == 'y' || c == 'c' || c == '<' || c == '>') {
        /* operator [count] (motion | text object | itself for whole lines) */
        if (!*p)
            return R_MORE;
        int count2 = parse_count(&p);
        if (!*p)
            return R_MORE;
        int total = n * MAX(count2, 1);
        gboolean counted = count || count2;
        gunichar m = g_utf8_get_char(p);
        Range r;
        if (m == c) { /* dd yy cc >> << */
            int line = gtk_text_iter_get_line(&cur);
            if (line + total - 1 >= line_count() && c != 'y' && total > 1)
                total = line_count() - line;
            GtkTextIter s, e;
            iter_line(&s, line);
            iter_line(&e, line + total - 1);
            to_line_end(&e);
            gtk_text_iter_forward_char(&e);
            r = (Range){offset_of(&s), offset_of(&e), TRUE};
        } else if (m == 'i' || m == 'a') {
            p = g_utf8_next_char(p);
            Res tr = text_object(&p, m, &r);
            if (tr != R_DONE)
                return tr;
        } else if (c == 'c' && (m == 'w' || m == 'W') && cls_at(&cur, m == 'W')) {
            /* cw is ce (but a word's last char changes just that char) */
            GtkTextIter e = cur;
            for (int i = 0; i < total; i++)
                if (i || !at_word_end(&e, m == 'W'))
                    word_end(&e, m == 'W');
            Motion mi = {.inclusive = TRUE};
            make_range(cur, e, &mi, &r);
        } else {
            GtkTextIter e = cur;
            Motion mi;
            Res mr = motion(&p, counted ? total : 0, c, &e, &mi);
            if (mr != R_DONE)
                return mr;
            make_range(cur, e, &mi, &r);
        }
        apply_op(c, &r);
        V.want_col = -1;
        return R_DONE;
    } else {
        switch (c) {
        case 'i':
            enter_insert(&cur);
            return R_DONE;
        case 'a':
            if (!gtk_text_iter_ends_line(&cur))
                gtk_text_iter_forward_char(&cur);
            enter_insert(&cur);
            return R_DONE;
        case 'I':
            first_nonblank(&cur);
            enter_insert(&cur);
            return R_DONE;
        case 'A':
            to_line_end(&cur);
            enter_insert(&cur);
            return R_DONE;
        case 'o':
        case 'O':
            open_line(c == 'o');
            return R_DONE;
        case 'x':
        case 's': {
            GtkTextIter e = cur;
            for (int i = 0; i < n && !gtk_text_iter_ends_line(&e); i++)
                gtk_text_iter_forward_char(&e);
            if (gtk_text_iter_equal(&e, &cur)) {
                if (c == 's')
                    enter_insert(&cur);
                return c == 's' ? R_DONE : R_BAD;
            }
            Range r = {offset_of(&cur), offset_of(&e), FALSE};
            apply_op(c == 's' ? 'c' : 'd', &r);
            return R_DONE;
        }
        case 'X': {
            GtkTextIter s = cur;
            for (int i = 0; i < n && !gtk_text_iter_starts_line(&s); i++)
                gtk_text_iter_backward_char(&s);
            if (gtk_text_iter_equal(&s, &cur))
                return R_BAD;
            Range r = {offset_of(&s), offset_of(&cur), FALSE};
            apply_op('d', &r);
            return R_DONE;
        }
        case 'S':
            return exec_with_count(count, "cc");
        case 'D':
            return exec_with_count(count, "d$");
        case 'C':
            return exec_with_count(count, "c$");
        case 'Y':
            return exec_with_count(count, "yy");
        case 'p':
        case 'P':
        case CTRL('v'):
            put(c != 'P', n);
            return R_DONE;
        case 'u':
        case CTRL('r'): {
            gboolean undo = c == 'u';
            for (int i = 0; i < n; i++) {
                if (undo ? !gtk_text_buffer_get_can_undo(J.buf) : !gtk_text_buffer_get_can_redo(J.buf)) {
                    vim_message(undo ? "Already at oldest change" : "Already at newest change", FALSE);
                    break;
                }
                undo ? gtk_text_buffer_undo(J.buf) : gtk_text_buffer_redo(J.buf);
            }
            cursor_iter(&it);
            set_cursor(&it);
            return R_DONE;
        }
        case 'J':
            return join_lines(gtk_text_iter_get_line(&cur), MAX(1, n - 1)) ? R_DONE : R_BAD;
        case 'r': {
            if (!*p)
                return R_MORE;
            gunichar t = next_char(&p);
            if (t == K_ESC)
                return R_DONE;
            GtkTextIter e = cur;
            for (int i = 0; i < n; i++)
                if (gtk_text_iter_ends_line(&e) || !gtk_text_iter_forward_char(&e))
                    return R_BAD;
            if (gtk_text_iter_ends_line(&cur))
                return R_BAD;
            GString *rep = g_string_new(NULL);
            if (t == K_CR)
                g_string_append_c(rep, '\n');
            else
                for (int i = 0; i < n; i++)
                    g_string_append_unichar(rep, t);
            int off = offset_of(&cur);
            gtk_text_buffer_begin_user_action(J.buf);
            gtk_text_buffer_delete(J.buf, &cur, &e);
            gtk_text_buffer_insert(J.buf, &cur, rep->str, -1);
            gtk_text_buffer_end_user_action(J.buf);
            iter_at(&it, t == K_CR ? off + 1 : off + n - 1);
            set_cursor(&it);
            g_string_free(rep, TRUE);
            return R_DONE;
        }
        case '~': {
            GtkTextIter e = cur;
            for (int i = 0; i < n && !gtk_text_iter_ends_line(&e); i++)
                gtk_text_iter_forward_char(&e);
            Range r = {offset_of(&cur), offset_of(&e), FALSE};
            set_case(&r, '~');
            iter_at(&it, r.e);
            set_cursor(&it);
            return R_DONE;
        }
        case 'v':
            enter_visual(MODE_VISUAL, &cur, &cur);
            return R_DONE;
        case 'V':
            enter_visual(MODE_VISUAL_LINE, &cur, &cur);
            return R_DONE;
        case ':':
            cmdline_open(':', count ? (count == 1 ? "." : NULL) : NULL);
            if (count > 1) {
                g_autofree char *pre = g_strdup_printf(".,.+%d", count - 1);
                gtk_editable_set_text(GTK_EDITABLE(V.cmd), pre);
                gtk_editable_set_position(GTK_EDITABLE(V.cmd), -1);
            }
            return R_DONE;
        case '.':
            vim_message("'.' (repeat) isn't supported yet", TRUE);
            return R_DONE;
        case 'q':
        case 'm':
        case '`':
        case '\'':
        case '"':
        case '@': {
            g_autofree char *msg = g_strdup_printf("'%c' isn't supported yet", (char)c);
            vim_message(msg, TRUE);
            return R_DONE;
        }
        }
    }

    /* Commands that work in normal and visual mode. */
    switch (c) {
    case '/':
    case '?':
        cmdline_open(c, NULL);
        return R_DONE;
    case 'n':
    case 'N':
        if (!vis) {
            search_jump((c == 'n') == V.last_search_fwd, n);
            return R_DONE;
        }
        break;
    case '*':
    case '#': {
        g_autofree char *w = word_under_cursor();
        if (!w)
            return R_BAD;
        g_free(V.last_search);
        V.last_search = g_steal_pointer(&w);
        V.last_search_fwd = c == '*';
        search_jump(c == '*', n);
        return R_DONE;
    }
    case 'z': {
        if (!*p)
            return R_MORE;
        gunichar d = next_char(&p);
        double align = (d == 'z' || d == '.') ? 0.5 : (d == 't' || d == K_CR) ? 0.0 : (d == 'b' || d == '-') ? 1.0 : -1;
        if (align < 0)
            return R_BAD;
        GtkTextMark *m = vis ? V.vcur : gtk_text_buffer_get_insert(J.buf);
        gtk_text_view_scroll_to_mark(J.view, m, 0, TRUE, 0, align);
        return R_DONE;
    }
    case CTRL('d'):
    case CTRL('u'):
        page_move(c == CTRL('d') ? 1 : -1, TRUE, count);
        return R_DONE;
    case CTRL('f'):
    case CTRL('b'):
        page_move(c == CTRL('f') ? 1 : -1, FALSE, count);
        return R_DONE;
    case CTRL('e'):
    case CTRL('y'):
        scroll_lines(c == CTRL('e') ? 1 : -1, count);
        return R_DONE;
    case 'g':
        if (!*p)
            return R_MORE;
        if (g_utf8_get_char(p) == 'x') { /* gx: open the image or link under the cursor */
            GdkPaintable *img = gtk_text_iter_get_paintable(&cur);
            if (JOT_IS_IMAGE(img))
                open_image(JOT_IMAGE(img));
            else if (!md_open_url_at(&cur))
                vim_message("gx: no link or image under the cursor", TRUE);
            return R_DONE;
        }
        break;
    }

    /* Otherwise it's a motion: just move. */
    p = cmd_start;
    it = cur;
    Motion mi;
    Res r = motion(&p, count, 0, &it, &mi);
    if (r != R_DONE)
        return r;
    if (mi.dollar)
        V.want_col = G_MAXINT;
    else if (!mi.jk)
        V.want_col = -1;
    set_cursor(&it);
    return R_DONE;
}

/* ------------------------------------------------------------------------- */
/* Input                                                                      */
/* ------------------------------------------------------------------------- */

static void vim_input(gunichar c) {
    V.search_retry = FALSE; /* the user moved on: don't jump later */
    if (V.message_shown) {
        V.message_shown = FALSE;
        update_status();
    }
    if (V.mode == MODE_INSERT) {
        if (c == K_ESC)
            leave_insert();
        return;
    }
    if (V.mode == MODE_NORMAL) {
        GtkTextIter s, e;
        if (gtk_text_buffer_get_selection_bounds(J.buf, &s, &e)) {
            GtkTextIter ins;
            cursor_iter(&ins);
            if (V.search_hl) {
                gtk_text_buffer_place_cursor(J.buf, &ins); /* drop the match highlight */
            } else {
                /* A selection made with the mouse: carry on in visual mode. */
                gboolean ins_at_end = gtk_text_iter_equal(&ins, &e);
                GtkTextIter v = ins_at_end ? e : s;
                if (ins_at_end)
                    gtk_text_iter_backward_char(&v);
                enter_visual(MODE_VISUAL, ins_at_end ? &s : &e, &v);
                if (!ins_at_end) {
                    GtkTextIter a = e;
                    gtk_text_iter_backward_char(&a);
                    gtk_text_buffer_move_mark(J.buf, V.anchor, &a);
                    set_cursor(&v);
                }
            }
        }
    }
    V.search_hl = FALSE;
    clear_image_mark(); /* search highlights last until the next key */
    g_string_append_unichar(V.pending, c);
    Res r = vim_exec(V.pending->str);
    if (r != R_MORE)
        g_string_truncate(V.pending, 0);
    if (r == R_BAD)
        gtk_widget_error_bell(GTK_WIDGET(J.view));
    update_status();
}

/* Insert mode: Ctrl+W deletes the word before the cursor, Ctrl+U back to the indent. */
static void insert_delete_back(gboolean word) {
    GtkTextIter cur, s;
    cursor_iter(&cur);
    s = cur;
    if (word) {
        word_back(&s, FALSE);
    } else {
        first_nonblank(&s);
        if (gtk_text_iter_compare(&s, &cur) >= 0)
            gtk_text_iter_set_line_offset(&s, 0);
    }
    if (gtk_text_iter_equal(&s, &cur) && !gtk_text_iter_is_start(&s))
        gtk_text_iter_backward_char(&s);
    gtk_text_buffer_delete(J.buf, &s, &cur);
}

/* Called first from jot.c's key handler. TRUE = handled (in normal/visual mode that's every key
 * that would otherwise type or edit). */
gboolean vim_key(guint keyval, GdkModifierType mods) {
    mods &= gtk_accelerator_get_default_mod_mask();
    gboolean ctrl = (mods & GDK_CONTROL_MASK) != 0;
    if (mods & (GDK_ALT_MASK | GDK_SUPER_MASK))
        return FALSE;
    if (V.mode == MODE_INSERT) {
        if (keyval == GDK_KEY_Escape || (ctrl && keyval == GDK_KEY_bracketleft)) {
            leave_insert();
            return TRUE;
        }
        if (ctrl && (keyval == GDK_KEY_w || keyval == GDK_KEY_u)) {
            insert_delete_back(keyval == GDK_KEY_w);
            return TRUE;
        }
        return FALSE;
    }
    gunichar c = 0;
    switch (keyval) {
    case GDK_KEY_Escape: c = K_ESC; break;
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: c = K_CR; break;
    case GDK_KEY_BackSpace: c = K_BS; break;
    case GDK_KEY_Tab: case GDK_KEY_ISO_Left_Tab: c = K_TAB; break;
    case GDK_KEY_Left: case GDK_KEY_KP_Left: c = 'h'; break;
    case GDK_KEY_Right: case GDK_KEY_KP_Right: c = 'l'; break;
    case GDK_KEY_Up: case GDK_KEY_KP_Up: c = 'k'; break;
    case GDK_KEY_Down: case GDK_KEY_KP_Down: c = 'j'; break;
    case GDK_KEY_Home: case GDK_KEY_KP_Home: c = '0'; break;
    case GDK_KEY_End: case GDK_KEY_KP_End: c = '$'; break;
    case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down: c = CTRL('f'); break;
    case GDK_KEY_Page_Up: case GDK_KEY_KP_Page_Up: c = CTRL('b'); break;
    case GDK_KEY_Delete: case GDK_KEY_KP_Delete: c = 'x'; break;
    case GDK_KEY_Insert: c = 'i'; break;
    }
    if (!c && ctrl) {
        guint k = gdk_keyval_to_lower(keyval);
        if (k == GDK_KEY_bracketleft)
            c = K_ESC;
        else if (k >= GDK_KEY_a && k <= GDK_KEY_z && strchr("bdefhnpruvy", (int)k))
            c = CTRL(k);
        else
            return FALSE; /* Ctrl+C, Ctrl+S, Ctrl+Q, zoom... go to GTK and jot's shortcuts */
    } else if (!c) {
        c = gdk_keyval_to_unicode(keyval);
        if (!c)
            return FALSE; /* modifiers, function keys */
    }
    vim_input(c);
    return TRUE;
}

/* ------------------------------------------------------------------------- */
/* Setup                                                                      */
/* ------------------------------------------------------------------------- */

static void on_vim_mark_set(GtkTextBuffer *buf, GtkTextIter *it, GtkTextMark *mark, gpointer unused) {
    (void)buf, (void)it, (void)unused;
    if (mark == gtk_text_buffer_get_insert(J.buf) || mark == V.vcur)
        update_status();
}

/* The bottom bar: mode / messages on the left (or the : / ? command line), pending keys and
 * line,col on the right. */
GtkWidget *vim_build_bar(void) {
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_widget_add_css_class(bar, "jot-statusline");
    gtk_widget_add_css_class(bar, "monospace"); /* the system monospace font, like the editor */

    V.stack = GTK_STACK(gtk_stack_new());
    gtk_widget_set_hexpand(GTK_WIDGET(V.stack), TRUE);
    V.mode_label = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_xalign(V.mode_label, 0);
    gtk_label_set_ellipsize(V.mode_label, PANGO_ELLIPSIZE_END);
    gtk_stack_add_named(V.stack, GTK_WIDGET(V.mode_label), "status");

    GtkWidget *cmdbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    V.cmd_prefix = GTK_LABEL(gtk_label_new(":"));
    V.cmd = GTK_TEXT(gtk_text_new());
    gtk_widget_set_hexpand(GTK_WIDGET(V.cmd), TRUE);
    g_signal_connect(V.cmd, "activate", G_CALLBACK(on_cmd_activate), NULL);
    g_signal_connect(V.cmd, "changed", G_CALLBACK(on_cmd_changed), NULL);
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_cmd_key), NULL);
    gtk_widget_add_controller(GTK_WIDGET(V.cmd), keys);
    gtk_box_append(GTK_BOX(cmdbox), GTK_WIDGET(V.cmd_prefix));
    gtk_box_append(GTK_BOX(cmdbox), GTK_WIDGET(V.cmd));
    gtk_stack_add_named(V.stack, cmdbox, "cmd");
    gtk_stack_set_visible_child_name(V.stack, "status");

    V.showcmd_label = GTK_LABEL(gtk_label_new(NULL));
    gtk_widget_add_css_class(GTK_WIDGET(V.showcmd_label), "dim-label");
    V.pos_label = GTK_LABEL(gtk_label_new(NULL));
    gtk_label_set_width_chars(V.pos_label, 10);
    gtk_label_set_xalign(V.pos_label, 1);

    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(V.stack));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(V.showcmd_label));
    V.ocr_label = GTK_LABEL(gtk_label_new(NULL));
    gtk_widget_add_css_class(GTK_WIDGET(V.ocr_label), "dim-label");
    gtk_widget_set_visible(GTK_WIDGET(V.ocr_label), FALSE);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(V.ocr_label));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(V.pos_label));
    return bar;
}

/* Shows OCR progress in the status line; when a read finishes, re-runs a search it may affect:
 * one being typed at the prompt, or one that just failed (unless a key has been pressed since). */
void vim_ocr_changed(gboolean finished) {
    if (!V.ocr_label)
        return;
    guint n = ocr_pending_count();
    if (n) {
        g_autofree char *text = n == 1 ? g_strdup("indexing image…") : g_strdup_printf("indexing %u images…", n);
        gtk_label_set_text(V.ocr_label, text);
    }
    gtk_widget_set_visible(GTK_WIDGET(V.ocr_label), n > 0);
    if (!finished)
        return;
    if (cmdline_is_open()) {
        if (V.cmd_kind != ':')
            on_cmd_changed(GTK_EDITABLE(V.cmd), NULL);
    } else if (V.search_retry) {
        V.search_retry = FALSE;
        search_jump(V.search_retry_fwd, 1);
    }
}

/* After the editor exists and the page is loaded: start in normal mode. */
void vim_init(void) {
    if (!V.pending) {
        V.pending = g_string_new(NULL);
        V.history = g_ptr_array_new_with_free_func(g_free);
        V.want_col = -1;
        g_signal_connect(J.buf, "mark-set", G_CALLBACK(on_vim_mark_set), NULL);
        g_signal_connect_swapped(J.buf, "changed", G_CALLBACK(update_status), NULL);
    }
    set_mode(MODE_NORMAL);
    GtkTextIter it;
    cursor_iter(&it);
    set_cursor(&it);
}

