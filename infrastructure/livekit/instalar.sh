#!/usr/bin/env bash
# Instala o servidor de chamadas (SFU) do Briks numa VPS Ubuntu/Debian: LiveKit + Caddy (HTTPS automatico), tudo em Docker.
# Uso (como root, na VPS):   bash instalar.sh chamadas.seudominio.com.br
# Antes: o dominio precisa apontar (registro A) para o IP publico desta VPS; senao o Caddy nao consegue o certificado.
# O que faz: instala Docker/ufw/vnstat, gera API Key e Secret ALEATORIOS, grava tudo em /opt/briks-livekit, abre so as portas
# necessarias no firewall da VPS, sobe os containers e espera o HTTPS responder. No fim mostra a URL, a Key e o Secret UMA vez
# (tambem ficam em /opt/briks-livekit/credenciais.txt, so root le). Rode de novo para atualizar: as chaves sao mantidas.
set -euo pipefail

DOM="${1:-}"
[ "$(id -u)" = 0 ] || { echo "ERRO: rode como root (sudo bash instalar.sh DOMINIO)."; exit 1; }
[[ "$DOM" =~ ^[A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?\.[A-Za-z]{2,}$ ]] || { echo "Uso: bash instalar.sh chamadas.seudominio.com.br   (um dominio/subdominio seu, so o nome, sem https://)"; exit 1; }
DIR=/opt/briks-livekit

echo "== 1/5 Pacotes =="
export DEBIAN_FRONTEND=noninteractive
apt-get update -y >/dev/null
apt-get install -y ca-certificates curl openssl ufw vnstat dnsutils >/dev/null
if ! command -v docker >/dev/null 2>&1; then curl -fsSL https://get.docker.com | sh >/dev/null; fi
docker compose version >/dev/null 2>&1 || { echo "ERRO: 'docker compose' nao encontrado (instale o plugin docker-compose-plugin)."; exit 1; }

echo "== 2/5 Conferindo o DNS =="
IP="$(curl -fsS -4 https://api.ipify.org || true)"
DNS_IP="$(dig +short A "$DOM" @1.1.1.1 | tail -1 || true)"
echo "IP desta VPS: ${IP:-?} | $DOM aponta para: ${DNS_IP:-nada}"
if [ -z "$DNS_IP" ] || { [ -n "$IP" ] && [ "$DNS_IP" != "$IP" ]; }; then
  echo "AVISO: o dominio ainda nao aponta para esta VPS. Crie o registro A  $DOM -> ${IP:-IP-da-VPS}  e espere propagar."
  read -r -p "Continuar mesmo assim? (s/N) " r; [ "${r,,}" = "s" ] || exit 1
fi

echo "== 3/5 Arquivos em $DIR =="
mkdir -p "$DIR"; cd "$DIR"
if [ -f credenciais.txt ]; then
  KEY="$(sed -n 's/^API_KEY=//p' credenciais.txt)"; SECRET="$(sed -n 's/^API_SECRET=//p' credenciais.txt)"; echo "Chaves ja existiam: mantidas."
else
  KEY="API$(openssl rand -hex 7)"; SECRET="$(openssl rand -hex 24)"
  umask 077; printf 'URL=wss://%s\nAPI_KEY=%s\nAPI_SECRET=%s\n' "$DOM" "$KEY" "$SECRET" > credenciais.txt; umask 022
fi
cat > livekit.yaml <<EOF
# Gerado por instalar.sh. Nao ponha este arquivo em repositorio: tem o segredo.
port: 7880
log_level: info
rtc:
  tcp_port: 7881
  port_range_start: 50000
  port_range_end: 60000
  use_external_ip: true
# O TURN fica por conta da funcao turn-credentials do Briks; aqui so o SFU.
turn:
  enabled: false
keys:
  $KEY: $SECRET
EOF
chmod 600 livekit.yaml
cat > Caddyfile <<EOF
$DOM {
	reverse_proxy 127.0.0.1:7880
}
EOF
cat > compose.yml <<'EOF'
services:
  livekit:
    image: livekit/livekit-server:latest
    command: --config /etc/livekit.yaml
    network_mode: host
    restart: unless-stopped
    volumes:
      - ./livekit.yaml:/etc/livekit.yaml:ro
  caddy:
    image: caddy:2
    network_mode: host
    restart: unless-stopped
    volumes:
      - ./Caddyfile:/etc/caddy/Caddyfile:ro
      - caddy_data:/data
      - caddy_config:/config
volumes:
  caddy_data: {}
  caddy_config: {}
EOF

echo "== 4/5 Firewall da VPS (ufw) =="
SSH_PORT="$(sshd -T 2>/dev/null | awk '/^port /{print $2; exit}')"; SSH_PORT="${SSH_PORT:-22}"
ufw allow "${SSH_PORT}/tcp" >/dev/null     # nunca se trancar para fora
ufw allow 80/tcp >/dev/null; ufw allow 443/tcp >/dev/null            # HTTPS (certificado e conexao do app)
ufw allow 7881/tcp >/dev/null; ufw allow 50000:60000/udp >/dev/null  # midia WebRTC (TCP de reserva e UDP)
ufw --force enable >/dev/null
echo "Portas abertas aqui: ssh($SSH_PORT) 80 443 7881/tcp 50000-60000/udp."
echo "ATENCAO: se a VPS tem OUTRO firewall no painel do provedor (Oracle, AWS, Azure...), abra as mesmas portas la tambem."

echo "== 5/5 Subindo =="
docker compose -f compose.yml pull >/dev/null
docker compose -f compose.yml up -d
ok=0
for i in $(seq 1 40); do
  if curl -fsS -m 5 "https://$DOM/" >/dev/null 2>&1; then ok=1; break; fi; sleep 3
done
if [ "$ok" = 1 ]; then echo "HTTPS respondendo em https://$DOM (certificado obtido)."
else echo "AVISO: https://$DOM ainda nao respondeu. Veja:  cd $DIR && docker compose -f compose.yml logs caddy --tail 50"; fi

echo
echo "==================== COPIE PARA O ativar-sfu.ps1 (no seu PC) ===================="
cat credenciais.txt
echo "================================================================================="
echo "Tambem ficam em $DIR/credenciais.txt (so o root le). Nao cole estes valores em conversa nenhuma."
echo "Acompanhar o consumo de banda no mes:  vnstat -m     |  em tempo real:  vnstat -l"
echo "Atualizar o LiveKit depois:  cd $DIR && docker compose -f compose.yml pull && docker compose -f compose.yml up -d"
