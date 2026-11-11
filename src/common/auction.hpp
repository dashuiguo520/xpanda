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
#define AUCTION_PROTOCOL_VERSION 1
#define AUCTION_MAX_NAMEID_FILTERS 64
#define AUCTION_MAX_DURATION_SLOTS 5
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
