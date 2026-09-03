# Cloud hourly billing — deployment

UDP billing service for chiaki-ng cloud play. Clients connect to **VM IP:13750** (JSON over UDP).

## Architecture

```
chiaki-ng  --UDP:13750-->  cloud_billing_udp_server.py  --MySQL-->  your_database
                                    |
                                    +--> Robokassa Recurring (CloudStreaming_PaymentMethods.StartPaymentID)

cloud_catalog_sync (separate pm2)  --hourly-->  CloudStreaming_Catalog (same MySQL)
```

Catalog sync is a **separate service**: `../cloud_catalog_sync/` — see its `DEPLOY.md`.

**Hourly billing clients** load the game grid over **TCP port `billing_udp_port+1` (default 13751)**: one JSON line request, then a header line + qCompress blob. UDP cannot carry the catalog (NAT/firewall drops multi-packet floods → client `0/0` chunks). Open **TCP 13751** in the VM firewall. Players do **not** need a personal NPSSO for browsing.

Optional catalog request filters: `service_type` (`psnow`/`pscloud`), `platform`, `only_billable`. Default: entire visible catalog, PS Now preferred over PS5 cloud for duplicate titles. Cover URLs are synthesized on the client from `productId`.

## 1. MySQL

```bash
mysql -h <DB_HOST> -u <DB_USER> -p <DB_NAME> < schema.sql
# optional examples (edit NPSSO placeholder first):
mysql ... < seed_example.sql
```

Tables: `CloudStreaming_Catalog` (filled by `../cloud_catalog_sync`), `CloudStreaming_Users`, ...

Fresh install:

```bash
mysql -h <DB_HOST> -u <DB_USER> -p <DB_NAME> < schema.sql
```

Existing database (already has old `CloudStreaming_*` tables):

```bash
mysql -h <DB_HOST> -u <DB_USER> -p <DB_NAME> < migrate_catalog_v2.sql
mysql -h <DB_HOST> -u <DB_USER> -p <DB_NAME> < migrate_sessions_balance_v3.sql
```

`migrate_sessions_balance_v3.sql` adds `PlusMinutesLeft` / `OwnedMinutesLeft` and
`CloudStreaming_SessionsArchive`. On `pm2 restart`, the billing server also
consolidates duplicate live sessions (one row per user) and moves extras to the
archive automatically.

**Billing model:** each player has one live `CloudStreaming_Sessions` row with two
residual wallets — Plus/F2P (`PlusMinutesLeft`) and owned titles (`OwnedMinutesLeft`).
Minutes burn only while `StreamActive=1`. Charging +60 min credits the matching pool.

If a previous attempt failed halfway:

```sql
SET FOREIGN_KEY_CHECKS = 0;
DROP TABLE IF EXISTS CloudStreaming_AccountOwnedGames_new;
SET FOREIGN_KEY_CHECKS = 1;
```

Then run `migrate_catalog_v2.sql` again.

Users need a row in **`CloudStreaming_PaymentMethods`** with `StartPaymentID` (Robokassa parent invoice for **cloud gaming only**). This is **not** the console rental `autobilling` table.

```sql
-- After user exists in CloudStreaming_Users:
INSERT INTO CloudStreaming_PaymentMethods (UserID, Email, StartPaymentID)
SELECT u.ID, u.User, 'PARENT_INVOICE_FROM_ROBOKASSA'
FROM CloudStreaming_Users u
WHERE u.User = 'user@example.com'
LIMIT 1;
```

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
pm2 restart cloud-billing-udp --update-env
pm2 logs cloud-billing-udp
```

## 2b. Catalog sync (separate pm2, same VM)

```bash
cd ../cloud_catalog_sync
chmod +x install.sh && ./install.sh
pm2 logs cloud-catalog-sync
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

## 4. Add PS accounts and games

For each rental account:

```sql
INSERT INTO CloudStreaming_Accounts (Label, NPSSO, HasPsPlus, Region, Status)
VALUES ('ps-rent-02', '<NPSSO>', 1, 'PL', 'available');
```

**PS Plus / PSNOW** — `HasPsPlus = 1` is enough; catalog link not required.

**Purchased PS5 cloud games** — pick from synced catalog:

```sql
-- list titles
SELECT CatalogID, Name, ServiceType, StreamIdentifier
FROM v_cs_catalog_picker
WHERE Name LIKE '%Game Name%'
LIMIT 20;

-- assign to account
INSERT INTO CloudStreaming_AccountOwnedGames (AccountID, CatalogID)
VALUES (
  (SELECT ID FROM CloudStreaming_Accounts WHERE Label = 'ps-rent-02'),
  12345   -- CatalogID from picker
);
```

Pre-orders not in Sony catalog yet: wait for hourly sync in `cloud-catalog-sync` (set `CS_CATALOG_NPSSO` in `../cloud_catalog_sync/.env`).

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
