-- Per-lease daily play + save-freeze accounting.
-- Run: mysql -h <host> -u <user> -p <database> < migrate_daily_play_per_lease_v8.sql
-- (Also applied automatically by cloud_billing_udp_server on startup.)

SET NAMES utf8mb4;

-- 1) Add lease/account columns if missing
SET @has_lease := (
    SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME = 'CloudStreaming_DailyPlay'
      AND COLUMN_NAME = 'LeaseID'
);
SET @sql := IF(
    @has_lease = 0,
    'ALTER TABLE CloudStreaming_DailyPlay
        ADD COLUMN LeaseID BIGINT UNSIGNED NULL AFTER UserID,
        ADD COLUMN AccountID BIGINT UNSIGNED NULL AFTER LeaseID',
    'SELECT 1'
);
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- 2) Backfill: attach orphan daily rows to the user's earliest lease
UPDATE CloudStreaming_DailyPlay d
JOIN (
    SELECT UserID, MIN(ID) AS LeaseID
    FROM CloudStreaming_Leases
    GROUP BY UserID
) x ON x.UserID = d.UserID
JOIN CloudStreaming_Leases l ON l.ID = x.LeaseID
SET d.LeaseID = l.ID,
    d.AccountID = l.AccountID
WHERE d.LeaseID IS NULL;

DELETE FROM CloudStreaming_DailyPlay WHERE LeaseID IS NULL;

UPDATE CloudStreaming_DailyPlay d
JOIN CloudStreaming_Leases l ON l.ID = d.LeaseID
SET d.AccountID = l.AccountID
WHERE d.AccountID IS NULL OR d.AccountID <> l.AccountID;

-- 3) Rebuild primary key to (LeaseID, PlayDateMSK)
SET @pk_has_lease := (
    SELECT COUNT(*) FROM information_schema.KEY_COLUMN_USAGE
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME = 'CloudStreaming_DailyPlay'
      AND CONSTRAINT_NAME = 'PRIMARY'
      AND COLUMN_NAME = 'LeaseID'
);

-- Drop known FKs so PK can change
SET @fk := (
    SELECT CONSTRAINT_NAME FROM information_schema.TABLE_CONSTRAINTS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME = 'CloudStreaming_DailyPlay'
      AND CONSTRAINT_TYPE = 'FOREIGN KEY'
      AND CONSTRAINT_NAME = 'fk_cs_daily_user'
    LIMIT 1
);
SET @sql := IF(
    @fk IS NOT NULL,
    CONCAT('ALTER TABLE CloudStreaming_DailyPlay DROP FOREIGN KEY ', @fk),
    'SELECT 1'
);
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @fk2 := (
    SELECT CONSTRAINT_NAME FROM information_schema.TABLE_CONSTRAINTS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME = 'CloudStreaming_DailyPlay'
      AND CONSTRAINT_TYPE = 'FOREIGN KEY'
      AND CONSTRAINT_NAME = 'fk_cs_daily_lease'
    LIMIT 1
);
SET @sql := IF(
    @fk2 IS NOT NULL,
    CONCAT('ALTER TABLE CloudStreaming_DailyPlay DROP FOREIGN KEY ', @fk2),
    'SELECT 1'
);
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @sql := IF(
    @pk_has_lease = 0,
    'ALTER TABLE CloudStreaming_DailyPlay DROP PRIMARY KEY,
     MODIFY LeaseID BIGINT UNSIGNED NOT NULL,
     MODIFY AccountID BIGINT UNSIGNED NOT NULL,
     ADD PRIMARY KEY (LeaseID, PlayDateMSK)',
    'ALTER TABLE CloudStreaming_DailyPlay
     MODIFY LeaseID BIGINT UNSIGNED NOT NULL,
     MODIFY AccountID BIGINT UNSIGNED NOT NULL'
);
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- 4) Indexes + FKs
SET @idx := (
    SELECT COUNT(*) FROM information_schema.STATISTICS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME = 'CloudStreaming_DailyPlay'
      AND INDEX_NAME = 'idx_cs_daily_user_date'
);
SET @sql := IF(
    @idx = 0,
    'ALTER TABLE CloudStreaming_DailyPlay ADD KEY idx_cs_daily_user_date (UserID, PlayDateMSK)',
    'SELECT 1'
);
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @fk_user := (
    SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME = 'CloudStreaming_DailyPlay'
      AND CONSTRAINT_NAME = 'fk_cs_daily_user'
);
SET @sql := IF(
    @fk_user = 0,
    'ALTER TABLE CloudStreaming_DailyPlay
     ADD CONSTRAINT fk_cs_daily_user FOREIGN KEY (UserID) REFERENCES tableu(ID) ON DELETE CASCADE',
    'SELECT 1'
);
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @fk_lease := (
    SELECT COUNT(*) FROM information_schema.TABLE_CONSTRAINTS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME = 'CloudStreaming_DailyPlay'
      AND CONSTRAINT_NAME = 'fk_cs_daily_lease'
);
SET @sql := IF(
    @fk_lease = 0,
    'ALTER TABLE CloudStreaming_DailyPlay
     ADD CONSTRAINT fk_cs_daily_lease FOREIGN KEY (LeaseID) REFERENCES CloudStreaming_Leases(ID) ON DELETE CASCADE',
    'SELECT 1'
);
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
