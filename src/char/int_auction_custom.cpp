#include "int_auction_custom.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <common/mmo.hpp>
#include <common/showmsg.hpp>
#include <common/socket.hpp>
#include <common/sql.hpp>
#include <common/strlib.hpp>
#include <common/timer.hpp>
#include <common/translate.hpp>
#include <common/utilities.hpp>

#include <common/auction.hpp>

#include "char.hpp"
#include "char_mapif.hpp"
#include "inter.hpp"
#include "int_mail.hpp"

// ===== 拍卖行系统：开始 =====
// Request layouts are documented in src/map/auction_custom.cpp (authoritative).
// v2 batch: 0x30BA escrow (one transaction for N rows) / 0x30BB commit,
// replies 0x38BA / 0x38BB; see the per-handler comments below.
// ESCROW_REQ (111): cmd.W char_id.L txid.Q price.L buy_now.L currency.B
//   duration_h.B fee_permille.W deposit_permille.W blob_version.W blob.77B
//   item_type.B cap.B reserved.2B
// BID_REQ (20): cmd.W auction_id.L bidder_char_id.L price.L currency.B
//   min_increment.L min_increment_percent.B
// Replies are all variable (len.W at offset 2), see the per-handler comments.
//
// Status machine: 0 ESCROWING -> 1 ACTIVE -> 2 SOLD | 4 CLOSED | 5 CANCELLED;
// 3 RETURN_PENDING is produced by the reconcile pass before its return mail.
//
// Money rules (A11/A12 + §7): fee/deposit permille are locked per row at
// register time and fees are computed from the FINAL price (uint64 math).
// The deposit flow is dormant (map never charges), so nothing refunds it.
// zeny payouts travel as RODEX mail zeny; cash payouts go 0x38B9 to the
// owning map server for online players and direct `acc_reg_num` writes for
// offline ones (A12), with the 0x30B9 reply driving the fallback.
//
// All multi-write operations run inside explicit transactions on the shared
// sql_handle (int_autoplay.cpp precedent). Mail helpers use
// mail_sendmail_in_tx, which joins the CALLER's transaction — the plain
// mail_sendmail manages its own and its START TRANSACTION would implicitly
// commit the outer one (audit P0-2). Binary item blobs travel through SqlStmt
// bound parameters (SQLDT_BLOB), never through printf-style SQL.

namespace {
constexpr uint32 AUCTION_PAGE_SIZE = 20;
constexpr uint32 AUCTION_ESCROW_RECLAIM_TIMEOUT = 600;        // seconds
constexpr uint32 AUCTION_RECONCILE_INTERVAL = 60 * 60 * 1000; // 1h
constexpr uint32 AUCTION_RECONCILE_FIRST_DELAY = 30 * 1000;   // wait for map links
constexpr uint32 AUCTION_PAYOUT_REPLY_TIMEOUT = 10;           // seconds
// The reap timer runs at the verdict timeout, not at the reconcile period:
// with the old 1h interval a dead map's payout sat in limbo for an hour,
// and a late reply after the fallback had a huge window to double-credit
// (audit P0-4).
constexpr uint32 AUCTION_PAYOUT_REAP_INTERVAL = AUCTION_PAYOUT_REPLY_TIMEOUT * 1000;

enum e_auction_inter_op : uint8 {
	AUC_OP_CANCEL = 1,
	AUC_OP_BUY_NOW = 2,          // quote phase
	AUC_OP_SETTLE_DUE = 3,
	AUC_OP_DETAIL = 4,
	AUC_OP_ESCROW_ABORT = 5,
	AUC_OP_BUY_NOW_CONFIRM = 6,  // commit phase after the wallet deduction
};

enum e_auction_scope : uint8 {
	AUC_SCOPE_BOARD = 0,
	AUC_SCOPE_MY_LISTINGS = 1,
	AUC_SCOPE_MY_BIDS = 2,
};

struct PendingPayout { // 0x38B9 handoff awaiting the 0x30B9 verdict
	uint32 auction_id;
	uint32 amount;
	uint32 account_id;
	time_t sent;
	bool reaped; // the timeout fallback already wrote the wallet
};
// Keyed by (char_id, auction_id): a char_id alone would collapse a seller's
// concurrent settlements into one entry and pay out the wrong lot (audit
// P0-4). Both ids are uint32, so the pair packs into one uint64.
uint64 payout_key(uint32 char_id, uint32 auction_id) {
	return (static_cast<uint64>(char_id) << 32) | auction_id;
}
std::unordered_map<uint64, PendingPayout> pending_payouts;

struct ProbeRow { // reconcile rows waiting for their txid probe verdict
	uint32 auction_id;
	uint32 seller_char_id;
	uint32 account_id;
	uint64 unique_id;
	time_t sent;
};
std::unordered_map<uint64, ProbeRow> reconcile_probes;

int32 reconcile_timer_id = INVALID_TIMER;

// Low-level helpers -----------------------------------------------------------

void reply_ack(int32 fd, uint16 id, const void* body, int32 body_len) {
	WFIFOHEAD(fd, 4 + body_len);
	WFIFOW(fd, 0) = id;
	WFIFOW(fd, 2) = static_cast<uint16>(4 + body_len);
	memcpy(WFIFOP(fd, 4), body, body_len);
	WFIFOSET(fd, 4 + body_len);
}

uint32 account_id_of(uint32 char_id) {
	if (SQL_ERROR == Sql_Query(sql_handle, "SELECT `account_id` FROM `char` WHERE `char_id`='%u'", char_id))
		return 0;
	uint32 account_id = 0;
	if (SQL_SUCCESS == Sql_NextRow(sql_handle)) {
		char* data = nullptr; size_t len = 0;
		Sql_GetData(sql_handle, 0, &data, &len);
		account_id = data != nullptr ? strtoul(data, nullptr, 10) : 0;
	}
	Sql_FreeResult(sql_handle);
	return account_id;
}

bool char_name(uint32 char_id, char* out, size_t out_len) {
	if (SQL_ERROR == Sql_Query(sql_handle, "SELECT `name` FROM `char` WHERE `char_id`='%u'", char_id))
		return false;
	bool ok = SQL_SUCCESS == Sql_NextRow(sql_handle);
	if (ok) {
		char* data = nullptr; size_t len = 0;
		Sql_GetData(sql_handle, 0, &data, &len);
		safestrncpy(out, data != nullptr ? data : "", out_len);
	}
	Sql_FreeResult(sql_handle);
	return ok;
}

int32 mapfd_for_char(uint32 char_id) {
	const uint32 account_id = account_id_of(char_id);
	if (account_id == 0)
		return -1;
	std::shared_ptr<struct online_char_data> character = rathena::util::umap_find(char_get_onlinedb(), account_id);
	if (character == nullptr || character->server < 0)
		return -1;
	return map_server[character->server].fd;
}

// Offline cash write (A12). Never used for online players: the map keeps a
// cached copy and would overwrite this on the next account-reg save.
// `out_value` (optional) receives the resulting balance so the 0x7E36
// notification can carry it — the wire semantics of event 6 are "new
// balance", and the map's online path already sends sd->cashPoints.
bool acc_cash_add(uint32 account_id, int64 amount, int64* out_value = nullptr) {
	if (SQL_ERROR == Sql_Query(sql_handle, "SELECT `value` FROM `acc_reg_num` WHERE `account_id`='%u' AND `key`='#CASHPOINTS' AND `index`='0'", account_id))
		return false;
	int64 current = 0;
	if (SQL_SUCCESS == Sql_NextRow(sql_handle)) {
		char* data = nullptr; size_t len = 0;
		Sql_GetData(sql_handle, 0, &data, &len);
		current = data != nullptr ? strtoll(data, nullptr, 10) : 0;
	}
	Sql_FreeResult(sql_handle);
	const int64 target = current + amount;
	// Cap at the wallet ceiling: sd->cashPoints is int32, so a legacy row
	// priced above INT_MAX must not wrap the stored value (audit P2-4).
	if (target < 0 || target > static_cast<int64>(MAX_CASHPOINT))
		return false;
	if (SQL_ERROR == Sql_Query(sql_handle, "INSERT INTO `acc_reg_num` (`account_id`,`key`,`index`,`value`) VALUES ('%u','#CASHPOINTS','0','%lld') ON DUPLICATE KEY UPDATE `value`='%lld'", account_id, static_cast<long long>(target), static_cast<long long>(target)))
		return false;
	if (out_value != nullptr)
		*out_value = target;
	return true;
}

void notify_char(uint32 char_id, uint8 event, uint8 reason, uint32 auction_id, uint32 value) {
	const int32 mapfd = mapfd_for_char(char_id);
	if (mapfd < 0)
		return;
	uint8 body[14];
	uint16 o = 0;
	auto put32 = [&](int32 v) { memcpy(body + o, &v, 4); o += 4; };
	auto put8 = [&](uint8 v) { body[o++] = v; };
	put32(static_cast<int32>(char_id));
	put8(event);
	put8(reason);
	put32(static_cast<int32>(auction_id));
	put32(static_cast<int32>(value));
	reply_ack(mapfd, 0x38B6, body, o);
}

// Cash payout per A12: online -> 0x38B9 instruction (the 0x30B9 verdict
// drives the offline fallback), offline -> direct acc_reg_num write.
void cash_payout(uint32 char_id, uint32 auction_id, uint32 amount) {
	const int32 mapfd = mapfd_for_char(char_id);
	if (mapfd >= 0) {
		uint8 body[13];
		uint16 o = 0;
		auto put32 = [&](int32 v) { memcpy(body + o, &v, 4); o += 4; };
		put32(static_cast<int32>(char_id));
		put32(static_cast<int32>(auction_id));
		put32(static_cast<int32>(amount));
		body[o++] = 0; // reason: settlement
		reply_ack(mapfd, 0x38B9, body, o);
		PendingPayout pending{};
		pending.auction_id = auction_id;
		pending.amount = amount;
		pending.account_id = account_id_of(char_id);
		pending.sent = time(nullptr);
		pending.reaped = false;
		pending_payouts[payout_key(char_id, auction_id)] = pending;
		return;
	}
	const uint32 account_id = account_id_of(char_id);
	int64 new_value = 0;
	if (account_id == 0 || !acc_cash_add(static_cast<int64>(account_id), static_cast<int64>(amount), &new_value)) {
		ShowError("auction cash payout failed: char_id=%u auction_id=%u amount=%u\n", char_id, auction_id, amount);
		return;
	}
	// Event 6 carries the resulting balance, matching the map's online path.
	notify_char(char_id, AUCTION_NOTIFY_CASH_CHANGED, AUCTION_REASON_OK, auction_id, static_cast<uint32>(new_value));
}

// Single settlement entry for both the 0x30B9 verdict and the reap timeout,
// so a payout is written exactly once per lot from THIS process (audit
// P0-4): map applied -> nothing to do; map failed/timed out -> the offline
// write plus the balance notification. A verdict that arrives after the reap
// finds the reaped marker and is dropped with a warning (the map's own late
// write cannot be undone in-process; a durable ledger would be needed to
// close that window fully).
void payout_resolve(uint32 char_id, uint32 auction_id, bool map_applied) {
	auto it = pending_payouts.find(payout_key(char_id, auction_id));
	if (it == pending_payouts.end())
		return; // verdict already consumed, or nothing was ever pending
	PendingPayout& pending = it->second;
	if (pending.reaped) {
		// The timeout fallback already credited the wallet; the late verdict
		// must not trigger a second write.
		if (map_applied)
			ShowWarning("auction cash payout: late verdict after reap, possible double credit, char_id=%u auction_id=%u amount=%u\n", char_id, auction_id, pending.amount);
		pending_payouts.erase(it);
		return;
	}
	if (map_applied) {
		pending_payouts.erase(it);
		return;
	}
	const uint32 account_id = account_id_of(char_id);
	int64 new_value = 0;
	if (account_id == 0 || !acc_cash_add(static_cast<int64>(account_id), static_cast<int64>(pending.amount), &new_value)) {
		ShowError("auction cash payout fallback failed: char_id=%u auction_id=%u amount=%u\n", char_id, auction_id, pending.amount);
		return;
	}
	pending.reaped = true; // wallet credited: a late verdict must not re-add
	notify_char(char_id, AUCTION_NOTIFY_CASH_CHANGED, AUCTION_REASON_OK, auction_id, static_cast<uint32>(new_value));
}

// Mail helpers: mail_sendmail_in_tx joins the CALLER's transaction (A4 /
// audit P0-2). The plain mail_sendmail manages its own transaction and its
// START TRANSACTION would implicitly commit the auction writes around it.
//
// Source literals in this file are UTF-8; the SQL connection and the game client
// both speak the server's data codepage (GBK here). Handing the raw UTF-8 bytes
// of "拍卖行" (E6 8B 8D E5 8D 96 E8 A1 8C) to mail_sendmail() made MySQL reject
// every auction mail with "Incorrect string value: '\x8C' for column
// 'send_name'" — read as GBK the last byte dangles — and the rollback took the
// listing with it. Convert at this single boundary instead.
static std::string data_text(const char* utf8) {
	return utf8 == nullptr ? std::string() : convert_utf8_file_content(utf8, std::strlen(utf8));
}

// Mail zeny is an int32 column and mail_zeny silently skips non-positive
// amounts, so a legacy row priced above INT_MAX would make its seller wait
// forever for a payout that never comes. Clamp at the zeny ceiling instead
// (audit P2-4); rows priced that high cannot be paid by any wallet anyway
// once the map-side upper bound is in force.
int32 zeny_mail_amount(uint64 value) {
	return static_cast<int32>(std::min<uint64>(value, MAX_ZENY));
}

bool mail_zeny(uint32 dest_char_id, const char* dest_name, int32 zeny, const char* title) {
	if (zeny <= 0)
		return true;
	const std::string sender = data_text("拍卖行");
	const std::string subject = data_text(title);
	const std::string body = data_text("拍卖行结算");
	return mail_sendmail_in_tx(0, sender.c_str(), dest_char_id, dest_name, subject.c_str(), body.c_str(), zeny, nullptr, 0) ? true : false;
}

bool mail_item(uint32 dest_char_id, const char* dest_name, const struct item& it, const char* title) {
	const std::string sender = data_text("拍卖行");
	const std::string subject = data_text(title);
	const std::string body = data_text("拍卖行交付");
	return mail_sendmail_in_tx(0, sender.c_str(), dest_char_id, dest_name, subject.c_str(), body.c_str(), 0, &const_cast<struct item&>(it), 1) ? true : false;
}

uint64 fee_of(uint32 price, uint16 permille) {
	// price <= 2^31-1 and permille <= 500 always fit int64; the overflow
	// branch is unreachable but guarded anyway (A13 fee math rule).
	int64 result = 0;
	if (rathena::util::safe_multiplication(static_cast<int64>(price), static_cast<int64>(permille), result) || result < 0)
		return price;
	return static_cast<uint64>(result) / 1000;
}

// Writes one page row at `out` and returns the cursor for the next row:
// exactly `row_size` bytes further on. Returning the cursor (instead of
// taking it by reference and letting the caller step it too) keeps a single
// owner of the stride. The old `append_row(...); rows_out += row_size;` pair
// advanced twice and left a zeroed 45-byte hole between every two rows, which
// the client painted as "Item #0 / x0 / 0 Z / 已结束".
uint8* append_row(uint8* out, uint32 auction_id, uint32 nameid, uint16 amount, uint8 refine, uint8 type, uint8 cards, uint8 options, uint32 price_now, uint32 price_buynow, uint32 expire, uint8 currency, const char* seller, uint8 status, uint8 row_size) {
	memcpy(out, &auction_id, 4); out += 4;
	memcpy(out, &nameid, 4); out += 4;
	memcpy(out, &amount, 2); out += 2;
	*out++ = refine;
	*out++ = type;
	// Counts only: the board needs "how many sockets are filled / how many
	// random options", while the contents ride the detail blob.
	*out++ = cards;
	*out++ = options;
	memcpy(out, &price_now, 4); out += 4;
	memcpy(out, &price_buynow, 4); out += 4;
	memcpy(out, &expire, 4); out += 4;
	*out++ = 0; // flags
	*out++ = currency;
	memset(out, 0, 19);
	safestrncpy(reinterpret_cast<char*>(out), seller, 19);
	out += 19;
	if (row_size >= 48)
		*out++ = status;
	return out;
}

// Handlers --------------------------------------------------------------------

void parse_search(int32 fd) {
	const uint32 char_id = RFIFOL(fd, 2);
	const uint16 page = RFIFOW(fd, 6);
	const uint8 scope = RFIFOB(fd, 8);
	const uint8 currency = RFIFOB(fd, 9);
	const uint8 type = RFIFOB(fd, 10);
	const uint8 refine_min = RFIFOB(fd, 11);
	const uint8 refine_max = RFIFOB(fd, 12);
	const uint32 price_min = RFIFOL(fd, 13);
	const uint32 price_max = RFIFOL(fd, 17);
	const uint8 sort = RFIFOB(fd, 21);
	const uint8 nameid_count = std::min<uint8>(RFIFOB(fd, 22), AUCTION_MAX_NAMEID_FILTERS);
	const uint8 board_kind = RFIFOB(fd, 151);

	uint16 zc_id = 0x7e31;
	uint8 row_size = AUCTION_SEARCH_ROW_SIZE;
	std::string where = "`status`='1'";
	if (scope == AUC_SCOPE_MY_LISTINGS) {
		zc_id = 0x7e34;
		row_size = AUCTION_MY_ROW_SIZE;
		where = "`seller_char_id`='" + std::to_string(char_id) + "' AND `status` IN (1,2,3,5)";
	} else if (scope == AUC_SCOPE_MY_BIDS) {
		zc_id = 0x7e35;
		row_size = AUCTION_MY_ROW_SIZE;
		where = "`top_bidder_char_id`='" + std::to_string(char_id) + "' AND `status`='1'";
	} else {
		if (currency != AUCTION_CURRENCY_ANY)
			where += " AND `currency`='" + std::to_string(currency) + "'";
		if (type == AUCTION_TYPE_CONSUMABLE)
			// One tab, three real item types: potions/usable/delay-consume.
			where += " AND `type` IN (0,2,10)";
		else if (type != AUCTION_TYPE_ANY)
			where += " AND `type`='" + std::to_string(type) + "'";
		if (refine_max != 0xFF) {
			where += " AND `refine`>='" + std::to_string(refine_min) + "'";
			where += " AND `refine`<='" + std::to_string(refine_max) + "'";
		}
		// Price filters run on the price the board displays: the 定价 board
		// shows price_buynow, the 竞价 board the standing price_now. Filtering
		// one column while displaying the other silently dropped rows (audit
		// P2-3).
		const char* price_column = board_kind == AUCTION_KIND_BUYNOW ? "`price_buynow`" : "`price_now`";
		if (price_min > 0)
			where += std::string(" AND ") + price_column + ">='" + std::to_string(price_min) + "'";
		if (price_max > 0)
			where += std::string(" AND ") + price_column + "<='" + std::to_string(price_max) + "'";
		// Board kind: the 定价 board lists only lots with a fixed price, the
		// 竞价 board only pure bid lots. A lot appears on exactly one board —
		// `price_buynow` > 0 settles by buy-out (legacy two-price rows count
		// as 定价), `price_buynow` = 0 is bidding only.
		if (board_kind == AUCTION_KIND_BUYNOW)
			where += " AND `price_buynow`>'0'";
		else if (board_kind == AUCTION_KIND_BID)
			where += " AND `price_buynow`='0'";
		if (nameid_count > 0) {
			std::string ids;
			for (uint8 i = 0; i < nameid_count; ++i) {
				const uint16 nameid = RFIFOW(fd, 23 + i * 2);
				if (nameid == 0)
					continue;
				ids += (ids.empty() ? "" : ",") + std::to_string(nameid);
			}
			where += ids.empty() ? " AND 0" : " AND `nameid` IN (" + ids + ")";
		}
	}
	// auction_id is the stable secondary key everywhere: ties (equal prices)
	// must not reshuffle between pages, or pagination repeats and drops rows
	// (audit P2-3).
	const char* order = "`price_now` ASC, `auction_id` ASC";
	switch (sort) {
		case 1: order = "`price_now` DESC, `auction_id` ASC"; break;
		case 2: order = "`expire_time` ASC, `auction_id` ASC"; break;
		case 3: order = "`price_buynow` DESC, `price_now` ASC, `auction_id` ASC"; break;
		default: break;
	}

	uint8 body[4 + 4 + 2 + 2 + 2 + 1 + 1 + 1 + AUCTION_PAGE_SIZE * AUCTION_MY_ROW_SIZE];
	memset(body, 0, sizeof(body));
	uint16 o = 0;
	body[o++] = 0; // kind 0 page
	uint32 cid = char_id;
	memcpy(body + o, &cid, 4); o += 4;
	memcpy(body + o, &zc_id, 2); o += 2;
	uint16 total_pages = 1;
	uint16 cur_page = page;

	if (SQL_ERROR == Sql_Query(sql_handle, "SELECT COUNT(*) FROM `auction_custom` WHERE %s", where.c_str())) {
		Sql_FreeResult(sql_handle);
		o += 4; // total_pages zeroed
		body[o++] = 0; // count
		body[o++] = row_size;
		body[o++] = (zc_id == 0x7e31) ? board_kind : AUCTION_KIND_ANY;
		reply_ack(fd, 0x38B2, body, o);
		return;
	}
	if (SQL_SUCCESS == Sql_NextRow(sql_handle)) {
		char* data = nullptr; size_t len = 0;
		Sql_GetData(sql_handle, 0, &data, &len);
		const uint64 rows = data != nullptr ? strtoull(data, nullptr, 10) : 0;
		total_pages = static_cast<uint16>((rows + AUCTION_PAGE_SIZE - 1) / AUCTION_PAGE_SIZE);
		if (total_pages == 0)
			total_pages = 1;
		if (cur_page >= total_pages)
			cur_page = 0;
	}
	Sql_FreeResult(sql_handle);

	memcpy(body + o, &total_pages, 2); o += 2;
	memcpy(body + o, &cur_page, 2); o += 2;
	const uint16 count_slot = o; o += 1;
	const uint16 row_size_slot = o; o += 1;
	// Board-kind echo for the 0x7E31 search page: the client files each reply
	// under its own board cache (定价 / 竞价), so a slow reply can never paint
	// rows of one board onto the other. The my-pages (0x7E34/0x7E35) carry
	// ANY.
	const uint16 kind_slot = o; o += 1;
	body[count_slot] = 0;
	body[row_size_slot] = row_size;
	body[kind_slot] = (zc_id == 0x7e31) ? board_kind : AUCTION_KIND_ANY;
	uint8* rows_out = body + o;
	uint8 count = 0;
	if (SQL_ERROR == Sql_Query(sql_handle, "SELECT `auction_id`,`nameid`,`amount`,`refine`,`type`,`price_now`,`price_buynow`,`expire_time`,`currency`,`seller_name`,`status`,`card0`,`card1`,`card2`,`card3`,`option_id0`,`option_id1`,`option_id2`,`option_id3`,`option_id4` FROM `auction_custom` WHERE %s ORDER BY %s LIMIT %u OFFSET %u", where.c_str(), order, AUCTION_PAGE_SIZE, static_cast<uint32>(cur_page) * AUCTION_PAGE_SIZE)) {
		Sql_FreeResult(sql_handle);
		reply_ack(fd, 0x38B2, body, o);
		return;
	}
	while (count < AUCTION_PAGE_SIZE && SQL_SUCCESS == Sql_NextRow(sql_handle)) {
		auto col = [&](int32 i) { char* d = nullptr; size_t l = 0; Sql_GetData(sql_handle, i, &d, &l); return d != nullptr ? d : "0"; };
		uint32 v[8];
		for (int32 i = 0; i < 8; ++i) v[i] = static_cast<uint32>(strtoul(col(i), nullptr, 10));
		const uint8 currency_byte = static_cast<uint8>(strtoul(col(8), nullptr, 10));
		char seller[NAME_LENGTH];
		safestrncpy(seller, col(9), NAME_LENGTH);
		const uint8 status_byte = static_cast<uint8>(strtoul(col(10), nullptr, 10));
		// The board only needs how many sockets are filled and how many random
		// options are rolled; both are stored as fixed zero-filled slots, so a
		// zero id means empty. Contents still come with the detail blob.
		uint8 cards = 0;
		for (int32 i = 11; i < 15; ++i)
			if (strtol(col(i), nullptr, 10) != 0)
				++cards;
		uint8 options = 0;
		for (int32 i = 15; i < 20; ++i)
			if (strtol(col(i), nullptr, 10) != 0)
				++options;
		rows_out = append_row(rows_out, v[0], v[1], static_cast<uint16>(v[2]), static_cast<uint8>(v[3]), static_cast<uint8>(v[4]), cards, options, v[5], v[6], v[7], currency_byte, seller, status_byte, row_size);
		++count;
	}
	Sql_FreeResult(sql_handle);
	body[count_slot] = count;
	body[row_size_slot] = row_size;
	reply_ack(fd, 0x38B2, body, o + count * row_size);
}

// Insert one ESCROWING row inside the caller's transaction. The caller owns
// START TRANSACTION / COMMIT / ROLLBACK; both the single-item escrow and the
// v2 atomic batch call this exactly once per row. fee/deposit permille are
// locked per row at register time: without the columns the INSERT fell back
// to the table defaults and every settlement charged 0‰ regardless of
// auction.conf (audit P1-1).
bool insert_auction_row(uint64 txid, uint32 char_id, char* seller, uint32 price, uint32 buy_now,
	uint8 currency, uint16 fee_permille, uint16 deposit_permille, uint32 expire, const struct item& it,
	uint8 item_type, uint16 blob_version, uint32& out_auction_id) {
	SqlStmt stmt(*sql_handle);
	if (SQL_ERROR == stmt.PrepareStr("INSERT INTO `auction_custom` (`escrow_txid`,`seller_char_id`,`seller_name`,`price_now`,`price_buynow`,`currency`,`status`,`fee_permille`,`deposit_permille`,`expire_time`,`nameid`,`amount`,`refine`,`type`,`card0`,`card1`,`card2`,`card3`,`option_id0`,`option_val0`,`option_parm0`,`option_id1`,`option_val1`,`option_parm1`,`option_id2`,`option_val2`,`option_parm2`,`option_id3`,`option_val3`,`option_parm3`,`option_id4`,`option_val4`,`option_parm4`,`unique_id`,`enchantgrade`,`blob_version`,`item_blob`) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"))
		return false;
	size_t p = 0;
	stmt.BindParam(p++, SQLDT_UINT64, &txid, sizeof(txid));
	stmt.BindParam(p++, SQLDT_UINT32, &char_id, sizeof(char_id));
	stmt.BindParam(p++, SQLDT_STRING, seller, strlen(seller));
	stmt.BindParam(p++, SQLDT_UINT32, &price, sizeof(price));
	stmt.BindParam(p++, SQLDT_UINT32, &buy_now, sizeof(buy_now));
	stmt.BindParam(p++, SQLDT_UINT8, &currency, sizeof(currency));
	uint8 status_escrowing = AUCTION_ST_ESCROWING;
	stmt.BindParam(p++, SQLDT_UINT8, &status_escrowing, sizeof(status_escrowing));
	uint16 fee16 = fee_permille;
	uint16 deposit16 = deposit_permille;
	stmt.BindParam(p++, SQLDT_UINT16, &fee16, sizeof(fee16));
	stmt.BindParam(p++, SQLDT_UINT16, &deposit16, sizeof(deposit16));
	stmt.BindParam(p++, SQLDT_UINT32, &expire, sizeof(expire));
	uint32 nameid32 = it.nameid;
	stmt.BindParam(p++, SQLDT_UINT32, &nameid32, sizeof(nameid32));
	uint16 amount16 = it.amount;
	stmt.BindParam(p++, SQLDT_UINT16, &amount16, sizeof(amount16));
	uint8 refine8 = static_cast<uint8>(it.refine);
	stmt.BindParam(p++, SQLDT_UINT8, &refine8, sizeof(refine8));
	stmt.BindParam(p++, SQLDT_UINT8, &item_type, sizeof(item_type));
	uint32 cards[4];
	for (int32 i = 0; i < 4; ++i) cards[i] = it.card[i];
	stmt.BindParam(p++, SQLDT_UINT32, &cards[0], sizeof(uint32));
	stmt.BindParam(p++, SQLDT_UINT32, &cards[1], sizeof(uint32));
	stmt.BindParam(p++, SQLDT_UINT32, &cards[2], sizeof(uint32));
	stmt.BindParam(p++, SQLDT_UINT32, &cards[3], sizeof(uint32));
	uint16 opt_id[5];
	int16 opt_val[5];
	uint8 opt_parm[5];
	for (int32 i = 0; i < 5; ++i) {
		opt_id[i] = it.option[i].id;
		opt_val[i] = it.option[i].value;
		opt_parm[i] = it.option[i].param;
	}
	for (int32 i = 0; i < 5; ++i) stmt.BindParam(p++, SQLDT_UINT16, &opt_id[i], sizeof(uint16));
	for (int32 i = 0; i < 5; ++i) stmt.BindParam(p++, SQLDT_INT16, &opt_val[i], sizeof(int16));
	for (int32 i = 0; i < 5; ++i) stmt.BindParam(p++, SQLDT_UINT8, &opt_parm[i], sizeof(uint8));
	uint64 unique = it.unique_id;
	stmt.BindParam(p++, SQLDT_UINT64, &unique, sizeof(unique));
	uint8 grade = it.enchantgrade;
	stmt.BindParam(p++, SQLDT_UINT8, &grade, sizeof(grade));
	stmt.BindParam(p++, SQLDT_UINT16, &blob_version, sizeof(blob_version));
	uint8 blob_copy[AUCTION_ITEM_BLOB_SIZE];
	memcpy(blob_copy, &it, AUCTION_ITEM_BLOB_SIZE);
	stmt.BindParam(p++, SQLDT_BLOB, blob_copy, AUCTION_ITEM_BLOB_SIZE);
	if (SQL_ERROR == stmt.Execute())
		return false;
	out_auction_id = static_cast<uint32>(stmt.LastInsertId());
	return true;
}

void escrow_ack(int32 fd, uint64 txid, uint32 auction_id, uint8 result, uint8 reason) {
	uint8 body[14];
	memcpy(body, &txid, 8);
	memcpy(body + 8, &auction_id, 4);
	body[12] = result;
	body[13] = reason;
	reply_ack(fd, 0x38B3, body, 14);
}

void parse_escrow(int32 fd) {
	uint32 char_id = RFIFOL(fd, 2);
	uint64 txid = RFIFOQ(fd, 6);
	uint32 price = RFIFOL(fd, 14);
	uint32 buy_now = RFIFOL(fd, 18);
	uint8 currency = RFIFOB(fd, 22);
	uint8 duration_h = RFIFOB(fd, 23);
	uint16 fee_permille = RFIFOW(fd, 24);
	uint16 deposit_permille = RFIFOW(fd, 26);
	uint16 blob_version = RFIFOW(fd, 28);
	uint32 auction_id = 0;
	struct item it;
	memset(&it, 0, sizeof(it));
	memcpy(&it, RFIFOP(fd, 30), AUCTION_ITEM_BLOB_SIZE);
	uint8 item_type = RFIFOB(fd, 107);
	const uint8 cap = RFIFOB(fd, 108) == 0 ? 20 : RFIFOB(fd, 108);

	if (SQL_ERROR == Sql_Query(sql_handle, "START TRANSACTION")) {
		escrow_ack(fd, txid, 0, AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL);
		return;
	}
	bool ok = SQL_ERROR != Sql_Query(sql_handle, "SELECT COUNT(*) FROM `auction_custom` WHERE `seller_char_id`='%u' AND `status` IN (0,1)", char_id);
	uint64 count = 0;
	if (ok && SQL_SUCCESS == Sql_NextRow(sql_handle)) {
		char* data = nullptr; size_t len = 0;
		Sql_GetData(sql_handle, 0, &data, &len);
		count = data != nullptr ? strtoull(data, nullptr, 10) : 0;
	}
	Sql_FreeResult(sql_handle);
	if (!ok) {
		Sql_Query(sql_handle, "ROLLBACK");
		escrow_ack(fd, txid, 0, AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL);
		return;
	}
	if (count >= cap) {
		Sql_Query(sql_handle, "ROLLBACK");
		escrow_ack(fd, txid, 0, AUCTION_RESULT_FAIL, AUCTION_REASON_LISTING_CAP);
		return;
	}
	char seller[NAME_LENGTH];
	if (!char_name(char_id, seller, NAME_LENGTH)) {
		Sql_Query(sql_handle, "ROLLBACK");
		escrow_ack(fd, txid, 0, AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL);
		return;
	}
	uint32 expire = static_cast<uint32>(time(nullptr)) + static_cast<uint32>(duration_h) * 3600;
	{
		// escrow_txid doubles as the idempotency key: a retried request for
		// the same txid violates the txid-keyed lookup and is refused instead
		// of duplicating the escrow.
		if (SQL_ERROR == Sql_Query(sql_handle, "SELECT `auction_id` FROM `auction_custom` WHERE `escrow_txid`='%llu' AND `status`='0' LIMIT 1", static_cast<unsigned long long>(txid))) {
			Sql_Query(sql_handle, "ROLLBACK");
			escrow_ack(fd, txid, 0, AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL);
			return;
		}
		if (SQL_SUCCESS == Sql_NextRow(sql_handle)) {
			char* data = nullptr; size_t len = 0;
			Sql_GetData(sql_handle, 0, &data, &len);
			const uint32 existing = data != nullptr ? strtoul(data, nullptr, 10) : 0;
			Sql_FreeResult(sql_handle);
			Sql_Query(sql_handle, "COMMIT");
			escrow_ack(fd, txid, existing, AUCTION_RESULT_OK, AUCTION_REASON_OK);
			return;
		}
		Sql_FreeResult(sql_handle);
	}
	if (!insert_auction_row(txid, char_id, seller, price, buy_now, currency, fee_permille, deposit_permille, expire, it, item_type, blob_version, auction_id)) {
		Sql_Query(sql_handle, "ROLLBACK");
		escrow_ack(fd, txid, 0, AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL);
		return;
	}
	if (SQL_ERROR == Sql_Query(sql_handle, "COMMIT")) {
		escrow_ack(fd, txid, auction_id, AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL);
		return;
	}
	escrow_ack(fd, txid, auction_id, AUCTION_RESULT_OK, AUCTION_REASON_OK);
}

// ===== v2 atomic batch: 开始 =====
// 0x30BA ESCROW_BATCH_REQ (variable, len.W@2):
//   char_id.L@4 batch_id.Q@6 count.B@14 currency.B@15 duration_h.B@16
//   fee_permille.W@17 deposit_permille.W@19 cap.B@21 reserved.B@22
//   rows@23 (96B): txid.Q price.L buy_now.L blob_version.W blob.77B item_type.B
// 0x38BA ESCROW_BATCH_ACK: batch_id.Q@4 result.B@12 reason.B@13
//   fail_row.B@14 count.B@15 auction_ids.L[count]@16
// 0x30BB COMMIT_BATCH_REQ: batch_id.Q@4 count.B@12 reserved.3B@13
//   rows@16 (12B): txid.Q auction_id.L
// 0x38BB COMMIT_BATCH_ACK: batch_id.Q@4 result.B@12 reason.B@13
void escrow_batch_ack(int32 fd, uint64 batch_id, uint8 result, uint8 reason, uint8 fail_row,
	const uint32* auction_ids, uint8 count) {
	uint8 body[12 + AUCTION_MAX_BATCH_ITEMS * 4] = {};
	memcpy(body, &batch_id, 8);
	body[8] = result;
	body[9] = reason;
	body[10] = fail_row;
	body[11] = count;
	for (uint8 i = 0; i < count; ++i) {
		const uint32 id = auction_ids != nullptr ? auction_ids[i] : 0;
		memcpy(body + 12 + i * 4, &id, 4);
	}
	reply_ack(fd, 0x38BA, body, 12 + count * 4);
}

void commit_batch_ack(int32 fd, uint64 batch_id, uint8 result, uint8 reason) {
	uint8 body[10];
	memcpy(body, &batch_id, 8);
	body[8] = result;
	body[9] = reason;
	reply_ack(fd, 0x38BB, body, 10);
}

// All N rows are inserted inside ONE transaction. Any failure rolls the
// whole batch back, so the map server can safely leave the inventory alone.
void parse_escrow_batch(int32 fd) {
	const uint32 char_id = RFIFOL(fd, 4);
	const uint64 batch_id = RFIFOQ(fd, 8);
	const uint8 count = RFIFOB(fd, 16);
	const uint8 currency = RFIFOB(fd, 17);
	const uint8 duration_h = RFIFOB(fd, 18);
	const uint16 fee_permille = RFIFOW(fd, 19);
	const uint16 deposit_permille = RFIFOW(fd, 21);
	const uint8 cap_byte = RFIFOB(fd, 23);
	const uint8 cap = cap_byte == 0 ? 20 : cap_byte;
	uint32 auction_ids[AUCTION_MAX_BATCH_ITEMS] = {};
	ShowInfo("auction batch escrow recv: char_id=%u batch_id=%llu count=%u currency=%u duration_h=%u cap=%u\n",
		char_id, static_cast<unsigned long long>(batch_id), count, currency, duration_h, cap);

	if (count == 0 || count > AUCTION_MAX_BATCH_ITEMS) {
		ShowError("auction batch escrow FAIL: invalid count=%u\n", count);
		escrow_batch_ack(fd, batch_id, AUCTION_RESULT_FAIL, AUCTION_REASON_TOO_MANY_ITEMS, 0, nullptr, 0);
		return;
	}
	// Variable-length frame: the declared length must match the row count, or
	// the per-row reads below walk past the buffer (audit P3).
	if (RFIFOW(fd, 2) != AUCTION_INTER_BATCH_HDR_SIZE + static_cast<uint32>(count) * AUCTION_INTER_BATCH_ROW_SIZE) {
		ShowError("auction batch escrow FAIL: len=%u does not match count=%u\n", RFIFOW(fd, 2), count);
		escrow_batch_ack(fd, batch_id, AUCTION_RESULT_FAIL, AUCTION_REASON_TOO_MANY_ITEMS, 0, nullptr, 0);
		return;
	}
	if (SQL_ERROR == Sql_Query(sql_handle, "START TRANSACTION")) {
		ShowError("auction batch escrow FAIL: START TRANSACTION\n");
		escrow_batch_ack(fd, batch_id, AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL, 0, nullptr, count);
		return;
	}
	uint64 existing = 0;
	bool ok = SQL_ERROR != Sql_Query(sql_handle, "SELECT COUNT(*) FROM `auction_custom` WHERE `seller_char_id`='%u' AND `status` IN (0,1)", char_id);
	if (ok && SQL_SUCCESS == Sql_NextRow(sql_handle)) {
		char* data = nullptr; size_t len = 0;
		Sql_GetData(sql_handle, 0, &data, &len);
		existing = data != nullptr ? strtoull(data, nullptr, 10) : 0;
	}
	Sql_FreeResult(sql_handle);
	if (!ok) {
		ShowError("auction batch escrow FAIL: listing count query\n");
		Sql_Query(sql_handle, "ROLLBACK");
		escrow_batch_ack(fd, batch_id, AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL, 0, nullptr, count);
		return;
	}
	// The cap must cover the WHOLE batch, not just its first row.
	if (existing + count > cap) {
		ShowError("auction batch escrow LISTING_CAP existing=%llu count=%u cap=%u\n",
			static_cast<unsigned long long>(existing), count, cap);
		Sql_Query(sql_handle, "ROLLBACK");
		escrow_batch_ack(fd, batch_id, AUCTION_RESULT_FAIL, AUCTION_REASON_LISTING_CAP, 0, nullptr, count);
		return;
	}
	char seller[NAME_LENGTH];
	if (!char_name(char_id, seller, NAME_LENGTH)) {
		ShowError("auction batch escrow FAIL: char_name lookup char_id=%u\n", char_id);
		Sql_Query(sql_handle, "ROLLBACK");
		escrow_batch_ack(fd, batch_id, AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL, 0, nullptr, count);
		return;
	}
	const uint32 expire = static_cast<uint32>(time(nullptr)) + static_cast<uint32>(duration_h) * 3600;
	for (uint8 i = 0; i < count; ++i) {
		const int32 base = AUCTION_INTER_BATCH_HDR_SIZE + i * AUCTION_INTER_BATCH_ROW_SIZE;
		const uint64 txid = RFIFOQ(fd, base);
		const uint32 price = RFIFOL(fd, base + 8);
		const uint32 buy_now = RFIFOL(fd, base + 12);
		const uint16 blob_version = RFIFOW(fd, base + 16);
		struct item it;
		memset(&it, 0, sizeof(it));
		memcpy(&it, RFIFOP(fd, base + 18), AUCTION_ITEM_BLOB_SIZE);
		const uint8 item_type = RFIFOB(fd, base + 95);
		if (!insert_auction_row(txid, char_id, seller, price, buy_now, currency, fee_permille, deposit_permille, expire, it, item_type, blob_version, auction_ids[i])) {
			ShowError("auction batch escrow FAIL: insert row=%u txid=%llu price=%u buynow=%u\n",
				i, static_cast<unsigned long long>(txid), price, buy_now);
			Sql_Query(sql_handle, "ROLLBACK");
			escrow_batch_ack(fd, batch_id, AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL, i, nullptr, count);
			return;
		}
	}
	if (SQL_ERROR == Sql_Query(sql_handle, "COMMIT")) {
		ShowError("auction batch escrow FAIL: COMMIT\n");
		escrow_batch_ack(fd, batch_id, AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL, 0, nullptr, count);
		return;
	}
	ShowInfo("auction batch escrow OK: inserted %u rows, first auction_id=%u\n", count, auction_ids[0]);
	escrow_batch_ack(fd, batch_id, AUCTION_RESULT_OK, AUCTION_REASON_OK, 0xFF, auction_ids, count);
}

// Flip all N ESCROWING rows to ACTIVE in one transaction.
void parse_commit_batch(int32 fd) {
	const uint64 batch_id = RFIFOQ(fd, 4);
	const uint8 count = RFIFOB(fd, 12);
	if (count == 0 || count > AUCTION_MAX_BATCH_ITEMS) {
		commit_batch_ack(fd, batch_id, AUCTION_RESULT_FAIL, AUCTION_REASON_TOO_MANY_ITEMS);
		return;
	}
	// Variable-length frame: declared length must match the row count (audit P3).
	if (RFIFOW(fd, 2) != 16 + static_cast<uint32>(count) * 12) {
		commit_batch_ack(fd, batch_id, AUCTION_RESULT_FAIL, AUCTION_REASON_TOO_MANY_ITEMS);
		return;
	}
	if (SQL_ERROR == Sql_Query(sql_handle, "START TRANSACTION")) {
		commit_batch_ack(fd, batch_id, AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL);
		return;
	}
	for (uint8 i = 0; i < count; ++i) {
		const int32 base = 16 + i * 12;
		const uint64 txid = RFIFOQ(fd, base);
		const uint32 auction_id = RFIFOL(fd, base + 8);
		if (SQL_ERROR == Sql_Query(sql_handle, "UPDATE `auction_custom` SET `status`='1' WHERE `auction_id`='%u' AND `escrow_txid`='%llu' AND `status`='0'", auction_id, static_cast<unsigned long long>(txid))) {
			Sql_Query(sql_handle, "ROLLBACK");
			commit_batch_ack(fd, batch_id, AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL);
			return;
		}
		if (Sql_NumRowsAffected(sql_handle) == 0) {
			Sql_Query(sql_handle, "ROLLBACK");
			commit_batch_ack(fd, batch_id, AUCTION_RESULT_FAIL, AUCTION_REASON_ALREADY_ENDED);
			return;
		}
	}
	if (SQL_ERROR == Sql_Query(sql_handle, "COMMIT")) {
		commit_batch_ack(fd, batch_id, AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL);
		return;
	}
	commit_batch_ack(fd, batch_id, AUCTION_RESULT_OK, AUCTION_REASON_OK);
}
// ===== v2 atomic batch: 结束 =====

void commit_ack(int32 fd, uint64 txid, uint32 auction_id, uint8 result, uint8 reason) {
	uint8 body[14];
	memcpy(body, &txid, 8);
	memcpy(body + 8, &auction_id, 4);
	body[12] = result;
	body[13] = reason;
	reply_ack(fd, 0x38B4, body, 14);
}

void parse_commit(int32 fd) {
	const uint64 txid = RFIFOQ(fd, 2);
	const uint32 auction_id = RFIFOL(fd, 10);
	// Flip ESCROWING -> ACTIVE; only the row that still matches the txid
	// flips, so a retried/unknown commit is refused (A6).
	if (SQL_ERROR == Sql_Query(sql_handle, "UPDATE `auction_custom` SET `status`='1' WHERE `auction_id`='%u' AND `escrow_txid`='%llu' AND `status`='0'", auction_id, static_cast<unsigned long long>(txid))) {
		commit_ack(fd, txid, auction_id, AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL);
		return;
	}
	const uint64 affected = Sql_NumRowsAffected(sql_handle);
	if (affected == 0) {
		commit_ack(fd, txid, auction_id, AUCTION_RESULT_FAIL, AUCTION_REASON_ALREADY_ENDED);
		return;
	}
	commit_ack(fd, txid, auction_id, AUCTION_RESULT_OK, AUCTION_REASON_OK);
}

void parse_bid(int32 fd) {
	const uint32 auction_id = RFIFOL(fd, 2);
	const uint32 bidder_char_id = RFIFOL(fd, 6);
	const uint32 price = RFIFOL(fd, 10);
	const uint8 currency = RFIFOB(fd, 14);
	const uint32 min_increment = RFIFOL(fd, 15);
	const uint8 min_increment_percent = RFIFOB(fd, 19);

	uint8 result = AUCTION_RESULT_FAIL;
	uint8 reason = AUCTION_REASON_INTERNAL;
	uint32 price_now = 0;
	char bidder_name[NAME_LENGTH];
	if (!char_name(bidder_char_id, bidder_name, NAME_LENGTH)) {
		uint8 body[19];
		memcpy(body, &bidder_char_id, 4);
		memcpy(body + 4, &auction_id, 4);
		body[12] = AUCTION_RESULT_FAIL;
		body[13] = AUCTION_REASON_INTERNAL;
		body[14] = currency;
		memcpy(body + 15, &price_now, 4);
		reply_ack(fd, 0x38B5, body, 19);
		return;
	}
	if (SQL_ERROR == Sql_Query(sql_handle, "START TRANSACTION")) {
		uint8 body[19];
		memcpy(body, &bidder_char_id, 4);
		memcpy(body + 4, &auction_id, 4);
		body[12] = AUCTION_RESULT_FAIL;
		body[13] = AUCTION_REASON_INTERNAL;
		body[14] = currency;
		memcpy(body + 15, &price_now, 4);
		reply_ack(fd, 0x38B5, body, 19);
		return;
	}
	bool ok = SQL_ERROR != Sql_Query(sql_handle, "SELECT `status`,`currency`,`price_now`,`price_buynow`,`fee_permille`,`deposit_permille`,`top_bidder_char_id`,`top_bidder_name`,`seller_char_id`,`seller_name`,`expire_time` FROM `auction_custom` WHERE `auction_id`='%u' FOR UPDATE", auction_id);
	struct AuctionRowData {
		uint8 status; uint8 currency; uint32 price_now; uint32 price_buynow;
		uint16 fee_permille; uint16 deposit_permille; uint32 top_bidder_char_id;
		char top_bidder_name[NAME_LENGTH]; uint32 seller_char_id; char seller_name[NAME_LENGTH]; uint32 expire_time;
	} row{};
	if (ok && SQL_SUCCESS == Sql_NextRow(sql_handle)) {
		auto col = [&](int32 i) { char* d = nullptr; size_t l = 0; Sql_GetData(sql_handle, i, &d, &l); return d != nullptr ? d : "0"; };
		row.status = static_cast<uint8>(strtoul(col(0), nullptr, 10));
		row.currency = static_cast<uint8>(strtoul(col(1), nullptr, 10));
		row.price_now = strtoul(col(2), nullptr, 10);
		row.price_buynow = strtoul(col(3), nullptr, 10);
		row.fee_permille = static_cast<uint16>(strtoul(col(4), nullptr, 10));
		row.deposit_permille = static_cast<uint16>(strtoul(col(5), nullptr, 10));
		row.top_bidder_char_id = strtoul(col(6), nullptr, 10);
		safestrncpy(row.top_bidder_name, col(7), NAME_LENGTH);
		row.seller_char_id = strtoul(col(8), nullptr, 10);
		safestrncpy(row.seller_name, col(9), NAME_LENGTH);
		row.expire_time = strtoul(col(10), nullptr, 10);
	} else {
		ok = false;
	}
	Sql_FreeResult(sql_handle);
	if (!ok) {
		Sql_Query(sql_handle, "ROLLBACK");
		uint8 body[19];
		memcpy(body, &bidder_char_id, 4);
		memcpy(body + 4, &auction_id, 4);
		body[12] = AUCTION_RESULT_FAIL;
		body[13] = AUCTION_REASON_ITEM_NOT_FOUND;
		body[14] = currency;
		memcpy(body + 15, &price_now, 4);
		reply_ack(fd, 0x38B5, body, 19);
		return;
	}
	if (row.status != AUCTION_ST_ACTIVE)
		reason = AUCTION_REASON_ALREADY_ENDED;
	else if (currency != row.currency)
		reason = AUCTION_REASON_CURRENCY_MISMATCH;
	else if (bidder_char_id == row.seller_char_id)
		reason = AUCTION_REASON_OWN_LISTING;
	else if (row.expire_time <= static_cast<uint32>(time(nullptr)))
		reason = AUCTION_REASON_ALREADY_ENDED;
	else if (row.price_buynow != 0 && price >= row.price_buynow)
		// A bid at or above the buy-out is pointless: the lot settles by
		// buy-out at that price, so the money is better spent there (audit P2-3).
		reason = AUCTION_REASON_EXCEEDS_BUYNOW;
	else if (price <= row.price_now)
		reason = AUCTION_REASON_BID_TOO_LOW;
	else if (price < row.price_now + std::max<uint64>(min_increment, fee_of(row.price_now, static_cast<uint16>(min_increment_percent * 10))))
		reason = AUCTION_REASON_BID_TOO_LOW;
	if (reason != AUCTION_REASON_INTERNAL) {
		Sql_Query(sql_handle, "ROLLBACK");
		uint8 body[19];
		memcpy(body, &bidder_char_id, 4);
		memcpy(body + 4, &auction_id, 4);
		body[12] = AUCTION_RESULT_FAIL;
		body[13] = reason;
		body[14] = currency;
		memcpy(body + 15, &row.price_now, 4);
		reply_ack(fd, 0x38B5, body, 19);
		return;
	}
	// Optimistic guard: only a row still at the observed state accepts the
	// new price; racing transactions lose the update and are rejected. The
	// display name is escaped: it comes from the char table, but a quote in a
	// name would otherwise terminate the SQL string (audit P3).
	char escaped_bidder[(NAME_LENGTH) * 2 + 1];
	Sql_EscapeString(sql_handle, escaped_bidder, bidder_name);
	if (SQL_ERROR == Sql_Query(sql_handle, "UPDATE `auction_custom` SET `price_now`='%u', `top_bidder_char_id`='%u', `top_bidder_name`='%s' WHERE `auction_id`='%u' AND `status`='1' AND `price_now`='%u' AND `top_bidder_char_id`='%u'", price, bidder_char_id, escaped_bidder, auction_id, row.price_now, row.top_bidder_char_id)) {
		Sql_Query(sql_handle, "ROLLBACK");
		uint8 body[19];
		memcpy(body, &bidder_char_id, 4);
		memcpy(body + 4, &auction_id, 4);
		body[12] = AUCTION_RESULT_FAIL;
		body[13] = AUCTION_REASON_INTERNAL;
		body[14] = currency;
		memcpy(body + 15, &row.price_now, 4);
		reply_ack(fd, 0x38B5, body, 19);
		return;
	}
	if (Sql_NumRowsAffected(sql_handle) == 0) {
		Sql_Query(sql_handle, "ROLLBACK");
		uint8 body[19];
		memcpy(body, &bidder_char_id, 4);
		memcpy(body + 4, &auction_id, 4);
		body[12] = AUCTION_RESULT_FAIL;
		body[13] = AUCTION_REASON_BID_TOO_LOW;
		body[14] = currency;
		memcpy(body + 15, &row.price_now, 4);
		reply_ack(fd, 0x38B5, body, 19);
		return;
	}
	// Journal entry: the audit backstop for the bid-deduction crash window
	// (ADR §9.2 solution 1 — report, never auto-fix).
	Sql_Query(sql_handle, "INSERT INTO `auction_bid_journal` (`auction_id`,`bidder_char_id`,`price`,`currency`,`created_at`) VALUES ('%u','%u','%u','%u','%u')", auction_id, bidder_char_id, price, currency, static_cast<uint32>(time(nullptr)));
	// Refund the previous top bidder inside the same transaction (zeny path).
	if (row.top_bidder_char_id != 0 && row.currency == AUCTION_CURRENCY_ZENY) {
		if (!mail_zeny(row.top_bidder_char_id, row.top_bidder_name, zeny_mail_amount(row.price_now), "拍卖行出价被超越"))
			ShowError("auction outbid refund mail failed: char_id=%u auction_id=%u price=%u\n", row.top_bidder_char_id, auction_id, row.price_now);
	}
	const bool committed = SQL_ERROR != Sql_Query(sql_handle, "COMMIT");
	if (!committed) {
		uint8 body[19];
		memcpy(body, &bidder_char_id, 4);
		memcpy(body + 4, &auction_id, 4);
		body[12] = AUCTION_RESULT_FAIL;
		body[13] = AUCTION_REASON_INTERNAL;
		body[14] = currency;
		memcpy(body + 15, &row.price_now, 4);
		reply_ack(fd, 0x38B5, body, 19);
		return;
	}
	// Post-commit: cash refunds to the previous bidder (online -> 0x38B9,
	// offline -> acc_reg_num) and the outbid notification.
	if (row.top_bidder_char_id != 0 && row.currency == AUCTION_CURRENCY_CASH)
		cash_payout(row.top_bidder_char_id, auction_id, row.price_now);
	if (row.top_bidder_char_id != 0)
		notify_char(row.top_bidder_char_id, AUCTION_NOTIFY_OUTBID, AUCTION_REASON_OK, auction_id, price);
	uint8 body[19];
	memcpy(body, &bidder_char_id, 4);
	memcpy(body + 4, &auction_id, 4);
	body[12] = AUCTION_RESULT_OK;
	body[13] = AUCTION_REASON_OK;
	body[14] = currency;
	memcpy(body + 15, &price, 4);
	reply_ack(fd, 0x38B5, body, 19);
}

// Settle one due row inside its own transaction (ADR A4: the status flip and
// the delivery mails commit together or not at all).
bool settle_one(uint32 auction_id) {
	if (SQL_ERROR == Sql_Query(sql_handle, "START TRANSACTION"))
		return false;
	bool ok = SQL_ERROR != Sql_Query(sql_handle, "SELECT `status`,`currency`,`price_now`,`price_buynow`,`fee_permille`,`top_bidder_char_id`,`top_bidder_name`,`seller_char_id`,`seller_name`,`item_blob` FROM `auction_custom` WHERE `auction_id`='%u' AND `status`='1' AND `expire_time`<=UNIX_TIMESTAMP() FOR UPDATE", auction_id);
	struct {
		uint8 status; uint8 currency; uint32 price_now; uint32 price_buynow; uint16 fee_permille;
		uint32 top_bidder_char_id; char top_bidder_name[NAME_LENGTH];
		uint32 seller_char_id; char seller_name[NAME_LENGTH];
		struct item it;
	} row{};
	memset(&row, 0, sizeof(row));
	if (ok && SQL_SUCCESS == Sql_NextRow(sql_handle)) {
		auto col = [&](int32 i) { char* d = nullptr; size_t l = 0; Sql_GetData(sql_handle, i, &d, &l); return d != nullptr ? d : "0"; };
		row.status = static_cast<uint8>(strtoul(col(0), nullptr, 10));
		row.currency = static_cast<uint8>(strtoul(col(1), nullptr, 10));
		row.price_now = strtoul(col(2), nullptr, 10);
		row.price_buynow = strtoul(col(3), nullptr, 10);
		row.fee_permille = static_cast<uint16>(strtoul(col(4), nullptr, 10));
		row.top_bidder_char_id = strtoul(col(5), nullptr, 10);
		safestrncpy(row.top_bidder_name, col(6), NAME_LENGTH);
		row.seller_char_id = strtoul(col(7), nullptr, 10);
		safestrncpy(row.seller_name, col(8), NAME_LENGTH);
		size_t len = 0;
		char* blob = nullptr;
		Sql_GetData(sql_handle, 9, &blob, &len);
		if (blob != nullptr && len >= AUCTION_ITEM_BLOB_SIZE)
			memcpy(&row.it, blob, AUCTION_ITEM_BLOB_SIZE);
	} else {
		ok = false;
	}
	Sql_FreeResult(sql_handle);
	if (!ok) {
		Sql_Query(sql_handle, "ROLLBACK");
		return false;
	}
	if (SQL_ERROR == Sql_Query(sql_handle, "UPDATE `auction_custom` SET `status`='2', `buyer_char_id`='%u', `buyer_name`='%s' WHERE `auction_id`='%u' AND `status`='1' AND `expire_time`<=UNIX_TIMESTAMP()", row.top_bidder_char_id, row.top_bidder_name, auction_id)) {
		Sql_Query(sql_handle, "ROLLBACK");
		return false;
	}
	if (Sql_NumRowsAffected(sql_handle) == 0) {
		// Someone else settled it between the select and the update.
		Sql_Query(sql_handle, "ROLLBACK");
		return true;
	}
	const uint64 fee = fee_of(row.price_now, row.fee_permille);
	const uint64 seller_gain = row.price_now > fee ? row.price_now - fee : 0;
	if (row.top_bidder_char_id != 0) {
		if (!mail_item(row.top_bidder_char_id, row.top_bidder_name, row.it, "拍卖行竞拍成功")) {
			Sql_Query(sql_handle, "ROLLBACK");
			return false;
		}
		if (row.currency == AUCTION_CURRENCY_ZENY) {
			if (!mail_zeny(row.seller_char_id, row.seller_name, zeny_mail_amount(seller_gain), "拍卖行成交款")) {
				Sql_Query(sql_handle, "ROLLBACK");
				return false;
			}
		}
	} else {
		// Expired with no bids: item goes back to the seller.
		if (!mail_item(row.seller_char_id, row.seller_name, row.it, "拍卖行流拍退回")) {
			Sql_Query(sql_handle, "ROLLBACK");
			return false;
		}
	}
	if (SQL_ERROR == Sql_Query(sql_handle, "COMMIT")) {
		return false;
	}
	// Post-commit notifications and cash payout (A12 async path). The payout
	// amount is capped at the wallet ceiling: a legacy row priced above
	// INT_MAX must not wrap acc_cash_add (audit P2-4).
	if (row.top_bidder_char_id != 0) {
		notify_char(row.seller_char_id, AUCTION_NOTIFY_SOLD, AUCTION_REASON_OK, auction_id, row.price_now);
		notify_char(row.top_bidder_char_id, AUCTION_NOTIFY_DELIVERED, AUCTION_REASON_OK, auction_id, row.price_now);
		if (row.currency == AUCTION_CURRENCY_CASH)
			cash_payout(row.seller_char_id, auction_id, static_cast<uint32>(std::min<uint64>(seller_gain, MAX_CASHPOINT)));
	} else {
		notify_char(row.seller_char_id, AUCTION_NOTIFY_EXPIRED, AUCTION_REASON_OK, auction_id, 0);
	}
	return true;
}

// Units bought out of a stacked lot are priced pro rata against the lot total
// the seller listed it at: N of M units cost total*N/M, and N == M returns the
// total exactly. A partial purchase writes back the REMAINING total instead of
// re-deriving a unit price, so buying a lot a few units at a time can never
// drift by a zeny — the last units pay whatever is left.
uint32 lot_quote(uint32 total, uint16 units, uint16 want) {
	if (units == 0 || want == 0)
		return 0;
	if (want >= units)
		return total;
	// Integer division of a tiny total can floor to zero, which the wallet
	// and the confirm both refuse — the units would be unpurchasable forever.
	// One unit of currency is the smallest honest price (audit P2-4).
	return std::max<uint32>(1, static_cast<uint32>((static_cast<uint64>(total) * want) / units));
}

// Shrink an ACTIVE listing to the units a partial buy-now left behind. The row
// keeps its lot semantics: the `amount` column and the packed blob move
// together (the blob is what the next sale mails, so the two can never
// disagree), both prices drop by the share that was just paid for, and any
// standing bid is dropped — the caller refunds that bidder separately because
// the bid was cast for the whole lot.
bool update_auction_remainder(uint32 auction_id, uint16 units, uint32 price_now, uint32 price_buynow, const struct item& it) {
	SqlStmt stmt(*sql_handle);
	if (SQL_ERROR == stmt.PrepareStr("UPDATE `auction_custom` SET `amount`=?, `price_now`=?, `price_buynow`=?, `item_blob`=?, `top_bidder_char_id`='0', `top_bidder_name`='' WHERE `auction_id`=? AND `status`='1'"))
		return false;
	uint16 units16 = units;
	uint32 now32 = price_now;
	uint32 buy32 = price_buynow;
	uint32 id32 = auction_id;
	uint8 blob[AUCTION_ITEM_BLOB_SIZE];
	memcpy(blob, &it, AUCTION_ITEM_BLOB_SIZE);
	stmt.BindParam(0, SQLDT_UINT16, &units16, sizeof(units16));
	stmt.BindParam(1, SQLDT_UINT32, &now32, sizeof(now32));
	stmt.BindParam(2, SQLDT_UINT32, &buy32, sizeof(buy32));
	stmt.BindParam(3, SQLDT_BLOB, blob, AUCTION_ITEM_BLOB_SIZE);
	stmt.BindParam(4, SQLDT_UINT32, &id32, sizeof(id32));
	return SQL_ERROR != stmt.Execute();
}

void parse_op(int32 fd) {
	const uint8 op = RFIFOB(fd, 2);
	const uint32 char_id = RFIFOL(fd, 3);
	const uint32 auction_id = RFIFOL(fd, 7);
	const uint32 price = RFIFOL(fd, 11);
	const uint8 currency = RFIFOB(fd, 15);
	// Units for the buy-now quote/confirm; 0 (or absent) means the whole lot.
	const uint16 amount = RFIFOW(fd, 16);

	auto ack = [&](uint8 result, uint8 reason, uint8 cur, uint32 now_price) {
		uint8 body[16];
		body[0] = op;
		memcpy(body + 1, &char_id, 4);
		memcpy(body + 5, &auction_id, 4);
		body[9] = result;
		body[10] = reason;
		body[11] = cur;
		memcpy(body + 12, &now_price, 4);
		reply_ack(fd, 0x38B8, body, 16);
	};

	switch (op) {
		case AUC_OP_CANCEL: {
			// Cancel only before any bid: top_bidder_char_id must be 0. The
			// status flip and the return mail share one transaction: the old
			// order (flip first, mail after) left a mailbox-full or SQL error
			// holding the item forever at status 5 (audit P0-3).
			if (SQL_ERROR == Sql_Query(sql_handle, "START TRANSACTION")) {
				ack(AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL, currency, 0);
				return;
			}
			if (SQL_ERROR == Sql_Query(sql_handle, "UPDATE `auction_custom` SET `status`='5' WHERE `auction_id`='%u' AND `seller_char_id`='%u' AND `status`='1' AND `top_bidder_char_id`='0'", auction_id, char_id)) {
				Sql_Query(sql_handle, "ROLLBACK");
				ack(AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL, currency, 0);
				return;
			}
			if (Sql_NumRowsAffected(sql_handle) == 0) {
				Sql_Query(sql_handle, "ROLLBACK");
				ack(AUCTION_RESULT_FAIL, AUCTION_REASON_ALREADY_ENDED, currency, 0);
				return;
			}
			// Return the escrowed item via mail (works with a full inventory).
			char seller[NAME_LENGTH];
			if (!char_name(char_id, seller, NAME_LENGTH)) {
				Sql_Query(sql_handle, "ROLLBACK");
				ack(AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL, currency, 0);
				return;
			}
			if (SQL_ERROR == Sql_Query(sql_handle, "SELECT `item_blob` FROM `auction_custom` WHERE `auction_id`='%u' AND `status`='5'", auction_id)) {
				Sql_Query(sql_handle, "ROLLBACK");
				ack(AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL, currency, 0);
				return;
			}
			struct item it;
			memset(&it, 0, sizeof(it));
			bool have_item = false;
			if (SQL_SUCCESS == Sql_NextRow(sql_handle)) {
				size_t len = 0;
				char* blob = nullptr;
				Sql_GetData(sql_handle, 0, &blob, &len);
				if (blob != nullptr && len >= AUCTION_ITEM_BLOB_SIZE) {
					memcpy(&it, blob, AUCTION_ITEM_BLOB_SIZE);
					have_item = true;
				}
			}
			Sql_FreeResult(sql_handle);
			if (!have_item || !mail_item(char_id, seller, it, "拍卖行下架退回")) {
				Sql_Query(sql_handle, "ROLLBACK");
				ack(AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL, currency, 0);
				return;
			}
			if (SQL_ERROR == Sql_Query(sql_handle, "COMMIT")) {
				ack(AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL, currency, 0);
				return;
			}
			ack(AUCTION_RESULT_OK, AUCTION_REASON_OK, currency, 0);
			break;
		}
		case AUC_OP_BUY_NOW: {
			// Quote phase: answer the listing truth without state changes. The
			// map asks for `amount` units (0 = the whole lot); the answer is
			// that many units' share of the lot price, so a partial purchase is
			// never charged the whole stack.
			if (SQL_ERROR == Sql_Query(sql_handle, "SELECT `status`,`currency`,`price_buynow`,`seller_char_id`,`amount`,`expire_time` FROM `auction_custom` WHERE `auction_id`='%u'", auction_id)) {
				ack(AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL, currency, 0);
				return;
			}
			if (SQL_SUCCESS != Sql_NextRow(sql_handle)) {
				Sql_FreeResult(sql_handle);
				ack(AUCTION_RESULT_FAIL, AUCTION_REASON_ITEM_NOT_FOUND, currency, 0);
				return;
			}
			auto col = [&](int32 i) { char* d = nullptr; size_t l = 0; Sql_GetData(sql_handle, i, &d, &l); return d != nullptr ? d : "0"; };
			const uint8 row_status = static_cast<uint8>(strtoul(col(0), nullptr, 10));
			const uint8 row_currency = static_cast<uint8>(strtoul(col(1), nullptr, 10));
			const uint32 buynow = strtoul(col(2), nullptr, 10);
			const uint32 seller_id = strtoul(col(3), nullptr, 10);
			const uint16 listed = static_cast<uint16>(strtoul(col(4), nullptr, 10));
			const uint32 expire_time = strtoul(col(5), nullptr, 10);
			Sql_FreeResult(sql_handle);
			// Expired lots are settled by the scan within 30s; until then they
			// must not quote as purchasable (audit P2-2).
			if (row_status != AUCTION_ST_ACTIVE || expire_time <= static_cast<uint32>(time(nullptr))) {
				ack(AUCTION_RESULT_FAIL, AUCTION_REASON_ALREADY_ENDED, row_currency, buynow);
				return;
			}
			// Buying your own listing would charge your wallet, pay the
			// proceeds back to you minus the fee and mail the item to
			// yourself: a guaranteed loss and a pointless mail pair. The bid
			// path has always rejected this; buy-now has to as well. Rejected
			// here, before the map deducts anything.
			if (seller_id == char_id) {
				ack(AUCTION_RESULT_FAIL, AUCTION_REASON_OWN_LISTING, row_currency, buynow);
				return;
			}
			if (buynow == 0 || listed == 0) {
				ack(AUCTION_RESULT_FAIL, AUCTION_REASON_INVALID_PRICE, row_currency, buynow);
				return;
			}
			const uint16 want = amount == 0 ? listed : amount;
			if (want > listed) {
				ack(AUCTION_RESULT_FAIL, AUCTION_REASON_NOT_ENOUGH_ITEMS, row_currency, buynow);
				return;
			}
			ack(AUCTION_RESULT_OK, AUCTION_REASON_OK, row_currency, lot_quote(buynow, listed, want));
			break;
		}
		case AUC_OP_BUY_NOW_CONFIRM: {
			// Atomic buy-now: the whole lot flips to SOLD, a partial purchase
			// leaves the unpaid units listed. Mails and the payout ride in the
			// same transaction; the wallet deduction already happened on the map
			// (quote price for the same count).
			char buyer_name[NAME_LENGTH];
			if (!char_name(char_id, buyer_name, NAME_LENGTH)) {
				ack(AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL, currency, price);
				return;
			}
			if (SQL_ERROR == Sql_Query(sql_handle, "START TRANSACTION")) {
				ack(AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL, currency, price);
				return;
			}
			bool ok = SQL_ERROR != Sql_Query(sql_handle, "SELECT `currency`,`fee_permille`,`top_bidder_char_id`,`top_bidder_name`,`price_now`,`seller_char_id`,`seller_name`,`item_blob`,`price_buynow`,`amount` FROM `auction_custom` WHERE `auction_id`='%u' AND `status`='1' AND `expire_time`>UNIX_TIMESTAMP() FOR UPDATE", auction_id);
			struct { uint8 currency; uint16 fee_permille; uint32 top_bidder_char_id; char top_bidder_name[NAME_LENGTH]; uint32 price_now; uint32 seller_char_id; char seller_name[NAME_LENGTH]; struct item it; uint32 price_buynow; uint16 amount; } row{};
			memset(&row, 0, sizeof(row));
			if (!ok) {
				// No lockable ACTIVE, unexpired row: the lot ended (or sold)
				// between the quote and the confirm. Distinct from the currency
				// check below so a stale page reads honestly (audit P2-2).
				Sql_Query(sql_handle, "ROLLBACK");
				ack(AUCTION_RESULT_FAIL, AUCTION_REASON_ALREADY_ENDED, currency, price);
				return;
			}
			if (SQL_SUCCESS == Sql_NextRow(sql_handle)) {
				auto col = [&](int32 i) { char* d = nullptr; size_t l = 0; Sql_GetData(sql_handle, i, &d, &l); return d != nullptr ? d : "0"; };
				row.currency = static_cast<uint8>(strtoul(col(0), nullptr, 10));
				row.fee_permille = static_cast<uint16>(strtoul(col(1), nullptr, 10));
				row.top_bidder_char_id = strtoul(col(2), nullptr, 10);
				safestrncpy(row.top_bidder_name, col(3), NAME_LENGTH);
				row.price_now = strtoul(col(4), nullptr, 10);
				row.seller_char_id = strtoul(col(5), nullptr, 10);
				safestrncpy(row.seller_name, col(6), NAME_LENGTH);
				size_t len = 0;
				char* blob = nullptr;
				Sql_GetData(sql_handle, 7, &blob, &len);
				if (blob != nullptr && len >= AUCTION_ITEM_BLOB_SIZE)
					memcpy(&row.it, blob, AUCTION_ITEM_BLOB_SIZE);
				row.price_buynow = strtoul(col(8), nullptr, 10);
				row.amount = static_cast<uint16>(strtoul(col(9), nullptr, 10));
			} else {
				ok = false;
			}
			Sql_FreeResult(sql_handle);
			if (!ok || row.currency != currency || price == 0) {
				Sql_Query(sql_handle, "ROLLBACK");
				ack(AUCTION_RESULT_FAIL, ok ? AUCTION_REASON_CURRENCY_MISMATCH : AUCTION_REASON_ALREADY_ENDED, ok ? row.currency : currency, 0);
				return;
			}
			// A listing is not a player's to buy: reject the seller here too,
			// under the same lock the settlement uses, so a quote that slipped
			// through (or a replayed confirm) still cannot pay yourself.
			if (row.seller_char_id == char_id) {
				Sql_Query(sql_handle, "ROLLBACK");
				ack(AUCTION_RESULT_FAIL, AUCTION_REASON_OWN_LISTING, currency, price);
				return;
			}
			// The listing's own count is the truth: the amount column and the
			// packed blob are written together, so either being empty means a
			// corrupt row, and asking for more than is left means a stale page.
			const uint16 listed = row.it.amount;
			const uint16 want = amount == 0 ? listed : amount;
			if (listed == 0 || row.amount == 0 || want == 0 || want > listed || want > row.amount) {
				Sql_Query(sql_handle, "ROLLBACK");
				ack(AUCTION_RESULT_FAIL, AUCTION_REASON_NOT_ENOUGH_ITEMS, currency, row.price_buynow);
				return;
			}
			// The price comes from the map's quote, but the listing owns the
			// truth: only this many units' share of the lot price may settle.
			const uint32 quote = lot_quote(row.price_buynow, listed, want);
			if (price != quote) {
				Sql_Query(sql_handle, "ROLLBACK");
				ack(AUCTION_RESULT_FAIL, AUCTION_REASON_INVALID_PRICE, currency, quote);
				return;
			}
			const bool whole_lot = want >= listed;
			char escaped_buyer[(NAME_LENGTH) * 2 + 1];
			Sql_EscapeString(sql_handle, escaped_buyer, buyer_name);
			if (whole_lot) {
				if (SQL_ERROR == Sql_Query(sql_handle, "UPDATE `auction_custom` SET `status`='2', `buyer_char_id`='%u', `buyer_name`='%s' WHERE `auction_id`='%u' AND `status`='1'", char_id, escaped_buyer, auction_id)) {
					Sql_Query(sql_handle, "ROLLBACK");
					ack(AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL, currency, price);
					return;
				}
				if (Sql_NumRowsAffected(sql_handle) == 0) {
					Sql_Query(sql_handle, "ROLLBACK");
					ack(AUCTION_RESULT_FAIL, AUCTION_REASON_ALREADY_SOLD, currency, price);
					return;
				}
			} else {
				// The lot lives on with the units nobody paid for. A standing bid
				// was cast for the whole lot and cannot survive the split, so the
				// row drops it (the bidder is refunded below) and the remaining
				// units return to the fixed price they were listed at.
				struct item rest = row.it;
				rest.amount = static_cast<int16>(listed - want);
				const uint32 keep_price = row.price_buynow - quote;
				if (!update_auction_remainder(auction_id, static_cast<uint16>(listed - want), keep_price, keep_price, rest)) {
					Sql_Query(sql_handle, "ROLLBACK");
					ack(AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL, currency, price);
					return;
				}
			}
			const uint64 fee = fee_of(price, row.fee_permille);
			const uint64 seller_gain = price > fee ? price - fee : 0;
			// Deliver exactly the units that were paid for: a partial sale must
			// never mail the whole escrowed stack.
			struct item delivered = row.it;
			delivered.amount = static_cast<int16>(want);
			if (!mail_item(char_id, buyer_name, delivered, "拍卖行一口价成交")) {
				Sql_Query(sql_handle, "ROLLBACK");
				ack(AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL, currency, price);
				return;
			}
			if (row.currency == AUCTION_CURRENCY_ZENY) {
				if (!mail_zeny(row.seller_char_id, row.seller_name, zeny_mail_amount(seller_gain), "拍卖行成交款")) {
					Sql_Query(sql_handle, "ROLLBACK");
					ack(AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL, currency, price);
					return;
				}
			}
			// A standing top bidder loses the lot to the buy-now: refund the
			// whole bid, never a share of it — the bid was cast for a lot that
			// no longer exists, so a prorated refund would be money lost.
			const bool refund_bidder = row.top_bidder_char_id != 0;
			if (refund_bidder && row.currency == AUCTION_CURRENCY_ZENY) {
				if (!mail_zeny(row.top_bidder_char_id, row.top_bidder_name, zeny_mail_amount(row.price_now), "拍卖行出价被超越")) {
					Sql_Query(sql_handle, "ROLLBACK");
					ack(AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL, currency, price);
					return;
				}
			}
			if (SQL_ERROR == Sql_Query(sql_handle, "COMMIT")) {
				ack(AUCTION_RESULT_FAIL, AUCTION_REASON_INTERNAL, currency, price);
				return;
			}
			// Post-commit cash payout + notifications; amounts capped at the
			// wallet ceiling for legacy rows priced above INT_MAX (audit P2-4).
			if (row.currency == AUCTION_CURRENCY_CASH)
				cash_payout(row.seller_char_id, auction_id, static_cast<uint32>(std::min<uint64>(seller_gain, MAX_CASHPOINT)));
			if (refund_bidder && row.currency == AUCTION_CURRENCY_CASH)
				cash_payout(row.top_bidder_char_id, auction_id, static_cast<uint32>(std::min<uint64>(row.price_now, MAX_CASHPOINT)));
			if (refund_bidder)
				notify_char(row.top_bidder_char_id, AUCTION_NOTIFY_OUTBID, AUCTION_REASON_OK, auction_id, price);
			notify_char(row.seller_char_id, AUCTION_NOTIFY_SOLD, AUCTION_REASON_OK, auction_id, price);
			ack(AUCTION_RESULT_OK, AUCTION_REASON_OK, currency, price);
			break;
		}
		case AUC_OP_SETTLE_DUE: {
			// Char-side scan + per-row atomic settlement (ADR A4).
			if (SQL_ERROR == Sql_Query(sql_handle, "SELECT `auction_id` FROM `auction_custom` WHERE `status`='1' AND `expire_time`<=UNIX_TIMESTAMP()")) {
				Sql_FreeResult(sql_handle);
				return;
			}
			std::vector<uint32> due;
			while (SQL_SUCCESS == Sql_NextRow(sql_handle)) {
				char* data = nullptr; size_t len = 0;
				Sql_GetData(sql_handle, 0, &data, &len);
				if (data != nullptr)
					due.push_back(strtoul(data, nullptr, 10));
			}
			Sql_FreeResult(sql_handle);
			for (uint32 id : due)
				settle_one(id);
			break;
		}
		case AUC_OP_DETAIL: {
			if (SQL_ERROR == Sql_Query(sql_handle, "SELECT `price_now`,`price_buynow`,`expire_time`,`top_bidder_char_id`,`top_bidder_name`,`currency`,`blob_version`,`item_blob` FROM `auction_custom` WHERE `auction_id`='%u'", auction_id)) {
				return;
			}
			if (SQL_SUCCESS != Sql_NextRow(sql_handle)) {
				Sql_FreeResult(sql_handle);
				return;
			}
			auto col = [&](int32 i) { char* d = nullptr; size_t l = 0; Sql_GetData(sql_handle, i, &d, &l); return d != nullptr ? d : "0"; };
			// 0x38B2 kind 1 = [kind][char_id][ZC_AUCTION_DETAIL body]. The map
			// server memcpy's the tail into the ZC frame unchanged and the
			// client parses auctionId first, so the tail must be the packet
			// body byte for byte. It used to start at priceNow and be one
			// byte short: the map's length check rejected every reply, which is
			// why right-clicking a row never opened the detail panel.
			uint8 body[1 + 4 + AUCTION_ZC_DETAIL_BODY_SIZE];
			memset(body, 0, sizeof(body));
			body[0] = 1; // kind 1 detail
			memcpy(body + 1, &char_id, 4);
			uint8* d = body + 5;
			memcpy(d, &auction_id, 4); d += 4; // auctionId (client matches on it)
			uint32 v = strtoul(col(0), nullptr, 10);
			memcpy(d, &v, 4); d += 4; // price_now
			v = strtoul(col(1), nullptr, 10);
			memcpy(d, &v, 4); d += 4; // price_buynow
			v = strtoul(col(2), nullptr, 10);
			memcpy(d, &v, 4); d += 4; // expire
			v = strtoul(col(3), nullptr, 10);
			memcpy(d, &v, 4); d += 4; // top_bidder_id
			char top[NAME_LENGTH];
			safestrncpy(top, col(4), NAME_LENGTH);
			memcpy(d, top, 24); d += 24;
			uint16 blob_version = static_cast<uint16>(strtoul(col(6), nullptr, 10));
			memcpy(d, &blob_version, 2); d += 2;
			*d++ = static_cast<uint8>(strtoul(col(5), nullptr, 10)); // currency
			size_t len = 0;
			char* blob = nullptr;
			Sql_GetData(sql_handle, 7, &blob, &len);
			if (blob != nullptr && len >= AUCTION_ITEM_BLOB_SIZE)
				memcpy(d, blob, AUCTION_ITEM_BLOB_SIZE);
			Sql_FreeResult(sql_handle);
			reply_ack(fd, 0x38B2, body, sizeof(body));
			break;
		}
		case AUC_OP_ESCROW_ABORT: {
			// Unknown/aborted escrow: the reconcile-safe outcome is a plain
			// delete while the row is still ESCROWING (item returns via the
			// inventory reload; nothing was charged yet).
			Sql_Query(sql_handle, "DELETE FROM `auction_custom` WHERE `auction_id`='%u' AND `status`='0'", auction_id);
			break;
		}
		default:
			break;
	}
}

// Reconcile -------------------------------------------------------------------

// 0x30BC LISTING_COUNTS_REQ (6, fixed): cmd.W char_id.L
// 0x38BC LISTING_COUNTS (variable, len.W@2): char_id.L@4 listings.L@8 bids.L@12
// The open handshake's count round trip (audit P3): my_listings follows the
// same rule as the escrow cap (status IN (0,1)), my_bids counts the standing
// top bids. On SQL failure the counts go out as zero instead of stalling the
// handshake.
void parse_listing_counts(int32 fd) {
	const uint32 char_id = RFIFOL(fd, 2);
	uint32 listings = 0;
	uint32 bids = 0;
	if (SQL_ERROR != Sql_Query(sql_handle,
		"SELECT"
		" (SELECT COUNT(*) FROM `auction_custom` WHERE `seller_char_id`='%u' AND `status` IN (0,1)) AS listings,"
		" (SELECT COUNT(*) FROM `auction_custom` WHERE `top_bidder_char_id`='%u' AND `status`='1') AS bids",
		char_id, char_id)) {
		if (SQL_SUCCESS == Sql_NextRow(sql_handle)) {
			char* data = nullptr; size_t len = 0;
			Sql_GetData(sql_handle, 0, &data, &len);
			listings = data != nullptr ? strtoul(data, nullptr, 10) : 0;
			Sql_GetData(sql_handle, 1, &data, &len);
			bids = data != nullptr ? strtoul(data, nullptr, 10) : 0;
		}
	}
	Sql_FreeResult(sql_handle);
	uint8 body[12];
	memcpy(body, &char_id, 4);
	memcpy(body + 4, &listings, 4);
	memcpy(body + 8, &bids, 4);
	reply_ack(fd, 0x38BC, body, 12);
}

// Resolve a probe whose map never answered (or answered "not alive"): named
// items are checked against the inventory table, which answers exactly
// whether the player still holds them; non-named items take the delete safe
// default (the item comes back with the inventory reload, A6).
void resolve_dead_escrow(const ProbeRow& row) {
	if (row.unique_id != 0) {
		// Named item: the inventory table answers exactly whether the player
		// still holds it (crash before delitem) or not (crash after save).
		if (SQL_ERROR == Sql_Query(sql_handle, "SELECT COUNT(*) FROM `inventory` WHERE `char_id`='%u' AND `unique_id`='%llu'", row.seller_char_id, static_cast<unsigned long long>(row.unique_id))) {
			Sql_FreeResult(sql_handle);
			return;
		}
		uint64 found = 0;
		if (SQL_SUCCESS == Sql_NextRow(sql_handle)) {
			char* data = nullptr; size_t len = 0;
			Sql_GetData(sql_handle, 0, &data, &len);
			found = data != nullptr ? strtoull(data, nullptr, 10) : 0;
		}
		Sql_FreeResult(sql_handle);
		if (found != 0) {
			Sql_Query(sql_handle, "DELETE FROM `auction_custom` WHERE `auction_id`='%u' AND `status`='0'", row.auction_id);
			return;
		}
		Sql_Query(sql_handle, "UPDATE `auction_custom` SET `status`='3' WHERE `auction_id`='%u' AND `status`='0'", row.auction_id);
		return;
	}
	// Non-named items cannot be matched reliably: the safe default is to
	// delete the row (item comes back with the inventory reload; the player
	// simply re-lists). Never auto-mail these (duplication risk, A6).
	Sql_Query(sql_handle, "DELETE FROM `auction_custom` WHERE `auction_id`='%u' AND `status`='0'", row.auction_id);
}

// Periodic pass: (0) probe entries past their timeout resolve as dead; (1)
// RETURN_PENDING rows get their return mail and close; (2) stale ESCROWING
// rows are probed (txid liveness) and either deleted (item still with the
// player / non-named safe default) or queued for the return mail (named item
// proven gone from the inventory, A6).
TIMER_FUNC(reconcile_timer){
	// (0) A probe that stayed unanswered for the reclaim timeout is resolved
	// exactly like a "not alive" verdict: a live map answers within seconds,
	// so silence means the packet or the map was lost. Without this the entry
	// sat forever (and with it the ESCROWING row, holding a board slot —
	// audit P1-3).
	const time_t probe_now = time(nullptr);
	for (auto it = reconcile_probes.begin(); it != reconcile_probes.end();) {
		if (it->second.sent + AUCTION_ESCROW_RECLAIM_TIMEOUT > probe_now) {
			++it;
			continue;
		}
		const ProbeRow row = it->second;
		reconcile_probes.erase(it++);
		resolve_dead_escrow(row);
	}
	// (1) RETURN_PENDING -> mail + CLOSED, one transaction per row so a crash
	// between the mail insert and the status flip cannot re-send the item on
	// the next pass (audit P0-3).
	if (SQL_ERROR == Sql_Query(sql_handle, "SELECT `auction_id`,`seller_char_id`,`seller_name`,`item_blob` FROM `auction_custom` WHERE `status`='3'")) {
		Sql_FreeResult(sql_handle);
	} else {
		while (SQL_SUCCESS == Sql_NextRow(sql_handle)) {
			auto col = [&](int32 i) { char* d = nullptr; size_t l = 0; Sql_GetData(sql_handle, i, &d, &l); return d != nullptr ? d : "0"; };
			const uint32 auction_id = strtoul(col(0), nullptr, 10);
			const uint32 seller_char_id = strtoul(col(1), nullptr, 10);
			char seller[NAME_LENGTH];
			safestrncpy(seller, col(2), NAME_LENGTH);
			size_t len = 0;
			char* blob = nullptr;
			Sql_GetData(sql_handle, 3, &blob, &len);
			struct item it;
			memset(&it, 0, sizeof(it));
			if (blob != nullptr && len >= AUCTION_ITEM_BLOB_SIZE)
				memcpy(&it, blob, AUCTION_ITEM_BLOB_SIZE);
			char_name(seller_char_id, seller, NAME_LENGTH);
			if (SQL_ERROR == Sql_Query(sql_handle, "START TRANSACTION"))
				continue;
			if (mail_item(seller_char_id, seller, it, "拍卖行退回")
				&& SQL_ERROR != Sql_Query(sql_handle, "UPDATE `auction_custom` SET `status`='4' WHERE `auction_id`='%u' AND `status`='3'", auction_id)
				&& Sql_NumRowsAffected(sql_handle) != 0
				&& SQL_ERROR != Sql_Query(sql_handle, "COMMIT")) {
				notify_char(seller_char_id, AUCTION_NOTIFY_RETURNED, AUCTION_REASON_OK, auction_id, 0);
			} else {
				Sql_Query(sql_handle, "ROLLBACK");
			}
		}
		Sql_FreeResult(sql_handle);
	}
	// (2) Stale ESCROWING rows -> probe the owning map server (txid liveness).
	// The creation time travels in the txid's high bits (time << 20 | counter),
	// so `escrow_txid DIV 1048576` is the wall-clock second the row was made.
	// The old condition compared expire_time (= creation + duration, at least
	// +12h) against now+3000 and was therefore never true before expiry,
	// which stranded every stuck ESCROWING row on the board cap forever
	// (audit P1-3).
	const uint32 cutoff = static_cast<uint32>(time(nullptr)) - AUCTION_ESCROW_RECLAIM_TIMEOUT;
	if (SQL_ERROR == Sql_Query(sql_handle, "SELECT `auction_id`,`escrow_txid`,`seller_char_id`,`unique_id` FROM `auction_custom` WHERE `status`='0' AND (`escrow_txid` DIV 1048576) <= '%u'", cutoff)) {
		Sql_FreeResult(sql_handle);
		return 0;
	}
	while (SQL_SUCCESS == Sql_NextRow(sql_handle)) {
		auto col = [&](int32 i) { char* d = nullptr; size_t l = 0; Sql_GetData(sql_handle, i, &d, &l); return d != nullptr ? d : "0"; };
		ProbeRow row{};
		row.auction_id = strtoul(col(0), nullptr, 10);
		const uint64 txid = strtoull(col(1), nullptr, 10);
		row.seller_char_id = strtoul(col(2), nullptr, 10);
		row.account_id = account_id_of(row.seller_char_id);
		row.unique_id = strtoull(col(3), nullptr, 10);
		row.sent = time(nullptr);
		if (reconcile_probes.count(txid) != 0)
			continue;
		const int32 mapfd = mapfd_for_char(row.seller_char_id);
		// Probe regardless of the seller's own presence: the map that owns
		// the pending escrow state is the one this map server list holds. A
		// seller with no map link queues the row for the probe timeout above
		// instead of parking it forever (audit P1-3).
		if (mapfd < 0) {
			reconcile_probes[txid] = row;
			continue;
		}
		uint8 body[8];
		memcpy(body, &txid, 8);
		reply_ack(mapfd, 0x38B7, body, 8);
		reconcile_probes[txid] = row;
	}
	Sql_FreeResult(sql_handle);
	return 0;
}

void parse_probe_reply(int32 fd) {
	const uint64 txid = RFIFOQ(fd, 2);
	const uint8 alive = RFIFOB(fd, 10);
	auto it = reconcile_probes.find(txid);
	if (it == reconcile_probes.end())
		return;
	ProbeRow row = it->second;
	reconcile_probes.erase(it);
	if (alive) {
		return; // still in flight on a live map server: leave it alone
	}
	resolve_dead_escrow(row);
}

void parse_cash_reply(int32 fd) {
	const uint32 char_id = RFIFOL(fd, 2);
	const uint32 auction_id = RFIFOL(fd, 6);
	const uint8 result = RFIFOB(fd, 10);
	// The verdict names its lot: a stale reply for another auction must not
	// consume a pending handoff (audit P0-4).
	payout_resolve(char_id, auction_id, result == 1);
}

TIMER_FUNC(payout_reap_timer){
	const time_t now = time(nullptr);
	for (auto it = pending_payouts.begin(); it != pending_payouts.end();) {
		// Handoffs that never got a verdict fall back to the offline write;
		// the entry stays with its reaped marker so a late verdict cannot
		// trigger a second write (audit P0-4).
		if (!it->second.reaped && it->second.sent + AUCTION_PAYOUT_REPLY_TIMEOUT < now) {
			const uint32 reap_char_id = static_cast<uint32>((it->first >> 32) & 0xFFFFFFFFu);
			payout_resolve(reap_char_id, it->second.auction_id, false);
		}
		// Reaped entries linger only for the late-verdict grace window.
		if (it->second.reaped && it->second.sent + 600 < now)
			it = pending_payouts.erase(it);
		else
			++it;
	}
}

TIMER_FUNC(reconcile_timer_entry){
	reconcile_timer(tid, tick, id, data);
	return 0;
}
} // namespace

int32 inter_auction_custom_parse_frommap(int32 fd) {
	switch (RFIFOW(fd, 0)) {
		case 0x30B2: parse_search(fd); return 1;
		case 0x30B3: parse_escrow(fd); return 1;
		case 0x30B4: parse_commit(fd); return 1;
		case 0x30B5: parse_bid(fd); return 1;
		case 0x30B7: parse_probe_reply(fd); return 1;
		case 0x30B8: parse_op(fd); return 1;
		case 0x30B9: parse_cash_reply(fd); return 1;
		case 0x30BA: parse_escrow_batch(fd); return 1;
		case 0x30BB: parse_commit_batch(fd); return 1;
		case 0x30BC: parse_listing_counts(fd); return 1;
		default: return 0;
	}
}

void int_auction_custom_init(void) {
	add_timer_func_list(reconcile_timer_entry, "auction_reconcile_timer");
	add_timer_func_list(payout_reap_timer, "auction_payout_reap_timer");
	reconcile_timer_id = add_timer_interval(gettick() + AUCTION_RECONCILE_FIRST_DELAY, reconcile_timer_entry, 0, 0, AUCTION_RECONCILE_INTERVAL);
	add_timer_interval(gettick() + AUCTION_RECONCILE_FIRST_DELAY, payout_reap_timer, 0, 0, AUCTION_PAYOUT_REAP_INTERVAL);
}

void int_auction_custom_final(void) {
	if (reconcile_timer_id != INVALID_TIMER) {
		delete_timer(reconcile_timer_id, reconcile_timer_entry);
		reconcile_timer_id = INVALID_TIMER;
	}
	reconcile_probes.clear();
	pending_payouts.clear();
}
// ===== 拍卖行系统：结束 =====
