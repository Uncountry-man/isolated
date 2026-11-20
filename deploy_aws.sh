#!/usr/bin/env bash
# Atualiza o servidor de produção na AWS (Lightsail). Uso: ./deploy_aws.sh   (rodar FORA do sandbox do Claude Code)
# Compila o servidor estático, envia, instala/atualiza o serviço `isolated` (com --data, NUNCA --debug), os timers (chaves do Firebase a cada
# hora e backup diário) e reinicia. NÃO apaga dados nem o mundo: antes de trocar o binário faz um backup (backups/pre-deploy-*.tar.gz).
# ATENÇÃO: reiniciar derruba os jogadores conectados por alguns segundos (o app reconecta sozinho; as contas são gravadas no desligamento).
# ADMINS: cria /opt/isolated/data/admins.txt (uid do Firebase do dono) SÓ SE NÃO EXISTIR; um arquivo existente nunca é sobrescrito.
# Variáveis: AWS_HOST (obrigatória), AWS_USER (ubuntu), AWS_KEY (aws-key.pem), ADMIN_UID (uid do Firebase do dono, p/ criar o admins.txt).
# Podem ficar num arquivo local `deploy.env` (fora do git), lido automaticamente.
set -euo pipefail
cd "$(dirname "$0")"
[ -f deploy.env ] && . ./deploy.env
: "${AWS_HOST:?defina AWS_HOST (ou crie deploy.env com AWS_HOST=ip)}"
HOST="$AWS_HOST"; USER_="${AWS_USER:-ubuntu}"; KEY="${AWS_KEY:-aws-key.pem}"
[ -f "$KEY" ] || { echo "Chave SSH não encontrada: $KEY"; exit 1; }
grep "^ExecStart" server/deploy/isolated.service | grep -q -- "--debug" && { echo "ERRO: isolated.service com --debug"; exit 1; }
SSH_OPTS=(-i "$KEY" -o UserKnownHostsFile=build/known_hosts -o StrictHostKeyChecking=accept-new -o ConnectTimeout=15)
mkdir -p build
echo "1/5 compilando o servidor (estático)..."
g++ -std=c++17 -O2 -pthread -static server/server_main.cpp shared/world.cpp shared/world_sync.cpp -o build/server-linux
echo "2/5 enviando para $HOST..."
ssh "${SSH_OPTS[@]}" "$USER_@$HOST" 'rm -rf /tmp/isolated-deploy && mkdir -p /tmp/isolated-deploy'
scp "${SSH_OPTS[@]}" build/server-linux server/deploy/* "$USER_@$HOST:/tmp/isolated-deploy/"
echo "3/5 instalando (diretórios, scripts, serviço e timers)..."
ssh "${SSH_OPTS[@]}" "$USER_@$HOST" 'set -e
D=/tmp/isolated-deploy
sudo install -d -o isolated -g isolated -m 750 /opt/isolated/data /opt/isolated/backups /opt/isolated/bin
sudo install -o root -g root -m 755 $D/fetch_firebase_keys.sh $D/backup_data.sh $D/fcm_send.py /opt/isolated/bin/
sudo -u isolated test -s /opt/isolated/data/fcm_service_account.json && echo "push (FCM): conta de servico OK" || echo "AVISO: sem data/fcm_service_account.json: notificacoes push da tripulacao desligadas (ver CLAUDE.md TRIPULACAO)"
sudo install -o root -g root -m 644 $D/isolated.service $D/isolated-firebase-keys.service $D/isolated-firebase-keys.timer $D/isolated-backup.service $D/isolated-backup.timer /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now isolated-firebase-keys.timer isolated-backup.timer >/dev/null
sudo systemctl start isolated-firebase-keys.service || echo "AVISO: nao baixou as chaves do Firebase agora (o timer tenta de novo)"
sudo -u isolated test -s /opt/isolated/data/firebase_jwk.json && echo "chaves do Firebase: OK" || echo "AVISO: sem data/firebase_jwk.json"
if sudo test -e /opt/isolated/data/admins.txt; then echo "admins.txt: ja existe (mantido)"; else
  sudo -u isolated sh -c "umask 027; set -C; printf \"%s\\n\" \"# ADMINS do servidor: um uid do Firebase por linha (conta Google verificada; visitante nunca e admin).\" \"# O servidor rele o arquivo sozinho (ate ~2 s). Comentarios com #.\" \"${ADMIN_UID:-}\" > /opt/isolated/data/admins.txt" && echo "admins.txt: CRIADO"
fi'
echo "4/5 backup + troca do binário + reinício..."
ssh "${SSH_OPTS[@]}" "$USER_@$HOST" 'set -e
sudo -u isolated tar -czf /opt/isolated/backups/pre-deploy-$(date -u +%Y-%m-%d-%H%M%S).tar.gz -C /opt/isolated --exclude="*.tmp" data worlds
sudo systemctl stop isolated
sudo install -o isolated -g isolated -m 755 /tmp/isolated-deploy/server-linux /opt/isolated/server
sudo systemctl enable isolated >/dev/null 2>&1 || true
sudo systemctl start isolated
sleep 5; systemctl is-active isolated
systemctl show isolated -p ExecStart | grep -q -- "--debug" && { echo "ERRO: serviço rodando com --debug"; exit 1; } || echo "sem --debug: OK"
rm -rf /tmp/isolated-deploy'
echo "5/5 log do servidor:"
ssh "${SSH_OPTS[@]}" "$USER_@$HOST" 'sudo journalctl -u isolated -n 14 --no-pager | cut -c1-220; echo "--- admins.txt (sem comentarios) ---"; sudo grep -v "^#" /opt/isolated/data/admins.txt || true'
echo "OK: servidor da AWS atualizado. (o kProtocolMagic do app e o do servidor precisam ser o mesmo)"
