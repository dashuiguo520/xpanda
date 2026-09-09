-- ===== rAthena 修复挂机数据库升级重复执行：开始 =====
-- 修改原因：基础建表脚本已可能包含这些字段，升级时应允许重复执行且不依赖字段顺序。
SET @autoplay_upgrade_sql = IF(
  EXISTS(SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'autoplay_config' AND COLUMN_NAME = 'sit_recovery'),
  'DO 0', 'ALTER TABLE `autoplay_config` ADD COLUMN `sit_recovery` TINYINT UNSIGNED NOT NULL DEFAULT 0');
PREPARE autoplay_upgrade_stmt FROM @autoplay_upgrade_sql;
EXECUTE autoplay_upgrade_stmt;
DEALLOCATE PREPARE autoplay_upgrade_stmt;

SET @autoplay_upgrade_sql = IF(
  EXISTS(SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'autoplay_config' AND COLUMN_NAME = 'sit_hp_below'),
  'DO 0', 'ALTER TABLE `autoplay_config` ADD COLUMN `sit_hp_below` TINYINT UNSIGNED NOT NULL DEFAULT 30');
PREPARE autoplay_upgrade_stmt FROM @autoplay_upgrade_sql;
EXECUTE autoplay_upgrade_stmt;
DEALLOCATE PREPARE autoplay_upgrade_stmt;

SET @autoplay_upgrade_sql = IF(
  EXISTS(SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'autoplay_config' AND COLUMN_NAME = 'sit_sp_below'),
  'DO 0', 'ALTER TABLE `autoplay_config` ADD COLUMN `sit_sp_below` TINYINT UNSIGNED NOT NULL DEFAULT 20');
PREPARE autoplay_upgrade_stmt FROM @autoplay_upgrade_sql;
EXECUTE autoplay_upgrade_stmt;
DEALLOCATE PREPARE autoplay_upgrade_stmt;
-- ===== rAthena 修复挂机数据库升级重复执行：结束 =====
