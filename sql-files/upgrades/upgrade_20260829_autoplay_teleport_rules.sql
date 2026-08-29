ALTER TABLE `autoplay_config`
  ADD COLUMN `no_monster_teleport_seconds` SMALLINT UNSIGNED NOT NULL DEFAULT 0 AFTER `teleport_min_interval`,
  ADD COLUMN `surrounded_teleport_count` TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER `no_monster_teleport_seconds`;
