#include "safety.h"
#include "json.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <windows.h>

static bool g_revealed[BRIKS_MAX_PEERS], g_hidden[BRIKS_MAX_PEERS];
static char g_rev_id[BRIKS_MAX_PEERS][48];   /* quem o slot era quando foi revelado: outro usuário no mesmo slot volta a ser coberto */

bool safety_guard_on(void) { return settings_sens_guard(); }
void safety_guard_set(bool on) { settings_set_sens_guard(on); }

bool safety_is_friend(const app_state_t *s, int slot) {
    if (slot < 0 || slot >= BRIKS_MAX_PEERS || !s->rtc_peers[slot].id[0]) return false;
    const char *id = s->rtc_peers[slot].id, *fp = s->rtc_peers[slot].fp;
    for (int i = 0; i < s->friend_count; i++) {
        if (s->friends[i].id[0] && !strcmp(s->friends[i].id, id)) return true;
        if (fp[0] && s->friends[i].fp[0] && !strcmp(s->friends[i].fp, fp)) return true;
    }
    return false;
}

static void sync_slot(const app_state_t *s, int slot) {
    if (strcmp(g_rev_id[slot], s->rtc_peers[slot].id)) { snprintf(g_rev_id[slot], sizeof g_rev_id[slot], "%s", s->rtc_peers[slot].id); g_revealed[slot] = false; g_hidden[slot] = false; }
}
bool safety_hidden(const app_state_t *s, int slot) { if (slot < 0 || slot >= BRIKS_MAX_PEERS) return false; sync_slot(s, slot); return g_hidden[slot]; }
bool safety_covered(const app_state_t *s, int slot) {
    if (slot < 0 || slot >= BRIKS_MAX_PEERS || !s->rtc_peers[slot].id[0]) return false;
    sync_slot(s, slot);
    if (g_hidden[slot]) return true;
    return safety_guard_on() && !g_revealed[slot] && !safety_is_friend(s, slot);
}
void safety_reveal(app_state_t *s, int slot) { if (slot < 0 || slot >= BRIKS_MAX_PEERS) return; sync_slot(s, slot); g_revealed[slot] = true; g_hidden[slot] = false; }
void safety_hide(app_state_t *s, int slot, bool hide) { if (slot < 0 || slot >= BRIKS_MAX_PEERS) return; sync_slot(s, slot); g_hidden[slot] = hide; if (hide) g_revealed[slot] = false; }
void safety_slot_reset(int slot) { if (slot < 0 || slot >= BRIKS_MAX_PEERS) return; g_revealed[slot] = g_hidden[slot] = false; g_rev_id[slot][0] = 0; }

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
