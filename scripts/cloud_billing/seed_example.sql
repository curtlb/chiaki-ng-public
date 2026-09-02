-- Example seed data (NO real NPSSO — replace placeholders on the server only)
-- Run after schema.sql

-- Example game in catalog (PS Plus title)
INSERT INTO CloudStreaming_Games (Code, Name, ServiceType, GameIdentifier, AccessType, HourlyPrice, Currency)
VALUES (
    'ghostship',
    'Ghost of Tsushima (PSNOW example)',
    'psnow',
    'EP9000-CUSA32709_00-GHOSTSHIP0000000',
    'ps_plus',
    110.00,
    'RUB'
) ON DUPLICATE KEY UPDATE Name = VALUES(Name), HourlyPrice = VALUES(HourlyPrice);

-- Example PS account pool entry
-- REPLACE __NPSSO_PLACEHOLDER__ with real token via SQL client (never commit real value)
INSERT INTO CloudStreaming_Accounts (Label, NPSSO, HasPsPlus, Region, Status)
VALUES (
    'ps-rent-01',
    '__NPSSO_PLACEHOLDER__',
    1,
    'PL',
    'available'
);

-- Link owned game to account (optional — for purchased PS5 cloud titles)
-- INSERT INTO CloudStreaming_AccountOwnedGames (AccountID, GameID, EntitlementID)
-- SELECT a.ID, g.ID, 'UB0837-PPSA34206_00-0944122131716983'
-- FROM CloudStreaming_Accounts a, CloudStreaming_Games g
-- WHERE a.Label = 'ps-rent-01' AND g.Code = 'your_game_code';
