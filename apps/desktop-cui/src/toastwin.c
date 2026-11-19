/* Cartão de aviso FORA da janela do Briks (estilo Steam): uma janela própria, sem borda, sempre no topo, que não rouba o foco,
 * no canto de baixo à direita da área útil do monitor principal. Aparece com o Briks em segundo plano, minimizado ou na bandeja.
 *  - desenho: tudo em software num DIB de 32 bits (cantos arredondados, foto redonda, bolinha verde) + texto do GDI; o canal alfa
 *    sai de uma distância ao retângulo arredondado, então a borda fica suave e nada vaza com cantos retos;
 *  - foto: decodificada pelo WIC (PNG/JPEG do cache do core), recortada em quadrado e reduzida por média de área; sem foto = inicial;
 *  - animação: sobe e aparece em 260 ms, fica 6 s (o mouse em cima segura), some em 400 ms; clique abre a conversa, botão direito dispensa;
 *  - teste automático (headless): só conta, nunca abre janela, a não ser pelo comando de roteiro `sys toast`. */
#define COBJMACROS
#include "sysint.h"
#include <windows.h>
#include <wincodec.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TW_W 320
#define TW_H 80
#define TW_MS 6000
#define TW_IN 260
#define TW_OUT 400
#define TW_TIMER 7

static const GUID TW_IID_FACTORY = { 0xec5ec8a9, 0xc395, 0x4314, { 0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70 } };   /* IID_IWICImagingFactory (não vem no uuid.lib) */
int g_toastwin_shown, g_toastwin_serial;
static HWND g_wnd; static HDC g_dc; static HBITMAP g_bmp, g_old; static uint32_t *g_px;
static int g_w, g_h, g_x, g_y; static float g_sc = 1.0f;
static ULONGLONG g_t0, g_last; static bool g_hover, g_tracking, g_clicked;
static char g_id[48], g_clicked_id[48];

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static void wide(const char *s, wchar_t *o, int cap) { if (MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, o, cap) <= 0) o[0] = 0; o[cap - 1] = 0; }

/* ---- foto (WIC) ---- */
static uint32_t *photo_load(const char *path, int size) {
    if (!path || !path[0]) return NULL;
    uint32_t *out = NULL, *src = NULL;
    IWICImagingFactory *f = NULL; IWICBitmapDecoder *dec = NULL; IWICBitmapFrameDecode *fr = NULL; IWICFormatConverter *cv = NULL;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);   /* já iniciado em outro modo: tudo bem, o WIC funciona igual */
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &TW_IID_FACTORY, (void **)&f))) goto done;
    wchar_t wp[MAX_PATH + 8]; wide(path, wp, MAX_PATH + 8);
    if (FAILED(IWICImagingFactory_CreateDecoderFromFilename(f, wp, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) goto done;
    if (FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &fr))) goto done;
    if (FAILED(IWICImagingFactory_CreateFormatConverter(f, &cv))) goto done;
    if (FAILED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)fr, &GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom))) goto done;
    UINT w = 0, h = 0; IWICBitmapSource_GetSize((IWICBitmapSource *)cv, &w, &h);
    if (w < 2 || h < 2 || w > 8192 || h > 8192) goto done;
    src = (uint32_t *)malloc((size_t)w * h * 4); out = (uint32_t *)malloc((size_t)size * size * 4);
    if (!src || !out || FAILED(IWICBitmapSource_CopyPixels((IWICBitmapSource *)cv, NULL, w * 4, w * h * 4, (BYTE *)src))) { free(out); out = NULL; goto done; }
    {   /* recorte quadrado centralizado, média de área */
        const int side = (int)(w < h ? w : h), ox = ((int)w - side) / 2, oy = ((int)h - side) / 2; const float k = (float)side / (float)size;
        for (int dy = 0; dy < size; dy++) for (int dx = 0; dx < size; dx++) {
            int x0 = ox + (int)(dx * k), x1 = ox + (int)((dx + 1) * k), y0 = oy + (int)(dy * k), y1 = oy + (int)((dy + 1) * k);
            if (x1 <= x0) x1 = x0 + 1; if (y1 <= y0) y1 = y0 + 1; if (x1 > (int)w) x1 = (int)w; if (y1 > (int)h) y1 = (int)h;
            const int sx = (x1 - x0) > 12 ? (x1 - x0) / 6 : 1, sy = (y1 - y0) > 12 ? (y1 - y0) / 6 : 1;
            unsigned long a = 0, r = 0, g = 0, b = 0, n = 0;
            for (int y = y0; y < y1; y += sy) for (int x = x0; x < x1; x += sx) { uint32_t p = src[(size_t)y * w + x]; b += p & 255; g += (p >> 8) & 255; r += (p >> 16) & 255; a += p >> 24; n++; }
            if (!n) n = 1;
            out[dy * size + dx] = ((uint32_t)(a / n) << 24) | ((uint32_t)(r / n) << 16) | ((uint32_t)(g / n) << 8) | (uint32_t)(b / n);
        }
    }
done:
    free(src);
    if (cv) IWICFormatConverter_Release(cv); if (fr) IWICBitmapFrameDecode_Release(fr); if (dec) IWICBitmapDecoder_Release(dec); if (f) IWICImagingFactory_Release(f);
    return out;
}

/* ---- desenho ---- */
static uint32_t lerp_px(uint32_t a, uint32_t b, float t) {   /* opaco: a -> b */
    const float u = 1.0f - t;
    const uint32_t r = (uint32_t)(((a >> 16) & 255) * u + ((b >> 16) & 255) * t + 0.5f), g = (uint32_t)(((a >> 8) & 255) * u + ((b >> 8) & 255) * t + 0.5f), bl = (uint32_t)((a & 255) * u + (b & 255) * t + 0.5f);
    return 0xFF000000u | (r << 16) | (g << 8) | bl;
}
static void disc(float cx, float cy, float rad, uint32_t color) {
    const int x0 = (int)(cx - rad - 2), x1 = (int)(cx + rad + 2), y0 = (int)(cy - rad - 2), y1 = (int)(cy + rad + 2);
    for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < g_h; y++) for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < g_w; x++) {
        const float dx = x + 0.5f - cx, dy = y + 0.5f - cy, cov = clampf(rad + 0.5f - sqrtf(dx * dx + dy * dy), 0, 1);
        if (cov > 0) g_px[y * g_w + x] = lerp_px(g_px[y * g_w + x], color, cov);
    }
}
static void photo_disc(float cx, float cy, float rad, const uint32_t *ph, int size) {
    const int ox = (int)(cx - size * 0.5f + 0.5f), oy = (int)(cy - size * 0.5f + 0.5f);
    for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) {
        const int px = ox + x, py = oy + y; if (px < 0 || py < 0 || px >= g_w || py >= g_h) continue;
        const float dx = px + 0.5f - cx, dy = py + 0.5f - cy, cov = clampf(rad + 0.5f - sqrtf(dx * dx + dy * dy), 0, 1);
        if (cov <= 0) continue;
        const uint32_t p = ph[y * size + x]; const float a = ((p >> 24) / 255.0f) * cov;   /* foto em alfa pré-multiplicado */
        const uint32_t d = g_px[py * g_w + px];
        const float rr = ((p >> 16) & 255) * cov + ((d >> 16) & 255) * (1 - a), gg = ((p >> 8) & 255) * cov + ((d >> 8) & 255) * (1 - a), bb = (p & 255) * cov + (d & 255) * (1 - a);
        g_px[py * g_w + px] = 0xFF000000u | ((uint32_t)clampf(rr, 0, 255) << 16) | ((uint32_t)clampf(gg, 0, 255) << 8) | (uint32_t)clampf(bb, 0, 255);
    }
}
static HFONT mkfont(float px, int weight) { return CreateFontW(-(int)(px * g_sc + 0.5f), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"); }
static void text(const wchar_t *t, HFONT f, COLORREF c, float x0, float y0, float x1, float y1, UINT fl) {
    HFONT o = (HFONT)SelectObject(g_dc, f); SetTextColor(g_dc, c); SetBkMode(g_dc, TRANSPARENT);
    RECT r = { (int)(x0 * g_sc), (int)(y0 * g_sc), (int)(x1 * g_sc), (int)(y1 * g_sc) };
    DrawTextW(g_dc, t, -1, &r, fl | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(g_dc, o);
}
static void render(const char *name, const char *what, bool call, const uint32_t *photo, int av) {
    const uint32_t BG = 0xFF1A1326, GREEN = 0xFF57D28B, ACCENT = 0xFF6D4AFF;
    const float S = g_sc;
    for (int i = 0; i < g_w * g_h; i++) g_px[i] = BG;
    const float acx = 40.0f * S, acy = 40.0f * S, arad = 24.0f * S;
    if (photo) photo_disc(acx, acy, arad, photo, av); else disc(acx, acy, arad, ACCENT);
    disc(58.0f * S, 58.0f * S, 9.0f * S, BG);      /* anel: recorta a foto em volta da bolinha */
    disc(58.0f * S, 58.0f * S, 6.5f * S, GREEN);
    wchar_t wn[100], ww[70], li[3] = { 0, 0, 0 }; wide(name, wn, 100); wide(what, ww, 70);
    HFONT fn = mkfont(15, FW_SEMIBOLD), fw = mkfont(13, FW_NORMAL), fl = mkfont(22, FW_SEMIBOLD);
    if (!photo) { li[0] = wn[0] >= L'a' && wn[0] <= L'z' ? (wchar_t)(wn[0] - 32) : wn[0]; text(li, fl, RGB(255, 255, 255), 16, 16, 64, 64, DT_CENTER | DT_VCENTER); }
    text(wn, fn, RGB(0xF2, 0xEE, 0xFC), 78, 17, TW_W - 16, 40, DT_LEFT | DT_TOP);
    text(ww, fw, call ? RGB(0xB9, 0xA8, 0xFF) : RGB(0x57, 0xD2, 0x8B), 78, 43, TW_W - 16, 64, DT_LEFT | DT_TOP);
    GdiFlush();
    DeleteObject(fn); DeleteObject(fw); DeleteObject(fl);
    /* canal alfa: retângulo arredondado suave + fio verde na borda; fora dele fica transparente */
    const float hw = g_w * 0.5f, hh = g_h * 0.5f, rad = 16.0f * S, bw = 1.0f * S;
    for (int y = 0; y < g_h; y++) for (int x = 0; x < g_w; x++) {
        const float qx = fabsf(x + 0.5f - hw) - (hw - rad), qy = fabsf(y + 0.5f - hh) - (hh - rad);
        const float mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0, d = sqrtf(mx * mx + my * my) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - rad;
        const float a = clampf(0.5f - d, 0, 1), b = clampf(d + bw + 0.5f, 0, 1) * 0.45f;
        uint32_t p = g_px[y * g_w + x]; if (b > 0) p = lerp_px(p, GREEN, b);
        const unsigned ia = (unsigned)(a * 255.0f + 0.5f);
        g_px[y * g_w + x] = (ia << 24) | ((((p >> 16) & 255) * ia / 255) << 16) | ((((p >> 8) & 255) * ia / 255) << 8) | ((p & 255) * ia / 255);
    }
}

/* ---- janela ---- */
static void tw_close(void) {
    if (g_wnd) { KillTimer(g_wnd, TW_TIMER); HWND w = g_wnd; g_wnd = NULL; DestroyWindow(w); }
    if (g_dc) { if (g_old) SelectObject(g_dc, g_old); DeleteDC(g_dc); }
    if (g_bmp) DeleteObject(g_bmp);
    g_dc = NULL; g_bmp = NULL; g_old = NULL; g_px = NULL; g_hover = g_tracking = false;
}
static void tw_frame(void) {
    const ULONGLONG age = GetTickCount64() - g_t0;
    if (age >= TW_MS) { tw_close(); return; }
    const float in = age < TW_IN ? (float)age / TW_IN : 1.0f, out = age > TW_MS - TW_OUT ? (float)(TW_MS - age) / TW_OUT : 1.0f;
    const float e = 1.0f - (1.0f - in) * (1.0f - in), al = in < out ? in : out;
    POINT pt = { g_x, g_y + (int)((1.0f - e) * 30.0f * g_sc) }; SIZE sz = { g_w, g_h }, *psz = &sz; POINT zero = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)(al * 255.0f + 0.5f), AC_SRC_ALPHA };
    UpdateLayeredWindow(g_wnd, NULL, &pt, psz, g_dc, &zero, 0, &bf, ULW_ALPHA);
}
static LRESULT CALLBACK tw_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SETCURSOR: SetCursor(LoadCursor(NULL, IDC_HAND)); return TRUE;
    case WM_MOUSEMOVE: if (!g_tracking) { TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 }; TrackMouseEvent(&t); g_tracking = true; } g_hover = true; return 0;
    case WM_MOUSELEAVE: g_hover = false; g_tracking = false; return 0;
    case WM_LBUTTONUP: snprintf(g_clicked_id, sizeof g_clicked_id, "%s", g_id); g_clicked = true; if (h == g_wnd) tw_close(); return 0;
    case WM_RBUTTONUP: if (h == g_wnd) tw_close(); return 0;
    case WM_TIMER:
        if (w == TW_TIMER && h == g_wnd) { const ULONGLONG now = GetTickCount64(); if (g_hover) g_t0 += now - g_last; g_last = now; tw_frame(); return 0; }
        break;
    }
    return DefWindowProcW(h, m, w, l);
}
static bool tw_class(void) {
    static bool ok, tried; if (tried) return ok; tried = true;
    WNDCLASSEXW c = { sizeof c }; c.lpfnWndProc = tw_proc; c.hInstance = GetModuleHandleW(NULL); c.lpszClassName = L"BriksToastWin"; c.hCursor = LoadCursor(NULL, IDC_HAND);
    ok = RegisterClassExW(&c) != 0; return ok;
}

static bool show_ex(const char *id, const char *name, const char *what, bool call, const char *photo_path, bool force) {
    g_toastwin_shown++;
    if (!force && sys_is_headless() && !getenv("BRIKS_TOAST_REAL")) return false;   /* teste automático: conta, mas nunca abre janela */
    if (!tw_class()) return false;
    tw_close();
    UINT dpi = GetDpiForSystem(); if (dpi < 96) dpi = 96;
    g_sc = dpi / 96.0f; g_w = (int)(TW_W * g_sc + 0.5f); g_h = (int)(TW_H * g_sc + 0.5f);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = g_w; bi.bmiHeader.biHeight = -g_h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    HDC scr = GetDC(NULL); g_dc = CreateCompatibleDC(scr); ReleaseDC(NULL, scr);
    void *bits = NULL; g_bmp = g_dc ? CreateDIBSection(g_dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    if (!g_dc || !g_bmp || !bits) { tw_close(); return false; }
    g_old = (HBITMAP)SelectObject(g_dc, g_bmp); g_px = (uint32_t *)bits;
    const int av = (int)(48.0f * g_sc + 0.5f); uint32_t *ph = photo_load(photo_path, av);
    render(name, what, call, ph, av); free(ph);
    RECT wa; if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) { wa.right = GetSystemMetrics(SM_CXSCREEN); wa.bottom = GetSystemMetrics(SM_CYSCREEN); }
    const int mg = (int)(16.0f * g_sc); g_x = wa.right - g_w - mg; g_y = wa.bottom - g_h - mg;
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, L"BriksToastWin", L"Briks aviso", WS_POPUP, g_x, g_y, g_w, g_h, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_wnd) { tw_close(); return false; }
    snprintf(g_id, sizeof g_id, "%s", id ? id : ""); g_clicked = false;
    g_t0 = g_last = GetTickCount64(); g_hover = g_tracking = false;
    tw_frame();
    SetWindowPos(g_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetTimer(g_wnd, TW_TIMER, 16, NULL);
    g_toastwin_serial++;
    return true;
}
bool toastwin_show(const char *id, const char *name, const char *what, bool call, const char *photo_path) { return show_ex(id, name, what, call, photo_path, false); }
bool toastwin_script(const char *name, const char *what, bool call, const char *photo_path) { return show_ex("script", name, what, call, photo_path, true); }
bool toastwin_take_click(char *id, size_t cap) { if (!g_clicked) return false; snprintf(id, cap, "%s", g_clicked_id); g_clicked = false; return true; }
bool toastwin_visible(void) { return g_wnd != NULL; }
void toastwin_close(void) { tw_close(); }

/* teste: grava o cartão desenhado (sobre cinza) num BMP de 24 bits, para conferir o visual sem depender da tela */
bool toastwin_dump(const char *path) {
    if (!g_px || !path) return false;
    const int rowb = (g_w * 3 + 3) & ~3; BYTE *row = (BYTE *)malloc((size_t)rowb); if (!row) return false;
    FILE *f = fopen(path, "wb"); if (!f) { free(row); return false; }
    BITMAPFILEHEADER fh = { 0x4D42, (DWORD)(54 + rowb * g_h), 0, 0, 54 }; BITMAPINFOHEADER ih = { sizeof ih, g_w, g_h, 1, 24, BI_RGB, (DWORD)(rowb * g_h), 0, 0, 0, 0 };
    fwrite(&fh, 1, sizeof fh, f); fwrite(&ih, 1, sizeof ih, f);
    for (int y = g_h - 1; y >= 0; y--) {
        memset(row, 0, (size_t)rowb);
        for (int x = 0; x < g_w; x++) {
            const uint32_t p = g_px[y * g_w + x]; const unsigned a = p >> 24, bgc = (((x / 16) + (y / 16)) & 1) ? 0x70 : 0x90;   /* xadrez */
            row[x * 3] = (BYTE)((p & 255) + bgc * (255 - a) / 255); row[x * 3 + 1] = (BYTE)(((p >> 8) & 255) + bgc * (255 - a) / 255); row[x * 3 + 2] = (BYTE)(((p >> 16) & 255) + bgc * (255 - a) / 255);
        }
        fwrite(row, 1, (size_t)rowb, f);
    }
    fclose(f); free(row); return true;
}

/* teste: consulta o estado real da janela (roteiro: expect toast_<nome> <valor>) */
int toastwin_probe(const char *n) {
    if (!strcmp(n, "visible")) return g_wnd && IsWindowVisible(g_wnd) ? 1 : 0;
    if (!g_wnd) return 0;
    const LONG ex = GetWindowLongW(g_wnd, GWL_EXSTYLE);
    if (!strcmp(n, "topmost")) return (ex & WS_EX_TOPMOST) != 0;
    if (!strcmp(n, "noactivate")) return (ex & WS_EX_NOACTIVATE) != 0;
    if (!strcmp(n, "focus")) return GetForegroundWindow() == g_wnd;   /* nunca pode roubar o foco */
    RECT r, wa; GetWindowRect(g_wnd, &r); SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    if (!strcmp(n, "inarea")) return r.left >= wa.left && r.top >= wa.top && r.right <= wa.right && r.bottom <= wa.bottom;   /* dentro da área útil (não cobre a barra de tarefas) */
    if (!strcmp(n, "corner")) return (wa.right - r.right) < (int)(40 * g_sc) && (wa.bottom - r.bottom) < (int)(70 * g_sc);   /* canto de baixo à direita */
    return -9999;
}
