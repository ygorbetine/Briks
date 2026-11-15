#include "safety.h"
#include "call.h"
#include "storage.h"
#include "core.h"
#include "json.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

static bool g_open; static int g_slot = -1, g_reason = 0; static bool g_block = true;
void safety_report_open(int slot) { g_slot = slot; g_reason = 0; g_block = true; g_open = true; }

void safety_report_dialog(cui_ctx *ui, app_state_t *s) {
    if (!g_open) return;
    if (g_slot < 0 || g_slot >= BRIKS_MAX_PEERS || !s->rtc_peers[g_slot].id[0]) { g_open = false; return; }   /* a pessoa saiu */
    const char *who = s->rtc_peers[g_slot].name[0] ? s->rtc_peers[g_slot].name : "esta pessoa";
    cui_dialog(ui, "Denunciar", &g_open, .width = 420) {
        cui_textf(ui, "Denunciar %s", who);
        cui_text(ui, "Escolha o motivo. A denúncia é enviada ao Briks com um quadro da transmissão (se houver) e fica registrada neste computador.", .size = 13, .color = B_MUTED, .width = CUI_FILL);
        for (int i = 0; i < safety_reason_count(); i++) {
            cui_push_id_int(ui, 900 + i);
            bool sel = g_reason == i;
            if (cui_button(ui, safety_reason_label(i), .variant = sel ? CUI_BUTTON_PRIMARY : CUI_BUTTON_SECONDARY, .size = CUI_SIZE_SM, .width = CUI_FILL)) g_reason = i;
            cui_pop_id(ui);
        }
        cui_checkbox(ui, "Também silenciar e parar de ver nesta chamada", &g_block);
        cui_row(ui, .gap = 10) {
            if (cui_button(ui, "Cancelar")) g_open = false;
            if (cui_button(ui, "Denunciar", .variant = CUI_BUTTON_DANGER)) {
                char data[MAX_PATH], hist[MAX_PATH], recv[MAX_PATH]; storage_dirs(data, hist, recv, sizeof data);
                safety_report_write(data, s->rtc_peers[g_slot].id, s->rtc_peers[g_slot].name, safety_reason_key(g_reason), g_block);
                safety_hide(s, g_slot, true);
                {   /* o servidor recebe a denúncia com um quadro da transmissão (se a pessoa está transmitindo); sem o servidor, fica só o registro local */
                    char fp[160], nm[200], rm[160], fr[640], line[1400];
                    json_escape(s->rtc_peers[g_slot].fp, fp, sizeof fp); json_escape(s->rtc_peers[g_slot].name, nm, sizeof nm); json_escape(s->room_name, rm, sizeof rm);
                    const char *fpath = s->sh.remote[g_slot].on && s->sh.remote[g_slot].path[0] ? s->sh.remote[g_slot].path : (s->sh.remote2[g_slot].on ? s->sh.remote2[g_slot].path : "");
                    json_escape(fpath, fr, sizeof fr);
                    snprintf(line, sizeof line, "{\"cmd\":\"report.submit\",\"fp\":\"%s\",\"name\":\"%s\",\"reason\":\"%s\",\"room\":\"%s\",\"frame\":\"%s\"}", fp, nm, safety_reason_key(g_reason), rm, fr);
                    core_send(line);
                }
                if (g_block) rtc_mute_peer(s, g_slot, true);
                call_toast("Enviando a denúncia…");
                g_open = false;
            }
        }
    }
}
