/* Anotações sobre as transmissões e Board da sala (lado da UI). Protocolo, limites e permissões ficam no core (briks-core/src/annot.rs);
 * aqui: modelo de operações espelhado, ferramentas, desenho no cui_canvas e barra flutuante. Plano: docs/plans/anotacoes-board.md. */
#ifndef BRIKS_ANNOT_H
#define BRIKS_ANNOT_H
#include <stdbool.h>
#include <stddef.h>
#include "json.h"
#include "briks_ui.h"

#define AN_COORD 10000          /* coordenadas normalizadas inteiras: imagem inteira = 0..10000 */
#define AN_MAX_PTS 2000

/* operação (traço k='s', forma 'g', texto 't'); apagar/limpar atuam sobre o modelo e não ficam guardados */
typedef struct aop {
    char a[72]; unsigned i; unsigned long long c; char k;
    int m, col, w, f, sz; char sh; int x0, y0, x1, y1;   /* m: 0 caneta 1 marca-texto; w: espessura (décimos de milésimo da largura); f: preenchida */
    char s[128];
    int *p; int np;            /* pontos x,y alternados (np = nº de pontos) */
    float *sm; int nsm;        /* cache do traço suavizado (x,y normalizados 0..1) */
    int sent;                  /* pontos já enviados à rede (traço em andamento) */
} aop_t;

/* retângulo (em px, relativo ao canvas) onde a imagem normalizada 0..1 aparece */
typedef struct asurf { float ox, oy, iw, ih; } asurf_t;
/* contexto de uma superfície: escopo compartilhado (nome no core) e quem é o dono */
typedef struct actx { char base[176]; bool board; bool own; char owner_fp[72]; bool compact; } actx_t;

/* eventos do core (annot.*), pares autenticados e fim de transmissão */
void annot_core_event(app_state_t *s, const jval *j, const char *ev);
void annot_peer_up(app_state_t *s);                        /* um par autenticou: pede o estado atual (quem já está na sala responde) */
void annot_stream_gone(app_state_t *s, int peer, int slot);/* a transmissão de rtc_peers[peer] (slot 0/1) acabou: some o que era dela */
void annot_stream_gone_own(app_state_t *s, int slot);      /* parei a minha transmissão */
void annot_room_left(void);                                /* saí da chamada: esquece tudo */

/* camada sobre uma transmissão (chamados por video_tile em ui_share.c) */
bool annot_tile_active(int key);                           /* este bloco está sendo anotado agora (pede o desenho próprio da imagem) */
bool annot_tile_image(cui_ctx *ui, app_state_t *s, int key, cui_texture tex, int tw, int th, cui_image_view *zv, float dw, float dh, float sc);   /* true = desenhou a imagem e a camada */
void annot_tile_ui(cui_ctx *ui, app_state_t *s, int key, bool hover, bool compact);   /* botão "Anotar" e barra de ferramentas */
bool annot_tile_scope(app_state_t *s, int key, actx_t *out);

/* Board (board.c) */
void board_stage(cui_ctx *ui, app_state_t *s, float W, float H);
void board_room_left(void);

/* núcleo compartilhado (annot.c) usado pelo Board */
void annot_surface(cui_ctx *ui, app_state_t *s, const actx_t *cx, const asurf_t *sf, const char *id, bool interactive);
void annot_toolbar_board(cui_ctx *ui, app_state_t *s, const actx_t *cx);
const float *annot_op_smooth(const aop_t *o, int *n);       /* traço suavizado (x,y normalizados) para exportar */
void annot_tool_reset(void);                               /* sem ferramenta (modo "mover") */
void annot_tool_default(void);                             /* caneta se não houver ferramenta */
bool annot_tool_active(void);                              /* há ferramenta de desenho escolhida (o Board desenha em vez de arrastar) */
int  annot_scope_count(const char *base, bool local);      /* operações vivas (camada compartilhada ou só minha) */
const aop_t *annot_scope_op(const char *base, bool local, int idx);
void annot_clear_scope(app_state_t *s, const actx_t *cx);  /* "limpar tudo" do escopo (respeita o modo Só eu/Para todos) */
bool annot_shared_mode(void);
const char *annot_note(void);                              /* aviso curto (permissão negada, limite) ou "" */
cui_color annot_cur_color(void);

/* roteiros de teste (BRIKS_SCRIPT) */
void annot_script(app_state_t *s, const char *cmd, const char *arg);
int annot_probe(const char *name);
#endif
