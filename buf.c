/* buf.c - the page in memory: a gap buffer of cells (character, style bits, image), so styles and
 * images travel with the text and offsets are plain character indexes. See buf.h. */
#include "lite.h"
#include "buf.h"

static void *xalloc(gsize n) {
    void *p = malloc(n ? n : 1);
    if (!p)
        abort();
    return p;
}

static void *xrealloc(void *p, gsize n) {
    p = realloc(p, n ? n : 1);
    if (!p)
        abort();
    return p;
}

typedef struct {
    gunichar ch;     /* 0xFFFC for an image */
    unsigned tags;
    JotImage *img;
} Cell;

struct JotTag {
    unsigned bit;
};

struct JotMark {
    int off;
    gboolean left;
    JotMark *next;
};

typedef struct {
    gboolean is_insert;
    int off, n;
    int sel_ins, sel_bound; /* the selection when it happened */
    Cell *cells;
} Op;

typedef struct {
    Op *ops;
    int n, cap;
    int sel_ins, sel_bound; /* the selection before the group */
    gboolean merge_blocked; /* a multi-step group; GTK only merges single actions */
} Group;

enum { SIG_INSERT, SIG_DELETE, SIG_CHANGED, SIG_MARK, N_SIGS };

typedef struct {
    void (*fn)(void);
    gpointer data;
    int mode; /* 0 before, 1 after, 2 swapped */
} Handler;

struct JotBuf {
    Cell *c;
    int cap, gap_s, gap_e;   /* the gap is c[gap_s .. gap_e) */
    int *ls, n_lines, ls_cap; /* the offset where each line starts */
    JotMark *marks, *insert, *bound;
    int n_tags;
    Group *undo, *redo;
    int n_undo, n_redo, cap_undo, cap_redo;
    Group *open;
    int action_depth, irreversible;
    gboolean replaying;
    Handler h[N_SIGS][8];
    int n_h[N_SIGS];
};

static int count(const JotBuf *b) {
    return b->cap - (b->gap_e - b->gap_s);
}

static const Cell *cell(const JotBuf *b, int off) {
    return &b->c[off < b->gap_s ? off : off + (b->gap_e - b->gap_s)];
}

static Cell *cell_w(JotBuf *b, int off) {
    return &b->c[off < b->gap_s ? off : off + (b->gap_e - b->gap_s)];
}

static void gap_to(JotBuf *b, int off, int need) {
    int gap = b->gap_e - b->gap_s;
    if (gap < need) {
        int n = count(b), cap = MAX(b->cap * 2, n + need + 256), tail = b->cap - b->gap_e;
        b->c = xrealloc(b->c, (gsize)cap * sizeof(Cell));
        memmove(b->c + cap - tail, b->c + b->gap_e, (gsize)tail * sizeof(Cell));
        b->gap_e = cap - tail;
        b->cap = cap;
        gap = b->gap_e - b->gap_s;
    }
    if (off < b->gap_s)
        memmove(b->c + off + gap, b->c + off, (gsize)(b->gap_s - off) * sizeof(Cell));
    else if (off > b->gap_s)
        memmove(b->c + b->gap_s, b->c + b->gap_e, (gsize)(off - b->gap_s) * sizeof(Cell));
    b->gap_s = off;
    b->gap_e = off + gap;
}

/* The line holding `off`: the last line start <= off. */
static int line_of(const JotBuf *b, int off) {
    int lo = 0, hi = b->n_lines - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (b->ls[mid] <= off)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

static void lines_reserve(JotBuf *b, int extra) {
    if (b->n_lines + extra > b->ls_cap) {
        b->ls_cap = MAX(b->ls_cap * 2, b->n_lines + extra + 64);
        b->ls = xrealloc(b->ls, (gsize)b->ls_cap * sizeof(int));
    }
}

JotBuf *jb_new(void) {
    JotBuf *b = g_malloc0(sizeof *b);
    b->n_lines = 1;
    lines_reserve(b, 1);
    b->ls[0] = 0;
    JotIter s = {b, 0};
    b->bound = jb_create_mark(b, NULL, &s, TRUE);
    b->insert = jb_create_mark(b, NULL, &s, FALSE);
    b->bound->left = FALSE;
    return b;
}

static void group_clear(Group *g) {
    for (int i = 0; i < g->n; i++) {
        for (int k = 0; k < g->ops[i].n; k++)
            if (g->ops[i].cells[k].img)
                jot_image_unref(g->ops[i].cells[k].img);
        free(g->ops[i].cells);
    }
    free(g->ops);
    memset(g, 0, sizeof *g);
}

static void stack_clear(Group *s, int *n) {
    for (int i = 0; i < *n; i++)
        group_clear(&s[i]);
    *n = 0;
}

void jb_free(JotBuf *b) {
    for (int i = 0; i < count(b); i++)
        if (cell(b, i)->img)
            jot_image_unref(cell(b, i)->img);
    stack_clear(b->undo, &b->n_undo);
    stack_clear(b->redo, &b->n_redo);
    for (JotMark *m = b->marks, *n; m; m = n) {
        n = m->next;
        free(m);
    }
    free(b->undo);
    free(b->redo);
    free(b->open);
    free(b->c);
    free(b->ls);
    free(b);
}

/* ---- Signals ------------------------------------------------------------- */

void jb_connect_mode(JotBuf *b, const char *name, void (*fn)(void), gpointer data, int mode) {
    int s = g_str_equal(name, "insert-text")    ? SIG_INSERT
            : g_str_equal(name, "delete-range") ? SIG_DELETE
            : g_str_equal(name, "changed")      ? SIG_CHANGED
            : g_str_equal(name, "mark-set")     ? SIG_MARK
                                                : -1;
    if (s < 0 || b->n_h[s] == 8)
        abort();
    b->h[s][b->n_h[s]++] = (Handler){fn, data, mode};
}

typedef void (*InsertCb)(JotBuf *, JotIter *, char *, int, gpointer);
typedef void (*DeleteCb)(JotBuf *, JotIter *, JotIter *, gpointer);
typedef void (*ChangedCb)(JotBuf *, gpointer);
typedef void (*MarkCb)(JotBuf *, JotIter *, JotMark *, gpointer);

static void emit_changed(JotBuf *b) {
    for (int i = 0; i < b->n_h[SIG_CHANGED]; i++) {
        Handler *h = &b->h[SIG_CHANGED][i];
        if (h->mode == 2)
            ((void (*)(gpointer, JotBuf *))h->fn)(h->data, b);
        else
            ((ChangedCb)h->fn)(b, h->data);
    }
}

static void emit_mark(JotBuf *b, JotMark *m) {
    JotIter it = {b, m->off};
    for (int i = 0; i < b->n_h[SIG_MARK]; i++)
        ((MarkCb)b->h[SIG_MARK][i].fn)(b, &it, m, b->h[SIG_MARK][i].data);
}

static void emit_insert(JotBuf *b, int end, const char *text, int len) {
    JotIter it = {b, end};
    for (int i = 0; i < b->n_h[SIG_INSERT]; i++)
        ((InsertCb)b->h[SIG_INSERT][i].fn)(b, &it, (char *)text, len, b->h[SIG_INSERT][i].data);
}

static void emit_delete(JotBuf *b, int s, int e, int mode) {
    JotIter a = {b, s}, z = {b, e};
    for (int i = 0; i < b->n_h[SIG_DELETE]; i++)
        if (b->h[SIG_DELETE][i].mode == mode)
            ((DeleteCb)b->h[SIG_DELETE][i].fn)(b, &a, &z, b->h[SIG_DELETE][i].data);
}

/* ---- Undo ---------------------------------------------------------------- */

static void group_begin(JotBuf *b) {
    if (!b->open) {
        b->open = g_malloc0(sizeof(Group));
        b->open->sel_ins = b->insert->off;
        b->open->sel_bound = b->bound->off;
    }
}

static void push(Group **s, int *n, int *cap, Group g) {
    if (*n == *cap) {
        *cap = MAX(*cap * 2, 32);
        *s = xrealloc(*s, (gsize)*cap * sizeof(Group));
    }
    (*s)[(*n)++] = g;
}

/* test_engine.c only: GTK merges a delete into the delete before it, which makes `xxu` undo both.
 * vim undoes one, and so does lite.c; this copies GTK so the two can still be compared. */
gboolean jb_merge_deletes;

static gboolean merges(const JotBuf *b, const Group *g) {
    if (!jb_merge_deletes || !b->n_undo || g->n != 1 || g->ops[0].is_insert)
        return FALSE;
    const Group *p = &b->undo[b->n_undo - 1];
    const Op *last = &p->ops[p->n - 1], *op = &g->ops[0];
    if (last->is_insert || p->merge_blocked)
        return FALSE;
    if (op->off != last->off && op->off + op->n != last->off)
        return FALSE;
    for (int i = 0; i < op->n; i++)
        if (g_unichar_isspace(op->cells[i].ch))
            return FALSE;
    return TRUE;
}

static void group_end(JotBuf *b) {
    Group *g = b->open;
    b->open = NULL;
    if (!g)
        return;
    if (g->n && merges(b, g)) {
        Group *p = &b->undo[b->n_undo - 1];
        if (p->n == p->cap) {
            p->cap = MAX(p->cap * 2, 8);
            p->ops = xrealloc(p->ops, (gsize)p->cap * sizeof(Op));
        }
        p->ops[p->n++] = g->ops[0];
        free(g->ops);
        stack_clear(b->redo, &b->n_redo);
    } else if (g->n) {
        push(&b->undo, &b->n_undo, &b->cap_undo, *g);
        stack_clear(b->redo, &b->n_redo);
    } else {
        stack_clear(b->redo, &b->n_redo); /* GTK: even an action that changed nothing */
    }
    free(g);
}

static void record(JotBuf *b, gboolean is_insert, int off, int n, const Cell *cells) {
    if (b->replaying || b->irreversible)
        return;
    gboolean own = !b->action_depth;
    group_begin(b);
    Group *g = b->open;
    if (g->n == g->cap) {
        g->cap = MAX(g->cap * 2, 8);
        g->ops = xrealloc(g->ops, (gsize)g->cap * sizeof(Op));
    }
    Cell *copy = xalloc((gsize)n * sizeof(Cell));
    memcpy(copy, cells, (gsize)n * sizeof(Cell));
    for (int i = 0; i < n; i++) {
        copy[i].tags = 0;
        if (copy[i].img)
            jot_image_ref(copy[i].img);
    }
    g->ops[g->n++] = (Op){is_insert, off, n, b->insert->off, b->bound->off, copy};
    if (own)
        group_end(b);
}

void jb_begin_user_action(JotBuf *b) {
    if (b->action_depth++ == 0)
        group_begin(b);
}

void jb_end_user_action(JotBuf *b) {
    if (--b->action_depth == 0)
        group_end(b);
}

void jb_begin_irreversible_action(JotBuf *b) {
    b->irreversible++;
}

void jb_end_irreversible_action(JotBuf *b) {
    if (--b->irreversible == 0) {
        stack_clear(b->undo, &b->n_undo);
        stack_clear(b->redo, &b->n_redo);
    }
}

gboolean jb_get_can_undo(JotBuf *b) {
    return b->n_undo > 0;
}

gboolean jb_get_can_redo(JotBuf *b) {
    return b->n_redo > 0;
}

/* ---- Editing ------------------------------------------------------------- */

static void insert_cells(JotBuf *b, int off, const Cell *cells, int n, const char *text, int len) {
    if (n <= 0)
        return;
    record(b, TRUE, off, n, cells);
    gap_to(b, off, n);
    memcpy(b->c + b->gap_s, cells, (gsize)n * sizeof(Cell));
    for (int i = 0; i < n; i++)
        if (cells[i].img)
            jot_image_ref(cells[i].img);
    b->gap_s += n;

    int line = line_of(b, off), nl = 0;
    for (int i = 0; i < n; i++)
        nl += cells[i].ch == '\n';
    lines_reserve(b, nl);
    memmove(b->ls + line + 1 + nl, b->ls + line + 1, (gsize)(b->n_lines - line - 1) * sizeof(int));
    b->n_lines += nl;
    for (int i = line + 1 + nl; i < b->n_lines; i++)
        b->ls[i] += n;
    for (int i = 0, l = line + 1; i < n; i++)
        if (cells[i].ch == '\n')
            b->ls[l++] = off + i + 1;

    for (JotMark *m = b->marks; m; m = m->next)
        if (m->off > off || (m->off == off && !m->left))
            m->off += n;
    emit_insert(b, off + n, text, len);
    emit_changed(b);
}

static void delete_cells(JotBuf *b, int s, int e) {
    if (e <= s)
        return;
    int n = e - s;
    emit_delete(b, s, e, 0);
    Cell *copy = xalloc((gsize)n * sizeof(Cell));
    for (int i = 0; i < n; i++)
        copy[i] = *cell(b, s + i);
    record(b, FALSE, s, n, copy);
    for (int i = 0; i < n; i++)
        if (copy[i].img)
            jot_image_unref(copy[i].img);
    free(copy);
    gap_to(b, s, 0);
    b->gap_e += n;

    int a = line_of(b, s), z = a + 1;
    while (z < b->n_lines && b->ls[z] <= e)
        z++;
    /* line starts inside (s, e] are gone; the rest shift */
    memmove(b->ls + a + 1, b->ls + z, (gsize)(b->n_lines - z) * sizeof(int));
    b->n_lines -= z - a - 1;
    for (int i = a + 1; i < b->n_lines; i++)
        b->ls[i] -= n;

    for (JotMark *m = b->marks; m; m = m->next)
        m->off = m->off >= e ? m->off - n : m->off > s ? s : m->off;
    emit_delete(b, s, s, 1);
    emit_changed(b);
}

static int text_cells(const char *text, int len, Cell **out) {
    gsize n = len < 0 ? strlen(text) : (gsize)len;
    Cell *c = xalloc((n + 1) * sizeof(Cell));
    int k = 0;
    for (const char *p = text, *e = text + n; p < e; p = g_utf8_next_char(p))
        c[k++] = (Cell){g_utf8_get_char(p), 0, NULL};
    *out = c;
    return k;
}

void jb_insert(JotBuf *b, JotIter *it, const char *text, int len) {
    Cell *c;
    int n = text_cells(text, len, &c);
    insert_cells(b, it->off, c, n, text, len < 0 ? (int)strlen(text) : len);
    it->off += n;
    free(c);
}

void jb_insert_at_cursor(JotBuf *b, const char *text, int len) {
    JotIter it = {b, b->insert->off};
    jb_insert(b, &it, text, len);
}

void jb_insert_image(JotBuf *b, JotIter *it, JotImage *p) {
    Cell c = {0xFFFC, 0, p};
    insert_cells(b, it->off, &c, 1, "\xef\xbf\xbc", 3);
    it->off++;
}

void jb_delete(JotBuf *b, JotIter *s, JotIter *e) {
    int a = MIN(s->off, e->off), z = MAX(s->off, e->off);
    delete_cells(b, a, z);
    s->off = e->off = a;
}

gboolean jb_delete_selection(JotBuf *b, gboolean interactive, gboolean editable) {
    (void)interactive, (void)editable;
    JotIter s, e;
    if (!jb_get_selection_bounds(b, &s, &e))
        return FALSE;
    jb_delete(b, &s, &e);
    return TRUE;
}

void jb_set_text(JotBuf *b, const char *text, int len) {
    delete_cells(b, 0, count(b));
    JotIter s = {b, 0};
    jb_insert(b, &s, text, len);
}

static char *get_range(JotBuf *b, const JotIter *s, const JotIter *e, gboolean objects) {
    GString *o = g_string_new(NULL);
    int a = MIN(s->off, e->off), z = MAX(s->off, e->off);
    for (int i = a; i < z; i++) {
        gunichar c = cell(b, i)->ch;
        if (c != 0xFFFC || objects)
            g_string_append_unichar(o, c);
    }
    return g_string_free(o, FALSE);
}

char *jb_get_text(JotBuf *b, const JotIter *s, const JotIter *e, gboolean hidden) {
    (void)hidden;
    return get_range(b, s, e, FALSE);
}

char *jb_get_slice(JotBuf *b, const JotIter *s, const JotIter *e, gboolean hidden) {
    (void)hidden;
    return get_range(b, s, e, TRUE);
}

/* ---- Tags ---------------------------------------------------------------- */

JotTag *jb_create_tag(JotBuf *b, const char *name) {
    (void)name;
    JotTag *t = g_malloc0(sizeof *t);
    t->bit = 1u << b->n_tags++;
    return t;
}

void jb_apply_tag(JotBuf *b, JotTag *t, const JotIter *s, const JotIter *e) {
    for (int i = s->off; i < e->off; i++)
        cell_w(b, i)->tags |= t->bit;
}

void jb_remove_tag(JotBuf *b, JotTag *t, const JotIter *s, const JotIter *e) {
    for (int i = s->off; i < e->off; i++)
        cell_w(b, i)->tags &= ~t->bit;
}

unsigned jb_tags_at(JotBuf *b, int offset) {
    return offset < count(b) ? cell(b, offset)->tags : 0;
}

int jb_copy_line(JotBuf *b, int line, gunichar *ch, unsigned *tags, int max) {
    if (line < 0 || line >= b->n_lines)
        return 0;
    int s = b->ls[line], n = 0;
    for (int i = s; i < count(b) && n < max; i++) {
        const Cell *c = cell(b, i);
        if (c->ch == '\n')
            break;
        ch[n] = c->ch;
        if (tags)
            tags[n] = c->tags;
        n++;
    }
    return n;
}

/* ---- Marks and iterators ------------------------------------------------- */

int jb_get_char_count(JotBuf *b) {
    return count(b);
}

int jb_get_line_count(JotBuf *b) {
    return b->n_lines;
}

void jb_get_start_iter(JotBuf *b, JotIter *it) {
    *it = (JotIter){b, 0};
}

void jb_get_end_iter(JotBuf *b, JotIter *it) {
    *it = (JotIter){b, count(b)};
}

void jb_get_bounds(JotBuf *b, JotIter *s, JotIter *e) {
    jb_get_start_iter(b, s);
    jb_get_end_iter(b, e);
}

gboolean jb_get_iter_at_line(JotBuf *b, JotIter *it, int line) {
    if (line < 0) {
        jb_get_start_iter(b, it);
        return FALSE;
    }
    if (line >= b->n_lines) {
        jb_get_end_iter(b, it);
        return FALSE;
    }
    *it = (JotIter){b, b->ls[line]};
    return TRUE;
}

void jb_get_iter_at_offset(JotBuf *b, JotIter *it, int offset) {
    *it = (JotIter){b, offset < 0 ? count(b) : MIN(offset, count(b))};
}

void jb_get_iter_at_mark(JotBuf *b, JotIter *it, JotMark *m) {
    *it = (JotIter){b, m->off};
}

JotMark *jb_create_mark(JotBuf *b, const char *name, const JotIter *at, gboolean left_gravity) {
    (void)name;
    JotMark *m = g_malloc0(sizeof *m);
    m->off = at->off;
    m->left = left_gravity;
    m->next = b->marks;
    b->marks = m;
    return m;
}

void jb_move_mark(JotBuf *b, JotMark *m, const JotIter *at) {
    m->off = at->off;
    emit_mark(b, m);
}

JotMark *jb_get_insert(JotBuf *b) {
    return b->insert;
}

void jb_select_range(JotBuf *b, const JotIter *ins, const JotIter *bound) {
    b->insert->off = ins->off;
    b->bound->off = bound->off;
    emit_mark(b, b->insert);
    emit_mark(b, b->bound);
}

void jb_place_cursor(JotBuf *b, const JotIter *at) {
    jb_select_range(b, at, at);
}

gboolean jb_get_selection_bounds(JotBuf *b, JotIter *s, JotIter *e) {
    int a = MIN(b->insert->off, b->bound->off), z = MAX(b->insert->off, b->bound->off);
    if (s)
        *s = (JotIter){b, a};
    if (e)
        *e = (JotIter){b, z};
    return a != z;
}

gboolean jb_get_has_selection(JotBuf *b) {
    return b->insert->off != b->bound->off;
}

gunichar jb_iter_get_char(const JotIter *it) {
    return it->off < count(it->buf) ? cell(it->buf, it->off)->ch : 0;
}

JotImage *jb_iter_get_image(const JotIter *it) {
    return it->off < count(it->buf) ? cell(it->buf, it->off)->img : NULL;
}

int jb_iter_get_offset(const JotIter *it) {
    return it->off;
}

int jb_iter_get_line(const JotIter *it) {
    return line_of(it->buf, it->off);
}

int jb_iter_get_line_offset(const JotIter *it) {
    return it->off - it->buf->ls[line_of(it->buf, it->off)];
}

void jb_iter_set_line_offset(JotIter *it, int col) {
    JotBuf *b = it->buf;
    int l = line_of(b, it->off), s = b->ls[l];
    int next = l + 1 < b->n_lines ? b->ls[l + 1] : count(b);
    it->off = MIN(s + col, next);
}

gboolean jb_iter_is_start(const JotIter *it) {
    return it->off == 0;
}

gboolean jb_iter_is_end(const JotIter *it) {
    return it->off == count(it->buf);
}

gboolean jb_iter_starts_line(const JotIter *it) {
    return it->off == 0 || cell(it->buf, it->off - 1)->ch == '\n';
}

gboolean jb_iter_ends_line(const JotIter *it) {
    return jb_iter_is_end(it) || cell(it->buf, it->off)->ch == '\n';
}

gboolean jb_iter_equal(const JotIter *a, const JotIter *b) {
    return a->off == b->off;
}

int jb_iter_compare(const JotIter *a, const JotIter *b) {
    return a->off < b->off ? -1 : a->off > b->off;
}

gboolean jb_iter_forward_char(JotIter *it) {
    if (jb_iter_is_end(it))
        return FALSE;
    it->off++;
    return !jb_iter_is_end(it);
}

gboolean jb_iter_backward_char(JotIter *it) {
    if (it->off == 0)
        return FALSE;
    it->off--;
    return TRUE;
}

gboolean jb_iter_forward_chars(JotIter *it, int n) {
    int before = it->off;
    it->off = MIN(it->off + n, count(it->buf));
    return it->off != before && !jb_iter_is_end(it);
}

gboolean jb_iter_forward_line(JotIter *it) {
    JotBuf *b = it->buf;
    int l = line_of(b, it->off);
    if (l + 1 < b->n_lines) {
        it->off = b->ls[l + 1];
        return !jb_iter_is_end(it);
    }
    it->off = count(b);
    return FALSE;
}

gboolean jb_iter_forward_to_line_end(JotIter *it) {
    int before = it->off;
    if (jb_iter_ends_line(it) && !jb_iter_is_end(it))
        it->off++; /* already at a line end: on to the next one */
    while (!jb_iter_ends_line(it))
        it->off++;
    return it->off != before && !jb_iter_is_end(it);
}

gboolean jb_iter_forward_find_char(JotIter *it, gboolean (*pred)(gunichar, gpointer), gpointer data, const JotIter *limit) {
    int end = limit ? limit->off : count(it->buf);
    if (it->off >= end)
        return FALSE;
    while (++it->off < end)
        if (pred(jb_iter_get_char(it), data))
            return TRUE;
    return FALSE;
}

gboolean jb_iter_backward_find_char(JotIter *it, gboolean (*pred)(gunichar, gpointer), gpointer data, const JotIter *limit) {
    int start = limit ? limit->off : 0;
    while (it->off > start) {
        it->off--;
        if (it->off >= start && pred(jb_iter_get_char(it), data))
            return TRUE;
    }
    return FALSE;
}

static gboolean match_at(JotBuf *b, int off, const gunichar *q, int qn, gboolean fold) {
    if (off + qn > count(b))
        return FALSE;
    for (int i = 0; i < qn; i++) {
        gunichar c = cell(b, off + i)->ch;
        if (c != q[i] && !(fold && g_unichar_tolower(c) == g_unichar_tolower(q[i])))
            return FALSE;
    }
    return TRUE;
}

gboolean jb_iter_forward_search(const JotIter *it, const char *qs, JotSearchFlags f, JotIter *ms, JotIter *me, const JotIter *limit) {
    long qn;
    gunichar *q = g_utf8_to_ucs4_fast(qs, -1, &qn);
    JotBuf *b = it->buf;
    int end = limit ? limit->off : count(b);
    gboolean fold = (f & JB_SEARCH_CASE_INSENSITIVE) != 0;
    for (int o = it->off; o + qn <= end; o++) {
        if (match_at(b, o, q, (int)qn, fold)) {
            *ms = (JotIter){b, o};
            *me = (JotIter){b, o + (int)qn};
            free(q);
            return TRUE;
        }
    }
    free(q);
    return FALSE;
}

gboolean jb_iter_backward_search(const JotIter *it, const char *qs, JotSearchFlags f, JotIter *ms, JotIter *me, const JotIter *limit) {
    long qn;
    gunichar *q = g_utf8_to_ucs4_fast(qs, -1, &qn);
    JotBuf *b = it->buf;
    int start = limit ? limit->off : 0;
    gboolean fold = (f & JB_SEARCH_CASE_INSENSITIVE) != 0;
    for (int o = it->off - (int)qn; o >= start; o--) {
        if (match_at(b, o, q, (int)qn, fold)) {
            *ms = (JotIter){b, o};
            *me = (JotIter){b, o + (int)qn};
            free(q);
            return TRUE;
        }
    }
    free(q);
    return FALSE;
}

/* ---- Undo and redo ------------------------------------------------------- */

static void replay(JotBuf *b, Group *g, gboolean undo) {
    b->replaying = TRUE;
    for (int i = undo ? g->n - 1 : 0; undo ? i >= 0 : i < g->n; i += undo ? -1 : 1) {
        Op *op = &g->ops[i];
        if (op->is_insert == undo)
            delete_cells(b, op->off, op->off + op->n);
        else
            insert_cells(b, op->off, op->cells, op->n, "", 0);
    }
    b->replaying = FALSE;
}

/* Like GtkTextHistory: undoing a delete brings back the selection from when it happened; otherwise
 * the cursor goes to the edge of what changed. */
static void cursor_after(JotBuf *b, const Op *op, gboolean undo) {
    int ins = undo ? op->off : op->is_insert ? op->off + op->n : op->off, bound = ins;
    gboolean sel_gone = op->sel_ins != op->sel_bound && (MIN(op->sel_ins, op->sel_bound) != op->off || abs(op->sel_ins - op->sel_bound) != op->n);
    if (undo && !op->is_insert && !sel_gone) {
        ins = MIN(MIN(op->sel_ins, op->sel_bound), count(b));
        bound = MIN(MAX(op->sel_ins, op->sel_bound), count(b));
    }
    JotIter i = {b, ins}, o = {b, bound};
    jb_select_range(b, &i, &o);
}

void jb_undo(JotBuf *b) {
    if (!b->n_undo)
        return;
    Group g = b->undo[--b->n_undo];
    replay(b, &g, TRUE);
    cursor_after(b, &g.ops[0], TRUE);
    push(&b->redo, &b->n_redo, &b->cap_redo, g);
}

void jb_redo(JotBuf *b) {
    if (!b->n_redo)
        return;
    Group g = b->redo[--b->n_redo];
    replay(b, &g, FALSE);
    cursor_after(b, &g.ops[g.n - 1], FALSE);
    push(&b->undo, &b->n_undo, &b->cap_undo, g);
}
