-- Useful admin queries for CloudStreaming catalog + account assignment

-- 1) Browse synced catalog (search by name)
SELECT CatalogID, Name, ServiceType, Platform, StreamIdentifier, EntitlementId, LastSyncedAt
FROM v_cs_catalog_picker
WHERE Name LIKE '%ghost%'
ORDER BY Name
LIMIT 50;

-- 2) What is assigned to each rental account (readable names)
SELECT * FROM v_cs_account_owned_games ORDER BY AccountLabel, GameName;

-- Or raw join:
SELECT a.Label, c.Name, c.ServiceType, c.StreamIdentifier, c.EntitlementId
FROM CloudStreaming_AccountOwnedGames aog
JOIN CloudStreaming_Accounts a ON a.ID = aog.AccountID
JOIN CloudStreaming_Catalog c ON c.ID = aog.CatalogID
ORDER BY a.Label, c.Name;

-- 2b) GameID empty? Normal — only CatalogID is required.
-- CloudStreaming_Games is filled when a user first plays (billing auto-creates)
-- or manually. Optional link:
-- UPDATE CloudStreaming_AccountOwnedGames aog
-- JOIN CloudStreaming_Catalog c ON c.ID = aog.CatalogID
-- JOIN CloudStreaming_Games g ON g.ServiceType = c.ServiceType AND g.GameIdentifier = c.StreamIdentifier
-- SET aog.GameID = g.ID
-- WHERE aog.GameID IS NULL;

-- 3) Assign a game to an account (replace labels / CatalogID)
INSERT INTO CloudStreaming_AccountOwnedGames (AccountID, CatalogID)
VALUES (
    (SELECT ID FROM CloudStreaming_Accounts WHERE Label = 'ps-rent-01' LIMIT 1),
    12345
);

-- 4) Remove assignment
DELETE FROM CloudStreaming_AccountOwnedGames
WHERE AccountID = (SELECT ID FROM CloudStreaming_Accounts WHERE Label = 'ps-rent-01' LIMIT 1)
  AND CatalogID = 12345;

-- 5) Catalog sync stats
SELECT ServiceType, Category, COUNT(*) AS cnt, MAX(LastSyncedAt) AS last_sync
FROM CloudStreaming_Catalog
GROUP BY ServiceType, Category;

-- 6) Cloud gaming payment (NOT console autobilling)
SELECT u.Email, pm.StartPaymentID, pm.Status, pm.UpdatedAt
FROM CloudStreaming_PaymentMethods pm
JOIN tableu u ON u.ID = pm.UserID;

INSERT INTO CloudStreaming_PaymentMethods (UserID, Email, StartPaymentID)
SELECT u.ID, u.Email, 'ROBOKASSA_PARENT_INVOICE_ID'
FROM tableu u
WHERE LOWER(u.Email) = 'curtlb@yandex.ru'
ORDER BY u.ID DESC
LIMIT 1;
