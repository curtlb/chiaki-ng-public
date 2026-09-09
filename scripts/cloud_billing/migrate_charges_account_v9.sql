-- Add AccountID to CloudStreaming_Charges for per-account revenue stats.
-- Run: mysql ... < migrate_charges_account_v9.sql
-- (Also applied automatically by cloud_billing_udp_server on startup.)

SET NAMES utf8mb4;

SET @has_account := (
    SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME = 'CloudStreaming_Charges'
      AND COLUMN_NAME = 'AccountID'
);
SET @sql := IF(
    @has_account = 0,
    'ALTER TABLE CloudStreaming_Charges
        ADD COLUMN AccountID BIGINT UNSIGNED NULL
            COMMENT ''PS account active at charge/renew time''
            AFTER UserID,
        ADD KEY idx_cs_charge_account (AccountID, CreatedAt)',
    'SELECT 1'
);
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- Backfill from live/archive sessions when possible
UPDATE CloudStreaming_Charges c
JOIN CloudStreaming_Sessions s ON s.ID = c.SessionID
SET c.AccountID = s.AccountID
WHERE c.AccountID IS NULL AND s.AccountID IS NOT NULL;

UPDATE CloudStreaming_Charges c
JOIN CloudStreaming_SessionsArchive s ON s.ID = c.SessionID
SET c.AccountID = s.AccountID
WHERE c.AccountID IS NULL AND s.AccountID IS NOT NULL;
