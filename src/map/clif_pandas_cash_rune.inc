// ===== rAthena 移植 Pandas GoldPC、Cash Emotion 与符文石板客户端逻辑：开始 =====
// 修改内容：集中实现三项功能的客户端数据包解析与响应，避免扩大 clif.cpp 主文件差异。
void clif_parse_askreward_rune(int32 fd, map_session_data* sd) {
#if PACKETVER >= 20241001
	struct PACKET_CZ_RUNE_ASK_REWARD* p = (struct PACKET_CZ_RUNE_ASK_REWARD*)RFIFOP(fd, 0);

	std::tuple<e_runereward_result, uint8> result = rune_askreward(sd, p->tagID, p->runesetid, p->reward);
	//ShowError("2 clif_parse_askreward_rune tagID %d runesetid %d reward %d std::get<0>(result) %d std::get<1>(result) %d \n", p->tagID, p->runesetid, p->reward, std::get<0>(result), std::get<1>(result));

	clif_resultreward_rune(sd, std::get<0>(result), std::get<1>(result), p->tagID, p->runesetid);
#endif
}

void clif_resultreward_rune(map_session_data* sd, e_runereward_result result, uint8 reward, uint16 tagID, uint32 runesetid) {
#if PACKETVER >= 20241001
	struct PACKET_ZC_RUNE_RESULT_REWARD p = {};
	//ShowError("clif_resultreward_rune : tagId %d runesetid %d result %d reward %d \n", tagID, runesetid, result, reward);

	p.packetType = HEADER_ZC_RUNE_RESULT_REWARD;
	p.result = result;
	p.tagID = tagID;
	p.runesetid = runesetid;
	p.reward = reward;

	clif_send(&p, sizeof(p), sd, SELF);
#endif
}

void clif_onlogreward_rune(map_session_data* sd, uint16 tagID) {
#if PACKETVER >= 20241001
	if (!sd) return;
	if (sd->runeSets.empty()) return;
	if (sd->runeSets.size() <= 0) return;

	struct PACKET_ZC_REWARD_INFO_RUNE* p = (struct PACKET_ZC_REWARD_INFO_RUNE*)packet_buffer;

	p->packetType = HEADER_ZC_REWARD_INFO_RUNE;
	p->set_amount = 0;
	p->tagID = tagID;

	for (auto& set_data : sd->runeSets) {
		if (set_data.tagId == tagID) {
			struct PACKET_ZC_REWARD_INFO_RUNE_sub* setreward_rune = &p->setreward_rune[p->set_amount];
			setreward_rune->runesetid = static_cast<uint32>(set_data.setId);
			setreward_rune->reward = set_data.reward;
			p->set_amount++;
		}
	}

	p->packetLength = sizeof(struct PACKET_ZC_REWARD_INFO_RUNE) + (sizeof(struct PACKET_ZC_REWARD_INFO_RUNE_sub) * p->set_amount);
	p->unknown = 0;

	//ShowError("clif_setinfo_rune tagID %d \n", p->tagID);
	clif_send(p, p->packetLength, sd, SELF);
#endif
}
void clif_goldpc_info( map_session_data& sd ){
#if PACKETVER_MAIN_NUM >= 20140508 || PACKETVER_RE_NUM >= 20140508 || defined(PACKETVER_ZERO)
	if (!sd.vars_ok)
		return;
	// ===== rAthena 修复 GoldPC 客户端计时显示：开始 =====
	// 修改原因：显示进度必须使用可配置周期，不能固定按 3600 秒计算。
	const int32 client_max_seconds = battle_config.feature_goldpc_time;
	// ===== rAthena 修复 GoldPC 客户端计时显示：结束 =====
	if( battle_config.feature_goldpc_active ){
		struct PACKET_ZC_GOLDPCCAFE_POINT p = {};
		p.PacketType = HEADER_ZC_GOLDPCCAFE_POINT;

		if (map_getmapflag(sd.m, MF_GVG_TE_CASTLE) ||
			map_getmapflag(sd.m, MF_GVG_CASTLE) ||
			map_getmapflag(sd.m, MF_GVG_TE) ||
			map_getmapflag(sd.m, MF_GVG) ||
			map_getmapflag(sd.m, MF_BATTLEGROUND) ||
			map_getmapflag(sd.m, MF_PVP)
			)
			p.isActive = false;
		else
			p.isActive = true;

		if( battle_config.feature_goldpc_vip && pc_isvip( &sd ) ){
			p.mode = 2;
		}else{
			p.mode = 1;
		}
		p.point = (int32)pc_readparam( &sd, SP_GOLDPC_POINTS );
		if( sd.goldpc_tid != INVALID_TIMER ){
			const struct TimerData* td = get_timer( sd.goldpc_tid );
			if( td != nullptr ){
				// ===== rAthena 修复 GoldPC 剩余时间取整：开始 =====
				// 修改原因：按秒向上取整并限制范围，避免旧公式的取余误差和负值。
				const t_tick remaining = std::max<t_tick>(td->tick - gettick(), 0);
				p.playedTime = cap_value(client_max_seconds - static_cast<int32>((remaining + 999) / 1000), 0, client_max_seconds);
				// ===== rAthena 修复 GoldPC 剩余时间取整：结束 =====
			}else{
				p.playedTime = 0;
			}
		}else{
			p.playedTime = client_max_seconds;
		}
		clif_send( &p, sizeof( p ), &sd, SELF);
	}
#endif
}

void clif_parse_dynamic_npc( int fd, map_session_data* sd ){
#if PACKETVER_MAIN_NUM >= 20140430 || PACKETVER_RE_NUM >= 20140430 || defined(PACKETVER_ZERO)

	struct PACKET_CZ_DYNAMICNPC_CREATE_REQUEST* p = (struct PACKET_CZ_DYNAMICNPC_CREATE_REQUEST*)RFIFOP(fd, 0);
	char npcname[NPC_NAME_LENGTH + 1];
	if (strncasecmp("GOLDPCCAFE", p->name, sizeof(p->name)) == 0) {
		safestrncpy(npcname, p->name, sizeof(npcname));
	}
	else {
		return;
	}
	npc_data* nd = npc_name2id(npcname);
	if (nd == nullptr) {
		ShowError("clif_parse_dynamic_npc: Original NPC \"%s\" was not found.\n", npcname);
		clif_dynamicnpc_result(*sd, DYNAMICNPC_RESULT_UNKNOWNNPC);
		return;
	}

	// ===== rAthena 修正 Pandas GoldPC 点击处理：开始 =====
	// 修改原因：保持 Pandas 原始语义，点击按钮时触发 OnGoldPcClick，而不是直接复制 NPC。
	npc_script_event(*sd, NPCE_GOLDPC);
	clif_dynamicnpc_result(*sd, DYNAMICNPC_RESULT_SUCCESS);
	// ===== rAthena 修正 Pandas GoldPC 点击处理：结束 =====
#endif
}
void clif_parse_emotion2(const int32 fd, map_session_data* const sd)
{
#if (PACKETVER_MAIN_NUM >= 20230925)
	nullpo_retv(sd);

	const PACKET_CZ_REQ_EMOTION2* const Packet = reinterpret_cast<PACKET_CZ_REQ_EMOTION2*>(RFIFOP(fd, 0));

	pc_use_emotion(sd, Packet->ExpantionId, Packet->EmotionId);
#endif
}

void clif_emotion2(block_list* const bl, const uint16 ExpantionId, const uint16 EmotionId)
{
#if (PACKETVER_MAIN_NUM >= 20230925)
	nullpo_retv(bl);

	PACKET_ZC_EMOTION2 Packet = {};
	Packet.PacketType = HEADER_ZC_EMOTION2;
	Packet.GID = bl->id;
	Packet.ExpantionId = ExpantionId;
	Packet.EmotionId = EmotionId;

	clif_send(&Packet, sizeof(PACKET_ZC_EMOTION2), bl, AREA);
#endif
}

void clif_emotion2_fail(map_session_data* const sd, const uint16 ExpantionId, const uint16 EmotionId, const EEmotionStatus Status)
{
#if (PACKETVER_MAIN_NUM >= 20230925)
	nullpo_retv(sd);

	const int32 fd = sd->fd;

	WFIFOHEAD(fd, sizeof(PACKET_ZC_EMOTION2_FAIL));

	PACKET_ZC_EMOTION2_FAIL* const Packet = reinterpret_cast<PACKET_ZC_EMOTION2_FAIL*>(WFIFOP(fd, 0));
	Packet->PacketType = HEADER_ZC_EMOTION2_FAIL;
	Packet->ExpantionId = ExpantionId;
	Packet->EmotionId = EmotionId;
	Packet->Status = Status;

	WFIFOSET(fd, sizeof(PACKET_ZC_EMOTION2_FAIL));
#endif
}

void clif_parse_emotion2_expantion(const int32 fd, map_session_data* const sd)
{
#if (PACKETVER_MAIN_NUM >= 20230925)
	nullpo_retv(sd);

	const PACKET_CZ_REQ_EMOTION2_EXPANTION* const Packet = reinterpret_cast<PACKET_CZ_REQ_EMOTION2_EXPANTION*>(RFIFOP(fd, 0));

	pc_buy_emotion_expantion(sd, Packet->ExpantionId, Packet->ItemId, Packet->Amount);
#endif
}

void clif_emotion2_expantion(map_session_data* const sd, const uint16 ExpantionId, const bool bRented, const uint32 RentEndTime)
{
#if (PACKETVER_MAIN_NUM >= 20230925)
	nullpo_retv(sd);

	const int32 fd = sd->fd;

	WFIFOHEAD(fd, sizeof(PACKET_ZC_EMOTION2_EXPANTION));

	PACKET_ZC_EMOTION2_EXPANTION* const Packet = reinterpret_cast<PACKET_ZC_EMOTION2_EXPANTION*>(WFIFOP(fd, 0));
	Packet->PacketType = HEADER_ZC_EMOTION2_EXPANTION;
	Packet->ExpantionId = ExpantionId;
	Packet->bRented = bRented;
	Packet->Timestamp = RentEndTime;

	WFIFOSET(fd, sizeof(PACKET_ZC_EMOTION2_EXPANTION));
#endif
}

void clif_emotion2_expantion_fail(map_session_data* const sd, const uint16 ExpantionId, const EEmotionExpantionStatus Status)
{
#if (PACKETVER_MAIN_NUM >= 20230925)
	nullpo_retv(sd);

	const int fd = sd->fd;

	WFIFOHEAD(fd, sizeof(PACKET_ZC_EMOTION2_EXPANTION_FAIL));

	PACKET_ZC_EMOTION2_EXPANTION_FAIL* const Packet = reinterpret_cast<PACKET_ZC_EMOTION2_EXPANTION_FAIL*>(WFIFOP(fd, 0));
	Packet->PacketType = HEADER_ZC_EMOTION2_EXPANTION_FAIL;
	Packet->ExpantionId = ExpantionId;
	Packet->Status = Status;

	WFIFOSET(fd, sizeof(PACKET_ZC_EMOTION2_EXPANTION_FAIL));
#endif
}

void clif_emotion2_expantion_list(map_session_data* const sd, const std::vector<PACKET_ZC_EMOTION2_EXPANTION_LIST_SUB>& List)
{
#if (PACKETVER_MAIN_NUM >= 20230925)
	nullpo_retv(sd);

	const int fd = sd->fd;

	const size_t PacketTotalSize = sizeof(PACKET_ZC_EMOTION2_EXPANTION_LIST) + sizeof(PACKET_ZC_EMOTION2_EXPANTION_LIST_SUB) * List.size();
	WFIFOHEAD(fd, PacketTotalSize);

	PACKET_ZC_EMOTION2_EXPANTION_LIST* const Packet = reinterpret_cast<PACKET_ZC_EMOTION2_EXPANTION_LIST*>(WFIFOP(fd, 0));
	Packet->PacketType = HEADER_ZC_EMOTION2_EXPANTION_LIST;
	Packet->Timestamp = uint32(time(nullptr));
	Packet->Timezone = 540; // Seems to be 9 (Korean UTC) * 60

	for (size_t Num = 0; Num < List.size(); ++Num)
	{
		Packet->List[Num] = List[Num];
	}

	Packet->PacketLength = uint16(PacketTotalSize);

	WFIFOSET(fd, PacketTotalSize);
#endif
}
void clif_rune_ui_open( map_session_data* sd ){
#if PACKETVER >= 20230802
	struct PACKET_ZC_OPEN_RUNE_UI p = {};

	p.packetType = HEADER_ZC_OPEN_RUNE_UI;
	p.state = 1;

	clif_send( &p, sizeof( p ), sd, SELF );
#endif
}

void clif_parse_result_rune_ui_open( int32 fd, map_session_data* sd ){
#if PACKETVER >= 20230802
	struct PACKET_CZ_RESULT_OPEN_RUNE_UI* p = (struct PACKET_CZ_RESULT_OPEN_RUNE_UI*)RFIFOP( fd, 0 );

	//ShowError("clif_parse_result_rune_ui_open result ui open state %d\n",p->state);

	sd->state.runeui_open = p->state;
#endif
}

void clif_parse_asktag_rune( int32 fd, map_session_data* sd ){
#if PACKETVER >= 20230802
	struct PACKET_CZ_ASK_TAG_LOAD_RUNE* p = (struct PACKET_CZ_ASK_TAG_LOAD_RUNE*)RFIFOP( fd, 0 );

	//ShowError("clif_parse_asktab_rune tagID %d\n",p->tagID);

	clif_bookinfo_rune(sd, p->tagID);
	clif_setinfo_rune(sd, p->tagID);
#endif
}

void clif_bookinfo_rune( map_session_data* sd, uint16 tagID ){
#if PACKETVER >= 20230802
	struct PACKET_ZC_BOOK_INFO_RUNE* p = (struct PACKET_ZC_BOOK_INFO_RUNE*)packet_buffer;;

	p->packetType = HEADER_ZC_BOOK_INFO_RUNE;
	p->book_amount = 0;

	for (const s_runebook_data& runebook_data : sd->runeBooks) {
		if (runebook_data.tagId == tagID) {
			struct PACKET_BOOK_LIST_RUNE_sub* booklist_rune = &p->booklist_rune[p->book_amount];
			booklist_rune->runebookid = static_cast<uint32>(runebook_data.bookId);

			p->book_amount++;
		}
	}

	p->packetLength = sizeof( struct PACKET_ZC_BOOK_INFO_RUNE ) + ( sizeof( struct PACKET_BOOK_LIST_RUNE_sub ) * p->book_amount );
	p->unknown = 0;
	p->tagID = tagID;

	//ShowError("clif_setinfo_rune tagID %d \n", p->tagID);
	clif_send( p, p->packetLength, sd, SELF );
#endif
}

void clif_setinfo_rune( map_session_data* sd, uint16 tagID ){
#if PACKETVER >= 20230802
	struct PACKET_ZC_SET_INFO_RUNE* p = (struct PACKET_ZC_SET_INFO_RUNE*)packet_buffer;

	p->packetType = HEADER_ZC_SET_INFO_RUNE;
	p->set_amount = 0;

	for (const s_runeset_data& set_data : sd->runeSets) {
		if(set_data.tagId == tagID){
			struct PACKET_SET_LIST_RUNE_sub* setlist_rune = &p->setlist_rune[p->set_amount];
			setlist_rune->runesetid = static_cast<uint32>(set_data.setId);
			setlist_rune->upgrade = set_data.upgrade;
			setlist_rune->failcount = set_data.failcount;
			// ===== rAthena 修复符文套装数量封包：开始 =====
			// 修改原因：只有匹配当前 tagID 的套装被写入封包时才能增加数量。
			p->set_amount++;
			// ===== rAthena 修复符文套装数量封包：结束 =====
		}
	}

	// ===== rAthena 修复符文套装封包头类型：开始 =====
	// 修改原因：计算 SET_INFO 封包长度时应使用自身结构，避免两种封包结构后续变化时长度错误。
	p->packetLength = sizeof( struct PACKET_ZC_SET_INFO_RUNE ) + ( sizeof( struct PACKET_SET_LIST_RUNE_sub ) * p->set_amount );
	// ===== rAthena 修复符文套装封包头类型：结束 =====
	p->unknown = 0;
	p->tagID = tagID;

	//ShowError("clif_setinfo_rune tagID %d \n", p->tagID);
	clif_send( p, p->packetLength, sd, SELF );

	if(sd->runeactivated_data.tagID && !sd->runeactivated_data.loaded){
		clif_enablerefresh_rune2(sd,0,0);
		clif_enablerefresh_rune2(sd,sd->runeactivated_data.tagID,sd->runeactivated_data.runesetid);
		sd->runeactivated_data.loaded = true;
	}
#endif
}

void clif_parse_bookactivate_rune( int32 fd, map_session_data* sd ){
#if PACKETVER >= 20230802
	struct PACKET_CZ_BOOK_ACTIVATE_RUNE* p = (struct PACKET_CZ_BOOK_ACTIVATE_RUNE*)RFIFOP( fd, 0 );

	struct PACKET_ZC_BOOK_RESULT_RUNE pm = {};

	pm.packetType = HEADER_ZC_BOOK_RESULT_RUNE;
	pm.runebookid = p->runebookid;
	pm.tagID = p->tagID;
	pm.result = rune_bookactivate(sd,p->tagID,p->runebookid);

	//ShowError("clif_parse_bookactivate_rune tagID %d runebookid %d result %d\n",p->tagID, p->runebookid, pm.result);
	clif_send( &pm, sizeof( pm ), sd, SELF );

	if(sd->runeactivated_data.tagID && sd->runeactivated_data.tagID == pm.tagID){
		clif_enablerefresh_rune2(sd,0,0);
		clif_enablerefresh_rune2(sd,sd->runeactivated_data.tagID,sd->runeactivated_data.runesetid);
	}
#endif
}

void clif_parse_setactivate_rune( int32 fd, map_session_data* sd ){
#if PACKETVER >= 20230802
	struct PACKET_CZ_SET_ACTIVATE_RUNE* p = (struct PACKET_CZ_SET_ACTIVATE_RUNE*)RFIFOP( fd, 0 );

	//ShowError("clif_parse_setactivate_rune tagID %d runesetid %d \n",p->tagID, p->runesetid);

	clif_setactivate_rune(sd, p->tagID, p->runesetid);
#endif
}

void clif_setactivate_rune (map_session_data* sd, uint16 tagID, uint32 runesetid ){
#if PACKETVER >= 20230802
	struct PACKET_ZC_SET_RESULT_RUNE p = {};

	p.packetType = HEADER_ZC_SET_RESULT_RUNE;
	p.tagID = tagID;
	p.runesetid = runesetid;

	p.result = rune_setactivate(sd,tagID,runesetid);
	p.upgrade = 0;
	p.failcount = 0;

	//ShowError("clif_setactivate_rune result %d upgrade : %d failcount %d \n",p.result, p.upgrade, p.failcount);
	clif_send( &p, sizeof( p ), sd, SELF );
#endif
}

void clif_parse_setupgrade_rune( int32 fd, map_session_data* sd ){
#if PACKETVER >= 20230802
	struct PACKET_CZ_SET_UPGRADE_RUNE* p = (struct PACKET_CZ_SET_UPGRADE_RUNE*)RFIFOP( fd, 0 );
	//ShowError("clif_parse_setupgrade_rune tagID %d runesetid %d \n",p->tagID, p->runesetid);

	clif_setupgrade_rune(sd, p->tagID, p->runesetid);
#endif
}

void clif_setupgrade_rune (map_session_data* sd, uint16 tagID, uint32 runesetid ){
#if PACKETVER >= 20230802
	struct PACKET_ZC_SET_RESULT_RUNE2 p = {};

	p.packetType = HEADER_ZC_SET_RESULT_RUNE2;
	p.tagID = tagID;
	p.runesetid = runesetid;

	std::tuple<uint8, uint16, uint16>  rune_setupgrade_result = rune_setupgrade(sd, tagID, runesetid);
	p.result = std::get<0>(rune_setupgrade_result);
	p.upgrade = std::get<1>(rune_setupgrade_result);
	p.failcount = std::get<2>(rune_setupgrade_result);

	//ShowError("clif_setupgrade_rune result %d upgrade : %d failcount %d \n",p.result, p.upgrade, p.failcount);
	clif_send( &p, sizeof( p ), sd, SELF );

	if(sd->runeactivated_data.tagID && sd->runeactivated_data.tagID == tagID && sd->runeactivated_data.runesetid == runesetid){
		clif_enablerefresh_rune2(sd,0,0);
		clif_enablerefresh_rune2(sd,sd->runeactivated_data.tagID,sd->runeactivated_data.runesetid);
	}
#endif
}

void clif_parse_enable_rune( int32 fd, map_session_data* sd ){
#if PACKETVER >= 20230802
	struct PACKET_CZ_ENABLE_RUNE* p = (struct PACKET_CZ_ENABLE_RUNE*)RFIFOP( fd, 0 );

	//ShowError("clif_parse_enable_rune tagID %d runesetid %d \n",p->tagID, p->runesetid);

	clif_enablerefresh_rune(sd, p->tagID, p->runesetid);
#endif
}

void clif_enablerefresh_rune (map_session_data* sd, uint16 tagID, uint32 runesetid ){
#if PACKETVER >= 20230802
	struct PACKET_ZC_REFRESH_ENABLE_RUNE p = {};

	p.packetType = HEADER_ZC_REFRESH_ENABLE_RUNE;
	p.tagID = tagID;

	if(rune_changestate(sd, tagID, runesetid))
		p.runesetid = runesetid;
	else
		p.runesetid = 0;

	//ShowError("clif_enablerefresh_rune tagID %d runesetid %d \n",p.tagID, p.runesetid);
	clif_send( &p, sizeof( p ), sd, SELF );
#endif
}

void clif_enablerefresh_rune2 (map_session_data* sd, uint16 tagID, uint32 runesetid ){
#if PACKETVER >= 20230802
	struct PACKET_ZC_REFRESH_ENABLE_RUNE2 p = {};

	p.packetType = HEADER_ZC_REFRESH_ENABLE_RUNE2;
	p.tagID = tagID;
	p.runesetid = runesetid;

	//ShowError("clif_enablerefresh_rune2 tagID %d runesetid %d \n",p.tagID, p.runesetid);
	clif_send( &p, sizeof( p ), sd, SELF );
#endif
}

void clif_onlogenable_rune (map_session_data* sd){
#if PACKETVER >= 20230802
	//ShowError("clif_onlogenable_rune \n");
	struct PACKET_ZC_ONLOG_ENABLE_RUNE p = {};
	bool isSelectedSet = false;

   if(!sd) return;
   if(sd->runeSets.empty()) return;
   if(sd->runeSets.size() <= 0) return;

	for (auto& set_data : sd->runeSets) {
		if (&set_data == nullptr)
			continue;
		if(set_data.selected){
			isSelectedSet = true;
			p.tagID = sd->runeactivated_data.tagID = static_cast<uint16>(set_data.tagId);
			p.runesetid = sd->runeactivated_data.runesetid = static_cast<uint32>(set_data.setId);
			p.upgrade = sd->runeactivated_data.upgrade = static_cast<uint8>(set_data.upgrade);
			p.failcount = set_data.failcount;
			rune_count_bookactivated(sd, p.tagID, p.runesetid);

			break;
		}
		if(isSelectedSet)
			break;
	}

	if(!isSelectedSet)
		return;

	p.packetType = HEADER_ZC_ONLOG_ENABLE_RUNE;

	clif_send( &p, sizeof( p ), sd, SELF );

#if PACKETVER >= 20241001
	// load reward per tag
	for (auto& rune_data : rune_db)
		clif_onlogreward_rune(sd, rune_data.first);
#endif

#endif
}

void clif_parse_decompo_rune( int32 fd, map_session_data* sd ){
#if PACKETVER >= 20230802
	std::unordered_map<t_itemid, uint32> material_item_list;

	struct PACKET_CZ_RUNE_DECOMPO* p = (struct PACKET_CZ_RUNE_DECOMPO*)RFIFOP( fd, 0 );
	// ===== rAthena 修复符文分解类型校验：开始 =====
	// 修改原因：type 来自客户端，非 1/2 值不能默认按30个物品处理。
	if (p->type != 1 && p->type != 2) {
		clif_runedecompowindow_result(sd, ZC_RUNEDECOMPO_INVALID, material_item_list);
		return;
	}
	// ===== rAthena 修复符文分解类型校验：结束 =====

	if( !sd->state.runeui_open ){
		clif_runedecompowindow_result(sd, ZC_RUNEDECOMPO_UINOTOPENED, material_item_list);
		return;
	}

	uint16 index = server_index( p->index );

	if( index >= MAX_INVENTORY ){
		clif_runedecompowindow_result(sd, ZC_RUNEDECOMPO_INVENTORYSPACE, material_item_list);
		return;
	}

	if( sd->inventory_data[index] == nullptr ){
		clif_runedecompowindow_result(sd, ZC_RUNEDECOMPO_NOCARD, material_item_list);
		return;
	}

	struct item& selected_item = sd->inventory.u.items_inventory[index];

	if (selected_item.nameid != p->itemId) {
		clif_runedecompowindow_result(sd, ZC_RUNEDECOMPO_UNKNOWN, material_item_list);
		return;
	}

	struct item_data* id = itemdb_search(selected_item.nameid);

	// Checking if decompoRune is empty
	if (id->decompoRune.empty()) {
		clif_runedecompowindow_result(sd, ZC_RUNEDECOMPO_INVALID, material_item_list);
		return;
	}

	const uint32 amount = p->type == 1 ? 1 : 30;

	uint8 runeEntryit = 0;
	// ===== rAthena 修复符文分解配方与产物校验：开始 =====
	// 修改原因：未匹配配方时禁止消耗原料，并在扣除前校验产物重量、堆叠上限和背包槽位。
	bool matched_recipe = false;
	uint64 material_weight = 0;
	uint16 required_slots = 0;
	for (const auto& runeEntry : id->decompoRune) {
		runeEntryit++;

		//ShowError("runeEntryit %d runeEntry.Second %d p->type %d \n",runeEntryit,runeEntry.second, p->type);

		if(runeEntryit != p->type)
			continue;
		matched_recipe = true;

		std::shared_ptr<s_runedecompo_db> runedecompo_data = runedecompo_db.find( runeEntry.second );
		if(runedecompo_data == nullptr){
			//ShowError("runedecompo_data not found in db \n");
			clif_runedecompowindow_result(sd, ZC_RUNEDECOMPO_INVALID, material_item_list);
			return;
		}

		if( sd->inventory.u.items_inventory[index].amount < amount ){
			clif_runedecompowindow_result(sd, ZC_RUNEDECOMPO_NOTENOUGHITEMS, material_item_list);
			return;
		}

		//ShowError("runedecompo_data size %d \n",runedecompo_data->materials.size());
		for( const auto &materialEntry : runedecompo_data->materials ){
			struct s_runedecompo_material_data material_data = materialEntry.second;
			//ShowError("Material list itemid %d, amountmin %d amountmax %d \n",material_data.material,material_data.amountmin,material_data.amountmax);

			uint32 chance = material_data.chance;
			if( chance == 0 ){
				clif_runedecompowindow_result(sd, ZC_RUNEDECOMPO_INVALID, material_item_list);
				return;
			}
			if( chance < 100000 && rnd_value( 0, 100000 ) > chance )
				continue;

			uint32 random_amount = rnd_value( material_data.amountmin, material_data.amountmax );
			std::shared_ptr<item_data> item = item_db.find(material_data.material);
			if (item == nullptr || pc_checkadditem(sd, material_data.material, random_amount) == CHKADDITEM_OVERAMOUNT) {
				clif_runedecompowindow_result(sd, ZC_RUNEDECOMPO_INVALID, material_item_list);
				return;
			}
			material_weight += static_cast<uint64>(item->weight) * random_amount;
			if (pc_checkadditem(sd, material_data.material, random_amount) == CHKADDITEM_NEW)
				required_slots++;
			material_item_list[material_data.material] = random_amount;
			//ShowError("random_amount %d \n", random_amount);
		}

		const uint64 consumed_weight = static_cast<uint64>(id->weight) * amount;
		const uint64 final_weight = static_cast<uint64>(sd->weight) - std::min<uint64>(sd->weight, consumed_weight) + material_weight;
		if(final_weight > static_cast<uint64>(sd->max_weight)){
			clif_runedecompowindow_result(sd, ZC_RUNEDECOMPO_WEIGHT, material_item_list);
			return;
		}
		const uint16 available_slots = pc_inventoryblank(sd) + (selected_item.amount == amount ? 1 : 0);
		if (required_slots > available_slots) {
			clif_runedecompowindow_result(sd, ZC_RUNEDECOMPO_INVENTORYSPACE, material_item_list);
			return;
		}
	}

	if (!matched_recipe) {
		clif_runedecompowindow_result(sd, ZC_RUNEDECOMPO_INVALID, material_item_list);
		return;
	}
	// ===== rAthena 修复符文分解配方与产物校验：结束 =====

	// ===== rAthena 修复符文分解原子性：开始 =====
	// 修改原因：先确认并消耗原料，再发放已通过预检的产物，避免先发产物后扣除失败导致复制。
	if (pc_delitem(sd, index, amount, 0, 0, LOG_TYPE_OTHER) != 0){
		clif_runedecompowindow_result(sd, ZC_RUNEDECOMPO_INVALID, material_item_list);
		return;
	}
	for (const auto& materialEntry : material_item_list) {
		struct item item_material = {};
		item_material.nameid = materialEntry.first;
		item_material.identify = 1;
		if (pc_additem(sd, &item_material, materialEntry.second, LOG_TYPE_OTHER) != ADDITEM_SUCCESS)
			map_addflooritem(&item_material, materialEntry.second, sd->m, sd->x, sd->y, 0, 0, 0, 0, 0);
	}
	// ===== rAthena 修复符文分解原子性：结束 =====

	clif_runedecompowindow_result( sd, ZC_RUNEDECOMPO_SUCCESS, material_item_list );
#endif
}

void clif_runedecompowindow_result (map_session_data* sd, enum e_runedecompo_result result, std::unordered_map<t_itemid, uint32> material_item_list){
#if PACKETVER >= 20230802
	//ShowError("clif_runedecompowindow_result : %d \n", result);
	struct PACKET_ZC_RESULT_RUNE_DECOMPO p = {};

	p.packetType = HEADER_ZC_RESULT_RUNE_DECOMPO;
	p.result = result;

	uint8 material_number = 0;
	if(result == ZC_RUNEDECOMPO_SUCCESS){
		for (const auto& materialEntry : material_item_list) {
			if (material_number >= MAX_RUNEDECOMPO)
				break;
			p.itemlistdecompo_rune[material_number] = materialEntry.first;
			p.amountlistdecompo_rune[material_number] = materialEntry.second;

			material_number++;
		}
	}

	for(int32 i = material_number; i < MAX_RUNEDECOMPO; i++){
		p.itemlistdecompo_rune[i] = 0;
		p.amountlistdecompo_rune[i] = 0;
	}

	clif_send( &p, sizeof( p ), sd, SELF );
#endif
}
// ===== rAthena 移植 Pandas GoldPC、Cash Emotion 与符文石板客户端逻辑：结束 =====
