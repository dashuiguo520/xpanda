ALTER TABLE `autoplay_config`
  ADD COLUMN `mode` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `surrounded_teleport_count`,
  ADD COLUMN `boss_action` TINYINT UNSIGNED NOT NULL DEFAULT 2 AFTER `mode`,
  ADD COLUMN `miss_teleport` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `boss_action`,
  ADD COLUMN `storage_weight_at` TINYINT UNSIGNED NOT NULL DEFAULT 70 AFTER `miss_teleport`;
