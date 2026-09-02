-- Safe catalog migration for EXISTING CloudStreaming DB (idempotent, phpMyAdmin-friendly).
-- Fixes MySQL #1022 (duplicate FK name) from migrate_catalog.sql when the old
-- CloudStreaming_AccountOwnedGames table already defines fk_cs_aog_* constraints.
--
-- Run once:
--   mysql -h HOST -u USER -p DBNAME < migrate_catalog_v2.sql
--
-- If a previous attempt failed halfway, this script cleans up *_new leftovers first.

SET NAMES utf8mb4;
SET FOREIGN_KEY_CHECKS = 0;

-- ---------------------------------------------------------------------------
-- 0) Cleanup partial runs
-- ---------------------------------------------------------------------------
DROP TABLE IF EXISTS CloudStreaming_AccountOwnedGames_new;

-- ---------------------------------------------------------------------------
-- 1) Catalog table
-- ---------------------------------------------------------------------------
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

-- ---------------------------------------------------------------------------
-- 2) CloudStreaming_Games.CatalogID (skip if already present)
-- ---------------------------------------------------------------------------
SET @games_has_catalog := (
    SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME = 'CloudStreaming_Games'
      AND COLUMN_NAME = 'CatalogID'
);
SET @sql_games := IF(
    @games_has_catalog = 0,
    'ALTER TABLE CloudStreaming_Games ADD COLUMN CatalogID BIGINT UNSIGNED NULL COMMENT ''link to synced catalog row'' AFTER ID',
    'SELECT ''CloudStreaming_Games.CatalogID already exists'' AS info'
);
PREPARE stmt_games FROM @sql_games;
EXECUTE stmt_games;
DEALLOCATE PREPARE stmt_games;

-- Optional FK on Games.CatalogID (ignore if exists)
SET @games_fk_exists := (
    SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME = 'CloudStreaming_Games'
      AND CONSTRAINT_NAME = 'fk_cs_game_catalog'
      AND CONSTRAINT_TYPE = 'FOREIGN KEY'
);
SET @sql_games_fk := IF(
    @games_fk_exists = 0,
    'ALTER TABLE CloudStreaming_Games ADD CONSTRAINT fk_cs_game_catalog FOREIGN KEY (CatalogID) REFERENCES CloudStreaming_Catalog(ID) ON DELETE SET NULL',
    'SELECT ''fk_cs_game_catalog already exists'' AS info'
);
PREPARE stmt_games_fk FROM @sql_games_fk;
EXECUTE stmt_games_fk;
DEALLOCATE PREPARE stmt_games_fk;

-- Seed catalog rows from legacy CloudStreaming_Games (manual entries)
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

-- ---------------------------------------------------------------------------
-- 3) Rebuild AccountOwnedGames ONLY when CatalogID column is still missing
-- ---------------------------------------------------------------------------
SET @aog_has_catalog := (
    SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME = 'CloudStreaming_AccountOwnedGames'
      AND COLUMN_NAME = 'CatalogID'
);

-- Unique FK names for the staging table (avoids #1022 vs old table constraints)
SET @sql_aog_rebuild := IF(
    @aog_has_catalog = 0,
    'CREATE TABLE CloudStreaming_AccountOwnedGames_new (
        ID BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
        AccountID BIGINT UNSIGNED NOT NULL,
        CatalogID BIGINT UNSIGNED NOT NULL,
        GameID BIGINT UNSIGNED NULL,
        CreatedAt DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
        PRIMARY KEY (ID),
        UNIQUE KEY uq_cs_aog_mig (AccountID, CatalogID),
        CONSTRAINT fk_cs_aog_mig_account FOREIGN KEY (AccountID) REFERENCES CloudStreaming_Accounts(ID) ON DELETE CASCADE,
        CONSTRAINT fk_cs_aog_mig_catalog FOREIGN KEY (CatalogID) REFERENCES CloudStreaming_Catalog(ID) ON DELETE CASCADE,
        CONSTRAINT fk_cs_aog_mig_game FOREIGN KEY (GameID) REFERENCES CloudStreaming_Games(ID) ON DELETE SET NULL
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci',
    'SELECT ''CloudStreaming_AccountOwnedGames already has CatalogID — skip rebuild'' AS info'
);
PREPARE stmt_aog_create FROM @sql_aog_rebuild;
EXECUTE stmt_aog_create;
DEALLOCATE PREPARE stmt_aog_create;

SET @sql_aog_copy := IF(
    @aog_has_catalog = 0,
    'INSERT INTO CloudStreaming_AccountOwnedGames_new (AccountID, CatalogID, GameID, CreatedAt)
     SELECT aog.AccountID, c.ID, aog.GameID, aog.CreatedAt
     FROM CloudStreaming_AccountOwnedGames aog
     JOIN CloudStreaming_Games g ON g.ID = aog.GameID
     JOIN CloudStreaming_Catalog c ON c.StreamIdentifier = g.GameIdentifier AND c.ServiceType = g.ServiceType',
    'SELECT ''skip copy'' AS info'
);
PREPARE stmt_aog_copy FROM @sql_aog_copy;
EXECUTE stmt_aog_copy;
DEALLOCATE PREPARE stmt_aog_copy;

SET @sql_aog_swap := IF(
    @aog_has_catalog = 0,
    'DROP TABLE CloudStreaming_AccountOwnedGames',
    'SELECT ''skip drop'' AS info'
);
PREPARE stmt_aog_drop FROM @sql_aog_swap;
EXECUTE stmt_aog_drop;
DEALLOCATE PREPARE stmt_aog_drop;

SET @sql_aog_rename := IF(
    @aog_has_catalog = 0,
    'RENAME TABLE CloudStreaming_AccountOwnedGames_new TO CloudStreaming_AccountOwnedGames',
    'SELECT ''skip rename'' AS info'
);
PREPARE stmt_aog_rename FROM @sql_aog_rename;
EXECUTE stmt_aog_rename;
DEALLOCATE PREPARE stmt_aog_rename;

-- ---------------------------------------------------------------------------
-- 4) Admin views
-- ---------------------------------------------------------------------------
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

-- ---------------------------------------------------------------------------
-- 5) Sanity check (should return 3 rows with cnt=1)
-- ---------------------------------------------------------------------------
SELECT 'CloudStreaming_Games.CatalogID' AS check_name,
       COUNT(*) AS cnt
FROM information_schema.COLUMNS
WHERE TABLE_SCHEMA = DATABASE()
  AND TABLE_NAME = 'CloudStreaming_Games'
  AND COLUMN_NAME = 'CatalogID'
UNION ALL
SELECT 'CloudStreaming_AccountOwnedGames.CatalogID',
       COUNT(*)
FROM information_schema.COLUMNS
WHERE TABLE_SCHEMA = DATABASE()
  AND TABLE_NAME = 'CloudStreaming_AccountOwnedGames'
  AND COLUMN_NAME = 'CatalogID'
UNION ALL
SELECT 'CloudStreaming_Catalog table',
       COUNT(*)
FROM information_schema.TABLES
WHERE TABLE_SCHEMA = DATABASE()
  AND TABLE_NAME = 'CloudStreaming_Catalog';
