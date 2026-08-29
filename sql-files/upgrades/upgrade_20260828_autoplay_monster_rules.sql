CREATE TABLE IF NOT EXISTS `autoplay_monster_rule` (
  `char_id` INT UNSIGNED NOT NULL,
  `slot` TINYINT UNSIGNED NOT NULL,
  `mob_id` INT UNSIGNED NOT NULL,
  `action` TINYINT UNSIGNED NOT NULL,
  PRIMARY KEY (`char_id`,`slot`),
  KEY `mob_id` (`mob_id`)
) ENGINE=InnoDB;
