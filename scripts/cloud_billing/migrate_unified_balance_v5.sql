-- Unified play balance: PaidUntil is the source of truth (minutes computed at read time).
-- Legacy PlusMinutesLeft / OwnedMinutesLeft are merged into PaidUntil on billing server startup.
--
-- mysql ... < migrate_unified_balance_v5.sql

SET NAMES utf8mb4;

-- Merge legacy split wallets into PaidUntil where still populated.
UPDATE CloudStreaming_Sessions s
SET
    s.PaidUntil = DATE_ADD(
        GREATEST(COALESCE(s.PaidUntil, NOW(3)), NOW(3)),
        INTERVAL GREATEST(0, s.PlusMinutesLeft + s.OwnedMinutesLeft) MINUTE
    ),
    s.PlusMinutesLeft = 0,
    s.OwnedMinutesLeft = 0
WHERE s.PlusMinutesLeft > 0 OR s.OwnedMinutesLeft > 0;

UPDATE CloudStreaming_Sessions s
SET s.RenewAt = GREATEST(
    NOW(3),
    DATE_SUB(s.PaidUntil, INTERVAL 10 MINUTE)
);
