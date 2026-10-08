static_assert(MAX_OFFICIAL_PACKET_DB < HEADER_CZ_AUTOPLAY_CONFIG_GET,
    "Official packet range overlaps the project-local autoplay range");
static_assert(MAX_PACKET_DB >= HEADER_ZC_AUTOPLAY_WALK_STEP,
    "Packet database must cover the project-local autoplay range");
for (uint16 cmd = HEADER_CZ_AUTOPLAY_CONFIG_GET; cmd <= HEADER_ZC_AUTOPLAY_WALK_STEP; ++cmd) {
    if (packet_db[cmd].len != 0) {
        ShowFatalError("Autoplay custom packet ID 0x%04x is already registered.\n", cmd);
        exit(EXIT_FAILURE);
    }
}
// ===== 挂机系统：开始 =====
parseable_packet(HEADER_CZ_AUTOPLAY_CONFIG_GET, sizeof(PACKET_CZ_AUTOPLAY_CONFIG_GET), clif_parse_autoplay_config_get, 0);
parseable_packet(HEADER_CZ_AUTOPLAY_CONFIG_SAVE, sizeof(PACKET_CZ_AUTOPLAY_CONFIG_SAVE), clif_parse_autoplay_config_save, 0);
parseable_packet(HEADER_CZ_AUTOPLAY_CONTROL, sizeof(PACKET_CZ_AUTOPLAY_CONTROL), clif_parse_autoplay_control, 0);
parseable_packet(HEADER_CZ_AUTOPLAY_STATUS_GET, sizeof(PACKET_CZ_AUTOPLAY_STATUS_GET), clif_parse_autoplay_status_get, 0);
parseable_packet(HEADER_CZ_AUTOPLAY_WALK_TO, sizeof(PACKET_CZ_AUTOPLAY_WALK_TO), clif_parse_autoplay_walk_to, 0);
packet(HEADER_ZC_AUTOPLAY_CONFIG, sizeof(PACKET_ZC_AUTOPLAY_CONFIG));
packet(HEADER_ZC_AUTOPLAY_RESULT, sizeof(PACKET_ZC_AUTOPLAY_RESULT));
packet(HEADER_ZC_AUTOPLAY_STATUS, sizeof(PACKET_ZC_AUTOPLAY_STATUS));
packet(HEADER_ZC_AUTOPLAY_WALK_STEP, sizeof(PACKET_ZC_AUTOPLAY_WALK_STEP));
// ===== 挂机系统：结束 =====
