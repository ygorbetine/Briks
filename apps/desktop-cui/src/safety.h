/* Segurança de conteúdo: "Parar de ver" uma transmissão e Denunciar. Sem cobertura automática por não ser amigo. */
#ifndef BRIKS_SAFETY_H
#define BRIKS_SAFETY_H
#include "briks_ui.h"

bool safety_is_friend(const app_state_t *s, int slot);        /* o slot da chamada é de um amigo (fp ou id na lista) */
bool safety_covered(const app_state_t *s, int slot);                /* a transmissão deste slot está escondida ("Parar de ver") */
bool safety_hidden(const app_state_t *s, int slot);                 /* "Parar de ver" ativo para este slot */
void safety_reveal(app_state_t *s, int slot);                 /* "Mostrar" */
void safety_hide(app_state_t *s, int slot, bool hide);
void safety_slot_reset(int slot);                             /* a pessoa saiu: o estado do slot não passa para quem entrar depois */
void safety_report_open(int slot);                            /* abre o diálogo de denúncia para o slot */
void safety_report_dialog(cui_ctx *ui, app_state_t *s);       /* desenha o diálogo (chamar no fim da chamada) */
/* testes: grava a denúncia sem UI; devolve true se o registro foi escrito */
bool safety_report_write(const char *dir, const char *who_id, const char *who_name, const char *reason, bool blocked);
int safety_reason_count(void); const char *safety_reason_label(int i); const char *safety_reason_key(int i);
#endif
