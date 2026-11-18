# Plano 1.5.28: qualidade e performance da transmissão

Pedido (2026-11-17): 480p como opção na transmissão e estudo de qualidade/performance. Nada aqui é publicado sem ok explícito
do usuário (build, tag e release só com autorização). Tudo precisa funcionar em SFU e em P2P.

## Regras de execução (para não errar)
1. Uma frente por vez, cada uma em um commit próprio no `briks-dev`, com teste antes do commit.
2. Mudança de comportamento só entra com teste que a prove (unitário no core, roteiro `scripts/paridade` na UI).
3. Medir antes de mexer em desempenho: sem número, a mudança fica só como hipótese neste documento.
4. As ferramentas de vídeo de demonstração (`overlay.c` modo `--film` e gancho `BRIKS_DEMO_VIDEO`, só ativas por argumento/variável) foram juntas no commit do 480p porque dividem arquivo; `sysmodel.h` de sons fica fora até o usuário decidir os sons.
5. Ao fim: rodar todos os testes (core, C, roteiros), listar o que NÃO foi verificado, e esperar o ok para publicar.

## Frente A: 480p (entrega certa)
- [x] UI: `RES_H`/`RES_LABEL` com 480p na frente (`ui_share.c`); índices nomeados (`RES_720`, `RES_1080`).
- [x] Não há migração: a resolução escolhida não é gravada em disco (checado em `settings.c`); o estado é refeito a cada sessão.
- [x] Escada de adaptação: 480 entra em `HEIGHTS` (`adapt.rs`), entre 540 e 360.
- [x] Roteiro `transmissao-qualidade.txt` ajustado aos novos índices; `share-dlgq.txt` continua válido (1º segmento = 480p).
- [x] Testes: core (`four_eighty_p_is_a_valid_ceiling_and_a_step_below_720p`, 23 de adapt passam) e roteiro `transmissao-qualidade` passam.
- [x] Seletor visto em captura: 480p | 720p | 1080p | 1440p | 4K cabe no painel. Falta ver na transmissão real, SFU e P2P.

## Frente B: medir antes de decidir (sem mudar comportamento)
Medir numa transmissão real de 2 PCs, 1080p30 e 1440p60: CPU/GPU de quem transmite, tempo do encode, bitrate enviado x pedido,
PLI e NACK por minuto, encoder (hardware ou software), decode do espectador.
- [ ] (precisa de 2 PCs e GPU; `scripts/medir-transmissao.ps1` mede CPU e uso de GPU/codificador) Ler a telemetria que já existe (`quality_chip`, contadores de NACK/PLI em `adapt.rs`) e registrar os números aqui.
- [x] Captura: já converte BGRA para NV12 e reduz na GPU (`gpuconv.rs`); só o quadro reduzido volta para a memória (`capture.rs`, `BRIKS_CONVERT=cpu` força o caminho antigo). Candidato 4 descartado, a menos que a medição real mostre cópia pela CPU.
- [ ] Verificar o custo da redução no receptor com duas transmissões (dois monitores).

## Frente C: candidatos (cada um só vira tarefa com número da frente B)
1. Prioridade nitidez x fluidez: hoje o fps nunca cai e a resolução desce até 360p; opção de segurar resolução e abaixar o fps.
2. Simulcast no SFU (várias camadas por transmissão): maior ganho para espectadores diferentes, maior custo de encode.
3. Hardware no encode e no decode: medir quantos PCs caem em software.
4. ~~Captura e conversão 100% na GPU em 1440p/4K.~~ Já é assim (ver frente B).
5. Quadros-chave: intra refresh no encoder de hardware para suavizar picos (sondado em `mfenc.rs`).
6. Redução no receptor com duas transmissões.

## Critério de pronto da 1.5.28
Frente A completa e testada; frente B com números; frente C com uma decisão por item (fazer, adiar ou descartar).
