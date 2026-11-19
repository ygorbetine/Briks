#include "safety.h"
#include "json.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <windows.h>

static bool g_revealed[BRIKS_MAX_PEERS], g_hidden[BRIKS_MAX_PEERS], g_checked[BRIKS_MAX_PEERS];
static char g_rev_id[BRIKS_MAX_PEERS][48];   /* quem o slot era quando foi revelado: outro usuário no mesmo slot volta a ser coberto */


static int friend_of(const app_state_t *s, int slot) {
    if (slot < 0 || slot >= BRIKS_MAX_PEERS || !s->rtc_peers[slot].id[0]) return -1;
    const char *id = s->rtc_peers[slot].id, *fp = s->rtc_peers[slot].fp;
    for (int i = 0; i < s->friend_count; i++) {
        if (s->friends[i].id[0] && !strcmp(s->friends[i].id, id)) return i;
        if (fp[0] && s->friends[i].fp[0] && !strcmp(s->friends[i].fp, fp)) return i;
    }
    return -1;
}
bool safety_is_friend(const app_state_t *s, int slot) { return friend_of(s, slot) >= 0; }

/* ---- QoL-5: "Parar de ver" lembrado por amigo (ocultos.txt: uma identidade por linha, a impressão digital "fp:..." ou o id da conta "id:..."). Só amigos:
 * quem não é amigo nunca entra no arquivo, então o Parar de ver de um desconhecido vale só até ele sair. ---- */
#define HID_MAX 256
static void hid_path(char *out, size_t cap) {
    const char *e = getenv("BRIKS_HIDDEN_FILE");
    if (e && e[0]) { snprintf(out, cap, "%s", e); return; }
    const char *d = getenv("BRIKS_CUI_DATA"); char ad[MAX_PATH];
    if (d && d[0]) snprintf(out, cap, "%s\\ocultos.txt", d);
    else { const char *a = getenv("APPDATA"); snprintf(ad, sizeof ad, "%s", a ? a : "."); snprintf(out, cap, "%s\\Briks-CUI\\ocultos.txt", ad); }
}
static int hid_read(char lines[][100]) {
    char p[MAX_PATH]; hid_path(p, sizeof p); FILE *f = fopen(p, "rb"); if (!f) return 0;
    int n = 0; char b[160];
    while (n < HID_MAX && fgets(b, sizeof b, f)) { size_t L = strlen(b); while (L && (b[L - 1] == '\n' || b[L - 1] == '\r' || b[L - 1] == ' ')) b[--L] = 0; if (L && L < 100) { memcpy(lines[n], b, L + 1); n++; } }
    fclose(f); return n;
}
static void make_keys(const app_state_t *s, int fi, char *kfp, char *kid, size_t cap) {
    kfp[0] = kid[0] = 0;
    if (s->friends[fi].fp[0] && strlen(s->friends[fi].fp) < cap - 4) snprintf(kfp, cap, "fp:%s", s->friends[fi].fp);
    if (s->friends[fi].id[0] && strlen(s->friends[fi].id) < cap - 4) snprintf(kid, cap, "id:%s", s->friends[fi].id);
}
static bool hid_has(const app_state_t *s, int fi) {
    static char L[HID_MAX][100]; char kfp[100], kid[100]; make_keys(s, fi, kfp, kid, sizeof kfp);
    int n = hid_read(L);
    for (int i = 0; i < n; i++) if ((kfp[0] && !strcmp(L[i], kfp)) || (kid[0] && !strcmp(L[i], kid))) return true;
    return false;
}
static void hid_set(const app_state_t *s, int fi, bool on) {
    static char L[HID_MAX][100]; char kfp[100], kid[100]; make_keys(s, fi, kfp, kid, sizeof kfp);
    int n = hid_read(L), m = 0;
    for (int i = 0; i < n; i++) if (!((kfp[0] && !strcmp(L[i], kfp)) || (kid[0] && !strcmp(L[i], kid)))) { if (m != i) memcpy(L[m], L[i], sizeof L[m]); m++; }
    if (on && m < HID_MAX) { const char *k = kfp[0] ? kfp : kid; if (k[0]) { snprintf(L[m], sizeof L[m], "%s", k); m++; } }
    char p[MAX_PATH]; hid_path(p, sizeof p); FILE *f = fopen(p, "wb"); if (!f) return;
    for (int i = 0; i < m; i++) fprintf(f, "%s\r\n", L[i]);
    fclose(f);
}

static void sync_slot(const app_state_t *s, int slot) {
    if (strcmp(g_rev_id[slot], s->rtc_peers[slot].id)) { snprintf(g_rev_id[slot], sizeof g_rev_id[slot], "%s", s->rtc_peers[slot].id); g_revealed[slot] = false; g_hidden[slot] = false; g_checked[slot] = false; }
    if (!g_checked[slot]) {   /* amigo que parei de ver em outra chamada: volta escondido (a impressão digital pode chegar depois do id, então tenta de novo até reconhecer) */
        int fi = friend_of(s, slot);
        if (fi >= 0) { g_checked[slot] = true; if (!g_revealed[slot] && hid_has(s, fi)) g_hidden[slot] = true; }
    }
}
bool safety_hidden(const app_state_t *s, int slot) { if (slot < 0 || slot >= BRIKS_MAX_PEERS) return false; sync_slot(s, slot); return g_hidden[slot]; }
bool safety_covered(const app_state_t *s, int slot) {
    if (slot < 0 || slot >= BRIKS_MAX_PEERS || !s->rtc_peers[slot].id[0]) return false;
    sync_slot(s, slot);
    return g_hidden[slot];   /* só cobre quando a própria pessoa escolheu "Parar de ver" (sem cobertura automática por não ser amigo) */
}
void safety_reveal(app_state_t *s, int slot) {
    if (slot < 0 || slot >= BRIKS_MAX_PEERS) return; sync_slot(s, slot); g_revealed[slot] = true; g_hidden[slot] = false;
    { int fi = friend_of(s, slot); if (fi >= 0) hid_set(s, fi, false); }
}
void safety_hide(app_state_t *s, int slot, bool hide) {
    if (slot < 0 || slot >= BRIKS_MAX_PEERS) return; sync_slot(s, slot); g_hidden[slot] = hide; if (hide) g_revealed[slot] = false;
    { int fi = friend_of(s, slot); if (fi >= 0) hid_set(s, fi, hide); }   /* só amigo é lembrado entre chamadas */
}
void safety_slot_reset(int slot) { if (slot < 0 || slot >= BRIKS_MAX_PEERS) return; g_revealed[slot] = g_hidden[slot] = g_checked[slot] = false; g_rev_id[slot][0] = 0; }

/* ---- denúncia: registro local (denuncias.jsonl em %APPDATA%\Briks-CUI) com a pessoa, o motivo e a hora ---- */
static const char *const REASONS[] = { "Conteúdo violento ou agressivo", "Conteúdo sexual", "Ódio, assédio ou ameaça", "Conteúdo ilegal", "Outro motivo" };
static const char *const REASON_KEY[] = { "violencia", "sexual", "odio", "ilegal", "outro" };
#define NREASONS 5

bool safety_report_write(const char *dir, const char *who_id, const char *who_name, const char *reason, bool blocked) {
    char path[MAX_PATH]; snprintf(path, sizeof path, "%s\\denuncias.jsonl", dir);
    FILE *f = fopen(path, "ab"); if (!f) return false;
    char id[96], nm[200], rs[64]; json_escape(who_id ? who_id : "", id, sizeof id); json_escape(who_name ? who_name : "", nm, sizeof nm); json_escape(reason ? reason : "", rs, sizeof rs);
    fprintf(f, "{\"t\":%lld,\"id\":\"%s\",\"nome\":\"%s\",\"motivo\":\"%s\",\"bloqueado\":%s}\n", (long long)time(NULL), id, nm, rs, blocked ? "true" : "false");
    fclose(f); return true;
}

const char *safety_reason_label(int i) { return (i >= 0 && i < NREASONS) ? REASONS[i] : ""; }
const char *safety_reason_key(int i) { return (i >= 0 && i < NREASONS) ? REASON_KEY[i] : ""; }
int safety_reason_count(void) { return NREASONS; }
