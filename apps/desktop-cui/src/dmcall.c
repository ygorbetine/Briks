/* Chamada individual e sinais (dmcall.h). Tudo passa por room.join (rtc_join_room): funciona em SFU e em P2P. */
#define _CRT_RAND_S
#include <stdlib.h>
#include "briks_ui.h"
#include "dmcall.h"
#include "ginvite.h"
#include "sysint.h"
#include "call.h"
#include "json.h"
#include "core.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <windows.h>

#define RING_MS 45000
#define NIN 4
typedef struct { bool used, shown; char fp[72], name[96], room[80]; ULONGLONG t, ring; } in_t;
static in_t g_in[NIN];
static struct { bool active, sent, connected; char fp[72], name[96], room[80], key[64], uid[48]; ULONGLONG t0; } g_out;
static bool g_open; static int g_open_for = -1;

static const person_t *friend_fp(const app_state_t *s, const char *fp) {
    if (!fp || !fp[0]) return NULL;
    for (int i = 0; i < s->friend_count; i++) if (s->friends[i].fp[0] && !strcmp(s->friends[i].fp, fp)) return &s->friends[i];
    return NULL;
}
static void rnd_hex(char *o, int n) {
    static const char *H = "0123456789abcdef"; unsigned v;
    for (int i = 0; i < n; i++) { if (rand_s(&v)) v = (unsigned)GetTickCount64() * 2654435761u + (unsigned)i; o[i] = H[v & 15]; }
    o[n] = 0;
}
static void say(app_state_t *s, const char *t) { snprintf(s->notice, sizeof s->notice, "%s", t); call_toast(t); }

void dmcall_log(const char *text) {
    if (core_state() != CORE_READY) return;
    char t[300], line[360]; json_escape(text, t, sizeof t);
    snprintf(line, sizeof line, "{\"cmd\":\"dm.log\",\"text\":\"%s\"}", t); core_send(line);
}
bool dmcall_calling(void) { return g_out.active; }
int dmcall_ringing(void) { int n = 0; for (int i = 0; i < NIN; i++) if (g_in[i].used) n++; return n; }

void dmcall_signal_stop(const char *key) {
    if (!key || !key[0] || core_state() != CORE_READY) return;
    char k[100], line[200]; json_escape(key, k, sizeof k);
    snprintf(line, sizeof line, "{\"cmd\":\"dm.signal.stop\",\"key\":\"%s\"}", k); core_send(line);
}
static bool send_signal(const char *uid, const char *kind, const char *key, int hold_s, const char *data_json) {
    char u[100], k[100], line[900]; json_escape(uid, u, sizeof u); json_escape(key, k, sizeof k);
    snprintf(line, sizeof line, "{\"cmd\":\"dm.signal\",\"user_id\":\"%s\",\"kind\":\"%s\",\"key\":\"%s\",\"hold_s\":%d,\"data\":%s}", u, kind, k, hold_s, data_json);
    return core_send(line);
}
static void out_stop(void) { if (g_out.key[0]) dmcall_signal_stop(g_out.key); g_out.active = false; g_out.key[0] = 0; }

void dmcall_start(app_state_t *s, int fi) {
    if (fi < 0 || fi >= s->friend_count || !s->friends[fi].fp[0] || !s->friends[fi].id[0]) return;
    if (g_out.active) return;
    if (s->in_room) { say(s, "Você já está em uma chamada. Saia dela antes de ligar para outra pessoa."); return; }
    const person_t *f = &s->friends[fi];
    memset(&g_out, 0, sizeof g_out);
    g_out.active = true;
    snprintf(g_out.fp, sizeof g_out.fp, "%s", f->fp); snprintf(g_out.name, sizeof g_out.name, "%s", f->name); snprintf(g_out.uid, sizeof g_out.uid, "%s", f->id);
    char r[40]; rnd_hex(r, 24); snprintf(g_out.room, sizeof g_out.room, "dm-%s", r);   /* aleatória: nunca derivada das impressões digitais */
    char k[24]; rnd_hex(k, 12); snprintf(g_out.key, sizeof g_out.key, "call-%s", k);
    g_out.t0 = GetTickCount64();
    s->notice[0] = 0; s->screen = SCREEN_CALL; s->mic_on = g_sys.mic_on;
    rtc_join_room(s, g_out.room);
}

static void out_try_send(app_state_t *s) {
    if (!g_out.active || g_out.sent || core_state() != CORE_READY || !s->rtc_session_ready) return;
    char n[200], rm[200], d[500]; json_escape(s->user_name[0] ? s->user_name : "Alguém", n, sizeof n); json_escape(g_out.room, rm, sizeof rm);
    snprintf(d, sizeof d, "{\"room\":\"%s\",\"name\":\"%s\"}", rm, n);
    if (send_signal(g_out.uid, "call", g_out.key, 45, d)) g_out.sent = true;
}

void dmcall_tick(app_state_t *s) {
    ULONGLONG now = GetTickCount64();
    if (g_out.active) {
        out_try_send(s);
        if (now - g_out.t0 > 1500 && !s->in_room) { dmcall_log("chamada: desligou antes de atenderem"); out_stop(); }   /* desligou antes de atenderem */
        else if (s->in_room && s->rtc_peer_count > 0) { dmcall_log("chamada: a pessoa entrou, sinal retirado"); g_out.connected = true; out_stop(); }
        else if (now - g_out.t0 > RING_MS) {
            char t[160]; snprintf(t, sizeof t, "%s não atendeu.", g_out.name); dmcall_log("chamada: ninguem atendeu em 45 s"); out_stop();
            if (s->in_room) { rtc_leave_room(s); s->screen = SCREEN_SOCIAL; }
            say(s, t);
        }
    }
    ginvite_tick(s);
    for (int i = 0; i < NIN; i++) if (g_in[i].used) {
        if (now - g_in[i].t > RING_MS + 15000) { if (g_open_for == i) { g_open = false; g_open_for = -1; } g_in[i].used = false; continue; }
        if (g_in[i].shown && now - g_in[i].ring > 3500) { g_in[i].ring = now; sound_play(SND_NOTIFY); }
    }
}

bool dmcall_event(app_state_t *s, const char *ev, const jval *j) {
    if (!strcmp(ev, "dm.signal.sent")) {
        const jval *ok = json_get(j, "ok");
        if (!(ok && ok->type == J_BOOL && ok->b) && g_out.active && !strcmp(json_str(j, "key", ""), g_out.key)) { say(s, "Não foi possível avisar a pessoa agora."); g_out.t0 = GetTickCount64() - (RING_MS - 3000); g_out.sent = true; }
        return true;
    }
    if (!strcmp(ev, "dmcall.script")) { dmcall_start(s, 0); return true; }
    if (!strcmp(ev, "friendtoast.script")) { friends_toast_demo(s); return true; }
    if (strcmp(ev, "dm.signal") && strcmp(ev, "dm.signal.gone")) return false;
    const char *fp = json_str(j, "fp", ""), *kind = json_str(j, "kind", ""); const jval *d = json_get(j, "data");
    const bool gone = !strcmp(ev, "dm.signal.gone");
    const person_t *f = friend_fp(s, fp);
    if (!f) { dmcall_log("sinal descartado: remetente nao esta na lista de amigos"); return true; }
    if (!d) return true;
    if (!strcmp(kind, "call")) {
        const char *room = json_str(d, "room", "");
        if (!room[0] || strlen(room) > 70 || strncmp(room, "dm-", 3)) { dmcall_log("sinal call descartado: sala invalida"); return true; }
        if (gone) {
            for (int i = 0; i < NIN; i++) if (g_in[i].used && !strcmp(g_in[i].room, room) && !strcmp(g_in[i].fp, fp)) {
                const bool was_open = g_in[i].shown; g_in[i].used = false; if (g_open_for == i) { g_open = false; g_open_for = -1; }
                if (was_open) { char t[160]; snprintf(t, sizeof t, "Chamada perdida de %s.", f->name); notify_post(s, NT_INFO, NULL, "Briks", t); say(s, t); dmcall_log("sinal call retirado: chamada perdida"); }
            }
            return true;
        }
        for (int i = 0; i < NIN; i++) if (g_in[i].used && !strcmp(g_in[i].room, room)) return true;   /* já mostrada */
        int k = -1; for (int i = 0; i < NIN; i++) if (!g_in[i].used) { k = i; break; }
        if (k < 0) return true;
        in_t *e = &g_in[k]; memset(e, 0, sizeof *e); e->used = true; e->t = e->ring = GetTickCount64();
        snprintf(e->fp, sizeof e->fp, "%s", fp); snprintf(e->room, sizeof e->room, "%s", room); snprintf(e->name, sizeof e->name, "%s", f->name);
        char t[160]; snprintf(t, sizeof t, "%s está te ligando", f->name); notify_post(s, NT_REQUEST, NULL, "Briks", t); sound_play(SND_NOTIFY);
        dmcall_log("sinal call de amigo: cartao aberto");
        return true;
    }
    if (!strcmp(kind, "ginv") && !gone) {
        const char *gn = json_str(d, "name", "um grupo"), *iv = json_str(d, "inviter", f->name);
        char t[200]; snprintf(t, sizeof t, "%s convidou você para %s", iv[0] ? iv : f->name, gn);
        notify_post(s, NT_INFO, NULL, "Briks", t); say(s, t);
        chat_dm_wake(s, fp);   /* abre o canal direto para o cartão chegar */
        dmcall_log("sinal ginv de amigo: aviso mostrado, abrindo canal direto");
    }
    return true;
}

void dmcall_signal_ginv(app_state_t *s, const person_t *f, const char *gid, const char *gname, const char *inviter, const char *key) {
    (void)s; if (!f || !f->id[0]) return;
    char g[100], n[200], iv[200], d[700]; json_escape(gid, g, sizeof g); json_escape(gname, n, sizeof n); json_escape(inviter, iv, sizeof iv);
    snprintf(d, sizeof d, "{\"gid\":\"%s\",\"name\":\"%s\",\"inviter\":\"%s\"}", g, n, iv);
    send_signal(f->id, "ginv", key, 600, d);
}

static void answer(app_state_t *s, int i) {
    in_t e = g_in[i]; g_in[i].used = false; g_open = false; g_open_for = -1;
    dmcall_log("chamada recebida: atendida");
    if (s->in_room) rtc_leave_room(s);
    s->rtc_session_sent = s->rtc_session_ready; s->screen = SCREEN_CALL; s->mic_on = g_sys.mic_on; rtc_join_room(s, e.room);
}

void dmcall_button(cui_ctx *ui, app_state_t *s) {
    if (s->active_friend < 0 || s->active_friend >= s->friend_count || !s->friends[s->active_friend].fp[0]) return;
    if (cui_icon_button(ui, CUI_ICON_PHONE, .tooltip = g_out.active ? "Chamando…" : s->in_room ? "Você já está em uma chamada" : "Ligar", .disabled = g_out.active || s->in_room)) dmcall_start(s, s->active_friend);
}

void dmcall_overlay(cui_ctx *ui, app_state_t *s) {
    if (!(g_out.active && !g_out.connected && s->in_room)) return;   /* faixa "Chamando…" no topo, em cima da tela de chamada */
    ULONGLONG now = GetTickCount64();
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_START, .pad_top = 48, .pad_x = 8) {
        cui_box(ui, .id = "dmcall-pill", .dir = CUI_ROW, .pad_x = 14, .pad_y = 6, .gap = 10, .radius = 20, .bg = CUI_RGBA(0x14141cee), .border = 1, .border_color = B_GLASS_BORDER,
                .align = CUI_ALIGN_CENTER, .elevation = briks_elev(CUI_ELEVATION_RAISED)) {
            char t[160]; int left = (int)((RING_MS - (now - g_out.t0)) / 1000) + 1; if (left < 0) left = 0;
            snprintf(t, sizeof t, "Chamando %s… %d s", g_out.name, left);
            cui_icon(ui, CUI_ICON_PHONE, 14, B_C3); cui_text(ui, t, .size = 13, .nowrap = true);
            if (cui_button(ui, "Cancelar", .variant = CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM)) { dmcall_log("chamada: cancelada por mim"); out_stop(); rtc_leave_room(s); s->screen = SCREEN_SOCIAL; }
        }
    }
    cui_request_wakeup(ui, 0.5f);
}

void dmcall_dialog(cui_ctx *ui, app_state_t *s) {
    int cur = -1; for (int i = 0; i < NIN; i++) if (g_in[i].used) { cur = i; break; }
    if (cur < 0) { g_open = false; g_open_for = -1; return; }
    if (g_open_for != cur) { g_open_for = cur; g_open = true; g_in[cur].shown = true; g_in[cur].ring = GetTickCount64(); }
    cui_vec2 vp = cui_viewport_size(ui);
    cui_dialog(ui, "Chamada recebida", &g_open, .width = fminf(380.0f, fmaxf(vp.x - 48.0f, 260.0f))) {
        char t[160]; snprintf(t, sizeof t, "%s está te ligando", g_in[cur].name);
        cui_row(ui, .gap = 10, .align = CUI_ALIGN_CENTER) { cui_icon(ui, CUI_ICON_PHONE, 20, B_C3); cui_text(ui, t, .font = CUI_FONT_MEDIUM); }
        if (s->in_room) cui_text(ui, "Atender vai tirar você da chamada em que está agora.", .size = 12.5f, .color = B_MUTED, .width = CUI_FILL);
        cui_dialog_footer(ui) {
            if (cui_button(ui, "Recusar", .variant = CUI_BUTTON_SECONDARY)) { dmcall_log("chamada recebida: recusada"); g_in[cur].used = false; g_open = false; g_open_for = -1; }
            else if (cui_button(ui, s->in_room ? "Sair e atender" : "Atender", .variant = CUI_BUTTON_PRIMARY)) answer(s, cur);
        }
    }
    if (g_open_for == cur && g_in[cur].used && !g_open) { g_in[cur].used = false; g_open_for = -1; }   /* fechou no X: ignora essa sala */
    cui_request_wakeup(ui, 0.5f);
}
