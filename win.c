/* win.c - jot on Windows: the window, the painted page view, keyboard and mouse, clipboard, files
 * and images. All editing lives in the engine (vim.c, md.c, core.c, buf.c); this implements ui.h
 * and what jot.h asks of a platform.
 *
 * The view is painted by hand: monospace text on a grid, wrapped at word boundaries, with images
 * on their own lines. Images are decoded when first drawn and dropped again past IMAGE_CACHE_BYTES.
 *
 * `jot --selftest out.bmp file keys` runs keys through the real window procedure, paints to
 * out.bmp and writes the page and status to `file`. It needs no display, so it runs under Wine.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include "jot.h"

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wctype.h>

#define IMAGE_CACHE_BYTES (256u * 1024 * 1024)
#define SAVE_DELAY_MS 400
#define TIMER_SAVE 1
#define DEFAULT_FONT_PT 11
/* Sizes are in 96 DPI pixels, scaled with px(). */
#define PAD_TOP px(18)
#define PAD_LEFT px(14)
#define PAD_RIGHT px(28)
#define PAD_BOTTOM px(300)
#define LINE_GAP px(2)

char *notes_dir, *page_path;
Jot J;

typedef struct {
    int w, h;
    HBITMAP bmp; /* NULL until drawn */
    gsize bytes;
    unsigned long used;
} WinImage;

/* One line, measured: where each character sits on the grid. */
typedef struct {
    gunichar *ch;
    unsigned *tags;
    int *col, *row;
    int n, cap, rows;
} Measured;

static struct {
    HWND hwnd;
    HFONT font[4]; /* bit 0 bold, bit 1 italic */
    int cell_w, line_h, font_pt, dpi;
    int client_w, client_h, bar_h, gutter_w, cols;
    int scroll, total;
    int *ly, n_ly; /* each line's top; ly[n_lines] is the end */
    gboolean layout_dirty, loading, read_only, block_cursor, dark, test_ctrl, test_shift;
    char *last_saved, *assets_dir, *state_path;
    gint64 mtime;
    char mode[512], keys[64], pos[64], ocr[64];
    gboolean mode_error, cmd_open;
    GString *cmd;
    char cmd_prefix[8];
    wchar_t high_surrogate;
    JotImage *outlined;
    GPtrArray *images; /* every image loaded, kept for reuse: JotImage* */
    gsize image_bytes;
    unsigned long clock;
    IWICImagingFactory *wic;
    JotTag **tags;
    Measured m;
    COLORREF bg, ink, dim, soft, accent, chip, block, sel, bar, rule, err;
} W;

/* A size at 96 DPI, on this display. */
static int px(int n) {
    return MulDiv(n, W.dpi, 96);
}

/* ------------------------------------------------------------------------- */
/* UTF-8 <-> UTF-16, files                                                    */
/* ------------------------------------------------------------------------- */

static wchar_t *wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = g_new0(wchar_t, n + 1);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static char *narrow(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = g_new0(char, n + 1);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static char *join(const char *a, const char *b) {
    return g_strdup_printf("%s\\%s", a, b);
}

static gboolean read_file(const char *path, char **out, gsize *len) {
    g_autofree wchar_t *w = wide(path);
    HANDLE f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return FALSE;
    LARGE_INTEGER size;
    GetFileSizeEx(f, &size);
    char *data = g_new0(char, (gsize)size.QuadPart + 1);
    DWORD got = 0;
    BOOL ok = ReadFile(f, data, (DWORD)size.QuadPart, &got, NULL);
    CloseHandle(f);
    if (!ok) {
        g_free(data);
        return FALSE;
    }
    *out = data;
    if (len)
        *len = got;
    return TRUE;
}

/* All or nothing: a crash mid-write can't leave half a page. */
static gboolean write_file(const char *path, const char *data, gsize len) {
    g_autofree char *tmp = g_strconcat(path, ".tmp", NULL);
    g_autofree wchar_t *wt = wide(tmp), *wp = wide(path);
    HANDLE f = CreateFileW(wt, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return FALSE;
    DWORD put = 0;
    BOOL ok = WriteFile(f, data, (DWORD)len, &put, NULL) && put == len && FlushFileBuffers(f);
    CloseHandle(f);
    if (ok)
        ok = MoveFileExW(wt, wp, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok)
        DeleteFileW(wt);
    return ok;
}

static gint64 file_mtime(const char *path) {
    g_autofree wchar_t *w = wide(path);
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(w, GetFileExInfoStandard, &a))
        return -1;
    return ((gint64)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
}

static gboolean file_exists(const char *path) {
    g_autofree wchar_t *w = wide(path);
    return GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
}

static void make_dirs(const char *path) {
    g_autofree wchar_t *w = wide(path);
    SHCreateDirectoryExW(NULL, w, NULL);
}

/* ------------------------------------------------------------------------- */
/* Images                                                                     */
/* ------------------------------------------------------------------------- */

static IWICBitmapFrameDecode *open_frame(const char *rel, IWICBitmapDecoder **dec) {
    g_autofree char *abs = join(notes_dir, rel);
    g_autofree wchar_t *w = wide(abs);
    for (wchar_t *p = w; *p; p++)
        if (*p == '/')
            *p = '\\';
    IWICBitmapFrameDecode *frame = NULL;
    if (FAILED(IWICImagingFactory_CreateDecoderFromFilename(W.wic, w, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, dec)))
        return NULL;
    if (FAILED(IWICBitmapDecoder_GetFrame(*dec, 0, &frame))) {
        IWICBitmapDecoder_Release(*dec);
        return NULL;
    }
    return frame;
}

static void image_drop_pixels(WinImage *wi) {
    if (wi->bmp) {
        DeleteObject(wi->bmp);
        wi->bmp = NULL;
        W.image_bytes -= wi->bytes;
    }
}

static void image_evict(void) {
    while (W.image_bytes > IMAGE_CACHE_BYTES) {
        WinImage *oldest = NULL;
        for (guint i = 0; i < W.images->len; i++) {
            WinImage *wi = ((JotImage *)W.images->pdata[i])->platform;
            if (wi->bmp && (!oldest || wi->used < oldest->used))
                oldest = wi;
        }
        if (!oldest || oldest->used == W.clock)
            return; /* what's on screen stays */
        image_drop_pixels(oldest);
    }
}

/* The decoded pixels, loading them if this is the first time the image is drawn. */
static HBITMAP image_pixels(JotImage *img) {
    WinImage *wi = img->platform;
    wi->used = W.clock;
    if (wi->bmp)
        return wi->bmp;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = open_frame(img->path, &dec);
    if (!frame)
        return NULL;
    IWICFormatConverter *conv = NULL;
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), wi->w, -wi->h, 1, 32, BI_RGB, 0, 0, 0, 0, 0}, {{0, 0, 0, 0}}};
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    gsize bytes = (gsize)wi->w * wi->h * 4;
    if (bmp && SUCCEEDED(IWICImagingFactory_CreateFormatConverter(W.wic, &conv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppPBGRA,
                                                 WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(IWICFormatConverter_CopyPixels(conv, NULL, (UINT)wi->w * 4, (UINT)bytes, bits))) {
        wi->bmp = bmp;
        wi->bytes = bytes;
        W.image_bytes += bytes;
    } else if (bmp) {
        DeleteObject(bmp);
    }
    if (conv)
        IWICFormatConverter_Release(conv);
    IWICBitmapFrameDecode_Release(frame);
    IWICBitmapDecoder_Release(dec);
    image_evict();
    return wi->bmp;
}

JotImage *jot_image_ref(JotImage *img) {
    img->refs++;
    return img;
}

void jot_image_unref(JotImage *img) {
    img->refs--; /* W.images keeps one, so a page reload reuses it */
}

JotImage *load_image(const char *rel) {
    for (guint i = 0; i < W.images->len; i++) {
        JotImage *img = W.images->pdata[i];
        if (g_str_equal(img->path, rel))
            return jot_image_ref(img);
    }
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = open_frame(rel, &dec);
    if (!frame)
        return NULL;
    UINT w = 0, h = 0;
    IWICBitmapFrameDecode_GetSize(frame, &w, &h);
    IWICBitmapFrameDecode_Release(frame);
    IWICBitmapDecoder_Release(dec);
    if (!w || !h)
        return NULL;
    JotImage *img = g_new0(JotImage, 1);
    WinImage *wi = g_new0(WinImage, 1);
    wi->w = (int)w;
    wi->h = (int)h;
    img->refs = 2; /* the caller's, and W.images' */
    img->path = g_strdup(rel);
    img->platform = wi;
    g_ptr_array_add(W.images, img);
    return img;
}

static char *asset_name(void) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    return g_strdup_printf("%04d%02d%02d-%02d%02d%02d-%06x.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                           (unsigned)(GetTickCount() * 2654435761u) & 0xffffff);
}

/* Saves 32-bit BGRA pixels as a new asset; returns "assets/x.png" or NULL. */
static char *save_png(const void *bgra, int w, int h) {
    g_autofree char *name = asset_name();
    g_autofree char *abs = join(W.assets_dir, name);
    g_autofree wchar_t *wp = wide(abs);
    IWICStream *stream = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    gboolean ok = FALSE;
    if (SUCCEEDED(IWICImagingFactory_CreateStream(W.wic, &stream)) &&
        SUCCEEDED(IWICStream_InitializeFromFilename(stream, wp, GENERIC_WRITE)) &&
        SUCCEEDED(IWICImagingFactory_CreateEncoder(W.wic, &GUID_ContainerFormatPng, NULL, &enc)) &&
        SUCCEEDED(IWICBitmapEncoder_Initialize(enc, (IStream *)stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(enc, &frame, NULL)) &&
        SUCCEEDED(IWICBitmapFrameEncode_Initialize(frame, NULL)) &&
        SUCCEEDED(IWICBitmapFrameEncode_SetSize(frame, (UINT)w, (UINT)h))) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(IWICBitmapFrameEncode_SetPixelFormat(frame, &fmt)) &&
             IsEqualGUID(&fmt, &GUID_WICPixelFormat32bppBGRA) &&
             SUCCEEDED(IWICBitmapFrameEncode_WritePixels(frame, (UINT)h, (UINT)w * 4, (UINT)w * 4 * h, (BYTE *)bgra)) &&
             SUCCEEDED(IWICBitmapFrameEncode_Commit(frame)) && SUCCEEDED(IWICBitmapEncoder_Commit(enc));
    }
    if (frame)
        IWICBitmapFrameEncode_Release(frame);
    if (enc)
        IWICBitmapEncoder_Release(enc);
    if (stream)
        IWICStream_Release(stream);
    if (!ok) {
        DeleteFileW(wp);
        return NULL;
    }
    return g_strconcat("assets/", name, NULL);
}

void open_image(JotImage *img) {
    g_autofree char *abs = join(notes_dir, img->path);
    g_autofree wchar_t *w = wide(abs);
    for (wchar_t *p = w; *p; p++)
        if (*p == '/')
            *p = '\\';
    ShellExecuteW(W.hwnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}

/* OCR comes later. A page already read on Linux still searches, through its .txt sidecars. */
const char *ocr_lookup(const char *rel) {
    static GPtrArray *cache;
    if (!cache)
        cache = g_ptr_array_new_with_free_func(NULL);
    for (guint i = 0; i + 1 < cache->len; i += 2)
        if (g_str_equal(cache->pdata[i], rel))
            return cache->pdata[i + 1];
    g_autofree char *abs = join(notes_dir, rel);
    g_autofree char *side = g_strconcat(abs, ".txt", NULL);
    char *text = NULL;
    if (!read_file(side, &text, NULL))
        return NULL;
    g_ptr_array_add(cache, g_strdup(rel));
    g_ptr_array_add(cache, text);
    return text;
}

void ocr_request(const char *rel) {
    (void)rel;
}

guint ocr_pending_count(void) {
    return 0;
}

void image_set_highlight(JotImage *img, const char *q) {
    (void)img, (void)q;
    InvalidateRect(W.hwnd, NULL, FALSE);
}

/* ------------------------------------------------------------------------- */
/* Measuring                                                                  */
/* ------------------------------------------------------------------------- */

static int char_cols(gunichar c) {
    if (c < 0x1100)
        return 1;
    if (c <= 0x115F || c == 0x2329 || c == 0x232A || (c >= 0x2E80 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7A3) ||
        (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE6F) || (c >= 0xFF00 && c <= 0xFF60) ||
        (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1FAFF) || (c >= 0x20000 && c <= 0x3FFFD))
        return 2;
    return 1;
}

/* Lays out `line` into W.m: wraps after a space when it can, mid-word when it must. */
static void measure(int line) {
    Measured *m = &W.m;
    JotIter ls;
    jb_get_iter_at_line(J.buf, &ls, line);
    int len = jb_line_len(J.buf, line);
    if (len + 1 > m->cap) {
        m->cap = (len + 1) * 2;
        m->ch = realloc(m->ch, (gsize)m->cap * sizeof *m->ch);
        m->tags = realloc(m->tags, (gsize)m->cap * sizeof *m->tags);
        m->col = realloc(m->col, (gsize)m->cap * sizeof *m->col);
        m->row = realloc(m->row, (gsize)m->cap * sizeof *m->row);
    }
    m->n = jb_copy_line(J.buf, line, m->ch, m->tags, len);
    int col = 0, row = 0, row_start = 0, last_space = -1;
    for (int i = 0; i < m->n; i++) {
        int w = m->ch[i] == '\t' ? 4 - col % 4 : char_cols(m->ch[i]);
        if (col + w > W.cols && i > row_start) {
            int at = last_space >= row_start ? last_space + 1 : i;
            row++;
            row_start = at;
            last_space = -1;
            col = 0;
            i = at - 1;
            continue;
        }
        m->col[i] = col;
        m->row[i] = row;
        col += w;
        if (m->ch[i] == ' ' || m->ch[i] == '\t')
            last_space = i;
    }
    m->rows = row + 1;
}

static void image_fit(JotImage *img, int *w, int *h) {
    WinImage *wi = img->platform;
    int max_w = W.cols * W.cell_w;
    *w = MIN(wi->w, max_w);
    *h = MAX(1, (int)((gint64)wi->h * *w / MAX(wi->w, 1)));
}

static void layout(void) {
    if (!W.layout_dirty)
        return;
    W.layout_dirty = FALSE;
    int n = jb_get_line_count(J.buf);
    if (n + 1 > W.n_ly) {
        W.n_ly = (n + 1) * 2;
        W.ly = realloc(W.ly, (gsize)W.n_ly * sizeof *W.ly);
    }
    int y = PAD_TOP;
    for (int l = 0; l < n; l++) {
        W.ly[l] = y;
        JotIter it;
        jb_get_iter_at_line(J.buf, &it, l);
        JotImage *img = jb_iter_get_image(&it);
        int rows = 1;
        if (img) {
            int iw, ih;
            image_fit(img, &iw, &ih);
            y += ih + LINE_GAP;
            continue;
        }
        if (jb_line_len(J.buf, l) * 4 > W.cols) { /* it might wrap */
            measure(l);
            rows = W.m.rows;
        }
        y += rows * W.line_h;
    }
    W.ly[n] = y;
    W.total = y + PAD_BOTTOM;
}

static int view_h(void) {
    return MAX(1, W.client_h - W.bar_h);
}

static void set_scroll(int y) {
    W.scroll = CLAMP(y, 0, MAX(0, W.total - view_h()));
    SCROLLINFO si = {sizeof si, SIF_ALL, 0, W.total, (UINT)view_h(), W.scroll, 0};
    SetScrollInfo(W.hwnd, SB_VERT, &si, TRUE);
    InvalidateRect(W.hwnd, NULL, FALSE);
}

/* The page position of offset `off`, top of its row. */
static void offset_xy(int off, int *x, int *y) {
    layout();
    JotIter it;
    jb_get_iter_at_offset(J.buf, &it, off);
    int line = jb_iter_get_line(&it), col = jb_iter_get_line_offset(&it);
    *y = W.ly[line];
    *x = W.gutter_w + PAD_LEFT;
    if (jb_iter_get_image(&it))
        return;
    measure(line);
    if (col < W.m.n) {
        *x += W.m.col[col] * W.cell_w;
        *y += W.m.row[col] * W.line_h;
    } else if (W.m.n) {
        int i = W.m.n - 1, w = W.m.ch[i] == '\t' ? 4 - W.m.col[i] % 4 : char_cols(W.m.ch[i]);
        *x += (W.m.col[i] + w) * W.cell_w;
        *y += W.m.row[i] * W.line_h;
    }
}

static int line_height(int line) {
    return W.ly[line + 1] - W.ly[line];
}

static int line_at_y(int y) {
    int lo = 0, hi = jb_get_line_count(J.buf) - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (W.ly[mid] <= y)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

/* ------------------------------------------------------------------------- */
/* Platform: status bar, command line, view                                   */
/* ------------------------------------------------------------------------- */

static void repaint(void) {
    InvalidateRect(W.hwnd, NULL, FALSE);
}

gboolean ui_status_ready(void) { return TRUE; }

void ui_status_mode(const char *t, gboolean error) {
    snprintf(W.mode, sizeof W.mode, "%s", t);
    W.mode_error = error;
    repaint();
}

void ui_status_keys(const char *t) { snprintf(W.keys, sizeof W.keys, "%s", t); repaint(); }
void ui_status_pos(const char *t) { snprintf(W.pos, sizeof W.pos, "%s", t); repaint(); }
void ui_status_ocr(const char *t) { snprintf(W.ocr, sizeof W.ocr, "%s", t ? t : ""); repaint(); }

void toast(const char *msg) { vim_show_error(msg); }

void ui_cmd_open(const char *prefix, const char *text) {
    snprintf(W.cmd_prefix, sizeof W.cmd_prefix, "%s", prefix);
    g_string_truncate(W.cmd, 0);
    g_string_append(W.cmd, text);
    W.cmd_open = TRUE;
    repaint();
}

void ui_cmd_close(void) { W.cmd_open = FALSE; repaint(); }
gboolean ui_cmd_is_open(void) { return W.cmd_open; }
const char *ui_cmd_text(void) { return W.cmd->str; }

void ui_cmd_set_text(const char *text) {
    g_string_truncate(W.cmd, 0);
    g_string_append(W.cmd, text);
    vim_cmd_changed();
    repaint();
}

void ui_block_cursor(gboolean block) { W.block_cursor = block; repaint(); }
void ui_bell(void) { MessageBeep(MB_OK); }

void ui_scroll_iter_onscreen(const JotIter *it) {
    int x, y, off = jb_iter_get_offset(it);
    offset_xy(off, &x, &y);
    JotIter li = *it;
    int h = jb_iter_get_image(&li) ? line_height(jb_iter_get_line(&li)) : W.line_h;
    int margin = view_h() / 10;
    if (y < W.scroll + margin)
        set_scroll(y - margin);
    else if (y + h > W.scroll + view_h() - margin)
        set_scroll(y + h - view_h() + margin);
}

void ui_scroll_mark_onscreen(JotMark *mark) {
    JotIter it;
    jb_get_iter_at_mark(J.buf, &it, mark);
    int x, y;
    offset_xy(jb_iter_get_offset(&it), &x, &y);
    int h = jb_iter_get_image(&it) ? line_height(jb_iter_get_line(&it)) : W.line_h;
    if (y < W.scroll)
        set_scroll(y - PAD_TOP);
    else if (y + h > W.scroll + view_h())
        set_scroll(y + h - view_h());
}

void ui_scroll_mark_to(JotMark *mark, double yalign) {
    JotIter it;
    jb_get_iter_at_mark(J.buf, &it, mark);
    int x, y;
    offset_xy(jb_iter_get_offset(&it), &x, &y);
    set_scroll(y - (int)(yalign * (view_h() - W.line_h)));
}

void ui_scroll_lines(const JotIter *cursor, int dir, int count) {
    (void)cursor;
    set_scroll(W.scroll + dir * W.line_h * MAX(count, 1));
}

void ui_visible_lines(int *top, int *bottom) {
    layout();
    *top = line_at_y(W.scroll);
    *bottom = line_at_y(W.scroll + view_h() - 1);
}

double ui_iter_screen_pos(const JotIter *it) {
    int x, y;
    offset_xy(jb_iter_get_offset(it), &x, &y);
    return CLAMP((double)(y - W.scroll) / view_h(), 0.0, 1.0);
}

void ui_quit(void) { PostMessageW(W.hwnd, WM_CLOSE, 0, 0); }

void ui_open_folder(const char *path) {
    g_autofree wchar_t *w = wide(path);
    ShellExecuteW(W.hwnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}

/* Only web links: a page is just text, and a link in it shouldn't be able to start a program. */
void ui_open_uri(const char *uri) {
    if (!g_str_has_prefix(uri, "http://") && !g_str_has_prefix(uri, "https://") && !g_str_has_prefix(uri, "mailto:")) {
        ui_status_mode("Not opening that link: only http, https and mailto", TRUE);
        return;
    }
    g_autofree wchar_t *w = wide(uri);
    ShellExecuteW(W.hwnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}

void ui_md_style(JotTag **tags) { W.tags = tags; }

void after_insert(void) {
    W.layout_dirty = TRUE;
    ui_scroll_mark_onscreen(jb_get_insert(J.buf));
    repaint();
}

void set_line_numbers(gboolean show) {
    J.line_numbers = show;
    W.layout_dirty = TRUE;
    repaint();
}

/* The image the cursor is on (normal mode), or the one selected by itself. */
static JotImage *current_image(void) {
    JotIter s, e;
    if (jb_get_selection_bounds(J.buf, &s, &e))
        return jb_iter_get_offset(&e) - jb_iter_get_offset(&s) == 1 ? jb_iter_get_image(&s) : NULL;
    if (!vim_image_cursor_mode())
        return NULL;
    cursor_iter(&s);
    return jb_iter_get_image(&s);
}

void update_image_selection(void) {
    JotImage *img = current_image();
    if (img != W.outlined) {
        if (W.outlined)
            W.outlined->selected = FALSE;
        if (img)
            img->selected = TRUE;
        W.outlined = img;
        repaint();
    }
}

/* ------------------------------------------------------------------------- */
/* Clipboard                                                                  */
/* ------------------------------------------------------------------------- */

/* Text goes out with \r\n, as Windows expects, and comes back as \n. */
static void clip_put_text(const char *text) {
    GString *crlf = g_string_new(text);
    g_string_replace(crlf, "\n", "\r\n", 0);
    g_autofree wchar_t *w = wide(crlf->str);
    g_string_free(crlf, TRUE);
    gsize bytes = (wcslen(w) + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h)
        return;
    memcpy(GlobalLock(h), w, bytes);
    GlobalUnlock(h);
    if (!SetClipboardData(CF_UNICODETEXT, h))
        GlobalFree(h);
}

void clipboard_set(const char *text, JotImage *lone_image) {
    if (!OpenClipboard(W.hwnd))
        return;
    EmptyClipboard();
    clip_put_text(text);
    if (lone_image) {
        HBITMAP bmp = image_pixels(lone_image);
        if (bmp) {
            HDC dc = GetDC(NULL);
            BITMAP bm;
            GetObject(bmp, sizeof bm, &bm);
            BITMAPINFOHEADER bh = {sizeof bh, bm.bmWidth, bm.bmHeight, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
            gsize size = (gsize)bm.bmWidth * bm.bmHeight * 4;
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sizeof bh + size);
            if (h) {
                char *p = GlobalLock(h);
                memcpy(p, &bh, sizeof bh);
                GetDIBits(dc, bmp, 0, (UINT)bm.bmHeight, p + sizeof bh, (BITMAPINFO *)&bh, DIB_RGB_COLORS);
                GlobalUnlock(h);
                if (!SetClipboardData(CF_DIB, h))
                    GlobalFree(h);
            }
            ReleaseDC(NULL, dc);
        }
    }
    CloseClipboard();
}

/* The clipboard's image saved as an asset; NULL if there isn't one. Any bit depth becomes 32. */
static char *clip_image(void) {
    if (!IsClipboardFormatAvailable(CF_DIB))
        return NULL;
    HANDLE h = GetClipboardData(CF_DIB);
    BITMAPINFO *bi = h ? GlobalLock(h) : NULL;
    if (!bi)
        return NULL;
    BITMAPINFOHEADER *bh = &bi->bmiHeader;
    int w = bh->biWidth, height = abs(bh->biHeight);
    char *rel = NULL;
    if (w > 0 && height > 0 && (gsize)w * height <= 200000000u) {
        int colors = bh->biClrUsed ? (int)bh->biClrUsed : bh->biBitCount <= 8 ? 1 << bh->biBitCount : 0;
        int masks = bh->biCompression == BI_BITFIELDS && bh->biSize == sizeof(BITMAPINFOHEADER) ? 3 : 0;
        const char *px = (const char *)bi + bh->biSize + masks * sizeof(DWORD) + colors * sizeof(RGBQUAD);
        BITMAPINFO out = {{sizeof(BITMAPINFOHEADER), w, -height, 1, 32, BI_RGB, 0, 0, 0, 0, 0}, {{0, 0, 0, 0}}};
        void *bits = NULL;
        HBITMAP bmp = CreateDIBSection(NULL, &out, DIB_RGB_COLORS, &bits, NULL, 0);
        if (bmp) {
            HDC dc = CreateCompatibleDC(NULL);
            HGDIOBJ old = SelectObject(dc, bmp);
            SetDIBitsToDevice(dc, 0, 0, (DWORD)w, (DWORD)height, 0, 0, 0, (UINT)height, px, bi, DIB_RGB_COLORS);
            SelectObject(dc, old);
            DeleteDC(dc);
            GdiFlush();
            for (gsize i = 0, n = (gsize)w * height; i < n; i++)
                ((BYTE *)bits)[i * 4 + 3] = 0xFF; /* screenshots have no real alpha */
            rel = save_png(bits, w, height);
            DeleteObject(bmp);
        }
    }
    GlobalUnlock(h);
    return rel;
}

static char *clip_text(void) {
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *w = h ? GlobalLock(h) : NULL;
    if (!w)
        return NULL;
    char *s = narrow(w);
    GlobalUnlock(h);
    GString *g = g_string_new(s);
    g_free(s);
    g_string_replace(g, "\r\n", "\n", 0);
    return g_string_free(g, FALSE);
}

void ui_clipboard_read(void *request) {
    char *text = NULL, *image = NULL;
    gboolean has_image = FALSE;
    if (OpenClipboard(W.hwnd)) {
        if (IsClipboardFormatAvailable(CF_UNICODETEXT))
            text = clip_text();
        else if ((has_image = IsClipboardFormatAvailable(CF_DIB)))
            image = clip_image();
        CloseClipboard();
    }
    if (text)
        vim_put_text(request, text);
    else if (has_image)
        vim_put_image(request, image);
    else
        vim_put_none(request);
    g_free(text);
}

/* Ctrl+V in insert mode: text as typed, or the image on its own line. */
static void paste(void) {
    char *text = NULL, *image = NULL;
    if (!OpenClipboard(W.hwnd))
        return;
    if (IsClipboardFormatAvailable(CF_UNICODETEXT))
        text = clip_text();
    else
        image = clip_image();
    CloseClipboard();
    if (text) {
        if (W.cmd_open) {
            g_string_append(W.cmd, text);
            vim_cmd_changed();
        } else {
            jb_begin_user_action(J.buf);
            jb_delete_selection(J.buf, TRUE, TRUE);
            JotIter it;
            cursor_iter(&it);
            insert_text(J.buf, &it, text);
            jb_place_cursor(J.buf, &it);
            jb_end_user_action(J.buf);
            after_insert();
        }
        g_free(text);
    } else if (image) {
        g_autoptr(GPtrArray) rels = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(rels, image);
        insert_images(rels, NULL);
    }
}

/* ------------------------------------------------------------------------- */
/* The page on disk                                                           */
/* ------------------------------------------------------------------------- */

static void set_text(const char *text) {
    W.loading = TRUE;
    jb_begin_irreversible_action(J.buf);
    jb_set_text(J.buf, "", 0);
    JotIter it;
    jb_get_start_iter(J.buf, &it);
    insert_text(J.buf, &it, text);
    jb_end_irreversible_action(J.buf);
    W.loading = FALSE;
    g_free(W.last_saved);
    W.last_saved = serialize(J.buf);
    W.layout_dirty = TRUE;
}

void save(void) {
    KillTimer(W.hwnd, TIMER_SAVE);
    if (W.read_only)
        return;
    g_autofree char *text = serialize(J.buf);
    if (g_str_equal(text, W.last_saved))
        return;
    g_autofree char *contents = *text ? g_strconcat(text, "\n", NULL) : g_strdup("");
    if (!write_file(page_path, contents, strlen(contents))) {
        toast("Save failed");
        return;
    }
    W.mtime = file_mtime(page_path);
    g_free(W.last_saved);
    W.last_saved = g_steal_pointer(&text);
}

static gboolean valid_utf8(const char *s, gsize n) {
    for (gsize i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        if (!c)
            return FALSE;
        int k = c < 0x80 ? 1 : c >= 0xC2 && c < 0xE0 ? 2 : c >= 0xE0 && c < 0xF0 ? 3 : c >= 0xF0 && c < 0xF5 ? 4 : 0;
        if (!k || i + k > n)
            return FALSE;
        for (int j = 1; j < k; j++)
            if (((unsigned char)s[i + j] & 0xC0) != 0x80)
                return FALSE;
        i += (gsize)k;
    }
    return TRUE;
}

/* A file jot can't represent is shown as nothing and never written, so it can't be destroyed. */
static char *read_page(void) {
    char *text = NULL;
    gsize len = 0;
    W.read_only = FALSE;
    if (!read_file(page_path, &text, &len))
        return g_strdup("");
    if (!valid_utf8(text, len)) {
        g_free(text);
        W.read_only = TRUE;
        return g_strdup("");
    }
    if (len && text[len - 1] == '\n')
        text[len - 1] = 0;
    return text;
}

/* Says so in the title too: the status line only lasts until the next key. */
static void show_read_only(void) {
    SetWindowTextW(W.hwnd, W.read_only ? L"jot - NOT SAVING: jot.md isn't text jot can read" : L"jot");
    if (W.read_only)
        toast("jot.md isn't UTF-8 text: not loaded, and jot won't save over it");
}

void load_page(void) {
    g_autofree char *text = read_page();
    W.mtime = file_mtime(page_path);
    set_text(text);
    JotIter end;
    jb_get_end_iter(J.buf, &end);
    jb_place_cursor(J.buf, &end);
    after_insert();
}

/* Back at the window: pick up edits made elsewhere, unless there's unsaved typing. */
static void on_activate(gboolean active) {
    if (!active) {
        save();
        return;
    }
    gint64 mtime = file_mtime(page_path);
    g_autofree char *current = serialize(J.buf);
    if (mtime < 0 || mtime == W.mtime || !g_str_equal(current, W.last_saved))
        return;
    JotIter it;
    cursor_iter(&it);
    int offset = jb_iter_get_offset(&it);
    g_autofree char *text = read_page();
    W.mtime = mtime;
    set_text(text);
    jb_get_iter_at_offset(J.buf, &it, offset);
    jb_place_cursor(J.buf, &it);
    show_read_only();
    repaint();
}

static void on_changed(JotBuf *buf, gpointer unused) {
    (void)buf, (void)unused;
    W.layout_dirty = TRUE;
    repaint();
    if (!W.loading)
        SetTimer(W.hwnd, TIMER_SAVE, SAVE_DELAY_MS, NULL);
}

/* ---- Window state: a few key=value lines ---------------------------------- */

static int state_int(const char *state, const char *key, int fallback) {
    g_autofree char *needle = g_strdup_printf("%s=", key);
    gsize n = strlen(needle);
    for (const char *p = state; p && *p;) {
        if (strncmp(p, needle, n) == 0)
            return atoi(p + n);
        p = strchr(p, '\n');
        p = p ? p + 1 : NULL;
    }
    return fallback;
}

static void save_state(void) {
    WINDOWPLACEMENT wp = {.length = sizeof wp};
    GetWindowPlacement(W.hwnd, &wp);
    RECT r = wp.rcNormalPosition;
    g_autofree char *s = g_strdup_printf("width=%d\nheight=%d\nmaximized=%d\nfont_size=%d\nline_numbers=%d\nmarkdown=%d\n",
                                         (int)MulDiv(r.right - r.left, 96, W.dpi), (int)MulDiv(r.bottom - r.top, 96, W.dpi), wp.showCmd == SW_SHOWMAXIMIZED, W.font_pt,
                                         J.line_numbers, md_enabled());
    write_file(W.state_path, s, strlen(s));
}

/* ------------------------------------------------------------------------- */
/* Painting                                                                   */
/* ------------------------------------------------------------------------- */

static COLORREF mix(COLORREF a, COLORREF b, int pct) {
    int p = pct, q = 100 - pct;
    return RGB((GetRValue(a) * p + GetRValue(b) * q) / 100, (GetGValue(a) * p + GetGValue(b) * q) / 100,
               (GetBValue(a) * p + GetBValue(b) * q) / 100);
}

static void set_colors(void) {
    DWORD light = 1, size = sizeof light;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &size);
    W.dark = !light;
    W.bg = W.dark ? RGB(0x1e, 0x1e, 0x20) : RGB(0xfd, 0xfd, 0xfc);
    W.ink = W.dark ? RGB(0xe8, 0xe8, 0xe8) : RGB(0x20, 0x20, 0x20);
    W.accent = W.dark ? RGB(0x78, 0xae, 0xed) : RGB(0x1c, 0x5f, 0xb8);
    W.err = W.dark ? RGB(0xff, 0x80, 0x80) : RGB(0xc0, 0x1c, 0x28);
    W.dim = mix(W.ink, W.bg, 40);
    W.soft = mix(W.ink, W.bg, 70);
    W.chip = mix(W.ink, W.bg, W.dark ? 10 : 7);
    W.block = mix(W.ink, W.bg, W.dark ? 6 : 4);
    W.sel = mix(W.accent, W.bg, 30);
    W.bar = mix(W.ink, W.bg, W.dark ? 8 : 5);
    W.rule = mix(W.ink, W.bg, 10);
}

static void set_fonts(void) {
    int height = -MulDiv(W.font_pt, W.dpi, 72);
    for (int i = 0; i < 4; i++) {
        if (W.font[i])
            DeleteObject(W.font[i]);
        W.font[i] = CreateFontW(height, 0, 0, 0, i & 1 ? FW_BOLD : FW_NORMAL, (i & 2) != 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    }
    HDC dc = GetDC(W.hwnd);
    HGDIOBJ old = SelectObject(dc, W.font[0]);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SIZE s;
    GetTextExtentPoint32W(dc, L"M", 1, &s);
    SelectObject(dc, old);
    ReleaseDC(W.hwnd, dc);
    W.cell_w = MAX(1, s.cx);
    W.line_h = tm.tmHeight + LINE_GAP;
    W.bar_h = W.line_h + px(8);
}

static void resized(void) {
    int digits = 1;
    for (int n = jb_get_line_count(J.buf); n >= 10; n /= 10)
        digits++;
    W.gutter_w = J.line_numbers ? (MAX(digits, 3) + 1) * W.cell_w + px(8) : 0;
    W.cols = MAX(8, (W.client_w - W.gutter_w - PAD_LEFT - PAD_RIGHT) / W.cell_w);
    W.layout_dirty = TRUE;
}

static void fill(HDC dc, int x, int y, int w, int h, COLORREF c) {
    RECT r = {x, y, x + w, y + h};
    COLORREF old = SetBkColor(dc, c);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &r, NULL, 0, NULL);
    SetBkColor(dc, old);
}

static void outline(HDC dc, int x, int y, int w, int h, int t, COLORREF c) {
    fill(dc, x, y, w, t, c);
    fill(dc, x, y + h - t, w, t, c);
    fill(dc, x, y, t, h, c);
    fill(dc, x + w - t, y, t, h, c);
}

static void draw_char(HDC dc, gunichar c, int x, int y, COLORREF color) {
    wchar_t w[2];
    int n = 1;
    if (c >= 0x10000) {
        c -= 0x10000;
        w[0] = (wchar_t)(0xD800 | (c >> 10));
        w[1] = (wchar_t)(0xDC00 | (c & 0x3FF));
        n = 2;
    } else {
        w[0] = (wchar_t)c;
    }
    SetTextColor(dc, color);
    TextOutW(dc, x, y, w, n);
}

static gboolean has(unsigned tags, int t) {
    return (tags >> t) & 1;
}

static COLORREF tag_color(unsigned tags) {
    if (has(tags, T_DIM) || has(tags, T_DONE))
        return W.dim;
    if (has(tags, T_LINK) || has(tags, T_HEADING) || has(tags, T_BULLET) || has(tags, T_QUOTE_MARK))
        return W.accent;
    if (has(tags, T_QUOTE))
        return W.soft;
    return W.ink;
}

static void draw_text_line(HDC dc, int line, int top, int sel_s, int sel_e, int cursor) {
    measure(line);
    Measured *m = &W.m;
    int ls = 0;
    JotIter it;
    jb_get_iter_at_line(J.buf, &it, line);
    ls = jb_iter_get_offset(&it);
    int x0 = W.gutter_w + PAD_LEFT;
    gboolean code_block = has(jb_tags_at(J.buf, ls), T_CODEBLOCK);
    if (m->n == 0)
        code_block = has(jb_tags_at(J.buf, ls), T_CODEBLOCK); /* the newline itself carries it */
    if (code_block)
        fill(dc, W.gutter_w, top, W.client_w - W.gutter_w, m->rows * W.line_h, W.block);
    if (sel_s < sel_e && ls >= sel_s && ls < sel_e && m->n == 0)
        fill(dc, x0, top, W.cell_w, W.line_h, W.sel); /* an empty line in the selection */
    gboolean cursor_drawn = FALSE;
    for (int i = 0; i < m->n; i++) {
        gunichar c = m->ch[i];
        unsigned t = m->tags[i];
        int x = x0 + m->col[i] * W.cell_w, y = top + m->row[i] * W.line_h;
        int w = (c == '\t' ? 4 - m->col[i] % 4 : char_cols(c)) * W.cell_w;
        int off = ls + i;
        gboolean selected = off >= sel_s && off < sel_e, on_cursor = off == cursor;
        if (selected)
            fill(dc, x, y, w, W.line_h - LINE_GAP, W.sel);
        else if (has(t, T_CODE))
            fill(dc, x, y, w, W.line_h - LINE_GAP, W.chip);
        COLORREF color = tag_color(t);
        if (on_cursor && W.block_cursor) {
            fill(dc, x, y, w, W.line_h - LINE_GAP, W.ink);
            color = W.bg;
            cursor_drawn = TRUE;
        }
        SelectObject(dc, W.font[(has(t, T_BOLD) || has(t, T_HEADING) ? 1 : 0) | (has(t, T_ITALIC) ? 2 : 0)]);
        if (c != '\t' && c != ' ')
            draw_char(dc, c, x, y, color);
        if (has(t, T_LINK))
            fill(dc, x, y + W.line_h - LINE_GAP - px(2), w, px(1), W.accent);
        if (has(t, T_DONE))
            fill(dc, x, y + (W.line_h - LINE_GAP) / 2, w, px(1), W.dim);
        if (on_cursor && !W.block_cursor) {
            fill(dc, x, y, px(2), W.line_h - LINE_GAP, W.ink);
            cursor_drawn = TRUE;
        }
    }
    if (!cursor_drawn && cursor >= ls && cursor <= ls + m->n) {
        int i = m->n - 1, col = m->n ? m->col[i] + (m->ch[i] == '\t' ? 4 - m->col[i] % 4 : char_cols(m->ch[i])) : 0;
        int x = x0 + col * W.cell_w, y = top + (m->n ? m->row[i] : 0) * W.line_h;
        fill(dc, x, y, W.block_cursor ? W.cell_w : px(2), W.line_h - LINE_GAP, W.ink);
    }
}

static void draw_image(HDC dc, HDC mem, JotImage *img, int x, int y) {
    int iw, ih;
    image_fit(img, &iw, &ih);
    WinImage *wi = img->platform;
    HBITMAP bmp = image_pixels(img);
    if (bmp) {
        HGDIOBJ old = SelectObject(mem, bmp);
        SetStretchBltMode(dc, HALFTONE);
        StretchBlt(dc, x, y, iw, ih, mem, 0, 0, wi->w, wi->h, SRCCOPY);
        SelectObject(mem, old);
    } else {
        outline(dc, x, y, iw, ih, px(1), W.dim);
    }
    if (img->selected)
        outline(dc, x - px(2), y - px(2), iw + px(4), ih + px(4), px(2), W.accent);
}

static void draw_status(HDC dc) {
    int y = W.client_h - W.bar_h, ty = y + px(4);
    fill(dc, 0, y, W.client_w, W.bar_h, W.bar);
    SelectObject(dc, W.font[0]);
    int x = px(10);
    g_autofree char *left = W.cmd_open ? g_strconcat(W.cmd_prefix, W.cmd->str, NULL) : g_strdup(W.mode);
    COLORREF color = W.mode_error && !W.cmd_open ? W.err : W.ink;
    int cols = 0;
    for (const char *p = left; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        draw_char(dc, c, x + cols * W.cell_w, ty, color);
        cols += char_cols(c);
    }
    if (W.cmd_open)
        fill(dc, x + cols * W.cell_w, ty, px(2), W.line_h - LINE_GAP, W.ink);
    int r = W.client_w - px(10);
    const char *right[3] = {W.pos, W.ocr, W.keys};
    COLORREF rc[3] = {W.ink, W.dim, W.dim};
    for (int i = 0; i < 3; i++) {
        int n = (int)g_utf8_strlen(right[i], -1);
        if (!n)
            continue;
        r -= n * W.cell_w;
        int c = 0;
        for (const char *p = right[i]; *p; p = g_utf8_next_char(p))
            draw_char(dc, g_utf8_get_char(p), r + c++ * W.cell_w, ty, rc[i]);
        r -= 2 * W.cell_w;
    }
}

static void paint(HDC dc) {
    lite_idle_run();
    W.clock++;
    layout();
    int bottom = view_h();
    fill(dc, 0, 0, W.client_w, W.client_h, W.bg);
    SetBkMode(dc, TRANSPARENT);
    JotIter c, s, e;
    get_vim_cursor(&c);
    int cursor = W.cmd_open ? -1 : jb_iter_get_offset(&c), cur_line = jb_iter_get_line(&c);
    int sel_s = 0, sel_e = 0;
    if (jb_get_selection_bounds(J.buf, &s, &e)) {
        sel_s = jb_iter_get_offset(&s);
        sel_e = jb_iter_get_offset(&e);
    }
    HDC mem = CreateCompatibleDC(dc);
    int n = jb_get_line_count(J.buf);
    if (J.line_numbers)
        fill(dc, W.gutter_w - px(1), 0, px(1), bottom, W.rule);
    for (int l = line_at_y(W.scroll); l < n; l++) {
        int y = W.ly[l] - W.scroll;
        if (y >= bottom)
            break;
        if (J.line_numbers) {
            char num[16];
            int len = snprintf(num, sizeof num, "%d", l + 1);
            SelectObject(dc, W.font[0]);
            for (int i = 0; i < len; i++)
                draw_char(dc, (gunichar)num[i], W.gutter_w - px(8) - (len - i) * W.cell_w, y, l == cur_line ? W.ink : W.dim);
        }
        JotIter it;
        jb_get_iter_at_line(J.buf, &it, l);
        JotImage *img = jb_iter_get_image(&it);
        if (img)
            draw_image(dc, mem, img, W.gutter_w + PAD_LEFT, y);
        else
            draw_text_line(dc, l, y, sel_s, sel_e, cursor);
    }
    DeleteDC(mem);
    draw_status(dc);
}

static void on_paint(void) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(W.hwnd, &ps);
    HDC back = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, W.client_w, W.client_h);
    HGDIOBJ old = SelectObject(back, bmp);
    paint(back);
    BitBlt(dc, 0, 0, W.client_w, W.client_h, back, 0, 0, SRCCOPY);
    SelectObject(back, old);
    DeleteObject(bmp);
    DeleteDC(back);
    EndPaint(W.hwnd, &ps);
}

/* ------------------------------------------------------------------------- */
/* Input                                                                      */
/* ------------------------------------------------------------------------- */

static void input_done(void) {
    lite_idle_run();
    resized();
    set_scroll(W.scroll);
}

static gboolean key_down(int vk) {
    return (GetKeyState(vk) & 0x8000) != 0;
}

static JotKey key_of(WPARAM vk) {
    switch (vk) {
    case VK_ESCAPE: return JK_ESC;
    case VK_RETURN: return JK_RETURN;
    case VK_BACK: return JK_BACKSPACE;
    case VK_TAB: return JK_TAB;
    case VK_LEFT: return JK_LEFT;
    case VK_RIGHT: return JK_RIGHT;
    case VK_UP: return JK_UP;
    case VK_DOWN: return JK_DOWN;
    case VK_HOME: return JK_HOME;
    case VK_END: return JK_END;
    case VK_PRIOR: return JK_PAGE_UP;
    case VK_NEXT: return JK_PAGE_DOWN;
    case VK_DELETE: return JK_DELETE;
    case VK_INSERT: return JK_INSERT;
    default: return JK_NONE;
    }
}

static void zoom(int d) {
    W.font_pt = d == 0 ? DEFAULT_FONT_PT : CLAMP(W.font_pt + d, 7, 32);
    set_fonts();
    resized();
    repaint();
}

/* Moves the insert-mode cursor. */
static void insert_move(JotKey key) {
    JotIter it;
    cursor_iter(&it);
    switch (key) {
    case JK_LEFT: jb_iter_backward_char(&it); break;
    case JK_RIGHT: jb_iter_forward_char(&it); break;
    case JK_HOME: jb_iter_set_line_offset(&it, 0); break;
    case JK_END:
        if (!jb_iter_ends_line(&it))
            jb_iter_forward_to_line_end(&it);
        break;
    case JK_UP:
    case JK_DOWN: {
        int col = jb_iter_get_line_offset(&it), line = jb_iter_get_line(&it) + (key == JK_UP ? -1 : 1);
        if (line < 0 || line >= jb_get_line_count(J.buf))
            return;
        jb_get_iter_at_line(J.buf, &it, line);
        for (int i = 0; i < col && !jb_iter_ends_line(&it); i++)
            jb_iter_forward_char(&it);
        break;
    }
    default: return;
    }
    jb_place_cursor(J.buf, &it);
    ui_scroll_mark_onscreen(jb_get_insert(J.buf));
}

/* Insert mode's Backspace and Delete. */
static void insert_erase(gboolean forward) {
    if (jb_delete_selection(J.buf, TRUE, TRUE))
        return;
    JotIter a, b;
    cursor_iter(&a);
    b = a;
    if (forward ? jb_iter_is_end(&a) : jb_iter_is_start(&a))
        return;
    if (forward)
        jb_iter_forward_char(&b);
    else
        jb_iter_backward_char(&a);
    jb_delete(J.buf, &a, &b);
}

static void on_key(WPARAM vk) {
    gboolean alt = key_down(VK_MENU);
    gboolean ctrl = (W.test_ctrl || key_down(VK_CONTROL)) && !alt; /* AltGr types, it isn't Ctrl */
    gboolean shift = W.test_shift || key_down(VK_SHIFT);
    JotKey key = key_of(vk);
    gunichar ch = 0;
    if (ctrl) {
        if (vk >= 'A' && vk <= 'Z')
            ch = (gunichar)(vk + 32);
        else if (vk == VK_OEM_4)
            ch = '[';
        if (ch && key == JK_NONE)
            key = JK_CHAR;
    }
    if (ctrl && vk == VK_OEM_PLUS) { zoom(1); return; }
    if (ctrl && vk == VK_OEM_MINUS) { zoom(-1); return; }
    if (ctrl && vk == '0') { zoom(0); return; }
    if (key == JK_NONE)
        return;
    if (W.cmd_open) {
        if (vim_cmd_key(key, ch, ctrl))
            return;
        if (key == JK_RETURN) {
            vim_cmd_activate();
        } else if (key == JK_BACKSPACE && W.cmd->len) {
            gsize n = W.cmd->len - 1;
            while (n && (W.cmd->str[n] & 0xC0) == 0x80)
                n--;
            g_string_truncate(W.cmd, n);
            vim_cmd_changed();
        } else if (ctrl && ch == 'v') {
            paste();
        }
        return;
    }
    if (vim_key(key, ch, ctrl))
        return;
    if (ctrl) {
        if (ch == 's')
            save();
        else if (ch == 'q')
            ui_quit();
        else if (ch == 'v')
            paste();
        return;
    }
    switch (key) {
    case JK_RETURN: edit_return(); break;
    case JK_TAB: edit_tab(shift); break;
    case JK_BACKSPACE: insert_erase(FALSE); break;
    case JK_DELETE: insert_erase(TRUE); break;
    default: insert_move(key); break;
    }
}

static void on_char(wchar_t wc) {
    gunichar c = wc;
    if (wc >= 0xD800 && wc < 0xDC00) {
        W.high_surrogate = wc;
        return;
    }
    if (wc >= 0xDC00 && wc < 0xE000) {
        c = 0x10000 + (((gunichar)W.high_surrogate - 0xD800) << 10) + (wc - 0xDC00u);
        W.high_surrogate = 0;
    }
    if (c < 0x20 || c == 0x7F)
        return; /* Esc, Enter and Ctrl+letter arrive through on_key */
    char b[8] = {0};
    g_unichar_to_utf8(c, b);
    if (W.cmd_open) {
        g_string_append(W.cmd, b);
        vim_cmd_changed();
    } else if (!vim_key(JK_CHAR, c, FALSE)) {
        edit_type(b);
    }
}

/* The offset under a window position. */
static int offset_at(int wx, int wy) {
    layout();
    int y = wy + W.scroll, line = line_at_y(y);
    JotIter it;
    jb_get_iter_at_line(J.buf, &it, line);
    int ls = jb_iter_get_offset(&it);
    if (jb_iter_get_image(&it))
        return ls;
    measure(line);
    int row = MAX(0, (y - W.ly[line]) / W.line_h);
    int col = (wx - W.gutter_w - PAD_LEFT + W.cell_w / 2) / W.cell_w;
    int found = -1, end_of_row = -1;
    for (int i = 0; i < W.m.n; i++) {
        if (W.m.row[i] != row)
            continue;
        end_of_row = i;
        if (W.m.col[i] <= col)
            found = i;
    }
    if (found < 0)
        found = end_of_row >= 0 ? end_of_row : W.m.n;
    else if (found == end_of_row && col > W.m.col[found])
        found++;
    return ls + MIN(found, W.m.n);
}

static void on_click(int x, int y, gboolean dbl) {
    JotIter it;
    jb_get_iter_at_offset(J.buf, &it, offset_at(x, y));
    if (W.test_ctrl || key_down(VK_CONTROL)) {
        if (md_open_url_at(&it))
            return;
    }
    if (dbl) {
        JotImage *img = jb_iter_get_image(&it);
        if (img) {
            open_image(img);
            return;
        }
    }
    vim_move_cursor(&it);
}

/* ------------------------------------------------------------------------- */
/* The window                                                                 */
/* ------------------------------------------------------------------------- */

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        on_paint();
        return 0;
    case WM_SIZE:
        W.client_w = LOWORD(lp);
        W.client_h = HIWORD(lp);
        resized();
        set_scroll(W.scroll);
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (msg == WM_SYSKEYDOWN && wp != VK_RETURN)
            break;
        on_key(wp);
        input_done();
        return 0;
    case WM_CHAR:
        on_char((wchar_t)wp);
        input_done();
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        SetFocus(h);
        on_click(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), msg == WM_LBUTTONDBLCLK);
        input_done();
        return 0;
    case WM_MOUSEWHEEL:
        layout();
        set_scroll(W.scroll - GET_WHEEL_DELTA_WPARAM(wp) * 3 * W.line_h / WHEEL_DELTA);
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO si = {sizeof si, SIF_ALL, 0, 0, 0, 0, 0};
        GetScrollInfo(h, SB_VERT, &si);
        int y = W.scroll;
        switch (LOWORD(wp)) {
        case SB_LINEUP: y -= W.line_h; break;
        case SB_LINEDOWN: y += W.line_h; break;
        case SB_PAGEUP: y -= view_h(); break;
        case SB_PAGEDOWN: y += view_h(); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: y = si.nTrackPos; break;
        }
        set_scroll(y);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_SAVE)
            save();
        return 0;
    case WM_ACTIVATE:
        on_activate(LOWORD(wp) != WA_INACTIVE);
        return 0;
    case WM_SETTINGCHANGE:
        set_colors();
        repaint();
        return 0;
    case WM_DPICHANGED: {
        W.dpi = HIWORD(wp);
        set_fonts();
        const RECT *r = (const RECT *)lp;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        resized(); /* the suggested size can be the size it already has: no WM_SIZE then */
        layout();
        set_scroll(W.scroll);
        return 0;
    }
    case WM_CLOSE:
        save();
        save_state();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ------------------------------------------------------------------------- */
/* Paths                                                                      */
/* ------------------------------------------------------------------------- */

static char *known_folder(const KNOWNFOLDERID *id) {
    PWSTR p = NULL;
    char *s = SUCCEEDED(SHGetKnownFolderPath(id, 0, NULL, &p)) ? narrow(p) : g_strdup(".");
    CoTaskMemFree(p);
    return s;
}

/* `jot` -> %JOT_DIR% or Documents\jot\default; `jot work` -> Documents\jot\work; anything that
 * looks like a path -> that folder. */
static char *resolve_dir(const char *arg) {
    g_autofree char *docs = known_folder(&FOLDERID_Documents);
    g_autofree char *root = join(docs, "jot");
    wchar_t env[1024];
    if ((!arg || !*arg) && GetEnvironmentVariableW(L"JOT_DIR", env, 1024) && *env)
        return narrow(env);
    if (!arg || !*arg)
        return join(root, "default");
    if (!strpbrk(arg, "/\\:") && !g_str_equal(arg, ".") && !g_str_equal(arg, ".."))
        return join(root, arg);
    g_autofree wchar_t *w = wide(arg);
    wchar_t full[1024];
    return GetFullPathNameW(w, 1024, full, NULL) ? narrow(full) : g_strdup(arg);
}

static unsigned hash(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static void init_paths(const char *arg) {
    notes_dir = resolve_dir(arg);
    W.assets_dir = join(notes_dir, "assets");
    page_path = join(notes_dir, "jot.md");
    g_autofree char *cfg = known_folder(&FOLDERID_RoamingAppData);
    g_autofree char *cfg_dir = join(cfg, "jot");
    W.state_path = join(cfg_dir, "state.ini");
    make_dirs(cfg_dir);
    gboolean fresh = !file_exists(page_path);
    make_dirs(W.assets_dir);
    if (fresh) {
        g_autofree char *agents = join(notes_dir, "AGENTS.md");
        g_autofree char *claude = join(notes_dir, "CLAUDE.md");
        if (!file_exists(agents))
            write_file(agents, jot_agents_md, strlen(jot_agents_md));
        if (!file_exists(claude)) /* symlinks need admin rights here: a copy */
            write_file(claude, jot_agents_md, strlen(jot_agents_md));
    }
}

/* ------------------------------------------------------------------------- */
/* Start                                                                      */
/* ------------------------------------------------------------------------- */

static void init_engine(void) {
    W.cmd = g_string_new(NULL);
    W.images = g_ptr_array_new_with_free_func(NULL);
    W.last_saved = g_strdup("");
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&W.wic);
    J.buf = jb_new();
    g_autofree char *state = NULL;
    read_file(W.state_path, &state, NULL);
    W.font_pt = state_int(state, "font_size", DEFAULT_FONT_PT);
    J.line_numbers = state_int(state, "line_numbers", 1);
    set_colors();
    set_fonts();
    resized();
    md_init();
    md_set_enabled(state_int(state, "markdown", 1));
    jb_connect(J.buf, "changed", on_changed, NULL);
    load_page();
    vim_init();
    show_read_only();
}

static void test_clipboard_image(void) {
    enum { IW = 64, IH = 32, STRIDE = (IW * 3 + 3) & ~3 };
    gsize size = sizeof(BITMAPINFOHEADER) + (gsize)STRIDE * IH;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
    BITMAPINFOHEADER *bh = GlobalLock(h);
    *bh = (BITMAPINFOHEADER){sizeof *bh, IW, IH, 1, 24, BI_RGB, 0, 0, 0, 0, 0};
    BYTE *px = (BYTE *)(bh + 1);
    for (int y = 0; y < IH; y++) /* bottom-up: a red band across the top of the picture */
        for (int x = 0; x < IW; x++) {
            BYTE *p = px + (IH - 1 - y) * STRIDE + x * 3;
            p[0] = y < 8 ? 0 : 200, p[1] = y < 8 ? 0 : 200, p[2] = y < 8 ? 255 : 200;
        }
    GlobalUnlock(h);
    OpenClipboard(W.hwnd);
    EmptyClipboard();
    SetClipboardData(CF_DIB, h);
    CloseClipboard();
}

/* --selftest: every key goes through the real window procedure. \c = next key with Ctrl, \d = put
 * a screenshot on the clipboard, \D = double the display scale. */
static int selftest(const char *bmp_path, const char *out_path, const char *keys) {
    DWORD t0 = GetTickCount();
    SetWindowPos(W.hwnd, NULL, 0, 0, px(900), px(620), SWP_NOMOVE | SWP_NOZORDER);
    SendMessageW(W.hwnd, WM_SIZE, 0, MAKELPARAM(px(900), px(620)));
    for (const char *p = keys; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (c == '\\' && p[1] == 'c') {
            W.test_ctrl = TRUE;
            p++;
            continue;
        }
        if (c == '\\' && p[1] == 'D') { /* moved to a monitor at twice the scale, same window size */
            RECT r;
            GetWindowRect(W.hwnd, &r);
            SendMessageW(W.hwnd, WM_DPICHANGED, MAKEWPARAM(W.dpi * 2, W.dpi * 2), (LPARAM)&r);
            p++;
            continue;
        }
        if (c == '\\' && p[1] == 'd') { /* a 24-bit screenshot on the clipboard */
            test_clipboard_image();
            p++;
            continue;
        }
        if (W.test_ctrl) {
            SendMessageW(W.hwnd, WM_KEYDOWN, (WPARAM)(c >= 'a' && c <= 'z' ? c - 32 : c), 0);
            W.test_ctrl = FALSE;
        } else if (c == 0x1b || c == '\r' || c == '\t' || c == 8) {
            WPARAM vk = c == 0x1b ? VK_ESCAPE : c == '\r' ? VK_RETURN : c == '\t' ? VK_TAB : VK_BACK;
            SendMessageW(W.hwnd, WM_KEYDOWN, vk, 0);
        } else {
            wchar_t w[2];
            int n = 1;
            if (c >= 0x10000) {
                w[0] = (wchar_t)(0xD800 | ((c - 0x10000) >> 10));
                w[1] = (wchar_t)(0xDC00 | ((c - 0x10000) & 0x3FF));
                n = 2;
            } else {
                w[0] = (wchar_t)c;
            }
            for (int i = 0; i < n; i++)
                SendMessageW(W.hwnd, WM_CHAR, w[i], 0);
        }
    }
    save();
    HDC screen = GetDC(NULL);
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), W.client_w, -W.client_h, 1, 32, BI_RGB, 0, 0, 0, 0, 0}, {{0, 0, 0, 0}}};
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HDC dc = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(dc, bmp);
    paint(dc);
    GdiFlush();
    gsize size = (gsize)W.client_w * W.client_h * 4;
    BITMAPFILEHEADER fh = {0x4D42, (DWORD)(sizeof fh + sizeof bi.bmiHeader + size), 0, 0, sizeof fh + sizeof bi.bmiHeader};
    GString *file = g_string_new(NULL);
    g_string_append_len(file, (const char *)&fh, sizeof fh);
    g_string_append_len(file, (const char *)&bi.bmiHeader, sizeof bi.bmiHeader);
    g_string_append_len(file, bits, size);
    write_file(bmp_path, file->str, file->len);
    g_string_free(file, TRUE);
    SelectObject(dc, old);
    g_autofree char *text = serialize(J.buf);
    g_autofree char *report = g_strdup_printf("text=[%s]\nstatus=[%s]\npos=[%s]\nsize=%dx%d cell=%dx%d dpi=%d cols=%d\nimages_loaded=%u bytes=%zu ms=%lu\n", text,
                                              W.mode, W.pos, W.client_w, W.client_h, W.cell_w, W.line_h, W.dpi, W.cols, W.images->len, W.image_bytes, GetTickCount() - t0);
    write_file(out_path, report, strlen(report));
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)prev, (void)cmdline;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    gboolean test = argc > 4 && wcscmp(argv[1], L"--selftest") == 0;
    g_autofree char *arg = !test && argc > 1 ? narrow(argv[1]) : NULL;
    init_paths(arg);

    /* One window per folder: a second `jot work` raises the first. */
    wchar_t cls[64];
    swprintf(cls, 64, L"dev.jot.Jot.%08x", hash(notes_dir));
    g_autofree wchar_t *title_dir = wide(notes_dir);
    if (!test) {
        HWND other = FindWindowW(cls, NULL);
        if (other) {
            if (IsIconic(other))
                ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
            return 0;
        }
    }
    WNDCLASSW wc = {.style = CS_DBLCLKS, .lpfnWndProc = wndproc, .hInstance = inst, .lpszClassName = cls,
                    .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_IBEAM), .hIcon = LoadIconW(inst, L"APP")};
    RegisterClassW(&wc);
    W.dpi = 96;
    g_autofree char *state = NULL;
    read_file(W.state_path, &state, NULL);
    int w = state_int(state, "width", 1000), h = state_int(state, "height", 760);
    W.hwnd = CreateWindowExW(0, cls, L"jot", WS_OVERLAPPEDWINDOW | WS_VSCROLL, CW_USEDEFAULT, CW_USEDEFAULT, w, h, NULL, NULL, inst, NULL);
    if (!W.hwnd)
        return 1;
    W.dpi = (int)GetDpiForWindow(W.hwnd);
    if (!W.dpi)
        W.dpi = 96;
    SetWindowPos(W.hwnd, NULL, 0, 0, px(w), px(h), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    RECT rc;
    GetClientRect(W.hwnd, &rc);
    W.client_w = rc.right;
    W.client_h = rc.bottom;
    init_engine();
    (void)title_dir;
    if (test) {
        g_autofree char *b = narrow(argv[2]), *o = narrow(argv[3]), *k = narrow(argv[4]);
        return selftest(b, o, k);
    }
    ShowWindow(W.hwnd, state_int(state, "maximized", 0) ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(W.hwnd);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
