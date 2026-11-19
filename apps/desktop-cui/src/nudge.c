/* "Chamar atenção" (nudge estilo MSN): o amigo vê a tela tremer por ~1 s.
 *  - envio: sinal assinado "nudge" na caixa de chegada do amigo (comando dm.signal do core; limites em nudge_model.h);
 *  - recepção: só de AMIGO (fp da lista), não bloqueado, 1 a cada 15 s por amigo; o core já conferiu assinatura, destinatário e idade;
 *  - efeito: imagem do monitor do cursor, SÓ local (nunca enviada nem gravada em disco), numa janela sem borda, topmost, sem foco e
 *    clique-através; treme com amplitude decrescente e some. A imagem é liberada no fim. Só a janela do Briks: move uns poucos pixels e volta.
 *  - não treme: Não perturbar, jogo/apresentação (SHQueryUserNotificationState), Briks fechado na bandeja (só aviso) ou em segundo plano (pisca). */
#include "sysint.h"
#include "nudge_model.h"
#include "call.h"
#include "json.h"
#include <windows.h>
#include <shellapi.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern bool core_send(const char *json_line);
bool friends_blocked(app_state_t *s, const char *id);
void friends_banner_text(const char *text);   /* ui_friends.c: a faixa verde no topo da conversa (some sozinha) */

#define FX_SCREEN_MS 1000
#define FX_WINDOW_MS 600
#define FX_TIMER 1
static nm_limiter g_tx, g_rx;
static int g_sent, g_recv_ok, g_dropped, g_tx_denied, g_rx_denied, g_last_action = -1, g_fx_started, g_fx_screen, g_fx_window, g_flashes, g_notices;
static int g_t_busy = -1, g_t_bg = -1, g_t_tray = -1, g_t_max = -1;   /* roteiro: -1 = valor real */
static unsigned long long g_clock_off;                                   /* roteiro: adianta o relógio dos limites */
static bool g_fx_busy_state;

static unsigned long long now_ms(void) { return GetTickCount64() + g_clock_off; }

/* ---------- Não perturbar ---------- */
bool nudge_dnd_active(void) { return nm_dnd_active(g_sys.dnd_until, (long long)time(NULL)); }
bool nudge_dnd_silences_calls(void) { return g_sys.dnd_calls && nudge_dnd_active(); }
void nudge_dnd_set(int minutes, bool on) {
    g_sys.dnd_until = on ? nm_dnd_for(minutes, (long long)time(NULL)) : 0;
    sys_cfg_save();
}

/* ---------- efeito ---------- */
static HWND g_fx;                    /* janela do efeito (tela inteira) ou janela-relógio escondida (só a janela) */
static HDC g_mem; static HBITMAP g_bmp, g_old; static int g_mw, g_mh;
static int g_fx_mode;                /* NM_SHAKE_SCREEN ou NM_SHAKE_WINDOW */
static ULONGLONG g_fx_t0; static unsigned g_rng = 2463534242u;
static RECT g_win_saved; static HWND g_win_target;
static int g_cur_dx, g_cur_dy;

static unsigned rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng & 0xffff; }

static void fx_free(void) {   /* a imagem capturada nunca fica na memória depois do efeito */
    if (g_mem) { if (g_old) SelectObject(g_mem, g_old); DeleteDC(g_mem); }
    if (g_bmp) DeleteObject(g_bmp);
    g_mem = NULL; g_bmp = NULL; g_old = NULL;
}
static void fx_end(void) {
    if (g_fx) { KillTimer(g_fx, FX_TIMER); HWND w = g_fx; g_fx = NULL; DestroyWindow(w); }
    if (g_win_target) { if (IsWindow(g_win_target) && !IsZoomed(g_win_target) && !IsIconic(g_win_target)) SetWindowPos(g_win_target, NULL, g_win_saved.left, g_win_saved.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); g_win_target = NULL; }
    fx_free();
}
static void fx_paint(HDC dc) {
    if (!g_mem) return;
    const int dx = g_cur_dx, dy = g_cur_dy;
    BitBlt(dc, dx, dy, g_mw, g_mh, g_mem, 0, 0, SRCCOPY);
    HBRUSH k = (HBRUSH)GetStockObject(BLACK_BRUSH);   /* o que o deslocamento deixa descoberto */
    RECT r;
    if (dx > 0) { SetRect(&r, 0, 0, dx, g_mh); FillRect(dc, &r, k); } else if (dx < 0) { SetRect(&r, g_mw + dx, 0, g_mw, g_mh); FillRect(dc, &r, k); }
    if (dy > 0) { SetRect(&r, 0, 0, g_mw, dy); FillRect(dc, &r, k); } else if (dy < 0) { SetRect(&r, 0, g_mh + dy, g_mw, g_mh); FillRect(dc, &r, k); }
}
static LRESULT CALLBACK fx_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); if (g_fx_mode == NM_SHAKE_SCREEN) fx_paint(dc); EndPaint(h, &ps); return 0; }
    case WM_TIMER: {
        if (w != FX_TIMER || h != g_fx) break;
        const unsigned total = g_fx_mode == NM_SHAKE_SCREEN ? FX_SCREEN_MS : FX_WINDOW_MS, amp = g_fx_mode == NM_SHAKE_SCREEN ? 22 : 9;
        unsigned el = (unsigned)(GetTickCount64() - g_fx_t0);
        if (el >= total) { fx_end(); return 0; }
        g_cur_dx = nm_offset((int)amp, (int)el, (int)total, rnd()); g_cur_dy = nm_offset((int)amp, (int)el, (int)total, rnd());
        if (g_fx_mode == NM_SHAKE_SCREEN) { HDC dc = GetDC(h); if (dc) { fx_paint(dc); ReleaseDC(h, dc); } }
        else if (g_win_target && IsWindow(g_win_target) && !IsZoomed(g_win_target) && !IsIconic(g_win_target)) SetWindowPos(g_win_target, NULL, g_win_saved.left + g_cur_dx, g_win_saved.top + g_cur_dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return 0; }
    case WM_DESTROY: if (h == g_fx) g_fx = NULL; break;
    }
    return DefWindowProcW(h, m, w, l);
}
static bool fx_class(void) {
    static bool ok, tried; if (tried) return ok; tried = true;
    WNDCLASSEXW c = { sizeof c }; c.lpfnWndProc = fx_proc; c.hInstance = GetModuleHandleW(NULL); c.lpszClassName = L"BriksNudgeFx"; c.hCursor = LoadCursor(NULL, IDC_ARROW);
    ok = RegisterClassExW(&c) != 0; return ok;
}
static bool fx_real(void) { return !sys_is_headless() || getenv("BRIKS_NUDGE_REAL"); }   /* teste automático conta, mas não cobre a tela de ninguém */

/* treme a imagem de uma região da tela (monitor inteiro, ou só o retângulo da janela do Briks quando ela está maximizada) */
static void shake_region(const RECT *rg) {
    if (g_fx || !fx_class()) return;
    const int l = rg->left, t = rg->top; g_mw = rg->right - l; g_mh = rg->bottom - t;
    if (g_mw < 64 || g_mh < 64) return;
    HDC scr = GetDC(NULL); if (!scr) return;
    g_mem = CreateCompatibleDC(scr); g_bmp = g_mem ? CreateCompatibleBitmap(scr, g_mw, g_mh) : NULL;
    bool ok = g_mem && g_bmp; if (ok) { g_old = (HBITMAP)SelectObject(g_mem, g_bmp); ok = BitBlt(g_mem, 0, 0, g_mw, g_mh, scr, l, t, SRCCOPY | CAPTUREBLT) != 0; }
    ReleaseDC(NULL, scr);
    if (!ok) { fx_free(); return; }
    g_cur_dx = g_cur_dy = 0; g_fx_mode = NM_SHAKE_SCREEN; g_fx_t0 = GetTickCount64();
    g_fx = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, L"BriksNudgeFx", L"", WS_POPUP, l, t, g_mw, g_mh, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_fx) { fx_free(); return; }
    SetLayeredWindowAttributes(g_fx, 0, 255, LWA_ALPHA);
    SetWindowPos(g_fx, HWND_TOPMOST, l, t, g_mw, g_mh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    { HDC dc = GetDC(g_fx); if (dc) { fx_paint(dc); ReleaseDC(g_fx, dc); } }   /* quadro 0 = a própria tela: sem piscada */
    SetTimer(g_fx, FX_TIMER, 15, NULL);
    g_fx_screen++;
}
static void shake_screen(void) {
    POINT cp; GetCursorPos(&cp);
    MONITORINFO mi = { sizeof mi }; HMONITOR mon = MonitorFromPoint(cp, MONITOR_DEFAULTTONEAREST);
    if (GetMonitorInfoW(mon, &mi)) shake_region(&mi.rcMonitor);
}
static void shake_window(void) {
    HWND app = (HWND)sys_hwnd(); if (g_fx || !app || !fx_class() || IsIconic(app) || !IsWindowVisible(app)) return;
    if (IsZoomed(app)) {   /* maximizada não tem para onde andar: treme só a imagem da própria janela (nada além do retângulo dela) */
        RECT r; GetWindowRect(app, &r);
        HMONITOR mon = MonitorFromWindow(app, MONITOR_DEFAULTTONEAREST); MONITORINFO mi = { sizeof mi };
        if (GetMonitorInfoW(mon, &mi) && !IntersectRect(&r, &r, &mi.rcMonitor)) return;
        shake_region(&r); return;
    }
    GetWindowRect(app, &g_win_saved); g_win_target = app; g_fx_mode = NM_SHAKE_WINDOW; g_fx_t0 = GetTickCount64();
    g_fx = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"BriksNudgeFx", L"", WS_POPUP, 0, 0, 1, 1, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);   /* só o relógio: nunca aparece */
    if (!g_fx) { g_win_target = NULL; return; }
    SetTimer(g_fx, FX_TIMER, 15, NULL);
    g_fx_window++;
}

/* ---------- contexto e ação ---------- */
static bool os_busy(void) {
    if (g_t_busy >= 0) return g_t_busy != 0;
    QUERY_USER_NOTIFICATION_STATE st = QUNS_ACCEPTS_NOTIFICATIONS;
    if (FAILED(SHQueryUserNotificationState(&st))) return false;
    return st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_BUSY || st == QUNS_PRESENTATION_MODE;
}
static void ctx_build(nm_ctx *c) {
    HWND h = (HWND)sys_hwnd();
    memset(c, 0, sizeof *c);
    c->recv_on = g_sys.nudge_on; c->dnd = nudge_dnd_active(); c->mode = g_sys.nudge_mode; c->os_busy = os_busy();
    c->in_tray = g_t_tray >= 0 ? g_t_tray != 0 : (h && !IsWindowVisible(h)) || (!h && !sys_is_headless());
    c->background = g_t_bg >= 0 ? g_t_bg != 0 : (h && (IsIconic(h) || GetForegroundWindow() != h)) || !h;
    c->maximized = g_t_max >= 0 ? g_t_max != 0 : (h && IsZoomed(h));
    if (sys_is_headless() && g_t_tray < 0) c->in_tray = false;
}
static void do_action(app_state_t *s, nm_action a, const char *who) {
    char t[160]; snprintf(t, sizeof t, "%s chamou sua atenção", who && who[0] ? who : "Alguém");
    g_last_action = (int)a;
    switch (a) {
    case NM_SHAKE_SCREEN: if (fx_real()) shake_screen(); else g_fx_screen++; g_fx_started++; if (g_sys.nudge_sound) sound_play(SND_NOTIFY); break;
    case NM_SHAKE_WINDOW: if (fx_real()) shake_window(); else g_fx_window++; g_fx_started++; if (g_sys.nudge_sound) sound_play(SND_NOTIFY); break;
    case NM_FLASH: {
        HWND h = (HWND)sys_hwnd();
        if (h && fx_real()) { FLASHWINFO fi = { sizeof fi, h, FLASHW_TRAY | FLASHW_TIMERNOFG, 5, 0 }; FlashWindowEx(&fi); }
        g_flashes++;
        if (g_sys.nudge_sound) notify_post(s, NT_REQUEST, NULL, "Briks", t); else { tray_balloon("Briks", t); }
        break; }
    case NM_NOTICE: g_notices++; tray_balloon("Briks", t); if (g_sys.nudge_sound) sound_play(SND_NOTIFY); break;
    default: break;
    }
    (void)s;
}

static const person_t *friend_by_fp(const app_state_t *s, const char *fp) {
    if (!fp || strlen(fp) < 16) return NULL;
    for (int i = 0; i < s->friend_count; i++) if (s->friends[i].fp[0] && !strcmp(s->friends[i].fp, fp)) return &s->friends[i];
    return NULL;
}

void nudge_core_event(app_state_t *s, const jval *j) {
    const char *fp = json_str(j, "fp", "");
    const person_t *f = friend_by_fp(s, fp);
    if (!f) { g_dropped++; return; }                                       /* não é amigo: descarta */
    if (friends_blocked(s, f->id)) { g_dropped++; return; }                 /* bloqueado: descarta */
    if (!nm_allow(&g_rx, fp, now_ms())) { g_rx_denied++; return; }          /* 1 a cada 15 s por amigo (e 6/min no total) */
    nudge_dnd_active();
    g_recv_ok++;
    nm_ctx c; ctx_build(&c);
    nm_action a = nm_decide(&c);
    g_last_action = (int)a;
    if (a != NM_NONE) do_action(s, a, f->name);
}

/* ---------- envio ---------- */
static void toast(app_state_t *s, const char *t) { if (s->screen == SCREEN_CALL) call_toast(t); else friends_banner_text(t); }
static const person_t *friend_by_id(const app_state_t *s, const char *id) {
    if (!id || !id[0]) return NULL;
    for (int i = 0; i < s->friend_count; i++) if (!strcmp(s->friends[i].id, id)) return &s->friends[i];
    return NULL;
}
static bool send_to(app_state_t *s, const person_t *f) {
    if (!f || !f->id[0]) return false;
    if (!nm_allow(&g_tx, f->id, now_ms())) { g_tx_denied++; toast(s, "Aguarde alguns segundos"); return false; }
    char nm[200], uid[100], me[200], key[24], line[640];
    json_escape(f->name, nm, sizeof nm); json_escape(f->id, uid, sizeof uid); json_escape(s->user_name, me, sizeof me);
    snprintf(key, sizeof key, "nudge-%.8s", f->id);
    snprintf(line, sizeof line, "{\"cmd\":\"dm.signal\",\"kind\":\"nudge\",\"key\":\"%s\",\"user_id\":\"%s\",\"data\":{\"name\":\"%s\"},\"hold_s\":5}", key, uid, me);
    (void)nm;
    char m[200];
    if (!core_send(line)) { snprintf(m, sizeof m, "Não foi possível enviar agora."); toast(s, m); return false; }
    g_sent++;
    snprintf(m, sizeof m, "Você chamou a atenção de %s", f->name[0] ? f->name : "seu amigo");   /* nunca diz se o outro está em Não perturbar */
    toast(s, m);
    return true;
}
bool nudge_send(app_state_t *s, int friend_idx) {
    if (friend_idx < 0 || friend_idx >= s->friend_count) return false;
    return send_to(s, &s->friends[friend_idx]);
}
/* pelo bloco da chamada: acha o amigo pelo fp/id do participante */
bool nudge_send_peer(app_state_t *s, int slot) {
    if (slot < 0 || slot >= BRIKS_MAX_PEERS || !s->rtc_peers[slot].id[0]) return false;
    const person_t *f = friend_by_id(s, s->rtc_peers[slot].id);
    if (!f) f = friend_by_fp(s, s->rtc_peers[slot].fp);
    if (!f) { toast(s, "Só dá para chamar a atenção de amigos."); return false; }
    return send_to(s, f);
}

void nudge_tick(app_state_t *s) {
    static long long last_chk;
    long long t = (long long)time(NULL);
    if (t == last_chk) return; last_chk = t;
    if (g_sys.dnd_until > 0 && t >= g_sys.dnd_until) { g_sys.dnd_until = 0; sys_cfg_save(); if (s->app) cui_request_redraw(cui_app_ui(s->app)); }   /* o tempo acabou */
    if (g_fx && GetTickCount64() - g_fx_t0 > 2500) fx_end();   /* guarda: o efeito nunca passa de ~2,5 s */
}

/* ---------- Configurações ---------- */
#define NUDGE_CARD(ui, title) CUI__SCOPE((cui_box_begin((ui), CUI__OPTS(cui_box, .width = CUI_FILL, .pad = 14, .gap = 10, .radius = 16, .bg = B_GLASS_CARD, \
    .border = 1, .border_color = B_GLASS_BORDER)), cui_text((ui), (title), .font = CUI_FONT_MEDIUM, .size = 13, .color = B_C3)), cui_box_end(ui))
static void nhint(cui_ctx *ui, const char *t) { cui_text(ui, t, .size = 12, .color = B_MUTED); }
void nudge_settings(cui_ctx *ui) {
    NUDGE_CARD(ui, "CHAMAR ATENÇÃO") {
        if (cui_switch(ui, "Receber chamadas de atenção de amigos", &g_sys.nudge_on)) sys_cfg_save();
        nhint(ui, "Um amigo pode fazer a sua tela tremer por um instante. Só amigos, no máximo 1 vez a cada 15 segundos por amigo. Nada é enviado nem gravado: a imagem da tela só existe no seu computador durante o efeito.");
        static const char *const modes[] = { "Tela inteira (efeito)", "Só a tela do Briks", "Só aviso" };
        int m = g_sys.nudge_mode; cui_text(ui, "Efeito", .size = 13);
        if (cui_segmented(ui, "nudge-mode", modes, 3, &m, .full_width = true, .size = CUI_SIZE_SM) && m != g_sys.nudge_mode) { g_sys.nudge_mode = m; sys_cfg_save(); }
        nhint(ui, m == 0 ? "Treme uma imagem do monitor onde está o mouse. Não pega o teclado nem o mouse." : m == 1 ? "Só a janela do Briks se mexe por um instante." : "Só um aviso do sistema, sem tremer.");
        if (cui_switch(ui, "Som ao receber", &g_sys.nudge_sound)) sys_cfg_save();
        if (cui_button(ui, "Testar o efeito", .variant = CUI_BUTTON_SECONDARY)) { nm_ctx c; ctx_build(&c); c.recv_on = true; c.dnd = false; c.os_busy = false; c.in_tray = false; c.background = false; do_action(NULL, nm_decide(&c), "Teste"); }
        nhint(ui, "Não treme com jogo em tela cheia ou apresentação, nem com o Briks na bandeja (só aviso) ou em segundo plano (pisca na barra de tarefas).");
    }
    NUDGE_CARD(ui, "NÃO PERTURBAR") {
        if (nudge_dnd_active()) {
            if (g_sys.dnd_until > 0) { time_t u = (time_t)g_sys.dnd_until; struct tm tmv; localtime_s(&tmv, &u); cui_textf(ui, "Ligado até %02d:%02d", tmv.tm_hour, tmv.tm_min); }
            else cui_text(ui, "Ligado até você desligar", .size = 13);
            if (cui_button(ui, "Desligar", .variant = CUI_BUTTON_SECONDARY)) nudge_dnd_set(0, false);
        } else {
            nhint(ui, "Silencia as chamadas de atenção e os sons delas. Quem chamou não fica sabendo.");
            cui_row(ui, .gap = 8) {
                if (cui_button(ui, "30 minutos", .variant = CUI_BUTTON_SECONDARY)) nudge_dnd_set(30, true);
                if (cui_button(ui, "1 hora", .variant = CUI_BUTTON_SECONDARY)) nudge_dnd_set(60, true);
                if (cui_button(ui, "Até eu desligar", .variant = CUI_BUTTON_SECONDARY)) nudge_dnd_set(0, true);
            }
        }
        if (cui_switch(ui, "Não perturbar também silencia chamadas", &g_sys.dnd_calls)) sys_cfg_save();
        nhint(ui, "Desligado por padrão: com ele desligado, o toque de uma chamada recebida continua tocando.");
    }
}

/* ---------- roteiro de teste ---------- */
int nudge_probe(const char *n) {
    if (!strcmp(n, "sent")) return g_sent;
    if (!strcmp(n, "recv")) return g_recv_ok;
    if (!strcmp(n, "dropped")) return g_dropped;
    if (!strcmp(n, "tx_denied")) return g_tx_denied;
    if (!strcmp(n, "rx_denied")) return g_rx_denied;
    if (!strcmp(n, "action")) return g_last_action;          /* 0 nada, 1 tela, 2 janela, 3 pisca, 4 aviso */
    if (!strcmp(n, "fx_screen")) return g_fx_screen;
    if (!strcmp(n, "fx_window")) return g_fx_window;
    if (!strcmp(n, "flashes")) return g_flashes;
    if (!strcmp(n, "notices")) return g_notices;
    if (!strcmp(n, "dnd")) return nudge_dnd_active();
    if (!strcmp(n, "dnd_calls")) return nudge_dnd_silences_calls();
    if (!strcmp(n, "on")) return g_sys.nudge_on;
    if (!strcmp(n, "mode")) return g_sys.nudge_mode;
    if (!strcmp(n, "fx_live")) return g_fx != NULL;
    return -9999;
}
bool nudge_script(const char *a) {
    char b[200]; snprintf(b, sizeof b, "%s", a ? a : ""); char *t[4]; int n = 0;
    for (char *p = strtok(b, " "); p && n < 4; p = strtok(NULL, " ")) t[n++] = p;
    if (!n) return false;
    extern app_state_t *sys_state(void);
    app_state_t *s = sys_state();
    if (!strcmp(t[0], "send") && n > 1 && s) nudge_send(s, atoi(t[1]));
    else if (!strcmp(t[0], "sendpeer") && n > 1 && s) nudge_send_peer(s, atoi(t[1]));
    else if (!strcmp(t[0], "busy") && n > 1) g_t_busy = atoi(t[1]);
    else if (!strcmp(t[0], "bg") && n > 1) g_t_bg = atoi(t[1]);
    else if (!strcmp(t[0], "tray") && n > 1) g_t_tray = atoi(t[1]);
    else if (!strcmp(t[0], "max") && n > 1) g_t_max = atoi(t[1]);
    else if (!strcmp(t[0], "clock") && n > 1) g_clock_off += (unsigned long long)atoll(t[1]);
    else if (!strcmp(t[0], "dnd") && n > 1) { if (!strcmp(t[1], "off")) nudge_dnd_set(0, false); else nudge_dnd_set(atoi(t[1]), true); }
    else if (!strcmp(t[0], "dndexpired")) { g_sys.dnd_until = (long long)time(NULL) - 5; }
    else if (!strcmp(t[0], "cfg") && n > 2) { int v = atoi(t[2]); if (!strcmp(t[1], "on")) g_sys.nudge_on = v != 0; else if (!strcmp(t[1], "mode")) g_sys.nudge_mode = v; else if (!strcmp(t[1], "sound")) g_sys.nudge_sound = v != 0; else if (!strcmp(t[1], "calls")) g_sys.dnd_calls = v != 0; }
    else if (!strcmp(t[0], "friendfp") && n > 2 && s && atoi(t[1]) >= 0 && atoi(t[1]) < s->friend_count) snprintf(s->friends[atoi(t[1])].fp, sizeof s->friends[0].fp, "%s", t[2]);   /* teste: dá um fp ao amigo da demo */
    else if (!strcmp(t[0], "tick")) { if (s) nudge_tick(s); }
    else return false;
    return true;
}
