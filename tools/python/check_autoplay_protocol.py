"""Check the local autoplay protocol after updating rAthena or the client.

Usage: py tools/python/check_autoplay_protocol.py <nostalro-client-directory>
"""
import re
import sys
from pathlib import Path

server = Path(__file__).resolve().parents[2]
client = Path(sys.argv[1]).resolve()
custom = server / "src/custom/autoplay_packets.hpp"
headers = custom.read_text(encoding="utf-8")
ids = {
    name: int(value, 16)
    for name, value in re.findall(
        r"DEFINE_PACKET_HEADER\((\w+), (0x[0-9a-f]+)\)", headers
    )
}
assert set(ids.values()) == set(range(0x7E00, 0x7E09)), ids

# Upstream declarations must not reuse our project-local client IDs.
for path in (server / "src").rglob("*.hpp"):
    if path == custom:
        continue
    source = path.read_text(encoding="utf-8", errors="replace")
    for value in re.findall(
        r"DEFINE_PACKET_(?:HEADER|ID)\([^,\n]+,\s*(0x[0-9a-fA-F]+)", source
    ):
        assert int(value, 16) not in ids.values(), f"ID collision: {path}: {value}"

clif = (server / "src/map/clif.cpp").read_text(encoding="utf-8")
session = (server / "src/map/autoplay.hpp").read_text(encoding="utf-8")
assert "bool client_supported = false;" in session
for name in (
    "clif_autoplay_send_config", "clif_autoplay_send_result",
    "clif_parse_autoplay_status_get", "clif_autoplay_walk_step",
    "clif_parse_autoplay_config_save", "clif_parse_autoplay_control",
    "clif_parse_autoplay_walk_to",
):
    guard = (
        rf"void {name}\([^\n]*\) \{{\s*"
        r"if \([^\n]*!sd(?:\.|->)autoplay->client_supported\)\s*return;"
    )
    assert re.search(guard, clif), f"Missing capability guard: {name}"
assert clif.count("client_supported = true;") == 1
assert re.search(
    r"RFIFOL\(fd, 2\) != AUTOPLAY_PROTOCOL_MAGIC.*"
    r"RFIFOW\(fd, 6\) != AUTOPLAY_PROTOCOL_VERSION", clif
)
assert "0x594c5041" in headers and "AUTOPLAY_PROTOCOL_VERSION = 1;" in headers

protocol = (client / "lib/network/src/protocol_20250716.rs").read_text(encoding="utf-8")
sender = (client / "lib/network/src/sender.rs").read_text(encoding="utf-8")
for name, value in ids.items():
    size = re.search(rf"sizeof\(PACKET_{name}\) == (\d+)", headers).group(1)
    assert re.search(rf"0x{value:04x} => {size},", protocol), f"Client size mismatch: {name}"
    if name.startswith("CZ_"):
        assert f"0x{value:04x}u16.to_le_bytes()" in sender, name
assert 'bytes.extend_from_slice(b"APLY");' in sender
assert "bytes.extend(1u16.to_le_bytes());" in sender
# Negative regression tests deliberately mention the retired IDs.
runtime_sources = protocol.split("#[cfg(test)]", 1)[0] + sender.split("#[cfg(test)]", 1)[0]
assert not re.search(r"0x0c8[0-8]\b", runtime_sources, re.I)
print("PASS: custom IDs, upstream collision scan, capability gates, handshake and client lengths")
