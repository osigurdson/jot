/* lite.c - see lite.h. */
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
