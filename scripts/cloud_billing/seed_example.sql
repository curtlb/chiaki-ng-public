-- Example seed data (NO real NPSSO — replace placeholders on the server only)
-- Run after schema.sql and after catalog sync (../cloud_catalog_sync):
--   cd ../cloud_catalog_sync && python3 catalog_sync_service.py --once

-- Browse synced catalog (picker for admin UI / SQL client):
-- SELECT * FROM v_cs_catalog_picker WHERE Name LIKE '%Ghost%' LIMIT 20;

-- Example PS account pool entry
INSERT INTO CloudStreaming_Accounts (Label, NPSSO, HasPsPlus, Region, Status)
VALUES (
    'ps-rent-01',
    '__NPSSO_PLACEHOLDER__',
    1,
    'PL',
    'available'
) ON DUPLICATE KEY UPDATE Label = VALUES(Label);

-- Link a purchased cloud title to an account (pick CatalogID from v_cs_catalog_picker):
-- INSERT INTO CloudStreaming_AccountOwnedGames (AccountID, CatalogID)
-- SELECT a.ID, c.CatalogID
-- FROM CloudStreaming_Accounts a
-- JOIN v_cs_catalog_picker c ON c.Name LIKE '%Your Game Name%'
-- WHERE a.Label = 'ps-rent-01'
-- LIMIT 1;

-- Enable hourly billing for a catalog title (optional — auto-created on first client play):
-- INSERT INTO CloudStreaming_Games (CatalogID, Code, Name, ServiceType, GameIdentifier, AccessType, HourlyPrice)
-- SELECT c.ID, 'my_game', c.Name, c.ServiceType, c.StreamIdentifier, 'owned_only', 110.00
-- FROM CloudStreaming_Catalog c
-- WHERE c.StreamIdentifier = 'UB0837-PPSA34206_00-...'
-- LIMIT 1;
