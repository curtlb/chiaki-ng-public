-- Single residual minute balance (burns only while StreamActive=1).
-- PaidUntil becomes a display mirror only; MinutesLeft is the source of truth.
--
-- mysql ... < migrate_minutes_balance_v6.sql
-- Billing server also applies this on startup via migrate_to_minutes_balance().

SET NAMES utf8mb4;

SET @c := (
    SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME = 'CloudStreaming_Sessions'
      AND COLUMN_NAME = 'MinutesLeft'
);
SET @sql := IF(@c = 0,
    'ALTER TABLE CloudStreaming_Sessions ADD COLUMN MinutesLeft INT NOT NULL DEFAULT 0 AFTER UiMessage',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @c := (
    SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME = 'CloudStreaming_SessionsArchive'
      AND COLUMN_NAME = 'MinutesLeft'
);
SET @sql := IF(@c = 0,
    'ALTER TABLE CloudStreaming_SessionsArchive ADD COLUMN MinutesLeft INT NOT NULL DEFAULT 0 AFTER UiMessage',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- Seed from the best of: existing MinutesLeft, Plus+Owned, residual from PaidUntil.
UPDATE CloudStreaming_Sessions
SET MinutesLeft = GREATEST(
    COALESCE(MinutesLeft, 0),
    COALESCE(PlusMinutesLeft, 0) + COALESCE(OwnedMinutesLeft, 0),
    GREATEST(0, TIMESTAMPDIFF(MINUTE, NOW(3), PaidUntil))
),
PlusMinutesLeft = 0,
OwnedMinutesLeft = 0;

UPDATE CloudStreaming_Sessions
SET
    PaidUntil = DATE_ADD(NOW(3), INTERVAL MinutesLeft MINUTE),
    RenewAt = DATE_ADD(NOW(3), INTERVAL GREATEST(0, MinutesLeft - 10) MINUTE);
