#!/usr/bin/env bash
# Cloud catalog sync — separate pm2 service (same VM as billing, own directory)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "==> Cloud catalog sync install in $SCRIPT_DIR"

if ! command -v python3 >/dev/null 2>&1; then
  echo "Install python3 first: apt install python3 python3-pip"
  exit 1
fi

mkdir -p logs

if [ ! -f .env ]; then
  if [ -f .env.example ]; then
    cp .env.example .env
    echo "Created .env from .env.example — EDIT .env before starting!"
  else
    echo "Missing .env.example"
    exit 1
  fi
else
  echo ".env already exists (not overwritten)"
fi

python3 -m pip install --user -r requirements.txt

missing=0
for key in CS_DB_NAME CS_DB_USER CS_DB_PASS; do
  if ! grep -q "^${key}=." .env 2>/dev/null; then
    echo "ERROR: .env missing or empty: ${key}=..."
    missing=1
  fi
done
if [ "$missing" -ne 0 ]; then
  echo "Edit $SCRIPT_DIR/.env then run install.sh again."
  exit 1
fi

if ! command -v pm2 >/dev/null 2>&1; then
  echo "Installing pm2 globally (needs npm)..."
  npm install -g pm2
fi

pm2 delete cloud-catalog-sync 2>/dev/null || true
pm2 start ecosystem.config.js
pm2 save

echo ""
echo "Done. Catalog sync runs immediately, then every CS_CATALOG_SYNC_INTERVAL_SEC (default 1h)."
echo "Logs: pm2 logs cloud-catalog-sync"
echo "Manual run: python3 catalog_sync_service.py --once"
echo ""
echo "DB table CloudStreaming_Catalog must exist (run ../cloud_billing/schema.sql or migrate_catalog.sql)."
