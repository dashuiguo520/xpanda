--
-- ===== rAthena 移植 Pandas 符文持久化表：开始 =====
-- 修改内容：保存角色已激活的符文书、套装、升级、失败次数和奖励进度。
-- Table structure for table `runes`
--

CREATE TABLE `runes` (
  `char_id` int UNSIGNED NOT NULL,
  `rune_id` int UNSIGNED NOT NULL,
  `set_id` int UNSIGNED NOT NULL,
  `selected` tinyint NOT NULL DEFAULT '0',
  `upgrade` smallint UNSIGNED NOT NULL DEFAULT '0',
  `failcount` tinyint UNSIGNED NOT NULL DEFAULT '0',
  `reward` tinyint(1) UNSIGNED DEFAULT '0',
  UNIQUE KEY `char_id` (`char_id`,`rune_id`,`set_id`)
) ENGINE=MyISAM;

--
-- Table structure for table `runes_book`
--

CREATE TABLE `runes_book` (
  `char_id` int UNSIGNED NOT NULL,
  `rune_id` int UNSIGNED NOT NULL,
  `book_id` int UNSIGNED NOT NULL,
  UNIQUE KEY `char_id` (`char_id`,`rune_id`,`book_id`)
) ENGINE=MyISAM;

-- ===== rAthena 移植 Pandas 符文持久化表：结束 =====
