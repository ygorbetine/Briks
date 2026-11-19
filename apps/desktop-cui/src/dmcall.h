/* Chamada individual (1:1) e sinais da caixa de chegada (dm.signal do core): "Ligar" a um amigo e o cartão "<nome> está te ligando".
 * O sinal é assinado pelo core e entregue pelo canal de presença da caixa do amigo, sem depender do canal direto. */
#ifndef BRIKS_DMCALL_H
#define BRIKS_DMCALL_H
#include "briks_ui.h"
#include "json.h"

void dmcall_start(app_state_t *s, int friend_idx);                  /* Ligar: sala aleatória não listada, entra nela e manda o sinal "call" */
bool dmcall_event(app_state_t *s, const char *ev, const jval *j);   /* dm.signal / dm.signal.gone / dm.signal.sent; true se tratou */
void dmcall_tick(app_state_t *s);                                   /* chamada saindo: peer entrou, desligou ou 45 s sem resposta */
void dmcall_overlay(cui_ctx *ui, app_state_t *s);                   /* faixa "Chamando…" (qualquer tela) */
void dmcall_dialog(cui_ctx *ui, app_state_t *s);                    /* cartão de chamada recebida (qualquer tela) */
void dmcall_button(cui_ctx *ui, app_state_t *s);                    /* botão "Ligar" do cabeçalho da conversa aberta */
void dmcall_signal_ginv(app_state_t *s, const person_t *f, const char *gid, const char *gname, const char *inviter, const char *key);   /* convite de grupo por sinal (10 min) */
void dmcall_signal_stop(const char *key);
void dmcall_log(const char *text);                                  /* linha no rede.log do core */
bool dmcall_calling(void);                                          /* teste: há chamada saindo */
int  dmcall_ringing(void);                                          /* teste: chamadas recebidas em aberto */
#endif
