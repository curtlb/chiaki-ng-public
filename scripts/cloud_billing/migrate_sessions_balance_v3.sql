-- One live session per user + Plus/Owned residual minute balances.
-- Past sessions are moved to CloudStreaming_SessionsArchive by the billing
-- server on startup (ensure_sessions_balance_schema).
--
-- mysql ... < migrate_sessions_balance_v3.sql

SET NAMES utf8mb4;

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

-- Charges may reference archived SessionIDs after move.
SET @fk := (
    SELECT CONSTRAINT_NAME FROM information_schema.KEY_COLUMN_USAGE
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME = 'CloudStreaming_Charges'
      AND CONSTRAINT_NAME = 'fk_cs_charge_session'
    LIMIT 1
);
SET @sql := IF(@fk IS NOT NULL,
    'ALTER TABLE CloudStreaming_Charges DROP FOREIGN KEY fk_cs_charge_session',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @c := (
    SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME = 'CloudStreaming_Sessions'
      AND COLUMN_NAME = 'PlusMinutesLeft'
);
SET @sql := IF(@c = 0,
    'ALTER TABLE CloudStreaming_Sessions ADD COLUMN PlusMinutesLeft INT NOT NULL DEFAULT 0 AFTER UiMessage',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @c := (
    SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME = 'CloudStreaming_Sessions'
      AND COLUMN_NAME = 'OwnedMinutesLeft'
);
SET @sql := IF(@c = 0,
    'ALTER TABLE CloudStreaming_Sessions ADD COLUMN OwnedMinutesLeft INT NOT NULL DEFAULT 0 AFTER PlusMinutesLeft',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @c := (
    SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME = 'CloudStreaming_Sessions'
      AND COLUMN_NAME = 'BillingPool'
);
SET @sql := IF(@c = 0,
    'ALTER TABLE CloudStreaming_Sessions ADD COLUMN BillingPool ENUM(''plus'',''owned'') NULL AFTER OwnedMinutesLeft',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @c := (
    SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME = 'CloudStreaming_Sessions'
      AND COLUMN_NAME = 'BalanceTickAt'
);
SET @sql := IF(@c = 0,
    'ALTER TABLE CloudStreaming_Sessions ADD COLUMN BalanceTickAt DATETIME(3) NULL AFTER BillingPool',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- Unique one-row-per-user is applied by the billing server after it consolidates
-- existing duplicate sessions into the archive.
