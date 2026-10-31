/* lite.h - the small part of GLib the engine (vim.c, md.c, core.c, buf.c) uses, for platforms
 * without it: same names and same behavior. Add only what the engine calls; it is not a GLib port.
 */
#ifndef JOT_LITE_H
#define JOT_LITE_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Types and macros ---------------------------------------------------- */

typedef int gboolean;
typedef unsigned int guint;
typedef uint32_t gunichar;
typedef size_t gsize;
typedef void *gpointer;
typedef int64_t gint64;
typedef long glong;
typedef uint32_t guint32;

#define TRUE 1
#define FALSE 0
#define G_MAXINT 2147483647
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(x, lo, hi) ((x) > (hi) ? (hi) : (x) < (lo) ? (lo) : (x))
#define G_SOURCE_REMOVE FALSE
#define G_PRIORITY_HIGH_IDLE 100
#define G_CALLBACK(f) ((void (*)(void))(f))

#define g_free free
#define g_new0(T, n) ((T *)g_malloc0(sizeof(T) * (gsize)(n)))
void *g_malloc0(gsize n);
void *g_steal_ptr(void *pp);
#define g_steal_pointer(pp) ((__typeof__(*(pp)))g_steal_ptr(pp))
#define g_clear_pointer(pp, destroy)   \
    do {                               \
        if (*(pp))                     \
            destroy(*(pp));            \
        *(pp) = NULL;                  \
    } while (0)

static inline void lite_free_ptr(void *pp) {
    free(*(void **)pp);
}
#define g_autofree __attribute__((cleanup(lite_free_ptr)))
#define g_autoptr(T) __attribute__((cleanup(lite_unref_##T))) T *
#define g_auto(T) __attribute__((cleanup(lite_unref_##T))) T

/* ---- Strings ------------------------------------------------------------- */

char *g_strdup(const char *s);
char *g_strndup(const char *s, gsize n);
char *g_strdup_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
char *g_strconcat(const char *first, ...) __attribute__((sentinel));
char *g_strstrip(char *s);
char **g_strsplit(const char *s, const char *sep, int max);
void g_strfreev(char **v);
char *g_path_get_basename(const char *path);
gboolean g_str_equal(const void *a, const void *b);
gboolean g_str_has_prefix(const char *s, const char *prefix);
gboolean g_str_has_suffix(const char *s, const char *suffix);
#define g_snprintf snprintf
#define g_ascii_isdigit(c) ((c) >= '0' && (c) <= '9')
#define g_ascii_isalpha(c) (((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z'))
#define g_ascii_isalnum(c) (g_ascii_isalpha(c) || g_ascii_isdigit(c))

typedef char **GStrv;
static inline void lite_unref_GStrv(GStrv *v) {
    g_strfreev(*v);
}

typedef struct {
    char *str;
    gsize len, cap;
} GString;

GString *g_string_new(const char *init);
char *g_string_free(GString *s, gboolean free_segment);
GString *g_string_append(GString *s, const char *t);
GString *g_string_append_len(GString *s, const char *t, gsize n);
GString *g_string_append_c(GString *s, char c);
GString *g_string_append_unichar(GString *s, gunichar c);
GString *g_string_prepend_c(GString *s, char c);
GString *g_string_truncate(GString *s, gsize n);
void g_string_append_printf(GString *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
guint g_string_replace(GString *s, const char *find, const char *with, guint limit);
static inline void lite_unref_GString(GString **s) {
    if (*s)
        g_string_free(*s, TRUE);
}

typedef struct {
    void **pdata;
    guint len, cap;
    void (*free_fn)(void *);
} GPtrArray;

GPtrArray *g_ptr_array_new_with_free_func(void (*free_fn)(void *));
void g_ptr_array_add(GPtrArray *a, void *p);
void g_ptr_array_unref(GPtrArray *a);
static inline void lite_unref_GPtrArray(GPtrArray **a) {
    if (*a)
        g_ptr_array_unref(*a);
}

/* ---- Unicode ------------------------------------------------------------- */

gunichar g_utf8_get_char(const char *p);
#define g_utf8_next_char(p) ((const char *)((p) + lite_utf8_skip(*(const unsigned char *)(p))))
int lite_utf8_skip(unsigned char c);
int g_unichar_to_utf8(gunichar c, char *out);
long g_utf8_strlen(const char *p, long max_bytes);
long g_utf8_pointer_to_offset(const char *s, const char *pos);
gunichar *g_utf8_to_ucs4_fast(const char *s, long len, long *items);
char *g_utf8_casefold(const char *s, long len);
gboolean g_unichar_isspace(gunichar c);
gboolean g_unichar_isupper(gunichar c);
gboolean g_unichar_isalnum(gunichar c);
gunichar g_unichar_tolower(gunichar c);
gunichar g_unichar_toupper(gunichar c);

/* ---- Regular expressions (PCRE2, like GRegex) ------------------------------ */

typedef struct {
    char *message;
} GError;
typedef struct GRegex GRegex;
typedef struct GMatchInfo GMatchInfo;

#define G_REGEX_OPTIMIZE 0
#define G_REGEX_CASELESS 1
#define G_REGEX_MATCH_ANCHORED 1

GRegex *g_regex_new(const char *pattern, int compile_flags, int match_flags, GError **err);
void g_regex_unref(GRegex *re);
gboolean g_regex_match(const GRegex *re, const char *s, int flags, GMatchInfo **mi);
gboolean g_regex_match_full(const GRegex *re, const char *s, long len, int start, int flags, GMatchInfo **mi, GError **err);
gboolean g_match_info_matches(const GMatchInfo *mi);
gboolean g_match_info_next(GMatchInfo *mi, GError **err);
void g_match_info_fetch_pos(const GMatchInfo *mi, int group, int *start, int *end);
char *g_match_info_fetch(const GMatchInfo *mi, int group);
char *g_match_info_expand_references(const GMatchInfo *mi, const char *rep, GError **err);
void g_match_info_unref(GMatchInfo *mi);
char *g_regex_replace_literal(const GRegex *re, const char *s, long len, int start, const char *rep, int flags, GError **err);
char *g_regex_replace_eval(const GRegex *re, const char *s, long len, int start, int flags,
                           gboolean (*eval)(const GMatchInfo *, GString *, gpointer), gpointer data, GError **err);
void g_error_free(GError *e);
static inline void lite_unref_GRegex(GRegex **r) {
    if (*r)
        g_regex_unref(*r);
}
static inline void lite_unref_GMatchInfo(GMatchInfo **m) {
    if (*m)
        g_match_info_unref(*m);
}
static inline void lite_unref_GError(GError **e) {
    if (*e)
        g_error_free(*e);
}

/* ---- Idle callbacks: the platform runs them with lite_idle_run() before painting ------- */

guint g_idle_add_full(int priority, gboolean (*fn)(gpointer), gpointer data, void (*destroy)(gpointer));
void lite_idle_run(void);
gboolean lite_idle_pending(void);

#endif
