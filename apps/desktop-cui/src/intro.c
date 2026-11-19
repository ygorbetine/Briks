/* Abertura animada do Briks: assets/intro/abertura.webm (VP9 com canal alfa, só vídeo é lido) + assets/intro/abertura.wav (trilha).
 * O WebM é lido por um leitor EBML mínimo; o vídeo e o canal alfa (BlockAdditions, id 1) são decodificados pela libvpx e viram uma textura
 * RGBA por quadro. O áudio sai pelo waveOut. Tudo local; se faltar arquivo, decodificador ou áudio, a abertura é simplesmente pulada. */
#define WIN32_LEAN_AND_MEAN
#include "intro.h"
#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef BRIKS_INTRO_VPX
#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>

#define MAX_FR 1024
typedef struct { int64_t ms; const uint8_t *d; int n; const uint8_t *a; int an; } fr_t;

static uint8_t *g_file; static size_t g_len;
static fr_t g_fr[MAX_FR]; static int g_nfr;
static int64_t g_scale = 1000000;   /* ns por tick de relógio do WebM (padrão: 1 ms) */
static int64_t g_cluster_tc; static uint64_t g_vtrack = 1, g_tn, g_tt;
static const uint8_t *g_bd; static int g_bn; static const uint8_t *g_ad; static int g_an;

static vpx_codec_ctx_t g_dv, g_da; static bool g_dv_ok, g_da_ok;
static int g_next;   /* próximo quadro a decodificar */
static int g_w, g_h; static uint8_t *g_rgba; static bool g_have_rgba;
static cui_texture g_tex, g_old; static int g_tex_idx = -1;
static int g_state;   /* 0 = não começou, 1 = tocando, 2 = fim */
static LONGLONG g_t0, g_qf; static int64_t g_total_ms;
static int64_t g_force_ms = -1;
static HWAVEOUT g_wo; static WAVEHDR g_wh; static uint8_t *g_wav;

static bool rd_id(const uint8_t **p, const uint8_t *e, uint32_t *id) {
    if (*p >= e) return false;
    uint8_t b = **p; int n = 1; uint8_t m = 0x80;
    while (n <= 4 && !(b & m)) { n++; m >>= 1; }
    if (n > 4 || *p + n > e) return false;
    uint32_t v = 0; for (int i = 0; i < n; i++) v = (v << 8) | (*p)[i];
    *p += n; *id = v; return true;
}
static bool rd_size(const uint8_t **p, const uint8_t *e, int64_t *sz) {
    if (*p >= e) return false;
    uint8_t b = **p; int n = 1; uint8_t m = 0x80;
    while (n <= 8 && !(b & m)) { n++; m >>= 1; }
    if (n > 8 || *p + n > e) return false;
    uint64_t v = (uint64_t)(b & (m - 1)); bool ones = v == (uint64_t)(m - 1);
    for (int i = 1; i < n; i++) { v = (v << 8) | (*p)[i]; if ((*p)[i] != 0xff) ones = false; }
    *p += n; *sz = ones ? -1 : (int64_t)v; return true;
}
static uint64_t rd_uint(const uint8_t *d, int n) { uint64_t v = 0; for (int i = 0; i < n && i < 8; i++) v = (v << 8) | d[i]; return v; }

static void add_frame(const uint8_t *d, int n, const uint8_t *a, int an) {
    if (n < 4 || g_nfr >= MAX_FR) return;
    uint64_t tn = 0; const uint8_t *p = d, *e = d + n;
    if (p >= e) return;
    uint8_t b = *p; int l = 1; uint8_t m = 0x80; while (l <= 8 && !(b & m)) { l++; m >>= 1; }
    if (l > 8 || p + l + 3 > e) return;
    tn = (uint64_t)(b & (m - 1)); for (int i = 1; i < l; i++) tn = (tn << 8) | p[i];
    p += l;
    int16_t rel = (int16_t)((p[0] << 8) | p[1]); uint8_t flags = p[2]; p += 3;
    if (tn != g_vtrack || (flags & 0x06)) return;   /* só o vídeo, sem lacing */
    fr_t *f = &g_fr[g_nfr++];
    f->ms = (g_cluster_tc + rel) * g_scale / 1000000; f->d = p; f->n = (int)(e - p); f->a = a; f->an = an;
}

static void walk(const uint8_t *p, const uint8_t *e) {
    while (p < e) {
        uint32_t id; int64_t sz;
        if (!rd_id(&p, e, &id) || !rd_size(&p, e, &sz)) return;
        const uint8_t *end = sz < 0 || p + sz > e ? e : p + sz;
        switch (id) {
        case 0x18538067: case 0x1549A966: case 0x1654AE6B: case 0x1F43B675: case 0x75A1: case 0xA6:   /* Segment, Info, Tracks, Cluster, BlockAdditions, BlockMore */
            walk(p, end); break;
        case 0xAE: g_tn = g_tt = 0; walk(p, end); if (g_tt == 1 && g_tn) g_vtrack = g_tn; break;   /* TrackEntry */
        case 0xA0: g_bd = g_ad = NULL; g_bn = g_an = 0; walk(p, end); if (g_bd) add_frame(g_bd, g_bn, g_ad, g_an); g_bd = g_ad = NULL; break;   /* BlockGroup */
        case 0x2AD7B1: g_scale = (int64_t)rd_uint(p, (int)(end - p)); break;
        case 0xD7: g_tn = rd_uint(p, (int)(end - p)); break;
        case 0x83: g_tt = rd_uint(p, (int)(end - p)); break;
        case 0xE7: g_cluster_tc = (int64_t)rd_uint(p, (int)(end - p)); break;
        case 0xA3: add_frame(p, (int)(end - p), NULL, 0); break;   /* SimpleBlock */
        case 0xA1: g_bd = p; g_bn = (int)(end - p); break;
        case 0xA5: g_ad = p; g_an = (int)(end - p); break;   /* BlockAdditional (alfa) */
        default: break;
        }
        p = end;
    }
}

static uint8_t *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 64 * 1024 * 1024) { fclose(f); return NULL; }
    uint8_t *b = (uint8_t *)malloc((size_t)n); if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); *len = (size_t)n; return b;
}

static void exe_dir(char *out, size_t cap) {
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    while (n > 0 && out[n - 1] != '\\' && out[n - 1] != '/') n--;
    out[n] = 0;
}

static bool dec_open(vpx_codec_ctx_t *c) { vpx_codec_dec_cfg_t cfg; memset(&cfg, 0, sizeof cfg); cfg.threads = 1; return vpx_codec_dec_init(c, vpx_codec_vp9_dx(), &cfg, 0) == VPX_CODEC_OK; }
static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* Decodifica em ordem até o quadro `idx` (a predição precisa de todos) e converte só o último para RGBA (BT.601, faixa limitada; alfa cru). */
static bool decode_to(int idx) {
    const vpx_image_t *last = NULL, *lastA = NULL;
    while (g_next <= idx) {
        const fr_t *f = &g_fr[g_next++];
        if (vpx_codec_decode(&g_dv, f->d, (unsigned)f->n, NULL, 0) != VPX_CODEC_OK) return false;
        vpx_codec_iter_t it = NULL; const vpx_image_t *im = vpx_codec_get_frame(&g_dv, &it);
        const vpx_image_t *am = NULL;
        if (f->a && f->an > 0 && g_da_ok) {
            if (vpx_codec_decode(&g_da, f->a, (unsigned)f->an, NULL, 0) == VPX_CODEC_OK) { vpx_codec_iter_t ia = NULL; am = vpx_codec_get_frame(&g_da, &ia); }
        }
        if (g_next - 1 == idx) { last = im; lastA = am; }
    }
    if (!last) return false;
    if (last->fmt != VPX_IMG_FMT_I420 || last->d_w <= 0 || last->d_h <= 0) return false;
    if ((int)last->d_w != g_w || (int)last->d_h != g_h) { free(g_rgba); g_w = (int)last->d_w; g_h = (int)last->d_h; g_rgba = (uint8_t *)malloc((size_t)g_w * g_h * 4); if (!g_rgba) return false; }
    for (int y = 0; y < g_h; y++) {
        const uint8_t *Y = last->planes[0] + (size_t)y * last->stride[0], *U = last->planes[1] + (size_t)(y >> 1) * last->stride[1], *V = last->planes[2] + (size_t)(y >> 1) * last->stride[2];
        const uint8_t *A = lastA && lastA->planes[0] && (int)lastA->d_w >= g_w && (int)lastA->d_h >= g_h ? lastA->planes[0] + (size_t)y * lastA->stride[0] : NULL;
        uint8_t *o = g_rgba + (size_t)y * g_w * 4;
        for (int x = 0; x < g_w; x++) {
            int c = (int)Y[x] - 16, d = (int)U[x >> 1] - 128, e = (int)V[x >> 1] - 128;
            o[x * 4 + 0] = (uint8_t)clamp8((298 * c + 409 * e + 128) >> 8);
            o[x * 4 + 1] = (uint8_t)clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
            o[x * 4 + 2] = (uint8_t)clamp8((298 * c + 516 * d + 128) >> 8);
            o[x * 4 + 3] = A ? A[x] : 255;
        }
    }
    g_have_rgba = true; return true;
}

static LONGLONG qpc(void) { LARGE_INTEGER v; QueryPerformanceCounter(&v); return v.QuadPart; }

static void audio_stop(void) {
    if (g_wo) { waveOutReset(g_wo); waveOutUnprepareHeader(g_wo, &g_wh, sizeof g_wh); waveOutClose(g_wo); g_wo = NULL; }
    free(g_wav); g_wav = NULL;
}
static void audio_start(const char *path) {
    size_t n; uint8_t *b = read_all(path, &n); if (!b || n < 44 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) { free(b); return; }
    WAVEFORMATEX wf; memset(&wf, 0, sizeof wf); const uint8_t *data = NULL; uint32_t dlen = 0;
    for (size_t p = 12; p + 8 <= n;) {
        uint32_t cl = (uint32_t)(b[p + 4] | (b[p + 5] << 8) | (b[p + 6] << 16) | ((uint32_t)b[p + 7] << 24));
        if (!memcmp(b + p, "fmt ", 4) && cl >= 16 && p + 8 + 16 <= n) memcpy(&wf, b + p + 8, 16);
        else if (!memcmp(b + p, "data", 4)) { data = b + p + 8; dlen = cl; if (p + 8 + dlen > n) dlen = (uint32_t)(n - p - 8); break; }
        p += 8 + cl + (cl & 1);
    }
    if (!data || wf.wFormatTag != WAVE_FORMAT_PCM || wf.nChannels < 1 || wf.wBitsPerSample != 16) { free(b); return; }
    wf.cbSize = 0;
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { g_wo = NULL; free(b); return; }
    g_wav = b; memset(&g_wh, 0, sizeof g_wh); g_wh.lpData = (LPSTR)data; g_wh.dwBufferLength = dlen;
    if (waveOutPrepareHeader(g_wo, &g_wh, sizeof g_wh) != MMSYSERR_NOERROR || waveOutWrite(g_wo, &g_wh, sizeof g_wh) != MMSYSERR_NOERROR) audio_stop();
}

static void finish(cui_ctx *ui) {
    if (getenv("BRIKS_INTRO_DEBUG")) fprintf(stderr, "intro: fim em %lld ms (total %lld)\n", (long long)((qpc() - g_t0) * 1000 / g_qf), (long long)g_total_ms);
    g_state = 2; audio_stop();
    if (g_dv_ok) { vpx_codec_destroy(&g_dv); g_dv_ok = false; }
    if (g_da_ok) { vpx_codec_destroy(&g_da); g_da_ok = false; }
    if (g_tex) cui_image_destroy(ui, g_tex); if (g_old) cui_image_destroy(ui, g_old); g_tex = g_old = 0;
    free(g_file); g_file = NULL; free(g_rgba); g_rgba = NULL;
}

static bool begin(bool enabled, bool start_min) {
    const char *at = getenv("BRIKS_INTRO_AT");
    if (at && at[0]) g_force_ms = atoll(at);
    else {
        if (!enabled || start_min || getenv("BRIKS_INTRO_OFF")) return false;
        if (!getenv("BRIKS_INTRO_REAL") && (getenv("CUI_SCREENSHOT") || getenv("BRIKS_SCRIPT") || getenv("BRIKS_HIDDEN"))) return false;   /* teste automatizado: nunca (BRIKS_INTRO_REAL força, para testar o relógio) */
    }
    char dir[MAX_PATH], webm[MAX_PATH + 32], wav[MAX_PATH + 32];
    exe_dir(dir, sizeof dir); snprintf(webm, sizeof webm, "%sassets\\intro\\abertura.webm", dir); snprintf(wav, sizeof wav, "%sassets\\intro\\abertura.wav", dir);
    g_file = read_all(webm, &g_len); if (!g_file) { if (getenv("BRIKS_INTRO_DEBUG")) fprintf(stderr, "intro: sem %s\n", webm); return false; }
    g_nfr = 0; g_scale = 1000000; g_vtrack = 1; g_cluster_tc = 0;
    walk(g_file, g_file + g_len);
    if (getenv("BRIKS_INTRO_DEBUG")) fprintf(stderr, "intro: %d quadros, vtrack %llu, escala %lld\n", g_nfr, (unsigned long long)g_vtrack, (long long)g_scale);
    if (g_nfr < 2) { free(g_file); g_file = NULL; return false; }
    g_total_ms = g_fr[g_nfr - 1].ms + (g_fr[g_nfr - 1].ms - g_fr[g_nfr - 2].ms);
    if (!dec_open(&g_dv)) { free(g_file); g_file = NULL; return false; }
    g_dv_ok = true;
    for (int i = 0; i < g_nfr; i++) if (g_fr[i].a) { g_da_ok = dec_open(&g_da); break; }
    g_next = 0; g_tex_idx = -1; g_have_rgba = false;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qf = f.QuadPart;
    if (g_force_ms < 0 && !getenv("BRIKS_INTRO_MUTE")) audio_start(wav);
    g_t0 = qpc(); g_state = 1; return true;
}

#define INTRO_OPACITY 0.8f   /* opacidade máxima da abertura (fundo e vídeo): o app aparece de leve por baixo */
typedef struct { cui_texture tex; int w, h; float a; } paint_t;
static void paint(cui_ctx *ui, cui_rect r, void *u) {
    const paint_t *p = (const paint_t *)u;
    cui_draw_rect(ui, r, CUI_CORNERS(0), (cui_color){ 0x0f, 0x03, 0x16, (uint8_t)(255.0f * p->a + 0.5f) });
    if (!p->tex || p->w <= 0 || p->h <= 0) return;
    float s = fminf(1.0f, fminf(r.w * 0.8f / (float)p->w, r.h * 0.8f / (float)p->h));
    float w = (float)p->w * s, h = (float)p->h * s;
    cui_draw_image(ui, p->tex, (cui_rect){ r.x + (r.w - w) * 0.5f, r.y + (r.h - h) * 0.5f, w, h }, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(0), (cui_color){ 255, 255, 255, (uint8_t)(255.0f * p->a + 0.5f) });
}

void intro_draw(cui_ctx *ui, bool enabled, bool start_minimized) {
    if (g_state == 2) return;
    if (g_state == 0 && !begin(enabled, start_minimized)) { g_state = 2; return; }
    const int64_t t = g_force_ms >= 0 ? g_force_ms : (int64_t)((qpc() - g_t0) * 1000 / g_qf);
    if (g_force_ms < 0 && (t >= g_total_ms + 150 || cui_key_pressed(ui, CUI_KEY_ESCAPE) || cui_key_pressed(ui, CUI_KEY_SPACE) || cui_key_pressed(ui, CUI_KEY_ENTER) || cui_mouse_clicked(ui, CUI_MOUSE_LEFT))) { finish(ui); return; }
    int idx = 0; while (idx + 1 < g_nfr && g_fr[idx + 1].ms <= t) idx++;
    if (idx != g_tex_idx && idx >= g_next - 1) {
        bool dok = decode_to(idx); if (getenv("BRIKS_INTRO_DEBUG")) fprintf(stderr, "intro: decode idx %d -> %d (%dx%d)\n", idx, (int)dok, g_w, g_h);
        if (dok && g_have_rgba) {
            if (g_old) cui_image_destroy(ui, g_old);
            g_old = g_tex; g_tex = cui_image_create(ui, g_w, g_h, g_rgba); g_tex_idx = idx;
        } else if (!g_have_rgba) { finish(ui); return; }   /* sem nada para mostrar: desiste */
    }
    float a = 1.0f;
    if (g_force_ms < 0) { int64_t left = g_total_ms + 150 - t; if (left < 350) a = left <= 0 ? 0.0f : (float)left / 350.0f; if (t < 120) a = (float)t / 120.0f; }
    paint_t pt = { g_tex, g_w, g_h, a * INTRO_OPACITY };
    cui_canvas_result cr = cui_canvas(ui, "intro-canvas", CUI_FILL, CUI_FILL, paint, &pt, sizeof pt);
    if (g_force_ms < 0 && cr.clicked) finish(ui);
}
bool intro_active(void) { return g_state == 1; }
int intro_probe(const char *name) {
    if (!strcmp(name, "intro_state")) return g_state;
    if (!strcmp(name, "intro_frames")) return g_nfr;
    if (!strcmp(name, "intro_w")) return g_w;
    if (!strcmp(name, "intro_idx")) return g_tex_idx;
    return -1;
}
#else
void intro_draw(cui_ctx *ui, bool enabled, bool start_minimized) { (void)ui; (void)enabled; (void)start_minimized; }   /* sem libvpx neste build (-NoVpx): sem abertura */
bool intro_active(void) { return false; }
int intro_probe(const char *name) { (void)name; return -1; }
#endif
