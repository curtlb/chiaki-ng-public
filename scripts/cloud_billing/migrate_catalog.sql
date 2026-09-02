-- Migration for existing CloudStreaming deployments (run once on production DB)
-- mysql -h HOST -u USER -p DBNAME < migrate_catalog.sql

SET NAMES utf8mb4;
SET FOREIGN_KEY_CHECKS = 0;

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

ALTER TABLE CloudStreaming_Games
    ADD COLUMN CatalogID BIGINT UNSIGNED NULL COMMENT 'link to synced catalog row' AFTER ID;

-- Rebuild AccountOwnedGames if old schema had GameID+EntitlementID only
CREATE TABLE IF NOT EXISTS CloudStreaming_AccountOwnedGames_new (
    ID              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    AccountID       BIGINT UNSIGNED NOT NULL,
    CatalogID       BIGINT UNSIGNED NOT NULL,
    GameID          BIGINT UNSIGNED NULL,
    CreatedAt       DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (ID),
    UNIQUE KEY uq_cs_aog (AccountID, CatalogID),
    CONSTRAINT fk_cs_aog_account FOREIGN KEY (AccountID) REFERENCES CloudStreaming_Accounts(ID) ON DELETE CASCADE,
    CONSTRAINT fk_cs_aog_catalog FOREIGN KEY (CatalogID) REFERENCES CloudStreaming_Catalog(ID) ON DELETE CASCADE,
    CONSTRAINT fk_cs_aog_game FOREIGN KEY (GameID) REFERENCES CloudStreaming_Games(ID) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- Migrate old rows: match GameIdentifier -> catalog StreamIdentifier when possible
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

INSERT INTO CloudStreaming_AccountOwnedGames_new (AccountID, CatalogID, GameID, CreatedAt)
SELECT aog.AccountID, c.ID, aog.GameID, aog.CreatedAt
FROM CloudStreaming_AccountOwnedGames aog
JOIN CloudStreaming_Games g ON g.ID = aog.GameID
JOIN CloudStreaming_Catalog c ON c.StreamIdentifier = g.GameIdentifier AND c.ServiceType = g.ServiceType;

DROP TABLE IF EXISTS CloudStreaming_AccountOwnedGames;
RENAME TABLE CloudStreaming_AccountOwnedGames_new TO CloudStreaming_AccountOwnedGames;

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

SET FOREIGN_KEY_CHECKS = 1;
