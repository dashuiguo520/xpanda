// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#ifndef CHAR_INT_AUCTION_CUSTOM_HPP
#define CHAR_INT_AUCTION_CUSTOM_HPP

#include <common/cbasetypes.hpp>

// ===== 拍卖行系统：开始 =====
// Pure SQL persistence + atomic settlement for the project-local auction
// house (ADR A3/A4). The map server owns all game logic; this module only
// reads/writes `auction_custom` + `auction_bid_journal` and prepares RODEX
// mails for every delivery path.
void int_auction_custom_init(void);
void int_auction_custom_final(void);
int32 inter_auction_custom_parse_frommap(int32 fd);
// ===== 拍卖行系统：结束 =====

#endif /* CHAR_INT_AUCTION_CUSTOM_HPP */
