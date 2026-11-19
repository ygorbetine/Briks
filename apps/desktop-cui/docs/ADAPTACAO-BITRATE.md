# Bitrate adaptativo da transmissão (briks-core/src/adapt.rs)

## Como funciona hoje
- **Estimador** (`Estimator`, uma vez por segundo): AIMD próprio, porque o webrtc-rs 0.12 não traz goog-cc. Sinais: perda dos
  Receiver Reports (chegam 1–2 s atrasados), RTT acima da base (fila enchendo) e pacotes retransmitidos (NACK, do último segundo).
  Corta para ~90% do que passou (80% quando a perda passa de 35%), não em cascata; sobe 25%/s abaixo de metade do teto e 10%/s
  acima. Perda aleatória (Wi-Fi) sem aumento de atraso não derruba o bitrate (corte de sondagem de 20% + tolerância).
- **Memória da capacidade**: depois de um corte, o teto observado fica firme por 3 s. **Novo (1.6):** se a estimativa voltar a
  bater no mesmo teto (4–40 s depois, vindo de menos de 1,6× o teto antigo), o tempo firme dobra (até 12 s). Numa rede que
  não mudou, isso troca o dente de serra de ~10 s (estoura, corta, recria o codificador) por uma sondagem rara.
- **Controlador**: escada de resolução (FPS fixo no alvo), queda de nível na hora, subida só com 30% de folga por 3 s,
  castigo contra vaivém, olhar da CPU. Cada troca de resolução recria o codificador.
- **Codificador** (`share.rs`): recria só em queda para menos de 70% do bitrate atual e no máximo a cada 2 s; o resto vai ao vivo.

## Medições (simulação em malha fechada: `cargo test medida -- --nocapture`)
Modelo: fila de 25% da capacidade, relatórios 2 s atrasados, codificador obedece com 1 s de atraso, escada de 1080p30 (teto 6 Mbps).

| Cenário | Antes | Depois |
|---|---|---|
| Queda 20→2 Mbps: segundos até o envio caber no enlace | 4 | 1 |
| Queda 20→2 Mbps: segundos de estouro (>110%) | 8 | 6 |
| Volta 2→20 Mbps: segundos até 90% do possível | 11 | 8 |
| Escada 6→3→1,5→0,8→3→6: estouro total nas quedas | 18 s | 10 s |
| Escada: volta de 0,8 para 3 Mbps até 90% | 16 s | 12 s |
| Escada: utilização 3 / 1,5 / 0,8 Mbps | 92 / 92 / 92% | 94 / 88 / 90% |
| 3% de perda aleatória em 4 Mbps: utilização / recriações em 40 s | 85% / 5 | 99% / 1 |
| 8% de perda aleatória em 4 Mbps | 90% / 5 | 89% / 5 |
| Quedas de 5 s a cada 25 s (D): utilização / recriações | 71% / 12 | 71% / 12 (sem mudança) |
| Rampa 1→6 Mbps em 60 s: utilização | 87% | 86% |

Ganhos reais: reação à queda e menos estouro; menos oscilação em enlace estável. Sem ganho: o caso das quedas curtas
repetidas (D), onde a volta leva ~15 s e há recriações do codificador a cada queda/volta de nível.
A simulação não prova qualidade percebida nem o comportamento do NVENC real: isso só se valida em 2 PCs.

## Tentado e descartado (os testes existentes reprovaram ou não houve ganho)
- Teto da capacidade lembrada até 1,6× (em vez de 1,3×): recuperação mais lenta, reprovou `tight_lossy_link_…`.
- Esperar 6 s entre degraus de resolução em sequência: não mudou o número de recriações (elas vêm do dente de serra) e quebrou o teste de vaivém.
- Ignorar o corte por atraso enquanto o RTT cai (fila esvaziando): melhora utilização (escada 95%, recriações 11→7), mas deixa a média
  5,7% acima do enlace de 2 Mbps (reprovou o teste de enlace apertado). Candidato a revisitar com um teste com buffer raso.
- Rampa 1,35×/s na volta de queda curta: sem ganho claro, mais recriações.

## Propostas (não implementadas, precisam de ok)
1. **Estimador por TWCC** (atraso de chegada por pacote, estilo goog-cc): o webrtc-rs só gera o feedback do lado receptor; o
   Briks teria de ler os TransportCCs que o espectador devolve e rodar o filtro de gradiente de atraso. Feedback a cada ~100 ms em
   vez de 1 s: reage em ~300 ms e quase some o estouro depois da queda. Esforço: 4–6 dias (parser TWCC no mesh/SFU, filtro de
   trendline, testes de rede simulada, ajuste fino). Risco médio: substitui o sinal principal; entra atrás de chave, com o AIMD atual como reserva.
2. **Simulcast/SVC no SFU** (2–3 camadas por transmissão; cada espectador recebe a camada que cabe): resolve de vez "um espectador
   ruim derruba todos". Esforço: 2–3 semanas (codificação em várias resoluções = mais GPU/CPU no emissor, negociação de camadas,
   seleção por espectador no sfu.rs, UI). Só vale com 3+ espectadores comuns; hoje a regra da maioria cobre o caso simples.
3. **FEC adaptativo** (já existe `fec.rs`): ligar redundância proporcional à perda medida em vez de cortar bitrate nas perdas moderadas; esforço 2–3 dias.
4. **Quedas curtas (cenário D)**: segurar a resolução por ~2 s antes de baixar (o bitrate já cai sozinho) tiraria um par de recriações por queda de <3 s; precisa de teste com o app real.
