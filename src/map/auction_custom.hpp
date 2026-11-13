// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#ifndef MAP_AUCTION_CUSTOM_HPP
#define MAP_AUCTION_CUSTOM_HPP

#include <common/cbasetypes.hpp>
#include <vector>

struct map_session_data;
struct PACKET_CZ_AUCTION_SEARCH;

// ===== 拍卖行系统：开始 =====
// Authoritative auction logic (map side). char-server stays a pure SQL
// persistence layer (ADR A3/A4 in the design doc). Wire layouts for the
// inter packets live next to the intif wrappers in intif.cpp and next to the
// parse handlers in int_auction_custom.cpp.

namespace auction_custom {
// v2 atomic batch: one row of CZ_AUCTION_REGISTER_BATCH (12 wire bytes).
struct RegisterRow {
	uint16 index;
	uint16 amount;
	uint32 price;
	uint32 buy_now;
};

// Business requests, called from the clif handlers after the capability and
// permission gates. All of them forward to the char server.
bool request_search(map_session_data& sd, const PACKET_CZ_AUCTION_SEARCH& pkt);
bool request_detail(map_session_data& sd, uint32 auction_id);
bool request_register(map_session_data& sd, uint16 index, uint16 amount, uint32 price, uint32 buy_now, uint8 duration_idx, uint8 currency);
// v2: validate all rows, then escrow them in ONE char-side SQL transaction.
// The inventory is only touched after the whole transaction reports OK.
bool request_register_batch(map_session_data& sd, const std::vector<RegisterRow>& rows, uint8 duration_idx, uint8 currency);
bool request_cancel(map_session_data& sd, uint32 auction_id);
bool request_bid(map_session_data& sd, uint32 auction_id, uint32 price);
bool request_buy_now(map_session_data& sd, uint32 auction_id, uint16 amount);
bool request_my_listings(map_session_data& sd, uint16 page);
bool request_my_bids(map_session_data& sd, uint16 page);

// char→map callbacks (from intif_parse_* wrappers).
void on_escrow_ack(int32 fd);
void on_commit_ack(int32 fd);
void on_escrow_batch_ack(int32 fd);
void on_commit_batch_ack(int32 fd);
void on_bid_ack(int32 fd);
void on_search_or_detail(int32 fd);
void on_op_ack(int32 fd);
void on_notify_forward(int32 fd);
void on_txid_probe(int32 fd);
void on_cash_payout(int32 fd);
void on_listing_counts(uint32 char_id, uint32 listings, uint32 bids);

void do_init_auction_custom(void);
void do_final_auction_custom(void);
}
// ===== 拍卖行系统：结束 =====

#endif /* MAP_AUCTION_CUSTOM_HPP */
