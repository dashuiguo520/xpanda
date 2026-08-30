ALTER TABLE `autoplay_config`
  ADD COLUMN `follow_offline_action` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `auto_accept_party`;
