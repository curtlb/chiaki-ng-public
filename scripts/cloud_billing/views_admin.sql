-- Run on existing DB (phpMyAdmin / mysql client):
-- mysql -h HOST -u USER -p DBNAME < views_admin.sql
--
-- phpMyAdmin: показывать название игры в обзоре CatalogID
-- 1) Выполнить migrate_catalog_picker.sql (колонка PickerLabel)
-- 2) Структура → CloudStreaming_AccountOwnedGames → Связи (Relation view)
-- 3) CatalogID → CloudStreaming_Catalog → колонка отображения: PickerLabel (или Name)
--    НЕ CatalogKey и НЕ StreamIdentifier

CREATE OR REPLACE VIEW v_cs_account_owned_games AS
SELECT
    aog.ID,
    aog.AccountID,
    a.Label AS AccountLabel,
    aog.CatalogID,
    c.Name AS GameName,
    c.ServiceType,
    c.Platform,
    c.StreamIdentifier,
    c.EntitlementId,
    aog.GameID,
    g.Name AS BillingGameName,
    g.HourlyPrice,
    aog.CreatedAt
FROM CloudStreaming_AccountOwnedGames aog
JOIN CloudStreaming_Accounts a ON a.ID = aog.AccountID
JOIN CloudStreaming_Catalog c ON c.ID = aog.CatalogID
LEFT JOIN CloudStreaming_Games g ON g.ID = aog.GameID
ORDER BY a.Label, c.Name;

CREATE OR REPLACE VIEW v_cs_games_picker AS
SELECT
    g.ID AS GameID,
    g.Code,
    g.Name,
    g.ServiceType,
    g.GameIdentifier,
    g.HourlyPrice,
    g.Currency,
    g.IsActive,
    c.Name AS CatalogName
FROM CloudStreaming_Games g
LEFT JOIN CloudStreaming_Catalog c ON c.ID = g.CatalogID
ORDER BY g.Name;
