// ===== rAthena 移植 Pandas 脚本指令主体：开始 =====
// 修改内容：提供 pandas_script_commands.txt 中新增指令的实现；直接启用，不依赖 Pandas 宏开关。
static bool script_get_optnum(script_state* st, int loc, const char* desc, int& ret, bool allow_notexists = false, int defval = 0) {
	if (!script_hasdata(st, loc)) {
		if (allow_notexists) {
			ret = defval;
			return true;
		}
		ShowError("buildin_%s: parameter %d (%s) is required.\n", script_getfuncname(st), loc - 1, desc);
		return false;
	}
	if (!script_isint(st, loc)) {
		ShowError("buildin_%s: parameter %d (%s) must be an integer.\n", script_getfuncname(st), loc - 1, desc);
		return false;
	}
	ret = script_getnum(st, loc);
	return true;
}

TIMER_FUNC(selfdeletion_timer);

// 修改内容：补充新增指令共用的查询常量与变量写入助手，直接使用 rAthena 原生注册表。

static void script_both_setreg(script_state* st, const char* name, int64 value, bool array, int index = -1, int char_id = 0) {
	map_session_data* sd = char_id != 0 ? map_charid2sd(char_id) : map_id2sd(st->rid);
	std::string variable = "$@";
	variable += name;
	const int64 server_id = array ? reference_uid(add_str(variable.c_str()), index) : add_str(variable.c_str());
	mapreg_setreg(server_id, value);
	if (sd != nullptr) {
		variable = "@";
		variable += name;
		const int64 player_id = array ? reference_uid(add_str(variable.c_str()), index) : add_str(variable.c_str());
		pc_setreg(sd, player_id, value);
	}
}

static void script_both_setregstr(script_state* st, const char* name, const char* value, bool array, int index = -1, int char_id = 0) {
	map_session_data* sd = char_id != 0 ? map_charid2sd(char_id) : map_id2sd(st->rid);
	std::string variable = "$@";
	variable += name;
	const int64 server_id = array ? reference_uid(add_str(variable.c_str()), index) : add_str(variable.c_str());
	mapreg_setregstr(server_id, value);
	if (sd != nullptr) {
		variable = "@";
		variable += name;
		const int64 player_id = array ? reference_uid(add_str(variable.c_str()), index) : add_str(variable.c_str());
		pc_setregstr(sd, player_id, value);
	}
}

static bool script_get_array(script_state* st, int loc, int& varid, char*& varname, script_data*& vardata, bool expected_string = false) {
	if (!script_hasdata(st, loc))
		return false;
	vardata = script_getdata(st, loc);
	if (!data_isreference(vardata))
		return false;
	varid = reference_getid(vardata);
	varname = reference_getname(vardata);
	if (is_string_variable(varname) != expected_string)
		return false;
	map_session_data* sd = nullptr;
	if (not_server_variable(*varname) && !script_rid2sd(sd))
		return false;
	return script_array_src(st, sd, varname, reference_getref(vardata)) != nullptr;
}

static bool script_cleararray_st(script_state* st, int loc) {
	script_data* data = script_getdata(st, loc);
	if (!data_isreference(data))
		return false;
	map_session_data* sd = nullptr;
	const char* name = reference_getname(data);
	if (not_server_variable(*name) && !script_rid2sd(sd))
		return false;
	reg_db* source = script_array_src(st, sd, name, reference_getref(data));
	if (source == nullptr)
		return false;
	script_array* array = static_cast<script_array*>(idb_get(source->arrays, reference_getid(data)));
	if (array == nullptr)
		return true;
	uint32* keys = script_array_cpy_list(array);
	for (uint32 i = 0; i < array->size; ++i)
		clear_reg(st, sd, reference_uid(reference_getid(data), keys[i]), name, reference_getref(data));
	aFree(keys);
	return true;
}

static bool script_get_optstr(script_state* st, int loc, const char* desc, std::string& ret, bool allow_notexists = false, const std::string& defval = "") {
	if (!script_hasdata(st, loc)) {
		if (allow_notexists) {
			ret = defval;
			return true;
		}
		ShowError("buildin_%s: parameter %d (%s) is required.\n", script_getfuncname(st), loc - 1, desc);
		return false;
	}
	if (!script_isstring(st, loc)) {
		ShowError("buildin_%s: parameter %d (%s) must be a string.\n", script_getfuncname(st), loc - 1, desc);
		return false;
	}
	ret = script_getstr(st, loc);
	return true;
}

static bool script_get_mapindex(script_state* st, const char* mapname, int& map_id, int char_id = 0) {
	if (stricmp(mapname, "this") != 0) {
		map_id = map_mapname2mapid(mapname);
		return map_id >= 0;
	}
	map_session_data* sd = char_id != 0 ? map_charid2sd(char_id) : map_id2sd(st->rid);
	if (sd == nullptr)
		return false;
	map_id = sd->m;
	return map_id >= 0;
}

/* ===========================================================
 * 指令: setheaddir
 * 描述: 调整角色纸娃娃脑袋的朝向
 * 用法: setheaddir <朝向编号>{,<角色编号>};
 * 朝向: 0为看正前方, 1为向右看, 2为向左看
 * 返回: 该指令无论成功与否, 都不会有返回值
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(setheaddir) {
	map_session_data *sd = nullptr;
	int head_dir = script_getnum(st, 2);
	head_dir = cap_value(head_dir, 0, 2);

	if (!script_charid2sd(3, sd))
		return SCRIPT_CMD_SUCCESS;

	pc_setdir(sd, sd->ud.dir, head_dir);
	clif_changed_dir(*sd, AREA_WOS);
	clif_changed_dir(*sd, SELF);

	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: setbodydir
 * 描述: 用于调整角色纸娃娃身体的朝向
 * 用法: setbodydir <朝向编号>{,<角色编号>};
 * 返回: 该指令无论成功与否, 都不会有返回值
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(setbodydir) {
	map_session_data *sd = nullptr;
	int body_dir = script_getnum(st, 2);
	body_dir = cap_value(body_dir, 0, 7);

	if (!script_charid2sd(3, sd))
		return SCRIPT_CMD_SUCCESS;

	pc_setdir(sd, body_dir, sd->head_dir);
	clif_changed_dir(*sd, AREA_WOS);
	clif_changed_dir(*sd, SELF);

	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: instance_users
 * 描述: 获取指定的副本实例中已经进入副本地图的人数
 * 用法: instance_users <副本实例编号>;
 * 返回: 成功直接返回副本中的人数, 副本不存在或副本中无人存在则返回 0
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(instance_users) {
	struct instance_data *im = nullptr;
	int i = 0, users = 0, instance_id = 0;

	instance_id = script_getnum(st, 2);

	std::shared_ptr<s_instance_data> idata = util::umap_find(instances, instance_id);

	if (!idata || idata->state != INSTANCE_BUSY) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	for (const auto& it : idata->map)
		users += max(map_getmapdata(it.m)->users, 0);

	script_pushint(st, users);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: mobremove
 * 描述: 根据 GID 移除一个魔物单位 (只是移除, 不会让魔物死亡)
 * 用法: mobremove <魔物的GID>;
 * 返回: 该指令无论成功失败, 都不会有返回值
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(mobremove) {
	block_list *bl = nullptr;
	bl = map_id2bl(script_getnum(st, 2));

	if (!bl || bl->type != BL_MOB)
		return SCRIPT_CMD_SUCCESS;

	TBL_MOB* md = (TBL_MOB*)bl;

	if (!md->spawn)
		unit_free(bl, CLR_OUTSIGHT);
	else {
		unit_remove_map(bl, CLR_OUTSIGHT);
		if (!(md->sc.getSCE(SC_KAIZEL) || (md->sc.getSCE(SC_REBIRTH) && !md->state.rebirth)))
			mob_setdelayspawn(md);
		// rAthena 未启用 Pandas 战斗记录的魔物索引表，无需额外重建索引。
	}

	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: battleignore
 * 描述: 将角色设置为魔物免战状态, 避免被魔物攻击
 * 用法: battleignore <开关状态>{,<角色编号>};
 * 返回: 该指令无论成功失败, 都不会有返回值
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(battleignore) {
	map_session_data *sd = nullptr;
	int immune = script_getnum(st, 2);

	if (!script_charid2sd(3, sd))
		return SCRIPT_CMD_SUCCESS;

	if (cap_value(immune, 0, 1))
		sd->state.block_action |= PCBLOCK_IMMUNE;
	else
		sd->state.block_action &= ~PCBLOCK_IMMUNE;

	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: gethotkey
 * 描述: 获取指定快捷键位置当前的信息
 * 用法: gethotkey <快捷键位置编号>{,<要获取的数据类型>};
 * 返回: 若携带 <要获取的数据类型> 参数时, 发生错误将返回 -1, 成功则返回查询的值;
		不携带 <要获取的数据类型> 参数时, 发生错误将返回 -1, 成功则将信息保存到变量并返回 1
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(gethotkey) {
	map_session_data *sd = nullptr;

	if (!script_rid2sd(sd)) {
		script_pushint(st, -1);
		return SCRIPT_CMD_SUCCESS;
	}

	int hotkey_idx = script_getnum(st, 2);
	if (hotkey_idx < 0 || hotkey_idx > MAX_HOTKEYS_DB) {
		ShowError("buildin_gethotkey: hotkey index %d is out of range (0..%d).\n", hotkey_idx, MAX_HOTKEYS_DB);
		script_pushint(st, -1);
		return SCRIPT_CMD_SUCCESS;
	}

	if (script_hasdata(st, 3)) {
		if (!script_isint(st, 3)) {
			ShowError("buildin_gethotkey: request date type must be a integer value.\n");
			script_pushint(st, -1);
			return SCRIPT_CMD_SUCCESS;
		}

		switch (script_getnum(st, 3))
		{
		case 0:
			script_pushint(st, sd->status.hotkeys[hotkey_idx].type); break;
		case 1:
			script_pushint(st, sd->status.hotkeys[hotkey_idx].id); break;
		case 2:
			script_pushint(st, sd->status.hotkeys[hotkey_idx].lv); break;
		default:
			ShowError("buildin_gethotkey: request date type %d is out of range (0..2).\n", script_getnum(st, 3));
			script_pushint(st, -1);
		}
		return SCRIPT_CMD_SUCCESS;
	}

	pc_setreg(sd, add_str("@hotkey_type"), (int)sd->status.hotkeys[hotkey_idx].type);
	pc_setreg(sd, add_str("@hotkey_id"), (int)sd->status.hotkeys[hotkey_idx].id);
	pc_setreg(sd, add_str("@hotkey_lv"), (int)sd->status.hotkeys[hotkey_idx].lv);

	script_pushint(st, 1);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: sethotkey
 * 描述: 设置指定快捷键位置的信息
 * 用法: sethotkey <快捷键位置编号>,<快捷键的类型>,<物品/技能的ID>,<技能等级>;
 * 返回: 设置成功则返回 1, 设置失败则返回 0
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(sethotkey) {
	map_session_data *sd = nullptr;

	if (!script_rid2sd(sd)) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	int hotkey_idx = script_getnum(st, 2);
	if (hotkey_idx < 0 || hotkey_idx > MAX_HOTKEYS_DB) {
		ShowError("buildin_sethotkey: hotkey index %d is out of range (0..%d).\n", hotkey_idx, MAX_HOTKEYS_DB);
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	int hotkey_type = script_getnum(st, 3);
	if (hotkey_type < 0 || hotkey_type > 1) {
		ShowError("buildin_sethotkey: hotkey type %d is out of range (0..1).\n", hotkey_type);
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	int hotkey_id = script_getnum(st, 4);
	if (hotkey_type == 0) {	// 物品
		if (!item_db.exists(hotkey_id)) {
			ShowError("buildin_sethotkey: Nonexistant item %d requested.\n", hotkey_id);
			script_pushint(st, 0);
			return SCRIPT_CMD_SUCCESS;
		}
	}
	else {	// 技能
		if (!skill_get_index(hotkey_id)) {
			ShowError("buildin_sethotkey: Invalid skill ID %d , please review.\n", hotkey_id);
			script_pushint(st, 0);
			return SCRIPT_CMD_SUCCESS;
		}
	}

	int hotkey_lv = script_getnum(st, 5);
	if (hotkey_type == 0) {	// 物品
		hotkey_lv = 0;
	}

	sd->status.hotkeys[hotkey_idx].type = hotkey_type;
	sd->status.hotkeys[hotkey_idx].id = hotkey_id;
	sd->status.hotkeys[hotkey_idx].lv = hotkey_lv;

	clif_hotkeys_send(sd, 0);

#if PACKETVER_MAIN_NUM >= 20190522 || PACKETVER_RE_NUM >= 20190508 || PACKETVER_ZERO_NUM >= 20190605
	clif_hotkeys_send(sd, 1);
#endif

	clif_inventorylist(sd);

	script_pushint(st, 1);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: showvend
 * 描述: 使指定的 NPC 头上可以显示露天商店的招牌
 * 用法: showvend "<NPC名称>",<是否显示>{,"<招牌名称>"};
 * 返回: 操作成功则返回 1, 操作失败则返回 0
 * 作者: Jian916, Rewrite By Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(showvend) {
	npc_data *nd = nullptr;
	const char *npcname = nullptr, *message = nullptr;
	char buf[NAME_LENGTH + 1] = { 0 };
	int showit = 0;

	npcname = script_getstr(st, 2);
	showit = script_getnum(st, 3);

	if (showit && !script_hasdata(st, 4)) {
		ShowError("buildin_showvend: Can't create vendingboard without any message.\n");
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}
	else if (showit && !script_isstring(st, 4)) {
		ShowError("buildin_showvend: The 'message' param must be a string.\n");
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}
	else if (showit) {
		message = script_getstr(st, 4);
	}

	nd = npc_name2id(npcname);
	if (nd == nullptr) {
		ShowError("buildin_showvend: No such NPC '%s'.\n", npcname);
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	switch (showit)
	{
	case 0:
		clif_closevendingboard(*nd, AREA_WOS, nullptr);
		memset(nd->vendingboard.message, 0, NAME_LENGTH + 1);
		nd->vendingboard.show = false;
		break;
	default:
		memcpy(buf, message, NAME_LENGTH);
		clif_showvendingboard(nd, (const char*)buf);
		nd->vendingboard.show = true;
		memcpy(nd->vendingboard.message, buf, NAME_LENGTH + 1);
		break;
	}

	script_pushint(st, 1);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: viewequip
 * 描述: 查看指定在线角色的装备面板信息
 * 用法: viewequip <目标的角色编号|目标的账号编号>{,<是否强制查看>};
 * 返回: 操作成功则返回 1, 操作失败则返回 0
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(viewequip) {
	map_session_data *sd = nullptr;
	int cid = script_getnum(st, 2), force = 0;
	map_session_data *tsd = map_charid2sd(cid);

	if (!tsd || !script_rid2sd(sd)){
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	if (script_hasdata(st, 3)) {
		if (!script_isint(st, 3)) {
			ShowError("buildin_showvend: The 'force' param must be a integer.\n");
			script_pushint(st, 0);
			return SCRIPT_CMD_SUCCESS;
		}
		force = cap_value(script_getnum(st, 3), 0, 1);
	}

	if(tsd->status.show_equip || pc_has_permission(sd, PC_PERM_VIEW_EQUIPMENT) || force == 1){
		clif_viewequip_ack(*sd, *tsd);
		script_pushint(st, 1);
	}
	else{
		clif_msg(*sd, MSI_OPEN_EQUIPEDITEM_REFUSED);
		script_pushint(st, 0);
	}

	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: countitemidx
 * 描述: 获取指定背包序号的道具在背包中的数量
 * 用法: countitemidx <背包序号>{,<角色编号>};
 * 返回: 操作成功则返回道具的数量, 操作失败则返回 0
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(countitemidx) {
	map_session_data *sd = nullptr;
	int idx = -1;

	if (!script_charid2sd(3, sd)) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	idx = script_getnum(st, 2);
	if (idx < 0 || idx >= sd->inventory.max_amount) {
		ShowWarning("buildin_countitemidx: Index (%d) should be from 0-%d.\n", idx, sd->inventory.max_amount - 1);
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	if (!item_db.find(sd->inventory.u.items_inventory[idx].nameid)) {
		ShowWarning("buildin_countitemidx: Invalid Item ID (%u).\n", sd->inventory.u.items_inventory[idx].nameid);
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	script_pushint(st, sd->inventory.u.items_inventory[idx].amount);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: identifyidx
 * 描述: 鉴定指定背包序号的道具
 * 用法: identifyidx <背包序号>{,<角色编号>};
 * 返回: 操作成功则返回 1, 操作失败则返回 0
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(identifyidx) {
	map_session_data *sd = nullptr;
	int idx = -1;

	if (!script_charid2sd(3, sd)) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	idx = script_getnum(st, 2);
	if (idx < 0 || idx >= sd->inventory.max_amount) {
		ShowWarning("buildin_identifyidx: Index (%d) should be from 0-%d.\n", idx, sd->inventory.max_amount - 1);
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	if (!item_db.find(sd->inventory.u.items_inventory[idx].nameid)) {
		ShowWarning("buildin_identifyidx: Invalid Item ID (%u).\n", sd->inventory.u.items_inventory[idx].nameid);
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	if (sd->inventory.u.items_inventory[idx].identify != 1) {
		sd->inventory.u.items_inventory[idx].identify = 1;
		clif_item_identified(*sd, idx, 0);
	}

	script_pushint(st, 1);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: unequipidx
 * 描述: 脱下指定背包序号的道具
 * 用法: unequipidx <背包序号>{,<角色编号>};
 * 返回: 操作成功则返回 1, 操作失败则返回 0
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(unequipidx) {
	map_session_data *sd = nullptr;
	int idx = -1;

	if (!script_charid2sd(3, sd)) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	idx = script_getnum(st, 2);
	if (idx < 0 || idx >= sd->inventory.max_amount) {
		ShowWarning("buildin_unequipidx: Index (%d) should be from 0-%d.\n", idx, sd->inventory.max_amount - 1);
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	std::shared_ptr<item_data> id = item_db.find(sd->inventory.u.items_inventory[idx].nameid);
	if (id == nullptr) {
		ShowWarning("buildin_unequipidx: Invalid Item ID (%u).\n", sd->inventory.u.items_inventory[idx].nameid);
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	if (!itemdb_isequip2(id.get())) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	if (sd->inventory.u.items_inventory[idx].equip == 0) {
		script_pushint(st, 1);
		return SCRIPT_CMD_SUCCESS;
	}

	script_pushint(st, pc_unequipitem(sd, idx, id->equip) ? 1 : 0);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: equipidx
 * 描述: 穿戴指定背包序号的道具
 * 用法: equipidx <背包序号>{,<角色编号>};
 * 返回: 操作成功则返回 1, 操作失败则返回 0
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(equipidx) {
	map_session_data *sd = nullptr;
	int idx = -1;

	if (!script_charid2sd(3, sd)) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	idx = script_getnum(st, 2);
	if (idx < 0 || idx >= sd->inventory.max_amount) {
		ShowWarning("buildin_equipidx: Index (%d) should be from 0-%d.\n", idx, sd->inventory.max_amount - 1);
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	std::shared_ptr<item_data> id = item_db.find(sd->inventory.u.items_inventory[idx].nameid);
	if (id == nullptr) {
		ShowWarning("buildin_equipidx: Invalid Item ID (%u).\n", sd->inventory.u.items_inventory[idx].nameid);
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	if (!itemdb_isequip2(id.get())) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	if (sd->inventory.u.items_inventory[idx].equip != 0) {
		script_pushint(st, 1);
		return SCRIPT_CMD_SUCCESS;
	}

	script_pushint(st, pc_equipitem(sd, idx, id->equip) ? 1 : 0);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: itemexists
 * 描述: 确认物品数据库中是否存在指定物品
 * 用法: itemexists <物品编号/"物品名称">;
 * 返回: 若物品指定的道具编号不存在于物品数据库中则返回 0,
 *      若物品存在且可堆叠则返回正数物品编号, 不可堆叠则返回负数物品编号
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(itemexists) {
	std::shared_ptr<item_data> id;

	if (script_isstring(st, 2))
		id = item_db.searchname(script_getstr(st, 2));
	else
		id = item_db.find(script_getnum(st, 2));

	if (id == nullptr) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	script_pushint(st, (itemdb_isstackable2(id.get()) ? id->nameid : -(int64)id->nameid));
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: renttime
 * 描述: 增加/减少指定位置装备的租赁时间
 * 用法: renttime <EQI装备位置>,<增减的时间秒数>{,<角色编号>};
 * 返回: 操作失败返回 0, 非 0 的正数表示成功增减后新的剩余时间秒数
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(renttime) {
	map_session_data *sd = nullptr;
	int equip_num = script_getnum(st, 2);
	int second = script_getnum(st, 3);
	int idx = -1, expire_tick = 0;

	if (!script_charid2sd(4, sd)) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	if (!equip_index_check(equip_num)) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	idx = pc_checkequip(sd, equip_bitmask[equip_num]);
	if (idx < 0 || idx >= sd->inventory.max_amount) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	if (sd->inventory.u.items_inventory[idx].expire_time == 0) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	sd->inventory.u.items_inventory[idx].expire_time += second;
	expire_tick = (unsigned int)(sd->inventory.u.items_inventory[idx].expire_time - time(NULL));
	script_pushint(st, expire_tick);

	if (expire_tick > 0) {
		clif_rental_time(sd, sd->inventory.u.items_inventory[idx].nameid, expire_tick);
		pc_inventory_rental_add(sd, expire_tick);
		clif_inventorylist(sd);
	}
	else {
		int i = 0, c = 0;
		for (i = 0; i < sd->status.inventory_slots; i++) {
			if (sd->inventory.u.items_inventory[i].nameid == 0)
				continue;
			if (sd->inventory.u.items_inventory[i].expire_time == 0)
				continue;
			if (sd->inventory.u.items_inventory[i].expire_time <= time(NULL)) {
				if (sd->inventory_data[i]->unequip_script)
					run_script(sd->inventory_data[i]->unequip_script, 0, sd->id, fake_nd->id);
				clif_rental_expired(sd, i, sd->inventory.u.items_inventory[i].nameid);
				pc_delitem(sd, i, sd->inventory.u.items_inventory[i].amount, 0, 0, LOG_TYPE_OTHER);
			}
			else {
				c++;
			}
		}

		if (c <= 0) {
			pc_inventory_rental_clear(sd);
		}
	}
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: getequipidx
 * 描述: 获取指定位置装备的背包序号
 * 用法: getequipidx <EQI装备位置>{,<角色编号>};
 * 返回: 获取失败返回各种负数, 非 0 的正数表示获取到的背包序号
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(getequipidx) {
	map_session_data *sd = nullptr;
	int equip_num = script_getnum(st, 2), idx = -1;

	if (!script_charid2sd(3, sd)) {
		script_pushint(st, -3);
		return SCRIPT_CMD_SUCCESS;
	}

	if (!equip_index_check(equip_num)) {
		script_pushint(st, -2);
		return SCRIPT_CMD_SUCCESS;
	}

	idx = pc_checkequip(sd, equip_bitmask[equip_num]);
	if (idx < 0 || idx >= sd->inventory.max_amount) {
		script_pushint(st, -1);
		return SCRIPT_CMD_SUCCESS;
	}

	script_pushint(st, idx);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: getequipexpiretick
 * 描述: 获取指定位置装备的租赁到期剩余秒数
 * 用法: getequipexpiretick <EQI装备位置>{,<角色编号>};
 * 返回: 获取失败返回各种负数, 返回 0 表示目标装备非租赁, 其他非 0 正整数则代表剩余秒数
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(getequipexpiretick) {
	map_session_data *sd = nullptr;
	int equip_num = script_getnum(st, 2), idx = -1;
	int64 left_seconds = 0;

	if (!script_charid2sd(3, sd)) {
		script_pushint(st, -3);
		return SCRIPT_CMD_SUCCESS;
	}

	if (!equip_index_check(equip_num)) {
		script_pushint(st, -2);
		return SCRIPT_CMD_SUCCESS;
	}

	idx = pc_checkequip(sd, equip_bitmask[equip_num]);
	if (idx < 0 || idx >= sd->inventory.max_amount) {
		script_pushint(st, -1);
		return SCRIPT_CMD_SUCCESS;
	}
	if (!sd->inventory.u.items_inventory[idx].expire_time) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	left_seconds = (int64)(sd->inventory.u.items_inventory[idx].expire_time - time(NULL));
	if (left_seconds < 0) {
		script_pushint(st, -4);
		return SCRIPT_CMD_SUCCESS;
	}

	script_pushint(st, left_seconds);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: getinventoryinfo
 * 描述: 查询指定背包序号的道具的详细信息
 * 用法: getinventoryinfo <道具的背包序号>,<要查看的信息类型>{,<角色编号>};
 * 用法: getcartinfo <道具的手推车序号>,<要查看的信息类型>{,<角色编号>};
 * 用法: getguildstorageinfo <道具的公会仓库序号>,<要查看的信息类型>{,<角色编号>};
 * 用法: getstorageinfo <道具的个人仓库/扩充仓库序号>,<要查看的信息类型>{{,<仓库编号>},<角色编号>};
 * 返回: 查询失败返回 -1, 若查询成功则返回你所查询的信息
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
// 根据指令名称选择当前已加载的背包、手推车或仓库数据。
static bool script_pandas_getstorage(script_state* st, map_session_data* sd, s_storage** stor, item** inventory, int stor_id = 0) {
	const char* command = script_getfuncname(st);
	if (strstr(command, "cart")) {
		if (!pc_iscarton(sd))
			return false;
		*stor = &sd->cart;
		*inventory = (*stor)->u.items_cart;
	} else if (strstr(command, "guildstorage")) {
		*stor = guild2storage2(sd->status.guild_id);
		if (*stor == nullptr)
			return false;
		*inventory = (*stor)->u.items_guild;
	} else if (strstr(command, "storage")) {
		if (stor_id == 0)
			*stor = &sd->storage;
		else if (sd->premiumStorage.stor_id == stor_id)
			*stor = &sd->premiumStorage;
		else
			return false;
		*inventory = (*stor)->u.items_storage;
	} else {
		*stor = &sd->inventory;
		*inventory = (*stor)->u.items_inventory;
	}
	return true;
}

BUILDIN_FUNC(getinventoryinfo) {
    map_session_data *sd = nullptr;
	struct item_data *id = nullptr;
	int idx = script_getnum(st, 2);
	const char* command = script_getfuncname(st);
	int charid_slot = (!stricmp(command, "getstorageinfo") ? 5 : 4);

	if (!script_charid2sd(charid_slot, sd)) {
		script_pushint(st, -1);
		return SCRIPT_CMD_SUCCESS;
	}

	struct s_storage* stor = nullptr;
	struct item* inventory = nullptr;
	int stor_id = (script_hasdata(st, 4) ? script_getnum(st, 4) : 0);

	if (!script_pandas_getstorage(st, sd, &stor, &inventory, stor_id)) {
		return SCRIPT_CMD_FAILURE;
	}

	if (st->state == RERUNLINE) {
		return SCRIPT_CMD_SUCCESS;
	}

	if (!stor || !inventory) {
		ShowError("buildin_%s: cannot read inventory or storage data.\n", command);
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;
	}

	if (idx < 0 || idx >= stor->max_amount) {
		script_pushint(st, -1);
		return SCRIPT_CMD_SUCCESS;
	}

	if (!item_db.exists(inventory[idx].nameid)) {
		script_pushint(st, -1);
		return SCRIPT_CMD_SUCCESS;
	}

	if (inventory[idx].amount <= 0) {
		script_pushint(st, -1);
		return SCRIPT_CMD_SUCCESS;
	}

	int type = script_getnum(st, 3);
	switch (type)
	{
	case 0:  script_pushint(st, inventory[idx].nameid); break;
	case 1:  script_pushint(st, inventory[idx].amount); break;
	case 2:  script_pushint(st, inventory[idx].equip); break;
	case 3:  script_pushint(st, inventory[idx].refine); break;
	case 4:  script_pushint(st, inventory[idx].identify); break;
	case 5:  script_pushint(st, inventory[idx].attribute); break;
	case 6:  script_pushint(st, inventory[idx].card[0]); break;
	case 7:  script_pushint(st, inventory[idx].card[1]); break;
	case 8:  script_pushint(st, inventory[idx].card[2]); break;
	case 9:  script_pushint(st, inventory[idx].card[3]); break;
	case 10: script_pushint(st, inventory[idx].expire_time); break;
	case 11: script_pushint(st, inventory[idx].unique_id); break;
	case 12: case 13: case 14: case 15: case 16:
		script_pushint(st, inventory[idx].option[type - 12].id); break;
	case 17: case 18: case 19: case 20: case 21:
		script_pushint(st, inventory[idx].option[type - 17].value); break;
	case 22: case 23: case 24: case 25: case 26:
		script_pushint(st, inventory[idx].option[type - 22].param); break;
	case 27: script_pushint(st, inventory[idx].bound); break;
	case 28: script_pushint(st, inventory[idx].enchantgrade); break;
	case 29: script_pushint(st, inventory[idx].equipSwitch); break;
	case 30: script_pushint(st, inventory[idx].favorite); break;
	default:
		ShowWarning("buildin_%s: The type should be in range 0-%d, currently type is: %d.\n", command, 30, type);
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;
	}

	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: statuscheck
 * 描述: 判断状态是否存在, 并取得相关的状态参数
 * 用法: statuscheck <状态编号>{,<游戏单位编号>};
 * 返回: 获取成功则返回 1, 角色没有该状态则返回 0, 其他错误返回 -1
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(statuscheck) {
	map_session_data *sd = nullptr;
	if (!script_mapid2sd(3, sd)) {
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;
	}

	int id = script_getnum(st, 2);
	if (id <= SC_NONE || id >= SC_MAX) {
		ShowWarning("buildin_statuscheck: Invalid status type given (%d).\n", id);
		script_pushint(st, -1);
		return SCRIPT_CMD_SUCCESS;
	}

	if (sd->sc.empty() || !sd->sc.getSCE(id)) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	pc_setreg(sd, add_str("@sc_val1"), sd->sc.getSCE(id)->val1);
	pc_setreg(sd, add_str("@sc_val2"), sd->sc.getSCE(id)->val2);
	pc_setreg(sd, add_str("@sc_val3"), sd->sc.getSCE(id)->val3);
	pc_setreg(sd, add_str("@sc_val4"), sd->sc.getSCE(id)->val4);

	// 这种返回方式若剩余时间过长的话 @sc_tickleft 保存的数值会被截断, 不可靠
	// 建议还是多使用 rAthena 自带的 getstatus 指令来替代 statuscheck / sc_check
	// 之所以实现 statuscheck / sc_check 完全出于兼容目的考虑
	struct TimerData* timer = (struct TimerData*)get_timer(sd->sc.getSCE(id)->timer);
	t_tick tickleft = (timer ? DIFF_TICK(timer->tick, gettick()) : -1);
	pc_setreg(sd, add_str("@sc_tickleft"), tickleft);

	script_pushint(st, 1);
	return SCRIPT_CMD_SUCCESS;
}

//************************************
// Method:      inventory_rental_update
// Description: 移除过期租赁道具; 最后如果连一件租赁道具都没有, 那么直接删掉计时器
// Parameter:   map_session_data * sd
// Returns:     void
// Author:      Sola丶小克(CairoLee)  2020/5/26 23:23
//************************************
void inventory_rental_update(map_session_data* sd) {
	int i = 0, c = 0;

	if (!sd) {
		return;
	}

	for (i = 0; i < sd->status.inventory_slots; i++) {
		if (sd->inventory.u.items_inventory[i].nameid == 0)
			continue;
		if (sd->inventory.u.items_inventory[i].expire_time == 0)
			continue;
		if (sd->inventory.u.items_inventory[i].expire_time <= time(NULL)) {
			if (sd->inventory_data[i]->unequip_script)
				run_script(sd->inventory_data[i]->unequip_script, 0, sd->id, fake_nd->id);
			clif_rental_expired(sd, i, sd->inventory.u.items_inventory[i].nameid);
			pc_delitem(sd, i, sd->inventory.u.items_inventory[i].amount, 0, 0, LOG_TYPE_OTHER);
		}
		else {
			c++;
		}
	}

	if (c <= 0) {
		pc_inventory_rental_clear(sd);
	}
}

/* ===========================================================
 * 指令: renttimeidx
 * 描述: 增加/减少指定背包序号道具的租赁时间
 * 用法: renttimeidx <背包序号>,<增减的时间秒数>{,<角色编号>};
 * 返回: 操作失败返回 0, 非 0 的正数表示成功增减后新的剩余时间秒数
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(renttimeidx) {
	map_session_data *sd = nullptr;
	int idx = script_getnum(st, 2);
	int second = script_getnum(st, 3);
	int expire_tick = 0;

	if (!script_charid2sd(4, sd)) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	if (idx < 0 || idx >= sd->inventory.max_amount) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	if (sd->inventory.u.items_inventory[idx].expire_time == 0) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	sd->inventory.u.items_inventory[idx].expire_time += second;
	expire_tick = (unsigned int)(sd->inventory.u.items_inventory[idx].expire_time - time(NULL));
	script_pushint(st, expire_tick);

	if (expire_tick > 0) {
		clif_rental_time(sd, sd->inventory.u.items_inventory[idx].nameid, expire_tick);
		pc_inventory_rental_add(sd, expire_tick);
		clif_inventorylist(sd);
	}
	else {
		inventory_rental_update(sd);
	}
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: party_leave
 * 描述: 使当前角色或指定角色退出队伍
 * 用法: party_leave {<角色编号>};
 * 返回: 若指定的角色不在线或不在队伍中则返回 0, 若角色成功退出队伍则返回 1
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(party_leave) {
	map_session_data *sd = nullptr;

	if (!script_charid2sd(2, sd)) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	script_pushint(st, party_leave(*sd));
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: buildin_script4each_sub
 * 描述: 配合 script4each 指令使用的一个内部处理函数
 * -----------------------------------------------------------*/
static int buildin_script4each_sub(block_list *bl, va_list ap) {
	struct script_code* script = va_arg(ap, struct script_code*);
	int pos = va_arg(ap, int);

	if (!bl || !script) return 0;

	mapreg_setreg(add_str("$@gid"), bl->id);
	run_script(script, pos, bl->id, 0);
	return 1;
}

/* ===========================================================
 * 指令: script4each / script4eachmob / script4eachnpc
 * 描述: 对指定范围的玩家执行相同的一段脚本
 * 用法: script4each <"{脚本}">,<脚本的执行范围>{,<动态参数>...};
 * 用法: script4eachmob <"{脚本}">,<脚本的执行范围>{,<动态参数>...};
 * 用法: script4eachnpc <"{脚本}">,<脚本的执行范围>{,<动态参数>...};
 * 返回: 该指令无论成功失败, 都不会有返回值
 * 作者: Sola丶小克 (最早借鉴自 Sense 的代码进行改进)
 * -----------------------------------------------------------*/
BUILDIN_FUNC(script4each) {
	const char *execute_script = script_getstr(st, 2);
	int execute_range = script_getnum(st, 3);

	struct script_code* script = nullptr;
	int pos = 0;
	bool script_needfree = false;

	// rAthena 保持原生命令的脚本文本语义，事件标签可在脚本文本中通过 doevent 调用。
	script = parse_script(execute_script, script_getfuncname(st), 0, SCRIPT_IGNORE_EXTERNAL_BRACKETS);
	script_needfree = (script != nullptr);

	struct s_mapiterator *iter = nullptr;
	block_list* bl = nullptr;
	enum bl_type bltype = BL_PC;

	// 根据使用的指令名称, 确定后续对什么类型的单位进行遍历
	const char* command = script_getfuncname(st);
	if (stricmp(command, "script4each") == 0) {
		bltype = BL_PC;
		iter = mapit_geteachpc();
	}
	else if (stricmp(command, "script4eachmob") == 0) {
		bltype = BL_MOB;
		iter = mapit_geteachmob();
	}
	else if (stricmp(command, "script4eachnpc") == 0) {
		bltype = BL_NPC;
		iter = mapit_geteachnpc();
	}

	switch (execute_range)
	{
	case 0: {
		// 全服单位 - script4each "{<脚本>}",0;
		for (bl = mapit_first(iter); mapit_exists(iter); bl = mapit_next(iter)) {
			if (!bl || bl->type != bltype) continue;
			mapreg_setreg(add_str("$@gid"), bl->id);
			run_script(script, pos, bl->id, 0);
		}
		break;
	}
	case 1: {
		// 指定地图上的全部单位 - script4each "{<脚本>}",1,<"地图名称">;
		int map_id = -1;

		if (!script_hasdata(st, 4) || !script_isstring(st, 4)) break;
		if ((map_id = map_mapname2mapid(script_getstr(st, 4))) < 0) break;

		for (bl = mapit_first(iter); mapit_exists(iter); bl = mapit_next(iter)) {
			if (!bl || bl->type != bltype || bl->m != map_id) continue;
			mapreg_setreg(add_str("$@gid"), bl->id);
			run_script(script, pos, bl->id, 0);
		}
		break;
	}
	case 2: {
		// 以地图某个点为中心半径距离内的单位 - script4each "{<脚本>}",2,<"地图名称">,<中心坐标x>,<中心坐标y>,<范围>;
		int map_id = -1, map_x = 0, map_y = 0, range = 0;

		if (!script_hasdata(st, 4) || !script_isstring(st, 4)) break;
		if (!script_hasdata(st, 5) || !script_isint(st, 5)) break;
		if (!script_hasdata(st, 6) || !script_isint(st, 6)) break;
		if (!script_hasdata(st, 7) || !script_isint(st, 7)) break;

		if ((map_id = map_mapname2mapid(script_getstr(st, 4))) < 0) break;
		map_x = script_getnum(st, 5);
		map_y = script_getnum(st, 6);
		range = script_getnum(st, 7);

		struct block_list center_bl = { 0 };
		center_bl.m = map_id;
		center_bl.x = map_x;
		center_bl.y = map_y;

		map_foreachinrange(buildin_script4each_sub, &center_bl, range, bltype, script, pos);
		break;
	}
	case 3: {
		// 指定玩家所在的队伍中的全部队伍成员 - script4each "{<脚本>}",3,<角色编号>;
		int party_id = 0;
		map_session_data *target_sd = nullptr;

		// 该类型不支持 script4eachmob 和 script4eachnpc 指令
		if (bltype != BL_PC) break;

		if (!script_hasdata(st, 4) || !script_isint(st, 4)) break;
		target_sd = map_charid2sd(script_getnum(st, 4));
		if (!target_sd || (party_id = target_sd->status.party_id) <= 0) break;

		for (bl = mapit_first(iter); mapit_exists(iter); bl = mapit_next(iter)) {
			if (!bl || bl->type != bltype) continue;
			if (((TBL_PC*)bl)->status.party_id != party_id) continue;
			mapreg_setreg(add_str("$@gid"), bl->id);
			run_script(script, pos, bl->id, 0);
		}
		break;
	}
	case 4: {
		// 指定玩家所在的公会中的全部公会成员 - script4each "{<脚本>}",4,<角色编号>;
		int guild_id = 0;
		map_session_data *target_sd = nullptr;

		// 该类型不支持 script4eachmob 和 script4eachnpc 指令
		if (bltype != BL_PC) break;

		if (!script_hasdata(st, 4) || !script_isint(st, 4)) break;
		target_sd = map_charid2sd(script_getnum(st, 4));
		if (!target_sd || (guild_id = target_sd->status.guild_id) <= 0) break;

		for (bl = mapit_first(iter); mapit_exists(iter); bl = mapit_next(iter)) {
			if (!bl || bl->type != bltype) continue;
			if (((TBL_PC*)bl)->status.guild_id != guild_id) continue;
			mapreg_setreg(add_str("$@gid"), bl->id);
			run_script(script, pos, bl->id, 0);
		}
		break;
	}
	case 5: {
		// 指定区域 - script4each "{<脚本>}",5,<"地图名称">,<坐标x0>,<坐标y0>,<坐标x1>,<坐标y1>;
		int map_id = -1, map_x0 = 0, map_y0 = 0, map_x1 = 0, map_y1 = 0;

		if (!script_hasdata(st, 4) || !script_isstring(st, 4)) break;
		if (!script_hasdata(st, 5) || !script_isint(st, 5)) break;
		if (!script_hasdata(st, 6) || !script_isint(st, 6)) break;
		if (!script_hasdata(st, 7) || !script_isint(st, 7)) break;
		if (!script_hasdata(st, 8) || !script_isint(st, 8)) break;

		if ((map_id = map_mapname2mapid(script_getstr(st, 4))) < 0) break;
		map_x0 = script_getnum(st, 5);
		map_y0 = script_getnum(st, 6);
		map_x1 = script_getnum(st, 7);
		map_y1 = script_getnum(st, 8);

		map_foreachinarea(buildin_script4each_sub, map_id, map_x0, map_y0, map_x1, map_y1, bltype, script, pos);
		break;
	}
	case 6: {
		// 指定队伍中的全部队伍成员 - script4each "{<脚本>}",6,<队伍编号>;
		int party_id = 0;

		// 该类型不支持 script4eachmob 和 script4eachnpc 指令
		if (bltype != BL_PC) break;

		if (!script_hasdata(st, 4) || !script_isint(st, 4)) break;
		if ((party_id = script_getnum(st, 4)) <= 0) break;;

		for (bl = mapit_first(iter); mapit_exists(iter); bl = mapit_next(iter)) {
			if (!bl || bl->type != bltype) continue;
			if (((TBL_PC*)bl)->status.party_id != party_id) continue;
			mapreg_setreg(add_str("$@gid"), bl->id);
			run_script(script, pos, bl->id, 0);
		}
		break;
	}
	case 7: {
		// 指定公会中的全部公会成员 - script4each "{<脚本>}",7,<公会编号>;
		int guild_id = 0;

		// 该类型不支持 script4eachmob 和 script4eachnpc 指令
		if (bltype != BL_PC) break;

		if (!script_hasdata(st, 4) || !script_isint(st, 4)) break;
		if ((guild_id = script_getnum(st, 4)) <= 0) break;

		for (bl = mapit_first(iter); mapit_exists(iter); bl = mapit_next(iter)) {
			if (!bl || bl->type != bltype) continue;
			if (((TBL_PC*)bl)->status.guild_id != guild_id) continue;
			mapreg_setreg(add_str("$@gid"), bl->id);
			run_script(script, pos, bl->id, 0);
		}
		break;
	}
	default:
		ShowWarning("buildin_%s: Invalid execute range '%d'.\n", script_getfuncname(st), execute_range);
		break;
	}

	if (script && script_needfree) script_free_code(script);
	if (iter) mapit_free(iter);

	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: buildin_getsameipinfo_sub
 * 描述: 配合 getsameipinfo 指令使用的一个内部处理函数
 * -----------------------------------------------------------*/
static int buildin_getsameipinfo_sub(map_session_data* pl_sd, va_list ap)
{
	map_session_data *sd = va_arg(ap, map_session_data*);
	uint32 ipaddr = va_arg(ap, uint32);
	uint32 *count = va_arg(ap, uint32*);
	int32 m = va_arg(ap, int32);	// int16 通过可变参数方式传递, 会被提升为 int32

	if (!ipaddr || !sd || !count) return 0;
	if (!pl_sd || pl_sd->state.autotrade) return 0;
	if (m >= 0 && pl_sd->m != m) return 0;

	if (ipaddr == session[pl_sd->fd]->client_addr) {
		pc_setreg(sd, reference_uid(add_str("@sameip_aid"), (*count)), pl_sd->status.account_id);
		pc_setreg(sd, reference_uid(add_str("@sameip_cid"), (*count)), pl_sd->status.char_id);
		pc_setregstr(sd, reference_uid(add_str("@sameip_name$"), (*count)), pl_sd->status.name);
		(*count)++;
		return 1;
	}

	return 0;
}

/* ===========================================================
 * 指令: getsameipinfo
 * 描述: 获得某个指定 IP 在线的玩家信息
 * 用法: getsameipinfo {<"IP地址">{<,"地图名">}};
 * 返回: 出错返回 -1, 其他含 0 正整数表示查到的此 IP 的在线玩家数
 * 作者: Sola丶小克, 晓晓
 * -----------------------------------------------------------*/
BUILDIN_FUNC(getsameipinfo) {
	map_session_data *sd = nullptr;
	uint32 ipaddr = 0, match_count = 0;
	int16 m = -1;

	if (!script_rid2sd(sd)) {
		script_pushint(st, -1);
		return SCRIPT_CMD_SUCCESS;
	}

	if (script_hasdata(st, 2) && !script_isstring(st, 2)) {
		ShowError("buildin_getsameipinfo: ip address must be string variable\n");
		script_pushint(st, -1);
		return SCRIPT_CMD_SUCCESS;
	}

	if (script_hasdata(st, 2)) {
		ipaddr = str2ip(script_getstr(st, 2));
	}
	else {
		ipaddr = session[sd->fd]->client_addr;
	}

	if (script_hasdata(st, 3)) {
		const char* map_name = script_getstr(st, 3);
		if ((m = map_mapname2mapid(map_name)) < 0) {
			ShowWarning("buildin_getsameipinfo: Invalid map name '%s'.\n", map_name);
			script_pushint(st, -1);
			return SCRIPT_CMD_SUCCESS;
		}
	}

	map_foreachpc(buildin_getsameipinfo_sub, sd, ipaddr, &match_count, m);
	pc_setreg(sd, add_str("@sameip_amount"), match_count);

	script_pushint(st, match_count);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: logout
 * 描述: 使指定的角色立刻登出游戏
 * 用法: logout <登出理由>{,"<角色名称>"|<账号编号>|<角色编号>};
 * 返回: 该指令无论成功失败, 都不会有返回值
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(logout) {
	map_session_data *sd = nullptr;
	int reason = script_getnum(st, 2);

	if (script_hasdata(st, 3)) {
		if (script_isstring(st, 3)) {
			sd = map_nick2sd(script_getstr(st, 3), false);
		}
		else {
			int id = script_getnum(st, 3);
			sd = (map_id2sd(id) ? map_id2sd(id) : map_charid2sd(id));
		}
	}
	else {
		script_rid2sd(sd);
	}

	if (!sd) {
		return SCRIPT_CMD_SUCCESS;
	}

	if (!((reason >= 0 && reason <= 18) || (reason >= 100 && reason <= 110) || reason == 111)) {
		ShowWarning("buildin_logout: unknown logout reason %d\n", reason);
		return SCRIPT_CMD_SUCCESS;
	}

	if (sd->fd) {
		clif_authfail_fd(sd->fd, reason);
	}
	else {
		map_quit(sd);
	}
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: buildin_getareagid_sub
 * 描述: 配合 getareagid 指令使用的一个内部处理函数
 * -----------------------------------------------------------*/
static int buildin_getareagid_sub(block_list *bl, va_list ap) {
	map_session_data *sd = nullptr;
	struct script_state *st = va_arg(ap, struct script_state *);
	struct script_data *retdata = va_arg(ap, struct script_data *);
	uint32* found_count = va_arg(ap, uint32*);

	if (!st || !retdata || !found_count) {
		return 0;
	}

	int64 uid = reference_uid(reference_getid(retdata), (*found_count));
	const char *retname = reference_getname(retdata);
	sd = map_id2sd(st->rid);

	set_reg_num(st, sd, uid, retname, bl->id, reference_getref(retdata));

	(*found_count)++;
	return 1;
}

/* ===========================================================
 * 指令: getareagid
 * 描述: 获取指定范围内特定类型单位的全部 GID
 * 用法: getareagid <返回数组>,<搜索范围>{,<动态参数>...};
 * 返回: 操作失败返回 -1, 其他含 0 正整数表示搜索到的 GID 数量
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(getareagid) {
	map_session_data *sd = nullptr;
	struct script_data *retdata = script_getdata(st, 2);
	struct reg_db *src_reg_db = nullptr;
	int search_range = script_getnum(st, 3);
	uint32 found_count = 0;

	if (!data_isreference(retdata)) {
		ShowError("buildin_getareagid: error argument! please give a array variable for save gameid.\n");
		script_reportdata(retdata);
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;// not a variable
	}

	int retid = reference_getid(retdata);
	const char *retname = reference_getname(retdata);

	if (not_server_variable(*retname)) {
		if (!script_rid2sd(sd)) {
			ShowError("buildin_getareagid: '%s' is not server variable, please attach to a player.\n", retname);
			script_reportdata(retdata);
			script_pushint(st, -1);
			return SCRIPT_CMD_FAILURE;
		}
	}

	if (!(src_reg_db = script_array_src(st, sd, retname, reference_getref(retdata)))) {
		ShowError("buildin_getareagid: variable '%s' is not a array.\n", retname);
		script_reportdata(retdata);
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;
	}

	if (is_string_variable(retname)) {
		ShowError("buildin_getareagid: the array variable '%s' must be integer type.\n", retname);
		script_reportdata(retdata);
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;
	}

	// 以下用于清空存放返回数据的数值型数组 (参考 script_cleararray_pc 的实现)
	script_array_ensure_zero(st, NULL, retdata->u.num, reference_getref(retdata));
	struct script_array* sa = static_cast<script_array *>(idb_get(src_reg_db->arrays, retid));
	unsigned int array_len = script_array_highest_key(st, sd, retname, reference_getref(retdata));

	if (sa) {
		// 若给定的数组是存在的, 那么需要清空一下
		unsigned int* list = script_array_cpy_list(sa);
		unsigned int size = sa->size;

		for (unsigned int i = 0; i < size; i++) {
			clear_reg(st, sd, reference_uid(retid, list[i]), retname, reference_getref(retdata));
		}
	}

	switch (search_range)
	{
	case 0: {
		// getareagid <返回数组>,0,<想搜索的单位类型>,<"地图名称">;
		int map_id = -1, unitfilter = BL_ALL;
		std::string mapname;

		if (!script_get_optnum(st, 4, "Unit Type", unitfilter)) { script_pushint(st, -1); return SCRIPT_CMD_SUCCESS; }
		if (!script_get_optstr(st, 5, "Map Name", mapname)) { script_pushint(st, -1); return SCRIPT_CMD_SUCCESS; }
		if (!script_get_mapindex(st, mapname.c_str(), map_id)) { script_pushint(st, -1); return SCRIPT_CMD_SUCCESS; }

		map_foreachinmap(buildin_getareagid_sub, map_id, unitfilter, st, retdata, &found_count);
		break;
	}
	case 1: {
		// getareagid <返回数组>,1,<想搜索的单位类型>,<"地图名称">,<中心坐标x>,<中心坐标y>,<范围>;
		int map_id = -1, unitfilter = BL_ALL;
		int map_x = 0, map_y = 0, range = 0;
		std::string mapname;

		if (!script_get_optnum(st, 4, "Unit Type", unitfilter)) { script_pushint(st, -1); return SCRIPT_CMD_SUCCESS; }
		if (!script_get_optstr(st, 5, "Map Name", mapname)) { script_pushint(st, -1); return SCRIPT_CMD_SUCCESS; }
		if (!script_get_optnum(st, 6, "Center X", map_x)) { script_pushint(st, -1); return SCRIPT_CMD_SUCCESS; }
		if (!script_get_optnum(st, 7, "Center Y", map_y)) { script_pushint(st, -1); return SCRIPT_CMD_SUCCESS; }
		if (!script_get_optnum(st, 8, "Range", range)) { script_pushint(st, -1); return SCRIPT_CMD_SUCCESS; }
		if (!script_get_mapindex(st, mapname.c_str(), map_id)) { script_pushint(st, -1); return SCRIPT_CMD_SUCCESS; }

		struct block_list center_bl = { 0 };
		center_bl.m = map_id;
		center_bl.x = map_x;
		center_bl.y = map_y;

		map_foreachinrange(buildin_getareagid_sub, &center_bl, range, unitfilter, st, retdata, &found_count);
		break;
	}
	case 2: {
		// getareagid <返回数组>,2,<想搜索的单位类型>,<"地图名称">,<坐标x0>,<坐标y0>,<坐标x1>,<坐标y1>;
		int map_id = -1, unitfilter = BL_ALL;
		int map_x0 = 0, map_y0 = 0, map_x1 = 0, map_y1 = 0;
		std::string mapname;

		if (!script_get_optnum(st, 4, "Unit Type", unitfilter)) { script_pushint(st, -1); return SCRIPT_CMD_SUCCESS; }
		if (!script_get_optstr(st, 5, "Map Name", mapname)) { script_pushint(st, -1); return SCRIPT_CMD_SUCCESS; }
		if (!script_get_optnum(st, 6, "x0 coordinate", map_x0)) { script_pushint(st, -1); return SCRIPT_CMD_SUCCESS; }
		if (!script_get_optnum(st, 7, "y0 coordinate", map_y0)) { script_pushint(st, -1); return SCRIPT_CMD_SUCCESS; }
		if (!script_get_optnum(st, 8, "x1 coordinate", map_x1)) { script_pushint(st, -1); return SCRIPT_CMD_SUCCESS; }
		if (!script_get_optnum(st, 9, "y1 coordinate", map_y1)) { script_pushint(st, -1); return SCRIPT_CMD_SUCCESS; }
		if (!script_get_mapindex(st, mapname.c_str(), map_id)) { script_pushint(st, -1); return SCRIPT_CMD_SUCCESS; }

		map_foreachinarea(buildin_getareagid_sub, map_id, map_x0, map_y0, map_x1, map_y1, unitfilter, st, retdata, &found_count);
		break;
	}
	default:
		ShowWarning("buildin_getareagid: Invalid search range '%d'.\n", search_range);
		break;
	}

	script_pushint(st, found_count);
	return SCRIPT_CMD_SUCCESS;
}


/* ===========================================================
 * 指令: settrigger
 * 描述: 使用该指令可以设置某个事件或过滤器的触发行为 (禁止触发、下次触发、永久触发)
 * 用法: settrigger <事件的常量名称>,<触发行为>;
 * 返回: 该指令无论成功失败, 都不会有返回值
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(settrigger) {
	map_session_data *sd = nullptr;

	if (!script_rid2sd(sd))
		return SCRIPT_CMD_SUCCESS;

	uint16 envtype = script_getnum(st, 2);
	if (envtype >= NPCE_MAX) {
		ShowError("buildin_settrigger: Invalid npc event type: %d\n", envtype);
		return SCRIPT_CMD_FAILURE;
	}

	uint16 triggerflag = script_getnum(st, 3);
	if (triggerflag >= EVENT_TRIGGER_MAX) {
		ShowError("buildin_settrigger: Invalid npc event trigger type: %d\n", triggerflag);
		return SCRIPT_CMD_FAILURE;
	}

	const char* name = npc_get_script_event_name(envtype);
	if (name == nullptr) {
		ShowError("buildin_settrigger: Can not get the event name for event type: %d\n", envtype);
		return SCRIPT_CMD_FAILURE;
	}

	if (!setEventTrigger(sd, (npce_event)envtype, (npce_trigger)triggerflag)) {
		ShowError("buildin_processhalt: An error occurred while setting the '%s' event trigger type to '%d'.\n", name, triggerflag);
		return SCRIPT_CMD_FAILURE;
	}
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: messagecolor
 * 描述: 发送指定颜色的消息文本到聊天窗口中
 * 用法: messagecolor "<消息文本>"{,"<文本颜色代码>",<发送目标>,<游戏单位编号>};
 * 返回: 该指令无论成功失败, 都不会有返回值
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(messagecolor) {
	const char* text = script_getstr(st, 2);

	const char* color = "ffffff";
	if (script_hasdata(st, 3)) {
		color = script_getstr(st, 3);
	}

	block_list *bl = nullptr;
	if (!script_rid2bl(5, bl)) {
		return SCRIPT_CMD_FAILURE;
	}

	send_target target = AREA;
	if (script_hasdata(st, 4)) {
		switch (script_getnum(st, 4)) {
			case BC_ALL:	target = ALL_CLIENT;	break;
			case BC_MAP:	target = ALL_SAMEMAP;	break;
			case BC_SELF:	target = SELF;			break;
			case BC_AREA:
			default:		target = AREA;			break;
		}
	}

	clif_messagecolor(bl, strtol(color, (char**)NULL, 16), text, true, target);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: copynpc
 * 描述: 复制指定的 NPC 到一个新的位置
 * 用法: copynpc "<复制出来的新NPC所在地图名称>,<X坐标>,<Y坐标>,<朝向编号>","duplicate(<来源NPC名称>)","<复制出来的新NPC名称>","<图档外观编号>";
 * 用法: copynpc "<复制出来的新NPC所在地图名称>",<X坐标>,<Y坐标>,<朝向编号>,"<来源NPC名称>","<复制出来的新NPC名称>",<图档外观编号>;
 * 返回: 复制成功则返回新 NPC 的 GID, 复制失败则返回 0
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(copynpc) {
	// --------------------------------------------------------------------------------------
	// 本指令的绝大部分代码来源自 npc.cpp 的 npc_parse_duplicate 函数
	// 若未来 rAthena 官方修改了 npc_parse_duplicate 那么这里也需要进行对应的调整, 以便确保主流程一致
	// --------------------------------------------------------------------------------------

	const char *w1 = nullptr, *w2 = nullptr, *w3 = nullptr, *w4 = nullptr;
	char w1buf[256] = { 0 }, w2buf[256] = { 0 }, w3buf[256] = { 0 }, w4buf[256] = { 0 };
	DBMap* npcname_db = get_npcname_db_ptr();
	int* npc_script = get_npc_script_ptr();
	int* npc_shop = get_npc_shop_ptr();
	int* npc_warp = get_npc_warp_ptr();
	npc_data* local_nd = map_id2nd(st->oid);	// 当前执行了 copynpc 指令的 npc 对象
	const char* filepath = (local_nd ? local_nd->path : "Unable to confirm the file path where the NPC executes 'copynpc' command.");
	const char *start = nullptr, *buffer = nullptr; // npc_parsename 和 npc_parseview 需要用到, 由于没有具体数据, 所以故意不赋值

	// 如果在物品的使用脚本或类似的无 NPC 作为触发主体的地方使用了 copynpc,
	// 那么 filepath 将会是空指针, 而不是某个 npc 脚本的具体路径.
	// 这会导致 npc_parsename 函数中使用 strlen(filepath) 时导致空指针崩溃.
	// 在这种情况下我们将此 npc 归属的路径, 设置为当前函数的名字: buildin_copynpc 以规避此问题
	if (filepath == nullptr) {
		filepath = __func__;
	}

	do
	{
		if (script_lastdata(st) == 5) {
			// 若用四个参数的形式调用 copynpc, 那么四个参数必须全部都是字符串类型
			// 第四个参数可以允许是数值变量, 因为数值变量可以直接被 script_getstr 强制转换成字符串
			if (script_isstring(st, 2) && script_isstring(st, 3) &&
				script_isstring(st, 4) && (script_isstring(st, 5) || script_isint(st, 5))) {
				w1 = script_getstr(st, 2);
				w2 = script_getstr(st, 3);
				w3 = script_getstr(st, 4);
				w4 = script_getstr(st, 5);
				break; // 这里的 break 表示流程正常结束, 无需报错
			}
		}
		else if (script_lastdata(st) == 8) {
			// 若使用七个参数的形式调用 copynpc, 那么七个参数的类型分别需要是: siiissi
			if (script_isstring(st, 2) && script_isint(st, 3) &&
				script_isint(st, 4) && script_isint(st, 5) &&
				script_isstring(st, 6) && script_isstring(st, 7) && script_isint(st, 8)) {

				sprintf(w1buf, "%s,%d,%d,%d", script_getstr(st, 2), script_getnum(st, 3), script_getnum(st, 4), script_getnum(st, 5));
				sprintf(w2buf, "%s", script_getstr(st, 6));
				sprintf(w3buf, "%s", script_getstr(st, 7));
				sprintf(w4buf, "%d", script_getnum(st, 8));

				w1 = w1buf;
				w2 = w2buf;
				w3 = w3buf;
				w4 = w4buf;
				break; // 这里的 break 表示流程正常结束, 无需报错
			}
		}

		ShowError("buildin_copynpc: Invalid parameters, please read the manual for copynpc.\n");
		script_pushint(st, 0);
		return SCRIPT_CMD_FAILURE;
	} while (false);

	short x, y, m, xs = -1, ys = -1;
	int16 dir;
	char srcname[128];
	int i;
	//const char* end;
	size_t length;

	int src_id;
	int type;
	npc_data* nd;
	npc_data* dnd;

	//end = strchr(start, '\n');
	length = strlen(w2);

	if (strncmpi(w2, "duplicate(", 10) == 0) {
		// get the npc being duplicated
		if (w2[length - 1] != ')' || length <= 11 || length - 11 >= sizeof(srcname))
		{// does not match 'duplicate(%127s)', name is empty or too long
			//ShowError("npc_parse_script: bad duplicate name in file '%s', line '%d' : %s\n", filepath, strline(buffer, start - buffer), w2);
			//return end;// next line, try to continue
			ShowError("buildin_copynpc: bad duplicate name in file '%s' : %s\n", filepath, w2);
			script_pushint(st, 0);
			return SCRIPT_CMD_FAILURE;
		}
		safestrncpy(srcname, w2 + 10, length - 10);
	}
	else {
		safestrncpy(srcname, w2, sizeof(srcname));
	}

	dnd = npc_name2id(srcname);
	if (dnd == NULL) {
		//ShowError("npc_parse_script: original npc not found for duplicate in file '%s', line '%d' : %s\n", filepath, strline(buffer, start - buffer), srcname);
		//return end;// next line, try to continue
		ShowError("buildin_copynpc: original npc not found for duplicate in file '%s' : %s\n", filepath, srcname);
		script_pushint(st, 0);
		return SCRIPT_CMD_FAILURE;
	}
	src_id = dnd->src_id ? dnd->src_id : dnd->id;
	type = dnd->subtype;

	// get placement
	if ((type == NPCTYPE_SHOP || type == NPCTYPE_CASHSHOP || type == NPCTYPE_ITEMSHOP || type == NPCTYPE_POINTSHOP || type == NPCTYPE_SCRIPT || type == NPCTYPE_MARKETSHOP) && strcmp(w1, "-") == 0) {// floating shop/chashshop/itemshop/pointshop/script
		x = y = dir = 0;
		m = -1;
	}
	else {
		char mapname[MAP_NAME_LENGTH_EXT];

		if (sscanf(w1, "%15[^,],%6hd,%6hd,%4hd", mapname, &x, &y, &dir) != 4) { // <map name>,<x>,<y>,<facing>
			//ShowError("npc_parse_duplicate: Invalid placement format for duplicate in file '%s', line '%d'. Skipping line...\n * w1=%s\n * w2=%s\n * w3=%s\n * w4=%s\n", filepath, strline(buffer, start - buffer), w1, w2, w3, w4);
			//return end;// next line, try to continue
			ShowError("buildin_copynpc: Invalid placement format for duplicate in file '%s'\n * w1=%s\n * w2=%s\n * w3=%s\n * w4=%s\n", filepath, w1, w2, w3, w4);
			script_pushint(st, 0);
			return SCRIPT_CMD_FAILURE;
		}
		m = map_mapname2mapid(mapname);
	}

	struct map_data *mapdata = map_getmapdata(m);

	if (m != -1 && (x < 0 || x >= mapdata->xs || y < 0 || y >= mapdata->ys)) {
		//ShowError("npc_parse_duplicate: coordinates %d/%d are out of bounds in map %s(%dx%d), in file '%s', line '%d'\n", x, y, mapdata->name, mapdata->xs, mapdata->ys, filepath, strline(buffer, start - buffer));
		ShowError("buildin_copynpc: coordinates %d/%d are out of bounds in map %s(%dx%d), in file '%s'\n", x, y, mapdata->name, mapdata->xs, mapdata->ys, filepath);
		script_pushint(st, 0);
		return SCRIPT_CMD_FAILURE;
	}

	if (type == NPCTYPE_WARP && sscanf(w4, "%6hd,%6hd", &xs, &ys) == 2);// <spanx>,<spany>
	else if (type == NPCTYPE_SCRIPT && sscanf(w4, "%*[^,],%6hd,%6hd", &xs, &ys) == 2);// <sprite id>,<triggerX>,<triggerY>
	else if (type == NPCTYPE_WARP) {
		//ShowError("npc_parse_duplicate: Invalid span format for duplicate warp in file '%s', line '%d'. Skipping line...\n * w1=%s\n * w2=%s\n * w3=%s\n * w4=%s\n", filepath, strline(buffer, start - buffer), w1, w2, w3, w4);
		//return end;// next line, try to continue
		ShowError("buildin_copynpc: Invalid span format for duplicate warp in file '%s'\n * w1=%s\n * w2=%s\n * w3=%s\n * w4=%s\n", filepath, w1, w2, w3, w4);
		script_pushint(st, 0);
		return SCRIPT_CMD_FAILURE;
	}

	// --------------------------------------------------------------------------------------
	// 注意: 这里的 start 和 buffer 最终会在 npc_parsename 和 npc_parseview 中
	// 用于计算出报错代码的所在行数. 但是目前脚本引擎里面, 好像并没有办法知道当前的指令处于那一行,
	// 所以也无法模拟出一个可以提示正确行数的 start 和 buffer 的值用于后面的展现.
	//
	// 我们的原则是尽量少改动 rAthena 的代码, 这里不想大改 npc_parsename 和 npc_parseview 函数.
	// 所以这里不予设置 start 和 buffer 的值, 此做法会让出错的时候无法告诉用户出问题的脚本行数
	// --------------------------------------------------------------------------------------

	nd = npc_create_npc(m, x, y);
	npc_parsename(nd, w3, start, buffer, filepath);
	nd->class_ = m == -1 ? JT_FAKENPC : npc_parseview(w4, start, buffer, filepath);
	nd->speed = 200;
	nd->src_id = src_id;
	nd->type = BL_NPC;
	nd->subtype = (enum npc_subtype)type;
	switch (type) {
	case NPCTYPE_SCRIPT:
		++(*npc_script); // 与 npc_parse_duplicate 不同, 这里是个指针, 需要先解引用后再递增
		nd->u.scr.xs = xs;
		nd->u.scr.ys = ys;
		nd->u.scr.script = dnd->u.scr.script;
		nd->u.scr.label_list = dnd->u.scr.label_list;
		nd->u.scr.label_list_num = dnd->u.scr.label_list_num;
		break;

	case NPCTYPE_SHOP:
	case NPCTYPE_CASHSHOP:
	case NPCTYPE_ITEMSHOP:
	case NPCTYPE_POINTSHOP:
	case NPCTYPE_MARKETSHOP:
		++(*npc_shop); // 与 npc_parse_duplicate 不同, 这里是个指针, 需要先解引用后再递增
		safestrncpy(nd->u.shop.pointshop_str, dnd->u.shop.pointshop_str, strlen(dnd->u.shop.pointshop_str));
		nd->u.shop.itemshop_nameid = dnd->u.shop.itemshop_nameid;
		// 为了避免被复制出来的[子商店]和[来源商店]使用相同的商品道具信息源,
		// 而导致后面对[来源商店]或任意一个[子商店]的道具进行增删操作时影响到同一个[来源商店]的[子商店]
		// 这里在复制商店 NPC 的时候, 将全部的商品列表完整的复制一份出来, 他们之间相互独立
		CREATE(nd->u.shop.shop_item, struct npc_item_list, dnd->u.shop.count);
		memcpy(nd->u.shop.shop_item, dnd->u.shop.shop_item, sizeof(struct npc_item_list) * dnd->u.shop.count);
		nd->u.shop.count = dnd->u.shop.count;
		nd->u.shop.discount = dnd->u.shop.discount;
		safestrncpy(nd->u.shop.pointshop_str_nick, dnd->u.shop.pointshop_str_nick, sizeof(dnd->u.shop.pointshop_str_nick));
		break;

	case NPCTYPE_WARP:
		++(*npc_warp); // 与 npc_parse_duplicate 不同, 这里是个指针, 需要先解引用后再递增
		if (!battle_config.warp_point_debug)
			nd->class_ = JT_WARPNPC;
		else
			nd->class_ = JT_GUILD_FLAG;
		nd->u.warp.xs = xs;
		nd->u.warp.ys = ys;
		nd->u.warp.mapindex = dnd->u.warp.mapindex;
		nd->u.warp.x = dnd->u.warp.x;
		nd->u.warp.y = dnd->u.warp.y;
		nd->trigger_on_hidden = dnd->trigger_on_hidden;
		break;
	}

	//Add the npc to its location
	if (m >= 0) {
		map_addnpc(m, nd);
		unit_dataset(nd);
		nd->ud.dir = (uint8)dir;
		npc_setcells(nd);
		if (map_addblock(nd)) {
			//return end;
			script_pushint(st, 0);
			return SCRIPT_CMD_FAILURE;
		}
		if (nd->class_ != JT_FAKENPC) {
			status_set_viewdata(nd, nd->class_);
			if (map_getmapdata(nd->m)->users)
				clif_spawn(nd);
		}
	}
	else {
		// we skip map_addnpc, but still add it to the list of ID's
		map_addiddb(nd);
	}
	strdb_put(npcname_db, nd->exname, nd);

	if (type != NPCTYPE_SCRIPT) {
		//return end;
		script_pushint(st, nd->id);
		return SCRIPT_CMD_SUCCESS;
	}

	//-----------------------------------------
	// Loop through labels to export them as necessary
	for (i = 0; i < nd->u.scr.label_list_num; i++) {
		if (npc_event_export(nd, i)) {
// 			ShowWarning("npc_parse_duplicate : duplicate event %s::%s (%s)\n",
// 				nd->exname, nd->u.scr.label_list[i].name, filepath);
			ShowWarning("buildin_copynpc: duplicate event %s::%s (%s)\n", nd->exname, nd->u.scr.label_list[i].name, filepath);
		}
		npc_timerevent_export(nd, i);
	}

	//if (!strcmp(filepath, "INSTANCING")) //Instance NPCs will use this for commands
	//	nd->instance_id = mapdata->instance_id;

	// 如果复制出来的 NPC 出现在一个副本地图中, 那么该 NPC 应隶属该副本
	if (mapdata->instance_id) {
		nd->instance_id = mapdata->instance_id;
	}

	nd->u.scr.timerid = INVALID_TIMER;

	//return end;
	script_pushint(st, nd->id);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: gettimefmt
 * 描述: 将当前时间格式化输出成字符串, 是 gettimestr 的改进版
 * 用法: gettimefmt <"时间格式化标准">{,<要转换的秒数>{,<是否格式化成 UTC 时间>}};
 * 返回: 成功则返回被格式化的时间, 失败则返回空字符串
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(gettimefmt) {
	const char *fmtstr = script_getstr(st, 2);
	int is_utc = 0, default_len = 32;
	size_t result = 0;
	time_t time_tick = time(NULL);

	if (script_hasdata(st, 3)) {
		if (!script_isint(st, 3)) {
			ShowError("buildin_gettimefmt: The 'time_tick' param must be a integer.\n");
			script_pushconststr(st, "");
			return SCRIPT_CMD_FAILURE;
		}
		time_tick = (time_t)script_getnum(st, 3);
	}

	if (script_hasdata(st, 4)) {
		if (!script_isint(st, 4)) {
			ShowError("buildin_gettimefmt: The 'is_utc' param must be a integer.\n");
			script_pushconststr(st, "");
			return SCRIPT_CMD_FAILURE;
		}
		is_utc = cap_value(script_getnum(st, 4), 0, 1);
	}

	struct tm *now_time = (is_utc ? gmtime(&time_tick) : localtime(&time_tick));
	char* buf = (char *)aMalloc(default_len + 1);
	result = strftime(buf, default_len, fmtstr, now_time);

	if (!result) {
		buf = (char *)aRealloc(buf, (default_len * 4) + 1);
		result = strftime(buf, (default_len * 4), fmtstr, now_time);
	}

	if (!result) {
		if (buf) aFree(buf);
		ShowError("buildin_gettimefmt: time format is too long : %s\n", fmtstr);
		script_pushconststr(st, "");
		return SCRIPT_CMD_FAILURE;
	}

	script_pushstr(st, buf);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: multicatchpet
 * 描述: 与 catchpet 指令类似, 但可以指定更多支持捕捉的魔物编号
 * 用法: multicatchpet <魔物编号>{,<魔物编号>...};
 * 返回: 该指令无论成功失败, 都不会有返回值
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(multicatchpet) {
	map_session_data* sd = nullptr;
	if (!script_rid2sd(sd))
		return SCRIPT_CMD_SUCCESS;
	sd->pandas_pet_catch_targets.clear();
	for (int32 i = 2; script_hasdata(st, i); ++i) {
		const uint32 mob_id = script_getnum(st, i);
		if (pet_db.find(mob_id) != nullptr)
			sd->pandas_pet_catch_targets.push_back(mob_id);
	}
	if (sd->pandas_pet_catch_targets.empty())
		return SCRIPT_CMD_FAILURE;
	pet_catch_process_start(*sd, 0, PET_CATCH_MULTI_TARGET);
	return SCRIPT_CMD_SUCCESS;
}


//************************************
// Method:      selfdeletion_exec_endtalk
// Description: 当一个玩家结束与某 NPC 的交互时, 检查是否需要自毁此 NPC
// Parameter:   struct script_state * st
// Returns:     void
// Author:      Sola丶小克(CairoLee)  2019/11/25 01:04
//************************************
void selfdeletion_exec_endtalk(struct script_state* st) {
	if (!st->oid) return;

	TBL_NPC* nd = map_id2nd(st->oid);
	if (!nd || nd->pandas.destruction_strategy != 1) return;

	nd->pandas.destruction_timer = add_timer(
		gettick() + 500, selfdeletion_timer, st->oid, st->rid
	);
}

//************************************
// Method:      selfdeletion_timer
// Description: 配合 selfdeletion 指令使用的计时器
// Author:      Sola丶小克(CairoLee)  2019/11/25 01:03
//************************************
TIMER_FUNC(selfdeletion_timer) {
	TBL_NPC* nd = map_id2nd(id);
	if (!nd) return 1;

	if (nd->pandas.destruction_timer != INVALID_TIMER) {
		delete_timer(nd->pandas.destruction_timer, selfdeletion_timer);
		nd->pandas.destruction_timer = INVALID_TIMER;
	}

	if (nd->pandas.destruction_strategy == 1) {
		// 执行结束对话后是否自毁的检测
		// 若当前没有其他玩家正在与此 NPC 交互则自动卸载此 NPC
		struct s_mapiterator* iter = nullptr;
		block_list* bl = nullptr;
		iter = mapit_geteachpc();
		bool interacting = false;

		for (bl = mapit_first(iter); mapit_exists(iter); bl = mapit_next(iter)) {
			if (!bl || bl->m != nd->m) continue;

			struct script_state* bl_st = ((TBL_PC*)bl)->st;
			if (bl_st == nullptr) continue;

			if (bl_st->oid == id) {
				interacting = true;
				break;
			}
		}
		if (iter) mapit_free(iter);

		if (!interacting) {
			npc_unload_duplicates(nd);
			npc_unload(nd, true);
			npc_read_event_script();
		}
	}
	else {
		npc_unload_duplicates(nd);
		npc_unload(nd, true);
		npc_read_event_script();
	}

	return 1;
}

/* ===========================================================
 * 指令: selfdeletion
 * 描述: 设置 NPC 的自毁策略
 * 用法: selfdeletion <自毁策略>;
 * 返回: 该指令无论成功失败, 都不会有返回值
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(selfdeletion) {
	TBL_PC* sd = map_id2sd(st->rid);
	int option = (script_hasdata(st, 2) ? script_getnum(st, 2) : SELFDEL_NOW);

	// SELFDEL_NOW = 立刻终止全部与此 NPC 相关的玩家对话, 并立刻自毁
	// SELFDEL_WAITFREE = 与最后一个与玩家的交互结束后自毁
	// SELFDEL_CANCEL = 取消与最后一个与玩家的交互结束后自毁

	TBL_NPC* nd = map_id2nd(st->oid);
	bool immediately = (option == SELFDEL_NOW);

	if (sd && sd->state.using_fake_npc) {
		return SCRIPT_CMD_SUCCESS;
	}

	if (nd->id == fake_nd->id) {
		return SCRIPT_CMD_SUCCESS;
	}

	if (!immediately) {
		nd->pandas.destruction_strategy = (option == SELFDEL_WAITFREE ? 1 : 0);
		return SCRIPT_CMD_SUCCESS;
	}

	// 能执行到这里表示期望立刻销毁当前的 NPC
	// 先看看当前地图有多少人正在和此 NPC 对话, 且其 st 状态的 mes_active 为 1 的
	// 若发现了则先送此玩家一个关闭按钮, 并将他和此 NPC 的脚本流设置为 END
	struct s_mapiterator* iter = nullptr;
	block_list* bl = nullptr;
	iter = mapit_geteachpc();

	for (bl = mapit_first(iter); mapit_exists(iter); bl = mapit_next(iter)) {
		if (!bl || bl->m != nd->m) continue;
		if (bl->id == st->rid) continue; // 不结束当前对话

		struct script_state* bl_st = ((TBL_PC*)bl)->st;
		if (bl_st == nullptr || bl_st->oid != st->oid) continue;

		pc_close_npc(((TBL_PC*)bl), 1 | 4);
	}
	if (iter) mapit_free(iter);

	// 其他正在与当前 NPC 发生对话的角色, 至此已经全部发送了关闭按钮并终止了后续脚本执行
	// 接下来设置一个定时器, 时间到了把当前 NPC 直接 unload 掉
	nd->pandas.destruction_timer = add_timer(
		gettick() + 500, selfdeletion_timer, st->oid, st->rid
	);

	if (sd) {
		// 关闭当前 NPC 和当前玩家的对话, 若存在对话框则送一个关闭按钮
		pc_close_npc(sd, 1 | 4);
	}

	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: setchartitle
 * 描述: 设置指定玩家的称号ID
 * 用法: setchartitle <称号ID>{,<角色编号>};
 * 返回: 设置成功则返回 1, 设置失败则返回 0
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(setchartitle) {
#if PACKETVER < 20150513
	ShowWarning("%s: Title System it requires PACKETVER 2015-05-13 or newer...\n", __func__);
	script_pushint(st, 0);
	return SCRIPT_CMD_FAILURE;
#endif

	TBL_PC* sd = nullptr;
	uint32 title_id = max(script_getnum(st, 2), 0);

	if (!script_charid2sd(3, sd)) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	script_pushint(st, (npc_change_title_event(sd, title_id, 1) ? 1 : 0));
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: getchartitle
 * 描述: 获得指定玩家的称号ID
 * 用法: getchartitle {<角色编号>};
 * 返回: 返回玩家的称号ID (若为 0 则表示此玩家没有称号), 获取失败则返回 -1
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(getchartitle) {
#if PACKETVER < 20150513
	ShowWarning("%s: Title System it requires PACKETVER 2015-05-13 or newer...\n", __func__);
	script_pushint(st, -1);
	return SCRIPT_CMD_FAILURE;
#endif

	TBL_PC* sd = nullptr;

	if (!script_charid2sd(2, sd)) {
		script_pushint(st, -1);
		return SCRIPT_CMD_SUCCESS;
	}

	script_pushint(st, sd->status.title_id);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: npcexists
 * 描述: 判断指定名称的 NPC 是否存在, 就算不存在控制台也不会报错
 * 用法: npcexists "<NPC名称>"{,<用于保存 GameID 的变量>};
 * 返回: 存在返回 1, 不存在返回 0
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(npcexists) {
	struct script_data* vardata = nullptr;
	TBL_PC* sd = nullptr;
	int64 num = 0;
	const char* name = nullptr;

	if (script_hasdata(st, 3)) {
		vardata = script_getdata(st, 3);

		if (!data_isreference(vardata)) {
			ShowWarning("%s: GameID value is not a variable.\n", __func__);
			script_reportdata(vardata);
			script_pushint(st, 0);
			return SCRIPT_CMD_FAILURE;
		}

		if (is_string_variable(reference_getname(vardata))) {
			ShowWarning("%s: %s is not a integer variable.\n", __func__, reference_getname(vardata));
			script_reportdata(vardata);
			script_pushint(st, 0);
			return SCRIPT_CMD_FAILURE;
		}

		num = reference_getuid(vardata);
		name = reference_getname(vardata);
		char prefix = *name;

		if (not_server_variable(prefix) && !script_rid2sd(sd)) {
			ShowError("%s: variable '%s' for GameID is not a server variable, but no player is attached!\n", __func__, name);
			script_pushint(st, 0);
			return SCRIPT_CMD_FAILURE;
		}

		// 将用于保存 GameID 的变量内容设置为 0
		clear_reg(st, sd, num, name, script_getref(st, 3));
	}

	TBL_NPC* nd = nullptr;
	if ((nd = npc_name2id(script_getstr(st, 2))) != nullptr) {
		script_pushint(st, 1);
		if (vardata != nullptr) {
			set_reg_num(st, sd, num, name, nd->id, script_getref(st, 3));
		}
	}
	else {
		script_pushint(st, 0);
	}

	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: storagegetitem
 * 描述: 往仓库直接创造一个指定的道具
 * 用法: storagegetitem <物品编号>,<数量>{,<账号编号>};
 * 用法: storagegetitem "<物品名称>",<数量>{,<账号编号>};
 * 返回: 添加成功会返回 0, 返回小于 0 则表示有错误
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(storagegetitem) {
	int get_count = 0, i = 0;
	t_itemid nameid = 0;
	unsigned short amount = 0;
	struct item it = { 0 };
	TBL_PC* sd = nullptr;
	unsigned char flag = 0;
	std::shared_ptr<item_data> id;

	if (script_isstring(st, 2)) {// "<item name>"
		const char* name = script_getstr(st, 2);

		id = item_db.searchname(name);

		if (id == nullptr) {
			ShowError("buildin_storagegetitem: Nonexistant item %s requested.\n", name);
			script_pushint(st, -1);
			return SCRIPT_CMD_SUCCESS; //No item created.
		}
		nameid = id->nameid;
	}
	else {// <item id>
		nameid = script_getnum(st, 2);

		id = item_db.find(nameid);

		if (id == nullptr) {
			ShowError("buildin_storagegetitem: Nonexistant item %u requested.\n", nameid);
			script_pushint(st, -1);
			return SCRIPT_CMD_SUCCESS; //No item created.
		}
	}

	// <amount>
	if ((amount = script_getnum(st, 3)) <= 0) {
		script_pushint(st, -2);
		return SCRIPT_CMD_SUCCESS; //return if amount <=0, skip the useles iteration
	}

	memset(&it, 0, sizeof(it));
	it.nameid = nameid;
	it.identify = 1;
	it.bound = BOUND_NONE;

	if (!strcmp(script_getfuncname(st), "storagegetitembound")) {
		char bound = script_getnum(st, 4);
		if (bound < BOUND_NONE || bound >= BOUND_MAX) {
			ShowError("script_storagegetitembound: Not a correct bound type! Type=%d\n", bound);
			script_pushint(st, -3);
			return SCRIPT_CMD_SUCCESS;
		}
		script_mapid2sd(5, sd);
		it.bound = bound;
	}
	else {
		script_mapid2sd(4, sd);
	}

	if (sd == NULL) {
		script_pushint(st, -4);
		return SCRIPT_CMD_SUCCESS;
	}

	if (sd->state.storage_flag == 1 || sd->state.storage_flag == 3) {
		script_pushint(st, -5);
		return SCRIPT_CMD_SUCCESS;
	}

	//Check if it's stackable.
	if (!itemdb_isstackable2(id.get()))
		get_count = 1;
	else
		get_count = amount;

	if (!itemdb_canstore(&it, pc_get_group_level(sd))) {
		script_pushint(st, -6);
		return SCRIPT_CMD_SUCCESS;
	}

	if (pet_db_search(nameid, PET_EGG)) {
		script_pushint(st, -7);
		return SCRIPT_CMD_SUCCESS;
	}

	if (!sd->storage.state.put) {
		script_pushint(st, -8);
		return SCRIPT_CMD_SUCCESS;
	}

	for (i = 0; i < amount; i += get_count) {
		if (storage_additem(sd, &sd->storage, &it, get_count)) {
			if (pc_candrop(sd, &it))
				map_addflooritem(&it, get_count, sd->m, sd->x, sd->y, 0, 0, 0, 0, 0);
			else
				script_pushint(st, -9);
		}
	}

	script_pushint(st, 0);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: setinventoryinfo
 * 描述: 设置指定背包序号道具的部分详细信息, 与 getinventoryinfo 对应
 * 用法: setinventoryinfo <道具的背包序号>,<要设置的信息类型>,<值>{{,<标记位>},<角色编号>};
 * 返回: 设置成功则返回 1, 设置失败或角色不存在则返回 0, 返回负数也是失败 (值表示不同失败原因)
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(setinventoryinfo) {
	map_session_data* sd = nullptr;
	int expire_tick = 0, flag = 0;
	int idx = script_getnum(st, 2);
	int type = script_getnum(st, 3);
	uint64 value = script_getnum(st, 4);
	bool need_recalc_status = false;

	if (!script_charid2sd(6, sd)) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	if (idx < 0 || idx >= sd->status.inventory_slots || !sd->inventory_data[idx]) {
		ShowError("buildin_setinventoryinfo: Nonexistant item index.\n");
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	if (script_hasdata(st, 5) && script_isint(st, 5)) {
		//   0 : 维持默认行为
		// & 1 : 跳过角色能力重算
		// & 2 : 跳过全量刷新角色的背包数据
		// & 4 : 忽略卡片插入合理性的校验(必须是卡片, 卡片位置和装备位置符合)
		// & 8 : 插入卡片无需写入 picklog 记录(凭空生成)
		flag = script_getnum(st, 5);
	}

	switch (type)
	{
	case 3:
		value = cap_value(value, 0, MAX_REFINE);
		sd->inventory.u.items_inventory[idx].refine = (char)value;
		need_recalc_status = true;
		break;
	case 4:
		value = cap_value(value, 0, 1);
		sd->inventory.u.items_inventory[idx].identify = (char)value;
		if (sd->inventory.u.items_inventory[idx].equip && !value)
			pc_unequipitem(sd, idx, 3);
		break;
	case 5:
		value = cap_value(value, 0, 1);
		sd->inventory.u.items_inventory[idx].attribute = (char)value;
		if (sd->inventory.u.items_inventory[idx].equip && value)
			pc_unequipitem(sd, idx, 3);
		break;
	case 6: case 7: case 8: case 9: {
		if (!value) {
			sd->inventory.u.items_inventory[idx].card[type - 6] = 0;
			need_recalc_status = true;
			break;
		}

		std::shared_ptr<item_data> card_itemdata = item_db.find((t_itemid)value);
		if (!card_itemdata) {
			ShowWarning("buildin_setinventoryinfo: Nonexistant item : %u.\n", value);
			script_pushint(st, 0);
			return SCRIPT_CMD_SUCCESS;
		}
		if (!(flag & 4)) {
			if (card_itemdata->type != IT_CARD || (sd->inventory_data[idx]->equip & card_itemdata->equip) == 0) {
				script_pushint(st, -1);
				return SCRIPT_CMD_SUCCESS;
			}
		}
		sd->inventory.u.items_inventory[idx].card[type - 6] = card_itemdata->nameid;
		if (!(flag & 8)) {
			struct item item_tmp = { 0 };
			item_tmp.nameid = card_itemdata->nameid;
			item_tmp.identify = 1;
			if (card_itemdata->flag.guid && !item_tmp.unique_id)
				item_tmp.unique_id = pc_generate_unique_id(sd);
			log_pick_pc(sd, LOG_TYPE_SCRIPT, 1, &item_tmp);
		}
		need_recalc_status = true;
		break;
	}
	case 10:
		value = cap_value(value, 0, INT_MAX);
		sd->inventory.u.items_inventory[idx].expire_time = (unsigned int)value; // Timestamp, not seconds
		expire_tick = (unsigned int)(sd->inventory.u.items_inventory[idx].expire_time - time(NULL));
		if (expire_tick > 0) {
			unsigned int seconds = (unsigned int)(value - time(NULL));
			clif_rental_time(sd, sd->inventory.u.items_inventory[idx].nameid, seconds);
			pc_inventory_rental_add(sd, seconds);
		}
		else {
			inventory_rental_update(sd);
		}
		need_recalc_status = true;
		break;
	case 11:
		sd->inventory.u.items_inventory[idx].unique_id = value;
		break;
	case 12: case 13: case 14: case 15: case 16:
		if (value) {
			sd->inventory.u.items_inventory[idx].option[type - 12].id = (short)value;
		}
		else {
			sd->inventory.u.items_inventory[idx].option[type - 12].id = 0;
			sd->inventory.u.items_inventory[idx].option[type - 12].value = 0;
			sd->inventory.u.items_inventory[idx].option[type - 12].param = 0;
		}
		need_recalc_status = true;
		break;
	case 17: case 18: case 19: case 20: case 21:
		sd->inventory.u.items_inventory[idx].option[type - 17].value = (short)value;
		need_recalc_status = true;
		break;
	case 22: case 23: case 24: case 25: case 26:
		sd->inventory.u.items_inventory[idx].option[type - 22].param = (char)value;
		need_recalc_status = true;
		break;
	case 27:
		if (value < BOUND_NONE || value >= BOUND_MAX) {
			ShowWarning("buildin_setinventoryinfo: Invalid bound type\n");
			script_pushint(st, 0);
			return SCRIPT_CMD_SUCCESS;
		}
		sd->inventory.u.items_inventory[idx].bound = (char)value;
		break;
	case 28:
		if (value < 0 || value > MAX_ENCHANTGRADE) {
			ShowWarning("buildin_setinventoryinfo: The Value should be in range 0-%d, but you passed %d.\n", MAX_ENCHANTGRADE, value);
			script_pushint(st, 0);
			return SCRIPT_CMD_SUCCESS;
		}
		sd->inventory.u.items_inventory[idx].enchantgrade = (uint8)value;
		need_recalc_status = true;
		break;
	case 29:
		sd->inventory.u.items_inventory[idx].equipSwitch = (unsigned int)value;
		break;
	case 30:
		value = cap_value(value, 0, 1);
		sd->inventory.u.items_inventory[idx].favorite = (char)value;
		break;
	default:
		ShowWarning("buildin_setinventoryinfo: The type should be in range 3-%d, currently type is: %d.\n", 30, type);
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	if (need_recalc_status && !(flag & 1))
		status_calc_pc(sd, SCO_NONE);

	if (!(flag & 2))
		clif_inventorylist(sd);

	script_pushint(st, 1);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: updateinventory
 * 描述: 重新下发关联玩家的背包数据给客户端 (刷新客户端背包数据)
 * 用法: updateinventory {<角色编号>};
 * 返回: 返回 1 表示成功, 0 表示失败
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(updateinventory) {
	map_session_data* sd = nullptr;
	if (!script_charid2sd(2, sd)) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	clif_inventorylist(sd);
	script_pushint(st, 1);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: getcharmac
 * 描述: 获取指定角色登录时使用的 MAC 地址
 * 用法: getcharmac(<账户编号>/<角色编号>/<"角色名称">);
 * 返回: 成功则返回 MAC 地址, 失败则返回空字符串
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(getcharmac) {
	// rAthena 原生登录协议不采集客户端 MAC；保持兼容指令并返回空字符串。
	script_pushconststr(st, "");
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: getconstant
 * 描述: 查询一个常量字符串对应的数值
 * 用法: getconstant <"常量字符串">;
 * 返回: 成功则返回常量对应的数值, 查询失败则返回 -255
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(getconstant) {
	const char* name = script_getstr(st, 2);
	int64 value = 0;

	if (script_get_constant(name, &value)) {
		script_pushint(st, value);
	}
	else {
		script_pushint(st, -255);
	}

	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: preg_search
 * 描述: 使用正则表达式搜索并返回首个匹配的分组内容
 * 用法: preg_search <"字符串">,<"匹配表达式">,<拓展标记位>,<存放匹配结果的字符串数组>;
 * 返回: 返回负数表示错误, 其他正整数表示匹配到的分组个数
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(preg_search) {
	map_session_data* sd = nullptr;
	std::string text = std::string(script_getstr(st, 2));
	std::string patterns = std::string(script_getstr(st, 3));
	int flag = cap_value(script_getnum(st, 4), 0, 1);
	struct script_data* retdata = script_getdata(st, 5);
	struct reg_db* src_reg_db = nullptr;

	if (!data_isreference(retdata)) {
		ShowError("buildin_preg_search: error argument! please give a array variable for save gameid.\n");
		script_reportdata(retdata);
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;// not a variable
	}

	int retid = reference_getid(retdata);
	const char* retname = reference_getname(retdata);

	if (not_server_variable(*retname)) {
		if (!script_rid2sd(sd)) {
			ShowError("buildin_preg_search: '%s' is not server variable, please attach to a player.\n", retname);
			script_reportdata(retdata);
			script_pushint(st, -1);
			return SCRIPT_CMD_FAILURE;
		}
	}

	if (!(src_reg_db = script_array_src(st, sd, retname, reference_getref(retdata)))) {
		ShowError("buildin_preg_search: variable '%s' is not a array.\n", retname);
		script_reportdata(retdata);
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;
	}

	if (!is_string_variable(retname)) {
		ShowError("buildin_preg_search: the array variable '%s' must be string type.\n", retname);
		script_reportdata(retdata);
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;
	}

	// 以下用于清空存放返回数据的字符串数组 (参考 script_cleararray_pc 的实现)
	script_array_ensure_zero(st, NULL, retdata->u.num, reference_getref(retdata));
	struct script_array* sa = static_cast<script_array*>(idb_get(src_reg_db->arrays, retid));
	unsigned int array_len = script_array_highest_key(st, sd, retname, reference_getref(retdata));

	if (sa) {
		// 若给定的数组是存在的, 那么需要清空一下
		unsigned int* list = script_array_cpy_list(sa);
		unsigned int size = sa->size;

		for (unsigned int i = 0; i < size; i++) {
			clear_reg(st, sd, reference_uid(retid, list[i]), retname, reference_getref(retdata));
		}
	}

	try
	{
		std::regex re;
		if (flag & 1) {
			re = std::regex(patterns, std::regex::icase);
		}
		else {
			re = std::regex(patterns);
		}

		std::smatch match_result;

		if (!std::regex_search(text, match_result, re)) {
			script_pushint(st, -1);
			return SCRIPT_CMD_SUCCESS;
		}

		for (int i = 0; i < match_result.size(); i++) {
			int64 uid = reference_uid(retid, i);
			set_reg_str(st, sd, uid, retname, match_result[i].str().c_str(), reference_getref(retdata));
		}

		script_pushint(st, match_result.size());
	}
	catch (const std::regex_error& e)
	{
		ShowError("%s: throw regex_error : %s\n", __func__, e.what());
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;
	}

	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: aura
 * 描述: 激活指定的光环组合
 * 用法: aura <光环编号>{,<角色编号>};
 * 返回: 成功返回 1 失败返回 0
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(aura) {
	map_session_data* sd = nullptr;
	if (!script_charid2sd(3, sd)) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}
	const int32 effect_id = script_getnum(st, 2);
	sd->ud.aura_id = effect_id;
	if (effect_id > 0)
		clif_specialeffect(sd, effect_id, AREA);
	script_pushint(st, 1);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: unitaura
 * 描述: 用于调整七种单位的光环组合 (但仅 BL_PC 会被持久化)
 * 用法: unitaura <单位编号>,<光环编号>;
 * 返回: 成功返回 1 失败返回 0
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(unitaura) {
	block_list* bl = map_id2bl(script_getnum(st, 2));
	if (bl == nullptr) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}
	const int32 effect_id = script_getnum(st, 3);
	unit_data* ud = unit_bl2ud(bl);
	if (ud == nullptr) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}
	ud->aura_id = effect_id;
	if (effect_id > 0)
		clif_specialeffect(bl, effect_id, AREA);
	script_pushint(st, 1);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: getunittarget
 * 描述: 该指令用于获取指定单位当前正在攻击的目标单位编号
 * 用法: getunittarget <游戏单位编号>;
 * 返回: 目标的 GameID, 返回 0 表示没有目标
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(getunittarget) {
	block_list* bl = nullptr;
	bl = map_id2bl(script_getnum(st, 2));

	if (!bl) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	switch (bl->type) {
	case BL_PC:  script_pushint(st, ((TBL_PC*)bl)->ud.target); break;
	case BL_MOB: script_pushint(st, ((TBL_MOB*)bl)->target_id); break;
	case BL_NPC: script_pushint(st, ((TBL_NPC*)bl)->ud.target); break;
	case BL_HOM: script_pushint(st, ((TBL_HOM*)bl)->ud.target); break;
	case BL_MER: script_pushint(st, ((TBL_MER*)bl)->ud.target); break;
	case BL_PET: script_pushint(st, ((TBL_PET*)bl)->target_id); break;
	case BL_ELEM: script_pushint(st, ((TBL_ELEM*)bl)->ud.target); break;
	default:
		script_pushint(st, 0);
		break;
	}

	return SCRIPT_CMD_SUCCESS;
}


/* ===========================================================
 * 指令: batrec_query
 * 描述: 查询指定单位的战斗记录, 查看与交互目标单位产生的具体记录值
 * 用法: batrec_query <记录宿主的单位编号>,<交互目标的单位编号>,<记录类型>{,<聚合规则>};
 * 返回: 返回 -1 表示查无记录或发生错误, 含 0 正整数表示伤害值
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
struct s_pandas_batrec {
	bool enabled = false;
	std::unordered_map<int32, int64> receive;
	std::unordered_map<int32, int64> cause;
};
static std::unordered_map<int32, s_pandas_batrec> pandas_batrec_db;

void pandas_batrec_record(block_list* src, block_list* target, int64 damage) {
	if (src == nullptr || target == nullptr)
		return;
	auto src_it = pandas_batrec_db.find(src->id);
	if (src_it != pandas_batrec_db.end() && src_it->second.enabled)
		src_it->second.cause[target->id] += damage;
	auto target_it = pandas_batrec_db.find(target->id);
	if (target_it != pandas_batrec_db.end() && target_it->second.enabled)
		target_it->second.receive[src->id] += damage;
}

static void script_pandas_clear_array(script_state* st, script_data* data, map_session_data* sd) {
	if (!data_isreference(data))
		return;
	const char* name = reference_getname(data);
	reg_db* src_db = script_array_src(st, sd, name, reference_getref(data));
	if (src_db == nullptr)
		return;
	script_array* array = static_cast<script_array*>(idb_get(src_db->arrays, reference_getid(data)));
	if (array == nullptr)
		return;
	uint32* indexes = script_array_cpy_list(array);
	for (uint32 i = 0; i < array->size; ++i)
		clear_reg(st, sd, reference_uid(reference_getid(data), indexes[i]), name, reference_getref(data));
}

BUILDIN_FUNC(batrec_query) {
	const int32 host_id = script_getnum(st, 2);
	const int32 peer_id = script_getnum(st, 3);
	const int32 type = script_getnum(st, 4);
	auto host = pandas_batrec_db.find(host_id);
	if (host == pandas_batrec_db.end() || (type != BRT_DMG_RECEIVE && type != BRT_DMG_CAUSE)) {
		script_pushint(st, -1);
		return SCRIPT_CMD_SUCCESS;
	}
	const auto& values = type == BRT_DMG_RECEIVE ? host->second.receive : host->second.cause;
	auto value = values.find(peer_id);
	script_pushint(st, value == values.end() ? -1 : value->second);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: batrec_rank
 * 描述: 查询指定单位的战斗记录并对记录的值进行排序, 返回排行榜单
 * 用法: batrec_rank <记录宿主的单位编号>,<返回交互目标的单位编号数组>,<返回记录值数组>,<记录类型>{,<聚合规则>{,<排序规则>}};
 * 返回: 失败返回 -1, 含 0 正整数表示数组中返回的榜单记录数
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(batrec_rank) {
	const int32 host_id = script_getnum(st, 2);
	script_data* id_array = script_getdata(st, 3);
	script_data* value_array = script_getdata(st, 4);
	const int32 type = script_getnum(st, 5);
	const int32 sort_type = script_hasdata(st, 7) ? script_getnum(st, 7) : BRS_DESC;
	map_session_data* sd = map_id2sd(st->rid);
	if (!data_isreference(id_array) || !data_isreference(value_array)) {
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;
	}
	script_pandas_clear_array(st, id_array, sd);
	script_pandas_clear_array(st, value_array, sd);
	auto host = pandas_batrec_db.find(host_id);
	if (host == pandas_batrec_db.end() || (type != BRT_DMG_RECEIVE && type != BRT_DMG_CAUSE)) {
		script_pushint(st, -1);
		return SCRIPT_CMD_SUCCESS;
	}
	const auto& values = type == BRT_DMG_RECEIVE ? host->second.receive : host->second.cause;
	std::vector<std::pair<int32, int64>> ranking(values.begin(), values.end());
	std::sort(ranking.begin(), ranking.end(), [sort_type](const auto& left, const auto& right) {
		return sort_type == BRS_ASC ? left.second < right.second : left.second > right.second;
	});
	for (size_t i = 0; i < ranking.size(); ++i) {
		set_reg_num(st, sd, reference_uid(reference_getid(id_array), i), reference_getname(id_array), ranking[i].first, reference_getref(id_array));
		set_reg_num(st, sd, reference_uid(reference_getid(value_array), i), reference_getname(value_array), ranking[i].second, reference_getref(value_array));
	}
	script_pushint(st, ranking.size());
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: batrec_sortout
 * 描述: 移除指定单位的战斗记录中交互单位已经不存在 (或下线) 的记录
 * 用法: batrec_sortout <记录宿主的单位编号>{,<记录类型>};
 * 返回: 该指令无论成功与否, 都不会有返回值
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(batrec_sortout) {
	auto host = pandas_batrec_db.find(script_getnum(st, 2));
	if (host == pandas_batrec_db.end())
		return SCRIPT_CMD_SUCCESS;
	auto remove_missing = [](auto& values) {
		for (auto it = values.begin(); it != values.end();)
			it = map_id2bl(it->first) == nullptr ? values.erase(it) : std::next(it);
	};
	if (!script_hasdata(st, 3) || script_getnum(st, 3) == BRT_DMG_RECEIVE)
		remove_missing(host->second.receive);
	if (!script_hasdata(st, 3) || script_getnum(st, 3) == BRT_DMG_CAUSE)
		remove_missing(host->second.cause);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: batrec_reset
 * 描述: 清除指定单位的战斗记录
 * 用法: batrec_reset <记录宿主的单位编号>;
 * 返回: 该指令无论成功与否, 都不会有返回值
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(batrec_reset) {
	pandas_batrec_db.erase(script_getnum(st, 2));
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: enable_batrec
 * 描述: 启用指定单位的战斗记录
 * 用法: enable_batrec {<游戏单位编号>};
 * 返回: 该指令无论成功与否, 都不会有返回值
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(enable_batrec) {
	const int32 unit_id = script_hasdata(st, 2) ? script_getnum(st, 2) : st->rid;
	if (map_id2bl(unit_id) != nullptr)
		pandas_batrec_db[unit_id].enabled = true;
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: disable_batrec
 * 描述: 禁用指定单位的战斗记录
 * 用法: disable_batrec {<游戏单位编号>};
 * 返回: 该指令无论成功与否, 都不会有返回值
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(disable_batrec) {
	const int32 unit_id = script_hasdata(st, 2) ? script_getnum(st, 2) : st->rid;
	auto record = pandas_batrec_db.find(unit_id);
	if (record != pandas_batrec_db.end())
		record->second.enabled = false;
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: login
 * 描述: 将指定的角色以特定的登录模式拉上线
 * 用法: login <角色编号>{,<默认是否坐下>{,<默认身体朝向>{,<默认脑袋朝向>{,<登录模式>}}}};
 * 返回: 成功返回 1, 失败返回 0
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(login) {
	map_session_data* sd = map_charid2sd(script_getnum(st, 2));
	if (sd == nullptr) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}
	const bool sit = script_hasdata(st, 3) && script_getnum(st, 3) != 0;
	const uint8 body_dir = cap_value(script_hasdata(st, 4) ? script_getnum(st, 4) : DIR_SOUTH, 0, 7);
	const uint8 head_dir = cap_value(script_hasdata(st, 5) ? script_getnum(st, 5) : 0, 0, 2);
	pc_setdir(sd, body_dir, head_dir);
	if (sit) {
		pc_setsit(sd);
	} else {
		pc_setstand(sd, true);
	}
	clif_changed_dir(*sd, AREA_WOS);
	clif_changed_dir(*sd, SELF);
	script_pushint(st, 1);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: checksuspend
 * 描述: 获取指定角色或指定账号当前在线角色的挂机模式
 * 用法: checksuspend {<角色编号|账号编号|"角色名称">};
 * 返回: 角色不存在返回 -1, 否则返回当前的挂机状态
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(checksuspend) {
	map_session_data* sd = nullptr;
	if (script_hasdata(st, 2)) {
		if (script_isstring(st, 2))
			sd = map_nick2sd(script_getstr(st, 2), false);
		else {
			const int32 id = script_getnum(st, 2);
			sd = map_id2sd(id);
			if (sd == nullptr)
				sd = map_charid2sd(id);
		}
	} else {
		sd = map_id2sd(st->rid);
	}
	script_pushint(st, sd == nullptr ? -1 : (sd->state.autotrade ? 1 : 0));
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: bonus_script_remove
 * 描述: 移除指定的 bonus_script 效果脚本
 * 用法: bonus_script_remove <效果脚本编号>{,<角色编号>};
 * 返回: 成功移除则返回 true, 找不到脚本代码或移除失败则返回 false
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(bonus_script_remove) {
	TBL_PC* sd = nullptr;
	if (!script_charid2sd(3, sd)) {
		script_pushint(st, false);
		return SCRIPT_CMD_FAILURE;
	}

	uint64 bonus_id = script_getnum64(st, 2);
	if (pc_bonus_script_remove(sd, bonus_id))
		script_pushint(st, true);
	else
		script_pushint(st, false);

	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: bonus_script_list
 * 描述: 用于获取指定角色当前激活的全部 bonus_script 效果脚本编号
 * 用法: bonus_script_list <返回效果脚本编号的数值型数组>{,<角色编号>};
 * 返回: 获取到的脚本代码数量, 发生错误则返回 -1
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(bonus_script_list) {
	TBL_PC* sd = nullptr;
	if (!script_charid2sd(3, sd)) {
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;
	}

	int script_array_varid = 0;
	char* script_array_varname = nullptr;
	struct script_data* script_array_vardata = nullptr;
	if (!script_get_array(st, 2, script_array_varid, script_array_varname, script_array_vardata)) {
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;
	}
	script_cleararray_st(st, 2);

	uint16 count = 0;
	struct linkdb_node* node = NULL;
	struct s_bonus_script_entry* entry = NULL;
	if ((node = sd->bonus_script.head)) {
		while (node) {
			struct linkdb_node* next = node->next;
			entry = (struct s_bonus_script_entry*)node->data;
			if (!entry) continue;

			int64 uid = reference_uid(script_array_varid, count);
			set_reg_num(st, sd, uid, script_array_varname, entry->bonus_id, reference_getref(script_array_vardata));
			count++;
			node = next;
		}
	}

	script_pushint(st, count);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: bonus_script_exists
 * 描述: 查询指定角色是否已经激活了特定的 bonus_script 效果脚本
 * 用法: bonus_script_exists <效果脚本编号>{,<角色编号>};
 * 返回: 效果已经存在则返回 true, 角色不在线或效果不存在否则返回 false
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(bonus_script_exists) {
	TBL_PC* sd = nullptr;
	if (!script_charid2sd(3, sd)) {
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;
	}

	uint64 bonus_id = script_getnum64(st, 2);
	if (pc_bonus_script_exists(sd, bonus_id))
		script_pushint(st, true);
	else
		script_pushint(st, false);

	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: bonus_script_getid
 * 描述: 用于查询效果脚本代码对应的效果脚本编号
 * 用法: bonus_script_getid <"效果脚本代码">,<返回效果脚本编号数组>{,<角色编号>};
 * 返回: 查询到的记录数
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(bonus_script_getid) {
	TBL_PC* sd = nullptr;
	if (!script_charid2sd(4, sd)) {
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;
	}

	const char* script_str = nullptr;
	script_str = script_getstr(st, 2);

	int ret_varid = 0;
	char* ret_varname = nullptr;
	struct script_data* ret_vardata = nullptr;
	if (!script_get_array(st, 3, ret_varid, ret_varname, ret_vardata)) {
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;
	}
	script_cleararray_st(st, 3);

	uint16 count = 0;
	struct linkdb_node* node = NULL;
	struct s_bonus_script_entry* entry = NULL;

	if ((node = sd->bonus_script.head)) {
		while (node) {
			struct linkdb_node* next = node->next;
			entry = (struct s_bonus_script_entry*)node->data;
			if (strcmpi(script_str, StringBuf_Value(entry->script_buf)) == 0) {
				int64 uid = reference_uid(ret_varid, count);
				set_reg_num(st, sd, uid, ret_varname, entry->bonus_id, reference_getref(ret_vardata));
				count++;
			}
			node = next;
		}
	}

	script_pushint(st, count);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: bonus_script_info
 * 描述: 查询指定效果脚本的相关信息
 * 用法: bonus_script_info <效果脚本编号>,<查询类型>{,<角色编号>};
 * 返回: 直接返回所查询的结果值
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(bonus_script_info) {
	TBL_PC* sd = nullptr;
	if (!script_charid2sd(4, sd)) {
		script_pushint(st, -2);
		return SCRIPT_CMD_FAILURE;
	}

	uint64 bonus_id = script_getnum64(st, 2);
	int32 query_type = script_getnum(st, 3);

	bool found = false;
	struct linkdb_node* node = NULL;
	struct s_bonus_script_entry* entry = NULL;

	if ((node = sd->bonus_script.head)) {
		while (node) {
			struct linkdb_node* next = node->next;
			entry = (struct s_bonus_script_entry*)node->data;
			if (bonus_id == entry->bonus_id) {
				found = true;
				break;
			}
			node = next;
		}
	}

	if (!found) {
		script_pushint(st, -3);
		return SCRIPT_CMD_SUCCESS;
	}

	switch (query_type) {
	case 0:		// 效果脚本代码
		script_pushstrcopy(st, StringBuf_Value(entry->script_buf)); break;
	case 1:		// 标记位
		script_pushint(st, entry->flag); break;
	case 2:		// 状态图标编号
		script_pushint(st, entry->icon); break;
	case 3:		// 类型
		script_pushint(st, entry->type); break;
	case 4:		// 剩余时间 (毫秒)
		if (entry->tid == INVALID_TIMER) {
			script_pushint(st, -1);
			break;
		}
		script_pushint(st, DIFF_TICK(get_timer(entry->tid)->tick, gettick()));
		break;
	default:
		ShowWarning("buildin_bonus_script_info: The type should be in range 0-%d, currently type is: %d.\n", 4, query_type);
		script_pushint(st, -4);
		break;
	}

	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: expandinventory_adjust
 * 描述: 增加角色的背包容量上限
 * 用法: expandinventory_adjust <增加多少容量>;
 * 返回: 调整成功返回 1, 失败返回 0
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(expandinventory_adjust) {
	TBL_PC* sd = nullptr;
	if (!script_rid2sd(sd)) {
		script_pushint(st, 0);
		return SCRIPT_CMD_FAILURE;
	}

	int expand_count = script_getnum(st, 2);

	if (sd->status.inventory_slots + expand_count <= MAX_INVENTORY &&
		sd->status.inventory_slots + expand_count >= sd->inventory.amount &&
		sd->status.inventory_slots + expand_count >= INVENTORY_BASE_SIZE) {
		sd->status.inventory_slots += expand_count;
		clif_inventory_expansion_info(sd);
		chrif_save(sd, CSAVE_NORMAL);
		script_pushint(st, 1);
	}
	else {
		script_pushint(st, 0);
	}

	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: getinventorysize
 * 描述: 查询并获取当前角色的背包容量上限
 * 用法: getinventorysize {<角色编号>};
 * 返回: 找不到角色则返回 0, 否则返回查询到的背包容量
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(getinventorysize) {
	TBL_PC* sd = nullptr;
	if (!script_charid2sd(2, sd)) {
		script_pushint(st, 0);
		return SCRIPT_CMD_FAILURE;
	}
	script_pushint(st, sd->status.inventory_slots);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: getmapspawns
 * 描述: 获取指定地图的魔物刷新点信息
 * 用法: getmapspawns "<地图名称>"{,<掩码字段>{,<角色编号>}};
 * 返回: 成功则返回找到的刷新点数量, 失败则返回 -1
 * 作者: Sola丶小克
 * 优化: _vvvvf
 * -----------------------------------------------------------*/
BUILDIN_FUNC(getmapspawns) {
	int mapindex = -1;
	std::string mapname = script_getstr(st, 2);

	unsigned int fields = SPA_ALL; // 默认获取所有字段
	int char_id = 0; // 默认0

	if (script_hasdata(st, 3)) {
		fields = static_cast<unsigned int>(script_getnum(st, 3));
	}
	if (script_hasdata(st, 4)) {
		char_id = script_getnum(st, 4);
	}

	script_both_setreg(st, "spawn_count", 0, false, -1, char_id);

	if (!script_get_mapindex(st, mapname.c_str(), mapindex, char_id)) {
		ShowError("buildin_getmapspawns: Could not found valid map by map name '%s'\n", mapname.c_str());
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;
	}

	struct map_data* mapdata = map_getmapdata(mapindex);
	if (!mapdata) {
		script_reportsrc(st);
		script_reportfunc(st);
		ShowError("buildin_getmapspawns: Could not found valid map by map name '%s'\n", mapname.c_str());
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;
	}

	// 特殊处理：SPA_NONE只返回数量
	if (fields == SPA_NONE) {
		int count = 0;
		for (const auto& data : mapdata->moblist) {
			if (!data) continue;
			count++;
		}

		script_both_setreg(st, "spawn_count", count, false, -1, char_id);
		script_pushint(st, count);
		return SCRIPT_CMD_SUCCESS;
	}

	int j = 0;
	for (const auto& data : mapdata->moblist) {
		if (!data) continue;
		std::shared_ptr<s_mob_db> mob = mob_db.find(data->id);
		if (!mob) continue;

		// 按需设置字段
		if (fields & SPA_ID) {
			script_both_setreg(st, "spawn_mobid", data->id, true, j, char_id);
		}
		if (fields & SPA_NAME) {
			script_both_setregstr(st, "spawn_name$", data->name, true, j, char_id);
		}
		if (fields & SPA_NUM) {
			script_both_setreg(st, "spawn_num", data->num, true, j, char_id);
		}
		if (fields & SPA_ACTIVE) {
			script_both_setreg(st, "spawn_active", data->active, true, j, char_id);
		}
		if (fields & SPA_SIZE) {
			script_both_setreg(st, "spawn_size", data->state.size, true, j, char_id);
		}
		if (fields & SPA_ISBOSS) {
			script_both_setreg(st, "spawn_isboss", data->state.boss, true, j, char_id);
		}
		if (fields & SPA_DELAY1) {
			script_both_setreg(st, "spawn_delay1", data->delay1, true, j, char_id);
		}
		if (fields & SPA_DELAY2) {
			script_both_setreg(st, "spawn_delay2", data->delay2, true, j, char_id);
		}
		if (fields & SPA_AI) {
			script_both_setreg(st, "spawn_ai", data->state.ai, true, j, char_id);
		}
		if (fields & SPA_LEVEL) {
			int level = (data->level > 0 ? data->level : mob->lv);
			script_both_setreg(st, "spawn_level", level, true, j, char_id);
		}
		if (fields & SPA_EVENT) {
			script_both_setregstr(st, "spawn_eventname$", data->eventname, true, j, char_id);
		}
		if (fields & SPA_MAPID) {
			script_both_setreg(st, "spawn_mapid", data->m, true, j, char_id);
		}
		if (fields & SPA_MAPNAME) {
			script_both_setregstr(st, "spawn_mapname$", mapdata->name, true, j, char_id);
		}
		if (fields & SPA_MAPX) {
			script_both_setreg(st, "spawn_x", data->x, true, j, char_id);
		}
		if (fields & SPA_MAPY) {
			script_both_setreg(st, "spawn_y", data->y, true, j, char_id);
		}
		if (fields & SPA_MAPXS) {
			script_both_setreg(st, "spawn_xs", data->xs, true, j, char_id);
		}
		if (fields & SPA_MAPYS) {
			script_both_setreg(st, "spawn_ys", data->ys, true, j, char_id);
		}
		j++;
	}

	script_both_setreg(st, "spawn_count", j, false, -1, char_id);
	script_pushint(st, j);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: getmobspawns
 * 描述: 查询指定魔物在不同地图的刷新点信息
 * 用法: getmobspawns <魔物编号>{,"<地图名称>"{,<角色编号>}};
 * 返回: 成功则返回找到的刷新点数量, 失败则返回 -1
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(getmobspawns) {
	int mapindex = -1;
	int mob_id = script_getnum(st, 2);
	std::string mapname = (script_hasdata(st, 3) && script_isstring(st, 3)) ? script_getstr(st, 3) : "";
	int char_id = (script_hasdata(st, 4) && script_isint(st, 4)) ? script_getnum(st, 4) : 0;

	script_both_setreg(st, "spawn_count", 0, false, -1, char_id);

	// 若指定了地图名称则顺带查询地图数据信息, 没指定就算了
	if (mapname.length() > 0) {
		if (!script_get_mapindex(st, mapname.c_str(), mapindex, char_id)) {
			ShowError("buildin_getmobspawns: Could not found valid map by map name '%s'\n", mapname.c_str());
			script_pushint(st, -1);
			return SCRIPT_CMD_FAILURE;
		}

		struct map_data* mapdata = map_getmapdata(mapindex);

		if (!mapdata) {
			script_reportsrc(st);
			script_reportfunc(st);
			ShowError("buildin_getmobspawns: Could not found valid map by map name '%s'\n", mapname.c_str());
			script_pushint(st, -1);
			return SCRIPT_CMD_FAILURE;
		}
	}

	// 通过 rAthena 内建的刷新点缓存快速确定关联地图
	const std::vector<spawn_info> spawns = mob_get_spawns(mob_id);

	int j = 0;

	for (auto& spawn : spawns) {
		int16 mapid = map_mapindex2mapid(spawn.mapindex);

		// 若设置了仅查询某地图的魔物刷新点信息, 那么非指定地图的都跳过
		if (mapindex != -1 && mapid != mapindex)
			continue;

		// 获取有记载指定魔物刷新点的目标地图数据
		struct map_data* mapdata = map_getmapdata(mapid);

		// 找不到对应地图的数据则直接跳过
		if (!mapdata) continue;

		for (const auto& data : mapdata->moblist) {
			if (!data) continue;
			if (data->id != mob_id) continue;
			std::shared_ptr<s_mob_db> mob = mob_db.find(data->id);
			if (!mob) continue;

			script_both_setreg(st, "spawn_mobid", data->id, true, j, char_id);
			script_both_setregstr(st, "spawn_name$", data->name, true, j, char_id);
			script_both_setreg(st, "spawn_num", data->num, true, j, char_id);
			script_both_setreg(st, "spawn_active", data->active, true, j, char_id);
			script_both_setreg(st, "spawn_size", data->state.size, true, j, char_id);
			script_both_setreg(st, "spawn_isboss", data->state.boss, true, j, char_id);
			script_both_setreg(st, "spawn_ai", data->state.ai, true, j, char_id);
			script_both_setreg(st, "spawn_level", (data->level > 0 ? data->level : mob->lv), true, j, char_id);
			script_both_setreg(st, "spawn_delay1", data->delay1, true, j, char_id);
			script_both_setreg(st, "spawn_delay2", data->delay2, true, j, char_id);
			script_both_setregstr(st, "spawn_eventname$", data->eventname, true, j, char_id);

			script_both_setreg(st, "spawn_mapid", data->m, true, j, char_id);
			script_both_setregstr(st, "spawn_mapname$", mapdata->name, true, j, char_id);
			script_both_setreg(st, "spawn_x", data->x, true, j, char_id);
			script_both_setreg(st, "spawn_y", data->y, true, j, char_id);
			script_both_setreg(st, "spawn_xs", data->xs, true, j, char_id);
			script_both_setreg(st, "spawn_ys", data->ys, true, j, char_id);

			j++;
		}
	}

	script_both_setreg(st, "spawn_count", j, false, -1, char_id);
	script_pushint(st, j);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: getcalendartime
 * 描述: 获取下次出现指定时间的 UNIX 时间戳
 * 用法: getcalendartime <小时>,<分钟>{,<月的第几天>{,<周的第几天>}};
 * 返回: 成功则返回时间戳, 失败则返回 -1
 * 作者: Haru <haru@dotalux.com>
 * -----------------------------------------------------------*/
BUILDIN_FUNC(getcalendartime) {
	struct tm info = { 0 };
	int day_of_month = script_hasdata(st, 4) ? script_getnum(st, 4) : -1;
	int day_of_week = script_hasdata(st, 5) ? script_getnum(st, 5) : -1;
	int year = date_get_year();
	int month = date_get_month();
	int day = date_get_dayofmonth();
	int cur_hour = date_get_hour();
	int cur_min = date_get_min();
	int hour = script_getnum(st, 2);
	int minute = script_getnum(st, 3);

	info.tm_sec = 0;
	info.tm_min = minute;
	info.tm_hour = hour;
	info.tm_mday = day;
	info.tm_mon = month - 1;
	info.tm_year = year - 1900;

	if (day_of_month > -1 && day_of_week > -1) {
		ShowError("buildin_getcalendartime: You must only specify a day_of_week or a day_of_month, not both\n");
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;
	}
	if (day_of_month > -1 && (day_of_month < 1 || day_of_month > 31)) {
		ShowError("buildin_getcalendartime: Day of Month in invalid range. Must be between 1 and 31.\n");
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;
	}
	if (day_of_week > -1 && (day_of_week < 0 || day_of_week > 6)) {
		ShowError("buildin_getcalendartime: Day of Week in invalid range. Must be between 0 and 6.\n");
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;
	}
	if (hour > -1 && (hour > 23)) {
		ShowError("buildin_getcalendartime: Hour in invalid range. Must be between 0 and 23.\n");
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;
	}
	if (minute > -1 && (minute > 59)) {
		ShowError("buildin_getcalendartime: Minute in invalid range. Must be between 0 and 59.\n");
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;
	}
	if (hour == -1 || minute == -1) {
		ShowError("buildin_getcalendartime: Minutes and Hours are required\n");
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;
	}

	if (day_of_month > -1) {
		if (day_of_month < day) { // Next Month
			info.tm_mon++;
		}
		else if (day_of_month == day) { // Today
			if (hour < cur_hour || (hour == cur_hour && minute < cur_min)) { // But past time, next month
				info.tm_mon++;
			}
		}

		// Loops until month has finding a month that has day_of_month
		do {
			time_t t;
			struct tm* lt;
			info.tm_mday = day_of_month;
			t = mktime(&info);
			lt = localtime(&t);
			info = *lt;
		} while (info.tm_mday != day_of_month);
	}
	else if (day_of_week > -1) {
		int cur_wday = date_get_dayofweek();

		if (day_of_week > cur_wday) { // This week
			info.tm_mday += (day_of_week - cur_wday);
		}
		else if (day_of_week == cur_wday) { // Today
			if (hour < cur_hour || (hour == cur_hour && minute <= cur_min)) {
				info.tm_mday += 7; // Next week
			}
		}
		else if (day_of_week < cur_wday) { // Next week
			info.tm_mday += (7 - cur_wday + day_of_week);
		}
	}
	else if (day_of_week == -1 && day_of_month == -1) { // Next occurence of hour/min
		if (hour < cur_hour || (hour == cur_hour && minute < cur_min)) {
			info.tm_mday++;
		}
	}

	script_pushint(st, mktime(&info));

	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: getskillinfo
 * 描述: 获取指定技能在技能数据库中所配置的各项信息
 * 用法: getskillinfo <查询的信息类型>,<技能编号>{,<技能等级>{,<角色编号>}};
 * 用法: getskillinfo <查询的信息类型>,<"技能名称">{,<技能等级>{,<角色编号>}};
 * 返回: 请查阅 doc/pandas_script_commands.txt 中的说明
 * 作者: 聽風
 * -----------------------------------------------------------*/
BUILDIN_FUNC(getskillinfo) {
	int type = 0;
	uint16 skill_id = 0;
	int i = 0, j = 0;
	int skill_lv = (script_hasdata(st, 4) && script_isint(st, 4)) ? script_getnum(st, 4) : 0;
	int char_id = (script_hasdata(st, 5) && script_isint(st, 5)) ? script_getnum(st, 5) : 0;

	type = script_getnum(st, 2);
	if (script_isstring(st, 3)) {
		const char *name = script_getstr(st, 3);
		if (!(skill_id = skill_name2id(name))) {
			ShowError("buildin_getskillinfo: Invalid skill name %s.\n", name);
			return SCRIPT_CMD_FAILURE;
		}
	}
	else {
		skill_id = script_getnum(st, 3);
		if (!skill_get_index(skill_id)) {
			ShowError("buildin_getskillinfo: Invalid skill ID %d.\n", skill_id);
			return SCRIPT_CMD_FAILURE;
		}
	}

	std::shared_ptr<s_skill_db> skill = skill_db.find(skill_id);
	if (skill == nullptr) {
		ShowError("buildin_getskillinfo: Invalid skill ID %d.\n", skill_id);
		return SCRIPT_CMD_FAILURE;
	}

	switch (type) {
		case SKI_CASTTYPE: script_pushint(st, skill_get_casttype(skill_id)); break;
		case SKI_NAME: script_pushstrcopy(st, skill_get_name(skill_id)); break;
		case SKI_DESCRIPTION: script_pushstrcopy(st, skill_get_desc(skill_id)); break;
		case SKI_MAXLEVEL_IN_SKILLTREE: script_pushint(st, skill_tree_get_max(skill_id,skill_lv)); break;
		case SKI_SKILLTYPE: script_pushint(st, skill_get_type(skill_id)); break;
		case SKI_HIT: script_pushint(st, skill_get_hit(skill_id)); break;
		case SKI_TARGETTYPE: script_pushint(st, skill_get_inf(skill_id)); break;
		case SKI_ELEMENT: script_pushint(st, skill_get_ele(skill_id,skill_lv)); break;
		case SKI_MAXLEVEL: script_pushint(st, skill_get_max(skill_id)); break;
		case SKI_RANGE: script_pushint(st, skill_get_range(skill_id,skill_lv)); break;
		case SKI_SPLASHAREA: script_pushint(st, skill_get_splash(skill_id,skill_lv)); break;
		case SKI_HITCOUNT: script_pushint(st, skill_get_num(skill_id,skill_lv)); break;
		case SKI_CASTTIME: script_pushint(st, skill_get_cast(skill_id,skill_lv)); break;
#ifdef RENEWAL_CAST
		case SKI_FIXEDCASTTIME: script_pushint(st, skill->fixed_cast[cap_value(skill_lv, 1, MAX_SKILL_LEVEL) - 1]); break;
#else
		case SKI_FIXEDCASTTIME: script_pushint(st, -1); break;
#endif // RENEWAL_CAST
		case SKI_AFTERCASTACTDELAY: script_pushint(st, skill_get_delay(skill_id,skill_lv)); break;
		case SKI_AFTERCASTWALKDELAY: script_pushint(st, skill_get_walkdelay(skill_id,skill_lv)); break;
		case SKI_DURATION1: script_pushint(st, skill_get_time(skill_id,skill_lv)); break;
		case SKI_DURATION2: script_pushint(st, skill_get_time2(skill_id,skill_lv)); break;
		case SKI_CASTTIMEFLAGS: script_pushint(st, skill_get_castnodex(skill_id)); break;
		case SKI_CASTDELAYFLAGS: script_pushint(st, skill->delaynodex); break;
		case SKI_CASTDEFENSEREDUCTION: script_pushint(st, skill_get_castdef(skill_id)); break;
		case SKI_CASTCANCEL: script_pushint(st, skill_get_castcancel(skill_id)); break;
		case SKI_ACTIVEINSTANCE: script_pushint(st, skill_get_maxcount(skill_id,skill_lv)); break;
		case SKI_KNOCKBACK: script_pushint(st, skill_get_blewcount(skill_id,skill_lv)); break;
		case SKI_COOLDOWN: script_pushint(st, skill_get_cooldown(skill_id,skill_lv)); break;
		case SKI_NONEARNPC_TYPE: script_pushint(st, skill->unit_nonearnpc_type); break;
		case SKI_NONEARNPC_ADDITIONALRANGE: script_pushint(st, skill->unit_nonearnpc_range); break;
		case SKI_COPYFLAGS_SKILL: script_pushint(st, skill->copyable.option); break;
		case SKI_UNIT_ID: script_pushint(st, skill_get_unit_id(skill_id)); break;
		case SKI_UNIT_ALTERNATEID: script_pushint(st, skill_get_unit_id2(skill_id)); break;
		case SKI_UNIT_LAYOUT: script_pushint(st, skill->unit_layout_type[cap_value(skill_lv, 1, MAX_SKILL_LEVEL) - 1]); break;
		case SKI_UNIT_RANGE: script_pushint(st, skill_get_unit_range(skill_id,skill_lv)); break;
		case SKI_UNIT_INTERVAL: script_pushint(st, skill_get_unit_interval(skill_id)); break;
		case SKI_UNIT_TARGET: script_pushint(st, skill_get_unit_target(skill_id)); break;
		case SKI_REQUIRES_HPCOST: script_pushint(st, skill_get_hp(skill_id,skill_lv)); break;
		case SKI_REQUIRES_SPCOST: script_pushint(st, skill_get_sp(skill_id,skill_lv)); break;
		case SKI_REQUIRES_APCOST: script_pushint(st, skill_get_ap(skill_id, skill_lv)); break;
		case SKI_REQUIRES_MAXHPTRIGGER: script_pushint(st, skill_get_mhp(skill_id,skill_lv)); break;
		case SKI_REQUIRES_HPRATECOST: script_pushint(st, skill_get_hp_rate(skill_id,skill_lv)); break;
		case SKI_REQUIRES_SPRATECOST: script_pushint(st, skill_get_sp_rate(skill_id,skill_lv)); break;
		case SKI_REQUIRES_ZENYCOST: script_pushint(st, skill_get_zeny(skill_id,skill_lv)); break;
		case SKI_REQUIRES_WEAPON: script_pushint(st, skill_get_weapontype(skill_id)); break;
		case SKI_REQUIRES_AMMO: script_pushint(st, skill_get_ammotype(skill_id)); break;
		case SKI_REQUIRES_AMMOAMOUNT: script_pushint(st, skill_get_ammo_qty(skill_id,skill_lv)); break;
		case SKI_REQUIRES_STATE: script_pushint(st, skill_get_state(skill_id)); break;
		case SKI_REQUIRES_SPHERECOST: script_pushint(st, skill_get_spiritball(skill_id,skill_lv)); break;

		case SKI_REQUIRES_STATUS:
			for (const auto& sc : skill->require.status) {
				script_both_setreg(st, "skill_requires_status", (int64)sc, true, j, char_id);
				j++;
			}
			script_pushint(st, j);
			break;
		case SKI_DAMAGEFLAGS:
			for (i = 0; i < NK_MAX; i++) {
				script_both_setreg(st, "skill_damage_flags", skill->nk[i], true, i, char_id);
			}
			script_pushint(st, NK_MAX);
			break;
		case SKI_FLAGS:
			for (i = 0; i < INF2_MAX; i++) {
				script_both_setreg(st, "skill_flags", skill->inf2[i], true, i, char_id);
			}
			script_pushint(st, INF2_MAX);
			break;
		case SKI_UNIT_FLAG:
			for (i = 0; i < UF_MAX; i++) {
				script_both_setreg(st, "skill_unit_flag", skill->unit_flag[i], true, i, char_id);
			}
			script_pushint(st, UF_MAX);
			break;
		case SKI_REQUIRES_EQUIPMENT:
			for (const auto& item : skill->require.eqItem) {
				script_both_setreg(st, "skill_requires_equipment", (int64)item, true, j, char_id);
				j++;
			}
			script_pushint(st, j);
			break;
		case SKI_REQUIRES_ITEMCOST:
			for (i = 0; i < MAX_SKILL_ITEM_REQUIRE; i++) {
				if (!skill->require.itemid[i]) continue;
				script_both_setreg(st, "skill_requires_itemid", skill->require.itemid[i], true, j, char_id);
				script_both_setreg(st, "skill_requires_amount", skill->require.amount[i], true, j, char_id);
				j++;
			}
			script_pushint(st, j);
			break;
		default:
			script_pushint(st, -1);
			return SCRIPT_CMD_FAILURE;
	}

	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: sleep3
 * 描述: 休眠一段时间再执行后续脚本, 与 sleep2 类似但忽略报错
 * 用法: sleep3 <休眠毫秒数>;
 * 返回: 该指令无论成功与否, 都不会有返回值
 * 作者: 人鱼姬的思念
 * -----------------------------------------------------------*/
BUILDIN_FUNC(sleep3) {
	// First call(by function call)
	if (st->sleep.tick == 0) {
		int ticks;

		ticks = script_getnum(st, 2);

		if (ticks <= 0) {
			ShowError("buildin_sleep3: negative or zero amount('%d') of milli seconds is not supported\n", ticks);
			return SCRIPT_CMD_FAILURE;
		}

		// sleep for the target amount of time
		st->state = RERUNLINE;
		st->sleep.tick = ticks;
		// Second call(by timer after sleeping time is over)
	}
	else {
		// The unit is still attached - continue the script
		st->state = RUN;
		st->sleep.tick = 0;
	}

	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: getquesttime
 * 描述: 查询角色指定任务的时间信息
 * 用法: getquesttime <任务编号>{,<想查询的时间类型>{,<角色编号>}};
 * 返回: 成功返回时间戳, 失败返回 -1
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(getquesttime) {
	int quest_id = script_getnum(st, 2);
	int query_time_type = (script_hasdata(st, 3) && script_isint(st, 3)) ? script_getnum(st, 3) : 0;
	map_session_data* sd = nullptr;

	if (!script_charid2sd(4, sd)) {
		return SCRIPT_CMD_FAILURE;
	}

	std::shared_ptr<s_quest_db> qi = quest_search(quest_id);
	if (!qi) {
		ShowError("buildin_getquesttime: Quest %d not found in DB.\n", quest_id);
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;
	}

	int i = 0;
	ARR_FIND(0, sd->num_quests, i, sd->quest_log[i].quest_id == quest_id);
	if (i == sd->num_quests) {
		ShowError("buildin_getquesttime: Character %d doesn't have quest %d.\n", sd->status.char_id, quest_id);
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;
	}

	switch (query_time_type)
	{
	case 0:
		script_pushint(st, sd->quest_log[i].time);
		break;
	case 1:
		script_pushint(st, sd->quest_log[i].time - qi->time);
		break;
	case 2:
		script_pushint(st, u64max(sd->quest_log[i].time - time(nullptr), 0));
		break;
	default:
		ShowError("buildin_getquesttime: Invaild type for quest time.\n");
		script_pushint(st, -1);
		break;
	}

	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: unitspecialeffect
 * 描述: 使指定游戏单位可以显示某个特效, 并支持控制特效可见范围
 * 用法: unitspecialeffect <游戏单位编号>,<特效编号>{,<谁能看见特效>{,<能看见特效的账号编号>}};
 * 返回: 该指令无论成功与否, 都不会有返回值
 * 作者: 人鱼姬的思念
 * -----------------------------------------------------------*/
BUILDIN_FUNC(unitspecialeffect) {
	block_list* bl = nullptr;
	int type = script_getnum(st, 3);
	enum send_target target = AREA;

	bl = map_id2bl(script_getnum(st, 2));
	if (!bl) {
		return SCRIPT_CMD_SUCCESS;
	}

	if (script_hasdata(st, 4)) {
		target = (send_target)script_getnum(st, 4);
	}

	if (type <= EF_NONE || type >= EF_MAX) {
		ShowError("buildin_unitspecialeffect: unsupported effect id %d\n", type);
		return SCRIPT_CMD_FAILURE;
	}

	if (target != SELF) {
		clif_specialeffect(bl, type, target);
		return SCRIPT_CMD_SUCCESS;
	}

	map_session_data* sd = nullptr;
	if (!script_mapid2sd(5, sd)) {
		return SCRIPT_CMD_SUCCESS;
	}

	if (sd && sd->type == BL_PC) {
		clif_specialeffect_single(bl, type, sd->fd);
	}
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: next_dropitem_special
 * 描述: 对下一个掉落到地面上的物品进行特殊设置
 * 用法: next_dropitem_special <道具绑定类型>,<租赁时长>,<掉落光柱颜色>;
 * 返回: 该指令无论成功与否, 都不会有返回值
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(next_dropitem_special) {
	next_dropitem_special.bound = cap_value(script_getnum(st, 2), BOUND_NONE, BOUND_MAX - 1);
	next_dropitem_special.rent_duration = cap_value(script_getnum(st, 3), 0, INT32_MAX);
	next_dropitem_special.drop_effect = cap_value(script_getnum(st, 4), -1, DROPEFFECT_MAX - 1);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: getrateidx
 * 描述: 随机获取一个数值型数组的索引序号, 数组中每个元素的值为权重值
 * 用法: getrateidx <数值型数组变量>;
 * 用法: getrateidx <数值1>{, <数值2>{, ...<数值n>}};
 * 返回: 返回随机命中的索引序号
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
BUILDIN_FUNC(getrateidx) {
	uint64 total_weight = 0;
	struct script_data* data = script_getdata(st, 2);
	std::vector<uint64> weights;

	// 如果不是一个数组, 那么将参数逐个累加
	if (!data_isreference(data)) {
		for (int i = 0; i < script_lastdata(st) - 1; i++) {
			uint64 weight = script_getnum(st, i + 2);
			weights.push_back(weight);
			total_weight += weight;
		}
	}
	// 否则说明传递的是一个数组, 从数组中读取内容
	else {
		map_session_data* sd = nullptr;
		int varid = reference_getid(data);
		const char* varname = reference_getname(data);

		// 若不是服务器变量, 那么必须要关联到玩家
		if (not_server_variable(*varname)) {
			if (!script_rid2sd(sd)) {
				ShowError("buildin_getrateidx: '%s' is not server variable, please attach to a player.\n", varname);
				st->state = END;
				return SCRIPT_CMD_FAILURE;
			}
		}

		// 检查是否为数值类型的数组变量, 不是则报错并终止
		if (is_string_variable(varname)) {
			ShowError("buildin_getrateidx: '%s' is not an int array.\n", varname);
			st->state = END;
			return SCRIPT_CMD_FAILURE;
		}

		// 获取数组的元素个数, 并判断是否在有效范围内
		uint32 array_size = script_array_highest_key(st, sd, varname, reference_getref(data));
		if (array_size == 0 || array_size > SCRIPT_MAX_ARRAYSIZE) {
			ShowError("buildin_getrateidx: The size of '%s' is not in valid range.\n", varname);
			st->state = END;
			return SCRIPT_CMD_FAILURE;
		}

		for (uint32 i = 0; i < array_size; i++) {
			uint64 weight = get_val2_num(st, reference_uid(varid, i), reference_getref(data));
			weights.push_back(weight);
			total_weight += weight;
		}
	}

	if (!weights.size() || !total_weight) {
		ShowError("buildin_getrateidx: Could not find any valid data for the calculation.\n");
		st->state = END;
		return SCRIPT_CMD_FAILURE;
	}

	uint64 randNum = rnd() % total_weight;
	uint64 start = 0, end = 0;
	size_t idx = 0;

	for (idx = 0; idx < weights.size(); idx++) {
		if (weights[idx] == 0) {
			continue;
		}

		end = start + weights[idx];
		if (start <= randNum && randNum < end) {
			break;
		}
		start = end;
	}

	if (idx == weights.size()) {
		// Shouldn't happen
		ShowError("buildin_getrateidx: Some accidents happened.\n");
		st->state = END;
		return SCRIPT_CMD_FAILURE;
	}

	script_pushint(st, idx);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: getbossinfo
 * 描述: 查询 BOSS 魔物重生时间及其坟墓等信息
 * 用法: getbossinfo {<"地图名称">{,<魔物编号>{,<角色编号>}}};
 * 返回: 返回查询到的记录数
 * 作者: Sola丶小克
 * -----------------------------------------------------------*/
static int32 buildin_getbossinfo_sub(mob_data* md, va_list args) {
	script_state* st = va_arg(args, script_state*);
	const int mapid = va_arg(args, int);
	const int mobid = va_arg(args, int);
	const int char_id = va_arg(args, int);
	int* count = va_arg(args, int*);
	if (md == nullptr || md->get_bosstype() != BOSSTYPE_MVP || (mapid >= 0 && md->m != mapid) || (mobid != 0 && md->mob_id != mobid))
		return 0;
	const int index = (*count)++;
	npc_data* tomb = md->tomb_nid != 0 ? map_id2nd(md->tomb_nid) : nullptr;
	map_data* boss_map = map_getmapdata(md->m);
	map_data* tomb_map = tomb != nullptr ? map_getmapdata(tomb->m) : nullptr;
	int64 spawn_ms = 0;
	if (md->spawn_timer != INVALID_TIMER) {
		const TimerData* timer = get_timer(md->spawn_timer);
		if (timer != nullptr)
			spawn_ms = DIFF_TICK(timer->tick, gettick());
	}
	const int64 respawn_seconds = spawn_ms > 0 ? spawn_ms / 1000 : -1;
	script_both_setreg(st, "boss_mapid", md->m, true, index, char_id);
	script_both_setregstr(st, "boss_mapname$", boss_map != nullptr ? boss_map->name : "", true, index, char_id);
	script_both_setreg(st, "boss_x", md->x, true, index, char_id);
	script_both_setreg(st, "boss_y", md->y, true, index, char_id);
	script_both_setreg(st, "boss_gid", md->id, true, index, char_id);
	script_both_setreg(st, "boss_spawn", spawn_ms, true, index, char_id);
	script_both_setreg(st, "boss_classid", md->mob_id, true, index, char_id);
	script_both_setreg(st, "boss_tomb_mapid", tomb != nullptr ? tomb->m : -1, true, index, char_id);
	script_both_setregstr(st, "boss_tomb_mapname$", tomb_map != nullptr ? tomb_map->name : "", true, index, char_id);
	script_both_setreg(st, "boss_tomb_x", tomb != nullptr ? tomb->x : 0, true, index, char_id);
	script_both_setreg(st, "boss_tomb_y", tomb != nullptr ? tomb->y : 0, true, index, char_id);
	script_both_setreg(st, "boss_tomb_gid", tomb != nullptr ? tomb->id : 0, true, index, char_id);
	script_both_setreg(st, "boss_tomb_createtime", tomb != nullptr ? tomb->u.tomb.kill_time : 0, true, index, char_id);
	script_both_setreg(st, "boss_tomb_respawnsecs", respawn_seconds, true, index, char_id);
	script_both_setreg(st, "boss_tomb_respawntime", respawn_seconds >= 0 ? time(nullptr) + respawn_seconds : -1, true, index, char_id);
	script_both_setregstr(st, "boss_tomb_killer_name$", tomb != nullptr ? tomb->u.tomb.killer_name : "", true, index, char_id);
	// 当前 rAthena 坟墓结构不保存击杀者 GID，保留兼容字段并返回 0。
	script_both_setreg(st, "boss_tomb_killer_gid", 0, true, index, char_id);
	return 1;
}

BUILDIN_FUNC(getbossinfo) {
	const int char_id = script_hasdata(st, 4) ? script_getnum(st, 4) : 0;
	const int mobid = script_hasdata(st, 3) ? script_getnum(st, 3) : 0;
	int mapid = -1;
	if (script_hasdata(st, 2)) {
		const char* mapname = script_getstr(st, 2);
		if (strcmpi(mapname, "all") != 0 && !script_get_mapindex(st, mapname, mapid, char_id)) {
			script_pushint(st, 0);
			return SCRIPT_CMD_FAILURE;
		}
	}
	int count = 0;
	script_both_setreg(st, "boss_count", 0, false, -1, char_id);
	map_foreachmob(buildin_getbossinfo_sub, st, mapid, mobid, char_id, &count);
	script_both_setreg(st, "boss_count", count, false, -1, char_id);
	script_pushint(st, count);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: whodropitem
 * 描述: 查询指定道具会从哪些魔物身上掉落以及掉落的机率信息
 * 用法: whodropitem <物品编号/"物品名称">{,<返回的最大记录数>{,<角色编号>}};
 * 返回: 返回查询到的记录数量
 * 作者: Sola丶小克 (感谢 "人鱼姬的思念")
 * -----------------------------------------------------------*/
BUILDIN_FUNC(whodropitem) {
	t_itemid nameid = 0;
	int max_result = (script_hasdata(st, 3) && script_isint(st, 3)) ? script_getnum(st, 3) : 0;
	int char_id = (script_hasdata(st, 4) && script_isint(st, 4)) ? script_getnum(st, 4) : 0;
	std::shared_ptr<item_data> id;
	map_session_data *sd = nullptr;

	// 清空比较关键的返回值
	script_both_setreg(st, "whodropitem_count", 0, false, 0, char_id);

	if (script_isstring(st, 2)) {// "<item name>"
		const char* name = script_getstr(st, 2);

		id = item_db.searchname(name);

		if (id == nullptr) {
			ShowError("buildin_whodropitem: Nonexistant item %s requested.\n", name);
			script_pushint(st, 0);
			return SCRIPT_CMD_FAILURE;
		}
		nameid = id->nameid;
	}
	else {// <item id>
		nameid = script_getnum(st, 2);

		id = item_db.find(nameid);

		if (id == nullptr) {
			ShowError("buildin_whodropitem: Nonexistant item %u requested.\n", nameid);
			script_pushint(st, 0);
			return SCRIPT_CMD_FAILURE;
		}
	}

	// 最大返回的结果数量若未被指定, 则默认值为 MAX_SEARCH
	max_result = (max_result ? max_result : MAX_SEARCH);

	// 最大返回的结果数量应该在 1-500 之间 (这里的 500 是拍脑袋的, 通常应该够用了)
	max_result = cap_value(max_result, 1, 500);

	// 若连一个掉落此物品的魔物都没有, 那么直接返回 0
	if (id->mob[0].chance == 0) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	// 允许在未关联玩家的情况下执行该指令, 没关联玩家也不报错
	// 但是若明确指定了某个角色, 但目标角色不存在的话, 那么需要报错
	if (char_id && !(sd = map_charid2sd(char_id))) {
		script_pushint(st, 0);
		return SCRIPT_CMD_FAILURE;
	}
	else if (st->rid != 0) {
		sd = map_id2sd(st->rid);
	}

	// 用来临时保存结果的缓存区域 <魔物编号, 掉落机率>
	std::map<uint32, int> result_cache;

	// 构建我们需要的结果, 只查询单个道具其实速度非常快, 不用担心性能过渡开销
	for (auto const& pair : mob_db) {
		if (mob_is_clone(pair.first)) {
			continue;
		}

		for (const std::shared_ptr<s_mob_drop>&entry : pair.second->dropitem) {
			if (entry->nameid != nameid)
				continue;

			if (rathena::util::map_exists(result_cache, pair.first)) {
				if (result_cache[pair.first] < entry->rate)
					result_cache[pair.first] = entry->rate;
			}
			else {
				result_cache[pair.first] = entry->rate;
			}
		}
	}

	// 根据一些掉率调整设置来处理道具的掉率, 这样最后排序的时候才能考虑进去
	for (auto const& it : result_cache) {
		int dropchance = it.second;

#ifdef RENEWAL_DROP
		if (sd && battle_config.atcommand_mobinfo_type) {
			std::shared_ptr<s_mob_db> mob = mob_db.find(it.first);
			// 备注: 由于 pc_level_penalty_mod 内部已经判断了 mob 智能指针的有效性, 因此在此处不重复判断
			dropchance = dropchance * pc_level_penalty_mod(sd, PENALTY_DROP, mob) / 100;
			if (dropchance <= 0 && !battle_config.drop_rate0item)
				dropchance = 1;
		}
#endif
		if (sd && pc_isvip(sd)) // Display item rate increase for VIP
			dropchance += (dropchance * battle_config.vip_drop_increase) / 100;

		// 当前 rAthena 不提供 Pandas 的严格掉率覆盖表，沿用数据库及原生倍率计算结果。

		result_cache[it.first] = dropchance;
	}

	// 对 result_cache 的内容进行降序排序
	std::vector<std::pair<uint32, int>> result_sortd;
	for (auto const& pair : result_cache) {
		result_sortd.push_back(std::make_pair(pair.first, pair.second));
	}

	// 先按魔物编号从小到大排序
	std::sort(result_sortd.begin(), result_sortd.end(),
		[](const std::pair<uint32, int>& a, const std::pair<uint32, int>& b) {
			return a.first < b.first;
		}
	);

	// 再按掉率从大到小排序
	std::sort(result_sortd.begin(), result_sortd.end(),
		[](const std::pair<uint32, int>& a, const std::pair<uint32, int>& b) {
			return a.second > b.second;
		}
	);

	// 若排序后的 result_sortd 没有任何结果, 那么直接返回即可
	if (result_sortd.empty()) {
		script_pushint(st, 0);
		return SCRIPT_CMD_SUCCESS;
	}

	// 准备就绪, 开始填充数组内容并返回结果
	int current_index = 0;

	for (const auto& it : result_sortd) {
		int dropchance = it.second;

		std::shared_ptr<s_mob_db> mob = mob_db.find(it.first);
		if (!mob) continue;

		script_both_setreg(st, "whodropitem_mob_id", mob->id, true, current_index, char_id);
		script_both_setregstr(st, "whodropitem_mob_jname$", mob->jname.c_str(), true, current_index, char_id);
		script_both_setreg(st, "whodropitem_chance", dropchance, true, current_index, char_id);

		current_index++;

		if (current_index >= max_result)
			break;
	}

	script_both_setreg(st, "whodropitem_count", current_index, false, 0, char_id);
	script_pushint(st, current_index);
	return SCRIPT_CMD_SUCCESS;
}

/* ===========================================================
 * 指令: query_sql_async / query_logsql_async
 * 描述: rAthena 兼容实现复用原生查询及结果数组语义。
 * -----------------------------------------------------------*/
BUILDIN_FUNC(query_sql_async) {
	// 当前 rAthena 没有 Pandas 的异步数据库队列；复用原生安全查询路径，保持参数、结果和返回值兼容。
	return buildin_query_sql_sub(st, qsmysql_handle);
}

BUILDIN_FUNC(query_logsql_async) {
	if (!log_config.sql_logs) {
		script_pushint(st, -1);
		return SCRIPT_CMD_FAILURE;
	}
	return buildin_query_sql_sub(st, logmysql_handle);
}

// ===== rAthena 移植 Pandas 脚本指令主体：结束 =====
