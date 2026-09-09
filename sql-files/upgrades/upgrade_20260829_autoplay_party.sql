-- ===== rAthena 修复挂机数据库升级重复执行：开始 =====
-- 修改原因：基础建表脚本已可能包含这些字段，升级时应允许重复执行且不依赖文件名顺序。
SET @autoplay_upgrade_sql = IF(
  EXISTS(SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'autoplay_config' AND COLUMN_NAME = 'follow_char_id'),
  'DO 0', 'ALTER TABLE `autoplay_config` ADD COLUMN `follow_char_id` INT UNSIGNED NOT NULL DEFAULT 0');
PREPARE autoplay_upgrade_stmt FROM @autoplay_upgrade_sql;
EXECUTE autoplay_upgrade_stmt;
DEALLOCATE PREPARE autoplay_upgrade_stmt;

SET @autoplay_upgrade_sql = IF(
  EXISTS(SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'autoplay_config' AND COLUMN_NAME = 'follow_distance'),
  'DO 0', 'ALTER TABLE `autoplay_config` ADD COLUMN `follow_distance` SMALLINT UNSIGNED NOT NULL DEFAULT 3');
PREPARE autoplay_upgrade_stmt FROM @autoplay_upgrade_sql;
EXECUTE autoplay_upgrade_stmt;
DEALLOCATE PREPARE autoplay_upgrade_stmt;

SET @autoplay_upgrade_sql = IF(
  EXISTS(SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'autoplay_config' AND COLUMN_NAME = 'follow_attack'),
  'DO 0', 'ALTER TABLE `autoplay_config` ADD COLUMN `follow_attack` TINYINT UNSIGNED NOT NULL DEFAULT 1');
PREPARE autoplay_upgrade_stmt FROM @autoplay_upgrade_sql;
EXECUTE autoplay_upgrade_stmt;
DEALLOCATE PREPARE autoplay_upgrade_stmt;
-- ===== rAthena 修复挂机数据库升级重复执行：结束 =====
