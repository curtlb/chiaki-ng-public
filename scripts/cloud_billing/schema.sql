-- Cloud Streaming hourly billing schema
-- Run: mysql -h <host> -u <user> -p <database> < schema.sql

SET NAMES utf8mb4;
SET FOREIGN_KEY_CHECKS = 0;

-- ---------------------------------------------------------------------------
-- Users (4cloud email; payment method lives in existing `autobilling` table)
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
-- Game catalog (hourly price per title)
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS CloudStreaming_Games (
    ID              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
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
    KEY idx_cs_game_code (Code)
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

-- Owned games on a PS account (optional; PS Plus uses HasPsPlus flag)
CREATE TABLE IF NOT EXISTS CloudStreaming_AccountOwnedGames (
    ID              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    AccountID       BIGINT UNSIGNED NOT NULL,
    GameID          BIGINT UNSIGNED NOT NULL,
    EntitlementID   VARCHAR(128) NULL,
    CreatedAt       DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (ID),
    UNIQUE KEY uq_cs_aog (AccountID, GameID),
    CONSTRAINT fk_cs_aog_account FOREIGN KEY (AccountID) REFERENCES CloudStreaming_Accounts(ID) ON DELETE CASCADE,
    CONSTRAINT fk_cs_aog_game FOREIGN KEY (GameID) REFERENCES CloudStreaming_Games(ID) ON DELETE CASCADE
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
-- Hourly play session (billing block inside a lease)
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
    PaidUntil           DATETIME(3) NOT NULL,
    RenewAt             DATETIME(3) NOT NULL COMMENT 'paid_until - 10 minutes',

    StreamActive        TINYINT(1) NOT NULL DEFAULT 0,
    LastHeartbeatAt     DATETIME(3) NULL,

    EndedAt             DATETIME(3) NULL,
    EndReason           VARCHAR(64) NULL,
    UiMessage           VARCHAR(512) NULL,

    CreatedAt           DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    UpdatedAt           DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),

    PRIMARY KEY (ID),
    UNIQUE KEY uq_cs_session_token (SessionToken),
    KEY idx_cs_sess_user (UserID, Status),
    KEY idx_cs_sess_lease (LeaseID, Status),
    KEY idx_cs_sess_renew (Status, RenewAt),
    KEY idx_cs_sess_paid (Status, PaidUntil),

    CONSTRAINT fk_cs_sess_user FOREIGN KEY (UserID) REFERENCES CloudStreaming_Users(ID),
    CONSTRAINT fk_cs_sess_lease FOREIGN KEY (LeaseID) REFERENCES CloudStreaming_Leases(ID),
    CONSTRAINT fk_cs_sess_account FOREIGN KEY (AccountID) REFERENCES CloudStreaming_Accounts(ID),
    CONSTRAINT fk_cs_sess_game FOREIGN KEY (GameID) REFERENCES CloudStreaming_Games(ID)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- ---------------------------------------------------------------------------
-- Payment charges (Robokassa recurring)
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
    CONSTRAINT fk_cs_charge_session FOREIGN KEY (SessionID) REFERENCES CloudStreaming_Sessions(ID),
    CONSTRAINT fk_cs_charge_user FOREIGN KEY (UserID) REFERENCES CloudStreaming_Users(ID)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

SET FOREIGN_KEY_CHECKS = 1;

-- Example seed (adjust identifiers to your catalog):
-- INSERT INTO CloudStreaming_Games (Code, Name, ServiceType, GameIdentifier, AccessType, HourlyPrice)
-- VALUES ('ghostship', 'Ghost of Tsushima', 'psnow', 'EP9000-CUSA32709_00-GHOSTSHIP0000000', 'ps_plus', 110.00);
