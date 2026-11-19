# Tela azul (BSOD) durante testes: triagem, como ler o minidump e proteções

Data: 2026-11-19. Branch `bsod-testes`. O PC do dono desligou com tela azul durante testes; existe o minidump `C:\Windows\Minidumps\111826-10828-01.dmp` (18/11/2026). Ler esse arquivo exige administrador, então **o dono precisa rodar os comandos da seção 2**. Tudo aqui foi feito **por leitura**: nenhum teste de GPU, captura ou janela real foi executado.

## 1. Triagem por leitura (o que pode derrubar o driver de vídeo)

Já estava anotado no repositório que "a máquina já teve tela azul com NVENC" (`docs/plans/plano-pos-1.3.0.md`, `transmissao-boas-praticas.md`, `briks-core/src/robust.rs`). O que as sessões faziam para se proteger era só um combinado (lock `A:\BRiskCall\.gpu-test.lock` e variáveis de ambiente), nada era imposto pelo código.

| # | Teste / harness | O que toca | Risco | Por quê |
|---|---|---|---|---|
| 1 | `cargo test` do `briks-core` **sem variáveis** (os ~25 testes de `tests.rs` que fazem `ShareStart` com `SourceKind::Test*`, `tests_rede`, `tests_sfu*`, `robust`) | Codificador H264 do Media Foundation **de hardware (NVENC/QSV/AMF)**: `H264Enc::new_with_qp` só cai para software se `BRIKS_ENCODER` estiver definido | **ALTO** | O `cargo test` roda vários testes em paralelo (threads) e cada um abre uma sessão de codificador de GPU; várias sessões das conversas rodando `cargo test` ao mesmo tempo no mesmo PC multiplicam isso. Nenhum teste de `tests.rs` define `BRIKS_ENCODER`. |
| 2 | `probe_real_pequeno_responde_com_reserva_de_software` (`probe.rs`, o "Testar meu PC" da 1.6, commit `30ee50e`, 18/11 23:29) | `probe::run` abre o MFT de hardware **de propósito** e codifica em laço por fatias de tempo (até 240 quadros), e logo depois roda VP8/VP9 em todos os núcleos | **ALTO** | É um teste comum (não `#[ignore]`), entra em toda `cargo test`, e foi criado na mesma noite da tela azul. |
| 3 | `gpuconv::tests::gpu_conversion_matches_the_cpu_one` e `bench_gpu_vs_cpu_conversion` | `D3D11CreateDevice` real + shader de computação | MÉDIO | Dispositivo D3D11 próprio; o primeiro roda em todo `cargo test`. |
| 4 | `real_screen_share_stats` (`tests.rs`, `#[ignore]`) | Captura real de tela (Windows Graphics Capture) + conversão na GPU + NVENC, 1080p60 | ALTO | Só com `--ignored`; a captura e o codificador de GPU ao mesmo tempo é a combinação mais pesada. |
| 5 | `cycles_mf_encoder_create_drop` com `BRIKS_MF_HW=1` (`robust.rs`, `#[ignore]`) | Cria/destrói o encoder de hardware em laço (60+ ciclos) | ALTO | Criar e soltar sessão de NVENC em laço é o padrão clássico de driver instável. |
| 6 | `BRIKS_SFU_TEST_GPU=1 cargo test sfu_screen` (`tests_sfu.rs`) | NVENC sobre o SFU | MÉDIO | Opt-in, mas só pelo combinado. |
| 7 | Casos de paridade com `BRIKS_TOAST_REAL=1` (`amigos-toast-janela-fluxo`) | Janela `WS_EX_TOPMOST` + `UpdateLayeredWindow` real, mesmo com o app "oculto" | MÉDIO | Cria janela real no topo da tela de quem roda. |
| 8 | `BRIKS_NUDGE_REAL=1` (`nudge.c`, "tremer a tela") | `BitBlt` do monitor inteiro com `CAPTUREBLT` + janela topmost em tela cheia que se redesenha a cada 15 ms | MÉDIO/ALTO | Copia a tela inteira para uma janela layered topmost do tamanho do monitor: estressa DWM/driver. Não aparece em nenhum caso do harness, mas a sessão de teste do nudge o usa. |
| 9 | Casos com `BRIKS_OVERLAY_FORCE=1` (`overlay-live`, `overlay-barra`, `overlay-quadro`, `overlay-aviso-transmissao`) e o roteiro `share-overlayreal.txt` (usa `win game`) | Overlay de janelas topmost/layered; `win game` cria janela topmost do tamanho da tela | BAIXO (hidden: o overlay fica em "dry run") / MÉDIO (`share-overlayreal`) | Com `BRIKS_HIDDEN` o `overlay_host.c` não abre janela real (`dry_run`), mas fica pelo menos como "janela" por prudência. |
| 10 | `scripts/prova/matriz.ps1` (3 tamanhos x 3 desempenhos x 5 layouts + 13 + perfil, ~230 execuções do app em sequência) | Muitas instâncias do app, renderização repetida | BAIXO/MÉDIO | Tinha prazo de 90 s mas matava só o app (não os filhos). |
| 11 | `prova/modal/rodar.ps1`, `prova/firewall/rodar.ps1` | App oculto; firewall usa `netsh` **falso** (`BRIKS_NETSH`) | BAIXO | Usavam `Start-Process -Wait` **sem prazo**: um travamento prendia o PC. Agora têm disjuntor. |
| 12 | `scripts/paridade.ps1` (109 casos, app oculto, `BRIKS_DEMO`) | Renderização do app, sem captura do monitor | BAIXO | Só vira risco por volume (várias sessões ao mesmo tempo) e pelos 8 casos perigosos acima. |
| 13 | `tests_camera`, `tests_vpx`, `tests_sfurec/rede`, `tests_hybrid`, `tests_rede` | Software (openh264/libvpx/mf-soft) | BAIXO | Já forçam software. |
| 14 | Testes de áudio WASAPI com captura por processo (`process_audio_reaches_the_viewer`, `#[ignore]`) | WASAPI process loopback, toca som | BAIXO p/ BSOD | Pode reter dispositivo de áudio; não é suspeito de tela azul. |

Padrões que travam driver de vídeo, encontrados: **(a)** várias sessões de encoder de hardware ao mesmo tempo (item 1); **(b)** criar e destruir encoder em laço (5); **(c)** captura + conversão D3D11 + NVENC ao mesmo tempo (4); **(d)** janela topmost do tamanho do monitor redesenhada a 15 ms (8); **(e)** `Start-Process -Wait` sem prazo (11). Não achei `SetWindowPos` em laço sem folga nos testes (o overlay chama uma vez por atualização).

### Provável culpado
**Item 1/2: o `cargo test` padrão abrindo o codificador de hardware (NVENC) em vários testes ao mesmo tempo, mais o teste novo do "Testar meu PC".** Grau de certeza: **médio (~60%)**, por circunstâncias: já havia tela azul anterior com NVENC; o `probe` foi criado em 18/11 à noite, mesma data do minidump; e na hora havia várias sessões rodando `cargo test`/paridade ao mesmo tempo. **Não está confirmado**: só o minidump diz (seção 2). O segundo candidato (~25%) é o tremor de tela real (`BRIKS_NUDGE_REAL`), também de 18/11 à noite; o resto é pouco provável.

## 2. Instruções para o dono (PowerShell como **Administrador**)

O dump é de 18/11/2026 (`111826` = mm/dd/aa). Primeiro copie-o para um lugar legível:

```powershell
New-Item -ItemType Directory -Force C:\Dumps | Out-Null
Copy-Item C:\Windows\Minidumps\111826-10828-01.dmp C:\Dumps\
Get-ChildItem C:\Windows\Minidumps | Sort-Object LastWriteTime | Select-Object Name, LastWriteTime, Length
```

**Passo A: eventos do Windows (rápido, já dá o código e a hora).**

```powershell
# o que o Windows registrou após o reinício: BugCheck com código de parada e parâmetros
Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='Microsoft-Windows-WER-SystemErrorReporting'; StartTime='2026-11-18'} | Select-Object TimeCreated, Id, Message | Format-List
# desligamento inesperado (Kernel-Power 41; o campo BugcheckCode é o código da tela azul em decimal)
Get-WinEvent -FilterHashtable @{LogName='System'; Id=41; StartTime='2026-11-18'} | Select-Object TimeCreated, @{n='BugcheckCode';e={$_.Properties[0].Value}}, @{n='Param1';e={'0x{0:X}' -f $_.Properties[1].Value}} | Format-Table -Auto
# erros de hardware (WHEA): se houver, é CPU/RAM/PCIe/GPU, não código do Briks
Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='Microsoft-Windows-WHEA-Logger'; StartTime='2026-11-18'} -ErrorAction SilentlyContinue | Select-Object TimeCreated, Id, Message | Format-List
# o driver de vídeo "parou de responder e foi recuperado" (TDR) nas horas antes
Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='Display'; StartTime='2026-11-18'} -ErrorAction SilentlyContinue | Select-Object TimeCreated, Id, Message | Format-List
Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='nvlddmkm'; StartTime='2026-11-18'} -ErrorAction SilentlyContinue | Select-Object TimeCreated, Id, Message | Format-List
```

Converter o código de parada decimal em hexadecimal: `'0x{0:X}' -f 278` (troque o número).

**Passo B: analisar o dump com o WinDbg (`cdb`).**

```powershell
winget install --id Microsoft.WinDbg --source winget
```

Se o `cdb.exe` não estiver no PATH, instale o "Debugging Tools for Windows" (Windows SDK) e use `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe`. Depois:

```powershell
$cdb = (Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe","$env:LOCALAPPDATA\Microsoft\WindowsApps\*\cdb.exe" -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
New-Item -ItemType Directory -Force C:\Symbols | Out-Null
& $cdb -z C:\Dumps\111826-10828-01.dmp -y "srv*C:\Symbols*https://msdl.microsoft.com/download/symbols" -c "!analyze -v; lm t n; q" | Out-File C:\Dumps\analise.txt -Encoding utf8
Select-String -Path C:\Dumps\analise.txt -Pattern 'BUGCHECK_CODE|BUGCHECK_STR|IMAGE_NAME|MODULE_NAME|FAILURE_BUCKET_ID|PROCESS_NAME|SYMBOL_NAME|STACK_TEXT' -Context 0,3
```

Com o WinDbg gráfico: abrir o `.dmp`, `.symfix; .reload`, `!analyze -v`. **Mande o `C:\Dumps\analise.txt` (ou só as linhas de `BUGCHECK_CODE`, `IMAGE_NAME`, `FAILURE_BUCKET_ID`, `PROCESS_NAME` e `STACK_TEXT`) para a conversa.** Sem informação pessoal importante ali, mas confira antes de enviar.

### O que procurar e como interpretar

| Dado | Significado | O que fazer |
|---|---|---|
| `IMAGE_NAME` / `MODULE_NAME` = `nvlddmkm.sys`, `amdkmdag.sys`, `igdkmd64.sys` | Driver de vídeo (NVIDIA/AMD/Intel) | Culpado = uso de GPU. Confirma a hipótese do NVENC/encoder de hardware. Atualizar o driver; manter os testes de GPU desligados. |
| `dxgkrnl.sys`, `dxgmms2.sys` | Agendador/kernel gráfico do Windows (o driver não respondeu) | Quase sempre é o driver da GPU por baixo; mesma conclusão. |
| `win32kfull.sys`, `win32kbase.sys`, `dwm.exe` como `PROCESS_NAME` | Subsistema de janelas / compositor | Aponta para janelas topmost/layered (overlay, tremer a tela, toast real). |
| `ntoskrnl.exe` sozinho, `MEMORY_CORRUPTION`, `WHEA_UNCORRECTABLE_ERROR` | Memória/CPU/hardware ou driver de terceiros corrompendo memória | Rodar `mdsched.exe` (teste de memória), checar WHEA do passo A; provavelmente não é o Briks. |
| `PROCESS_NAME` = `briks-core.exe`, `briks_cui.exe`, `cargo.exe`, `briks_core-*.exe` (binário de teste) | O processo que estava rodando quando caiu | Qual teste: o binário de teste do `briks-core` (itens 1-6) ou o app (7-9). Ajuda a decidir, mas o dono do bug continua sendo o driver. |

Códigos de parada mais prováveis aqui:

- **`0x116 VIDEO_TDR_FAILURE`** / **`0x117 VIDEO_TDR_TIMEOUT_DETECTED`**: o driver de vídeo travou e não se recuperou. Culpado = GPU (NVENC, captura, shader). É o resultado mais provável.
- **`0x113 VIDEO_DXGKRNL_FATAL_ERROR`**, **`0x119 VIDEO_SCHEDULER_INTERNAL_ERROR`**, **`0x10E VIDEO_MEMORY_MANAGEMENT_INTERNAL`**: variações de problema no driver/gerenciador de memória de vídeo.
- **`0x3B SYSTEM_SERVICE_EXCEPTION`** (com `win32kfull.sys`/`nvlddmkm.sys` na pilha): chamada de sistema falhou dentro de um driver. Olhe o módulo da pilha; com `win32k*` suspeite das janelas, com `nvlddmkm` da GPU.
- **`0xA IRQL_NOT_LESS_OR_EQUAL`** / **`0xD1 DRIVER_IRQL_NOT_LESS_OR_EQUAL`**: driver acessou memória inválida; o `IMAGE_NAME` diz qual.
- **`0x124 WHEA_UNCORRECTABLE_ERROR`**: hardware (CPU/RAM/PCIe), não é software.
- **`0x1A MEMORY_MANAGEMENT`**: RAM ou driver; testar memória.

Se o dump disser **driver da GPU**, a conclusão é: o Briks/testes só *expuseram* o bug do driver; manter os testes de hardware desligados no PC do dono e testar codificador de hardware só em máquina de teste. Se disser **`win32k`/`dwm`**, o suspeito vira o overlay/tremer a tela. Se disser **hardware**, não é culpa dos testes.

## 3. Proteções implementadas (nesta branch)

Princípio: **o padrão é seguro**. Nada que use GPU de verdade, janela topmost real ou aviso real do Windows roda sem pedido explícito.

1. **`BRIKS_TESTES_PERIGOSOS=1`** (ou `-Perigosos` nos scripts) libera os testes perigosos, e mesmo assim só se o **lock exclusivo da GPU** (`A:\BRiskCall\.gpu-test.lock`) estiver livre (`scripts/lib/seguranca.ps1`, `Resolve-Perigosos`). Sem o lock, continuam pulados.
2. **`briks-core` (testes Rust):** em `cargo test` (`cfg!(test)`), sem `BRIKS_TESTES_PERIGOSOS=1`, `H264Enc` **nunca abre o codificador de hardware**: usa o MFT de software da Microsoft (`mf-soft`) quando `BRIKS_ENCODER` não foi definido (`video.rs`). O "Testar meu PC" em teste roda sem hardware (`probe::run`); os testes de GPU (`gpuconv`) e o ciclo de encoder de hardware (`BRIKS_MF_HW`, `robust.rs`) viram "PULADO" sem a variável. O app de verdade **não muda** (tudo atrás de `cfg!(test)`). Compila: `cargo check --tests` passou.
3. **`scripts/testes-core.ps1`**: jeito recomendado de rodar o `cargo test`. Define encoder/decoder de software, conversão em CPU e `BRIKS_PROBE_NOHW=1`, no máximo 2 testes simultâneos (`--test-threads=2`), não roda `#[ignore]`, tem prazo (padrão 900 s) e **disjuntor**: passou do prazo, `taskkill /T /F` na árvore inteira.
4. **`scripts/paridade.ps1`**: pula por padrão os 8 casos perigosos (`BRIKS_TOAST_REAL`, `BRIKS_NUDGE_REAL`, `BRIKS_SYS_OPEN_REAL`, `BRIKS_OVERLAY_FORCE`, `BRIKS_TEST_FULLSCREEN`, roteiros com `win game|fg|max|min` ou `sys ovl`) com a mensagem "PULADO (perigoso: motivo)", registra "PULADO" no relatório (não conta como falha) e lista os pulados no fim. Prazo por caso (`-TimeoutSeg`, 120 s) agora mata a **árvore**, e há um **disjuntor geral** (`-PrazoTotalSeg`, 2700 s). `Stop-Filhos` só mata processos `briks*` (antes matava qualquer processo na pasta do `-Exe`).
5. **`scripts/prova/modal/rodar.ps1` e `prova/firewall/rodar.ps1`**: trocados de `Start-Process -Wait` (sem prazo) para disjuntor de 90 s.
6. **Regra para as sessões:** rodar testes do core só por `scripts/testes-core.ps1`; nunca `cargo test --ignored` nem `BRIKS_NUDGE_REAL`/`BRIKS_TOAST_REAL` sem o dono presente e o lock.

Pendente (não feito, fora do escopo seguro): `prova/matriz.ps1` e os demais scripts soltos (`capturas-*.ps1`, `overlay-*.ps1`, `check-cantos.ps1`) ainda usam `Kill()` simples; são ocultos e curtos, risco baixo.

## 4. Verificação feita
- `cargo check --tests` do `briks-core` compila com as mudanças.
- `paridade.ps1` com executável falso: os casos perigosos foram **pulados** e nada abriu; o relatório mostra `PULADO`.
- Disjuntor testado com `ping.exe` inofensivo (prazo de 3 s: processo morto); lock da GPU: pega, nega o segundo pedido e solta.
- **Não** foram rodados: nenhum teste de GPU, captura, overlay ou janela real; nem o `cargo test` (o dono interrompeu a execução).
- Incidente de teste meu: ao testar o harness antigo com `-Exe C:\Windows\System32\whoami.exe`, o `Stop-Filhos` original matou o próprio PowerShell e possivelmente outros processos de `System32` do usuário (só os que um usuário comum pode encerrar). Corrigido (agora só `briks*`). Se algo do Windows reiniciou sozinho (barra de tarefas, ícones da bandeja), foi isso, não a tela azul.
