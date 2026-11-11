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
assert set(ids.values()) == set(range(0x7E20, 0x7E29)) | set(range(0x7E30, 0x7E37)), ids

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
assert len(handlers) == 9 and len(set(handlers)) == 9, handlers


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
assert "AUCTION_PROTOCOL_VERSION 1" in common
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
print("PASS: auction IDs (CZ 0x7E20-28, ZC 0x7E30-36), upstream + inter collision scan,")
print("      handshake/gate wiring, enable default off, DDL present,")
print("      client request/ZC lengths, variable-page arms, builders, AUCT handshake")
