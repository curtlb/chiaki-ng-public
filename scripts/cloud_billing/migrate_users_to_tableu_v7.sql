-- Remap CloudStreaming_* .UserID from CloudStreaming_Users.ID -> tableu.ID
-- Match: LOWER(tableu.Email) = LOWER(CloudStreaming_Users.User)
-- Do NOT touch tableu.UserID (legacy Telegram IDs).
--
-- mysql ... < migrate_users_to_tableu_v7.sql
-- Requires existing `tableu` table in the same database.

SET NAMES utf8mb4;

-- ---------------------------------------------------------------------------
-- 1) Drop FKs that point at CloudStreaming_Users
-- ---------------------------------------------------------------------------
DROP PROCEDURE IF EXISTS cs_drop_fk_if_exists;
DELIMITER //
CREATE PROCEDURE cs_drop_fk_if_exists(IN p_table VARCHAR(64), IN p_fk VARCHAR(64))
BEGIN
    IF EXISTS (
        SELECT 1 FROM information_schema.TABLE_CONSTRAINTS
        WHERE CONSTRAINT_SCHEMA = DATABASE()
          AND TABLE_NAME = p_table
          AND CONSTRAINT_NAME = p_fk
          AND CONSTRAINT_TYPE = 'FOREIGN KEY'
    ) THEN
        SET @sql = CONCAT('ALTER TABLE `', p_table, '` DROP FOREIGN KEY `', p_fk, '`');
        PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
    END IF;
END //
DELIMITER ;

CALL cs_drop_fk_if_exists('CloudStreaming_PaymentMethods', 'fk_cs_pay_user');
CALL cs_drop_fk_if_exists('CloudStreaming_Leases', 'fk_cs_lease_user');
CALL cs_drop_fk_if_exists('CloudStreaming_DailyPlay', 'fk_cs_daily_user');
CALL cs_drop_fk_if_exists('CloudStreaming_Sessions', 'fk_cs_sess_user');
CALL cs_drop_fk_if_exists('CloudStreaming_Charges', 'fk_cs_charge_user');
DROP PROCEDURE IF EXISTS cs_drop_fk_if_exists;

-- ---------------------------------------------------------------------------
-- 2) Build old CS user ID -> tableu.ID map (by email)
-- Skip remap if CloudStreaming_Users already retired.
-- ---------------------------------------------------------------------------
DROP TEMPORARY TABLE IF EXISTS cs_user_id_map;
CREATE TEMPORARY TABLE cs_user_id_map (
    OldID BIGINT UNSIGNED NOT NULL PRIMARY KEY,
    NewID BIGINT UNSIGNED NOT NULL,
    Email VARCHAR(255) NOT NULL
);

SET @has_cs_users := (
    SELECT COUNT(*) FROM information_schema.TABLES
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'CloudStreaming_Users'
);

SET @sql := IF(@has_cs_users > 0,
    'INSERT INTO cs_user_id_map (OldID, NewID, Email)
     SELECT cs.ID, MAX(t.ID), MAX(t.Email)
     FROM CloudStreaming_Users cs
     JOIN tableu t ON LOWER(TRIM(t.Email)) = LOWER(TRIM(cs.User))
     GROUP BY cs.ID',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- ---------------------------------------------------------------------------
-- 3) Remap UserID columns (no-op if map empty)
-- ---------------------------------------------------------------------------
UPDATE CloudStreaming_Sessions s
JOIN cs_user_id_map m ON m.OldID = s.UserID
SET s.UserID = m.NewID;

UPDATE CloudStreaming_SessionsArchive s
JOIN cs_user_id_map m ON m.OldID = s.UserID
SET s.UserID = m.NewID;

UPDATE CloudStreaming_Leases l
JOIN cs_user_id_map m ON m.OldID = l.UserID
SET l.UserID = m.NewID;

UPDATE CloudStreaming_Charges c
JOIN cs_user_id_map m ON m.OldID = c.UserID
SET c.UserID = m.NewID;

UPDATE CloudStreaming_PaymentMethods p
JOIN cs_user_id_map m ON m.OldID = p.UserID
SET p.UserID = m.NewID;

UPDATE CloudStreaming_DailyPlay d
JOIN cs_user_id_map m ON m.OldID = d.UserID
SET d.UserID = m.NewID;

-- ---------------------------------------------------------------------------
-- 4) Align UserID column types with tableu.ID (signed/unsigned must match for FK)
-- ---------------------------------------------------------------------------
SET @tableu_id_type := (
    SELECT COLUMN_TYPE FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME = 'tableu'
      AND COLUMN_NAME = 'ID'
    LIMIT 1
);

SET @sql := IF(@tableu_id_type IS NOT NULL,
    CONCAT('ALTER TABLE CloudStreaming_Sessions MODIFY UserID ', @tableu_id_type, ' NOT NULL'),
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @sql := IF(@tableu_id_type IS NOT NULL,
    CONCAT('ALTER TABLE CloudStreaming_SessionsArchive MODIFY UserID ', @tableu_id_type, ' NOT NULL'),
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @sql := IF(@tableu_id_type IS NOT NULL,
    CONCAT('ALTER TABLE CloudStreaming_Leases MODIFY UserID ', @tableu_id_type, ' NOT NULL'),
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @sql := IF(@tableu_id_type IS NOT NULL,
    CONCAT('ALTER TABLE CloudStreaming_Charges MODIFY UserID ', @tableu_id_type, ' NOT NULL'),
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @sql := IF(@tableu_id_type IS NOT NULL,
    CONCAT('ALTER TABLE CloudStreaming_PaymentMethods MODIFY UserID ', @tableu_id_type, ' NOT NULL'),
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @sql := IF(
    @tableu_id_type IS NOT NULL
    AND EXISTS (
        SELECT 1 FROM information_schema.TABLES
        WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'CloudStreaming_DailyPlay'
    ),
    CONCAT('ALTER TABLE CloudStreaming_DailyPlay MODIFY UserID ', @tableu_id_type, ' NOT NULL'),
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- ---------------------------------------------------------------------------
-- 5) Re-add FKs -> tableu(ID). tableu.UserID (Telegram) is never referenced.
-- ---------------------------------------------------------------------------
ALTER TABLE CloudStreaming_PaymentMethods
    ADD CONSTRAINT fk_cs_pay_user FOREIGN KEY (UserID) REFERENCES tableu(ID) ON DELETE CASCADE;

ALTER TABLE CloudStreaming_Leases
    ADD CONSTRAINT fk_cs_lease_user FOREIGN KEY (UserID) REFERENCES tableu(ID);

ALTER TABLE CloudStreaming_Sessions
    ADD CONSTRAINT fk_cs_sess_user FOREIGN KEY (UserID) REFERENCES tableu(ID);

ALTER TABLE CloudStreaming_Charges
    ADD CONSTRAINT fk_cs_charge_user FOREIGN KEY (UserID) REFERENCES tableu(ID);

SET @sql := IF(
    EXISTS (
        SELECT 1 FROM information_schema.TABLES
        WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'CloudStreaming_DailyPlay'
    ),
    'ALTER TABLE CloudStreaming_DailyPlay ADD CONSTRAINT fk_cs_daily_user FOREIGN KEY (UserID) REFERENCES tableu(ID) ON DELETE CASCADE',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- ---------------------------------------------------------------------------
-- 6) Retire CloudStreaming_Users (backup rename; skip if already done)
SET @sql := IF(
    EXISTS (
        SELECT 1 FROM information_schema.TABLES
        WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'CloudStreaming_Users'
    )
    AND NOT EXISTS (
        SELECT 1 FROM information_schema.TABLES
        WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'CloudStreaming_Users_legacy_backup'
    ),
    'RENAME TABLE CloudStreaming_Users TO CloudStreaming_Users_legacy_backup',
    'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
