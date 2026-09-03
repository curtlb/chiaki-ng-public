-- Save freeze: daily play (MSK) + lease SaveFreezeActive flag
-- Run: mysql -h <host> -u <user> -p <database> < migrate_save_retention_v4.sql

SET NAMES utf8mb4;

ALTER TABLE CloudStreaming_Leases
    ADD COLUMN SaveFreezeActive TINYINT(1) NOT NULL DEFAULT 0
        COMMENT '1 = saves frozen until RetentionUntil'
        AFTER RetentionUntil;

UPDATE CloudStreaming_Leases
SET SaveFreezeActive = 1
WHERE Status IN ('active', 'retention')
  AND RetentionUntil > NOW(3);

CREATE TABLE IF NOT EXISTS CloudStreaming_DailyPlay (
    UserID          BIGINT UNSIGNED NOT NULL,
    PlayDateMSK     DATE NOT NULL COMMENT 'calendar day Europe/Moscow',
    StreamSeconds   INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'active stream time this MSK day',
    ExtensionGranted TINYINT(1) NOT NULL DEFAULT 0 COMMENT '+1 day already applied this MSK day',
    CreatedAt       DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    UpdatedAt       DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
    PRIMARY KEY (UserID, PlayDateMSK),
    CONSTRAINT fk_cs_daily_user FOREIGN KEY (UserID) REFERENCES CloudStreaming_Users(ID) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
