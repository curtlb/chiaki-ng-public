-- Cloud Streaming hourly billing schema
-- Run: mysql -h <host> -u <user> -p <database> < schema.sql

SET NAMES utf8mb4;
SET FOREIGN_KEY_CHECKS = 0;

-- ---------------------------------------------------------------------------
-- Users (4cloud email)
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS CloudStreaming_Users (
    ID              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    User            VARCHAR(255) NOT NULL COMMENT '4cloud email',
    PlayedMinutes   INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'aggregate stat only',
    LastPlayedAt    DATETIME(3) NULL,
    MaxConcurrent   TINYINT UNSIGNED NOT NULL DEFAULT 2,
    Status          ENUM('active','blocked') NOT NULL DEFAULT 'active',
    CreatedAt       DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    UpdatedAt       DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    PRIMARY KEY (ID),
    UNIQUE KEY uq_cs_user_email (User)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------------
-- Cloud gaming payment credentials (separate from console rental `autobilling`)
-- Robokassa recurring parent invoice (StartPaymentID) for hourly charges only.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS CloudStreaming_PaymentMethods (
    ID              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    UserID          BIGINT UNSIGNED NOT NULL,
    Email           VARCHAR(255) NOT NULL COMMENT '4cloud email',
    StartPaymentID  VARCHAR(64) NOT NULL COMMENT 'Robokassa recurring parent invoice',
    Status          ENUM('active','disabled') NOT NULL DEFAULT 'active',
    CreatedAt       DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    UpdatedAt       DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    PRIMARY KEY (ID),
    UNIQUE KEY uq_cs_pay_user (UserID),
    UNIQUE KEY uq_cs_pay_email (Email),
    CONSTRAINT fk_cs_pay_user FOREIGN KEY (UserID) REFERENCES CloudStreaming_Users(ID) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------------
-- Synced Sony catalog (filled hourly by catalog_sync_service.py)
-- Pick CatalogID when linking games to rental accounts.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS CloudStreaming_Catalog (
    ID              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    CatalogKey      VARCHAR(200) NOT NULL COMMENT 'service:stream_id',
    Name            VARCHAR(512) NOT NULL COMMENT 'human-readable title (use in phpMyAdmin FK display)',
    PickerLabel     VARCHAR(720) GENERATED ALWAYS AS (
                        CONCAT(
                            Name,
                            ' [', ServiceType, '] ',
                            CASE
                                WHEN LOCATE('CUSA', COALESCE(ProductId, StreamIdentifier, '')) > 0 THEN
                                    CONCAT('CUSA', SUBSTRING_INDEX(SUBSTRING_INDEX(COALESCE(ProductId, StreamIdentifier), 'CUSA', -1), '_', 1))
                                WHEN LOCATE('PPSA', COALESCE(ProductId, StreamIdentifier, '')) > 0 THEN
                                    CONCAT('PPSA', SUBSTRING_INDEX(SUBSTRING_INDEX(COALESCE(ProductId, StreamIdentifier), 'PPSA', -1), '_', 1))
                                ELSE LEFT(COALESCE(ProductId, StreamIdentifier, ''), 16)
                            END,
                            ' • ',
                            CASE
                                WHEN LEFT(COALESCE(ProductId, StreamIdentifier, ''), 2) IN ('EP', 'EE', 'EC') THEN 'EU'
                                WHEN LEFT(COALESCE(ProductId, StreamIdentifier, ''), 2) = 'UP' THEN 'US'
                                WHEN LOCATE('CUSA', COALESCE(ProductId, StreamIdentifier, '')) > 0 THEN 'US'
                                WHEN LOCATE('PPSA', COALESCE(ProductId, StreamIdentifier, '')) > 0
                                     AND LEFT(COALESCE(ProductId, StreamIdentifier, ''), 2) = 'UP' THEN 'US'
                                WHEN LOCATE('PPSA', COALESCE(ProductId, StreamIdentifier, '')) > 0 THEN 'EU'
                                ELSE '?'
                            END
                        )
                    ) STORED COMMENT 'phpMyAdmin FK: Name + CUSA/PPSA + region hint',
    ServiceType     ENUM('psnow','pscloud') NOT NULL,
    Platform        ENUM('ps3','ps4','ps5','unknown') NOT NULL DEFAULT 'unknown',
    ProductId       VARCHAR(128) NULL,
    EntitlementId   VARCHAR(128) NULL,
    StreamIdentifier VARCHAR(128) NOT NULL COMMENT 'ID sent by chiaki-ng as game_identifier',
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
    KEY idx_cs_catalog_picker (PickerLabel(191)),
    KEY idx_cs_catalog_service (ServiceType, Platform),
    KEY idx_cs_catalog_stream (StreamIdentifier),
    KEY idx_cs_catalog_product (ProductId),
    KEY idx_cs_catalog_entitlement (EntitlementId)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------------
-- Game catalog (hourly price per title)
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS CloudStreaming_Games (
    ID              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    CatalogID       BIGINT UNSIGNED NULL COMMENT 'link to synced catalog row',
    Code            VARCHAR(64) NOT NULL,
    Name            VARCHAR(255) NOT NULL,
    ServiceType     ENUM('pscloud','psnow') NOT NULL,
    GameIdentifier  VARCHAR(128) NOT NULL COMMENT 'product_id (psnow) or entitlement_id (pscloud)',
    AccessType      ENUM('owned_only','ps_plus','both') NOT NULL DEFAULT 'both',
    HourlyPrice     DECIMAL(10,2) NOT NULL DEFAULT 110.00,
    Currency        CHAR(3) NOT NULL DEFAULT 'RUB',
    IsActive        TINYINT(1) NOT NULL DEFAULT 1,
    CreatedAt       DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (ID),
    UNIQUE KEY uq_cs_game_service_id (ServiceType, GameIdentifier),
    KEY idx_cs_game_code (Code),
    KEY idx_cs_game_catalog (CatalogID),
    CONSTRAINT fk_cs_game_catalog FOREIGN KEY (CatalogID) REFERENCES CloudStreaming_Catalog(ID) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------------
-- PS account pool (rental inventory)
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS CloudStreaming_Accounts (
    ID              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    Label           VARCHAR(64) NOT NULL,
    NPSSO           TEXT NOT NULL COMMENT 'store encrypted in production',
    NPSSO_Exp_Date  DATETIME(3) NULL,
    HasPsPlus       TINYINT(1) NOT NULL DEFAULT 1,
    Region          VARCHAR(8) NOT NULL DEFAULT 'PL',
    Status          ENUM('available','leased','maintenance','disabled','expired') NOT NULL DEFAULT 'available',
    CurrentLeaseID  BIGINT UNSIGNED NULL,
    LastUsedAt      DATETIME(3) NULL,
    CreatedAt       DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    UpdatedAt       DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    PRIMARY KEY (ID),
    KEY idx_cs_acc_pool (Status, HasPsPlus, Region)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- Owned / purchased titles on a rental PS account (pick CatalogID from CloudStreaming_Catalog)
CREATE TABLE IF NOT EXISTS CloudStreaming_AccountOwnedGames (
    ID              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    AccountID       BIGINT UNSIGNED NOT NULL,
    CatalogID       BIGINT UNSIGNED NOT NULL COMMENT 'FK CloudStreaming_Catalog — pick from v_cs_catalog_picker',
    GameID          BIGINT UNSIGNED NULL COMMENT 'optional; links CloudStreaming_Games (billing price). NULL = auto on first play',
    CreatedAt       DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (ID),
    UNIQUE KEY uq_cs_aog (AccountID, CatalogID),
    CONSTRAINT fk_cs_aog_account FOREIGN KEY (AccountID) REFERENCES CloudStreaming_Accounts(ID) ON DELETE CASCADE,
    CONSTRAINT fk_cs_aog_catalog FOREIGN KEY (CatalogID) REFERENCES CloudStreaming_Catalog(ID) ON DELETE CASCADE,
    CONSTRAINT fk_cs_aog_game FOREIGN KEY (GameID) REFERENCES CloudStreaming_Games(ID) ON DELETE SET NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------------
-- Account lease: user keeps account (saves) for 3 days after last activity
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS CloudStreaming_Leases (
    ID                  BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    UserID              BIGINT UNSIGNED NOT NULL,
    AccountID           BIGINT UNSIGNED NOT NULL,
    Status              ENUM('active','retention','expired','released') NOT NULL DEFAULT 'active',
    FirstAssignedAt     DATETIME(3) NOT NULL,
    LastActivityAt      DATETIME(3) NOT NULL,
    RetentionUntil      DATETIME(3) NOT NULL COMMENT 'last_activity + 3 days',
    ReleasedAt          DATETIME(3) NULL,
    ReleaseReason       VARCHAR(64) NULL,
    CreatedAt           DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    UpdatedAt           DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    PRIMARY KEY (ID),
    KEY idx_cs_lease_user (UserID, Status, RetentionUntil),
    KEY idx_cs_lease_account (AccountID, Status, RetentionUntil),
    KEY idx_cs_lease_expire (Status, RetentionUntil),
    CONSTRAINT fk_cs_lease_user FOREIGN KEY (UserID) REFERENCES CloudStreaming_Users(ID),
    CONSTRAINT fk_cs_lease_account FOREIGN KEY (AccountID) REFERENCES CloudStreaming_Accounts(ID)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------------
-- Live play wallet: ONE row per user (Plus + Owned residual minutes)
-- Past sessions are moved to CloudStreaming_SessionsArchive.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS CloudStreaming_Sessions (
    ID                  BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    SessionToken        CHAR(36) NOT NULL,
    UserID              BIGINT UNSIGNED NOT NULL,
    LeaseID             BIGINT UNSIGNED NOT NULL,
    AccountID           BIGINT UNSIGNED NOT NULL,
    GameID              BIGINT UNSIGNED NOT NULL,
    ServiceType         ENUM('pscloud','psnow') NOT NULL,
    GameIdentifier      VARCHAR(128) NOT NULL,

    Status              ENUM(
                            'pending_payment',
                            'active',
                            'grace_no_stream',
                            'renewal_pending',
                            'ended',
                            'failed'
                        ) NOT NULL DEFAULT 'pending_payment',

    BlockNo             INT NOT NULL DEFAULT 1,
    BlockStartedAt      DATETIME(3) NOT NULL,
    PaidUntil           DATETIME(3) NOT NULL COMMENT 'mirror of active-pool residual',
    RenewAt             DATETIME(3) NOT NULL COMMENT 'PaidUntil - 10 minutes',

    StreamActive        TINYINT(1) NOT NULL DEFAULT 0,
    LastHeartbeatAt     DATETIME(3) NULL,

    EndedAt             DATETIME(3) NULL,
    EndReason           VARCHAR(64) NULL,
    UiMessage           VARCHAR(512) NULL,

    PlusMinutesLeft     INT NOT NULL DEFAULT 0 COMMENT 'residual minutes for Plus/F2P pool',
    OwnedMinutesLeft    INT NOT NULL DEFAULT 0 COMMENT 'residual minutes for owned titles',
    BillingPool         ENUM('plus','owned') NULL,
    BalanceTickAt       DATETIME(3) NULL COMMENT 'last burn tick while StreamActive=1',

    CreatedAt           DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    UpdatedAt           DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),

    PRIMARY KEY (ID),
    UNIQUE KEY uq_cs_session_token (SessionToken),
    UNIQUE KEY uq_cs_sess_user_one (UserID),
    KEY idx_cs_sess_user (UserID, Status),
    KEY idx_cs_sess_lease (LeaseID, Status),
    KEY idx_cs_sess_renew (Status, RenewAt),
    KEY idx_cs_sess_paid (Status, PaidUntil),

    CONSTRAINT fk_cs_sess_user FOREIGN KEY (UserID) REFERENCES CloudStreaming_Users(ID),
    CONSTRAINT fk_cs_sess_lease FOREIGN KEY (LeaseID) REFERENCES CloudStreaming_Leases(ID),
    CONSTRAINT fk_cs_sess_account FOREIGN KEY (AccountID) REFERENCES CloudStreaming_Accounts(ID),
    CONSTRAINT fk_cs_sess_game FOREIGN KEY (GameID) REFERENCES CloudStreaming_Games(ID)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS CloudStreaming_SessionsArchive (
    ID                  BIGINT UNSIGNED NOT NULL,
    SessionToken        CHAR(36) NOT NULL,
    UserID              BIGINT UNSIGNED NOT NULL,
    LeaseID             BIGINT UNSIGNED NOT NULL,
    AccountID           BIGINT UNSIGNED NOT NULL,
    GameID              BIGINT UNSIGNED NOT NULL,
    ServiceType         ENUM('pscloud','psnow') NOT NULL,
    GameIdentifier      VARCHAR(128) NOT NULL,
    Status              VARCHAR(32) NOT NULL,
    BlockNo             INT NOT NULL DEFAULT 1,
    BlockStartedAt      DATETIME(3) NOT NULL,
    PaidUntil           DATETIME(3) NOT NULL,
    RenewAt             DATETIME(3) NOT NULL,
    StreamActive        TINYINT(1) NOT NULL DEFAULT 0,
    LastHeartbeatAt     DATETIME(3) NULL,
    EndedAt             DATETIME(3) NULL,
    EndReason           VARCHAR(64) NULL,
    UiMessage           VARCHAR(512) NULL,
    PlusMinutesLeft     INT NOT NULL DEFAULT 0,
    OwnedMinutesLeft    INT NOT NULL DEFAULT 0,
    BillingPool         ENUM('plus','owned') NULL,
    BalanceTickAt       DATETIME(3) NULL,
    CreatedAt           DATETIME(3) NULL,
    UpdatedAt           DATETIME(3) NULL,
    ArchivedAt          DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    ArchiveReason       VARCHAR(64) NOT NULL DEFAULT 'consolidate',
    PRIMARY KEY (ID),
    KEY idx_cs_sess_arch_user (UserID, ArchivedAt),
    KEY idx_cs_sess_arch_token (SessionToken)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------------
-- Payment charges (Robokassa recurring)
-- SessionID may point at live Sessions or SessionsArchive (no FK).
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS CloudStreaming_Charges (
    ID                  BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    SessionID           BIGINT UNSIGNED NOT NULL,
    UserID              BIGINT UNSIGNED NOT NULL,
    BlockNo             INT NOT NULL,
    Amount              DECIMAL(10,2) NOT NULL,
    Currency            CHAR(3) NOT NULL DEFAULT 'RUB',
    Status              ENUM('pending','succeeded','failed','refunded') NOT NULL DEFAULT 'pending',
    ProviderInvoiceID   VARCHAR(64) NULL,
    ErrorMessage        VARCHAR(512) NULL,
    IdempotencyKey      VARCHAR(128) NOT NULL,
    CreatedAt           DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),

    PRIMARY KEY (ID),
    UNIQUE KEY uq_cs_charge_idem (IdempotencyKey),
    UNIQUE KEY uq_cs_charge_session_block (SessionID, BlockNo),
    KEY idx_cs_charge_user (UserID, CreatedAt),
    CONSTRAINT fk_cs_charge_user FOREIGN KEY (UserID) REFERENCES CloudStreaming_Users(ID)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

SET FOREIGN_KEY_CHECKS = 1;

-- Admin picker: games available to assign to rental accounts
CREATE OR REPLACE VIEW v_cs_catalog_picker AS
SELECT
    c.ID AS CatalogID,
    c.PickerLabel,
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

-- Assignments with human-readable account + game names (use this in phpMyAdmin browse)
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

-- Billing games picker (GameID is optional; table fills on first client play or manual insert)
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

-- Example seed (adjust identifiers to your catalog):
-- INSERT INTO CloudStreaming_Games (Code, Name, ServiceType, GameIdentifier, AccessType, HourlyPrice)
-- VALUES ('ghostship', 'Ghost of Tsushima', 'psnow', 'EP9000-CUSA32709_00-GHOSTSHIP0000000', 'ps_plus', 110.00);
