-- 拍卖行（项目本地自定义协议 0x7E20-0x7E3F）持久化表
-- 规格：nostalro-client/docs/auction-house-design-2026-11-11.md §3
-- 旧 MyISAM `auction` 表保留不动，本表为 escrow/结算专用。

CREATE TABLE IF NOT EXISTS `auction_custom` (
  `auction_id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `escrow_txid` BIGINT UNSIGNED NOT NULL DEFAULT 0,
  `seller_char_id` INT UNSIGNED NOT NULL DEFAULT 0,
  `seller_name` VARCHAR(30) NOT NULL DEFAULT '',
  `buyer_char_id` INT UNSIGNED NOT NULL DEFAULT 0,
  `buyer_name` VARCHAR(30) NOT NULL DEFAULT '',
  `price_now` INT UNSIGNED NOT NULL DEFAULT 0,
  `price_buynow` INT UNSIGNED NOT NULL DEFAULT 0,
  `currency` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `top_bidder_char_id` INT UNSIGNED NOT NULL DEFAULT 0,
  `top_bidder_name` VARCHAR(30) NOT NULL DEFAULT '',
  `deposit_permille` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `fee_permille` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `status` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `expire_time` INT UNSIGNED NOT NULL DEFAULT 0,
  `nameid` INT UNSIGNED NOT NULL DEFAULT 0,
  `amount` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `refine` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `type` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `card0` INT UNSIGNED NOT NULL DEFAULT 0,
  `card1` INT UNSIGNED NOT NULL DEFAULT 0,
  `card2` INT UNSIGNED NOT NULL DEFAULT 0,
  `card3` INT UNSIGNED NOT NULL DEFAULT 0,
  `option_id0` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `option_val0` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `option_parm0` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `option_id1` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `option_val1` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `option_parm1` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `option_id2` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `option_val2` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `option_parm2` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `option_id3` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `option_val3` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `option_parm3` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `option_id4` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `option_val4` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `option_parm4` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `unique_id` BIGINT UNSIGNED NOT NULL DEFAULT 0,
  `enchantgrade` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `blob_version` SMALLINT UNSIGNED NOT NULL DEFAULT 1,
  `item_blob` BLOB NOT NULL,
  PRIMARY KEY (`auction_id`),
  KEY `idx_search` (`status`, `currency`, `nameid`, `price_now`),
  KEY `idx_expire` (`status`, `expire_time`),
  KEY `idx_seller` (`seller_char_id`, `status`),
  KEY `idx_bidder` (`top_bidder_char_id`, `status`),
  KEY `idx_txid` (`escrow_txid`, `status`),
  -- 板块浏览覆盖索引（审计 P3 性能项：type 过滤 + price_buynow/expire 排序
  -- 原本走 idx_search 之外的 filesort）。存量库需手工执行：
  -- ALTER TABLE `auction_custom`
  --   ADD KEY `idx_board` (`status`, `type`, `price_now`, `price_buynow`, `expire_time`);
  KEY `idx_board` (`status`, `type`, `price_now`, `price_buynow`, `expire_time`)
) ENGINE=InnoDB;

-- 出价流水（ADR §9.2 解法一的对账兜底：竞拍扣款与 char 端受理之间的崩溃
-- 窗口无法完全消除，只报警不自动修复，GM 审计用）
CREATE TABLE IF NOT EXISTS `auction_bid_journal` (
  `journal_id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `auction_id` BIGINT UNSIGNED NOT NULL DEFAULT 0,
  `bidder_char_id` INT UNSIGNED NOT NULL DEFAULT 0,
  `price` INT UNSIGNED NOT NULL DEFAULT 0,
  `currency` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `created_at` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`journal_id`),
  KEY `idx_auction` (`auction_id`),
  KEY `idx_bidder` (`bidder_char_id`)
) ENGINE=InnoDB;
