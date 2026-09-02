-- Separate cloud gaming payment methods from console rental `autobilling`
-- mysql -h HOST -u USER -p DBNAME < migrate_payment_methods.sql

SET NAMES utf8mb4;

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

-- Example: link payment after user binds card for cloud gaming (NOT from autobilling):
-- INSERT INTO CloudStreaming_PaymentMethods (UserID, Email, StartPaymentID)
-- SELECT u.ID, u.User, 'ROBOKASSA_PARENT_INVOICE_ID'
-- FROM CloudStreaming_Users u
-- WHERE u.User = 'curtlb@yandex.ru'
-- LIMIT 1;
