ALTER TABLE `autoplay_config`
  ADD COLUMN `follow_char_id` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `death_action`,
  ADD COLUMN `follow_distance` SMALLINT UNSIGNED NOT NULL DEFAULT 3 AFTER `follow_char_id`,
  ADD COLUMN `follow_attack` TINYINT UNSIGNED NOT NULL DEFAULT 1 AFTER `follow_distance`;
