/* test_engine.c - the vim engine and Markdown styling, with no window.
 *
 * Runs key scripts and prints what each one left behind: text, cursor, selection, mode, status
 * and style. It builds twice, against real GTK and against lite.c (-DJOT_LITE); `make check-engine`
 * diffs the two transcripts. Any difference is a bug in lite.c, which is how the platforms without
 * GTK stay faithful.
 *
 * Scripts come from the array below (written by hand), from engine_scripts.inc (harvested from
 * test_jot.c), and from a seeded fuzzer: both builds see the same random keys.
 */
#ifndef JOT_LITE
#define JOT_HEADLESS
#endif
#include "core.c"
#include "md.c"
#include "vim.c"

#include <stdio.h>

char *notes_dir = ".", *page_path = "jot.md";
Jot J;

/* ---- A fake platform ------------------------------------------------------ */

static struct {
    char mode[512], keys[64], pos[64], ocr[64];
    gboolean error, cmd_open;
    char cmd[512];
    char *clip;
    gboolean clip_linewise_unused;
    int bells;
} F;

static void copy(char *to, gsize n, const char *s) {
    snprintf(to, n, "%s", s ? s : "");
}

gboolean ui_status_ready(void) { return TRUE; }
void ui_status_mode(const char *t, gboolean e) { copy(F.mode, sizeof F.mode, t); F.error = e; }
void ui_status_keys(const char *t) { copy(F.keys, sizeof F.keys, t); }
void ui_status_pos(const char *t) { copy(F.pos, sizeof F.pos, t); }
void ui_status_ocr(const char *t) { copy(F.ocr, sizeof F.ocr, t); }
void ui_cmd_set_text(const char *t) {
    copy(F.cmd, sizeof F.cmd, t);
    vim_cmd_changed();
}
void ui_cmd_open(const char *prefix, const char *t) {
    (void)prefix;
    copy(F.cmd, sizeof F.cmd, t);
    F.cmd_open = TRUE;
}
void ui_cmd_close(void) { F.cmd_open = FALSE; }
gboolean ui_cmd_is_open(void) { return F.cmd_open; }
const char *ui_cmd_text(void) { return F.cmd; }
void ui_block_cursor(gboolean b) { (void)b; }
void ui_bell(void) { F.bells++; }
void ui_scroll_mark_onscreen(GtkTextMark *m) { (void)m; }
void ui_scroll_mark_to(GtkTextMark *m, double y) { (void)m, (void)y; }
void ui_scroll_iter_onscreen(const GtkTextIter *it) { (void)it; }
void ui_scroll_lines(const GtkTextIter *c, int d, int n) { (void)c, (void)d, (void)n; }
void ui_visible_lines(int *t, int *b) { *t = 0; *b = MIN(19, gtk_text_buffer_get_line_count(J.buf) - 1); }
double ui_iter_screen_pos(const GtkTextIter *it) { (void)it; return 0.5; }
void ui_quit(void) {}
void ui_open_folder(const char *p) { (void)p; }
void ui_open_uri(const char *u) { printf("  open %s\n", u); }
void ui_clipboard_read(void *req) {
    if (F.clip)
        vim_put_text(req, F.clip);
    else
        vim_put_none(req);
}

/* ---- The rest of what jot.c supplies ---------------------------------------- */

void toast(const char *m) { (void)m; }
void image_set_highlight(JotImage *i, const char *q) { (void)i, (void)q; }
void update_image_selection(void) {}
void clipboard_set(const char *text, JotImage *img) {
    (void)img;
    g_free(F.clip);
    F.clip = g_strdup(text);
}
void after_insert(void) {}
void open_image(JotImage *i) { (void)i; }
void save(void) {}
void load_page(void) {}
guint ocr_pending_count(void) { return 0; }
void set_line_numbers(gboolean s) { J.line_numbers = s; }
void vim_ocr_retry_unused(void) {}
JotImage *load_image(const char *rel) { (void)rel; return NULL; }
const char *ocr_lookup(const char *rel) { (void)rel; return NULL; }
void ocr_request(const char *rel) { (void)rel; }

#ifdef JOT_LITE
void lite_image_ref(JotImage *i) { i->refs++; }
void lite_image_unref(JotImage *i) {
    if (--i->refs == 0) {
        g_free(i->path);
        g_free(i);
    }
}
#else
struct _JotImageClass_unused;
G_DEFINE_FINAL_TYPE(JotImage, jot_image, G_TYPE_OBJECT)
static void jot_image_class_init(JotImageClass *k) { (void)k; }
static void jot_image_init(JotImage *s) { (void)s; }
#endif

/* ---- Driving it ------------------------------------------------------------- */

static void flush(void) {
#ifdef JOT_LITE
    lite_idle_run();
#else
    while (g_main_context_iteration(NULL, FALSE)) {
    }
#endif
}

static void feed(const char *keys) {
    for (const char *p = keys; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        char b[8] = {0};
        g_unichar_to_utf8(c, b);
        if (F.cmd_open) {
            if (c == '\r') {
                vim_cmd_activate();
            } else if (c == K_ESC) {
                vim_cmd_key(JK_ESC, 0, FALSE);
            } else if (c == K_BS) {
                gsize n = strlen(F.cmd);
                if (!n) {
                    vim_cmd_key(JK_BACKSPACE, 0, FALSE);
                } else {
                    while (n > 1 && (F.cmd[n - 1] & 0xC0) == 0x80)
                        n--;
                    F.cmd[n - 1] = 0;
                    vim_cmd_changed();
                }
            } else {
                gsize n = strlen(F.cmd);
                if (n + 8 < sizeof F.cmd) {
                    strcpy(F.cmd + n, b);
                    vim_cmd_changed();
                }
            }
        } else if (V.mode == MODE_INSERT && c != K_ESC) {
            if (c == '\r')
                edit_return();
            else if (c == '\t')
                edit_tab(FALSE);
            else if (c < 0x20 && c != K_BS)
                vim_key(JK_CHAR, c + 0x60, TRUE); /* Ctrl+W, Ctrl+U */
            else if (c != K_BS)
                edit_type(b);
        } else {
            vim_input(c);
        }
    }
    flush();
}

static void set_page(const char *text, int line, int col) {
    if (F.cmd_open)
        cmdline_cancel();
    if (V.mode == MODE_INSERT)
        leave_insert();
    if (is_visual())
        exit_visual(-1);
    gtk_text_buffer_begin_irreversible_action(J.buf);
    gtk_text_buffer_set_text(J.buf, "", 0);
    GtkTextIter it;
    gtk_text_buffer_get_start_iter(J.buf, &it);
    insert_text(J.buf, &it, text);
    gtk_text_buffer_end_irreversible_action(J.buf);
    iter_line_col(&it, line, col);
    set_cursor(&it);
    g_string_truncate(V.pending, 0);
    V.message_shown = FALSE;
    V.want_col = -1;
    flush();
}

/* So one script's clipboard or search can't change the next script's result. */
static void reset_state(void) {
    g_clear_pointer(&F.clip, g_free);
    g_clear_pointer(&V.reg, g_free);
    g_clear_pointer(&V.last_search, g_free);
    V.reg_linewise = FALSE;
    V.last_f = V.last_f_cmd = 0;
    V.vis_first = V.vis_last = 0;
    F.bells = 0;
    F.cmd[0] = 0;
    F.mode[0] = 0;
    F.error = FALSE;
}

/* Everything a script can leave behind, escaped onto one line. */
static void dump(const char *label) {
    g_autofree char *text = serialize(J.buf);
    GtkTextIter c, s, e;
    get_cursor(&c);
    gboolean sel = gtk_text_buffer_get_selection_bounds(J.buf, &s, &e);
    printf("%s\n  text ", label);
    for (const char *p = text; *p; p++) {
        if (*p == '\n')
            fputs("\\n", stdout);
        else if (*p == '\\')
            fputs("\\\\", stdout);
        else if ((unsigned char)*p < 0x20)
            printf("\\x%02x", *p);
        else
            putchar(*p);
    }
    printf("\n  cur %d line %d col %d mode %d", gtk_text_iter_get_offset(&c), gtk_text_iter_get_line(&c),
           gtk_text_iter_get_line_offset(&c), V.mode);
    if (sel)
        printf(" sel %d-%d", gtk_text_iter_get_offset(&s), gtk_text_iter_get_offset(&e));
    printf(" pend [%s] cmd %d [%s]\n  status %d [%s] keys [%s] pos [%s] bells %d\n", V.pending->str, F.cmd_open, F.cmd,
           F.error, F.mode, F.keys, F.pos, F.bells);
}

/* Which Markdown styles sit on which characters. */
static void dump_styles(const char *label) {
    printf("%s\n", label);
    GtkTextIter it;
    gtk_text_buffer_get_start_iter(J.buf, &it);
    for (int t = 0; t < N_TAGS; t++) {
        GtkTextIter i = it;
        int start = -1;
        printf("  tag %d:", t);
        for (;;) {
            gboolean on;
#ifdef JOT_LITE
            on = (lite_tags_at(J.buf, gtk_text_iter_get_offset(&i)) >> t) & 1;
#else
            on = gtk_text_iter_has_tag(&i, M.tag[t]);
#endif
            if (gtk_text_iter_is_end(&i))
                on = FALSE;
            if (on && start < 0)
                start = gtk_text_iter_get_offset(&i);
            if (!on && start >= 0) {
                printf(" %d-%d", start, gtk_text_iter_get_offset(&i));
                start = -1;
            }
            if (gtk_text_iter_is_end(&i))
                break;
            gtk_text_iter_forward_char(&i);
        }
        printf("\n");
    }
}

typedef struct {
    const char *text;
    int line, col;
    const char *keys;
} Script;

static const Script scripts[] = {
    {"hello world\nfoo bar baz\n\nlast line", 0, 0, "wdwjp"},
    {"one two three\nfour five six\nseven", 0, 4, "cwTWO\033jbdiwu\022"},
    {"a\nb\nc\nd\ne", 1, 0, "ddjddkpP"},
    {"foo foo foo\nbar foo\nbaz", 0, 0, ":%s/foo/X/g\ruu\022"},
    {"foo foo\nfoo", 0, 0, ":s/\\(foo\\) /[\\1]-&/\r"},
    {"alpha beta\ngamma", 0, 0, "/gam\rNn?alp\r"},
    {"Hello HELLO hello", 0, 0, "/hello\rnn"},
    {"Hello HELLO hello", 0, 0, "/Hello\rnn"},
    {"    indented\n\ttabbed\nplain", 0, 0, ">>j<<j>>uu"},
    {"line one\nline two\nline three", 0, 0, "Vjd"},
    {"line one\nline two\nline three", 0, 5, "vjy$p"},
    {"abc def", 0, 0, "A tail\033Ihead \033"},
    {"abc\n  def\nghi", 1, 3, "o new\033Oold\033"},
    {"abc\n  def", 1, 4, "a\rmore\033"},
    {"a b c d e f g", 0, 0, "3w2dwu"},
    {"abc def ghi", 0, 0, "fdx;,tgrZ~"},
    {"one\ntwo\nthree\nfour", 0, 0, "3J"},
    {"one\ntwo\nthree\nfour", 0, 0, "GkJggG"},
    {"(paren [bracket] {brace})", 0, 1, "di(u%ci[x\033"},
    {"\"quoted text\" tail", 0, 3, "ci\"new\033"},
    {"héllo wörld ñandú\nЖук ελληνικά", 0, 0, "wxwxjwgUiw~"},
    {"日本語 テキスト\nmixed 日本 text", 0, 0, "wdwjwx"},
    {"tabs\there\t\tand there", 0, 0, "wwdw$x"},
    {"", 0, 0, "ihello\033ddu"},
    {"\n\n\n", 1, 0, "ddoX\033"},
    {"no newline", 0, 0, "$a!\0330Dp"},
    {"x", 0, 0, "ddddu\022u"},
    {"word\n", 0, 0, "yyPjp"},
    {"a b\nc d", 0, 0, ":2\r:1,2d\ru:1\rA!\033"},
    {"bad", 0, 0, ":s/[/x/\r"},
    {"bad", 0, 0, ":nosuchcommand\r"},
    {"keep\nthis", 0, 0, "/nothere\r"},
    {"aaa", 0, 0, ":s/a*/b/g\r"},
    {"abc", 0, 0, ":s/x*/-/g\r"},
    {"héllo", 0, 0, ":s/\\w*/[&]/g\r"},
    {"x y", 0, 0, ":s/ /\\r/\r"},
    {"see https://example.com/a.b. now [t](http://x.y)", 0, 6, "gx"},
    {"# h1\n\ntext\n## h2\n```\n# no\n```\n### h3", 0, 0, "]]]]][[["},
};

static const char *const style_pages[] = {
    "# Heading *em* `c`\n## two\nnot#heading\n#nospace",
    "**bold** *it* _un_ __init__ a*b*c snake_case_name **kwargs",
    "`code *no*` ``a`b`` [link](http://a.b/c) https://x.org/y. <https://z.io>",
    "> quote\n>> deeper **b**\n- item\n* star\n+ plus\n1. num\n2) paren\n- [ ] todo\n- [x] done *x*",
    "---\n***\n___\n- - -\ntext\n```\ncode **no**\n# no\n```\nafter **yes**",
    "~~~\ncode\n~~~\n  ```\nindented fence\n  ```\nüñí **bóld** *ítalic* `cödé`",
    "[not a link] (x) [a](b c) https://a.b)c http://[x] **unterminated *mixed",
};

/* A key sequence built from pieces that do something, so it isn't all rejected. */
static guint32 rng = 12345;
static guint32 rnd(guint32 n) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng % n;
}

static const char *const tokens[] = {
    "h", "j", "k", "l", "w", "b", "e", "W", "B", "E", "0", "$", "^", "G", "gg", "ge", "H", "M", "L", "{", "}", "%",
    "x", "X", "D", "C!\033", "s?\033", "S..\033", "J", "~", "rz", "dd", "dw", "de", "db", "d$", "d0", "dj", "dk", "dG", "yy",
    "yw", "p", "P", "cc#\033", "cw_\033", "ciw:\033", "diw", "daw", "di(", "di\"", "ci[-\033", "u", "u", "\022", "v", "V",
    "\033", "\033", "vjd", "Vjd", "vey", "Vy", "vp", "o", "O", "a", "A", "i", "I", "ab", "x y\033", "\r", "\t", "  ", "\027", "\025",
    "fa", "Fa", "to", "To", ";", ",", "/o\r", "?e\r", "/zz\r", "n", "N", "*", "#", ":s/a/b/\r", ":%s/o/0/g\r", ":2d\r",
    ":1,2>\r", ":2,3<\r", ">>", "<<", "2", "3", "gU", "gu", "guu", "gUU", "g~~", "zz", "zt", "\004", "\025", "\006", "\002",
    "]]", "[[", "é", "日", "(", ")", "\"",
};

static const char *const seeds[] = {
    "the quick brown fox\njumps over\n\n  the lazy dog (and \"cat\")\n# heading\n- item one\n```\ncode\n```\nlast",
    "alpha\nbeta\ngamma\ndelta\nepsilon\nzeta\neta\ntheta\niota\nkappa\nlambda\nmu",
    "héllo wörld\n日本語 text\n\ttabbed line\n    spaced\n\n\n[x](http://a.b) *y*",
    "",
    "a",
};

static int token_index(const char *t) {
    for (gsize i = 0; i < sizeof tokens / sizeof *tokens; i++)
        if (tokens[i] == t)
            return (int)i;
    return -1;
}

int main(int argc, char **argv) {
    int fuzz = argc > 1 ? atoi(argv[1]) : 3000;
    gboolean no_undo = argc > 2 && g_str_equal(argv[2], "no-undo"); /* see check-engine */
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
#ifdef JOT_LITE
    lite_merge_deletes = TRUE;
#endif
    J.buf = gtk_text_buffer_new(NULL);
    md_init();
    vim_init();
    md_set_enabled(TRUE);

    if (argc > 3 && g_str_equal(argv[1], "run")) { /* run <seed page> <keys>: for chasing a difference */
        set_page(seeds[atoi(argv[2])], 0, 0);
        feed(argv[3]);
        dump("run");
        return 0;
    }
#ifdef JOT_LITE
    /* Shipped behavior, which GTK doesn't have: one undo per command, like vim. */
    if (argc > 1 && g_str_equal(argv[1], "undo-steps")) {
        lite_merge_deletes = FALSE;
        set_page("alpha", 0, 0);
        feed("xxu");
        g_autofree char *t = serialize(J.buf);
        printf("undo-steps: xxu leaves [%s]\n", t);
        return g_str_equal(t, "lpha") ? 0 : 1;
    }
#endif
    for (gsize i = 0; i < sizeof scripts / sizeof *scripts; i++) {
        set_page(scripts[i].text, scripts[i].line, scripts[i].col);
        feed(scripts[i].keys);
        char label[32];
        snprintf(label, sizeof label, "script %zu", i);
        dump(label);
        feed("\033\033");
        dump_styles("  styles");
    }
#ifdef ENGINE_HARVEST
    static const Script harvested[] = {
#include "engine_scripts.inc"
    };
    for (gsize i = 0; i < sizeof harvested / sizeof *harvested; i++) {
        set_page(harvested[i].text, harvested[i].line, harvested[i].col);
        feed(harvested[i].keys);
        char label[32];
        snprintf(label, sizeof label, "harvest %zu", i);
        dump(label);
    }
#endif
    for (gsize i = 0; i < sizeof style_pages / sizeof *style_pages; i++) {
        set_page(style_pages[i], 0, 0);
        char label[32];
        snprintf(label, sizeof label, "style %zu", i);
        dump_styles(label);
    }
    for (int i = 0; i < fuzz; i++) {
        int seed = (int)rnd(sizeof seeds / sizeof *seeds);
        reset_state();
        set_page(seeds[seed], 0, 0);
        char label[32];
        snprintf(label, sizeof label, "fuzz %d", i);
        printf("%s seed %d keys:", label, seed);
        for (int k = 0, n = 4 + (int)rnd(28); k < n; k++) {
            const char *t = tokens[rnd(sizeof tokens / sizeof *tokens)];
            if (no_undo && (g_str_equal(t, "u") || g_str_equal(t, "\022") || g_str_has_prefix(t, "gu")))
                continue;
            printf(" %zu", (gsize)token_index(t));
            feed(t);
        }
        printf("\n");
        dump(label);
        if (i % 50 == 0)
            dump_styles("  styles");
    }
    fflush(stdout);
    return 0;
}
