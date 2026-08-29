ALTER TABLE `autoplay_config`
  ADD COLUMN `sit_recovery` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `autoloot_types`,
  ADD COLUMN `sit_hp_below` TINYINT UNSIGNED NOT NULL DEFAULT 30 AFTER `sit_recovery`,
  ADD COLUMN `sit_sp_below` TINYINT UNSIGNED NOT NULL DEFAULT 20 AFTER `sit_hp_below`;
