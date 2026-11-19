/* Cobertura de transmissões de desconhecidos e registro de denúncias (src/safety.c). Sem janela. */
#include "../src/safety.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>
static int fails;
#define OK(c, name) do { if (!(c)) { fails++; printf("FALHOU: %s\n", name); } else printf("ok: %s\n", name); } while (0)

int main(void) {
    char hf[MAX_PATH]; GetTempPathA(sizeof hf, hf); strcat(hf, "briks_ocultos_test.txt"); DeleteFileA(hf); _putenv_s("BRIKS_HIDDEN_FILE", hf);
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
    snprintf(s.rtc_peers[0].id, sizeof s.rtc_peers[0].id, "peer-z"); snprintf(s.rtc_peers[0].fp, sizeof s.rtc_peers[0].fp, "fp-outro");
    OK(!safety_covered(&s, 0), "outra pessoa no mesmo slot não herda o Parar de ver");

    snprintf(s.rtc_peers[0].fp, sizeof s.rtc_peers[0].fp, "fp-amigo");
    /* QoL-5: amigo que parei de ver volta escondido na próxima chamada (outro id de sala, mesma impressão digital); desconhecido nunca é lembrado */
    safety_slot_reset(0); snprintf(s.rtc_peers[0].id, sizeof s.rtc_peers[0].id, "peer-a");
    safety_hide(&s, 0, true); safety_hide(&s, 1, true);
    safety_slot_reset(0); safety_slot_reset(1);
    snprintf(s.rtc_peers[0].id, sizeof s.rtc_peers[0].id, "peer-nova-chamada"); snprintf(s.rtc_peers[1].id, sizeof s.rtc_peers[1].id, "peer-estranho-2"); snprintf(s.rtc_peers[1].fp, sizeof s.rtc_peers[1].fp, "fp-estranho");
    OK(safety_hidden(&s, 0), "amigo parado de ver continua escondido na próxima chamada");
    OK(!safety_hidden(&s, 1), "desconhecido não é lembrado entre chamadas");
    { char buf2[512] = {0}; FILE *g = fopen(hf, "rb"); size_t k = g ? fread(buf2, 1, sizeof buf2 - 1, g) : 0; if (g) fclose(g); (void)k; OK(strstr(buf2, "fp:fp-amigo") && !strstr(buf2, "estranho"), "arquivo guarda só a identidade do amigo"); }
    safety_reveal(&s, 0);
    safety_slot_reset(0); snprintf(s.rtc_peers[0].id, sizeof s.rtc_peers[0].id, "peer-outra-vez");
    OK(!safety_hidden(&s, 0), "Voltar a ver também esquece o amigo");
    snprintf(s.rtc_peers[2].id, sizeof s.rtc_peers[2].id, "peer-c"); snprintf(s.friends[1].id, sizeof s.friends[1].id, "u2"); s.friend_count = 2;
    safety_hide(&s, 2, true);   /* peer-c ainda não é amigo */
    safety_slot_reset(2); snprintf(s.rtc_peers[2].id, sizeof s.rtc_peers[2].id, "u2");
    OK(safety_is_friend(&s, 2) && !safety_hidden(&s, 2), "quem virou amigo depois não herda o Parar de ver de quando era desconhecido");
    safety_hide(&s, 2, true); safety_slot_reset(2); snprintf(s.rtc_peers[2].id, sizeof s.rtc_peers[2].id, "u2");
    OK(safety_hidden(&s, 2), "amigo sem impressão digital é lembrado pelo id da conta");
    DeleteFileA(hf);

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
