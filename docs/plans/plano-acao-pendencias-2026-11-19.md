# Plano de ação das pendências (2026-11-19, depois da 1.6.3)

Fonte: notas de todas as conversas até a 1.6.3. Regras valem para todas as sessões: **não publicar versão nenhuma** (só com ok explícito do dono), commitar só na própria branch, não mexer em arquivos de outra sessão, funcionar nos modos Direto e Servidor, textos do app em português.

**Aviso de segurança:** o PC do dono deu tela azul (BSOD) durante testes. Até a sessão `bsod-testes` achar o culpado, ninguém roda a suíte inteira de paridade nem testes que usam captura de tela/GPU/janelas reais (`BRIKS_TOAST_REAL`, `BRIKS_NUDGE_REAL`, overlay-live, compartilhar tela de verdade). Rodar só filtros específicos do que mudou.

O relato antigo de "tela verde/azul na transmissão" era essa BSOD, não um bug de vídeo: nada a corrigir no decodificador por causa dele.

## Sessões

| Sessão | Itens | Arquivos principais |
| --- | --- | --- |
| `rnnoise` | Supressão de ruído por rede neural (RNNoise) como 3ª opção em Configurações > Voz | `briks-core/src/audio.rs`, `Cargo.toml`, `src/settings.c` |
| `assistir-live` | "Assistir" na lista de membros sem estar na sala; botão Assistir do aviso de live clicável; prazo no "Iniciando…" da transmissão | `src/ui_share.c`, `src/ui_groups.c`, `src/rtc.c`, `briks-core/src/share.rs` |
| `modo-token-sfu` | Copiar o modo (Direto/Servidor) da sala ao entrar; token novo a cada queda do Servidor (limite 30/10 min); conferir "transmissão só aparece depois de parar/retomar" e "volume da transmissão sumiu" | `briks-core/src/sfu.rs`, `ipc.rs`, `mesh.rs`, `src/rtc.c` |
| `bsod-testes` | Achar qual teste derrubou o PC; proteger os testes de risco; análise do minidump | `scripts/paridade*`, `scripts/prova`, harness de testes |
| `testes-defasados` | Teste A/V (595 ms contra 250 ms), `share-volume-controle`, 8 testes de paridade defasados (auth ×4, overlay-live ×3, call-ensurdecer) | `scripts/paridade/*`, `tests/*`, testes do core |
| `auditoria` | Só leitura: conferir no código o que já foi feito e o que falta (ícones só no hover, overlay, 2 monitores, silenciar amigo, status na lista de membros, convite) | nenhum (relatório) |

## Depende do dono (sem sessão)
- Aplicar a migration `0013_reports.sql` no Supabase.
- Defender: enviar as versões à Microsoft como falso positivo; assinatura de código a longo prazo.
- SFU próprio: VPS Oracle, DNS no Wix, `instalar.sh` e `scripts/ativar-sfu.ps1` (guia em `infrastructure/livekit/README.md`).
- Testes em 2 PCs de tudo da 1.5/1.6, incluindo o cartão do Windows da 1.6.3.
- Decidir se "Desconectar P2P" do chat vira "chat direto"; sons de tijolo (patch guardado em `apps/desktop-cui/shots/sysmodel-sons-tijolo.patch`).

## Adiado de propósito
Modo Jogo; Cloudflare Realtime SFU (plano B); medir transmissão em 2 PCs.
