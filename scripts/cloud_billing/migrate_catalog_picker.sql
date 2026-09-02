-- phpMyAdmin: show game title in CatalogID foreign-key picker
-- mysql -h HOST -u USER -p DBNAME < migrate_catalog_picker.sql

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
    ) STORED COMMENT 'phpMyAdmin FK display column'
    AFTER Name;

ALTER TABLE CloudStreaming_Catalog
    ADD KEY idx_cs_catalog_picker (PickerLabel(191));

CREATE OR REPLACE VIEW v_cs_catalog_picker AS
SELECT
    c.ID AS CatalogID,
    c.PickerLabel,
    c.Name,
    c.ServiceType,
    c.Platform,
    c.ProductId,
    c.EntitlementId,
    c.StreamIdentifier,
    c.Category,
    c.LastSyncedAt
FROM CloudStreaming_Catalog c
WHERE c.IsVisible = 1
ORDER BY c.Name;
