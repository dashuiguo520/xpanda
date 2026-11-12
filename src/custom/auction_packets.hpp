#ifndef CUSTOM_AUCTION_PACKETS_HPP
#define CUSTOM_AUCTION_PACKETS_HPP

#include <common/auction.hpp>

// Project-local auction protocol: keep this range separate from upstream
// packets. Sizes are frozen in
// nostalro-client/docs/auction-house-design-2026-11-11.md §2.

// ===== 拍卖行系统：开始 =====
#if !defined(sun) && (!defined(__NETBSD__) || __NetBSD_Version__ >= 600000000)
#pragma pack(push, 1)
#endif
struct PACKET_CZ_AUCTION_OPEN { uint16 packetType; uint32 magic; uint16 protocolVersion; } __attribute__((packed));
DEFINE_PACKET_HEADER(CZ_AUCTION_OPEN, 0x7e20);
struct PACKET_CZ_AUCTION_SEARCH { uint16 packetType; uint8 type; uint8 refineMin; uint8 refineMax; uint32 priceMin; uint32 priceMax; uint8 sort; uint8 currency; uint16 page; uint8 nameidCount; uint16 nameids[AUCTION_MAX_NAMEID_FILTERS]; } __attribute__((packed));
DEFINE_PACKET_HEADER(CZ_AUCTION_SEARCH, 0x7e21);
struct PACKET_CZ_AUCTION_DETAIL { uint16 packetType; uint32 auctionId; } __attribute__((packed));
DEFINE_PACKET_HEADER(CZ_AUCTION_DETAIL, 0x7e22);
struct PACKET_CZ_AUCTION_REGISTER { uint16 packetType; uint16 index; uint16 amount; uint32 price; uint32 buyNow; uint8 durationIdx; uint8 currency; } __attribute__((packed));
DEFINE_PACKET_HEADER(CZ_AUCTION_REGISTER, 0x7e23);
struct PACKET_CZ_AUCTION_CANCEL { uint16 packetType; uint32 auctionId; } __attribute__((packed));
DEFINE_PACKET_HEADER(CZ_AUCTION_CANCEL, 0x7e24);
struct PACKET_CZ_AUCTION_BID { uint16 packetType; uint32 auctionId; uint32 price; } __attribute__((packed));
DEFINE_PACKET_HEADER(CZ_AUCTION_BID, 0x7e25);
struct PACKET_CZ_AUCTION_BUY_NOW { uint16 packetType; uint32 auctionId; } __attribute__((packed));
DEFINE_PACKET_HEADER(CZ_AUCTION_BUY_NOW, 0x7e26);
struct PACKET_CZ_AUCTION_MY_LISTINGS { uint16 packetType; uint16 page; uint8 flags; uint8 reserved; } __attribute__((packed));
DEFINE_PACKET_HEADER(CZ_AUCTION_MY_LISTINGS, 0x7e27);
struct PACKET_CZ_AUCTION_MY_BIDS { uint16 packetType; uint16 page; uint8 flags; uint8 reserved; } __attribute__((packed));
DEFINE_PACKET_HEADER(CZ_AUCTION_MY_BIDS, 0x7e28);
// v2 atomic batch: variable length, len.W at offset 2. rows are
// { index.W amount.W price.L buyNow.L } = AUCTION_CZ_BATCH_ROW_SIZE bytes.
// A single packet carries 1..AUCTION_MAX_BATCH_ITEMS rows; the char server
// escrows all of them inside one SQL transaction.
struct PACKET_CZ_AUCTION_REGISTER_BATCH {
	uint16 packetType;
	uint16 length;
	uint8 count;
	uint8 durationIdx;
	uint8 currency;
	uint8 reserved;
} __attribute__((packed));
DEFINE_PACKET_HEADER(CZ_AUCTION_REGISTER_BATCH, 0x7e29);

struct PACKET_ZC_AUCTION_OPEN_ACK {
	uint16 packetType;
	uint8 result;
	uint8 reason;
	uint16 my_listings;
	uint16 my_bids;
	uint16 fee_permille;
	uint16 cash_fee_permille;
	uint8 duration_count;
	uint8 durations[AUCTION_MAX_DURATION_SLOTS];
	uint16 max_listings;
	uint32 min_increment;
	uint32 cash_balance;
} __attribute__((packed));
DEFINE_PACKET_HEADER(ZC_AUCTION_OPEN_ACK, 0x7e30);
// Variable pages: len.W total_pages.W page.W count.B { row }.
struct PACKET_ZC_AUCTION_PAGE_HEADER { uint16 packetType; uint16 length; uint16 total_pages; uint16 page; uint8 count; } __attribute__((packed));
DEFINE_PACKET_HEADER(ZC_AUCTION_SEARCH_RESULT, 0x7e31);
// Full item detail: 77-byte packed struct item blob with its own version tag.
struct PACKET_ZC_AUCTION_DETAIL {
	uint16 packetType;
	uint32 auctionId;
	uint32 priceNow;
	uint32 priceBuyNow;
	uint32 expire;
	uint32 topBidderId;
	char topBidderName[NAME_LENGTH];
	uint16 blobVersion;
	uint8 currency;
	uint8 itemBlob[AUCTION_ITEM_BLOB_SIZE];
} __attribute__((packed));
DEFINE_PACKET_HEADER(ZC_AUCTION_DETAIL, 0x7e32);
struct PACKET_ZC_AUCTION_RESULT {
	uint16 packetType;
	uint8 operation;
	uint8 result;
	uint8 reason;
	uint8 reserved;
	uint32 auctionId;
	uint32 value;
} __attribute__((packed));
DEFINE_PACKET_HEADER(ZC_AUCTION_RESULT, 0x7e33);
DEFINE_PACKET_HEADER(ZC_AUCTION_MY_LISTINGS, 0x7e34);
DEFINE_PACKET_HEADER(ZC_AUCTION_MY_BIDS, 0x7e35);
struct PACKET_ZC_AUCTION_NOTIFY {
	uint16 packetType;
	uint8 event;
	uint8 reason;
	uint16 reserved;
	uint32 auctionId;
	uint32 value;
} __attribute__((packed));
DEFINE_PACKET_HEADER(ZC_AUCTION_NOTIFY, 0x7e36);
// v2 batch receipt: len.W at offset 2, then one 7-byte result per row
// { rowIndex.B result.B reason.B auctionId.L }. Failure is all-or-nothing:
// when the batch result is FAIL every row is FAIL, the offending row carries
// the real reason and its siblings carry AUCTION_REASON_BATCH_ROLLED_BACK.
struct PACKET_ZC_AUCTION_REGISTER_BATCH_RESULT {
	uint16 packetType;
	uint16 length;
	uint8 result;
	uint8 reason;
	uint8 count;
	uint8 failRow;
} __attribute__((packed));
DEFINE_PACKET_HEADER(ZC_AUCTION_REGISTER_BATCH_RESULT, 0x7e37);
#if !defined(sun) && (!defined(__NETBSD__) || __NetBSD_Version__ >= 600000000)
#pragma pack(pop)
#endif

static_assert(sizeof(PACKET_CZ_AUCTION_OPEN) == 8);
static_assert(sizeof(PACKET_CZ_AUCTION_SEARCH) == 146);
static_assert(sizeof(PACKET_CZ_AUCTION_DETAIL) == 6);
static_assert(sizeof(PACKET_CZ_AUCTION_REGISTER) == 16);
static_assert(sizeof(PACKET_CZ_AUCTION_CANCEL) == 6);
static_assert(sizeof(PACKET_CZ_AUCTION_BID) == 10);
static_assert(sizeof(PACKET_CZ_AUCTION_BUY_NOW) == 6);
static_assert(sizeof(PACKET_CZ_AUCTION_MY_LISTINGS) == 6);
static_assert(sizeof(PACKET_CZ_AUCTION_MY_BIDS) == 6);
static_assert(sizeof(PACKET_ZC_AUCTION_OPEN_ACK) == 28);
static_assert(sizeof(PACKET_ZC_AUCTION_PAGE_HEADER) == 9);
static_assert(sizeof(PACKET_ZC_AUCTION_DETAIL) == 126);
// The inter 0x38B2 kind-1 reply ships this exact body size; the map server
// copies it into the ZC frame verbatim, so a struct change must move the macro
// too (and vice versa) or every detail reply gets dropped on the floor.
static_assert(AUCTION_ZC_DETAIL_BODY_SIZE == sizeof(PACKET_ZC_AUCTION_DETAIL) - 2);
static_assert(sizeof(PACKET_ZC_AUCTION_RESULT) == 14);
static_assert(sizeof(PACKET_ZC_AUCTION_NOTIFY) == 14);
// The detail blob mirrors packed struct item; a drift here is a protocol
// change and must come with a blob_version bump (see common/auction.hpp).
static_assert(sizeof(struct item) == AUCTION_ITEM_BLOB_SIZE);
// ===== 拍卖行系统：结束 =====

#endif
