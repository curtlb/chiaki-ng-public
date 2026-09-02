# Cloud catalog sync — separate pm2 service

Hourly fetch of PSNOW + PS Cloud titles into `CloudStreaming_Catalog` (shared MySQL with billing).

Independent from `scripts/cloud_billing` — own directory, own `.env`, own pm2 process.

## Prerequisites

1. MySQL schema from billing (includes `CloudStreaming_Catalog`):

```bash
mysql -h HOST -u USER -p DBNAME < ../cloud_billing/schema.sql
# or on existing DB:
mysql ... < ../cloud_billing/migrate_catalog.sql
```

2. Billing UDP server can run on the same VM in a **different folder** (`../cloud_billing`).

## Install on VM

```bash
cd /home/CloudCatalogSync   # suggested path (or clone repo subfolder)
git clone ... && cd chiaki-ng/scripts/cloud_catalog_sync
chmod +x install.sh
./install.sh
```

Edit `.env`: same `CS_DB_*` as billing + optional `CS_CATALOG_NPSSO`.

## pm2

| Process | Name |
|---------|------|
| Billing UDP | `cloud-billing-udp` (in `cloud_billing/`) |
| Catalog sync | `cloud-catalog-sync` (this folder) |

```bash
pm2 list
pm2 logs cloud-catalog-sync
pm2 restart cloud-catalog-sync --update-env
python3 catalog_sync_service.py --once
```

## Assign games to rental accounts

After sync:

```sql
SELECT CatalogID, Name, ServiceType, StreamIdentifier
FROM v_cs_catalog_picker
WHERE Name LIKE '%Game%';

INSERT INTO CloudStreaming_AccountOwnedGames (AccountID, CatalogID)
VALUES (
  (SELECT ID FROM CloudStreaming_Accounts WHERE Label = 'ps-rent-01'),
  12345
);
```

See `../cloud_billing/admin_queries.sql` for more examples.
