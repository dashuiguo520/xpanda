"""Check the local auction protocol after updating rAthena or the client.

Usage: py tools/python/check_auction_protocol.py <nostalro-client-directory>

Mirrors check_autoplay_protocol.py: packet-ID collision scan, capability
gates, enable-switch default, and the dual-end length cross-check between
the server static_asserts and the client length tables. Outgoing-whitelist
coverage is audited separately by the zc-gap-audit/ scripts in the client
repository.
"""
import re
import sys
from pathlib import Path

server = Path(__file__).resolve().parents[2]
client = Path(sys.argv[1]).resolve()
custom = server / "src/custom/auction_packets.hpp"
headers = custom.read_text(encoding="utf-8")
ids = {
    name: int(value, 16)
    for name, value in re.findall(
        r"DEFINE_PACKET_HEADER\((\w+), (0x[0-9a-f]+)\)", headers
    )
}
# v2 added CZ 0x7E29 (atomic batch) and ZC 0x7E37 (batch receipt).
assert set(ids.values()) == set(range(0x7E20, 0x7E2A)) | set(range(0x7E30, 0x7E38)), ids

# Upstream declarations must not reuse our project-local client IDs.
for path in (server / "src").rglob("*.hpp"):
    if path == custom:
        continue
    source = path.read_text(encoding="utf-8", errors="replace")
    for value in re.findall(
        r"DEFINE_PACKET_(?:HEADER|ID)\([^,\n]+,\s*(0x[0-9a-fA-F]+)", source
    ):
        assert int(value, 16) not in ids.values(), f"ID collision: {path}: {value}"

# The inter range (0x30B2-B9 / 0x38B2-B9) belongs to the auction code only;
# a hit anywhere else means an upstream or custom clash that needs a rerange.
inter_files = set()
for path in (server / "src").rglob("*.cpp"):
    source = path.read_text(encoding="utf-8", errors="replace")
    if re.search(r"0x38?[Bb][2-9]\b", source):
        inter_files.add(path.name)
assert inter_files == {"int_auction_custom.cpp", "auction_custom.cpp", "intif.cpp"}, inter_files

# Capability gate: only the open handler touches the handshake; it flips the
# session bit on a magic/version match. Every other handler goes through the
# shared gate (handshake bit -> enable switch -> PC_PERM_TRADE). The enable
# switch must default to off on both the battle_config row and the conf file.
clif = (server / "src/map/clif.cpp").read_text(encoding="utf-8")
session = (server / "src/map/pc.hpp").read_text(encoding="utf-8")
assert "bool auction_supported = false;" in session
handlers = re.findall(r"void (clif_parse_auction_\w+)\(", clif)
assert len(handlers) == 10 and len(set(handlers)) == 10, handlers


def body_at(source, needle):
    start = source.find(needle)
    assert start >= 0, needle
    end = source.find("\n}", start)
    return source[start:end]


open_body = body_at(clif, "void clif_parse_auction_open")
assert "RFIFOL(fd, 2) != AUCTION_PROTOCOL_MAGIC" in open_body
assert "RFIFOW(fd, 6) != AUCTION_PROTOCOL_VERSION" in open_body
assert open_body.count("auction_supported = true;") == 1
gate_body = body_at(clif, "bool clif_auction_gate(")
for needle in (
    "auction_supported",
    "battle_config.auction_enable",
    "pc_has_permission(sd, PC_PERM_TRADE)",
):
    assert needle in gate_body, f"Gate check missing: {needle}"
for name in handlers:
    if name == "clif_parse_auction_open":
        continue
    assert "clif_auction_gate(sd," in body_at(clif, f"void {name}("), f"Missing gate: {name}"
battle = (server / "src/map/battle.cpp").read_text(encoding="utf-8")
assert re.search(
    r'\{ "auction_enable",\s*&battle_config\.auction_enable,\s*0,\s*0,\s*1,', battle
)
conf = (server / "conf/battle/auction.conf").read_text(encoding="utf-8")
assert re.search(r"^auction_enable:\s*0", conf, re.M)
common = (server / "src/common/auction.hpp").read_text(encoding="utf-8")
assert "AUCTION_PROTOCOL_MAGIC 0x54435541" in common  # "AUCT" on the wire
assert "AUCTION_PROTOCOL_VERSION 2" in common  # v2 = atomic batch support
assert "AUCTION_MAX_BATCH_ITEMS 20" in common
assert "AUCTION_OP_REGISTER_BATCH = 10" in common
assert "AUCTION_REASON_BATCH_ROLLED_BACK = 17" in common
assert re.search(r"MAX_PACKET_DB\s*=\s*0x7E3F", (server / "src/map/clif.hpp").read_text(encoding="utf-8"))
ddl = (server / "sql-files/auction_custom.sql").read_text(encoding="utf-8", errors="replace")
assert "CREATE TABLE" in ddl and "auction_custom" in ddl and "auction_bid_journal" in ddl

# Client: request lengths must match the server static_asserts byte for byte,
# every CZ id needs a builder, fixed ZC responses need length-table rows, and
# the three variable pages are matched by row-size arms in the parser instead.
protocol = (client / "lib/network/src/protocol.rs").read_text(encoding="utf-8")
sender = (client / "lib/network/src/sender.rs").read_text(encoding="utf-8")
variable_zc = {0x7E31, 0x7E34, 0x7E35}
for name, value in ids.items():
    size = re.search(rf"sizeof\(PACKET_{name}\) == (\d+)", headers)
    if size is None:  # the variable pages share PACKET_ZC_AUCTION_PAGE_HEADER
        continue
    size = size.group(1)
    if name.startswith("CZ_"):
        assert re.search(rf"0x{value:04x} => {size},", protocol), f"Client request length mismatch: {name}"
        assert f"0x{value:04x}u16.to_le_bytes()" in sender, f"Client builder missing: {name}"
    elif value not in variable_zc:
        assert re.search(rf"0x{value:04x} => {size},", protocol), f"Client ZC length mismatch: {name}"
for value, row in ((0x7E31, 45), (0x7E34, 46), (0x7E35, 46)):
    assert re.search(
        rf"0x{value:04x} if frame\.len\(\) >= 9 && \(frame\.len\(\) - 9\) % {row} == 0", protocol
    ), f"Client variable-page arm missing: {value:04x}"
assert 'bytes.extend_from_slice(b"AUCT");' in sender
assert 'bytes.extend(2u16.to_le_bytes());' in sender, "client must handshake v2"

# v2 batch packets are variable-length: no fixed row in either length table,
# but both sides must have an explicit variable-length arm (outgoing early
# return, incoming len.W reader, decode arm and whitelist entry).
for needle in (
    "if id == 0x7e29 {",
    "0x7e37 => {",
    "0x7e37 if frame.len() >= 8 && (frame.len() - 8) % 7 == 0",
    "| 0x7e37",
):
    assert needle in protocol, f"Client v2 batch wire arm missing: {needle}"
assert "0x7e29u16.to_le_bytes()" in sender, "CZ batch builder missing"
assert "build_auction_register_batch_packet" in sender

# Inter length tables: the map->char entries must land at the right indices in
# char's inter_recv_packet_length (rows are positional; a misaligned row
# silently breaks the whole map<->char stream) and must equal the builder's
# WFIFOSET size. Map's packet_len_table must keep the 0x38B2-B9 replies
# variable (-1) as the builders write len.W.
inter_src = (server / "src/char/inter.cpp").read_text(encoding="utf-8", errors="replace")
tbl = re.search(r"inter_recv_packet_length\[\] = \{(.*?)\};", inter_src, re.S).group(1)
tbl = re.sub(r"//[^\n]*", "", tbl)
elems = [e.strip() for e in tbl.split(",") if e.strip()]
assert len(elems) == 192, f"char inter length table drifted: {len(elems)} entries (want 192)"
assert "sizeof(s_autoplay_config)" in elems[0xB1], "autoplay cfg slot moved"
builders = {
    "intif_auction_search": 0xB2,
    "intif_auction_escrow": 0xB3,
    "intif_auction_commit": 0xB4,
    "intif_auction_bid": 0xB5,
    "intif_auction_probe_reply": 0xB7,
    "intif_auction_op": 0xB8,
    "intif_auction_cash_reply": 0xB9,
}
for fn, slot in builders.items():
    src = (server / "src/map/intif.cpp").read_text(encoding="utf-8", errors="replace")
    m = re.search(rf"bool {fn}\(.*?WFIFOSET\(inter_fd,\s*(\d+)\)", src, re.S)
    assert m, f"WFIFOSET not found: {fn}"
    size = int(m.group(1))
    assert elems[slot] == str(size), (
        f"inter length table slot 0x30{slot:02X} = {elems[slot]!r} but {fn} sends {size}"
    )
assert elems[0xB6] == "0", "0x30B6 is unused and must stay 0"
# v2 batch inter frames are variable (len.W at offset 2) on both directions.
assert elems[0xBA] == "-1" and elems[0xBB] == "-1", "0x30BA/BB must be variable"
mapintif = (server / "src/map/intif.cpp").read_text(encoding="utf-8", errors="replace")
mtbl = re.search(r"packet_len_table\[\] = \{(.*?)\};", mapintif, re.S).group(1)
mtbl = re.sub(r"//[^\n]*", "", mtbl)
melems = [e.strip() for e in mtbl.split(",") if e.strip()]
for slot in range(0xB2, 0xBC):
    assert melems[slot] == "-1", f"map reply table slot 0x38{slot:02X} must be -1 (variable), got {melems[slot]!r}"
# Every char->map reply identifies the player by char_id. map_id2sd() looks up
# pc_db by account_id, so using it here returns nullptr and silently drops the
# receipt (client times out, escrow rows stay at status 0). Guard the fix.
auction_map_src = (server / "src/map/auction_custom.cpp").read_text(encoding="utf-8", errors="replace")
auction_map_code = "\n".join(
    line for line in auction_map_src.splitlines() if not line.lstrip().startswith("//")
)
assert not re.search(r"\bmap_id2sd\(", auction_map_code), (
    "auction_custom.cpp must resolve char_id with map_charid2sd(), not map_id2sd()"
)
assert auction_map_code.count("map_charid2sd(") >= 9, "char->map session lookups went missing"

# 0x30BA header offsets must agree on both sides. The v2 draft wrote char_id.L@4
# and then batch_id.Q@6, so the batch id's low word overwrote char_id's high
# half (150002 -> 18930); the char server failed the seller lookup and answered
# INTERNAL, rolling the whole batch back. Compare the offsets field by field
# instead of trusting the comment.
assert re.search(r"#define AUCTION_INTER_BATCH_HDR_SIZE 25\b", common), "0x30BA header size drifted"
escrow_batch_tx = body_at(mapintif, "bool intif_auction_escrow_batch(")
map_batch_offs = [int(v) for v in re.findall(r"WFIFO[WBLQ]\(inter_fd,\s*(\d+)\)", escrow_batch_tx)][2:]
assert map_batch_offs == [4, 8, 12, 16, 17, 18, 19, 21, 23, 24], map_batch_offs
assert "WFIFOP(inter_fd, AUCTION_INTER_BATCH_HDR_SIZE)" in escrow_batch_tx
assert "AUCTION_INTER_BATCH_HDR_SIZE + count * AUCTION_INTER_BATCH_ROW_SIZE" in escrow_batch_tx
char_auction_src = (server / "src/char/int_auction_custom.cpp").read_text(encoding="utf-8", errors="replace")
char_batch_offs = [
    int(v) for v in re.findall(r"RFIFO[WBLQ]\(fd,\s*(\d+)\)", body_at(char_auction_src, "void parse_escrow_batch("))
]
assert char_batch_offs == [4, 8, 16, 17, 18, 19, 21, 23], char_batch_offs
assert "AUCTION_INTER_BATCH_HDR_SIZE + i * AUCTION_INTER_BATCH_ROW_SIZE" in char_auction_src

# CZ inventory indices are *client* indices (server slot + 2, see clif.cpp
# client_index/server_index); the auction module works on server slots. Both
# register handlers must convert, otherwise the escrow (and the later
# pc_delitem) lands two rows off: the auction shows a different item than the
# one dragged and the dragged stack never leaves the bag.
batch_parse = body_at(clif, "void clif_parse_auction_register_batch(")
assert "row.index = server_index(RFIFOW(fd, at));" in batch_parse, (
    "batch register must convert the CZ client index to a server slot"
)
single_parse = body_at(clif, "void clif_parse_auction_register(")
assert "server_index(RFIFOW(fd, 2))" in single_parse, (
    "single register must convert the CZ client index to a server slot"
)

# Browse type tabs. CZ_AUCTION_SEARCH carries one `type` byte: real item types
# 0..12, 0xFF = any, and 0xFE = the consumable *group*. The char server must
# expand that sentinel before the equality filter; a plain `type='254'` matches
# no row and the tab looks unimplemented.
assert "AUCTION_TYPE_CONSUMABLE = 0xFE" in common, "consumable type sentinel drifted"
char_search = body_at(char_auction_src, "void parse_search(")
assert "type == AUCTION_TYPE_CONSUMABLE" in char_search, (
    "char search must special-case the consumable group before the equality filter"
)
assert "`type` IN (0,2,10)" in char_search, "consumable group lost its item types"

# The batch escrow ack deletes every staged stack. `pc_delitem` compacts the
# inventory when a stack is emptied, so deleting in row order shifts the later
# rows up: the batch then removes the wrong items (the auction shows X while
# the bag lost Y). Descending server-slot order is the only safe order.
batch_ack = body_at(auction_map_src, "void on_escrow_batch_ack(")
assert "std::sort(deletions.begin(), deletions.end()" in batch_ack, (
    "batch escrow ack must delete inventory slots in a deterministic order"
)
assert "a.index > b.index" in batch_ack, (
    "batch escrow ack must delete from the highest slot down (inventory compaction)"
)
# `pc_delitem` returns 0 on success and 1 on failure. The negated form logged
# every successful deletion as an error and stayed silent on real ones, which
# sent the "server has errors" investigation down the wrong path for a day.
assert "pc_delitem(sd, deletion.index, deletion.amount, 0, 1, LOG_TYPE_TRADE) != 0" in batch_ack, (
    "pc_delitem returns 0 on success; the batch must test `!= 0`"
)
single_ack = body_at(auction_map_src, "void on_escrow_ack(")
assert "pc_delitem(sd, pending.index, pending.amount, 0, 1, LOG_TYPE_TRADE) != 0" in single_ack, (
    "single escrow ack must report pc_delitem failures with the same 0=success rule"
)

# Detail reply. The char server answers 0x38B2 kind 1 with
# [kind][char_id][ZC_AUCTION_DETAIL body]; the map server memcpy's that tail
# straight into the ZC frame after its packetType, and the client parses
# auctionId first. The tail used to omit auctionId and be one byte short, so the
# map's length check dropped every reply: right-clicking a row did nothing.
common_hpp = (server / "src/common/auction.hpp").read_text(encoding="utf-8")
assert "#define AUCTION_ZC_DETAIL_BODY_SIZE 124" in common_hpp, (
    "the inter detail body size must be one shared constant"
)
assert "AUCTION_ZC_DETAIL_BODY_SIZE == sizeof(PACKET_ZC_AUCTION_DETAIL) - 2" in headers, (
    "the detail body constant must be pinned to the ZC struct by a static_assert"
)
char_detail = body_at(char_auction_src, "case AUC_OP_DETAIL:")
assert "uint8 body[1 + 4 + AUCTION_ZC_DETAIL_BODY_SIZE]" in char_detail, (
    "the char detail reply must carry exactly the ZC detail body"
)
assert re.search(r"uint8\* d = body \+ 5;\s*\n\s*memcpy\(d, &auction_id, 4\)", char_detail), (
    "the detail payload must start with auctionId; the client keys on it"
)
assert "reply_ack(fd, 0x38B2, body, sizeof(body))" in char_detail
map_detail = body_at(auction_map_src, "void on_search_or_detail(")
assert "body_len != AUCTION_ZC_DETAIL_BODY_SIZE" in map_detail, (
    "the map must compare the detail tail against the shared size"
)
assert 'ShowError("auction detail: char reply body' in map_detail, (
    "a dropped detail reply must be logged, not silently discarded"
)

# ---- Outgoing mail text must be in the data codepage --------------------
# The UTF-8 literal "拍卖行" (E6 8B 8D E5 8D 96 E8 A1 8C) read as GBK leaves a
# dangling 0x8C, so MySQL rejected every auction mail with
#   Incorrect string value: '\x8C' for column 'send_name'
# and the rollback took the listing with it. Both mail helpers must translate
# their literals first, and the char-side insert must repair (and log) anything
# that is still not valid in the connection codepage.
assert "convert_utf8_file_content" in char_auction_src
for helper in ("bool mail_zeny(", "bool mail_item("):
    body = body_at(char_auction_src, helper)
    assert "data_text(" in body, f"{helper} must convert its literals to the data codepage"
assert not re.search(r'mail_sendmail\([^;\n]*"[^"\n]*[\u4e00-\u9fff][^"\n]*"', char_auction_src), (
    "a raw CJK literal must never reach mail_sendmail: convert it with data_text() first"
)
int_mail_src = (server / "src/char/int_mail.cpp").read_text(encoding="utf-8")
for field in ("send_name", "dest_name", "title", "body"):
    assert f"normalize_data_text(msg->{field}" in int_mail_src, (
        f"mail_savemessage must normalise the `{field}` field before binding"
    )
assert "BindParam(0, SQLDT_STRING, const_cast<char*>(send_name.c_str())" in int_mail_src, (
    "the insert must bind the normalised buffer, not the raw struct field"
)
assert "mail_repair_text" in int_mail_src, (
    "a repaired field must be logged and written back so the client copy matches the row"
)

# ---- A listing is not the seller's to buy or bid on ---------------------
# Buying your own listing charged the wallet, paid the proceeds back minus the
# fee and mailed the item to yourself: a guaranteed loss. The bid path has
# always rejected it; the buy-now path (quote and confirm) had no check at all,
# so both phases must reject the seller and say why.
assert "AUCTION_REASON_OWN_LISTING = 20" in common, (
    "self-purchase needs its own reason code, not a generic 无权限"
)
buy_quote = body_at(char_auction_src, "case AUC_OP_BUY_NOW:")
assert "`seller_char_id`" in buy_quote, "the buy-now quote must read the seller id"
assert "seller_id == char_id" in buy_quote, (
    "the buy-now quote must reject the seller before the map deducts anything"
)
buy_confirm = body_at(char_auction_src, "case AUC_OP_BUY_NOW_CONFIRM:")
assert "row.seller_char_id == char_id" in buy_confirm, (
    "the buy-now confirm must reject the seller under the settlement lock"
)
assert "price != quote" in buy_confirm, (
    "the confirm must settle at this many units' share of the fixed price, not the caller's number"
)
assert re.search(
    r"bidder_char_id == row\.seller_char_id\)\s*\n\s*reason = AUCTION_REASON_OWN_LISTING;",
    char_auction_src,
), "the bid path must use the same self-purchase reason"
client_events = (client / "client/src/events/mod.rs").read_text(encoding="utf-8")
assert "20 => \"不能对自己的挂单出价或购买\"" in client_events, (
    "the client must explain reason 20 instead of falling back to 未知原因"
)

# ---- Per-unit listing price + partial buy-now ---------------------------
# The listing panel now takes a UNIT price and multiplies it by the staged
# quantity, because the wire/DB carry the lot total the server settles. A
# stacked buy-now therefore asks how many units to take and must settle only
# that share: the quote is prorated, the confirm re-derives it under the lock,
# and the row is written back with the units nobody paid for.
assert "AUCTION_REASON_NOT_ENOUGH_ITEMS = 21" in common, (
    "a partial buy-now needs its own reason code"
)
assert "uint32 lot_quote(" in char_auction_src, "the prorated price helper is missing"
assert "bool update_auction_remainder(" in char_auction_src, (
    "a partial sale must write back the remaining units and prices"
)
op_body = body_at(char_auction_src, "void parse_op(")
assert "RFIFOW(fd, 16)" in op_body, "0x30B8 amount@16 must be read"
for needle in (
    "lot_quote(buynow, listed, want)",
    "AUCTION_REASON_NOT_ENOUGH_ITEMS",
):
    assert needle in op_body, f"the buy-now quote is missing {needle}"
for needle in (
    "lot_quote(row.price_buynow, listed, want)",
    "update_auction_remainder(auction_id",
    "delivered.amount = static_cast<int16>(want)",
    "if (price != quote)",
):
    assert needle in op_body, f"the buy-now confirm is missing {needle}"
# The inter request grew by the amount word; both ends must agree.
assert "#define AUCTION_INTER_BATCH_HDR_SIZE 25" in common
assert re.search(r"WFIFOW\(inter_fd, 16\) = amount;", mapintif), "0x30B8 amount@16 not written"
assert re.search(r"WFIFOSET\(inter_fd, 18\)", mapintif), "0x30B8 must be 18 bytes"
# Client side: unit price on input, lot total on the wire, quantity on buy.
client_auction = (client / "lib/ui-component/src/game/auction_window.rs").read_text(encoding="utf-8")
assert "fn unit_price(total: u32, amount: u16) -> u32" in client_auction
assert "checked_mul(u32::from(self.parsed_amount()))" in client_auction, (
    "the listing panel must send unit * qty as the lot total"
)
assert "RequestAuctionBuyNow { auction_id, amount }" in client_auction, (
    "the buy request must carry the unit count"
)
assert "struct BuyDialog" in client_auction, "the quantity box is missing"
client_sender = (client / "lib/network/src/sender.rs").read_text(encoding="utf-8")
assert "amount.max(1).to_le_bytes()" in body_at(client_sender, "pub fn build_auction_buy_now_packet("), (
    "the CZ builder must write the unit count"
)

print("PASS: auction IDs (CZ 0x7E20-29, ZC 0x7E30-37), upstream + inter collision scan,")
print("      handshake/gate wiring, enable default off, DDL present,")
print("      client request/ZC lengths, variable-page arms, builders, AUCT handshake,")
print("      inter length tables (char slots aligned to WFIFOSET, map replies variable),")
print("      v2 atomic batch packets + handshake version 2,")
print("      browse type tabs incl. the 0xFE consumable group,")
print("      descending-slot batch deletion with the pc_delitem 0=success rule,")
print("      auctionId-first detail body pinned to the ZC struct,")
print("      auction mail text in the data codepage + char-side repair/log guard")
