/* lite.c - see lite.h. The text buffer is a gap buffer of cells (character, style bits, image), so
 * styles and images travel with the text and offsets are plain character indexes. */
#include "lite.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

/* ------------------------------------------------------------------------- */
/* Memory and strings                                                         */
/* ------------------------------------------------------------------------- */

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

void *g_malloc0(gsize n) {
    void *p = calloc(1, n ? n : 1);
    if (!p)
        abort();
    return p;
}

void *g_steal_ptr(void *pp) {
    void *p = *(void **)pp;
    *(void **)pp = NULL;
    return p;
}

char *g_strndup(const char *s, gsize n) {
    char *d = xalloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

char *g_strdup(const char *s) {
    return s ? g_strndup(s, strlen(s)) : NULL;
}

static char *vprintf_alloc(const char *fmt, va_list ap) {
    va_list c;
    va_copy(c, ap);
    char probe[1];
    int n = vsnprintf(probe, 1, fmt, c);
    va_end(c);
    char *s = xalloc((gsize)n + 1);
    vsnprintf(s, (gsize)n + 1, fmt, ap);
    return s;
}

char *g_strdup_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s = vprintf_alloc(fmt, ap);
    va_end(ap);
    return s;
}

char *g_strconcat(const char *first, ...) {
    GString *s = g_string_new(first);
    va_list ap;
    va_start(ap, first);
    for (const char *p; (p = va_arg(ap, const char *));)
        g_string_append(s, p);
    va_end(ap);
    return g_string_free(s, FALSE);
}

char *g_strstrip(char *s) {
    char *p = s;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r'))
        p++;
    gsize n = strlen(p);
    while (n && (p[n - 1] == ' ' || (p[n - 1] >= '\t' && p[n - 1] <= '\r')))
        n--;
    memmove(s, p, n);
    s[n] = 0;
    return s;
}

char **g_strsplit(const char *s, const char *sep, int max) {
    (void)max;
    gsize sl = strlen(sep), n = 1, i = 0;
    for (const char *p = s; (p = strstr(p, sep)); p += sl)
        n++;
    char **v = xalloc((n + 1) * sizeof *v);
    for (const char *p = s;;) {
        const char *q = strstr(p, sep);
        if (!q) {
            v[i++] = g_strdup(p);
            break;
        }
        v[i++] = g_strndup(p, (gsize)(q - p));
        p = q + sl;
    }
    v[i] = NULL;
    return v;
}

void g_strfreev(char **v) {
    if (!v)
        return;
    for (char **p = v; *p; p++)
        free(*p);
    free(v);
}

char *g_path_get_basename(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++)
        if ((*p == '/' || *p == '\\') && p[1])
            b = p + 1;
    gsize n = strcspn(b, "/\\");
    return g_strndup(b, n);
}

gboolean g_str_equal(const void *a, const void *b) {
    return strcmp(a, b) == 0;
}

gboolean g_str_has_prefix(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

gboolean g_str_has_suffix(const char *s, const char *suffix) {
    gsize a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

/* ---- GString ------------------------------------------------------------- */

static void gs_reserve(GString *s, gsize extra) {
    if (s->len + extra + 1 > s->cap) {
        s->cap = MAX(s->cap * 2, s->len + extra + 16);
        s->str = xrealloc(s->str, s->cap);
    }
}

GString *g_string_new(const char *init) {
    GString *s = g_malloc0(sizeof *s);
    gs_reserve(s, 0);
    s->str[0] = 0;
    if (init)
        g_string_append(s, init);
    return s;
}

char *g_string_free(GString *s, gboolean free_segment) {
    char *r = free_segment ? NULL : s->str;
    if (free_segment)
        free(s->str);
    free(s);
    return r;
}

GString *g_string_append_len(GString *s, const char *t, gsize n) {
    gs_reserve(s, n);
    memcpy(s->str + s->len, t, n);
    s->len += n;
    s->str[s->len] = 0;
    return s;
}

GString *g_string_append(GString *s, const char *t) {
    return g_string_append_len(s, t, strlen(t));
}

GString *g_string_append_c(GString *s, char c) {
    return g_string_append_len(s, &c, 1);
}

GString *g_string_append_unichar(GString *s, gunichar c) {
    char b[6];
    return g_string_append_len(s, b, (gsize)g_unichar_to_utf8(c, b));
}

GString *g_string_prepend_c(GString *s, char c) {
    gs_reserve(s, 1);
    memmove(s->str + 1, s->str, s->len + 1);
    s->str[0] = c;
    s->len++;
    return s;
}

GString *g_string_truncate(GString *s, gsize n) {
    if (n < s->len) {
        s->len = n;
        s->str[n] = 0;
    }
    return s;
}

void g_string_append_printf(GString *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *t = vprintf_alloc(fmt, ap);
    va_end(ap);
    g_string_append(s, t);
    free(t);
}

guint g_string_replace(GString *s, const char *find, const char *with, guint limit) {
    gsize fl = strlen(find);
    guint n = 0;
    GString *out = g_string_new(NULL);
    const char *p = s->str;
    for (const char *q; (q = strstr(p, find)) && (!limit || n < limit); p = q + fl, n++) {
        g_string_append_len(out, p, (gsize)(q - p));
        g_string_append(out, with);
    }
    g_string_append(out, p);
    free(s->str);
    *s = *out;
    free(out);
    return n;
}

/* ---- GPtrArray ----------------------------------------------------------- */

GPtrArray *g_ptr_array_new_with_free_func(void (*free_fn)(void *)) {
    GPtrArray *a = g_malloc0(sizeof *a);
    a->free_fn = free_fn;
    return a;
}

void g_ptr_array_add(GPtrArray *a, void *p) {
    if (a->len == a->cap) {
        a->cap = MAX(a->cap * 2, 8);
        a->pdata = xrealloc(a->pdata, a->cap * sizeof(void *));
    }
    a->pdata[a->len++] = p;
}

void g_ptr_array_unref(GPtrArray *a) {
    if (a->free_fn)
        for (guint i = 0; i < a->len; i++)
            a->free_fn(a->pdata[i]);
    free(a->pdata);
    free(a);
}

/* ------------------------------------------------------------------------- */
/* Unicode                                                                    */
/* ------------------------------------------------------------------------- */

int lite_utf8_skip(unsigned char c) {
    return c < 0xC0 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : c < 0xF8 ? 4 : 1;
}

gunichar g_utf8_get_char(const char *ps) {
    const unsigned char *p = (const unsigned char *)ps;
    if (p[0] < 0x80)
        return p[0];
    int n = lite_utf8_skip(p[0]);
    if (n == 1)
        return 0xFFFD;
    gunichar c = p[0] & (0xFF >> (n + 1));
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80)
            return 0xFFFD;
        c = (c << 6) | (p[i] & 0x3F);
    }
    return c;
}

int g_unichar_to_utf8(gunichar c, char *o) {
    if (c < 0x80) {
        o[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        o[0] = (char)(0xC0 | (c >> 6));
        o[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        o[0] = (char)(0xE0 | (c >> 12));
        o[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        o[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (c >> 18));
    o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((c >> 6) & 0x3F));
    o[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

long g_utf8_strlen(const char *p, long max) {
    long n = 0;
    for (const char *e = max < 0 ? NULL : p + max; e ? p < e : *p != 0; n++)
        p += lite_utf8_skip((unsigned char)*p);
    return n;
}

long g_utf8_pointer_to_offset(const char *s, const char *pos) {
    return g_utf8_strlen(s, pos - s);
}

gunichar *g_utf8_to_ucs4_fast(const char *s, long len, long *items) {
    long n = g_utf8_strlen(s, len);
    gunichar *u = xalloc(((gsize)n + 1) * sizeof *u);
    for (long i = 0; i < n; i++, s = g_utf8_next_char(s))
        u[i] = g_utf8_get_char(s);
    u[n] = 0;
    if (items)
        *items = n;
    return u;
}

gboolean g_unichar_isspace(gunichar c) {
    return c == ' ' || (c >= 9 && c <= 13) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

/* Case and letters: ASCII, Latin-1, Latin Extended-A, Greek and Cyrillic. That covers what vim's
 * word motions and ~ meet in practice; anything else above ASCII counts as a letter, without case. */
static gboolean nonletter(gunichar c) {
    return (c >= 0x80 && c <= 0xBF && c != 0xAA && c != 0xB5 && c != 0xBA) || c == 0xD7 || c == 0xF7 ||
           (c >= 0x2000 && c <= 0x2BFF) || (c >= 0x3000 && c <= 0x303F) || c == 0xFFFC || c == 0xFFFD;
}

gunichar g_unichar_tolower(gunichar c) {
    if (c < 0x80)
        return c >= 'A' && c <= 'Z' ? c + 32 : c;
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7) || (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) || (c >= 0x410 && c <= 0x42F))
        return c + 32;
    if (c >= 0x400 && c <= 0x40F)
        return c + 80;
    if (c >= 0x100 && c <= 0x17F && c != 0x130 && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x178 && c != 0x17F) {
        gboolean odd_upper = (c >= 0x139 && c <= 0x148) || c >= 0x179;
        return (c & 1) == (odd_upper ? 1u : 0u) ? c + 1 : c;
    }
    return c;
}

gunichar g_unichar_toupper(gunichar c) {
    if (c < 0x80)
        return c >= 'a' && c <= 'z' ? c - 32 : c;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7) || (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) || (c >= 0x430 && c <= 0x44F))
        return c - 32;
    if (c >= 0x450 && c <= 0x45F)
        return c - 80;
    if (c >= 0x100 && c <= 0x17F && c != 0x130 && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x178 && c != 0x17F) {
        gboolean odd_upper = (c >= 0x139 && c <= 0x148) || c >= 0x179;
        return (c & 1) == (odd_upper ? 0u : 1u) ? c - 1 : c;
    }
    return c;
}

gboolean g_unichar_isupper(gunichar c) {
    return g_unichar_tolower(c) != c;
}

gboolean g_unichar_isalnum(gunichar c) {
    if (c < 0x80)
        return g_ascii_isalnum(c);
    return !nonletter(c) && !g_unichar_isspace(c);
}

char *g_utf8_casefold(const char *s, long len) {
    GString *o = g_string_new(NULL);
    for (const char *p = s, *e = len < 0 ? s + strlen(s) : s + len; p < e; p = g_utf8_next_char(p))
        g_string_append_unichar(o, g_unichar_tolower(g_utf8_get_char(p)));
    return g_string_free(o, FALSE);
}

/* ------------------------------------------------------------------------- */
/* Regular expressions                                                        */
/* ------------------------------------------------------------------------- */

struct GRegex {
    pcre2_code *code;
};

struct GMatchInfo {
    const GRegex *re;
    char *subject;
    gsize len;
    pcre2_match_data *md;
    int rc;
    int start_flags;
    gsize next;      /* where the next search starts */
    gboolean empty;  /* the last match was empty: don't find it twice */
};

GRegex *g_regex_new(const char *pattern, int compile_flags, int match_flags, GError **err) {
    (void)match_flags;
    int ec;
    PCRE2_SIZE eo;
    uint32_t o = PCRE2_UTF | PCRE2_UCP | (compile_flags & G_REGEX_CASELESS ? PCRE2_CASELESS : 0);
    pcre2_code *code = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED, o, &ec, &eo, NULL);
    if (!code) {
        if (err) {
            PCRE2_UCHAR msg[160];
            pcre2_get_error_message(ec, msg, sizeof msg);
            *err = g_malloc0(sizeof **err);
            (*err)->message = g_strdup_printf("Error while compiling regular expression ‘%s’ at char %zu: %s",
                                              pattern, (size_t)eo, (char *)msg);
        }
        return NULL;
    }
    GRegex *re = g_malloc0(sizeof *re);
    re->code = code;
    return re;
}

void g_regex_unref(GRegex *re) {
    pcre2_code_free(re->code);
    free(re);
}

void g_error_free(GError *e) {
    free(e->message);
    free(e);
}

static void mi_search(GMatchInfo *mi) {
    for (;;) {
        if (mi->next > mi->len) {
            mi->rc = PCRE2_ERROR_NOMATCH;
            return;
        }
        uint32_t o = (mi->start_flags & G_REGEX_MATCH_ANCHORED ? PCRE2_ANCHORED : 0) |
                     (mi->empty ? PCRE2_NOTEMPTY_ATSTART | PCRE2_ANCHORED : 0);
        mi->rc = pcre2_match(mi->re->code, (PCRE2_SPTR)mi->subject, mi->len, mi->next, o, mi->md, NULL);
        if (mi->rc >= 0) {
            PCRE2_SIZE *v = pcre2_get_ovector_pointer(mi->md);
            mi->empty = v[0] == v[1];
            mi->next = v[1];
            return;
        }
        if (!mi->empty)
            return;
        /* No non-empty match right after an empty one: step one character and search on. */
        mi->empty = FALSE;
        if (mi->next >= mi->len) {
            mi->rc = PCRE2_ERROR_NOMATCH;
            return;
        }
        mi->next += (gsize)lite_utf8_skip((unsigned char)mi->subject[mi->next]);
    }
}

gboolean g_regex_match_full(const GRegex *re, const char *s, long len, int start, int flags, GMatchInfo **out, GError **err) {
    (void)err;
    GMatchInfo *mi = g_malloc0(sizeof *mi);
    mi->re = re;
    mi->len = len < 0 ? strlen(s) : (gsize)len;
    mi->subject = g_strndup(s, mi->len);
    mi->md = pcre2_match_data_create_from_pattern(re->code, NULL);
    mi->start_flags = flags;
    mi->next = (gsize)start;
    mi_search(mi);
    gboolean ok = mi->rc >= 0;
    if (out)
        *out = mi;
    else
        g_match_info_unref(mi);
    return ok;
}

gboolean g_regex_match(const GRegex *re, const char *s, int flags, GMatchInfo **mi) {
    return g_regex_match_full(re, s, -1, 0, flags, mi, NULL);
}

gboolean g_match_info_matches(const GMatchInfo *mi) {
    return mi->rc >= 0;
}

gboolean g_match_info_next(GMatchInfo *mi, GError **err) {
    (void)err;
    mi_search(mi);
    return mi->rc >= 0;
}

void g_match_info_fetch_pos(const GMatchInfo *mi, int group, int *start, int *end) {
    PCRE2_SIZE *v = pcre2_get_ovector_pointer(mi->md);
    gboolean set = group < mi->rc && v[2 * group] != PCRE2_UNSET;
    if (start)
        *start = set ? (int)v[2 * group] : -1;
    if (end)
        *end = set ? (int)v[2 * group + 1] : -1;
}

char *g_match_info_fetch(const GMatchInfo *mi, int group) {
    int a, b;
    g_match_info_fetch_pos(mi, group, &a, &b);
    return a < 0 ? g_strdup("") : g_strndup(mi->subject + a, (gsize)(b - a));
}

void g_match_info_unref(GMatchInfo *mi) {
    pcre2_match_data_free(mi->md);
    free(mi->subject);
    free(mi);
}

/* GRegex's replacement language, as far as vim_to_replacement produces it. */
char *g_match_info_expand_references(const GMatchInfo *mi, const char *rep, GError **err) {
    (void)err;
    GString *o = g_string_new(NULL);
    for (const char *p = rep; *p; p++) {
        if (*p != '\\' || !p[1]) {
            g_string_append_c(o, *p);
            continue;
        }
        p++;
        if (g_ascii_isdigit(*p)) {
            int n = *p - '0';
            while (g_ascii_isdigit(p[1]) && n < 100)
                n = n * 10 + (*++p - '0');
            char *g = g_match_info_fetch(mi, n);
            g_string_append(o, g);
            free(g);
            continue;
        }
        switch (*p) {
        case 'n': g_string_append_c(o, '\n'); break;
        case 't': g_string_append_c(o, '\t'); break;
        case 'r': g_string_append_c(o, '\r'); break;
        case 'f': g_string_append_c(o, '\f'); break;
        case 'a': g_string_append_c(o, '\a'); break;
        case 'b': g_string_append_c(o, '\b'); break;
        case 'e': g_string_append_c(o, 0x1b); break;
        default: g_string_append_c(o, *p); break;
        }
    }
    return g_string_free(o, FALSE);
}

char *g_regex_replace_eval(const GRegex *re, const char *s, long len, int start, int flags,
                           gboolean (*eval)(const GMatchInfo *, GString *, gpointer), gpointer data, GError **err) {
    (void)err;
    GString *o = g_string_new(NULL);
    gsize n = len < 0 ? strlen(s) : (gsize)len, pos = 0;
    GMatchInfo *mi = NULL;
    g_regex_match_full(re, s, (long)n, start, flags, &mi, NULL);
    while (g_match_info_matches(mi)) {
        int a, b;
        g_match_info_fetch_pos(mi, 0, &a, &b);
        g_string_append_len(o, s + pos, (gsize)a - pos);
        pos = (gsize)b;
        if (eval(mi, o, data))
            break;
        g_match_info_next(mi, NULL);
    }
    g_string_append_len(o, s + pos, n - pos);
    g_match_info_unref(mi);
    return g_string_free(o, FALSE);
}

static gboolean literal_eval(const GMatchInfo *mi, GString *out, gpointer rep) {
    (void)mi;
    g_string_append(out, rep);
    return FALSE;
}

char *g_regex_replace_literal(const GRegex *re, const char *s, long len, int start, const char *rep, int flags, GError **err) {
    return g_regex_replace_eval(re, s, len, start, flags, literal_eval, (void *)rep, err);
}

/* ------------------------------------------------------------------------- */
/* Idle callbacks                                                             */
/* ------------------------------------------------------------------------- */

static struct {
    gboolean (*fn)(gpointer);
    gpointer data;
} idles[32];
static guint n_idles;

guint g_idle_add_full(int priority, gboolean (*fn)(gpointer), gpointer data, void (*destroy)(gpointer)) {
    (void)priority, (void)destroy;
    if (n_idles == sizeof idles / sizeof *idles)
        abort();
    idles[n_idles].fn = fn;
    idles[n_idles].data = data;
    return ++n_idles;
}

gboolean lite_idle_pending(void) {
    return n_idles != 0;
}

void lite_idle_run(void) {
    while (n_idles) {
        gboolean (*fn)(gpointer) = idles[0].fn;
        gpointer data = idles[0].data;
        memmove(idles, idles + 1, --n_idles * sizeof *idles);
        if (fn(data))
            g_idle_add_full(0, fn, data, NULL);
    }
}

/* ------------------------------------------------------------------------- */
/* Text buffer                                                                */
/* ------------------------------------------------------------------------- */

typedef struct {
    gunichar ch;     /* 0xFFFC for an image */
    unsigned tags;
    GdkPaintable *img;
} Cell;

struct LiteTag {
    unsigned bit;
};

struct GtkTextMark {
    int off;
    gboolean left;
    struct GtkTextMark *next;
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

struct GtkTextBuffer {
    Cell *c;
    int cap, gap_s, gap_e;   /* the gap is c[gap_s .. gap_e) */
    int *ls, n_lines, ls_cap; /* the offset where each line starts */
    GtkTextMark *marks, *insert, *bound;
    int n_tags;
    Group *undo, *redo;
    int n_undo, n_redo, cap_undo, cap_redo;
    Group *open;
    int action_depth, irreversible;
    gboolean replaying;
    Handler h[N_SIGS][8];
    int n_h[N_SIGS];
};

static int count(const GtkTextBuffer *b) {
    return b->cap - (b->gap_e - b->gap_s);
}

static const Cell *cell(const GtkTextBuffer *b, int off) {
    return &b->c[off < b->gap_s ? off : off + (b->gap_e - b->gap_s)];
}

static Cell *cell_w(GtkTextBuffer *b, int off) {
    return &b->c[off < b->gap_s ? off : off + (b->gap_e - b->gap_s)];
}

static void gap_to(GtkTextBuffer *b, int off, int need) {
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
static int line_of(const GtkTextBuffer *b, int off) {
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

static void lines_reserve(GtkTextBuffer *b, int extra) {
    if (b->n_lines + extra > b->ls_cap) {
        b->ls_cap = MAX(b->ls_cap * 2, b->n_lines + extra + 64);
        b->ls = xrealloc(b->ls, (gsize)b->ls_cap * sizeof(int));
    }
}

GtkTextBuffer *gtk_text_buffer_new(void *unused) {
    (void)unused;
    GtkTextBuffer *b = g_malloc0(sizeof *b);
    b->n_lines = 1;
    lines_reserve(b, 1);
    b->ls[0] = 0;
    GtkTextIter s = {b, 0};
    b->bound = gtk_text_buffer_create_mark(b, NULL, &s, TRUE);
    b->insert = gtk_text_buffer_create_mark(b, NULL, &s, FALSE);
    b->bound->left = FALSE;
    return b;
}

static void group_clear(Group *g) {
    for (int i = 0; i < g->n; i++) {
        for (int k = 0; k < g->ops[i].n; k++)
            if (g->ops[i].cells[k].img)
                lite_image_unref(g->ops[i].cells[k].img);
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

void gtk_text_buffer_free(GtkTextBuffer *b) {
    for (int i = 0; i < count(b); i++)
        if (cell(b, i)->img)
            lite_image_unref(cell(b, i)->img);
    stack_clear(b->undo, &b->n_undo);
    stack_clear(b->redo, &b->n_redo);
    for (GtkTextMark *m = b->marks, *n; m; m = n) {
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

void lite_signal_connect(GtkTextBuffer *b, const char *name, void (*fn)(void), gpointer data, int mode) {
    int s = g_str_equal(name, "insert-text")    ? SIG_INSERT
            : g_str_equal(name, "delete-range") ? SIG_DELETE
            : g_str_equal(name, "changed")      ? SIG_CHANGED
            : g_str_equal(name, "mark-set")     ? SIG_MARK
                                                : -1;
    if (s < 0 || b->n_h[s] == 8)
        abort();
    b->h[s][b->n_h[s]++] = (Handler){fn, data, mode};
}

typedef void (*InsertCb)(GtkTextBuffer *, GtkTextIter *, char *, int, gpointer);
typedef void (*DeleteCb)(GtkTextBuffer *, GtkTextIter *, GtkTextIter *, gpointer);
typedef void (*ChangedCb)(GtkTextBuffer *, gpointer);
typedef void (*MarkCb)(GtkTextBuffer *, GtkTextIter *, GtkTextMark *, gpointer);

static void emit_changed(GtkTextBuffer *b) {
    for (int i = 0; i < b->n_h[SIG_CHANGED]; i++) {
        Handler *h = &b->h[SIG_CHANGED][i];
        if (h->mode == 2)
            ((void (*)(gpointer, GtkTextBuffer *))h->fn)(h->data, b);
        else
            ((ChangedCb)h->fn)(b, h->data);
    }
}

static void emit_mark(GtkTextBuffer *b, GtkTextMark *m) {
    GtkTextIter it = {b, m->off};
    for (int i = 0; i < b->n_h[SIG_MARK]; i++)
        ((MarkCb)b->h[SIG_MARK][i].fn)(b, &it, m, b->h[SIG_MARK][i].data);
}

static void emit_insert(GtkTextBuffer *b, int end, const char *text, int len) {
    GtkTextIter it = {b, end};
    for (int i = 0; i < b->n_h[SIG_INSERT]; i++)
        ((InsertCb)b->h[SIG_INSERT][i].fn)(b, &it, (char *)text, len, b->h[SIG_INSERT][i].data);
}

static void emit_delete(GtkTextBuffer *b, int s, int e, int mode) {
    GtkTextIter a = {b, s}, z = {b, e};
    for (int i = 0; i < b->n_h[SIG_DELETE]; i++)
        if (b->h[SIG_DELETE][i].mode == mode)
            ((DeleteCb)b->h[SIG_DELETE][i].fn)(b, &a, &z, b->h[SIG_DELETE][i].data);
}

/* ---- Undo ---------------------------------------------------------------- */

static void group_begin(GtkTextBuffer *b) {
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
gboolean lite_merge_deletes;

static gboolean merges(const GtkTextBuffer *b, const Group *g) {
    if (!lite_merge_deletes || !b->n_undo || g->n != 1 || g->ops[0].is_insert)
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

static void group_end(GtkTextBuffer *b) {
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

static void record(GtkTextBuffer *b, gboolean is_insert, int off, int n, const Cell *cells) {
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
            lite_image_ref(copy[i].img);
    }
    g->ops[g->n++] = (Op){is_insert, off, n, b->insert->off, b->bound->off, copy};
    if (own)
        group_end(b);
}

void gtk_text_buffer_begin_user_action(GtkTextBuffer *b) {
    if (b->action_depth++ == 0)
        group_begin(b);
}

void gtk_text_buffer_end_user_action(GtkTextBuffer *b) {
    if (--b->action_depth == 0)
        group_end(b);
}

void gtk_text_buffer_begin_irreversible_action(GtkTextBuffer *b) {
    b->irreversible++;
}

void gtk_text_buffer_end_irreversible_action(GtkTextBuffer *b) {
    if (--b->irreversible == 0) {
        stack_clear(b->undo, &b->n_undo);
        stack_clear(b->redo, &b->n_redo);
    }
}

gboolean gtk_text_buffer_get_can_undo(GtkTextBuffer *b) {
    return b->n_undo > 0;
}

gboolean gtk_text_buffer_get_can_redo(GtkTextBuffer *b) {
    return b->n_redo > 0;
}

/* ---- Editing ------------------------------------------------------------- */

static void insert_cells(GtkTextBuffer *b, int off, const Cell *cells, int n, const char *text, int len) {
    if (n <= 0)
        return;
    record(b, TRUE, off, n, cells);
    gap_to(b, off, n);
    memcpy(b->c + b->gap_s, cells, (gsize)n * sizeof(Cell));
    for (int i = 0; i < n; i++)
        if (cells[i].img)
            lite_image_ref(cells[i].img);
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

    for (GtkTextMark *m = b->marks; m; m = m->next)
        if (m->off > off || (m->off == off && !m->left))
            m->off += n;
    emit_insert(b, off + n, text, len);
    emit_changed(b);
}

static void delete_cells(GtkTextBuffer *b, int s, int e) {
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
            lite_image_unref(copy[i].img);
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

    for (GtkTextMark *m = b->marks; m; m = m->next)
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

void gtk_text_buffer_insert(GtkTextBuffer *b, GtkTextIter *it, const char *text, int len) {
    Cell *c;
    int n = text_cells(text, len, &c);
    insert_cells(b, it->off, c, n, text, len < 0 ? (int)strlen(text) : len);
    it->off += n;
    free(c);
}

void gtk_text_buffer_insert_at_cursor(GtkTextBuffer *b, const char *text, int len) {
    GtkTextIter it = {b, b->insert->off};
    gtk_text_buffer_insert(b, &it, text, len);
}

void gtk_text_buffer_insert_paintable(GtkTextBuffer *b, GtkTextIter *it, GdkPaintable *p) {
    Cell c = {0xFFFC, 0, p};
    insert_cells(b, it->off, &c, 1, "\xef\xbf\xbc", 3);
    it->off++;
}

void gtk_text_buffer_delete(GtkTextBuffer *b, GtkTextIter *s, GtkTextIter *e) {
    int a = MIN(s->off, e->off), z = MAX(s->off, e->off);
    delete_cells(b, a, z);
    s->off = e->off = a;
}

gboolean gtk_text_buffer_delete_selection(GtkTextBuffer *b, gboolean interactive, gboolean editable) {
    (void)interactive, (void)editable;
    GtkTextIter s, e;
    if (!gtk_text_buffer_get_selection_bounds(b, &s, &e))
        return FALSE;
    gtk_text_buffer_delete(b, &s, &e);
    return TRUE;
}

void gtk_text_buffer_set_text(GtkTextBuffer *b, const char *text, int len) {
    delete_cells(b, 0, count(b));
    GtkTextIter s = {b, 0};
    gtk_text_buffer_insert(b, &s, text, len);
}

static char *get_range(GtkTextBuffer *b, const GtkTextIter *s, const GtkTextIter *e, gboolean objects) {
    GString *o = g_string_new(NULL);
    int a = MIN(s->off, e->off), z = MAX(s->off, e->off);
    for (int i = a; i < z; i++) {
        gunichar c = cell(b, i)->ch;
        if (c != 0xFFFC || objects)
            g_string_append_unichar(o, c);
    }
    return g_string_free(o, FALSE);
}

char *gtk_text_buffer_get_text(GtkTextBuffer *b, const GtkTextIter *s, const GtkTextIter *e, gboolean hidden) {
    (void)hidden;
    return get_range(b, s, e, FALSE);
}

char *gtk_text_buffer_get_slice(GtkTextBuffer *b, const GtkTextIter *s, const GtkTextIter *e, gboolean hidden) {
    (void)hidden;
    return get_range(b, s, e, TRUE);
}

/* ---- Tags ---------------------------------------------------------------- */

GtkTextTag *gtk_text_buffer_create_tag(GtkTextBuffer *b, const char *name, const char *first_prop) {
    (void)name, (void)first_prop;
    GtkTextTag *t = g_malloc0(sizeof *t);
    t->bit = 1u << b->n_tags++;
    return t;
}

void gtk_text_buffer_apply_tag(GtkTextBuffer *b, GtkTextTag *t, const GtkTextIter *s, const GtkTextIter *e) {
    for (int i = s->off; i < e->off; i++)
        cell_w(b, i)->tags |= t->bit;
}

void gtk_text_buffer_remove_tag(GtkTextBuffer *b, GtkTextTag *t, const GtkTextIter *s, const GtkTextIter *e) {
    for (int i = s->off; i < e->off; i++)
        cell_w(b, i)->tags &= ~t->bit;
}

unsigned lite_tags_at(GtkTextBuffer *b, int offset) {
    return offset < count(b) ? cell(b, offset)->tags : 0;
}

int lite_copy_line(GtkTextBuffer *b, int line, gunichar *ch, unsigned *tags, int max) {
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

int gtk_text_buffer_get_char_count(GtkTextBuffer *b) {
    return count(b);
}

int gtk_text_buffer_get_line_count(GtkTextBuffer *b) {
    return b->n_lines;
}

void gtk_text_buffer_get_start_iter(GtkTextBuffer *b, GtkTextIter *it) {
    *it = (GtkTextIter){b, 0};
}

void gtk_text_buffer_get_end_iter(GtkTextBuffer *b, GtkTextIter *it) {
    *it = (GtkTextIter){b, count(b)};
}

void gtk_text_buffer_get_bounds(GtkTextBuffer *b, GtkTextIter *s, GtkTextIter *e) {
    gtk_text_buffer_get_start_iter(b, s);
    gtk_text_buffer_get_end_iter(b, e);
}

gboolean gtk_text_buffer_get_iter_at_line(GtkTextBuffer *b, GtkTextIter *it, int line) {
    if (line < 0) {
        gtk_text_buffer_get_start_iter(b, it);
        return FALSE;
    }
    if (line >= b->n_lines) {
        gtk_text_buffer_get_end_iter(b, it);
        return FALSE;
    }
    *it = (GtkTextIter){b, b->ls[line]};
    return TRUE;
}

void gtk_text_buffer_get_iter_at_offset(GtkTextBuffer *b, GtkTextIter *it, int offset) {
    *it = (GtkTextIter){b, offset < 0 ? count(b) : MIN(offset, count(b))};
}

void gtk_text_buffer_get_iter_at_mark(GtkTextBuffer *b, GtkTextIter *it, GtkTextMark *m) {
    *it = (GtkTextIter){b, m->off};
}

GtkTextMark *gtk_text_buffer_create_mark(GtkTextBuffer *b, const char *name, const GtkTextIter *at, gboolean left_gravity) {
    (void)name;
    GtkTextMark *m = g_malloc0(sizeof *m);
    m->off = at->off;
    m->left = left_gravity;
    m->next = b->marks;
    b->marks = m;
    return m;
}

void gtk_text_buffer_move_mark(GtkTextBuffer *b, GtkTextMark *m, const GtkTextIter *at) {
    m->off = at->off;
    emit_mark(b, m);
}

GtkTextMark *gtk_text_buffer_get_insert(GtkTextBuffer *b) {
    return b->insert;
}

void gtk_text_buffer_select_range(GtkTextBuffer *b, const GtkTextIter *ins, const GtkTextIter *bound) {
    b->insert->off = ins->off;
    b->bound->off = bound->off;
    emit_mark(b, b->insert);
    emit_mark(b, b->bound);
}

void gtk_text_buffer_place_cursor(GtkTextBuffer *b, const GtkTextIter *at) {
    gtk_text_buffer_select_range(b, at, at);
}

gboolean gtk_text_buffer_get_selection_bounds(GtkTextBuffer *b, GtkTextIter *s, GtkTextIter *e) {
    int a = MIN(b->insert->off, b->bound->off), z = MAX(b->insert->off, b->bound->off);
    if (s)
        *s = (GtkTextIter){b, a};
    if (e)
        *e = (GtkTextIter){b, z};
    return a != z;
}

gboolean gtk_text_buffer_get_has_selection(GtkTextBuffer *b) {
    return b->insert->off != b->bound->off;
}

gunichar gtk_text_iter_get_char(const GtkTextIter *it) {
    return it->off < count(it->buf) ? cell(it->buf, it->off)->ch : 0;
}

GdkPaintable *gtk_text_iter_get_paintable(const GtkTextIter *it) {
    return it->off < count(it->buf) ? cell(it->buf, it->off)->img : NULL;
}

int gtk_text_iter_get_offset(const GtkTextIter *it) {
    return it->off;
}

int gtk_text_iter_get_line(const GtkTextIter *it) {
    return line_of(it->buf, it->off);
}

int gtk_text_iter_get_line_offset(const GtkTextIter *it) {
    return it->off - it->buf->ls[line_of(it->buf, it->off)];
}

void gtk_text_iter_set_line_offset(GtkTextIter *it, int col) {
    GtkTextBuffer *b = it->buf;
    int l = line_of(b, it->off), s = b->ls[l];
    int next = l + 1 < b->n_lines ? b->ls[l + 1] : count(b);
    it->off = MIN(s + col, next);
}

gboolean gtk_text_iter_is_start(const GtkTextIter *it) {
    return it->off == 0;
}

gboolean gtk_text_iter_is_end(const GtkTextIter *it) {
    return it->off == count(it->buf);
}

gboolean gtk_text_iter_starts_line(const GtkTextIter *it) {
    return it->off == 0 || cell(it->buf, it->off - 1)->ch == '\n';
}

gboolean gtk_text_iter_ends_line(const GtkTextIter *it) {
    return gtk_text_iter_is_end(it) || cell(it->buf, it->off)->ch == '\n';
}

gboolean gtk_text_iter_equal(const GtkTextIter *a, const GtkTextIter *b) {
    return a->off == b->off;
}

int gtk_text_iter_compare(const GtkTextIter *a, const GtkTextIter *b) {
    return a->off < b->off ? -1 : a->off > b->off;
}

gboolean gtk_text_iter_forward_char(GtkTextIter *it) {
    if (gtk_text_iter_is_end(it))
        return FALSE;
    it->off++;
    return !gtk_text_iter_is_end(it);
}

gboolean gtk_text_iter_backward_char(GtkTextIter *it) {
    if (it->off == 0)
        return FALSE;
    it->off--;
    return TRUE;
}

gboolean gtk_text_iter_forward_chars(GtkTextIter *it, int n) {
    int before = it->off;
    it->off = MIN(it->off + n, count(it->buf));
    return it->off != before && !gtk_text_iter_is_end(it);
}

gboolean gtk_text_iter_forward_line(GtkTextIter *it) {
    GtkTextBuffer *b = it->buf;
    int l = line_of(b, it->off);
    if (l + 1 < b->n_lines) {
        it->off = b->ls[l + 1];
        return !gtk_text_iter_is_end(it);
    }
    it->off = count(b);
    return FALSE;
}

gboolean gtk_text_iter_forward_to_line_end(GtkTextIter *it) {
    int before = it->off;
    if (gtk_text_iter_ends_line(it) && !gtk_text_iter_is_end(it))
        it->off++; /* already at a line end: on to the next one */
    while (!gtk_text_iter_ends_line(it))
        it->off++;
    return it->off != before && !gtk_text_iter_is_end(it);
}

gboolean gtk_text_iter_forward_find_char(GtkTextIter *it, gboolean (*pred)(gunichar, gpointer), gpointer data, const GtkTextIter *limit) {
    int end = limit ? limit->off : count(it->buf);
    if (it->off >= end)
        return FALSE;
    while (++it->off < end)
        if (pred(gtk_text_iter_get_char(it), data))
            return TRUE;
    return FALSE;
}

gboolean gtk_text_iter_backward_find_char(GtkTextIter *it, gboolean (*pred)(gunichar, gpointer), gpointer data, const GtkTextIter *limit) {
    int start = limit ? limit->off : 0;
    while (it->off > start) {
        it->off--;
        if (it->off >= start && pred(gtk_text_iter_get_char(it), data))
            return TRUE;
    }
    return FALSE;
}

static gboolean match_at(GtkTextBuffer *b, int off, const gunichar *q, int qn, gboolean fold) {
    if (off + qn > count(b))
        return FALSE;
    for (int i = 0; i < qn; i++) {
        gunichar c = cell(b, off + i)->ch;
        if (c != q[i] && !(fold && g_unichar_tolower(c) == g_unichar_tolower(q[i])))
            return FALSE;
    }
    return TRUE;
}

gboolean gtk_text_iter_forward_search(const GtkTextIter *it, const char *qs, GtkTextSearchFlags f, GtkTextIter *ms, GtkTextIter *me, const GtkTextIter *limit) {
    long qn;
    gunichar *q = g_utf8_to_ucs4_fast(qs, -1, &qn);
    GtkTextBuffer *b = it->buf;
    int end = limit ? limit->off : count(b);
    gboolean fold = (f & GTK_TEXT_SEARCH_CASE_INSENSITIVE) != 0;
    for (int o = it->off; o + qn <= end; o++) {
        if (match_at(b, o, q, (int)qn, fold)) {
            *ms = (GtkTextIter){b, o};
            *me = (GtkTextIter){b, o + (int)qn};
            free(q);
            return TRUE;
        }
    }
    free(q);
    return FALSE;
}

gboolean gtk_text_iter_backward_search(const GtkTextIter *it, const char *qs, GtkTextSearchFlags f, GtkTextIter *ms, GtkTextIter *me, const GtkTextIter *limit) {
    long qn;
    gunichar *q = g_utf8_to_ucs4_fast(qs, -1, &qn);
    GtkTextBuffer *b = it->buf;
    int start = limit ? limit->off : 0;
    gboolean fold = (f & GTK_TEXT_SEARCH_CASE_INSENSITIVE) != 0;
    for (int o = it->off - (int)qn; o >= start; o--) {
        if (match_at(b, o, q, (int)qn, fold)) {
            *ms = (GtkTextIter){b, o};
            *me = (GtkTextIter){b, o + (int)qn};
            free(q);
            return TRUE;
        }
    }
    free(q);
    return FALSE;
}

/* ---- Undo and redo ------------------------------------------------------- */

static void replay(GtkTextBuffer *b, Group *g, gboolean undo) {
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
static void cursor_after(GtkTextBuffer *b, const Op *op, gboolean undo) {
    int ins = undo ? op->off : op->is_insert ? op->off + op->n : op->off, bound = ins;
    gboolean sel_gone = op->sel_ins != op->sel_bound && (MIN(op->sel_ins, op->sel_bound) != op->off || abs(op->sel_ins - op->sel_bound) != op->n);
    if (undo && !op->is_insert && !sel_gone) {
        ins = MIN(MIN(op->sel_ins, op->sel_bound), count(b));
        bound = MIN(MAX(op->sel_ins, op->sel_bound), count(b));
    }
    GtkTextIter i = {b, ins}, o = {b, bound};
    gtk_text_buffer_select_range(b, &i, &o);
}

void gtk_text_buffer_undo(GtkTextBuffer *b) {
    if (!b->n_undo)
        return;
    Group g = b->undo[--b->n_undo];
    replay(b, &g, TRUE);
    cursor_after(b, &g.ops[0], TRUE);
    push(&b->redo, &b->n_redo, &b->cap_redo, g);
}

void gtk_text_buffer_redo(GtkTextBuffer *b) {
    if (!b->n_redo)
        return;
    Group g = b->redo[--b->n_redo];
    replay(b, &g, FALSE);
    cursor_after(b, &g.ops[g.n - 1], FALSE);
    push(&b->undo, &b->n_undo, &b->cap_undo, g);
}
