# Servidor de chamadas (SFU) próprio do Briks: LiveKit + Caddy numa VPS

O SFU recebe cada transmissão uma vez e a distribui. Gasta **banda de saída por espectador**, e é isso que custa.
Numa VPS própria o custo é fixo (o plano da VPS inclui uma cota de tráfego) em vez de US$ 0,12/GB no LiveKit Cloud.

Nada muda no app: o cliente usa a URL que a função `livekit-token` devolve. Trocar de servidor = trocar 3 segredos no
Supabase (`ativar-sfu.ps1`); **não precisa lançar versão nova** e quem já está com o app instalado passa a usar o servidor novo.

## O que escolher (confira preços e cota de tráfego no site do provedor, mudam com o tempo)
- **Tráfego é o que importa**, não CPU: 1 vCPU/2 GB aguenta bastante; a cota mensal de saída é o limite.
- **Perto do Brasil** (São Paulo/Vinhedo) = menos latência, mas tráfego mais caro e cota menor (poucos TB).
- **EUA/Europa** costumam incluir 10–20 TB por pouco dinheiro, com 120–200 ms a mais de latência para o Brasil (ok para
  transmissão, perceptível em voz). O modo Direto (P2P) continua sendo o padrão e não passa por este servidor.
- Conta de cabeça: transmissão 720p ≈ 4 Mbps ≈ **1,8 GB por hora por espectador**. 2 TB ≈ 1.100 horas-espectador no mês.
- Precisa de IPv4 público, Ubuntu 22.04/24.04 ou Debian 12, e **um domínio/subdomínio seu** (ex.: `chamadas.seudominio.com.br`).

## Passo a passo
1. Crie a VPS e aponte o DNS: registro **A** `chamadas.seudominio.com.br` → IP da VPS.
2. Se o provedor tem firewall no painel (Oracle, AWS, Azure…), libere: 22/tcp, 80/tcp, 443/tcp, 7881/tcp, 50000–60000/udp.
3. Na VPS, como root: copie `instalar.sh` (scp) e rode `bash instalar.sh chamadas.seudominio.com.br`.
   Ele instala Docker, gera Key/Secret aleatórios, abre o firewall da VPS, sobe LiveKit + Caddy e espera o HTTPS.
   No fim mostra `URL`, `API_KEY` e `API_SECRET` (ficam em `/opt/briks-livekit/credenciais.txt`).
4. No seu PC, na raiz do repositório: `powershell -NoProfile -File scripts/ativar-sfu.ps1`, e cole a URL (`wss://…`), a Key e o Secret
   quando pedir. Isso grava os segredos no Supabase, publica `livekit-token`/`livekit-kick` e valida ponta a ponta.
5. Confira em 2 PCs: Configurações > Rede > diagnóstico mostra o servidor novo (`Modo da chamada: SFU · servidor chamadas.seudominio…`).
6. Novos instaladores: `$env:BRIKS_LIVEKIT_URL='wss://chamadas.seudominio.com.br'` antes de `scripts/build-installer.ps1`
   (para quem instalar do zero já vir com o endereço certo; quem já tem o app recebe pela função de tokens).

## Operação
- Consumo de banda do mês: `vnstat -m` (tempo real: `vnstat -l`). Fique de olho antes de estourar a cota da VPS.
- Logs: `cd /opt/briks-livekit && docker compose -f compose.yml logs --tail 100`.
- Atualizar: `docker compose -f compose.yml pull && docker compose -f compose.yml up -d`.
- Voltar para o LiveKit Cloud: rode `ativar-sfu.ps1` de novo com a URL/Key/Secret do projeto Cloud.
- Segurança: a Key/Secret só existem na VPS e no Supabase; o app só conhece a URL pública. Não cole em conversa nem no git.

## O que NÃO está incluído
- TURN próprio: o Briks já recebe credenciais TURN pela função `turn-credentials` (ver `docs/ativar-turn.md`); este servidor é só SFU.
- Vários servidores/regiões e Redis: um nó basta para o uso atual. Se um dia precisar de mais de um, o LiveKit pede Redis.

## Oracle Cloud (plano grátis "Always Free")
Dá 10 TB de saída por mês de graça, mas tem pegadinhas. Confira os limites atuais em oracle.com/br/cloud/free.
- **A região de origem é escolhida no cadastro e não muda depois**: escolha Brasil (Vinhedo ou São Paulo).
- Crie uma instância **Ubuntu 24.04**. As máquinas ARM grátis (Ampere A1) costumam dar "out of capacity"; tente de novo em horários
  diferentes, ou use a AMD grátis (VM.Standard.E2.1.Micro, 1 GB: serve para poucos usuários). Converter a conta para
  "Pay As You Go" (sem custo dentro do que é grátis) costuma liberar capacidade e evita a desativação de contas ociosas.
- **Duas camadas de firewall**: (1) no painel, em Rede > Lista de segurança (Security List) da sua sub-rede, adicione regras de
  entrada para 80, 443, 7881/tcp e 50000–60000/udp; (2) dentro da máquina, o `instalar.sh` já detecta as regras do iptables da
  imagem da Oracle e libera as portas lá também. Esquecer a camada 1 é o erro mais comum: tudo parece ok e nada conecta.
- Dê ao IP público da instância o tipo **reservado**, para ele não mudar se a máquina for recriada.
- Mantenha a máquina com algum uso: a Oracle pode recolher instâncias grátis que ficam ociosas por muito tempo.
