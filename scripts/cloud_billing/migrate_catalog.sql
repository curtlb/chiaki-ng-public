-- Миграция каталога для существующей БД CloudStreaming (без information_schema).
-- phpMyAdmin: выполняйте блоки по порядку. Если строка падает с «Duplicate column» /
-- «Duplicate key» / «already exists» — пропустите её и идите дальше.
--
-- mysql -h HOST -u USER -p DBNAME < migrate_catalog_v2.sql

SET NAMES utf8mb4;
SET FOREIGN_KEY_CHECKS = 0;

-- ===========================================================================
-- 1) Очистка после неудачной попытки
-- ===========================================================================
DROP TABLE IF EXISTS CloudStreaming_AccountOwnedGames_new;

-- ===========================================================================
-- 2) Таблица каталога
-- ===========================================================================
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

-- ===========================================================================
-- 3) Колонка CatalogID в CloudStreaming_Games
--    Ошибка #1060 Duplicate column — уже есть, пропустите.
-- ===========================================================================
ALTER TABLE CloudStreaming_Games
    ADD COLUMN CatalogID BIGINT UNSIGNED NULL COMMENT 'link to synced catalog row' AFTER ID;

-- Ошибка #1826 / duplicate FK — уже есть, пропустите.
ALTER TABLE CloudStreaming_Games
    ADD CONSTRAINT fk_cs_game_catalog FOREIGN KEY (CatalogID) REFERENCES CloudStreaming_Catalog(ID) ON DELETE SET NULL;

-- Заполнить каталог из старых строк Games (ручные записи)
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

-- ===========================================================================
-- 4) Пересборка CloudStreaming_AccountOwnedGames
--
--    СНАЧАЛА выполните вручную:
--      SHOW COLUMNS FROM CloudStreaming_AccountOwnedGames;
--
--    Если в списке УЖЕ есть CatalogID — весь блок 4 НЕ ЗАПУСКАЙТЕ, переходите к блоку 5.
--    Если CatalogID нет — выполните блок 4 целиком.
-- ===========================================================================

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

-- ===========================================================================
-- 5) Представления для phpMyAdmin
-- ===========================================================================
CREATE OR REPLACE VIEW v_cs_catalog_picker AS
SELECT
    c.ID AS CatalogID,
    c.Name,
    c.ServiceType,
    c.Platform,
    c.ProductId,
    c.EntitlementId,
    c.StreamIdentifier,
    c.Category,
    c.LastSyncedAt
FROM CloudStreaming_Catalog c
WHERE c.IsVisible = 1
ORDER BY c.Name;

CREATE OR REPLACE VIEW v_cs_account_owned_games AS
SELECT
    aog.ID,
    aog.AccountID,
    acc.Label AS AccountLabel,
    aog.CatalogID,
    c.Name AS GameName,
    c.ServiceType,
    c.Platform,
    c.StreamIdentifier,
    c.ProductId,
    c.EntitlementId,
    aog.GameID,
    aog.CreatedAt
FROM CloudStreaming_AccountOwnedGames aog
JOIN CloudStreaming_Accounts acc ON acc.ID = aog.AccountID
JOIN CloudStreaming_Catalog c ON c.ID = aog.CatalogID;

SET FOREIGN_KEY_CHECKS = 1;

-- ===========================================================================
-- 6) Проверка (без information_schema)
-- ===========================================================================
SHOW COLUMNS FROM CloudStreaming_Games LIKE 'CatalogID';
SHOW COLUMNS FROM CloudStreaming_AccountOwnedGames LIKE 'CatalogID';
SELECT COUNT(*) AS catalog_rows FROM CloudStreaming_Catalog;
