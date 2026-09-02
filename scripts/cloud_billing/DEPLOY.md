# Cloud hourly billing — deployment

UDP billing service for chiaki-ng cloud play. Clients connect to **VM IP:13750** (JSON over UDP).

## Architecture

```
chiaki-ng  --UDP:13750-->  cloud_billing_udp_server.py  --MySQL-->  your_database
                                    |
                                    +--> Robokassa Recurring (autobilling.StartPaymentID)
```

## 1. MySQL

```bash
mysql -h <DB_HOST> -u <DB_USER> -p <DB_NAME> < schema.sql
# optional examples (edit NPSSO placeholder first):
mysql ... < seed_example.sql
```

Tables: `CloudStreaming_Users`, `CloudStreaming_Games`, `CloudStreaming_Accounts`, `CloudStreaming_Leases`, `CloudStreaming_Sessions`, `CloudStreaming_Charges`.

Users must exist in `autobilling` with valid `StartPaymentID` for recurring charges.

## 2. VM setup (5.183.190.150)

```bash
git clone git@github.com:curtlb/chiaki-ng.git
cd chiaki-ng/scripts/cloud_billing
chmod +x install.sh
./install.sh
```

Edit `.env` (created from `.env.example`):

| Variable | Description |
|----------|-------------|
| `CS_DB_HOST` | MySQL host |
| `CS_DB_NAME` | Database name |
| `CS_DB_USER` | MySQL user |
| `CS_DB_PASS` | MySQL password |
| `CS_MRH_LOGIN` | Robokassa merchant login |
| `CS_ROBOKASSA_PASS1` | Robokassa password #1 |
| `CS_ROBOKASSA_PASS2` | Robokassa password #2 |
| `CS_BILLING_UDP_PORT` | Default `13750` |

Restart after editing:

```bash
pm2 restart cloud-billing-udp
pm2 logs cloud-billing-udp
```

## 3. Firewall

```bash
sudo ufw allow 13750/udp
sudo ufw reload
```

Verify from another machine:

```bash
echo '{"id":"test","action":"ping"}' | nc -u -w2 5.183.190.150 13750
```

Expected JSON: `{"id":"test","ok":true,"message":"pong",...}`

## 4. Add PS accounts

For each rental account:

```sql
INSERT INTO CloudStreaming_Accounts (Label, NPSSO, HasPsPlus, Region, Status)
VALUES ('ps-rent-02', '<NPSSO>', 1, 'PL', 'available');
```

Register games in `CloudStreaming_Games` and link owned titles via `CloudStreaming_AccountOwnedGames`.

## 5. chiaki-ng client

Settings → Cloud Play:

- **Почасовая оплата** — enabled
- **Сервер биллинга** — `5.183.190.150`
- **Порт** — `13750`
- Login to **4cloud.pro** (email used for billing)

Flow: quote dialog → confirm → charge → rented NPSSO → cloud stream.

## Security

- Never commit `.env` or real NPSSO tokens
- `.gitignore` excludes `.env`
- Rotate DB/Robokassa credentials if ever exposed
