/* Online/offline entre amigos: validade do aviso e limite de respostas (src/ppres_model.h). Sem janela. */
#include "../src/ppres_model.h"
#include <stdio.h>
static int fails;
#define OK(c, name) do { if (!(c)) { fails++; printf("FALHOU: %s\n", name); } else printf("ok: %s\n", name); } while (0)
int main(void) {
    OK(!ppres_fresh(0, 1000), "nunca visto: offline");
    OK(ppres_fresh(1000, 1000 + PPRES_TTL_MS - 1), "aviso recente: online");
    OK(!ppres_fresh(1000, 1000 + PPRES_TTL_MS), "passado o prazo: offline");
    OK(!ppres_fresh(5000, 4000), "relogio voltou: nao conta como online");
    OK(PPRES_TTL_MS > 2 * PPRES_REFRESH_MS, "o prazo cobre um anuncio perdido");
    OK(ppres_may_reply(0, 100), "primeira resposta passa");
    OK(!ppres_may_reply(1000, 1000 + PPRES_REPLY_MIN_MS - 1), "resposta repetida antes de 20 s: negada");
    OK(ppres_may_reply(1000, 1000 + PPRES_REPLY_MIN_MS), "depois de 20 s volta a responder");
    printf(fails ? "%d FALHA(S)\n" : "tudo certo\n", fails);
    return fails ? 1 : 0;
}
