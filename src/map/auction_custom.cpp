#include "auction_custom.hpp"

#include <cstring>
#include <unordered_map>

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
uint64 txid_counter = 0;
int32 settle_timer_id = INVALID_TIMER;

uint64 txid_next() {
	return (static_cast<uint64>(time(nullptr)) << 20) | (txid_counter++ & 0xFFFFF);
}

void send_result(map_session_data& sd, uint8 operation, uint8 result, uint8 reason, uint32 auction_id, uint32 value) {
	clif_auction_send_result(sd, operation, result, reason, auction_id, value);
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

	map_session_data* sd = map_id2sd(pending.char_id);
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
	map_session_data* sd = map_id2sd(char_id);
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
	map_session_data* sd = map_id2sd(char_id);
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
			map_session_data* sd = map_id2sd(char_id);
			if (sd != nullptr && sd->auction_supported)
				send_result(*sd, AUCTION_OP_CANCEL, result, reason, auction_id, price_now);
			break;
		}
		case AUC_OP_BUY_NOW: {
			// Quote phase. OK: currency+price_now are the listing truth; the
			// wallet is deducted here and the atomic confirm goes out. FAIL:
			// straight reject (ended/sold/unknown listing).
			map_session_data* sd = map_id2sd(char_id);
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
			map_session_data* sd = map_id2sd(char_id);
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
	map_session_data* sd = map_id2sd(char_id);
	if (sd == nullptr || !sd->auction_supported)
		return;
	clif_auction_send_notify(*sd, event, reason, auction_id, value);
}

void on_txid_probe(int32 fd) {
	const uint64 txid = read_txid(fd, 4);
	const uint8 alive = (pending_escrow.count(txid) != 0 || pending_commit.count(txid) != 0) ? 1 : 0;
	intif_auction_probe_reply(txid, alive);
}

void on_cash_payout(int32 fd) {
	const uint32 char_id = RFIFOL(fd, 4);
	const uint32 auction_id = RFIFOL(fd, 8);
	const uint32 amount = RFIFOL(fd, 12);
	map_session_data* sd = map_id2sd(char_id);
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
}
} // namespace auction_custom
// ===== 拍卖行系统：结束 =====
