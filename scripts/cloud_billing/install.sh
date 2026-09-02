#!/usr/bin/env bash
# Deploy cloud billing UDP server on Linux VM (pm2).
set -euo pipefail
cd "$(dirname "$0")"

if [[ ! -f .env ]]; then
  cp .env.example .env
  echo "Created .env — fill CS_DB_* and CS_ROBOKASSA_* before starting."
  exit 1
fi

python3 -m pip install -r requirements.txt

if ! command -v pm2 >/dev/null 2>&1; then
  echo "Install pm2: npm install -g pm2"
  exit 1
fi

pm2 start ecosystem.config.js --update-env || pm2 restart cloud-billing-udp --update-env
pm2 save
echo "Cloud billing UDP server started. Open firewall UDP port 13750."
