#!/usr/bin/env bash
# Cloud billing UDP server — install on VM (Ubuntu/Debian)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "==> Cloud billing install in $SCRIPT_DIR"

if ! command -v python3 >/dev/null 2>&1; then
  echo "Install python3 first: apt install python3 python3-pip python3-venv"
  exit 1
fi

if [ ! -f .env ]; then
  if [ -f .env.example ]; then
    cp .env.example .env
    echo "Created .env from .env.example — EDIT .env with real credentials before starting!"
  else
    echo "Missing .env.example"
    exit 1
  fi
else
  echo ".env already exists (not overwritten)"
fi

python3 -m pip install --user -r requirements.txt

if ! command -v pm2 >/dev/null 2>&1; then
  echo "Installing pm2 globally (needs npm)..."
  npm install -g pm2
fi

pm2 delete cloud-billing-udp 2>/dev/null || true
pm2 start ecosystem.config.js
pm2 save

echo ""
echo "Done. Server should listen on UDP port from .env (default 13750)."
echo "Open firewall: ufw allow 13750/udp"
echo "Logs: pm2 logs cloud-billing-udp"
echo "Test: echo '{\"id\":\"1\",\"action\":\"ping\"}' | nc -u -w1 127.0.0.1 13750"
