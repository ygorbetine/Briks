/* "Chamar atenção" (tremer a tela): lógica pura, sem Windows nem CUI, para o app (nudge.c) e o teste (tests/nudge_test.c) usarem o MESMO código.
 * Limites de envio/recebimento, Não perturbar e a decisão do que fazer quando o sinal chega. */
#ifndef BRIKS_NUDGE_MODEL_H
#define BRIKS_NUDGE_MODEL_H
#include <stdbool.h>
#include <string.h>

#define NM_PER_FRIEND_MS 15000        /* 1 chamada a cada 15 s por amigo (envio e recebimento) */
#define NM_GLOBAL_MAX 6               /* e no máximo 6 por minuto no total */
#define NM_GLOBAL_WINDOW_MS 60000
#define NM_KEYS 64

typedef struct { char key[80]; unsigned long long t; } nm_last;
typedef struct { nm_last last[NM_KEYS]; int n; unsigned long long glob[NM_GLOBAL_MAX]; int gn; } nm_limiter;

/* true = pode; já registra. `now` em ms (relógio monotônico). Só registra quando permite (negado não "renova" o prazo). */
static inline bool nm_allow(nm_limiter *l, const char *key, unsigned long long now) {
    if (!key || !key[0]) return false;
    nm_last *e = NULL;
    for (int i = 0; i < l->n; i++) if (!strcmp(l->last[i].key, key)) { e = &l->last[i]; break; }
    if (e && now >= e->t && now - e->t < NM_PER_FRIEND_MS) return false;
    int live = 0; unsigned long long keep[NM_GLOBAL_MAX];
    for (int i = 0; i < l->gn; i++) if (now >= l->glob[i] && now - l->glob[i] < NM_GLOBAL_WINDOW_MS) keep[live++] = l->glob[i];
    if (live >= NM_GLOBAL_MAX) { memcpy(l->glob, keep, sizeof(unsigned long long) * (size_t)live); l->gn = live; return false; }
    memcpy(l->glob, keep, sizeof(unsigned long long) * (size_t)live); l->gn = live; l->glob[l->gn++] = now;
    if (!e) {
        if (l->n >= NM_KEYS) { memmove(&l->last[0], &l->last[1], sizeof(nm_last) * (NM_KEYS - 1)); l->n = NM_KEYS - 1; }
        e = &l->last[l->n++]; strncpy(e->key, key, sizeof e->key - 1); e->key[sizeof e->key - 1] = 0;
    }
    e->t = now; return true;
}

/* ---- Não perturbar: 0 = desligado, -1 = até eu desligar, >0 = até esse instante (segundos Unix) ---- */
static inline bool nm_dnd_active(long long dnd_until, long long now_s) { return dnd_until == -1 || (dnd_until > 0 && now_s < dnd_until); }
static inline long long nm_dnd_for(int minutes, long long now_s) { return minutes <= 0 ? -1 : now_s + (long long)minutes * 60; }

/* ---- o que fazer ao receber ---- */
typedef enum { NM_MODE_SCREEN = 0, NM_MODE_WINDOW = 1, NM_MODE_NOTICE = 2 } nm_mode;
typedef enum { NM_NONE = 0, NM_SHAKE_SCREEN, NM_SHAKE_WINDOW, NM_FLASH, NM_NOTICE } nm_action;
typedef struct {
    bool recv_on;      /* "Receber chamadas de atenção" */
    bool dnd;          /* Não perturbar ativo */
    int mode;          /* nm_mode */
    bool os_busy;      /* jogo em tela cheia exclusiva, apresentação ou "ocupado" do Windows */
    bool in_tray;      /* janela fechada para a bandeja (oculta) */
    bool background;   /* minimizada ou não é a janela da frente */
    bool maximized;    /* janela maximizada: "só a janela" não tem para onde mover */
} nm_ctx;
static inline nm_action nm_decide(const nm_ctx *c) {
    if (!c->recv_on || c->dnd || c->os_busy) return NM_NONE;
    if (c->in_tray) return NM_NOTICE;
    if (c->background) return NM_FLASH;
    if (c->mode == NM_MODE_NOTICE) return NM_NOTICE;
    if (c->mode == NM_MODE_WINDOW) return c->maximized ? NM_NOTICE : NM_SHAKE_WINDOW;
    return NM_SHAKE_SCREEN;
}

/* deslocamento do quadro f de n (amplitude decrescente); r = sorteio em [0, 65535] */
static inline int nm_offset(int amp_max, int f, int n, unsigned r) {
    if (n <= 0 || f >= n) return 0;
    int amp = amp_max * (n - f) / n; if (amp < 1) return 0;
    return (int)(r % (unsigned)(2 * amp + 1)) - amp;
}
#endif
