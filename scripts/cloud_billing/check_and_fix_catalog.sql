-- =============================================================================
-- CloudStreaming: проверка + исправление миграции CatalogID (один файл, phpMyAdmin)
-- Без information_schema. Ошибки «Duplicate column/key» — нормально, идите дальше.
-- =============================================================================

SET NAMES utf8mb4;
SET FOREIGN_KEY_CHECKS = 0;

-- -----------------------------------------------------------------------------
-- A) ПРОВЕРКА (смотрите результаты внизу вкладки «Результат»)
-- -----------------------------------------------------------------------------

SELECT '=== 1. Таблица каталога ===' AS step;
SELECT COUNT(*) AS catalog_rows FROM CloudStreaming_Catalog;

SELECT '=== 2. Колонка Games.CatalogID (должна быть 1 строка) ===' AS step;
SHOW COLUMNS FROM CloudStreaming_Games LIKE 'CatalogID';

SELECT '=== 3. Колонка AccountOwnedGames.CatalogID (должна быть 1 строка) ===' AS step;
SHOW COLUMNS FROM CloudStreaming_AccountOwnedGames LIKE 'CatalogID';

SELECT '=== 4. Оплата 4cloud (должна быть хотя бы 1 строка active) ===' AS step;
SELECT ID, Email, StartPaymentID, Status FROM CloudStreaming_PaymentMethods;

SELECT '=== 5. PS-аккаунты ===' AS step;
SELECT ID, Label, Status, HasPsPlus, Region FROM CloudStreaming_Accounts;

SELECT '=== 6. Игры на аккаунтах (старая схема = только GameID, новая = CatalogID) ===' AS step;
SELECT * FROM CloudStreaming_AccountOwnedGames;

SELECT '=== 7. From the Bunker в каталоге ===' AS step;
SELECT ID, Name, ServiceType, StreamIdentifier, ProductId, EntitlementId
FROM CloudStreaming_Catalog
WHERE Name LIKE '%Bunker%' OR StreamIdentifier LIKE '%Bunker%'
LIMIT 10;

-- -----------------------------------------------------------------------------
-- B) ИСПРАВЛЕНИЕ СХЕМЫ
-- -----------------------------------------------------------------------------

SELECT '=== B1. Создать CloudStreaming_Catalog (если нет) ===' AS step;
CREATE TABLE IF NOT EXISTS CloudStreaming_Catalog (
    ID              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    CatalogKey      VARCHAR(200) NOT NULL,
    Name            VARCHAR(512) NOT NULL,
    ServiceType     ENUM('psnow','pscloud') NOT NULL,
    Platform        ENUM('ps3','ps4','ps5','unknown') NOT NULL DEFAULT 'unknown',
    ProductId       VARCHAR(128) NULL,
    EntitlementId   VARCHAR(128) NULL,
    StreamIdentifier VARCHAR(128) NOT NULL,
    ConceptId       VARCHAR(64) NULL,
    ImageUrl        VARCHAR(1024) NULL,
    Category        VARCHAR(32) NOT NULL DEFAULT 'streamable',
    SourceList      VARCHAR(64) NULL,
    Locale          VARCHAR(16) NULL,
    IsVisible       TINYINT(1) NOT NULL DEFAULT 1,
    FirstSeenAt     DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    LastSyncedAt    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (ID),
    UNIQUE KEY uq_cs_catalog_key (CatalogKey),
    KEY idx_cs_catalog_name (Name(191)),
    KEY idx_cs_catalog_service (ServiceType, Platform),
    KEY idx_cs_catalog_stream (StreamIdentifier),
    KEY idx_cs_catalog_product (ProductId),
    KEY idx_cs_catalog_entitlement (EntitlementId)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

SELECT '=== B2. Games.CatalogID (#1060 = уже есть, ОК) ===' AS step;
ALTER TABLE CloudStreaming_Games
    ADD COLUMN CatalogID BIGINT UNSIGNED NULL COMMENT 'link to synced catalog row' AFTER ID;

SELECT '=== B3. FK Games -> Catalog (#1826 duplicate = уже есть, ОК) ===' AS step;
ALTER TABLE CloudStreaming_Games
    ADD CONSTRAINT fk_cs_game_catalog FOREIGN KEY (CatalogID) REFERENCES CloudStreaming_Catalog(ID) ON DELETE SET NULL;

SELECT '=== B4. Заполнить каталог из CloudStreaming_Games ===' AS step;
INSERT IGNORE INTO CloudStreaming_Catalog
    (CatalogKey, Name, ServiceType, Platform, ProductId, EntitlementId, StreamIdentifier, Category)
SELECT
    CONCAT(g.ServiceType, ':', g.GameIdentifier),
    g.Name,
    g.ServiceType,
    IF(g.ServiceType = 'pscloud', 'ps5', 'ps4'),
    IF(g.ServiceType = 'psnow', g.GameIdentifier, NULL),
    IF(g.ServiceType = 'pscloud', g.GameIdentifier, NULL),
    g.GameIdentifier,
    'manual'
FROM CloudStreaming_Games g;

-- -----------------------------------------------------------------------------
-- B5) Пересборка AccountOwnedGames — ТОЛЬКО если в шаге 3 было ПУСТО (нет CatalogID).
--     Если шаг 3 уже показал CatalogID — ЗАКОММЕНТИРУЙТЕ блок B5 (строки CREATE…RENAME).
-- -----------------------------------------------------------------------------

SELECT '=== B5. Пересборка AccountOwnedGames (пропустите, если CatalogID уже есть) ===' AS step;
DROP TABLE IF EXISTS CloudStreaming_AccountOwnedGames_new;

CREATE TABLE CloudStreaming_AccountOwnedGames_new (
    ID              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    AccountID       BIGINT UNSIGNED NOT NULL,
    CatalogID       BIGINT UNSIGNED NOT NULL,
    GameID          BIGINT UNSIGNED NULL,
    CreatedAt       DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (ID),
    UNIQUE KEY uq_cs_aog_mig (AccountID, CatalogID),
    CONSTRAINT fk_cs_aog_mig_account FOREIGN KEY (AccountID) REFERENCES CloudStreaming_Accounts(ID) ON DELETE CASCADE,
    CONSTRAINT fk_cs_aog_mig_catalog FOREIGN KEY (CatalogID) REFERENCES CloudStreaming_Catalog(ID) ON DELETE CASCADE,
    CONSTRAINT fk_cs_aog_mig_game FOREIGN KEY (GameID) REFERENCES CloudStreaming_Games(ID) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT INTO CloudStreaming_AccountOwnedGames_new (AccountID, CatalogID, GameID, CreatedAt)
SELECT aog.AccountID, c.ID, aog.GameID, aog.CreatedAt
FROM CloudStreaming_AccountOwnedGames aog
JOIN CloudStreaming_Games g ON g.ID = aog.GameID
JOIN CloudStreaming_Catalog c ON c.StreamIdentifier = g.GameIdentifier AND c.ServiceType = g.ServiceType;

DROP TABLE CloudStreaming_AccountOwnedGames;
RENAME TABLE CloudStreaming_AccountOwnedGames_new TO CloudStreaming_AccountOwnedGames;

-- -----------------------------------------------------------------------------
-- C) VIEWS + привязка Bunker (отредактируйте Label аккаунта при необходимости)
-- -----------------------------------------------------------------------------

SELECT '=== C1. Views для phpMyAdmin ===' AS step;
CREATE OR REPLACE VIEW v_cs_catalog_picker AS
SELECT c.ID AS CatalogID, c.Name, c.ServiceType, c.Platform, c.ProductId,
       c.EntitlementId, c.StreamIdentifier, c.Category, c.LastSyncedAt
FROM CloudStreaming_Catalog c
WHERE c.IsVisible = 1
ORDER BY c.Name;

CREATE OR REPLACE VIEW v_cs_account_owned_games AS
SELECT aog.ID, aog.AccountID, acc.Label AS AccountLabel, aog.CatalogID,
       c.Name AS GameName, c.ServiceType, c.Platform, c.StreamIdentifier,
       c.ProductId, c.EntitlementId, aog.GameID, aog.CreatedAt
FROM CloudStreaming_AccountOwnedGames aog
JOIN CloudStreaming_Accounts acc ON acc.ID = aog.AccountID
JOIN CloudStreaming_Catalog c ON c.ID = aog.CatalogID;

SELECT '=== C2. Привязать From the Bunker к первому PS-аккаунту (если ещё нет) ===' AS step;
INSERT IGNORE INTO CloudStreaming_AccountOwnedGames (AccountID, CatalogID)
SELECT a.ID, c.ID
FROM CloudStreaming_Accounts a
JOIN CloudStreaming_Catalog c ON c.Name LIKE '%From the Bunker%'
ORDER BY a.ID
LIMIT 1;

SET FOREIGN_KEY_CHECKS = 1;

-- -----------------------------------------------------------------------------
-- D) ИТОГОВАЯ ПРОВЕРКА
-- -----------------------------------------------------------------------------

SELECT '=== D. ИТОГ ===' AS step;
SHOW COLUMNS FROM CloudStreaming_Games LIKE 'CatalogID';
SHOW COLUMNS FROM CloudStreaming_AccountOwnedGames LIKE 'CatalogID';
SELECT COUNT(*) AS catalog_rows FROM CloudStreaming_Catalog;
SELECT * FROM v_cs_account_owned_games WHERE GameName LIKE '%Bunker%';
