-- ===== rAthena 修复挂机数据库升级重复执行：开始 =====
-- 修改原因：基础建表脚本已可能包含该字段，升级时应允许重复执行且不依赖字段顺序。
SET @autoplay_upgrade_sql = IF(
  EXISTS(SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'autoplay_config' AND COLUMN_NAME = 'inventory_stop_at'),
  'DO 0', 'ALTER TABLE `autoplay_config` ADD COLUMN `inventory_stop_at` TINYINT UNSIGNED NOT NULL DEFAULT 0');
PREPARE autoplay_upgrade_stmt FROM @autoplay_upgrade_sql;
EXECUTE autoplay_upgrade_stmt;
DEALLOCATE PREPARE autoplay_upgrade_stmt;

CREATE TABLE IF NOT EXISTS `autoplay_discard_item` (
  `char_id` INT UNSIGNED NOT NULL,
  `slot` TINYINT UNSIGNED NOT NULL,
  `item_id` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`char_id`,`slot`)
) ENGINE=InnoDB;
-- ===== rAthena 修复挂机数据库升级重复执行：结束 =====
