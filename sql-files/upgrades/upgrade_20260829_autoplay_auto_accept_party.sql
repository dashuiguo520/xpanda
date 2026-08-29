ALTER TABLE `autoplay_config`
  ADD COLUMN `auto_accept_party` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `follow_attack`;
