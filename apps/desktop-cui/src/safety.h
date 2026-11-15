/* Segurança de conteúdo (1.5.28): transmissões de quem não é amigo começam cobertas; "Parar de ver"; denúncia com registro local. */
#ifndef BRIKS_SAFETY_H
#define BRIKS_SAFETY_H
#include "briks_ui.h"

bool settings_sens_guard(void); void settings_set_sens_guard(bool on);   /* settings.c */
bool safety_guard_on(void);                                   /* Configurações > Segurança (padrão: ligado) */
void safety_guard_set(bool on);
bool safety_is_friend(const app_state_t *s, int slot);        /* o slot da chamada é de um amigo (fp ou id na lista) */
bool safety_covered(const app_state_t *s, int slot);                /* a transmissão deste slot deve estar coberta agora (desconhecido ainda não revelado, ou "parei de ver") */
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
