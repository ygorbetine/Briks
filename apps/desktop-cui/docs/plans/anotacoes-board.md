# Anotações nas transmissões e Board da sala

Status: desenho + implementação (fora do escopo fechado da 1.6; só entra em versão com ok do usuário).

## 1. Transporte (decisão)

O painel de sons já resolve "evento de sala que vale em SFU e P2P": o core tem um módulo de lógica pura (`sonsnet.rs`) que
recebe `(par autenticado, tipo, JSON)` e devolve `NetOut::Send{to,kind,body}` / `NetOut::Ev`; `mesh.rs` (DataChannel P2P) e
`sfudata.rs` (data channels do SFU) só põem o envelope (`type/version` na malha, `t/v` no SFU). O híbrido herda os dois.
Anotações seguem exatamente esse padrão:

* `briks-core/src/annot.rs`: toda a lógica (validação, limites, mesclagem, permissão, limite de taxa, snapshot). Testável sem rede.
* Tipos de mensagem de fio: `annot.ops` (lote de operações), `annot.laser`, `annot.perm`, `annot.want` (pede o estado), `annot.snap` (pedaço do estado).
* Comando UI→core: `{"cmd":"annot.send","kind":..}` vira `MeshCmd::Annot{body}`. Eventos core→UI reaproveitam `MeshEvent::Sound(Value)`
  (já repassado inteiro pelo ipc) com `ev` = `annot.ops | annot.laser | annot.perm | annot.denied | annot.full`: nenhuma variante nova de evento.
* Ganchos nos arquivos do core: `mesh.rs` (despacho do tipo + comando), `sfudata.rs` (idem), `sfu.rs` (lista de comandos que vão para a camada de dados),
  `ipc.rs` (um braço de comando), `main.rs` (`mod annot`). Nenhuma mídia/DTLS é tocada.
* Por que não `chat.event`: ele é assinado, persistido e sincronizado como histórico de chat, com semântica de mensagem; traço de desenho é efêmero, volumoso e de sala.
* Por que não uma faixa nova: DataChannel confiável e ordenado já existe nos dois modos e já autentica o par.

Autor = identidade provada do canal (fp do par autenticado), nunca um campo do JSON. Operações NÃO são assinadas uma a uma
(custo e tamanho a 20 Hz); consequência aceita: no snapshot reenviado por um terceiro, as operações de outros autores valem pela palavra
de quem reenviou (todos na sala são autenticados e amigos/convidados). Fica documentado como limitação.

## 2. Modelo de dados

Escopos (`scope`): `b:<0..15>` = página do Board; `s:<fp do dono>:<slot 0|1>` = anotação sobre a transmissão de alguém.
Coordenadas normalizadas inteiras 0..10000 (imagem inteira = 0..10000), valem em qualquer tamanho/zoom/resolução.
Cada operação: `{i, c, k, ...}` — `i` contador por autor, `c` relógio lógico de Lamport, tipo `k`:

| k | conteúdo |
|---|----------|
| `s` | traço: `m` (0 caneta, 1 marca-texto), `col` RGB, `w` espessura (1..2000 décimos de milésimo), `p` pontos x,y alternados |
| `p` | acréscimo de pontos ao traço `i` do mesmo autor (envio ao vivo) |
| `g` | forma: `sh` l/a/r/e, `col`, `w`, `f` (preenchida), `x0 y0 x1 y1` |
| `t` | texto curto (≤ 120 caracteres): `col`, `sz`, `x`, `y`, `s` |
| `d` | apagar a operação `tg`=`{a: fp, i}` (tombstone) |
| `x` | limpar: `all` (só dono do escopo) ou só as minhas, vale para operações com relógio menor |

Mesclagem sem conflito: chave `(autor, i)`; `d` e `x` são lápides que não ressuscitam (chegada fora de ordem é tratada);
ordem de desenho = `(c, autor, i)`. Desfazer = `d` das minhas; refazer = reenviar o corpo com id novo. Apagar por área parte o
traço: `d` + novos traços.

Limites (core valida, UI respeita): 4000 operações vivas por escopo; 2000 pontos por traço; 200 000 pontos por escopo; 24 escopos;
mensagem ≤ 48 KB e ≤ 200 operações; texto ≤ 120; cores/espessuras em faixa; coordenadas fora de 0..10000 descartam a operação inteira.
Taxa por autor: 60 mensagens/s, 600 operações/s e 8000 pontos/s (excedente descartado), laser 30/s. `annot.want` no máx. 1 a cada 5 s por par;
`annot.snap` só vale se eu pedi há menos de 30 s (limite de 64 pedaços). Nenhuma alocação proporcional ao que o par diz sem teto.

Envio ao vivo: a UI manda lotes a ~20 Hz (`s` com os primeiros pontos, depois `p`); ponto novo só se andou ≥ ~0,15% da imagem; Ramer-Douglas-Peucker
no lote ao soltar o botão não é possível sem reenviar, então o RDP roda no traço local antes de renderizar/exportar e no snapshot (UI) — ver §5.

## 3. Permissões

* "Só eu": nada vai à rede; qualquer um.
* "Para todos" em transmissão: o dono (`s:<fp>`) sempre; outros só se o dono liberou (`annot.perm {all, fps[]}`, vale só de quem é o dono do escopo, ≤ 32 fps).
  Padrão: ninguém além do dono. Dono apaga/limpa tudo; os outros só apagam/limpam os seus. Os traços somem ao fim da transmissão (a UI manda `drop` e o core esquece o escopo).
* Board: todos da sala desenham e apagam (quadro colaborativo); "limpar" da página apaga tudo daquela página.
* O core recusa localmente quem tenta enviar sem permissão (`annot.denied`).

## 4. Quem entra depois

Ao um par ficar autenticado a UI manda `annot.want`; qualquer par que tenha estado responde `annot.snap` em pedaços (≤ 150 ops), mais o `annot.perm` se for dono.
O estado do que já foi apagado não é reenviado (só ops vivas); lápides tardias são tratadas por chave.

## 5. UI (C, `src/annot.c`, `src/board.c`)

* Modelo único (`annot.h`) usado pelo Board e pelas camadas de transmissão: coleção de operações por escopo, ordenada, com cache de geometria suavizada
  (Catmull-Rom → polilinha) só recalculada quando muda; o canvas só repinta o que o cui já pinta por quadro — custo é desenhar polilinhas, sem alocar por quadro.
* Renderer: `cui_draw_polyline`, `cui_draw_polygon` (convexo: retângulo/elipse preenchidos), `cui_draw_circle`, `cui_draw_text`; funciona no software e no OpenGL.
  Marca-texto = cor com alfa; sobreposição de segmentos do mesmo traço pode escurecer nas juntas (limitação do desenho por polilinha, aceita).
* Camada transparente sobre a imagem do tile: uma `cui_canvas` no mesmo retângulo da imagem (já com zoom/pan aplicados): conversão normalizado ↔ tela usa o retângulo
  da imagem visível. Com a ferramenta ativa o arrasto desenha (e não faz pan); sem ferramenta a camada só mostra.
* Barra flutuante compacta ("Anotar" no tile, tecla A): ferramentas, paleta de 8 cores + seletor RGB, 3 espessuras + controle, preenchimento, modo Só eu/Para todos,
  desfazer/refazer, limpar, e (dono) liberar desenho. Laser some em ~1 s.
* Board: aba "Board" na chamada (mudança mínima em `ui_call.c`), páginas 1..8, fundo claro/escuro, pan (botão do meio/espaço+arrasto) e zoom (roda), exportar PNG.
  Exportação: rasterizador próprio em CPU (traços como cápsulas, formas, texto com fonte bitmap 5x7 sem acentos), sem rede, arquivo escolhido na janela "Salvar".
  Janela destacável: não implementada nesta etapa (o cui nativo deste app tem uma janela só; ver relatório).
* Se a transmissão for pausada/encerrada: `drop` do escopo.

## 6. Testes

`cargo test -p briks-core annot`: validação, limites, mesclagem em ordem trocada, permissão, taxa, snapshot, tombstone, escopo, fuzz de entrada.
Roteiros `scripts/paridade/anot-*.txt` (coreev + capturas). Dois PCs: só se vê de verdade o tempo real, a latência dos traços e o snapshot de quem entra depois.
