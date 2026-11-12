#include "auction_custom.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <common/cbasetypes.hpp>
#include <common/mmo.hpp>
#include <common/nullpo.hpp>
#include <common/showmsg.hpp>
#include <common/socket.hpp>
#include <common/timer.hpp>
#include <common/utilities.hpp>

#include "battle.hpp"
#include "chrif.hpp"
#include "clif.hpp"
#include "intif.hpp"
#include "itemdb.hpp"
#include "log.hpp"
#include "map.hpp"
#include "packets_struct.hpp"
#include "pc.hpp"

// ===== 拍卖行系统：开始 =====
// Inter wire layouts (map->char 0x30Bx requests are built by the intif
// wrappers; char->map 0x38Bx replies are parsed here). All replies carry
// len.W at offset 2 so the map intif length table keeps a uniform -1 entry
// per id.
//
// 0x30B2 SEARCH_REQ (151, fixed):
//   cmd.W char_id.L page.W scope.B currency.B type.B refine_min.B refine_max.B
//   price_min.L price_max.L sort.B nameid_count.B nameids.64W (zero filled)
//   scope: 0 board, 1 my listings, 2 my bids
// 0x30B3 ESCROW_REQ (111, fixed):
//   cmd.W char_id.L txid.Q price.L buy_now.L currency.B duration_h.B
//   fee_permille.W deposit_permille.W blob_version.W blob.77B
//   item_type.B cap.B reserved.2B
// 0x30B4 COMMIT_REQ (14, fixed): cmd.W txid.Q auction_id.L
// 0x30B5 BID_REQ (20, fixed): cmd.W auction_id.L bidder_char_id.L price.L
//   currency.B min_increment.L min_increment_percent.B
// 0x30B7 TXID_PROBE_REPLY (11, fixed): cmd.W txid.Q alive.B
// 0x30B8 OP_REQ (16, fixed): cmd.W op.B char_id.L auction_id.L price.L currency.B
//   op: 1 cancel, 2 buy-now quote, 3 settle-due, 4 detail, 5 escrow-abort,
//       6 buy-now confirm (after the wallet deduction)
// 0x30B9 CASH_PAYOUT_REPLY (11, fixed): cmd.W char_id.L auction_id.L result.B
//
// 0x38B2 SEARCH/DETAIL (variable): len.W kind.B@4 char_id.L@5 ...
//   kind 0 page: zc_id.W@9 total_pages.W@11 page.W@13 count.B@15 row_size.B@16 rows@17
//   kind 1 detail: the ZC_AUCTION_DETAIL body (everything after cmd.W)@9
// 0x38B3 ESCROW_ACK (variable): len.W txid.Q@4 auction_id.L@12 result.B@16 reason.B@17
// 0x38B4 COMMIT_ACK (variable): same shape as 0x38B3
// 0x38B5 BID_ACK (variable): len.W char_id.L@4 auction_id.L@8 result.B@12 reason.B@13
//   currency.B@14 price_now.L@15
// 0x38B6 NOTIFY_FORWARD (variable): len.W char_id.L@4 event.B@8 reason.B@9 auction_id.L@10 value.L@14
// 0x38B7 TXID_PROBE (variable): len.W txid.Q@4
// 0x38B8 OP_ACK (variable): len.W op.B@4 char_id.L@5 auction_id.L@9 result.B@13 reason.B@14
//   currency.B@15 price_now.L@16
// 0x38B9 CASH_PAYOUT (variable): len.W char_id.L@4 auction_id.L@8 amount.L@12 reason.B@16
//
// v2 atomic batch:
// 0x30BA ESCROW_BATCH_REQ (variable): len.W@2 char_id.L@4 batch_id.Q@6
//   count.B@14 currency.B@15 duration_h.B@16 fee_permille.W@17
//   deposit_permille.W@19 cap.B@21 reserved.B@22 rows@23 (96B each:
//   txid.Q price.L buy_now.L blob_version.W blob.77B item_type.B)
// 0x38BA ESCROW_BATCH_ACK: batch_id.Q@4 result.B@12 reason.B@13
//   fail_row.B@14 count.B@15 auction_ids.L[count]@16
// 0x30BB COMMIT_BATCH_REQ: len.W@2 batch_id.Q@4 count.B@12 rows@16
//   (12B each: txid.Q auction_id.L)
// 0x38BB COMMIT_BATCH_ACK: batch_id.Q@4 result.B@12 reason.B@13

namespace auction_custom {
namespace {
enum e_auction_inter_op : uint8 {
	AUC_OP_CANCEL = 1,
	AUC_OP_BUY_NOW = 2,        // quote phase: char answers currency + price
	AUC_OP_SETTLE_DUE = 3,
	AUC_OP_DETAIL = 4,
	AUC_OP_ESCROW_ABORT = 5,
	AUC_OP_BUY_NOW_CONFIRM = 6, // commit phase after the wallet deduction
};

enum e_auction_scope : uint8 {
	AUC_SCOPE_BOARD = 0,
	AUC_SCOPE_MY_LISTINGS = 1,
	AUC_SCOPE_MY_BIDS = 2,
};

struct PendingEscrow {
	uint32 char_id;
	uint16 index;
	uint16 amount;
	uint32 nameid;
};

// v2 atomic batch: one client packet -> one char-side transaction. `rows`
// keeps just enough to re-verify and delete the inventory slots after the
// whole escrow commits; no item moves before that point.
struct BatchSlot {
	uint16 index;
	uint16 amount;
	uint32 nameid;
	uint64 txid;
};
struct PendingBatch {
	uint32 char_id;
	uint64 batch_id;
	std::vector<BatchSlot> rows;
};

// One in-flight wallet offer per char (bid or buy-now confirm); holds the
// currency+amount to re-credit exactly what left on a FAIL ack.
struct PendingOffer {
	uint32 price;
	uint8 currency;
	uint8 operation;
};

std::unordered_map<uint64, PendingEscrow> pending_escrow;
std::unordered_map<uint32, PendingOffer> pending_offer; // keyed by char_id
std::unordered_map<uint64, uint32> pending_commit;      // txid -> auction_id
std::unordered_map<uint64, PendingBatch> pending_batch; // v2: batch_id -> staged rows
std::unordered_map<uint64, uint64> batch_txid_owner;    // v2: row txid -> batch_id (probe, alive until COMMIT_ACK)
uint64 txid_counter = 0;
int32 settle_timer_id = INVALID_TIMER;

uint64 txid_next() {
	return (static_cast<uint64>(time(nullptr)) << 20) | (txid_counter++ & 0xFFFFF);
}

void send_result(map_session_data& sd, uint8 operation, uint8 result, uint8 reason, uint32 auction_id, uint32 value) {
	clif_auction_send_result(sd, operation, result, reason, auction_id, value);
}

// Little-endian wire helpers for the packed inter batch frames (x86 hosts,
// same representation as the WFIFOL/WFIFOQ builders).
void put_le32(uint8* dst, uint32 value) { memcpy(dst, &value, sizeof(value)); }
void put_le64(uint8* dst, uint64 value) { memcpy(dst, &value, sizeof(value)); }
void put_le16(uint8* dst, uint16 value) { memcpy(dst, &value, sizeof(value)); }

const int32* duration_slots() {
	static int32 slots[AUCTION_MAX_DURATION_SLOTS] = {};
	slots[0] = battle_config.auction_duration1;
	slots[1] = battle_config.auction_duration2;
	slots[2] = battle_config.auction_duration3;
	slots[3] = battle_config.auction_duration4;
	slots[4] = battle_config.auction_duration5;
	return slots;
}

// Side-effect-free precheck for one row, shared by the v2 batch path.
// Returns AUCTION_REASON_OK when the row may be escrowed.
uint8 validate_listing_row(map_session_data& sd, const RegisterRow& row, uint8 duration_idx, uint8 currency) {
	if (currency == AUCTION_CURRENCY_CASH && !battle_config.auction_allow_cash)
		return AUCTION_REASON_INVALID_PRICE;
	if (row.index >= MAX_INVENTORY || row.amount == 0 || row.amount > MAX_AMOUNT)
		return AUCTION_REASON_ITEM_NOT_FOUND;
	const struct item* it = &sd.inventory.u.items_inventory[row.index];
	if (it->nameid == 0 || it->amount < row.amount)
		return AUCTION_REASON_ITEM_NOT_FOUND;
	if (itemdb_exists(it->nameid) == nullptr)
		return AUCTION_REASON_ITEM_NOT_FOUND;
	if (battle_config.auction_ban_bound_items && it->bound)
		return AUCTION_REASON_ITEM_UNTRADEABLE;
	if (battle_config.auction_ban_rental_items && it->expire_time)
		return AUCTION_REASON_ITEM_UNTRADEABLE;
	if (!battle_config.auction_allow_named_items && it->unique_id != 0)
		return AUCTION_REASON_ITEM_UNTRADEABLE;
	if (row.price < static_cast<uint32>(battle_config.auction_min_price))
		return AUCTION_REASON_INVALID_PRICE;
	if (row.buy_now != 0) {
		int64 floor64 = 0;
		const bool overflow = rathena::util::safe_multiplication(static_cast<int64>(row.price), static_cast<int64>(battle_config.auction_buynow_min_ratio), floor64);
		if (overflow || floor64 < 0 || row.buy_now < static_cast<uint32>(floor64 / 100))
			return AUCTION_REASON_INVALID_PRICE;
	}
	if (duration_idx >= AUCTION_MAX_DURATION_SLOTS || duration_slots()[duration_idx] <= 0)
		return AUCTION_REASON_INVALID_PRICE;
	return AUCTION_REASON_OK;
}

// Abort every escrow row that a batch already created. Only used on an
// unexpected state divergence (session gone / slot changed between ACK and
// the inventory delete); the normal failure path never inserted anything.
void abort_batch_rows(int32 fd, uint8 count, int32 offset) {
	for (uint8 i = 0; i < count; ++i) {
		const uint32 auction_id = RFIFOL(fd, offset + i * 4);
		if (auction_id != 0)
			intif_auction_op(nullptr, AUC_OP_ESCROW_ABORT, 0, 0, auction_id, 0);
	}
}

void clear_batch_owner(const PendingBatch& pending) {
	for (const BatchSlot& slot : pending.rows)
		batch_txid_owner.erase(slot.txid);
}

uint64 read_txid(int32 fd, int32 offset) {
	uint64 txid = 0;
	txid |= static_cast<uint64>(RFIFOL(fd, offset));
	txid |= static_cast<uint64>(RFIFOL(fd, offset + 4)) << 32;
	return txid;
}

// Deduct one price unit in the given currency; false when the wallet cannot
// cover it. credit_price re-credits exactly what left.
bool deduct_price(map_session_data& sd, uint8 currency, uint32 price) {
	if (currency == AUCTION_CURRENCY_CASH)
		return pc_paycash(&sd, static_cast<int32>(price), 0, LOG_TYPE_TRADE) >= 0;
	return pc_payzeny(&sd, static_cast<int32>(price), LOG_TYPE_TRADE) == 0;
}

void credit_price(map_session_data& sd, uint8 currency, uint32 price) {
	if (currency == AUCTION_CURRENCY_CASH)
		pc_getcash(&sd, static_cast<int32>(price), 0, LOG_TYPE_TRADE);
	else
		pc_getzeny(&sd, static_cast<int32>(price), LOG_TYPE_TRADE);
}

// Map-side settle tick: asks the char server to run the atomic due-settlement
// transaction (ADR A4). The map never touches settlement data itself.
TIMER_FUNC(settle_timer){
	intif_auction_op(nullptr, AUC_OP_SETTLE_DUE, 0, 0, 0, 0);
	return 0;
}
} // namespace

bool request_search(map_session_data& sd, const PACKET_CZ_AUCTION_SEARCH& pkt) {
	return intif_auction_search(sd, AUC_SCOPE_BOARD, pkt.page, pkt.currency, pkt.type, pkt.refineMin, pkt.refineMax, pkt.priceMin, pkt.priceMax, pkt.sort, pkt.nameidCount, pkt.nameids);
}

bool request_detail(map_session_data& sd, uint32 auction_id) {
	return intif_auction_op(&sd, AUC_OP_DETAIL, sd.status.char_id, auction_id, 0, 0);
}

bool request_my_listings(map_session_data& sd, uint16 page) {
	return intif_auction_my_page(sd, AUC_SCOPE_MY_LISTINGS, page);
}

bool request_my_bids(map_session_data& sd, uint16 page) {
	return intif_auction_my_page(sd, AUC_SCOPE_MY_BIDS, page);
}

bool request_register(map_session_data& sd, uint16 index, uint16 amount, uint32 price, uint32 buy_now, uint8 duration_idx, uint8 currency) {
	nullpo_retr(false, &sd);
	if (currency == AUCTION_CURRENCY_CASH && !battle_config.auction_allow_cash) {
		send_result(sd, AUCTION_OP_REGISTER, AUCTION_RESULT_FAIL, AUCTION_REASON_INVALID_PRICE, 0, 0);
		return false;
	}
	if (index >= MAX_INVENTORY || amount == 0 || amount > MAX_AMOUNT) {
		send_result(sd, AUCTION_OP_REGISTER, AUCTION_RESULT_FAIL, AUCTION_REASON_ITEM_NOT_FOUND, 0, 0);
		return false;
	}
	struct item* it = &sd.inventory.u.items_inventory[index];
	if (it->nameid == 0 || it->amount < amount) {
		send_result(sd, AUCTION_OP_REGISTER, AUCTION_RESULT_FAIL, AUCTION_REASON_ITEM_NOT_FOUND, 0, 0);
		return false;
	}
	if (itemdb_exists(it->nameid) == nullptr) {
		send_result(sd, AUCTION_OP_REGISTER, AUCTION_RESULT_FAIL, AUCTION_REASON_ITEM_NOT_FOUND, 0, 0);
		return false;
	}
	if (battle_config.auction_ban_bound_items && it->bound) {
		send_result(sd, AUCTION_OP_REGISTER, AUCTION_RESULT_FAIL, AUCTION_REASON_ITEM_UNTRADEABLE, 0, 0);
		return false;
	}
	if (battle_config.auction_ban_rental_items && it->expire_time) {
		send_result(sd, AUCTION_OP_REGISTER, AUCTION_RESULT_FAIL, AUCTION_REASON_ITEM_UNTRADEABLE, 0, 0);
		return false;
	}
	if (!battle_config.auction_allow_named_items && it->unique_id != 0) {
		send_result(sd, AUCTION_OP_REGISTER, AUCTION_RESULT_FAIL, AUCTION_REASON_ITEM_UNTRADEABLE, 0, 0);
		return false;
	}
	if (price < static_cast<uint32>(battle_config.auction_min_price)) {
		send_result(sd, AUCTION_OP_REGISTER, AUCTION_RESULT_FAIL, AUCTION_REASON_INVALID_PRICE, 0, 0);
		return false;
	}
	if (buy_now != 0) {
		// buynow floor = price * ratio / 100; an overflow means the floor is
		// beyond any legal buy_now and the request is rejected either way.
		int64 floor64 = 0;
		const bool overflow = rathena::util::safe_multiplication(static_cast<int64>(price), static_cast<int64>(battle_config.auction_buynow_min_ratio), floor64);
		if (overflow || floor64 < 0 || buy_now < static_cast<uint32>(floor64 / 100)) {
			send_result(sd, AUCTION_OP_REGISTER, AUCTION_RESULT_FAIL, AUCTION_REASON_INVALID_PRICE, 0, 0);
			return false;
		}
	}
	const int32 durations[AUCTION_MAX_DURATION_SLOTS] = {
		battle_config.auction_duration1, battle_config.auction_duration2,
		battle_config.auction_duration3, battle_config.auction_duration4,
		battle_config.auction_duration5,
	};
	if (duration_idx >= AUCTION_MAX_DURATION_SLOTS || durations[duration_idx] <= 0) {
		send_result(sd, AUCTION_OP_REGISTER, AUCTION_RESULT_FAIL, AUCTION_REASON_INVALID_PRICE, 0, 0);
		return false;
	}
	const uint16 fee_permille = static_cast<uint16>(currency == AUCTION_CURRENCY_CASH ? battle_config.auction_cash_fee_permille : battle_config.auction_fee_permille);
	const uint16 deposit_permille = static_cast<uint16>(battle_config.auction_deposit_permille);

	// Escrow first (durable row char-side, A6); the inventory item is only
	// removed after the ESCROW_ACK arrives. The per-account listing cap is
	// enforced inside the char transaction so it stays atomic.
	const uint64 txid = txid_next();
	if (!intif_auction_escrow(sd, txid, price, buy_now, currency, static_cast<uint8>(durations[duration_idx]), fee_permille, deposit_permille, *it, itemdb_type(it->nameid), static_cast<uint8>(battle_config.auction_max_listings))) {
		send_result(sd, AUCTION_OP_REGISTER, AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL, 0, 0);
		return false;
	}
	PendingEscrow pending{};
	pending.char_id = sd.status.char_id;
	pending.index = index;
	pending.amount = amount;
	pending.nameid = it->nameid;
	pending_escrow[txid] = pending;
	return true;
}

// v2 atomic batch. Validation is entirely local and side-effect free; only
// after every row passes do we ask the char server to escrow all N rows in
// one transaction. A failure at any point produces exactly one batch receipt
// with all rows marked FAIL, and the inventory is never touched.
bool request_register_batch(map_session_data& sd, const std::vector<RegisterRow>& rows, uint8 duration_idx, uint8 currency) {
	nullpo_retr(false, &sd);
	const uint8 count = static_cast<uint8>(rows.size());
	auto fail = [&](uint8 reason, uint8 fail_row) {
		clif_auction_send_register_batch_result(sd, AUCTION_RESULT_FAIL, reason, fail_row, nullptr, nullptr, nullptr, count);
		return false;
	};
	if (count == 0 || count > AUCTION_MAX_BATCH_ITEMS)
		return fail(AUCTION_REASON_TOO_MANY_ITEMS, 0);
	// Two rows may not share an inventory slot: pc_delitem would then see a
	// shifted slot after the first deletion.
	for (uint8 i = 0; i < count; ++i) {
		for (uint8 j = 0; j < i; ++j) {
			if (rows[i].index == rows[j].index)
				return fail(AUCTION_REASON_ITEM_NOT_FOUND, i);
		}
		const uint8 reason = validate_listing_row(sd, rows[i], duration_idx, currency);
		if (reason != AUCTION_REASON_OK)
			return fail(reason, i);
	}

	const uint16 fee_permille = static_cast<uint16>(currency == AUCTION_CURRENCY_CASH ? battle_config.auction_cash_fee_permille : battle_config.auction_fee_permille);
	const uint16 deposit_permille = static_cast<uint16>(battle_config.auction_deposit_permille);
	const uint64 batch_id = txid_next();
	std::vector<uint8> payload(static_cast<size_t>(count) * AUCTION_INTER_BATCH_ROW_SIZE, 0);
	PendingBatch pending{};
	pending.char_id = sd.status.char_id;
	pending.batch_id = batch_id;
	pending.rows.reserve(count);
	for (uint8 i = 0; i < count; ++i) {
		const RegisterRow& row = rows[i];
		const struct item& original = sd.inventory.u.items_inventory[row.index];
		struct item blob = original;
		blob.amount = static_cast<decltype(blob.amount)>(row.amount); // escrow only the offered amount
		const uint64 txid = txid_next();
		uint8* p = payload.data() + static_cast<size_t>(i) * AUCTION_INTER_BATCH_ROW_SIZE;
		put_le64(p, txid);
		put_le32(p + 8, row.price);
		put_le32(p + 12, row.buy_now);
		put_le16(p + 16, static_cast<uint16>(AUCTION_ITEM_BLOB_VERSION));
		memcpy(p + 18, &blob, AUCTION_ITEM_BLOB_SIZE);
		p[95] = static_cast<uint8>(itemdb_type(original.nameid));
		pending.rows.push_back({ row.index, row.amount, original.nameid, txid });
		batch_txid_owner[txid] = batch_id;
	}
	const bool sent = intif_auction_escrow_batch(
		sd.status.char_id, batch_id, currency,
		static_cast<uint8>(duration_slots()[duration_idx]), fee_permille, deposit_permille,
		static_cast<uint8>(battle_config.auction_max_listings), count, payload.data());
	if (!sent) {
		for (const BatchSlot& slot : pending.rows)
			batch_txid_owner.erase(slot.txid);
		ShowError("auction batch: escrow request NOT sent (char-serv link down), char_id=%u count=%u batch_id=%llu\n",
			sd.status.char_id, count, static_cast<unsigned long long>(batch_id));
		return fail(AUCTION_REASON_INTER_UNAVAILABLE, 0);
	}
	ShowInfo("auction batch: escrow sent char_id=%u count=%u batch_id=%llu\n",
		sd.status.char_id, count, static_cast<unsigned long long>(batch_id));
	pending_batch[batch_id] = pending;
	return true;
}

bool request_cancel(map_session_data& sd, uint32 auction_id) {
	return intif_auction_op(&sd, AUC_OP_CANCEL, sd.status.char_id, auction_id, 0, 0);
}

bool request_bid(map_session_data& sd, uint32 auction_id, uint32 price) {
	nullpo_retr(false, &sd);
	if (price == 0) {
		send_result(sd, AUCTION_OP_BID, AUCTION_RESULT_FAIL, AUCTION_REASON_INVALID_PRICE, auction_id, 0);
		return false;
	}
	if (pending_offer.count(sd.status.char_id) != 0) {
		send_result(sd, AUCTION_OP_BID, AUCTION_RESULT_FAIL, AUCTION_REASON_RATE_LIMITED, auction_id, 0);
		return false;
	}
	// Guess the currency (zeny first). The listing currency is only known to
	// the char server: a CURRENCY_MISMATCH ack re-credits this attempt and
	// retries once with the other wallet (ADR A11).
	uint8 currency = AUCTION_CURRENCY_ZENY;
	if (!deduct_price(sd, currency, price)) {
		currency = AUCTION_CURRENCY_CASH;
		if (!deduct_price(sd, currency, price)) {
			send_result(sd, AUCTION_OP_BID, AUCTION_RESULT_FAIL, AUCTION_REASON_INSUFFICIENT_FUNDS, auction_id, 0);
			return false;
		}
	}
	if (!intif_auction_bid(sd, auction_id, price, currency, static_cast<uint32>(battle_config.auction_min_increment), static_cast<uint8>(battle_config.auction_min_increment_percent))) {
		credit_price(sd, currency, price);
		send_result(sd, AUCTION_OP_BID, AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL, auction_id, 0);
		return false;
	}
	PendingOffer offer{};
	offer.price = price;
	offer.currency = currency;
	offer.operation = AUCTION_OP_BID;
	pending_offer[sd.status.char_id] = offer;
	return true;
}

bool request_buy_now(map_session_data& sd, uint32 auction_id) {
	nullpo_retr(false, &sd);
	if (pending_offer.count(sd.status.char_id) != 0) {
		send_result(sd, AUCTION_OP_BUY_NOW, AUCTION_RESULT_FAIL, AUCTION_REASON_RATE_LIMITED, auction_id, 0);
		return false;
	}
	// Quote phase: the char server answers the listing currency and price
	// without changing state; the wallet deduction and the atomic commit
	// happen in on_op_ack / the confirm op.
	return intif_auction_op(&sd, AUC_OP_BUY_NOW, sd.status.char_id, auction_id, 0, 0);
}

// char->map callbacks -------------------------------------------------------

void on_escrow_ack(int32 fd) {
	const uint64 txid = read_txid(fd, 4);
	const uint32 auction_id = RFIFOL(fd, 12);
	const uint8 result = RFIFOB(fd, 16);
	const uint8 reason = RFIFOB(fd, 17);

	auto it = pending_escrow.find(txid);
	if (it == pending_escrow.end()) {
		// Unknown txid (map restarted mid-handshake): drop the row char-side
		// so the reconcile never returns an item this map no longer tracks.
		intif_auction_op(nullptr, AUC_OP_ESCROW_ABORT, 0, 0, auction_id, 0);
		return;
	}
	PendingEscrow pending = it->second;
	pending_escrow.erase(it);

	// Every char->map reply identifies the player by char_id, so the lookup
	// MUST use map_charid2sd (charid_db). map_id2sd indexes pc_db by
	// account_id: passing a char_id silently returned nullptr and dropped
	// the whole reply (no client receipt, escrow rows stuck at status 0).
	map_session_data* sd = map_charid2sd(pending.char_id);
	if (result != AUCTION_RESULT_OK || sd == nullptr || !sd->auction_supported) {
		if (sd != nullptr)
			send_result(*sd, AUCTION_OP_REGISTER, AUCTION_RESULT_FAIL, reason, 0, 0);
		else
			intif_auction_op(nullptr, AUC_OP_ESCROW_ABORT, 0, 0, auction_id, 0);
		return;
	}
	// Verify the slot still holds what the player offered; otherwise abort.
	struct item* it_inv = &sd->inventory.u.items_inventory[pending.index];
	if (pending.index >= MAX_INVENTORY || it_inv->nameid != pending.nameid || it_inv->amount < pending.amount) {
		intif_auction_op(nullptr, AUC_OP_ESCROW_ABORT, 0, 0, auction_id, 0);
		send_result(*sd, AUCTION_OP_REGISTER, AUCTION_RESULT_FAIL, AUCTION_REASON_ITEM_NOT_FOUND, 0, 0);
		return;
	}
	// Remove the item, persist the removal, then flip the row to ACTIVE.
	// Ordering keeps every crash window row-only (A6): before COMMIT the
	// reconcile deletes the row and the inventory reload restores the item;
	// a dup would need the row AND the saved inventory to both survive,
	// which only happens after COMMIT when the save already ran.
	pc_delitem(sd, pending.index, pending.amount, 0, 1, LOG_TYPE_TRADE);
	chrif_save(sd, CSAVE_INVENTORY);
	pending_commit[txid] = auction_id;
	if (!intif_auction_commit(txid, auction_id)) {
		pending_commit.erase(txid);
		// Inter link down: the removal is already persisted; the row stays
		// ESCROWING and the reconcile deletes it, restoring the item on the
		// next login. Tell the player the board is unavailable right now.
		send_result(*sd, AUCTION_OP_REGISTER, AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL, 0, 0);
		return;
	}
	send_result(*sd, AUCTION_OP_REGISTER, AUCTION_RESULT_OK, AUCTION_REASON_OK, auction_id, 0);
}

void on_commit_ack(int32 fd) {
	const uint64 txid = read_txid(fd, 4);
	const uint32 auction_id = RFIFOL(fd, 12);
	const uint8 result = RFIFOB(fd, 16);
	const uint8 reason = RFIFOB(fd, 17);
	pending_commit.erase(txid);
	if (result != AUCTION_RESULT_OK) {
		ShowError("auction commit failed: txid=%llu auction_id=%u reason=%u\n",
			static_cast<unsigned long long>(txid), auction_id, reason);
	}
}

// ===== v2 atomic batch ACKs: 开始 =====
// 0x38BA ESCROW_BATCH_ACK: len.W@2 batch_id.Q@4 result.B@12 reason.B@13
//   fail_row.B@14 count.B@15 auction_ids.L[count]@16
void on_escrow_batch_ack(int32 fd) {
	const uint64 batch_id = read_txid(fd, 4);
	const uint8 result = RFIFOB(fd, 12);
	const uint8 reason = RFIFOB(fd, 13);
	const uint8 fail_row = RFIFOB(fd, 14);
	const uint8 count = RFIFOB(fd, 15);

	ShowInfo("auction batch ack: batch_id=%llu result=%u reason=%u fail_row=%u count=%u\n",
		static_cast<unsigned long long>(batch_id), result, reason, fail_row, count);
	auto it = pending_batch.find(batch_id);
	if (it == pending_batch.end()) {
		ShowError("auction batch ack: unknown batch_id=%llu (pending dropped), aborting rows\n",
			static_cast<unsigned long long>(batch_id));
		// Map restarted mid-handshake: drop every row the char server kept.
		abort_batch_rows(fd, count, 16);
		return;
	}
	PendingBatch pending = it->second;
	pending_batch.erase(it);
	const uint8 rows = static_cast<uint8>(pending.rows.size());
	map_session_data* sd = map_charid2sd(pending.char_id);
	if (result != AUCTION_RESULT_OK || sd == nullptr || !sd->auction_supported) {
		ShowError("auction batch ack: rejected result=%u reason=%u sd=%p supported=%d rows=%u\n",
			result, reason, static_cast<void*>(sd), sd != nullptr ? sd->auction_supported : -1, rows);
		clear_batch_owner(pending);
		if (sd != nullptr)
			clif_auction_send_register_batch_result(*sd, AUCTION_RESULT_FAIL, reason, fail_row, nullptr, nullptr, nullptr, rows);
		else
			abort_batch_rows(fd, count, 16);
		return;
	}
	// The char transaction committed all N escrow rows. Re-verify every slot
	// before deleting anything; a divergence aborts ALL rows and leaves the
	// inventory exactly as it was.
	for (uint8 i = 0; i < rows; ++i) {
		const BatchSlot& slot = pending.rows[i];
		if (slot.index >= MAX_INVENTORY) {
			clear_batch_owner(pending);
			abort_batch_rows(fd, count, 16);
			clif_auction_send_register_batch_result(*sd, AUCTION_RESULT_FAIL, AUCTION_REASON_ITEM_NOT_FOUND, i, nullptr, nullptr, nullptr, rows);
			return;
		}
		const struct item* inv = &sd->inventory.u.items_inventory[slot.index];
		if (inv->nameid != slot.nameid || inv->amount < slot.amount) {
			clear_batch_owner(pending);
			abort_batch_rows(fd, count, 16);
			clif_auction_send_register_batch_result(*sd, AUCTION_RESULT_FAIL, AUCTION_REASON_ITEM_NOT_FOUND, i, nullptr, nullptr, nullptr, rows);
			return;
		}
	}
	std::vector<uint8> commit(static_cast<size_t>(rows) * 12, 0);
	uint8 row_results[AUCTION_MAX_BATCH_ITEMS] = {};
	uint8 row_reasons[AUCTION_MAX_BATCH_ITEMS] = {};
	uint32 auction_ids[AUCTION_MAX_BATCH_ITEMS] = {};
	// Build the commit list first: its pairing (txid, auction_id) follows the
	// char server's row order, which the deletion order must not disturb.
	struct BatchDeletion { int32 index; int32 amount; };
	std::vector<BatchDeletion> deletions;
	deletions.reserve(rows);
	for (uint8 i = 0; i < rows; ++i) {
		const BatchSlot& slot = pending.rows[i];
		auction_ids[i] = RFIFOL(fd, 16 + i * 4);
		put_le64(commit.data() + static_cast<size_t>(i) * 12, slot.txid);
		put_le32(commit.data() + static_cast<size_t>(i) * 12 + 8, auction_ids[i]);
		row_results[i] = AUCTION_RESULT_OK;
		row_reasons[i] = AUCTION_REASON_OK;
		deletions.push_back({ static_cast<int32>(slot.index), static_cast<int32>(slot.amount) });
	}
	// Delete high slots first. `pc_delitem` compacts the inventory when a stack
	// is emptied, so deleting in row order shifts every later row up by one and
	// the batch then removes the wrong items (or nothing at all). Descending
	// indices keep every remaining target slot valid.
	std::sort(deletions.begin(), deletions.end(),
		[](const BatchDeletion& a, const BatchDeletion& b) { return a.index > b.index; });
	for (const BatchDeletion& deletion : deletions) {
		if (!pc_delitem(sd, deletion.index, deletion.amount, 0, 1, LOG_TYPE_TRADE)) {
			// The escrow rows are already committed on the char server, so the
			// missing stack is reported and left to the reconcile pass instead
			// of being "restored" client-side twice.
			ShowError("auction batch: pc_delitem failed char_id=%u index=%d amount=%d\n",
				sd->status.char_id, deletion.index, deletion.amount);		}
	}
	chrif_save(sd, CSAVE_INVENTORY);
	if (!intif_auction_commit_batch(batch_id, rows, commit.data())) {
		clear_batch_owner(pending);
		// Rows stay ESCROWING; the reconcile deletes them and the inventory
		// reload restores the items (identical window to the single-item path).
		clif_auction_send_register_batch_result(*sd, AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL, 0xFF, nullptr, nullptr, nullptr, rows);
		return;
	}
	// batch_txid_owner stays alive until COMMIT_BATCH_ACK, so the char-side
	// reconcile probe still sees these txids during the second phase.
	clif_auction_send_register_batch_result(*sd, AUCTION_RESULT_OK, AUCTION_REASON_OK, 0xFF, row_results, row_reasons, auction_ids, rows);
}

// 0x38BB COMMIT_BATCH_ACK: len.W@2 batch_id.Q@4 result.B@12 reason.B@13
void on_commit_batch_ack(int32 fd) {
	const uint64 batch_id = read_txid(fd, 4);
	const uint8 result = RFIFOB(fd, 12);
	const uint8 reason = RFIFOB(fd, 13);
	for (auto it = batch_txid_owner.begin(); it != batch_txid_owner.end();) {
		if (it->second == batch_id)
			it = batch_txid_owner.erase(it);
		else
			++it;
	}
	if (result != AUCTION_RESULT_OK) {
		ShowError("auction batch commit failed: batch_id=%llu reason=%u\n",
			static_cast<unsigned long long>(batch_id), reason);
	}
}
// ===== v2 atomic batch ACKs: 结束 =====

void on_bid_ack(int32 fd) {
	const uint32 char_id = RFIFOL(fd, 4);
	const uint32 auction_id = RFIFOL(fd, 8);
	const uint8 result = RFIFOB(fd, 12);
	const uint8 reason = RFIFOB(fd, 13);
	const uint8 currency = RFIFOB(fd, 14);
	const uint32 price_now = RFIFOL(fd, 15);
	(void)currency;

	auto it = pending_offer.find(char_id);
	if (it == pending_offer.end())
		return;
	PendingOffer offer = it->second;
	pending_offer.erase(it);
	map_session_data* sd = map_charid2sd(char_id);
	if (sd == nullptr || !sd->auction_supported) {
		return; // session gone: deduction persisted via logout save; escrow
		        // row (accepted) or journal (rejected path) keeps the audit.
	}
	if (result == AUCTION_RESULT_OK) {
		send_result(*sd, AUCTION_OP_BID, AUCTION_RESULT_OK, AUCTION_REASON_OK, auction_id, price_now);
		return;
	}
	// Re-credit exactly what left the wallet, then optionally retry once in
	// the other currency on a currency mismatch (A11).
	credit_price(*sd, offer.currency, offer.price);
	if (reason == AUCTION_REASON_CURRENCY_MISMATCH) {
		const uint8 currency2 = offer.currency == AUCTION_CURRENCY_ZENY ? AUCTION_CURRENCY_CASH : AUCTION_CURRENCY_ZENY;
		if (deduct_price(*sd, currency2, offer.price)) {
			if (intif_auction_bid(*sd, auction_id, offer.price, currency2, static_cast<uint32>(battle_config.auction_min_increment), static_cast<uint8>(battle_config.auction_min_increment_percent))) {
				PendingOffer retry{};
				retry.price = offer.price;
				retry.currency = currency2;
				retry.operation = AUCTION_OP_BID;
				pending_offer[char_id] = retry;
				return;
			}
			credit_price(*sd, currency2, offer.price);
		}
	}
	send_result(*sd, AUCTION_OP_BID, AUCTION_RESULT_FAIL, reason, auction_id, price_now);
}

void on_search_or_detail(int32 fd) {
	const uint8 kind = RFIFOB(fd, 4);
	const uint32 char_id = RFIFOL(fd, 5);
	map_session_data* sd = map_charid2sd(char_id);
	if (sd == nullptr || !sd->auction_supported || sd->fd <= 0)
		return;
	const int32 out_fd = sd->fd;
	if (kind == 1) {
		// The 0x38B2 body@9 is the ZC_AUCTION_DETAIL body (after cmd.W).
		const int32 body_len = RFIFOW(fd, 2) - 9;
		if (body_len != sizeof(PACKET_ZC_AUCTION_DETAIL) - 2) {
			return;
		}
		PACKET_ZC_AUCTION_DETAIL packet{};
		memcpy(reinterpret_cast<uint8*>(&packet) + 2, RFIFOP(fd, 9), body_len);
		packet.packetType = HEADER_ZC_AUCTION_DETAIL;
		WFIFOHEAD(out_fd, sizeof(packet));
		memcpy(WFIFOP(out_fd, 0), &packet, sizeof(packet));
		WFIFOSET(out_fd, sizeof(packet));
		return;
	}
	// Page reply: zc_id selects which ZC frame to emit (0x7e31/0x7e34/0x7e35).
	const uint16 zc_id = RFIFOW(fd, 9);
	const uint16 total_pages = RFIFOW(fd, 11);
	const uint16 page = RFIFOW(fd, 13);
	const uint8 count = RFIFOB(fd, 15);
	const uint8 row_size = RFIFOB(fd, 16);
	if (row_size == 0 || count == 0) {
		WFIFOHEAD(out_fd, 9);
		WFIFOW(out_fd, 0) = zc_id;
		WFIFOW(out_fd, 2) = 9;
		WFIFOW(out_fd, 4) = total_pages;
		WFIFOW(out_fd, 6) = page;
		WFIFOB(out_fd, 8) = 0;
		WFIFOSET(out_fd, 9);
		return;
	}
	const int32 rows_len = count * row_size;
	const int32 zc_len = 9 + rows_len;
	WFIFOHEAD(out_fd, zc_len);
	WFIFOW(out_fd, 0) = zc_id;
	WFIFOW(out_fd, 2) = static_cast<uint16>(zc_len);
	WFIFOW(out_fd, 4) = total_pages;
	WFIFOW(out_fd, 6) = page;
	WFIFOB(out_fd, 8) = count;
	memcpy(WFIFOP(out_fd, 9), RFIFOP(fd, 17), rows_len);
	WFIFOSET(out_fd, zc_len);
}

void on_op_ack(int32 fd) {
	const uint8 op = RFIFOB(fd, 4);
	const uint32 char_id = RFIFOL(fd, 5);
	const uint32 auction_id = RFIFOL(fd, 9);
	const uint8 result = RFIFOB(fd, 13);
	const uint8 reason = RFIFOB(fd, 14);
	const uint8 currency = RFIFOB(fd, 15);
	const uint32 price_now = RFIFOL(fd, 16);

	switch (op) {
		case AUC_OP_CANCEL: {
			map_session_data* sd = map_charid2sd(char_id);
			if (sd != nullptr && sd->auction_supported)
				send_result(*sd, AUCTION_OP_CANCEL, result, reason, auction_id, price_now);
			break;
		}
		case AUC_OP_BUY_NOW: {
			// Quote phase. OK: currency+price_now are the listing truth; the
			// wallet is deducted here and the atomic confirm goes out. FAIL:
			// straight reject (ended/sold/unknown listing).
			map_session_data* sd = map_charid2sd(char_id);
			if (sd == nullptr || !sd->auction_supported)
				return;
			if (result != AUCTION_RESULT_OK) {
				send_result(*sd, AUCTION_OP_BUY_NOW, AUCTION_RESULT_FAIL, reason, auction_id, price_now);
				return;
			}
			if (!deduct_price(*sd, currency, price_now)) {
				send_result(*sd, AUCTION_OP_BUY_NOW, AUCTION_RESULT_FAIL, AUCTION_REASON_INSUFFICIENT_FUNDS, auction_id, price_now);
				return;
			}
			if (!intif_auction_op(sd, AUC_OP_BUY_NOW_CONFIRM, char_id, auction_id, price_now, currency)) {
				credit_price(*sd, currency, price_now);
				send_result(*sd, AUCTION_OP_BUY_NOW, AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL, auction_id, price_now);
				return;
			}
			PendingOffer offer{};
			offer.price = price_now;
			offer.currency = currency;
			offer.operation = AUCTION_OP_BUY_NOW;
			pending_offer[char_id] = offer;
			break;
		}
		case AUC_OP_BUY_NOW_CONFIRM: {
			auto it = pending_offer.find(char_id);
			if (it == pending_offer.end())
				return;
			PendingOffer offer = it->second;
			pending_offer.erase(it);
			map_session_data* sd = map_charid2sd(char_id);
			if (sd == nullptr || !sd->auction_supported)
				return;
			if (result != AUCTION_RESULT_OK) {
				// Someone bought/expired before the confirm transaction ran.
				credit_price(*sd, offer.currency, offer.price);
				send_result(*sd, AUCTION_OP_BUY_NOW, AUCTION_RESULT_FAIL, reason, auction_id, price_now);
				return;
			}
			send_result(*sd, AUCTION_OP_BUY_NOW, AUCTION_RESULT_OK, AUCTION_REASON_OK, auction_id, price_now);
			break;
		}
		case AUC_OP_DETAIL:
		case AUC_OP_SETTLE_DUE:
		case AUC_OP_ESCROW_ABORT:
		default:
			break;
	}
}

void on_notify_forward(int32 fd) {
	const uint32 char_id = RFIFOL(fd, 4);
	const uint8 event = RFIFOB(fd, 8);
	const uint8 reason = RFIFOB(fd, 9);
	const uint32 auction_id = RFIFOL(fd, 10);
	const uint32 value = RFIFOL(fd, 14);
	map_session_data* sd = map_charid2sd(char_id);
	if (sd == nullptr || !sd->auction_supported)
		return;
	clif_auction_send_notify(*sd, event, reason, auction_id, value);
}

void on_txid_probe(int32 fd) {
	const uint64 txid = read_txid(fd, 4);
	// A v2 batch row is alive from escrow request until COMMIT_BATCH_ACK.
	const uint8 alive = (pending_escrow.count(txid) != 0 || pending_commit.count(txid) != 0
		|| batch_txid_owner.count(txid) != 0) ? 1 : 0;
	intif_auction_probe_reply(txid, alive);
}

void on_cash_payout(int32 fd) {
	const uint32 char_id = RFIFOL(fd, 4);
	const uint32 auction_id = RFIFOL(fd, 8);
	const uint32 amount = RFIFOL(fd, 12);
	map_session_data* sd = map_charid2sd(char_id);
	if (sd == nullptr || !sd->auction_supported) {
		// Player left between the char decision and the payout: report
		// failure so the char server falls back to the offline acc_reg_num
		// write (A12).
		intif_auction_cash_reply(char_id, auction_id, 0);
		return;
	}
	if (pc_getcash(sd, static_cast<int32>(amount), 0, LOG_TYPE_TRADE) < 0) {
		intif_auction_cash_reply(char_id, auction_id, 0);
		return;
	}
	clif_auction_send_notify(*sd, AUCTION_NOTIFY_CASH_CHANGED, AUCTION_REASON_OK, auction_id, static_cast<uint32>(sd->cashPoints));
	intif_auction_cash_reply(char_id, auction_id, 1);
}

void do_init_auction_custom(void) {
	add_timer_func_list(settle_timer, "auction_custom_settle_timer");
	settle_timer_id = add_timer_interval(gettick() + 1000, settle_timer, 0, 0, battle_config.auction_settle_scan_interval);
}

void do_final_auction_custom(void) {
	if (settle_timer_id != INVALID_TIMER) {
		delete_timer(settle_timer_id, settle_timer);
		settle_timer_id = INVALID_TIMER;
	}
	pending_escrow.clear();
	pending_offer.clear();
	pending_commit.clear();
	pending_batch.clear();
	batch_txid_owner.clear();
}
} // namespace auction_custom
// ===== 拍卖行系统：结束 =====
