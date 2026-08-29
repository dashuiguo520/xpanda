ALTER TABLE `autoplay_config`
  ADD COLUMN `autoloot_types` SMALLINT UNSIGNED NOT NULL DEFAULT 0 AFTER `autoloot_rate`;

ALTER TABLE `autoplay_discard_item`
  ADD COLUMN `keep_amount` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `item_id`;

CREATE TABLE IF NOT EXISTS `autoplay_loot_item` (
  `char_id` INT UNSIGNED NOT NULL,
  `slot` TINYINT UNSIGNED NOT NULL,
  `item_id` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`char_id`,`slot`)
) ENGINE=InnoDB;
