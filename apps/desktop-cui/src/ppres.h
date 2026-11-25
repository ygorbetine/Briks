/* Online/offline entre amigos sem servidor guardando nada: ao entrar (e a cada 2 min) o Briks manda um aviso assinado "hi" para a caixa de cada
 * amigo; quem estiver online recebe, marca o amigo como online e responde "hi" com ack. Sair ou "Aparecer offline" manda "bye". Um amigo é
 * dado como offline depois de PPRES_TTL_MS sem aviso. Ligado por padrão; só "Aparecer offline" desliga (ver ppres.c). */
#ifndef BRIKS_PPRES_H
#define BRIKS_PPRES_H
#include <stdbool.h>
#include "briks_ui.h"
#include "json.h"
#include "ppres_model.h"
/* estado */
void ppres_event(app_state_t *s, const jval *j);   /* dm.signal de tipo hi/bye */
void ppres_tick(app_state_t *s);                    /* chamar a cada quadro: anuncia, responde e expira */
bool ppres_online(const char *friend_id);           /* o amigo me avisou há pouco */
void ppres_reset(void);                             /* sair da conta */
#endif
