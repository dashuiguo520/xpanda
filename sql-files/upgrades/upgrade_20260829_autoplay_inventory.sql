ALTER TABLE `autoplay_config`
  ADD COLUMN `inventory_stop_at` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `auto_accept_party`;

CREATE TABLE IF NOT EXISTS `autoplay_discard_item` (
  `char_id` INT UNSIGNED NOT NULL,
  `slot` TINYINT UNSIGNED NOT NULL,
  `item_id` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`char_id`,`slot`)
) ENGINE=InnoDB;
