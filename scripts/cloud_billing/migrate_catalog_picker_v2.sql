-- Widen PickerLabel: game name + CUSA/PPSA + region (EU/US)
-- Run in phpMyAdmin SQL after previous PickerLabel migration.

ALTER TABLE CloudStreaming_Catalog DROP INDEX idx_cs_catalog_picker;
ALTER TABLE CloudStreaming_Catalog DROP COLUMN PickerLabel;

ALTER TABLE CloudStreaming_Catalog
    ADD COLUMN PickerLabel VARCHAR(720) GENERATED ALWAYS AS (
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
    ) STORED
    AFTER Name;

ALTER TABLE CloudStreaming_Catalog
    ADD KEY idx_cs_catalog_picker (PickerLabel(191));

-- Preview:
-- SELECT ID, PickerLabel FROM CloudStreaming_Catalog LIMIT 20;
