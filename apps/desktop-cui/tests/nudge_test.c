/* "Chamar atenção": limites de envio/recebimento, Não perturbar e decisão do efeito (src/nudge_model.h). Sem janela. */
#include "../src/nudge_model.h"
#include <stdio.h>
static int fails;
#define OK(c, name) do { if (!(c)) { fails++; printf("FALHOU: %s\n", name); } else printf("ok: %s\n", name); } while (0)
int main(void) {
    static nm_limiter l;
    OK(nm_allow(&l, "ana", 1000), "primeiro envio passa");
    OK(!nm_allow(&l, "ana", 1000 + 14999), "mesmo amigo antes de 15 s: negado");
    OK(!nm_allow(&l, "ana", 1000 + 14999 + 1 - 1), "negado não renova o prazo");
    OK(nm_allow(&l, "bia", 2000), "outro amigo passa");
    OK(nm_allow(&l, "ana", 1000 + 15000), "mesmo amigo depois de 15 s passa");
    static nm_limiter g; int ok = 0; char k[16];
    for (int i = 0; i < 10; i++) { snprintf(k, sizeof k, "u%d", i); if (nm_allow(&g, k, 100000ULL + (unsigned)i * 1000)) ok++; }
    OK(ok == NM_GLOBAL_MAX, "no máximo 6 por minuto no total");
    OK(nm_allow(&g, "novo", 100000ULL + 61000), "passada a janela de 1 min volta a passar");
    OK(!nm_allow(&g, "", 5) && !nm_allow(&g, NULL, 5), "chave vazia é recusada");
    OK(nm_dnd_active(-1, 5) && !nm_dnd_active(0, 5) && nm_dnd_active(100, 99) && !nm_dnd_active(100, 100), "Não perturbar: sempre, desligado, por tempo");
    OK(nm_dnd_for(30, 1000) == 2800 && nm_dnd_for(0, 1000) == -1, "duração: 30 min = +1800 s; 0 = até desligar");
    nm_ctx c = { .recv_on = true, .mode = NM_MODE_SCREEN };
    OK(nm_decide(&c) == NM_SHAKE_SCREEN, "tela inteira com o Briks em foco");
    c.mode = NM_MODE_WINDOW; OK(nm_decide(&c) == NM_SHAKE_WINDOW, "só a janela");
    c.maximized = true; OK(nm_decide(&c) == NM_NOTICE, "só a janela, maximizada: só aviso");
    c.maximized = false; c.mode = NM_MODE_NOTICE; OK(nm_decide(&c) == NM_NOTICE, "modo só aviso");
    c.mode = NM_MODE_SCREEN; c.background = true; OK(nm_decide(&c) == NM_FLASH, "em segundo plano: pisca, sem tremer");
    c.in_tray = true; OK(nm_decide(&c) == NM_NOTICE, "na bandeja: só aviso");
    c.in_tray = false; c.background = false; c.os_busy = true; OK(nm_decide(&c) == NM_NONE, "jogo/apresentação: nada");
    c.os_busy = false; c.dnd = true; OK(nm_decide(&c) == NM_NONE, "Não perturbar: nada");
    c.dnd = false; c.recv_on = false; OK(nm_decide(&c) == NM_NONE, "receber desligado: nada");
    int mx = 0, last = 0; for (int f = 0; f < 60; f++) { int o = nm_offset(20, f, 60, (unsigned)(f * 7919)); if (o < 0) o = -o; if (f < 10 && o > mx) mx = o; if (f >= 58) last = o; }
    OK(mx <= 20 && last <= 1 && nm_offset(20, 60, 60, 123) == 0, "deslocamento decrescente até zero");
    printf(fails ? "%d falha(s)\n" : "tudo certo\n", fails); return fails != 0;
}
