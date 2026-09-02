# Cloud billing server (UDP)

## 1. MySQL

```bash
mysql -h <host> -u <user> -p <database> < schema.sql
```

## 2. VM setup

```bash
cd scripts/cloud_billing
cp .env.example .env
# Edit .env: CS_DB_*, CS_MRH_LOGIN, CS_ROBOKASSA_PASS1, CS_ROBOKASSA_PASS2
chmod +x install.sh
./install.sh
```

Open **UDP 13750** in the firewall.

## 3. PS accounts

Add rows to `CloudStreaming_Accounts` (see `seed_example.sql`). NPSSO only on the server, never in git.

## 4. chiaki-ng client

Settings → Облако → почасовая оплата:
- Host: billing VM IP (default `5.183.190.150`)
- Port: `13750`
- Login to 4cloud.pro in the app (email + autobilling card required)
