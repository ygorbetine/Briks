/* Abertura animada do Briks: WebM (VP9 com canal alfa) + trilha em WAV, tocada uma vez ao abrir o app (ver intro.c). */
#ifndef BRIKS_INTRO_H
#define BRIKS_INTRO_H
#include <stdbool.h>
#include "cui/cui.h"

/* Desenha a abertura por cima de tudo (chamar no fim de cada quadro). Começa sozinha no primeiro quadro, se estiver ligada e os arquivos
 * existirem; um clique, Esc, Espaço ou Enter pulam. `enabled` vem das Configurações. Não faz nada em testes automatizados (a menos que
 * BRIKS_INTRO_AT=<ms> congele um instante para captura) nem quando o app abre minimizado na bandeja. */
void intro_draw(cui_ctx *ui, bool enabled, bool start_minimized);
bool intro_active(void);
/* Probes de teste: 0 = ainda não começou, 1 = tocando, 2 = terminou/pulada; quadros decodificados; largura. */
int intro_probe(const char *name);
#endif
