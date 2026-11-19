/* Anotações sobre as transmissões e Board da sala (lado da UI). Veja annot.h e docs/plans/anotacoes-board.md.
 * Modelo: por escopo ("s:<fp>:<slot>", "b:<n>" na rede; "L:<escopo>" = só eu, nunca sai do aparelho) uma lista de operações ordenada por
 * (relógio lógico, autor, id). O core valida tudo o que chega e só repassa o que é válido (eventos annot.*); o que eu desenho vale na hora aqui
 * e vai ao core em lotes. Desfazer/refazer: pilhas por escopo de "ações" (operações criadas + cópias das removidas), simétricas. */
#include "annot.h"
#include "core.h"
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NSC 48
#define MAX_OPS 4000
#define NUNDO 48
#define BATCH_OPS 100
#define LASER_MS 1000
#define MAX_PERMS 16

typedef struct { char a[72]; unsigned i; } akey_t;
typedef struct { akey_t *add; int nadd; aop_t *rem; int nrem; } act_t;
typedef struct { char name[184]; aop_t *ops; int n, cap; act_t undo[NUNDO], redo[NUNDO]; int nundo, nredo; bool used; } ascope_t;
typedef struct { char owner[72]; bool all; int n; char fps[32][72]; bool used; } aperm_t;
typedef struct { char scope[184]; char a[72]; float x, y; unsigned col; unsigned long long t; } alaser_t;

static ascope_t g_sc[NSC];
static aperm_t g_perm[MAX_PERMS];
static alaser_t g_laser[128]; static int g_nlaser;
static unsigned long long g_clock; static unsigned g_next_id;
static char g_note[96]; static unsigned long long g_note_t;
static char g_me[72];
static app_state_t *g_app;   /* último estado visto (os roteiros de teste não passam o estado) */

/* ferramentas e estilo (globais: valem para qualquer superfície) */
enum { T_NONE, T_PEN, T_HI, T_LINE, T_ARROW, T_RECT, T_ELL, T_TEXT, T_ERASE, T_ERASE_AREA, T_LASER, T_COUNT };
static int g_tool = T_NONE, g_width = 1;       /* g_width: 0 fina 1 média 2 grossa */
static unsigned g_color = 0xe53935;
static bool g_fill = false, g_share = true;    /* g_share: modo "Para todos" (se permitido) */
static char g_active[176];                     /* escopo da transmissão sendo anotada ("" = nenhuma; o Board usa annot_tool_active) */
static const int WIDTHS[3] = { 18, 40, 90 };

static unsigned long long now_ms(void) { return GetTickCount64(); }
static unsigned long long wall_ms(void) { FILETIME ft; GetSystemTimeAsFileTime(&ft); unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime; return t / 10000ULL - 11644473600000ULL; }
static void note(const char *t) { snprintf(g_note, sizeof g_note, "%s", t); g_note_t = now_ms(); }
const char *annot_note(void) { return (g_note[0] && now_ms() - g_note_t < 3500) ? g_note : ""; }
bool annot_shared_mode(void) { return g_share; }
cui_color annot_cur_color(void) { return CUI_RGB(g_color); }
bool annot_tool_active(void) { return g_tool != T_NONE; }

static unsigned next_id(void) {
    unsigned base = (unsigned)((unsigned long long)(time(NULL) - 1767225600LL) * 10ULL);   /* 10 por segundo desde 2026-01-01: uma sessão nova não repete ids da anterior */
    if (base > g_next_id) g_next_id = base; else g_next_id++;
    return g_next_id;
}
static unsigned long long next_clock(void) { unsigned long long w = wall_ms(); if (w > g_clock) g_clock = w; else g_clock++; return g_clock; }

/* ------------------------------------------------------------------ modelo */

static void op_free(aop_t *o) { free(o->p); free(o->sm); o->p = NULL; o->sm = NULL; o->np = o->nsm = 0; }
static aop_t op_clone(const aop_t *o) {
    aop_t c = *o; c.sm = NULL; c.nsm = 0;
    if (o->p && o->np > 0) { c.p = malloc((size_t)o->np * 2 * sizeof(int)); if (c.p) memcpy(c.p, o->p, (size_t)o->np * 2 * sizeof(int)); else c.np = 0; } else c.p = NULL;
    return c;
}
static int op_cmp(const aop_t *x, const aop_t *y) {
    if (x->c != y->c) return x->c < y->c ? -1 : 1;
    int r = strcmp(x->a, y->a); if (r) return r < 0 ? -1 : 1;
    return x->i == y->i ? 0 : x->i < y->i ? -1 : 1;
}
static ascope_t *sc_get(const char *name, bool create) {
    for (int i = 0; i < NSC; i++) if (g_sc[i].used && !strcmp(g_sc[i].name, name)) return &g_sc[i];
    if (!create) return NULL;
    for (int i = 0; i < NSC; i++) if (!g_sc[i].used) { memset(&g_sc[i], 0, sizeof g_sc[i]); g_sc[i].used = true; snprintf(g_sc[i].name, sizeof g_sc[i].name, "%s", name); return &g_sc[i]; }
    return NULL;
}
static int sc_find(const ascope_t *sc, const char *a, unsigned i) {
    for (int k = 0; k < sc->n; k++) if (sc->ops[k].i == i && !strcmp(sc->ops[k].a, a)) return k;
    return -1;
}
static void act_free(act_t *a) { free(a->add); for (int i = 0; i < a->nrem; i++) op_free(&a->rem[i]); free(a->rem); memset(a, 0, sizeof *a); }
static void stack_clear(act_t *st, int *n) { for (int i = 0; i < *n; i++) act_free(&st[i]); *n = 0; }
static void sc_free(ascope_t *sc) {
    for (int i = 0; i < sc->n; i++) op_free(&sc->ops[i]);
    free(sc->ops); stack_clear(sc->undo, &sc->nundo); stack_clear(sc->redo, &sc->nredo); memset(sc, 0, sizeof *sc);
}
static void stack_push(act_t *st, int *n, act_t a) {
    if (*n >= NUNDO) { act_free(&st[0]); memmove(&st[0], &st[1], sizeof(act_t) * (NUNDO - 1)); (*n)--; }
    st[(*n)++] = a;
}
/* insere (toma posse de o->p); false = duplicada ou escopo cheio (libera) */
static bool sc_insert(ascope_t *sc, aop_t *o) {
    if (sc_find(sc, o->a, o->i) >= 0 || sc->n >= MAX_OPS) { op_free(o); return false; }
    if (sc->n == sc->cap) { int nc = sc->cap ? sc->cap * 2 : 64; aop_t *nn = realloc(sc->ops, (size_t)nc * sizeof *nn); if (!nn) { op_free(o); return false; } sc->ops = nn; sc->cap = nc; }
    int k = sc->n; while (k > 0 && op_cmp(&sc->ops[k - 1], o) > 0) { sc->ops[k] = sc->ops[k - 1]; k--; }
    sc->ops[k] = *o; sc->n++; return true;
}
static void sc_remove(ascope_t *sc, int k) { op_free(&sc->ops[k]); memmove(&sc->ops[k], &sc->ops[k + 1], (size_t)(sc->n - k - 1) * sizeof(aop_t)); sc->n--; }

/* aplica uma operação já validada (do core ou minha) ao modelo. 'p' acrescenta pontos (o->i = id do traço-alvo em o->x0), 'd' apaga (alvo: autor em o->s, id em o->x0), 'x' limpa. Consome o->p. */
static void model_apply(ascope_t *sc, aop_t *o, bool privileged) {
    if (o->c > g_clock) g_clock = o->c;
    switch (o->k) {
    case 's': case 'g': case 't': sc_insert(sc, o); return;
    case 'p': {
        int k = sc_find(sc, o->a, (unsigned)o->x0);
        if (k >= 0 && sc->ops[k].k == 's' && sc->ops[k].np + o->np <= AN_MAX_PTS && o->np > 0) {
            aop_t *t = &sc->ops[k]; int *np = realloc(t->p, (size_t)(t->np + o->np) * 2 * sizeof(int));
            if (np) { memcpy(np + (size_t)t->np * 2, o->p, (size_t)o->np * 2 * sizeof(int)); t->p = np; t->np += o->np; free(t->sm); t->sm = NULL; t->nsm = 0; }
        }
        op_free(o); return;
    }
    case 'd': {
        int k = sc_find(sc, o->s, (unsigned)o->x0);
        if (k >= 0 && (privileged || !strcmp(sc->ops[k].a, o->a))) sc_remove(sc, k);
        op_free(o); return;
    }
    case 'x': {
        const bool all = o->m != 0;
        for (int k = sc->n - 1; k >= 0; k--) {
            const aop_t *t = &sc->ops[k];
            if (all ? (privileged && op_cmp(t, o) < 0) : (!strcmp(t->a, o->a) && t->c < o->c)) sc_remove(sc, k);
        }
        op_free(o); return;
    }
    }
    op_free(o);
}

/* ------------------------------------------------------------------ permissões */

static aperm_t *perm_get(const char *owner, bool create) {
    for (int i = 0; i < MAX_PERMS; i++) if (g_perm[i].used && !strcmp(g_perm[i].owner, owner)) return &g_perm[i];
    if (!create) return NULL;
    for (int i = 0; i < MAX_PERMS; i++) if (!g_perm[i].used) { memset(&g_perm[i], 0, sizeof g_perm[i]); g_perm[i].used = true; snprintf(g_perm[i].owner, sizeof g_perm[i].owner, "%s", owner); return &g_perm[i]; }
    return NULL;
}
static bool can_share(const app_state_t *s, const actx_t *cx) {
    if (!s->in_room) return false;
    if (cx->board || cx->own) return true;
    const aperm_t *p = cx->owner_fp[0] ? perm_get(cx->owner_fp, false) : NULL;
    if (!p) return false;
    if (p->all) return true;
    for (int i = 0; i < p->n; i++) if (!strcmp(p->fps[i], g_me)) return true;
    return false;
}
static bool privileged(const actx_t *cx, bool shared) { return !shared || cx->board || cx->own; }

static void make_loc(const char *base, char *out, size_t cap) { snprintf(out, cap, "L:%s", base); }
static bool eff_share(app_state_t *s, const actx_t *cx) { return g_share && can_share(s, cx); }

/* ------------------------------------------------------------------ rede */

static char g_nb[48000]; static size_t g_nb_len; static int g_nb_ops; static char g_nb_scope[184];

static int op_json(const aop_t *o, char *b, size_t cap, int p0, int p1) {   /* uma operação como JSON; para 'p': p0..p1 pontos novos */
    int n = 0;
    switch (o->k) {
    case 's': case 'p': {
        n = snprintf(b, cap, "{\"i\":%u,\"c\":%llu,\"k\":\"%c\",", o->i, o->c, o->k);
        if (o->k == 's') n += snprintf(b + n, cap - (size_t)n, "\"m\":%d,\"col\":%d,\"w\":%d,", o->m, o->col, o->w); else n += snprintf(b + n, cap - (size_t)n, "\"tg\":%d,", o->x0);
        n += snprintf(b + n, cap - (size_t)n, "\"p\":[");
        for (int k = p0; k < p1 && (size_t)n + 24 < cap; k++) n += snprintf(b + n, cap - (size_t)n, "%s%d,%d", k > p0 ? "," : "", o->p[k * 2], o->p[k * 2 + 1]);
        n += snprintf(b + n, cap - (size_t)n, "]}"); break;
    }
    case 'g': n = snprintf(b, cap, "{\"i\":%u,\"c\":%llu,\"k\":\"g\",\"sh\":\"%c\",\"col\":%d,\"w\":%d,\"f\":%d,\"x0\":%d,\"y0\":%d,\"x1\":%d,\"y1\":%d}", o->i, o->c, o->sh, o->col, o->w, o->f, o->x0, o->y0, o->x1, o->y1); break;
    case 't': { char e[300]; json_escape(o->s, e, sizeof e); n = snprintf(b, cap, "{\"i\":%u,\"c\":%llu,\"k\":\"t\",\"s\":\"%s\",\"col\":%d,\"sz\":%d,\"x\":%d,\"y\":%d}", o->i, o->c, e, o->col, o->sz, o->x0, o->y0); break; }
    case 'd': n = snprintf(b, cap, "{\"i\":%u,\"c\":%llu,\"k\":\"d\",\"tg\":{\"a\":\"%s\",\"i\":%d}}", o->i, o->c, o->s, o->x0); break;
    case 'x': n = snprintf(b, cap, "{\"i\":%u,\"c\":%llu,\"k\":\"x\",\"all\":%s}", o->i, o->c, o->m ? "true" : "false"); break;
    }
    return n;
}
static void nb_flush(void) {
    if (!g_nb_ops) return;
    static char line[sizeof g_nb + 400];
    snprintf(line, sizeof line, "{\"cmd\":\"annot.send\",\"kind\":\"ops\",\"scope\":\"%s\",\"ops\":[%s]}", g_nb_scope, g_nb);
    core_send(line);
    g_nb_len = 0; g_nb[0] = 0; g_nb_ops = 0;
}
static void nb_add(const char *scope, const aop_t *o, int p0, int p1) {
    if (g_nb_ops && strcmp(scope, g_nb_scope)) nb_flush();
    char one[sizeof g_nb / 2];
    int n = op_json(o, one, sizeof one, p0, p1); if (n <= 0 || (size_t)n >= sizeof one) return;
    if (g_nb_ops >= BATCH_OPS || g_nb_len + (size_t)n + 2 >= sizeof g_nb) nb_flush();
    snprintf(g_nb_scope, sizeof g_nb_scope, "%s", scope);
    g_nb_len += (size_t)snprintf(g_nb + g_nb_len, sizeof g_nb - g_nb_len, "%s%s", g_nb_ops ? "," : "", one); g_nb_ops++;
}
/* traço em andamento: manda o que ainda não foi (s com os primeiros pontos, depois p em pedaços de 160) */
static void send_stroke_progress(const char *scope, aop_t *o) {
    while (o->sent < o->np) {
        int to = o->np - o->sent > 160 ? o->sent + 160 : o->np;
        if (o->sent == 0) { nb_add(scope, o, 0, to); }
        else { aop_t p = *o; p.k = 'p'; p.x0 = (int)o->i; p.i = next_id(); p.c = next_clock(); nb_add(scope, &p, o->sent, to); }
        o->sent = to;
    }
}
static void send_perm(app_state_t *s, bool all) {
    char line[160]; snprintf(line, sizeof line, "{\"cmd\":\"annot.send\",\"kind\":\"perm\",\"all\":%s,\"fps\":[]}", all ? "true" : "false"); core_send(line);
    aperm_t *p = perm_get(g_me, true); if (p) { p->all = all; p->n = 0; }
    (void)s;
}
static void send_laser(const char *scope, int x, int y) { char line[300]; snprintf(line, sizeof line, "{\"cmd\":\"annot.send\",\"kind\":\"laser\",\"scope\":\"%s\",\"x\":%d,\"y\":%d,\"col\":%u}", scope, x, y, g_color); core_send(line); }

/* ------------------------------------------------------------------ eventos do core */

static double jnum(const jval *o, const char *k, double d) { const jval *v = json_get(o, k); return v && v->type == J_NUM ? v->num : d; }
static bool parse_op(const jval *o, const char *author, aop_t *out) {
    memset(out, 0, sizeof *out);
    const char *k = json_str(o, "k", ""); if (!k[0]) return false;
    snprintf(out->a, sizeof out->a, "%s", json_str(o, "a", author)); out->i = (unsigned)jnum(o, "i", 0); out->c = (unsigned long long)jnum(o, "c", 0); out->k = k[0];
    switch (k[0]) {
    case 's': case 'p': {
        const jval *p = json_get(o, "p"); if (!p || p->type != J_ARR) return false;
        int n = json_len(p) / 2; if (n < 1 || n > AN_MAX_PTS) return false;
        out->p = malloc((size_t)n * 2 * sizeof(int)); if (!out->p) return false;
        out->np = n; for (int q = 0; q < n * 2; q++) { const jval *v = json_at(p, q); int x = v && v->type == J_NUM ? (int)v->num : 0; out->p[q] = x < 0 ? 0 : x > AN_COORD ? AN_COORD : x; }
        out->m = (int)jnum(o, "m", 0); out->col = (int)jnum(o, "col", 0); out->w = (int)jnum(o, "w", 30); out->x0 = (int)jnum(o, "tg", 0); out->sent = n; break;
    }
    case 'g': { const char *sh = json_str(o, "sh", "l"); out->sh = sh[0]; out->col = (int)jnum(o, "col", 0); out->w = (int)jnum(o, "w", 30); out->f = (int)jnum(o, "f", 0); out->x0 = (int)jnum(o, "x0", 0); out->y0 = (int)jnum(o, "y0", 0); out->x1 = (int)jnum(o, "x1", 0); out->y1 = (int)jnum(o, "y1", 0); break; }
    case 't': snprintf(out->s, sizeof out->s, "%s", json_str(o, "s", "")); out->col = (int)jnum(o, "col", 0); out->sz = (int)jnum(o, "sz", 200); out->x0 = (int)jnum(o, "x", 0); out->y0 = (int)jnum(o, "y", 0); break;
    case 'd': { const jval *tg = json_get(o, "tg"); if (!tg) return false; snprintf(out->s, sizeof out->s, "%s", json_str(tg, "a", "")); out->x0 = (int)jnum(tg, "i", 0); break; }
    case 'x': { const jval *a = json_get(o, "all"); out->m = a && a->type == J_BOOL && a->b; break; }
    default: return false;
    }
    return true;
}
static unsigned long long laser_cut(void) { return now_ms(); }

void annot_core_event(app_state_t *s, const jval *j, const char *ev) {
    g_app = s;
    if (!strcmp(ev, "annot.ops")) {
        const char *scope = json_str(j, "scope", ""); const jval *ops = json_get(j, "ops"); if (!scope[0] || !ops || ops->type != J_ARR) return;
        ascope_t *sc = sc_get(scope, true); if (!sc) return;
        const char *owner = ""; char ob[72] = ""; if (!strncmp(scope, "s:", 2)) { const char *e = strrchr(scope, ':'); size_t n = e && e > scope + 2 ? (size_t)(e - scope - 2) : 0; if (n && n < sizeof ob) { memcpy(ob, scope + 2, n); ob[n] = 0; owner = ob; } }
        for (int q = 0; q < json_len(ops); q++) {
            aop_t o; if (!parse_op(json_at(ops, q), json_str(j, "from_fp", ""), &o)) continue;
            const bool priv = scope[0] == 'b' || !strcmp(o.a, owner);
            model_apply(sc, &o, priv);
        }
    } else if (!strcmp(ev, "annot.laser")) {
        alaser_t l; memset(&l, 0, sizeof l);
        snprintf(l.scope, sizeof l.scope, "%s", json_str(j, "scope", "")); snprintf(l.a, sizeof l.a, "%s", json_str(j, "a", "")); l.x = (float)jnum(j, "x", 0) / AN_COORD; l.y = (float)jnum(j, "y", 0) / AN_COORD; l.col = (unsigned)jnum(j, "col", 0xff0000); l.t = laser_cut();
        if (g_nlaser >= 128) { memmove(&g_laser[0], &g_laser[1], sizeof(alaser_t) * 127); g_nlaser = 127; }
        g_laser[g_nlaser++] = l;
    } else if (!strcmp(ev, "annot.perm")) {
        aperm_t *p = perm_get(json_str(j, "owner", ""), true); if (!p) return;
        const jval *a = json_get(j, "all"); p->all = a && a->type == J_BOOL && a->b; p->n = 0;
        const jval *f = json_get(j, "fps"); for (int q = 0; f && f->type == J_ARR && q < json_len(f) && p->n < 32; q++) { const jval *v = json_at(f, q); if (v && v->type == J_STR) snprintf(p->fps[p->n++], 72, "%s", v->str); }
    } else if (!strcmp(ev, "annot.denied")) note("Sem permissão para desenhar para todos aqui.");
    else if (!strcmp(ev, "annot.full")) note("Limite de desenhos atingido (apague ou limpe algo).");
}

void annot_peer_up(app_state_t *s) {
    if (!s->in_room) return;
    snprintf(g_me, sizeof g_me, "%s", s->rtc_fp);
    core_send("{\"cmd\":\"annot.send\",\"kind\":\"sync\"}");
    aperm_t *p = perm_get(g_me, false); if (p && p->all) send_perm(s, true);   /* quem entrou também precisa saber o que liberei (o core responde ao want, isto é só reforço) */
}
static void drop_scope_local(const char *name) { ascope_t *sc = sc_get(name, false); if (sc) sc_free(sc); char l[190]; make_loc(name, l, sizeof l); sc = sc_get(l, false); if (sc) sc_free(sc); }
static void stream_gone_fp(const char *fp, int slot) {
    if (!fp[0]) return;
    char nm[190]; snprintf(nm, sizeof nm, "s:%s:%d", fp, slot); drop_scope_local(nm);
    char line[240]; snprintf(line, sizeof line, "{\"cmd\":\"annot.send\",\"kind\":\"drop\",\"scope\":\"%s\"}", nm); core_send(line);
    if (!strcmp(g_active, nm)) g_active[0] = 0;
}
void annot_stream_gone(app_state_t *s, int peer, int slot) { if (peer >= 0 && peer < BRIKS_MAX_PEERS) stream_gone_fp(s->rtc_peers[peer].fp, slot); }
void annot_stream_gone_own(app_state_t *s, int slot) { snprintf(g_me, sizeof g_me, "%s", s->rtc_fp); stream_gone_fp(g_me, slot); aperm_t *p = perm_get(g_me, false); if (p && slot == 0) p->all = false; }
void annot_room_left(void) {
    for (int i = 0; i < NSC; i++) if (g_sc[i].used) sc_free(&g_sc[i]);
    memset(g_perm, 0, sizeof g_perm); g_nlaser = 0; g_active[0] = 0; g_nb_ops = 0; g_nb_len = 0; g_tool = T_NONE;
    board_room_left();
}

int annot_scope_count(const char *base, bool local) { char l[190]; const char *n = base; if (local) { make_loc(base, l, sizeof l); n = l; } const ascope_t *sc = sc_get(n, false); return sc ? sc->n : 0; }
const aop_t *annot_scope_op(const char *base, bool local, int idx) { char l[190]; const char *n = base; if (local) { make_loc(base, l, sizeof l); n = l; } const ascope_t *sc = sc_get(n, false); return sc && idx >= 0 && idx < sc->n ? &sc->ops[idx] : NULL; }

/* ------------------------------------------------------------------ geometria e desenho */

static void rdp(const int *p, int a, int b, float eps2, unsigned char *keep) {   /* Ramer-Douglas-Peucker (iterativo, sem recursão profunda) */
    int stack[128]; int sp = 0; stack[sp++] = a; stack[sp++] = b;
    while (sp >= 2) {
        int hi = stack[--sp], lo = stack[--sp]; if (hi <= lo + 1) continue;
        const float x0 = (float)p[lo * 2], y0 = (float)p[lo * 2 + 1], dx = (float)p[hi * 2] - x0, dy = (float)p[hi * 2 + 1] - y0; const float L2 = dx * dx + dy * dy;
        float best = -1; int bi = -1;
        for (int k = lo + 1; k < hi; k++) {
            const float px = (float)p[k * 2] - x0, py = (float)p[k * 2 + 1] - y0; float d2;
            if (L2 < 1e-6f) d2 = px * px + py * py; else { float t = (px * dx + py * dy) / L2; t = t < 0 ? 0 : t > 1 ? 1 : t; const float qx = px - t * dx, qy = py - t * dy; d2 = qx * qx + qy * qy; }
            if (d2 > best) { best = d2; bi = k; }
        }
        if (best > eps2 && bi > 0 && sp + 4 <= 128) { keep[bi] = 1; stack[sp++] = lo; stack[sp++] = bi; stack[sp++] = bi; stack[sp++] = hi; }
    }
}
static void op_simplify(aop_t *o, float eps) {
    if (o->np < 4) return;
    unsigned char *keep = calloc((size_t)o->np, 1); if (!keep) return;
    keep[0] = keep[o->np - 1] = 1; rdp(o->p, 0, o->np - 1, eps * eps, keep);
    int m = 0; for (int k = 0; k < o->np; k++) if (keep[k]) { o->p[m * 2] = o->p[k * 2]; o->p[m * 2 + 1] = o->p[k * 2 + 1]; m++; }
    o->np = m; free(o->sm); o->sm = NULL; o->nsm = 0; free(keep);
}
/* Catmull-Rom → polilinha (cache em coordenadas normalizadas; refeito só quando o traço muda) */
static void build_sm(aop_t *o) {
    const int SUB = 5; int cap = (o->np < 3 ? o->np : (o->np - 1) * SUB + 1) * 2;
    float *sm = malloc((size_t)cap * sizeof(float)); if (!sm) return;
    int n = 0;
    if (o->np < 3) { for (int k = 0; k < o->np; k++) { sm[n * 2] = o->p[k * 2] / (float)AN_COORD; sm[n * 2 + 1] = o->p[k * 2 + 1] / (float)AN_COORD; n++; } }
    else for (int k = 0; k < o->np - 1; k++) {
        const int i0 = k > 0 ? k - 1 : 0, i3 = k + 2 < o->np ? k + 2 : o->np - 1;
        for (int u = 0; u < SUB; u++) {
            const float t = (float)u / SUB, t2 = t * t, t3 = t2 * t;
            for (int c = 0; c < 2; c++) {
                const float P0 = (float)o->p[i0 * 2 + c], P1 = (float)o->p[k * 2 + c], P2 = (float)o->p[(k + 1) * 2 + c], P3 = (float)o->p[i3 * 2 + c];
                sm[n * 2 + c] = 0.5f * ((2 * P1) + (-P0 + P2) * t + (2 * P0 - 5 * P1 + 4 * P2 - P3) * t2 + (-P0 + 3 * P1 - 3 * P2 + P3) * t3) / (float)AN_COORD;
            }
            n++;
        }
        if (k == o->np - 2) { sm[n * 2] = o->p[(k + 1) * 2] / (float)AN_COORD; sm[n * 2 + 1] = o->p[(k + 1) * 2 + 1] / (float)AN_COORD; n++; }
    }
    free(o->sm); o->sm = sm; o->nsm = n;
}

typedef struct { char base[176]; char loc[184]; asurf_t sf; bool cursor; cui_vec2 cur; float cur_r; bool dark_halo; } apaint_t;

static cui_color col_of(unsigned rgb, unsigned alpha) { return (cui_color){ (uint8_t)((rgb >> 16) & 255), (uint8_t)((rgb >> 8) & 255), (uint8_t)(rgb & 255), (uint8_t)alpha }; }
static cui_vec2 to_scr(const cui_rect *r, const asurf_t *sf, float nx, float ny) { return (cui_vec2){ r->x + sf->ox + nx * sf->iw, r->y + sf->oy + ny * sf->ih }; }

static void draw_op(cui_ctx *ui, const cui_rect *r, const asurf_t *sf, aop_t *o) {
    static cui_vec2 buf[AN_MAX_PTS * 5 + 16];
    float th = (float)o->w * sf->iw / (float)AN_COORD; if (th < 1.0f) th = 1.0f;
    switch (o->k) {
    case 's': {
        const bool hi = o->m == 1; if (hi) th *= 3.5f;
        const cui_color c = col_of((unsigned)o->col, hi ? 0x66 : 0xff);
        if (!o->sm) build_sm(o);
        if (!o->sm || o->nsm < 1) return;
        if (o->nsm == 1) { cui_vec2 q = to_scr(r, sf, o->sm[0], o->sm[1]); cui_draw_circle(ui, q, th * 0.5f, c); return; }
        int n = o->nsm < (int)(sizeof buf / sizeof buf[0]) ? o->nsm : (int)(sizeof buf / sizeof buf[0]);
        for (int k = 0; k < n; k++) buf[k] = to_scr(r, sf, o->sm[k * 2], o->sm[k * 2 + 1]);
        cui_draw_polyline(ui, buf, n, th, c, false);
        if (!hi && th >= 4.0f) { cui_draw_circle(ui, buf[0], th * 0.5f, c); cui_draw_circle(ui, buf[n - 1], th * 0.5f, c); }   /* pontas redondas */
        break;
    }
    case 'g': {
        const cui_color c = col_of((unsigned)o->col, 0xff), fc = col_of((unsigned)o->col, 0x59);
        const cui_vec2 a = to_scr(r, sf, o->x0 / (float)AN_COORD, o->y0 / (float)AN_COORD), b = to_scr(r, sf, o->x1 / (float)AN_COORD, o->y1 / (float)AN_COORD);
        if (o->sh == 'l' || o->sh == 'a') {
            cui_draw_line(ui, a, b, th, c);
            if (o->sh == 'a') {
                const float dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy);
                if (len > 2.0f) {
                    float hl = len * 0.3f, mx = 14.0f + th * 3.0f; if (hl > mx) hl = mx; const float ux = dx / len, uy = dy / len;
                    for (int sgn = -1; sgn <= 1; sgn += 2) { const float ang = 0.5f * (float)sgn, ca = cosf(ang), sa = sinf(ang); const cui_vec2 e = { b.x - hl * (ux * ca - uy * sa), b.y - hl * (ux * sa + uy * ca) }; cui_draw_line(ui, b, e, th, c); }
                    cui_draw_circle(ui, b, th * 0.5f, c);
                }
            }
        } else if (o->sh == 'r') {
            const cui_vec2 q[4] = { a, { b.x, a.y }, b, { a.x, b.y } };
            if (o->f) cui_draw_polygon(ui, q, 4, fc);
            cui_draw_polyline(ui, q, 4, th, c, true);
        } else if (o->sh == 'e') {
            const float cx = (a.x + b.x) * 0.5f, cy = (a.y + b.y) * 0.5f, rx = fabsf(b.x - a.x) * 0.5f, ry = fabsf(b.y - a.y) * 0.5f;
            if (rx < 0.5f && ry < 0.5f) return;
            const int N = 48; for (int k = 0; k < N; k++) { const float t = 6.2831853f * (float)k / N; buf[k] = (cui_vec2){ cx + cosf(t) * rx, cy + sinf(t) * ry }; }
            if (o->f) cui_draw_polygon(ui, buf, N, fc);
            cui_draw_polyline(ui, buf, N, th, c, true);
        }
        break;
    }
    case 't': {
        const cui_vec2 q = to_scr(r, sf, o->x0 / (float)AN_COORD, o->y0 / (float)AN_COORD);
        float px = (float)o->sz * sf->iw / (float)AN_COORD; if (px < 8.0f) px = 8.0f;
        cui_draw_text(ui, CUI_FONT_MEDIUM, px, q, col_of((unsigned)o->col, 0xff), o->s, (int)strlen(o->s));
        break;
    }
    }
}

static void paint_layer(cui_ctx *ui, const cui_rect *r, const asurf_t *sf, const char *name) {
    ascope_t *sc = sc_get(name, false); if (!sc) return;
    for (int k = 0; k < sc->n; k++) draw_op(ui, r, sf, &sc->ops[k]);
}
static void paint_canvas(cui_ctx *ui, cui_rect r, void *user) {
    const apaint_t *p = user;
    cui_push_clip(ui, r);
    paint_layer(ui, &r, &p->sf, p->base);
    paint_layer(ui, &r, &p->sf, p->loc);
    const unsigned long long t = now_ms();
    for (int k = 0; k < g_nlaser; k++) {   /* laser: cada ponto some em ~1 s */
        const alaser_t *l = &g_laser[k]; if (strcmp(l->scope, p->base)) continue;
        const float age = (float)(t - l->t) / LASER_MS; if (age >= 1.0f) continue;
        const cui_vec2 q = to_scr(&r, &p->sf, l->x, l->y); const float a = 1.0f - age;
        cui_draw_circle(ui, q, 3.0f + 7.0f * a, col_of(l->col, (unsigned)(60.0f * a)));
        cui_draw_circle(ui, q, 2.5f + 2.0f * a, col_of(l->col, (unsigned)(255.0f * a)));
    }
    if (p->cursor) { const cui_vec2 q = { r.x + p->cur.x, r.y + p->cur.y }; cui_draw_circle(ui, q, p->cur_r, CUI_RGBA(0xffffff30)); cui_draw_arc(ui, q, p->cur_r, 0.0f, 6.2831853f, 1.5f, CUI_RGBA(0xffffffd0)); }
    cui_pop_clip(ui);
}

/* ------------------------------------------------------------------ interação */

typedef struct {
    bool on; int tool; char layer[184]; bool shared; aop_t *op; unsigned op_i; char op_a[72]; unsigned long long last_flush, last_laser;
    int sx, sy; int lx, ly;   /* início e último ponto aceito (normalizados 0..10000) */
    act_t act; bool has_act;
} adrag_t;
static adrag_t g_drag;
static bool g_prev_down;
static bool g_text_open; static int g_text_x, g_text_y; static char g_text_buf[124]; static char g_text_layer[184]; static bool g_text_shared;
static int g_hit_count;

static void act_add_key(act_t *a, const char *who, unsigned i) { akey_t *n = realloc(a->add, (size_t)(a->nadd + 1) * sizeof *n); if (!n) return; a->add = n; snprintf(n[a->nadd].a, 72, "%s", who); n[a->nadd].i = i; a->nadd++; }
static void act_add_rem(act_t *a, const aop_t *o) { aop_t *n = realloc(a->rem, (size_t)(a->nrem + 1) * sizeof *n); if (!n) return; a->rem = n; n[a->nrem++] = op_clone(o); }
static void act_commit(const char *layer, act_t *a) {
    if (!a->nadd && !a->nrem) return;
    ascope_t *sc = sc_get(layer, true); if (!sc) { act_free(a); return; }
    stack_clear(sc->redo, &sc->nredo); stack_push(sc->undo, &sc->nundo, *a); memset(a, 0, sizeof *a);
}

/* cria uma operação minha na camada (local + rede se compartilhada) */
static aop_t new_op(char k) { aop_t o; memset(&o, 0, sizeof o); snprintf(o.a, sizeof o.a, "%s", g_me); o.i = next_id(); o.c = next_clock(); o.k = k; return o; }
static void layer_add(const char *layer, bool shared, aop_t *o, act_t *act) {   /* toma posse de o->p */
    ascope_t *sc = sc_get(layer, true);
    if (!sc) { op_free(o); return; }
    if (sc->n >= MAX_OPS) { note("Limite de desenhos atingido (apague ou limpe algo)."); op_free(o); return; }
    if (act) act_add_key(act, o->a, o->i);
    if (shared) { if (o->k == 's') send_stroke_progress(layer, o); else nb_add(layer, o, 0, o->np); } else o->sent = o->np;
    sc_insert(sc, o);
}
static void layer_del(const char *layer, bool shared, const aop_t *target, act_t *act) {
    ascope_t *sc = sc_get(layer, true); if (!sc) return;
    int k = sc_find(sc, target->a, target->i); if (k < 0) return;
    if (act) act_add_rem(act, &sc->ops[k]);
    aop_t d = new_op('d'); snprintf(d.s, sizeof d.s, "%s", target->a); d.x0 = (int)target->i;
    if (shared) nb_add(layer, &d, 0, 0);
    sc_remove(sc, sc_find(sc, target->a, target->i));
}

static float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
    const float dx = bx - ax, dy = by - ay, L2 = dx * dx + dy * dy; float t = L2 > 1e-6f ? ((px - ax) * dx + (py - ay) * dy) / L2 : 0; t = t < 0 ? 0 : t > 1 ? 1 : t;
    const float qx = ax + t * dx - px, qy = ay + t * dy - py; return sqrtf(qx * qx + qy * qy);
}
/* acerto do apagador: (px,py) em px relativos ao canvas; r = raio em px */
static bool op_hit(const aop_t *o, const asurf_t *sf, float px, float py, float r) {
    const float W = sf->iw, H = sf->ih, ox = sf->ox, oy = sf->oy;
    float th = (float)o->w * W / (float)AN_COORD * 0.5f; if (o->k == 's' && o->m == 1) th *= 3.5f;
    switch (o->k) {
    case 's':
        if (o->np == 1) return hypotf(px - (ox + o->p[0] * W / AN_COORD), py - (oy + o->p[1] * H / AN_COORD)) <= r + th;
        for (int k = 0; k + 1 < o->np; k++) if (seg_dist(px, py, ox + o->p[k * 2] * W / AN_COORD, oy + o->p[k * 2 + 1] * H / AN_COORD, ox + o->p[k * 2 + 2] * W / AN_COORD, oy + o->p[k * 2 + 3] * H / AN_COORD) <= r + th) return true;
        return false;
    case 'g': {
        const float ax = ox + o->x0 * W / AN_COORD, ay = oy + o->y0 * H / AN_COORD, bx = ox + o->x1 * W / AN_COORD, by = oy + o->y1 * H / AN_COORD;
        if (o->sh == 'l' || o->sh == 'a') return seg_dist(px, py, ax, ay, bx, by) <= r + th;
        const float x0 = fminf(ax, bx), x1 = fmaxf(ax, bx), y0 = fminf(ay, by), y1 = fmaxf(ay, by);
        if (o->sh == 'r') {
            if (o->f && px >= x0 && px <= x1 && py >= y0 && py <= y1) return true;
            return seg_dist(px, py, x0, y0, x1, y0) <= r + th || seg_dist(px, py, x1, y0, x1, y1) <= r + th || seg_dist(px, py, x1, y1, x0, y1) <= r + th || seg_dist(px, py, x0, y1, x0, y0) <= r + th;
        }
        { const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, rx = (x1 - x0) * 0.5f + 0.01f, ry = (y1 - y0) * 0.5f + 0.01f;
          const float nx = (px - cx) / rx, ny = (py - cy) / ry, d = sqrtf(nx * nx + ny * ny);
          if (o->f && d <= 1.0f) return true;
          return fabsf(d - 1.0f) * fminf(rx, ry) <= r + th; }
    }
    case 't': {
        const float sz = fmaxf((float)o->sz * W / AN_COORD, 8.0f), x0 = ox + o->x0 * W / AN_COORD, y0 = oy + o->y0 * H / AN_COORD, w = sz * 0.6f * (float)strlen(o->s);
        return px >= x0 - r && px <= x0 + w + r && py >= y0 - r && py <= y0 + sz + r;
    }
    }
    return false;
}
static bool may_erase(const aop_t *o, bool shared, const actx_t *cx) { return !strcmp(o->a, g_me) || privileged(cx, shared); }

/* apagador de área: tira os pontos dentro do círculo; o que sobra vira traços novos (o original é apagado) */
static bool erase_area_stroke(const char *layer, bool shared, aop_t *o, const asurf_t *sf, float px, float py, float r, act_t *act) {
    if (o->k != 's') { layer_del(layer, shared, o, act); return true; }
    bool *dead = calloc((size_t)o->np, sizeof(bool)); if (!dead) return false;
    int ndead = 0; const float R = r;
    for (int k = 0; k < o->np; k++) {
        const float x = sf->ox + o->p[k * 2] * sf->iw / AN_COORD, y = sf->oy + o->p[k * 2 + 1] * sf->ih / AN_COORD;
        bool hit = hypotf(x - px, y - py) <= R;
       
        dead[k] = hit; ndead += hit;
    }
    if (!ndead) { free(dead); return false; }
    aop_t orig = op_clone(o);   /* o ponteiro some quando o original for removido */
    layer_del(layer, shared, &orig, act);
    int k = 0;
    while (k < orig.np) {
        while (k < orig.np && dead[k]) k++;
        int a = k; while (k < orig.np && !dead[k]) k++;
        if (k > a) {
            aop_t n = new_op('s'); n.m = orig.m; n.col = orig.col; n.w = orig.w; n.np = k - a; n.p = malloc((size_t)n.np * 2 * sizeof(int));
            if (!n.p) continue;
            memcpy(n.p, orig.p + (size_t)a * 2, (size_t)n.np * 2 * sizeof(int));
            layer_add(layer, shared, &n, act);
        }
    }
    op_free(&orig); free(dead); return true;
}

static void drag_end(const actx_t *cx) {
    (void)cx;
    if (!g_drag.on) return;
    ascope_t *sc = sc_get(g_drag.layer, false);
    if (g_drag.op && sc) {
        int k = sc_find(sc, g_drag.op_a, g_drag.op_i);
        if (k >= 0) {
            aop_t *o = &sc->ops[k];
            if (o->k == 's') { if (g_drag.shared) send_stroke_progress(g_drag.layer, o); op_simplify(o, 5.0f); }
            else if (o->k == 'g') {
                const int dx = abs(o->x1 - o->x0), dy = abs(o->y1 - o->y0);
                if (dx < 20 && dy < 20) { sc_remove(sc, k); /* clique sem arrasto: descarta */ }
                else if (g_drag.shared) nb_add(g_drag.layer, o, 0, 0);
            }
        }
    }
    nb_flush();
    if (g_drag.has_act) act_commit(g_drag.layer, &g_drag.act);
    memset(&g_drag, 0, sizeof g_drag);
}

static void drag_begin(app_state_t *s, const actx_t *cx, int tool, const char *layer, bool shared, int nx, int ny) {
    memset(&g_drag, 0, sizeof g_drag);
    g_drag.on = true; g_drag.tool = tool; snprintf(g_drag.layer, sizeof g_drag.layer, "%s", layer); g_drag.shared = shared; g_drag.sx = g_drag.lx = nx; g_drag.sy = g_drag.ly = ny;
    g_drag.has_act = true; (void)s; (void)cx;
    if (tool == T_PEN || tool == T_HI) {
        aop_t o = new_op('s'); o.m = tool == T_HI ? 1 : 0; o.col = (int)g_color; o.w = WIDTHS[g_width]; o.np = 1; o.p = malloc(2 * sizeof(int)); if (!o.p) { g_drag.on = false; return; }
        o.p[0] = nx; o.p[1] = ny; snprintf(g_drag.op_a, sizeof g_drag.op_a, "%s", o.a); g_drag.op_i = o.i; g_drag.op = (aop_t *)1;
        layer_add(layer, shared, &o, &g_drag.act);
    } else if (tool == T_LINE || tool == T_ARROW || tool == T_RECT || tool == T_ELL) {
        aop_t o = new_op('g'); o.sh = tool == T_LINE ? 'l' : tool == T_ARROW ? 'a' : tool == T_RECT ? 'r' : 'e'; o.col = (int)g_color; o.w = WIDTHS[g_width]; o.f = g_fill && (tool == T_RECT || tool == T_ELL);
        o.x0 = o.x1 = nx; o.y0 = o.y1 = ny; snprintf(g_drag.op_a, sizeof g_drag.op_a, "%s", o.a); g_drag.op_i = o.i; g_drag.op = (aop_t *)1;
        ascope_t *sc = sc_get(layer, true);
        act_add_key(&g_drag.act, o.a, o.i);
        if (sc) sc_insert(sc, &o); else op_free(&o);
    }
}

/* atualiza o gesto em andamento; (mx,my) em px relativos ao canvas */
static void drag_update(app_state_t *s, const actx_t *cx, const asurf_t *sf, float mx, float my) {
    if (!g_drag.on || sf->iw < 1.0f || sf->ih < 1.0f) return;
    int nx = (int)lroundf((mx - sf->ox) / sf->iw * AN_COORD), ny = (int)lroundf((my - sf->oy) / sf->ih * AN_COORD);
    nx = nx < 0 ? 0 : nx > AN_COORD ? AN_COORD : nx; ny = ny < 0 ? 0 : ny > AN_COORD ? AN_COORD : ny;
    ascope_t *sc = sc_get(g_drag.layer, true); if (!sc) return;
    const int t = g_drag.tool;
    if (t == T_PEN || t == T_HI) {
        const float d = hypotf((float)(nx - g_drag.lx) * sf->iw / AN_COORD, (float)(ny - g_drag.ly) * sf->ih / AN_COORD);
        int k = sc_find(sc, g_drag.op_a, g_drag.op_i);
        if (k >= 0 && d >= 2.0f) {
            aop_t *o = &sc->ops[k];
            if (o->np >= AN_MAX_PTS - 2) {   /* traço cheio: fecha e continua num novo, sem emenda visível */
                if (g_drag.shared) send_stroke_progress(g_drag.layer, o);
                const int col = o->col, m = o->m, w = o->w; const int lx = g_drag.lx, ly = g_drag.ly;
                aop_t n = new_op('s'); n.m = m; n.col = col; n.w = w; n.np = 2; n.p = malloc(4 * sizeof(int));
                if (n.p) { n.p[0] = lx; n.p[1] = ly; n.p[2] = nx; n.p[3] = ny; snprintf(g_drag.op_a, sizeof g_drag.op_a, "%s", n.a); g_drag.op_i = n.i; layer_add(g_drag.layer, g_drag.shared, &n, &g_drag.act); }
            } else {
                int *np = realloc(o->p, (size_t)(o->np + 1) * 2 * sizeof(int));
                if (np) { o->p = np; o->p[o->np * 2] = nx; o->p[o->np * 2 + 1] = ny; o->np++; free(o->sm); o->sm = NULL; o->nsm = 0; }
            }
            g_drag.lx = nx; g_drag.ly = ny;
        }
        const unsigned long long t0 = now_ms();
        if (g_drag.shared && t0 - g_drag.last_flush >= 50) { k = sc_find(sc, g_drag.op_a, g_drag.op_i); if (k >= 0) send_stroke_progress(g_drag.layer, &sc->ops[k]); nb_flush(); g_drag.last_flush = t0; }
    } else if (t == T_LINE || t == T_ARROW || t == T_RECT || t == T_ELL) {
        int k = sc_find(sc, g_drag.op_a, g_drag.op_i);
        if (k >= 0) { sc->ops[k].x1 = nx; sc->ops[k].y1 = ny; }
    } else if (t == T_ERASE || t == T_ERASE_AREA) {
        const float rr = t == T_ERASE ? 9.0f : 16.0f;
        akey_t keys[64]; int nk = 0;
        for (int k = sc->n - 1; k >= 0 && nk < 64; k--) {
            const aop_t *o = &sc->ops[k];
            if (!may_erase(o, g_drag.shared, cx)) continue;
            if (op_hit(o, sf, mx, my, rr)) { snprintf(keys[nk].a, 72, "%s", o->a); keys[nk].i = o->i; nk++; }
        }
        for (int q = 0; q < nk; q++) {
            int k = sc_find(sc, keys[q].a, keys[q].i); if (k < 0) continue;
            if (t == T_ERASE) layer_del(g_drag.layer, g_drag.shared, &sc->ops[k], &g_drag.act);
            else { aop_t tmp = sc->ops[k]; erase_area_stroke(g_drag.layer, g_drag.shared, &tmp, sf, mx, my, rr, &g_drag.act); }
        }
        if (nk) nb_flush();
        g_drag.lx = nx; g_drag.ly = ny;
    } else if (t == T_LASER) {
        const unsigned long long t0 = now_ms();
        if (t0 - g_drag.last_laser >= 40) {
            g_drag.last_laser = t0;
            alaser_t l; memset(&l, 0, sizeof l); snprintf(l.scope, sizeof l.scope, "%s", cx->base); snprintf(l.a, sizeof l.a, "%s", g_me); l.x = nx / (float)AN_COORD; l.y = ny / (float)AN_COORD; l.col = g_color; l.t = t0;
            if (g_nlaser >= 128) { memmove(&g_laser[0], &g_laser[1], sizeof(alaser_t) * 127); g_nlaser = 127; }
            g_laser[g_nlaser++] = l;
            if (g_drag.shared) send_laser(cx->base, nx, ny);
        }
    }
    (void)s;
}

static void cur_layer(app_state_t *s, const actx_t *cx, char *layer, size_t cap, bool *shared) {
    *shared = eff_share(s, cx);
    if (*shared) snprintf(layer, cap, "%s", cx->base); else make_loc(cx->base, layer, cap);
}

static void do_undo(app_state_t *s, const actx_t *cx, bool redo) {
    char layer[184]; bool shared; cur_layer(s, cx, layer, sizeof layer, &shared);
    ascope_t *sc = sc_get(layer, false); if (!sc) return;
    act_t *st = redo ? sc->redo : sc->undo; int *n = redo ? &sc->nredo : &sc->nundo;
    if (*n <= 0) return;
    act_t a = st[--(*n)]; memset(&st[*n], 0, sizeof st[*n]);
    act_t inv; memset(&inv, 0, sizeof inv);
    for (int q = 0; q < a.nadd; q++) { int k = sc_find(sc, a.add[q].a, a.add[q].i); if (k >= 0) layer_del(layer, shared, &sc->ops[k], &inv); }
    for (int q = 0; q < a.nrem; q++) {
        aop_t c = op_clone(&a.rem[q]); snprintf(c.a, sizeof c.a, "%s", g_me); c.i = next_id(); c.c = next_clock(); c.sent = 0;
        layer_add(layer, shared, &c, &inv);
    }
    nb_flush(); act_free(&a);
    if (inv.nadd || inv.nrem) stack_push(redo ? sc->undo : sc->redo, redo ? &sc->nundo : &sc->nredo, inv); else act_free(&inv);
}

void annot_clear_scope(app_state_t *s, const actx_t *cx) {
    char layer[184]; bool shared; cur_layer(s, cx, layer, sizeof layer, &shared);
    ascope_t *sc = sc_get(layer, true); if (!sc) return;
    const bool all = privileged(cx, shared);
    act_t inv; memset(&inv, 0, sizeof inv);
    for (int k = 0; k < sc->n; k++) if (all || !strcmp(sc->ops[k].a, g_me)) act_add_rem(&inv, &sc->ops[k]);
    if (!inv.nrem) { act_free(&inv); return; }
    aop_t x = new_op('x'); x.m = all;
    if (shared) { nb_add(layer, &x, 0, 0); nb_flush(); }
    model_apply(sc, &x, all);
    stack_clear(sc->redo, &sc->nredo); stack_push(sc->undo, &sc->nundo, inv);
}

static bool down_edge(cui_ctx *ui, bool *down) {
    static uint64_t last_frame = (uint64_t)-1; static bool edge; const uint64_t f = cui_frame_count(ui);
    if (f != last_frame) { last_frame = f; const bool d = cui_mouse_down(ui, CUI_MOUSE_LEFT); edge = d && !g_prev_down; g_prev_down = d; }
    *down = g_prev_down; return edge;
}

static void text_commit(app_state_t *s, const actx_t *cx) {
    (void)s; (void)cx;
    if (g_text_buf[0]) {
        aop_t o = new_op('t'); o.col = (int)g_color; o.sz = g_width == 0 ? 130 : g_width == 1 ? 200 : 320; o.x0 = g_text_x; o.y0 = g_text_y; snprintf(o.s, sizeof o.s, "%s", g_text_buf);
        act_t a; memset(&a, 0, sizeof a); layer_add(g_text_layer, g_text_shared, &o, &a); nb_flush(); act_commit(g_text_layer, &a);
    }
    g_text_open = false; g_text_buf[0] = 0;
}

static bool laser_any_for(const char *base) { const unsigned long long t = now_ms(); for (int k = 0; k < g_nlaser; k++) if (!strcmp(g_laser[k].scope, base) && t - g_laser[k].t < LASER_MS) return true; return false; }

/* a superfície: camadas pintadas + interação com a ferramenta escolhida. Chamada dentro de uma pilha (STACK) que tem o tamanho da área desenhável. */
void annot_surface(cui_ctx *ui, app_state_t *s, const actx_t *cx, const asurf_t *sf, const char *id, bool interactive) {
    g_app = s;
    snprintf(g_me, sizeof g_me, "%s", s->rtc_fp[0] ? s->rtc_fp : "local-demo");
    apaint_t pd; memset(&pd, 0, sizeof pd);
    snprintf(pd.base, sizeof pd.base, "%s", cx->base); make_loc(cx->base, pd.loc, sizeof pd.loc); pd.sf = *sf;
    const cui_rect pr = cui_get_rect(ui, cui_make_id(ui, id)); const cui_vec2 mp = cui_mouse_pos(ui);
    const bool inside = pr.w > 0 && mp.x >= pr.x && mp.y >= pr.y && mp.x < pr.x + pr.w && mp.y < pr.y + pr.h;
    if (interactive && inside && (g_tool == T_ERASE || g_tool == T_ERASE_AREA) && !briks_covered(ui)) { pd.cursor = true; pd.cur = (cui_vec2){ mp.x - pr.x, mp.y - pr.y }; pd.cur_r = g_tool == T_ERASE ? 9.0f : 16.0f; }
    cui_canvas_result r = cui_canvas(ui, id, CUI_FILL, CUI_FILL, paint_canvas, &pd, sizeof pd);
    if (laser_any_for(cx->base)) cui_request_wakeup(ui, 0.016f);
    bool down; const bool edge = down_edge(ui, &down);
    if (g_drag.on && !down) drag_end(cx);
    if (!interactive || g_tool == T_NONE || briks_blocked()) return;
    if (!g_drag.on && edge && r.hovered && !briks_covered(ui) && !g_text_open) {
        const float mx = r.mouse.x, my = r.mouse.y;
        const int nx = (int)lroundf((mx - sf->ox) / sf->iw * AN_COORD), ny = (int)lroundf((my - sf->oy) / sf->ih * AN_COORD);
        if (nx >= 0 && nx <= AN_COORD && ny >= 0 && ny <= AN_COORD) {
            char layer[184]; bool shared; cur_layer(s, cx, layer, sizeof layer, &shared);
            if (g_share && !shared) note(cx->owner_fp[0] || cx->board ? "O dono não liberou o desenho para todos: você desenha só para você." : "Sem conexão com a sala: desenhando só para você.");
            if (g_tool == T_TEXT) { g_text_open = true; g_text_x = nx; g_text_y = ny; g_text_buf[0] = 0; snprintf(g_text_layer, sizeof g_text_layer, "%s", layer); g_text_shared = shared; }
            else drag_begin(s, cx, g_tool, layer, shared, nx, ny);
        }
    }
    if (g_drag.on) { drag_update(s, cx, sf, r.mouse.x, r.mouse.y); cui_request_wakeup(ui, 0.016f); }
    if (g_text_open) {
        const float px = sf->ox + g_text_x * sf->iw / AN_COORD, py = sf->oy + g_text_y * sf->ih / AN_COORD;
        cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_START, .pad_left = fmaxf(fminf(px, pr.w - 210.0f), 4.0f), .pad_top = fmaxf(fminf(py, pr.h - 44.0f), 4.0f)) {
            cui_box(ui, .id = "antxtbox", .dir = CUI_ROW, .gap = 6, .pad = 5, .radius = 12, .bg = B_GLASS_STRONG, .border = 1, .border_color = B_GLASS_BORDER, .align = CUI_ALIGN_CENTER) {
                cui_input(ui, "antxt", g_text_buf, sizeof g_text_buf, .placeholder = "Texto curto…", .width = CUI_PX(170), .autofocus = true);
                if (cui_button(ui, "OK", .variant = CUI_BUTTON_PRIMARY, .size = CUI_SIZE_SM)) text_commit(s, cx);
            }
        }
        if (cui_key_pressed(ui, CUI_KEY_ENTER)) text_commit(s, cx);
        if (cui_key_pressed(ui, CUI_KEY_ESCAPE)) { g_text_open = false; g_text_buf[0] = 0; }
    }
    if (inside) {
        if (cui_shortcut(ui, CUI_MOD_CTRL, CUI_KEY_Z)) do_undo(s, cx, false);
        if (cui_shortcut(ui, CUI_MOD_CTRL, CUI_KEY_Y)) do_undo(s, cx, true);
    }
}

/* ------------------------------------------------------------------ barra de ferramentas */

static void paint_glyph(cui_ctx *ui, cui_rect r, void *u) {
    const int g = *(const int *)u; const cui_color c = B_TEXT, d = CUI_RGBA(0xedf1f7a0);
    const float x = r.x, y = r.y, w = r.w, h = r.h, cx = x + w * 0.5f, cy = y + h * 0.5f;
    switch (g) {
    case T_PEN: cui_draw_line(ui, (cui_vec2){ x + w * 0.2f, y + h * 0.82f }, (cui_vec2){ x + w * 0.78f, y + h * 0.24f }, 3.2f, c); cui_draw_circle(ui, (cui_vec2){ x + w * 0.2f, y + h * 0.84f }, 2.0f, c); break;
    case T_HI: cui_draw_line(ui, (cui_vec2){ x + w * 0.18f, y + h * 0.8f }, (cui_vec2){ x + w * 0.8f, y + h * 0.28f }, 7.0f, CUI_RGBA(0xfdd835c0)); cui_draw_line(ui, (cui_vec2){ x + w * 0.1f, y + h * 0.95f }, (cui_vec2){ x + w * 0.5f, y + h * 0.95f }, 1.6f, c); break;
    case T_LINE: cui_draw_line(ui, (cui_vec2){ x + w * 0.14f, y + h * 0.84f }, (cui_vec2){ x + w * 0.86f, y + h * 0.16f }, 2.0f, c); break;
    case T_ARROW: cui_draw_line(ui, (cui_vec2){ x + w * 0.14f, y + h * 0.86f }, (cui_vec2){ x + w * 0.84f, y + h * 0.16f }, 2.0f, c); cui_draw_line(ui, (cui_vec2){ x + w * 0.84f, y + h * 0.16f }, (cui_vec2){ x + w * 0.52f, y + h * 0.18f }, 2.0f, c); cui_draw_line(ui, (cui_vec2){ x + w * 0.84f, y + h * 0.16f }, (cui_vec2){ x + w * 0.82f, y + h * 0.48f }, 2.0f, c); break;
    case T_RECT: { const cui_vec2 q[4] = { { x + w * 0.16f, y + h * 0.24f }, { x + w * 0.84f, y + h * 0.24f }, { x + w * 0.84f, y + h * 0.76f }, { x + w * 0.16f, y + h * 0.76f } }; cui_draw_polyline(ui, q, 4, 2.0f, c, true); break; }
    case T_ELL: { cui_vec2 q[32]; for (int k = 0; k < 32; k++) { const float t = 6.2831853f * (float)k / 32.0f; q[k] = (cui_vec2){ cx + cosf(t) * w * 0.36f, cy + sinf(t) * h * 0.28f }; } cui_draw_polyline(ui, q, 32, 2.0f, c, true); break; }
    case T_TEXT: cui_draw_line(ui, (cui_vec2){ x + w * 0.2f, y + h * 0.22f }, (cui_vec2){ x + w * 0.8f, y + h * 0.22f }, 2.2f, c); cui_draw_line(ui, (cui_vec2){ cx, y + h * 0.22f }, (cui_vec2){ cx, y + h * 0.84f }, 2.2f, c); break;
    case T_ERASE: { const cui_vec2 q[4] = { { x + w * 0.2f, y + h * 0.62f }, { x + w * 0.52f, y + h * 0.22f }, { x + w * 0.86f, y + h * 0.46f }, { x + w * 0.54f, y + h * 0.84f } }; cui_draw_polyline(ui, q, 4, 2.0f, c, true); cui_draw_line(ui, (cui_vec2){ x + w * 0.36f, y + h * 0.42f }, (cui_vec2){ x + w * 0.7f, y + h * 0.66f }, 1.6f, d); break; }
    case T_ERASE_AREA: cui_draw_arc(ui, (cui_vec2){ cx, cy }, w * 0.32f, 0.0f, 6.2831853f, 1.8f, c); cui_draw_circle(ui, (cui_vec2){ cx, cy }, w * 0.12f, d); break;
    case T_LASER: cui_draw_circle(ui, (cui_vec2){ cx, cy }, w * 0.34f, CUI_RGBA(0xe5393555)); cui_draw_circle(ui, (cui_vec2){ cx, cy }, w * 0.16f, CUI_RGB(0xe53935)); break;
    default: break;
    }
}
static bool tbtn_hit(cui_ctx *ui, const char *id, const char *tip) {
    if (briks_hover(ui, id)) briks_tip(tip);
    return briks_clicked_r(ui, id, 9.0f);
}
static bool tool_btn(cui_ctx *ui, int tool, const char *tip) {
    char id[16]; snprintf(id, sizeof id, "ant%d", tool);
    cui_box(ui, .id = id, .width = CUI_PX(28), .height = CUI_PX(28), .radius = 9, .bg = g_tool == tool ? CUI_RGBA(0x6d5cf6aa) : CUI_RGBA(0xffffff0d), .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
        cui_push_id_int(ui, tool); int g = tool; cui_canvas(ui, "g", CUI_PX(20), CUI_PX(20), paint_glyph, &g, sizeof g); cui_pop_id(ui);
    }
    return tbtn_hit(ui, id, tip);
}
static bool icon_btn(cui_ctx *ui, const char *id, const char *icon, bool on, const char *tip, bool dim) {
    cui_box(ui, .id = id, .width = CUI_PX(28), .height = CUI_PX(28), .radius = 9, .bg = on ? CUI_RGBA(0x6d5cf6aa) : CUI_RGBA(0xffffff0d), .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER, .opacity = dim ? 0.45f : 1.0f) { cui_icon(ui, icon, 15, B_TEXT); }
    return tbtn_hit(ui, id, tip);
}
static const unsigned PALETTE[8] = { 0xe53935, 0xfb8c00, 0xfdd835, 0x43a047, 0x1e88e5, 0x8e24aa, 0xffffff, 0x111111 };

static void toolbar_body(cui_ctx *ui, app_state_t *s, const actx_t *cx, bool closable, bool *close) {
    static const char *TIPS[T_COUNT] = { "", "Caneta", "Marca-texto", "Linha", "Seta", "Retângulo", "Elipse", "Texto", "Borracha (traço inteiro)", "Borracha (área)", "Laser (some em 1 s)" };
    snprintf(g_me, sizeof g_me, "%s", s->rtc_fp[0] ? s->rtc_fp : "local-demo");
    const bool can = can_share(s, cx);
    cui_column(ui, .gap = 3) {
        for (int t = T_PEN; t < T_COUNT; t += 2) cui_row(ui, .gap = 3) { if (tool_btn(ui, t, TIPS[t])) g_tool = t; if (t + 1 < T_COUNT && tool_btn(ui, t + 1, TIPS[t + 1])) g_tool = t + 1; }
        cui_row(ui, .gap = 3) {
            cui_box(ui, .id = "anclr", .width = CUI_PX(28), .height = CUI_PX(28), .radius = 9, .bg = CUI_RGBA(0xffffff0d), .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                cui_box(ui, .width = CUI_PX(16), .height = CUI_PX(16), .radius = 8, .bg = CUI_RGB(g_color), .border = 1, .border_color = CUI_RGBA(0xffffff90));
            }
            if (briks_hover(ui, "anclr")) briks_tip("Cor");
            if (briks_clicked_r(ui, "anclr", 9.0f)) cui_popover_toggle(ui, "ancolor");
            cui_box(ui, .id = "anw", .width = CUI_PX(28), .height = CUI_PX(28), .radius = 9, .bg = CUI_RGBA(0xffffff0d), .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                const float rr = 2.0f + (float)g_width * 2.2f; cui_box(ui, .width = CUI_PX(rr * 2), .height = CUI_PX(rr * 2), .radius = rr, .bg = B_TEXT);
            }
            if (briks_hover(ui, "anw")) briks_tip(g_width == 0 ? "Espessura: fina" : g_width == 1 ? "Espessura: média" : "Espessura: grossa");
            if (briks_clicked_r(ui, "anw", 9.0f)) g_width = (g_width + 1) % 3;
        }
        cui_row(ui, .gap = 3) {
            if (icon_btn(ui, "anfill", CUI_ICON_SQUARE, g_fill, g_fill ? "Formas preenchidas (ligado)" : "Formas preenchidas (desligado)", false)) g_fill = !g_fill;
            if (icon_btn(ui, "anmode", g_share && can ? CUI_ICON_USERS : CUI_ICON_USER, false, g_share ? (can ? "Para todos: os outros veem. Clique para desenhar só para você" : "Para todos (o dono não liberou: fica só com você)") : "Só eu: ninguém vê. Clique para desenhar para todos", g_share && !can)) {
                g_share = !g_share; if (g_share && !can) note(cx->owner_fp[0] || cx->board ? "O dono não liberou o desenho para todos." : "Sem conexão com a sala.");
            }
        }
        cui_row(ui, .gap = 3) {
            if (icon_btn(ui, "anundo", CUI_ICON_UNDO_2, false, "Desfazer (Ctrl+Z)", false)) do_undo(s, cx, false);
            if (icon_btn(ui, "anredo", CUI_ICON_REDO_2, false, "Refazer (Ctrl+Y)", false)) do_undo(s, cx, true);
        }
        cui_row(ui, .gap = 3) {
            if (icon_btn(ui, "anclear", CUI_ICON_TRASH_2, false, "Limpar tudo (o que é seu; o dono limpa tudo)", false)) annot_clear_scope(s, cx);
            if (cx->own && !cx->board) {
                const aperm_t *p = perm_get(g_me, false); const bool open = p && p->all;
                if (icon_btn(ui, "anperm", open ? CUI_ICON_UNLOCK : CUI_ICON_LOCK, open, open ? "Espectadores podem desenhar. Clique para travar" : "Só você desenha. Clique para liberar aos espectadores", false)) send_perm(s, !open);
            } else if (closable) cui_box(ui, .width = CUI_PX(28), .height = CUI_PX(28));
        }
        if (closable) cui_row(ui, .gap = 3) { if (icon_btn(ui, "anclose", CUI_ICON_X, false, "Sair das anotações (Esc)", false)) *close = true; }
    }
    cui_popover(ui, "ancolor", .anchor = cui_make_id(ui, "anclr"), .placement = CUI_PLACE_RIGHT_START, .pad = 8, .gap = 6) {
        cui_row(ui, .gap = 4) {
            for (int k = 0; k < 8; k++) {
                char id[16]; snprintf(id, sizeof id, "anp%d", k);
                cui_box(ui, .id = id, .width = CUI_PX(24), .height = CUI_PX(24), .radius = 12, .bg = CUI_RGB(PALETTE[k]), .border = g_color == PALETTE[k] ? 2 : 1, .border_color = g_color == PALETTE[k] ? B_C3 : CUI_RGBA(0xffffff50));
                if (briks_clicked_r(ui, id, 12.0f)) g_color = PALETTE[k];
            }
        }
        cui_color c = CUI_RGB(g_color);
        if (cui_color_picker(ui, "ancp", &c)) g_color = ((unsigned)c.r << 16) | ((unsigned)c.g << 8) | c.b;
    }
}

void annot_toolbar_board(cui_ctx *ui, app_state_t *s, const actx_t *cx) { bool c = false; toolbar_body(ui, s, cx, false, &c); }

/* ------------------------------------------------------------------ superfície sobre a transmissão */

static int g_active_key = -1;
static bool tile_scope(app_state_t *s, int key, actx_t *out) {
    memset(out, 0, sizeof *out);
    const int FVN = BRIKS_MAX_PEERS + 1;
    int slot = 0, peer = -1; bool own = false;
    if (key == BRIKS_MAX_PEERS) { own = true; slot = 0; } else if (key == FVN + BRIKS_MAX_PEERS) { own = true; slot = 1; }
    else if (key >= 0 && key < BRIKS_MAX_PEERS) { peer = key; slot = 0; } else if (key >= FVN && key < FVN + BRIKS_MAX_PEERS) { peer = key - FVN; slot = 1; } else return false;
    out->own = own;
    if (own) snprintf(out->owner_fp, sizeof out->owner_fp, "%s", s->rtc_fp[0] ? s->rtc_fp : "local-demo");
    else snprintf(out->owner_fp, sizeof out->owner_fp, "%s", s->rtc_peers[peer].fp);
    if (out->owner_fp[0]) snprintf(out->base, sizeof out->base, "s:%s:%d", out->owner_fp, slot); else snprintf(out->base, sizeof out->base, "s:peer%d:%d", peer, slot);
    return true;
}
bool annot_tile_scope(app_state_t *s, int key, actx_t *out) { return tile_scope(s, key, out); }
bool annot_tile_active(int key) { return g_active_key == key && g_tool != T_NONE; }

typedef struct { cui_texture tex; float tw, th, z, cx, cy, dw, dh; } aimg_t;
static void paint_img(cui_ctx *ui, cui_rect r, void *u) {
    const aimg_t *a = u;
    const float hw = a->dw * 0.5f / a->z, hh = a->dh * 0.5f / a->z;
    const cui_rect uv = { (a->cx - hw) / a->tw, (a->cy - hh) / a->th, (a->cx + hw) / a->tw, (a->cy + hh) / a->th };
    cui_draw_image(ui, a->tex, r, uv, CUI_CORNERS(13.0f), (cui_color){ 255, 255, 255, 255 });   /* mesmo raio da moldura do bloco: nada reto vaza */
}
static void clamp_center(float *c, float half, float full) { if (half * 2.0f >= full) *c = full * 0.5f; else if (*c < half) *c = half; else if (*c > full - half) *c = full - half; }

bool annot_tile_image(cui_ctx *ui, app_state_t *s, int key, cui_texture tex, int tw, int th, cui_image_view *zv, float dw, float dh, float sc) {
    actx_t cx; if (!tile_scope(s, key, &cx) || !zv) return false;
    const bool has = annot_scope_count(cx.base, false) > 0 || annot_scope_count(cx.base, true) > 0 || laser_any_for(cx.base);
    const bool active = g_active_key == key && g_tool != T_NONE;
    if (!has && !active) return false;
    float z = zv->zoom > 0 ? zv->zoom : sc, ccx = zv->zoom > 0 ? zv->cx : (float)tw * 0.5f, ccy = zv->zoom > 0 ? zv->cy : (float)th * 0.5f;
    const cui_rect pr = cui_get_rect(ui, cui_make_id(ui, "annimg")); const cui_vec2 mp = cui_mouse_pos(ui);
    const bool inside = pr.w > 0 && mp.x >= pr.x && mp.y >= pr.y && mp.x < pr.x + pr.w && mp.y < pr.y + pr.h && !briks_covered(ui) && !briks_blocked();
    if (inside) {   /* zoom com a roda em torno do ponteiro; arrasto move (botão do meio sempre; o esquerdo quando não há ferramenta) */
        const float w = cui_wheel_take(ui);
        if (w != 0.0f) {
            const float px = ccx + (mp.x - (pr.x + dw * 0.5f)) / z, py = ccy + (mp.y - (pr.y + dh * 0.5f)) / z;
            z *= powf(1.2f, w); if (z < sc) z = sc; if (z > sc * 8.0f) z = sc * 8.0f;
            ccx = px - (mp.x - (pr.x + dw * 0.5f)) / z; ccy = py - (mp.y - (pr.y + dh * 0.5f)) / z;
        }
        cui_wheel_claim(ui, pr);
    }
    static bool pan; static cui_vec2 pan0; static float pcx, pcy; static int pan_key = -1;
    const bool pdown = cui_mouse_down(ui, CUI_MOUSE_MIDDLE) || (g_tool == T_NONE && cui_mouse_down(ui, CUI_MOUSE_LEFT) && z > sc * 1.01f);
    if (pdown && ((inside && !pan) || (pan && pan_key == key))) { if (!pan) { pan = true; pan0 = mp; pcx = ccx; pcy = ccy; pan_key = key; } ccx = pcx - (mp.x - pan0.x) / z; ccy = pcy - (mp.y - pan0.y) / z; } else if (!pdown && pan && pan_key == key) { pan = false; }
    clamp_center(&ccx, dw * 0.5f / z, (float)tw); clamp_center(&ccy, dh * 0.5f / z, (float)th);
    if (z <= sc * 1.001f) { *zv = (cui_image_view){ 0, 0, 0 }; z = sc; ccx = (float)tw * 0.5f; ccy = (float)th * 0.5f; } else *zv = (cui_image_view){ z, ccx, ccy };
    aimg_t ai = { tex, (float)tw, (float)th, z, ccx, ccy, dw, dh };
    asurf_t sf = { dw * 0.5f - ccx * z, dh * 0.5f - ccy * z, (float)tw * z, (float)th * z };
    cui_box(ui, .dir = CUI_STACK, .width = CUI_FILL, .height = CUI_PX(dh)) {
        cui_canvas(ui, "annimg", CUI_FILL, CUI_FILL, paint_img, &ai, sizeof ai);
        annot_surface(ui, s, &cx, &sf, "annsurf", active);
    }
    return true;
}

void annot_tile_ui(cui_ctx *ui, app_state_t *s, int key, bool hover, bool compact) {
    actx_t cx; if (compact || !tile_scope(s, key, &cx)) return;
    const bool active = g_active_key == key && g_tool != T_NONE;
    if (hover && !g_text_open && cui_key_pressed(ui, CUI_KEY_A) && cui_key_mods(ui) == 0) { if (active) { g_tool = T_NONE; g_active_key = -1; } else { g_active_key = key; if (g_tool == T_NONE) g_tool = T_PEN; } }
    if (active && cui_key_pressed(ui, CUI_KEY_ESCAPE) && !g_text_open) { g_tool = T_NONE; g_active_key = -1; }
    if (hover || active) cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_START, .pad = 12) {
        cui_box(ui, .dir = CUI_ROW, .gap = 8, .align = CUI_ALIGN_CENTER) {
            cui_box(ui, .id = "anbtn", .dir = CUI_ROW, .gap = 6, .pad_x = 11, .height = CUI_PX(30), .radius = 15, .bg = active ? CUI_RGBA(0x6d5cf6d0) : B_GLASS_STRONG, .border = 1, .border_color = B_GLASS_BORDER, .align = CUI_ALIGN_CENTER) {
                cui_icon(ui, CUI_ICON_PENCIL, 14, B_TEXT); cui_text(ui, active ? "Anotando" : "Anotar", .size = 12, .nowrap = true);
            }
            if (briks_hover(ui, "anbtn")) briks_tip(active ? "Sair das anotações (Esc ou A)" : "Desenhar sobre esta transmissão (A)");
            if (briks_clicked_r(ui, "anbtn", 15.0f)) { if (active) { g_tool = T_NONE; g_active_key = -1; } else { g_active_key = key; if (g_tool == T_NONE) g_tool = T_PEN; } }
            const char *nt = annot_note();
            if (active && nt[0]) cui_box(ui, .pad_x = 10, .pad_y = 5, .radius = 12, .bg = B_GLASS_STRONG, .max_width = 300.0f) { cui_text(ui, nt, .size = 11, .color = B_MUTED); }
        }
    }
    if (active) cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_CENTER, .pad_left = 10) {
        cui_box(ui, .id = "anbar", .pad = 5, .radius = 14, .bg = B_GLASS_STRONG, .border = 1, .border_color = B_GLASS_BORDER) {
            bool close = false; toolbar_body(ui, s, &cx, true, &close);
            if (close) { g_tool = T_NONE; g_active_key = -1; }
        }
    }
    { const cui_rect br = cui_get_rect(ui, cui_make_id(ui, "anbar")); if (br.w > 0) briks_cover(br.x, br.y, br.w, br.h); }
}

/* ------------------------------------------------------------------ roteiros de teste */

void annot_script(app_state_t *s, const char *cmd, const char *arg) {
    if (!s) s = g_app;
    snprintf(g_me, sizeof g_me, "%s", s && s->rtc_fp[0] ? s->rtc_fp : "local-demo");
    if (!strcmp(cmd, "annot-tool")) { g_tool = (int)strtol(arg, NULL, 10); if (g_tool < 0 || g_tool >= T_COUNT) g_tool = T_NONE; }   /* 1 caneta 2 marca 3 linha 4 seta 5 ret 6 elipse 7 texto 8 borracha 9 area 10 laser */
    else if (!strcmp(cmd, "annot-on")) { g_active_key = (int)strtol(arg, NULL, 10); if (g_tool == T_NONE) g_tool = T_PEN; }
    else if (!strcmp(cmd, "annot-off")) { g_active_key = -1; g_tool = T_NONE; }
    else if (!strcmp(cmd, "annot-share")) g_share = strtol(arg, NULL, 10) != 0;
    else if (!strcmp(cmd, "annot-fill")) g_fill = strtol(arg, NULL, 10) != 0;
    else if (!strcmp(cmd, "annot-color")) g_color = (unsigned)strtoul(arg, NULL, 16);
    else if (!strcmp(cmd, "annot-width")) g_width = (int)strtol(arg, NULL, 10) % 3;
    else if (!strcmp(cmd, "annot-reset")) annot_room_left();
    else if (!strcmp(cmd, "annot-draw") || !strcmp(cmd, "annot-undo") || !strcmp(cmd, "annot-redo") || !strcmp(cmd, "annot-clear")) {
        /* annot-draw <alvo> <pen|hi|line|arrow|rect|ell|rectf|ellf|text:...> x,y x,y ...  (alvo = chave do bloco ou bN do Board; coordenadas 0..10000) */
        char buf[1024]; snprintf(buf, sizeof buf, "%s", arg); char *tg = strtok(buf, " "); if (!tg || !s) return;
        actx_t cx; if (tg[0] == 'b') { memset(&cx, 0, sizeof cx); cx.board = true; snprintf(cx.base, sizeof cx.base, "b:%d", atoi(tg + 1)); } else if (!tile_scope(s, atoi(tg), &cx)) return;
        if (!strcmp(cmd, "annot-undo")) { do_undo(s, &cx, false); return; } if (!strcmp(cmd, "annot-redo")) { do_undo(s, &cx, true); return; } if (!strcmp(cmd, "annot-clear")) { annot_clear_scope(s, &cx); return; }
        char *kind = strtok(NULL, " "); if (!kind) return;
        int xs[2 * AN_MAX_PTS], n = 0; char *c; while ((c = strtok(NULL, " ")) && n < 2 * 400) { int x = 0, y = 0; if (sscanf(c, "%d,%d", &x, &y) == 2) { xs[n++] = x; xs[n++] = y; } }
        char layer[184]; bool shared; cur_layer(s, &cx, layer, sizeof layer, &shared);
        aop_t o; act_t a; memset(&a, 0, sizeof a);
        if (!strcmp(kind, "pen") || !strcmp(kind, "hi")) {
            if (n < 2) return; o = new_op('s'); o.m = kind[0] == 'h'; o.col = (int)g_color; o.w = WIDTHS[g_width]; o.np = n / 2; o.p = malloc((size_t)n * sizeof(int)); if (!o.p) return; memcpy(o.p, xs, (size_t)n * sizeof(int));
            layer_add(layer, shared, &o, &a); op_simplify(&sc_get(layer, true)->ops[sc_find(sc_get(layer, true), g_me, g_next_id)], 3.0f);
        } else if (!strncmp(kind, "text:", 5)) {
            if (n < 2) return; o = new_op('t'); o.col = (int)g_color; o.sz = g_width == 0 ? 130 : g_width == 1 ? 200 : 320; o.x0 = xs[0]; o.y0 = xs[1]; snprintf(o.s, sizeof o.s, "%s", kind + 5); for (char *q = o.s; *q; q++) if (*q == '_') *q = ' ';
            layer_add(layer, shared, &o, &a);
        } else if (n >= 4) {
            o = new_op('g'); o.sh = !strcmp(kind, "line") ? 'l' : !strcmp(kind, "arrow") ? 'a' : kind[0] == 'r' ? 'r' : 'e'; o.f = kind[strlen(kind) - 1] == 'f'; o.col = (int)g_color; o.w = WIDTHS[g_width]; o.x0 = xs[0]; o.y0 = xs[1]; o.x1 = xs[2]; o.y1 = xs[3];
            layer_add(layer, shared, &o, &a);
        }
        nb_flush(); act_commit(layer, &a);
    }
}
int annot_probe(const char *name) {
    if (!strcmp(name, "annot.tool")) return g_tool;
    if (!strcmp(name, "annot.active")) return g_active_key;
    if (!strcmp(name, "annot.lasers")) return g_nlaser;
    if (!strncmp(name, "annot.ops:", 10)) { const char *sc = name + 10; bool loc = false; if (!strncmp(sc, "L:", 2)) { loc = true; sc += 2; } return annot_scope_count(sc, loc); }
    return -1;
}

const float *annot_op_smooth(const aop_t *o, int *n) { aop_t *w = (aop_t *)o; if (!w->sm) build_sm(w); *n = w->nsm; return w->sm; }
void annot_tool_reset(void) { g_tool = T_NONE; g_active_key = -1; }
void annot_tool_default(void) { if (g_tool == T_NONE) g_tool = T_PEN; }
