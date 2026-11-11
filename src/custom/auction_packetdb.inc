static_assert(MAX_OFFICIAL_PACKET_DB < HEADER_CZ_AUCTION_OPEN,
	"Official packet range overlaps the project-local auction range");
static_assert(MAX_PACKET_DB >= HEADER_ZC_AUCTION_NOTIFY,
	"Packet database must cover the project-local auction range");
for (uint16 cmd = HEADER_CZ_AUCTION_OPEN; cmd <= HEADER_ZC_AUCTION_NOTIFY; ++cmd) {
	if (packet_db[cmd].len != 0) {
		ShowFatalError("Auction custom packet ID 0x%04x is already registered.\n", cmd);
		exit(EXIT_FAILURE);
	}
}
// ===== 拍卖行系统：开始 =====
parseable_packet(HEADER_CZ_AUCTION_OPEN, sizeof(PACKET_CZ_AUCTION_OPEN), clif_parse_auction_open, 0);
parseable_packet(HEADER_CZ_AUCTION_SEARCH, sizeof(PACKET_CZ_AUCTION_SEARCH), clif_parse_auction_search, 0);
parseable_packet(HEADER_CZ_AUCTION_DETAIL, sizeof(PACKET_CZ_AUCTION_DETAIL), clif_parse_auction_detail, 0);
parseable_packet(HEADER_CZ_AUCTION_REGISTER, sizeof(PACKET_CZ_AUCTION_REGISTER), clif_parse_auction_register, 0);
parseable_packet(HEADER_CZ_AUCTION_CANCEL, sizeof(PACKET_CZ_AUCTION_CANCEL), clif_parse_auction_cancel, 0);
parseable_packet(HEADER_CZ_AUCTION_BID, sizeof(PACKET_CZ_AUCTION_BID), clif_parse_auction_bid, 0);
parseable_packet(HEADER_CZ_AUCTION_BUY_NOW, sizeof(PACKET_CZ_AUCTION_BUY_NOW), clif_parse_auction_buy_now, 0);
parseable_packet(HEADER_CZ_AUCTION_MY_LISTINGS, sizeof(PACKET_CZ_AUCTION_MY_LISTINGS), clif_parse_auction_my_listings, 0);
parseable_packet(HEADER_CZ_AUCTION_MY_BIDS, sizeof(PACKET_CZ_AUCTION_MY_BIDS), clif_parse_auction_my_bids, 0);
packet(HEADER_ZC_AUCTION_OPEN_ACK, sizeof(PACKET_ZC_AUCTION_OPEN_ACK));
packet(HEADER_ZC_AUCTION_SEARCH_RESULT, -1);
packet(HEADER_ZC_AUCTION_DETAIL, sizeof(PACKET_ZC_AUCTION_DETAIL));
packet(HEADER_ZC_AUCTION_RESULT, sizeof(PACKET_ZC_AUCTION_RESULT));
packet(HEADER_ZC_AUCTION_MY_LISTINGS, -1);
packet(HEADER_ZC_AUCTION_MY_BIDS, -1);
packet(HEADER_ZC_AUCTION_NOTIFY, sizeof(PACKET_ZC_AUCTION_NOTIFY));
// ===== 拍卖行系统：结束 =====
