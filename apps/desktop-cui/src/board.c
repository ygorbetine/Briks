/* Board: quadro branco/escuro compartilhado da sala (aba "Board" da chamada). Várias páginas ("b:0".."b:7" no protocolo), pan/zoom,
 * mesma barra de ferramentas das anotações e exportação em PNG (rasterizador próprio em CPU; texto pelo GDI; sem rede).
 * Plano: docs/plans/anotacoes-board.md. */
#include "annot.h"
#include <windows.h>
#include <commdlg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cui/cui_render.h"

#define BOARD_PAGES 8
#define PAGE_W 1920
#define PAGE_H 1080

static int g_page, g_npages = 1;
static bool g_dark;
static float g_zoom[BOARD_PAGES] = { 1, 1, 1, 1, 1, 1, 1, 1 }, g_panx[BOARD_PAGES], g_pany[BOARD_PAGES];
static char g_msg[200]; static unsigned long long g_msg_t;
static bool g_first = true;

void board_room_left(void) { g_page = 0; g_npages = 1; g_dark = false; g_first = true; for (int i = 0; i < BOARD_PAGES; i++) { g_zoom[i] = 1; g_panx[i] = g_pany[i] = 0; } }

static void say(const char *fmt, const char *a) { snprintf(g_msg, sizeof g_msg, fmt, a ? a : ""); g_msg_t = GetTickCount64(); }

/* ------------------------------------------------------------------ exportação PNG */

typedef struct { unsigned char *px; int w, h; } rimg_t;
static unsigned char *g_mask; static size_t g_mask_cap;

static void mask_capsule(unsigned char *m, int bw, int bh, int ox, int oy, float ax, float ay, float bx, float by, float r) {
    int x0 = (int)floorf(fminf(ax, bx) - r - 1) - ox, x1 = (int)ceilf(fmaxf(ax, bx) + r + 1) - ox, y0 = (int)floorf(fminf(ay, by) - r - 1) - oy, y1 = (int)ceilf(fmaxf(ay, by) + r + 1) - oy;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 >= bw) x1 = bw - 1; if (y1 >= bh) y1 = bh - 1;
    const float dx = bx - ax, dy = by - ay, L2 = dx * dx + dy * dy;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        const float px = (float)(x + ox) + 0.5f - ax, py = (float)(y + oy) + 0.5f - ay; float t = L2 > 1e-6f ? (px * dx + py * dy) / L2 : 0; t = t < 0 ? 0 : t > 1 ? 1 : t;
        const float qx = px - t * dx, qy = py - t * dy, d = sqrtf(qx * qx + qy * qy), cov = r + 0.5f - d;
        if (cov <= 0) continue; const unsigned char v = cov >= 1.0f ? 255 : (unsigned char)(cov * 255.0f);
        if (v > m[y * bw + x]) m[y * bw + x] = v;
    }
}
static void composite(rimg_t *im, const unsigned char *m, int bw, int bh, int ox, int oy, unsigned rgb, int alpha) {
    const int R = (rgb >> 16) & 255, G = (rgb >> 8) & 255, B = rgb & 255;
    for (int y = 0; y < bh; y++) { const int Y = y + oy; if (Y < 0 || Y >= im->h) continue;
        for (int x = 0; x < bw; x++) { const int X = x + ox; if (X < 0 || X >= im->w) continue; const int a = m[y * bw + x] * alpha / 255; if (!a) continue;
            unsigned char *d = im->px + ((size_t)Y * (size_t)im->w + (size_t)X) * 4; d[0] = (unsigned char)((d[0] * (255 - a) + R * a) / 255); d[1] = (unsigned char)((d[1] * (255 - a) + G * a) / 255); d[2] = (unsigned char)((d[2] * (255 - a) + B * a) / 255); d[3] = 255; } }
}
static unsigned char *mask_alloc(int bw, int bh) { size_t n = (size_t)bw * (size_t)bh; if (n > g_mask_cap) { free(g_mask); g_mask = malloc(n); g_mask_cap = g_mask ? n : 0; if (!g_mask) return NULL; } memset(g_mask, 0, n); return g_mask; }

static void export_text(rimg_t *im, const aop_t *o) {
    wchar_t w[160]; int n = MultiByteToWideChar(CP_UTF8, 0, o->s, -1, w, 160); if (n <= 1) return;
    const int fh = (int)fmaxf((float)o->sz * (float)PAGE_W / AN_COORD, 10.0f);
    HDC dc = CreateCompatibleDC(NULL); HFONT f = CreateFontW(-fh, 0, 0, 0, FW_MEDIUM, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI"); HGDIOBJ of = SelectObject(dc, f);
    SIZE sz; GetTextExtentPoint32W(dc, w, n - 1, &sz);
    BITMAPINFO bi; memset(&bi, 0, sizeof bi); bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = sz.cx + 4; bi.bmiHeader.biHeight = -(sz.cy + 4); bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void *bits = NULL; HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bm && bits) {
        HGDIOBJ ob = SelectObject(dc, bm); memset(bits, 0, (size_t)(sz.cx + 4) * (size_t)(sz.cy + 4) * 4);
        SetTextColor(dc, RGB(255, 255, 255)); SetBkMode(dc, TRANSPARENT); TextOutW(dc, 2, 2, w, n - 1); GdiFlush();
        const int bw = sz.cx + 4, bh = sz.cy + 4, ox = (int)((long long)o->x0 * PAGE_W / AN_COORD) - 2, oy = (int)((long long)o->y0 * PAGE_H / AN_COORD) - 2;
        unsigned char *m = mask_alloc(bw, bh);
        if (m) { const unsigned char *src = bits; for (int i = 0; i < bw * bh; i++) m[i] = src[i * 4 + 1]; composite(im, m, bw, bh, ox, oy, (unsigned)o->col, 255); }
        SelectObject(dc, ob);
    }
    if (bm) DeleteObject(bm); SelectObject(dc, of); DeleteObject(f); DeleteDC(dc);
}
static void export_op(rimg_t *im, aop_t *o) {
    const float SX = (float)PAGE_W / AN_COORD, SY = (float)PAGE_H / AN_COORD;
    float th = (float)o->w * SX; if (th < 1.0f) th = 1.0f;
    if (o->k == 't') { export_text(im, o); return; }
    if (o->k == 's') {
        const bool hi = o->m == 1; if (hi) th *= 3.5f;
        int n; const float *sm = annot_op_smooth(o, &n); if (!sm || n < 1) return;
        float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
        for (int k = 0; k < n; k++) { const float x = sm[k * 2] * AN_COORD * SX, y = sm[k * 2 + 1] * AN_COORD * SY; minx = fminf(minx, x); maxx = fmaxf(maxx, x); miny = fminf(miny, y); maxy = fmaxf(maxy, y); }
        const int ox = (int)floorf(minx - th - 2), oy = (int)floorf(miny - th - 2), bw = (int)ceilf(maxx - minx + 2 * th + 4), bh = (int)ceilf(maxy - miny + 2 * th + 4);
        unsigned char *m = mask_alloc(bw, bh); if (!m) return;
        if (n == 1) mask_capsule(m, bw, bh, ox, oy, sm[0] * AN_COORD * SX, sm[1] * AN_COORD * SY, sm[0] * AN_COORD * SX, sm[1] * AN_COORD * SY, th * 0.5f);
        for (int k = 0; k + 1 < n; k++) mask_capsule(m, bw, bh, ox, oy, sm[k * 2] * AN_COORD * SX, sm[k * 2 + 1] * AN_COORD * SY, sm[k * 2 + 2] * AN_COORD * SX, sm[k * 2 + 3] * AN_COORD * SY, th * 0.5f);
        composite(im, m, bw, bh, ox, oy, (unsigned)o->col, hi ? 0x66 : 255);
        return;
    }
    if (o->k != 'g') return;
    const float ax = o->x0 * SX, ay = o->y0 * SY, bx = o->x1 * SX, by = o->y1 * SY;
    const int ox = (int)floorf(fminf(ax, bx) - th - 3), oy = (int)floorf(fminf(ay, by) - th - 3), bw = (int)ceilf(fabsf(bx - ax) + 2 * th + 6), bh = (int)ceilf(fabsf(by - ay) + 2 * th + 6);
    unsigned char *m = mask_alloc(bw, bh); if (!m) return;
    if (o->sh == 'r' && o->f) {
        for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) { const float X = (float)(x + ox) + 0.5f, Y = (float)(y + oy) + 0.5f; if (X >= fminf(ax, bx) && X <= fmaxf(ax, bx) && Y >= fminf(ay, by) && Y <= fmaxf(ay, by)) m[y * bw + x] = 255; }
        composite(im, m, bw, bh, ox, oy, (unsigned)o->col, 0x59); memset(m, 0, (size_t)bw * (size_t)bh);
    } else if (o->sh == 'e' && o->f) {
        const float cx = (ax + bx) * 0.5f, cy = (ay + by) * 0.5f, rx = fabsf(bx - ax) * 0.5f + 0.01f, ry = fabsf(by - ay) * 0.5f + 0.01f;
        for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) { const float nx = ((float)(x + ox) + 0.5f - cx) / rx, ny = ((float)(y + oy) + 0.5f - cy) / ry; const float d = sqrtf(nx * nx + ny * ny); const float cov = (1.0f - d) * fminf(rx, ry) + 0.5f; if (cov > 0) m[y * bw + x] = cov >= 1 ? 255 : (unsigned char)(cov * 255); }
        composite(im, m, bw, bh, ox, oy, (unsigned)o->col, 0x59); memset(m, 0, (size_t)bw * (size_t)bh);
    }
    if (o->sh == 'l' || o->sh == 'a') {
        mask_capsule(m, bw, bh, ox, oy, ax, ay, bx, by, th * 0.5f);
        if (o->sh == 'a') {
            const float dx = bx - ax, dy = by - ay, len = sqrtf(dx * dx + dy * dy);
            if (len > 2.0f) { float hl = len * 0.3f, mx = 14.0f * 1.5f + th * 3.0f; if (hl > mx) hl = mx; const float ux = dx / len, uy = dy / len;
                for (int sg = -1; sg <= 1; sg += 2) { const float a = 0.5f * (float)sg, ca = cosf(a), sa = sinf(a); mask_capsule(m, bw, bh, ox, oy, bx, by, bx - hl * (ux * ca - uy * sa), by - hl * (ux * sa + uy * ca), th * 0.5f); } }
        }
    } else if (o->sh == 'r') {
        mask_capsule(m, bw, bh, ox, oy, ax, ay, bx, ay, th * 0.5f); mask_capsule(m, bw, bh, ox, oy, bx, ay, bx, by, th * 0.5f); mask_capsule(m, bw, bh, ox, oy, bx, by, ax, by, th * 0.5f); mask_capsule(m, bw, bh, ox, oy, ax, by, ax, ay, th * 0.5f);
    } else if (o->sh == 'e') {
        const float cx = (ax + bx) * 0.5f, cy = (ay + by) * 0.5f, rx = fabsf(bx - ax) * 0.5f, ry = fabsf(by - ay) * 0.5f; float px = cx + rx, py = cy;
        for (int k = 1; k <= 64; k++) { const float t = 6.2831853f * (float)k / 64.0f, x = cx + cosf(t) * rx, y = cy + sinf(t) * ry; mask_capsule(m, bw, bh, ox, oy, px, py, x, y, th * 0.5f); px = x; py = y; }
    }
    composite(im, m, bw, bh, ox, oy, (unsigned)o->col, 255);
}

static bool export_page(int page, const char *path) {
    rimg_t im = { malloc((size_t)PAGE_W * PAGE_H * 4), PAGE_W, PAGE_H }; if (!im.px) return false;
    const unsigned bg = g_dark ? 0x1b1b24 : 0xfafafa;
    for (int i = 0; i < PAGE_W * PAGE_H; i++) { im.px[i * 4] = (unsigned char)((bg >> 16) & 255); im.px[i * 4 + 1] = (unsigned char)((bg >> 8) & 255); im.px[i * 4 + 2] = (unsigned char)(bg & 255); im.px[i * 4 + 3] = 255; }
    char base[16]; snprintf(base, sizeof base, "b:%d", page);
    for (int layer = 0; layer < 2; layer++) {   /* o que todos veem e, por cima, o que só eu desenhei */
        const int n = annot_scope_count(base, layer == 1);
        for (int k = 0; k < n; k++) { const aop_t *o = annot_scope_op(base, layer == 1, k); if (o) export_op(&im, (aop_t *)o); }
    }
    const bool ok = cui_write_png(path, im.px, im.w, im.h, im.w * 4);
    free(im.px); return ok;
}

static void do_export(void) {
    char path[MAX_PATH]; snprintf(path, sizeof path, "board-pagina-%d.png", g_page + 1);
    const char *e = getenv("BRIKS_EXPORT_FILE");   /* testes: sem a janela de salvar */
    if (e && e[0]) snprintf(path, sizeof path, "%s", e);
    else {
        OPENFILENAMEA o; memset(&o, 0, sizeof o); o.lStructSize = sizeof o;
        o.lpstrFilter = "Imagem PNG (*.png)\0*.png\0"; o.lpstrFile = path; o.nMaxFile = sizeof path; o.lpstrDefExt = "png"; o.lpstrTitle = "Exportar página do Board";
        o.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameA(&o)) { say("Exportação cancelada.", NULL); return; }
    }
    if (export_page(g_page, path)) say("Página exportada: %s", path); else say("Não consegui gravar %s.", path);
}

/* ------------------------------------------------------------------ tela */

typedef struct { float ox, oy, iw, ih; bool dark; } bgpaint_t;
static void paint_bg(cui_ctx *ui, cui_rect r, void *u) {
    const bgpaint_t *b = u; const cui_rect pg = { r.x + b->ox, r.y + b->oy, b->iw, b->ih };
    cui_push_clip(ui, r);
    cui_draw_shadow(ui, pg, CUI_CORNERS(6), 24.0f, 0.0f, (cui_vec2){ 0, 6 }, CUI_RGBA(0x00000070));
    cui_draw_rect(ui, pg, CUI_CORNERS(6), b->dark ? CUI_RGB(0x1b1b24) : CUI_RGB(0xfafafa));
    cui_pop_clip(ui);
}

void board_stage(cui_ctx *ui, app_state_t *s, float W, float H) {
    if (g_first) { g_first = false; annot_tool_default(); }
    /* páginas que outros abriram também aparecem */
    for (int k = BOARD_PAGES - 1; k >= g_npages; k--) { char b[16]; snprintf(b, sizeof b, "b:%d", k); if (annot_scope_count(b, false) > 0) { g_npages = k + 1; break; } }
    if (g_page >= g_npages) g_page = g_npages - 1;
    const float HEAD = 40.0f, GAP = 8.0f, AH = H - HEAD - GAP, AW = W;
    cui_column(ui, .width = CUI_PX(W), .height = CUI_PX(H), .gap = GAP) {
        cui_row(ui, .width = CUI_PX(W), .height = CUI_PX(HEAD), .gap = 6, .align = CUI_ALIGN_CENTER) {
            for (int p = 0; p < g_npages; p++) {
                char l[8]; snprintf(l, sizeof l, "%d", p + 1);
                cui_push_id_int(ui, 100 + p);
                if (cui_button(ui, l, .variant = p == g_page ? CUI_BUTTON_PRIMARY : CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) g_page = p;
                cui_pop_id(ui);
            }
            if (g_npages < BOARD_PAGES && cui_button(ui, "+ Página", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) { g_page = g_npages; g_npages++; }
            cui_box(ui, .width = CUI_FILL, .height = CUI_PX(1));
            const unsigned long long t = GetTickCount64();
            if (g_msg[0] && t - g_msg_t < 6000) cui_box(ui, .max_width = fmaxf(W * 0.35f, 120.0f), .clip = true) { cui_text(ui, g_msg, .size = 12, .color = B_MUTED, .nowrap = true); }
            if (cui_button(ui, "Mover", .variant = annot_tool_active() ? CUI_BUTTON_SECONDARY : CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM)) annot_tool_reset();
            if (cui_button(ui, g_dark ? "Fundo claro" : "Fundo escuro", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) g_dark = !g_dark;
            if (cui_button(ui, "Ajustar", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) { g_zoom[g_page] = 1; g_panx[g_page] = g_pany[g_page] = 0; }
            if (cui_button(ui, "Exportar PNG", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) do_export();
        }
        actx_t cx; memset(&cx, 0, sizeof cx); cx.board = true; snprintf(cx.base, sizeof cx.base, "b:%d", g_page);
        const float fit = fminf(AW / (float)PAGE_W, AH / (float)PAGE_H) * 0.96f;
        float *zm = &g_zoom[g_page], *px = &g_panx[g_page], *py = &g_pany[g_page];
        const cui_rect pr = cui_get_rect(ui, cui_make_id(ui, "bsurf")); const cui_vec2 mp = cui_mouse_pos(ui);
        const bool inside = pr.w > 0 && mp.x >= pr.x && mp.y >= pr.y && mp.x < pr.x + pr.w && mp.y < pr.y + pr.h && !briks_covered(ui) && !briks_blocked();
        float iw = (float)PAGE_W * fit * *zm, ih = (float)PAGE_H * fit * *zm;
        if (inside) {   /* roda: zoom em torno do ponteiro; botão do meio (ou esquerdo sem ferramenta) arrasta */
            const float w = cui_wheel_take(ui);
            if (w != 0.0f) {
                const float oldz = *zm; float nz = oldz * powf(1.2f, w); if (nz < 0.3f) nz = 0.3f; if (nz > 8.0f) nz = 8.0f;
                const float ox = (AW - iw) * 0.5f + *px, oy = (AH - ih) * 0.5f + *py, fx = (mp.x - pr.x - ox) / iw, fy = (mp.y - pr.y - oy) / ih;
                *zm = nz; iw = (float)PAGE_W * fit * nz; ih = (float)PAGE_H * fit * nz;
                *px = (mp.x - pr.x) - fx * iw - (AW - iw) * 0.5f; *py = (mp.y - pr.y) - fy * ih - (AH - ih) * 0.5f;
            }
            cui_wheel_claim(ui, pr);
        }
        static bool pan; static cui_vec2 pan0; static float pp0x, pp0y;
        const bool pdown = cui_mouse_down(ui, CUI_MOUSE_MIDDLE) || (!annot_tool_active() && cui_mouse_down(ui, CUI_MOUSE_LEFT));
        if (pdown && inside && !pan) { pan = true; pan0 = mp; pp0x = *px; pp0y = *py; }
        if (pan) { if (pdown) { *px = pp0x + (mp.x - pan0.x); *py = pp0y + (mp.y - pan0.y); } else pan = false; }
        const float lim_x = iw * 0.5f + AW * 0.4f, lim_y = ih * 0.5f + AH * 0.4f;
        if (*px > lim_x) *px = lim_x; if (*px < -lim_x) *px = -lim_x; if (*py > lim_y) *py = lim_y; if (*py < -lim_y) *py = -lim_y;
        asurf_t sf = { (AW - iw) * 0.5f + *px, (AH - ih) * 0.5f + *py, iw, ih };
        bgpaint_t bp = { sf.ox, sf.oy, sf.iw, sf.ih, g_dark };
        cui_box(ui, .id = "bstage", .dir = CUI_STACK, .width = CUI_PX(AW), .height = CUI_PX(AH), .radius = 16, .clip = true, .bg = CUI_RGBA(0x1a0a2680), .border = 1, .border_color = CUI_RGBA(0x6d5cf655)) {
            cui_canvas(ui, "bbg", CUI_FILL, CUI_FILL, paint_bg, &bp, sizeof bp);
            annot_surface(ui, s, &cx, &sf, "bsurf", true);
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_CENTER, .pad_left = 10) {
                cui_box(ui, .id = "bbar", .pad = 5, .radius = 14, .bg = B_GLASS_STRONG, .border = 1, .border_color = B_GLASS_BORDER) { annot_toolbar_board(ui, s, &cx); }
            }
            const char *nt = annot_note();
            if (nt[0]) cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_END, .pad = 14) { cui_box(ui, .pad_x = 12, .pad_y = 6, .radius = 12, .bg = B_GLASS_STRONG) { cui_text(ui, nt, .size = 12, .color = B_MUTED); } }
        }
        { const cui_rect br = cui_get_rect(ui, cui_make_id(ui, "bbar")); if (br.w > 0) briks_cover(br.x, br.y, br.w, br.h); }
    }
}
