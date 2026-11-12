// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#ifndef COMMON_AUCTION_HPP
#define COMMON_AUCTION_HPP

#include "cbasetypes.hpp"

// ===== 拍卖行系统：开始 =====
// Project-local auction protocol shared by map and char servers.
// Wire format, packet IDs (0x7E20..0x7E3F / inter 0x30B2..0x30B9) and the
// status machine are documented in
// nostalro-client/docs/auction-house-design-2026-11-11.md.

#define AUCTION_PROTOCOL_MAGIC 0x54435541 // "AUCT" on the wire
// v2 adds the atomic batch register (0x7e29 / 0x7e37, inter 0x30BA-BB).
// A v1 client is deliberately gated off by the handshake: an old map server
// would treat the variable-length 0x7e29 as an unknown fixed packet.
#define AUCTION_PROTOCOL_VERSION 2
#define AUCTION_MAX_NAMEID_FILTERS 64
#define AUCTION_MAX_DURATION_SLOTS 5
// A batch is one CZ packet and one char-side SQL transaction. Keep the cap
// small enough that the inter frame (AUCTION_INTER_BATCH_HDR_SIZE + 96 * count)
// stays well inside the socket FIFO, and equal to the default per-character
// listing cap.
#define AUCTION_MAX_BATCH_ITEMS 20
#define AUCTION_CZ_BATCH_ROW_SIZE 12
#define AUCTION_ZC_BATCH_ROW_SIZE 7
#define AUCTION_INTER_BATCH_ROW_SIZE 96
// 0x30BA header, 25 bytes:
//   id.W@0 len.W@2 char_id.L@4 batch_id.Q@8 count.B@16 currency.B@17
//   duration_h.B@18 fee.W@19 deposit.W@21 cap.B@23 reserved.B@24
// The 23-byte v2 draft put batch_id.Q@6, whose low word overwrote char_id's
// high half (150002 became 18930), so the char server could not resolve the
// seller and answered INTERNAL. Keep char_id and batch_id non-overlapping and
// bump this constant instead of hand-writing offsets on either side.
#define AUCTION_INTER_BATCH_HDR_SIZE 25
// Packed `struct item` wire size (common/mmo.hpp). Bumping this requires a
// blob_version bump and a migration path (A7 in the design doc).
#define AUCTION_ITEM_BLOB_SIZE 77
// Version tag stored with every item blob; bump when struct item drifts (A7).
#define AUCTION_ITEM_BLOB_VERSION 1

enum e_auction_currency : uint8 {
	AUCTION_CURRENCY_ZENY = 0,
	AUCTION_CURRENCY_CASH,
	AUCTION_CURRENCY_ANY = 0xFF, // search filter only
};

// CZ_AUCTION_SEARCH carries one `type` byte. Real values are `e_item_type`
// (0..12, common/mmo.hpp); 0xFF has always meant "any". A consumable tab needs
// a group no single item type can express, so 0xFE is a pseudo type that the
// char server expands into `type IN (0,2,10)` (IT_HEALING, IT_USABLE,
// IT_DELAYCONSUME). Never store 0xFE in the table: rows keep the real type.
enum e_auction_type_filter : uint8 {
	AUCTION_TYPE_CONSUMABLE = 0xFE,
	AUCTION_TYPE_ANY = 0xFF, // search filter only
};

enum e_auction_status : uint8 {
	AUCTION_ST_ESCROWING = 0,
	AUCTION_ST_ACTIVE,
	AUCTION_ST_SOLD,
	AUCTION_ST_RETURN_PENDING,
	AUCTION_ST_CLOSED,
	AUCTION_ST_CANCELLED,
};

enum e_auction_operation : uint8 {
	AUCTION_OP_OPEN = 0,
	AUCTION_OP_SEARCH,
	AUCTION_OP_DETAIL,
	AUCTION_OP_REGISTER,
	AUCTION_OP_CANCEL,
	AUCTION_OP_BID,
	AUCTION_OP_BUY_NOW,
	AUCTION_OP_MY_LISTINGS,
	AUCTION_OP_MY_BIDS,
	AUCTION_OP_REGISTER_BATCH = 10, // v2: one packet / one transaction for N rows
};

enum e_auction_result : uint8 {
	AUCTION_RESULT_OK = 0,
	AUCTION_RESULT_FAIL = 1,
};

enum e_auction_reason : uint8 {
	AUCTION_REASON_OK = 0,
	AUCTION_REASON_DISABLED = 1,
	AUCTION_REASON_NO_PERMISSION = 2,
	AUCTION_REASON_NOT_SUPPORTED = 3,
	AUCTION_REASON_ITEM_NOT_FOUND = 4,
	AUCTION_REASON_ITEM_UNTRADEABLE = 5,
	AUCTION_REASON_INVALID_PRICE = 6,
	AUCTION_REASON_INSUFFICIENT_FUNDS = 7,
	AUCTION_REASON_LISTING_CAP = 8,
	AUCTION_REASON_NOT_TOP_BIDDER = 9,
	AUCTION_REASON_ALREADY_ENDED = 10,
	AUCTION_REASON_ALREADY_SOLD = 11,
	AUCTION_REASON_RATE_LIMITED = 12,
	AUCTION_REASON_INTERNAL = 13,
	AUCTION_REASON_DEPOSIT_TOO_LOW = 14,
	AUCTION_REASON_BID_TOO_LOW = 15,
	AUCTION_REASON_CURRENCY_MISMATCH = 16,
	AUCTION_REASON_BATCH_ROLLED_BACK = 17, // v2: this row was fine, a sibling failed
	AUCTION_REASON_TOO_MANY_ITEMS = 18,    // v2: batch count out of range
	AUCTION_REASON_INTER_UNAVAILABLE = 19, // v2: map<->char inter link is down
};

enum e_auction_notify_event : uint8 {
	AUCTION_NOTIFY_OUTBID = 1,
	AUCTION_NOTIFY_SOLD = 2,
	AUCTION_NOTIFY_EXPIRED = 3,
	AUCTION_NOTIFY_RETURNED = 4,
	AUCTION_NOTIFY_DELIVERED = 5,
	AUCTION_NOTIFY_CASH_CHANGED = 6,
};

// Search row layout shared by ZC_AUCTION_SEARCH_RESULT / MY_LISTINGS /
// MY_BIDS; kept here so the char server can build identical rows.
#define AUCTION_SEARCH_ROW_SIZE 45
#define AUCTION_MY_ROW_SIZE 46
// ===== 拍卖行系统：结束 =====

#endif /* COMMON_AUCTION_HPP */
