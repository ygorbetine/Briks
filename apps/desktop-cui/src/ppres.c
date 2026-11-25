#include "ppres.h"
#include "core.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

static struct { char id[48]; ULONGLONG last, replied; bool reply_due; } g_pp[MAX_FRIENDS];
static ULONGLONG g_sent;          /* último anúncio meu */
static bool g_sent_invisible;     /* o que anunciei na última vez */
static char g_who[48];

void ppres_reset(void) { memset(g_pp, 0, sizeof g_pp); g_sent = 0; g_who[0] = 0; g_sent_invisible = false; }

static int slot_for(const char *id, bool create) {
    int freeslot = -1;
    for (int i = 0; i < MAX_FRIENDS; i++) { if (!strcmp(g_pp[i].id, id)) return i; if (freeslot < 0 && !g_pp[i].id[0]) freeslot = i; }
    if (!create || freeslot < 0) return -1;
    snprintf(g_pp[freeslot].id, sizeof g_pp[freeslot].id, "%s", id);
    return freeslot;
}

bool ppres_online(const char *friend_id) {
    if (!friend_id || !friend_id[0]) return false;
    const int i = slot_for(friend_id, false);
    return i >= 0 && ppres_fresh(g_pp[i].last, GetTickCount64());
}

void ppres_event(app_state_t *s, const jval *j) {
    const char *fp = json_str(j, "fp", ""), *kind = json_str(j, "kind", "");
    if (!fp[0] || s->invisible) return;                                  /* invisível: não vejo nem respondo */
    const person_t *f = NULL;
    for (int i = 0; i < s->friend_count; i++) if (!strcmp(s->friends[i].fp, fp)) { f = &s->friends[i]; break; }
    if (!f || friends_blocked(s, f->id)) return;                          /* só de amigo, e não bloqueado */
    const int k = slot_for(f->id, true); if (k < 0) return;
    if (!strcmp(kind, "bye")) { g_pp[k].last = 0; cui_request_redraw(cui_app_ui(s->app)); return; }
    const jval *ack = json_get(j, "data"); const jval *a = ack ? json_get(ack, "ack") : NULL;
    g_pp[k].last = GetTickCount64();
    if (!(a && a->type == J_BOOL && a->b)) g_pp[k].reply_due = true;     /* ele acabou de entrar: eu respondo que estou aqui */
    cui_request_redraw(cui_app_ui(s->app));
}

static void send_hello(const char *kind, bool ack, const char (*ids)[48], int n) {
    if (n <= 0) return;
    char line[128 + 128 * 52]; int w = snprintf(line, sizeof line, "{\"cmd\":\"presence.hello\",\"kind\":\"%s\",\"ack\":%s,\"friends\":[", kind, ack ? "true" : "false");
    for (int i = 0; i < n && w < (int)sizeof line - 64; i++) w += snprintf(line + w, sizeof line - w, "%s\"%s\"", i ? "," : "", ids[i]);
    snprintf(line + w, sizeof line - w, "]}");
    core_send(line);
}

void ppres_tick(app_state_t *s) {
    if (core_state() != CORE_READY || !s->acct_user_id[0] || s->acct_demo || !s->friends_loaded) { if (!s->acct_user_id[0]) ppres_reset(); return; }
    const ULONGLONG now = GetTickCount64();
    if (strcmp(g_who, s->acct_user_id)) { ppres_reset(); snprintf(g_who, sizeof g_who, "%s", s->acct_user_id); }
    static char ids[MAX_FRIENDS][48];
    int n = 0;
    if (s->invisible != g_sent_invisible && g_sent) {                      /* mudou "Aparecer offline": avisa já (bye ao ligar, hi ao desligar) */
        for (int i = 0; i < s->friend_count; i++) if (!friends_blocked(s, s->friends[i].id)) snprintf(ids[n++], 48, "%s", s->friends[i].id);
        send_hello(s->invisible ? "bye" : "hi", false, ids, n);
        g_sent_invisible = s->invisible; if (!s->invisible) g_sent = now;
        return;
    }
    if (s->invisible) { g_sent_invisible = true; return; }
    if (!g_sent || now - g_sent >= PPRES_REFRESH_MS) {                    /* entrei (ou passou o tempo): me anuncio a todos os amigos */
        for (int i = 0; i < s->friend_count; i++) if (!friends_blocked(s, s->friends[i].id)) snprintf(ids[n++], 48, "%s", s->friends[i].id);
        send_hello("hi", false, ids, n); g_sent = now; g_sent_invisible = false; return;
    }
    for (int i = 0; i < MAX_FRIENDS; i++) if (g_pp[i].reply_due && g_pp[i].id[0]) {   /* quem acabou de entrar recebe a minha resposta */
        g_pp[i].reply_due = false;
        if (!ppres_may_reply(g_pp[i].replied, now)) continue;
        g_pp[i].replied = now; snprintf(ids[n++], 48, "%s", g_pp[i].id);
    }
    send_hello("hi", true, ids, n);
}
