/* Cobertura de transmissões de desconhecidos e registro de denúncias (src/safety.c). Sem janela. */
#include "../src/safety.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>
static int fails;
#define OK(c, name) do { if (!(c)) { fails++; printf("FALHOU: %s\n", name); } else printf("ok: %s\n", name); } while (0)

int main(void) {
    static app_state_t s; memset(&s, 0, sizeof s);
    snprintf(s.rtc_peers[0].id, sizeof s.rtc_peers[0].id, "peer-a"); snprintf(s.rtc_peers[0].fp, sizeof s.rtc_peers[0].fp, "fp-amigo");
    snprintf(s.rtc_peers[1].id, sizeof s.rtc_peers[1].id, "peer-b"); snprintf(s.rtc_peers[1].fp, sizeof s.rtc_peers[1].fp, "fp-estranho");
    snprintf(s.friends[0].id, sizeof s.friends[0].id, "u1"); snprintf(s.friends[0].fp, sizeof s.friends[0].fp, "fp-amigo"); s.friend_count = 1;

    OK(safety_is_friend(&s, 0) && !safety_is_friend(&s, 1), "amigo reconhecido pelo fp; desconhecido não");
    OK(!safety_covered(&s, 0) && !safety_covered(&s, 1), "nenhuma transmissão é coberta sozinha (amigo ou não)");
    OK(!safety_covered(&s, 2), "slot vazio não é coberto");
    safety_hide(&s, 1, true);
    OK(safety_covered(&s, 1) && safety_hidden(&s, 1), "Parar de ver esconde");
    safety_reveal(&s, 1);
    OK(!safety_hidden(&s, 1) && !safety_covered(&s, 1), "Voltar a ver");
    safety_hide(&s, 0, true);
    OK(safety_covered(&s, 0), "Parar de ver vale também para amigo");
    snprintf(s.rtc_peers[0].id, sizeof s.rtc_peers[0].id, "peer-z");
    OK(!safety_covered(&s, 0), "outra pessoa no mesmo slot não herda o Parar de ver");

    char dir[MAX_PATH]; GetTempPathA(sizeof dir, dir); strcat(dir, "briks_safety_test"); CreateDirectoryA(dir, NULL);
    char f[MAX_PATH]; snprintf(f, sizeof f, "%s\\denuncias.jsonl", dir); DeleteFileA(f);
    OK(safety_report_write(dir, "peer-b", "Fulano \"X\"", "violencia", true), "grava a denúncia");
    OK(safety_report_write(dir, "peer-c", "Ciclano", "odio", false), "grava a segunda denúncia");
    char buf[1024] = {0}; FILE *fp = fopen(f, "rb"); size_t n = fp ? fread(buf, 1, sizeof buf - 1, fp) : 0; if (fp) fclose(fp);
    int lines = 0; for (size_t i = 0; i < n; i++) if (buf[i] == '\n') lines++;
    OK(lines == 2 && strstr(buf, "\"motivo\":\"violencia\"") && strstr(buf, "\"bloqueado\":true") && strstr(buf, "Fulano \\\"X\\\""), "registro: uma linha por denúncia, com motivo, bloqueio e aspas escapadas");
    OK(safety_reason_count() == 5 && !strcmp(safety_reason_key(0), "violencia"), "motivos disponíveis");
    DeleteFileA(f); RemoveDirectoryA(dir);
    printf(fails ? "\n%d falha(s)\n" : "\ntudo certo\n", fails);
    return fails ? 1 : 0;
}
