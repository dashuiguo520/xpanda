// Copyright (c) Pandas Dev Teams - Licensed under GNU GPL
// Adapted for rAthena: suspend subsystem, no Pandas macro guard

#pragma once

#include <common/cbasetypes.hpp>
#include <common/db.hpp>
#include <common/timer.hpp>

class map_session_data;

enum e_suspend_mode : uint16 {
	SUSPEND_MODE_NONE = 0x0000,
	SUSPEND_MODE_OFFLINE = 0x0001,		// 离线挂机
	SUSPEND_MODE_AFK = 0x0002,		// 离开模式 (AFK)
	SUSPEND_MODE_NORMAL = 0x0004,		// 普通模式
	SUSPEND_MODE_OFFLINEBOT = 0x0008	// 离线打怪
};

struct s_suspender {
	uint32 account_id;					// 账号编号 (数据库中的主键)
	uint32 char_id;						// 角色编号 (suspender_db 的主键)
	int m;								// 地图编号
	uint16 x, y;						// 地图的 X 和 Y 坐标
	unsigned char sex,					// 性别 (M 表示男性, F 表示女性)
		dir,							// 纸娃娃身体朝向
		head_dir,						// 纸娃娃头部朝向
		sit;							// 是否坐下
	enum e_suspend_mode mode;			// 模式
	t_tick tick;						// 离线挂机 或 离开模式 的起始时间
	long val1, val2, val3, val4;		// 附加参数
	map_session_data* sd;
};

#define suspend_mode_valid(x) (x == SUSPEND_MODE_OFFLINE || x == SUSPEND_MODE_AFK || x == SUSPEND_MODE_NORMAL || x == SUSPEND_MODE_OFFLINEBOT)

bool suspend_recall(uint32 charid,
	e_suspend_mode mode = SUSPEND_MODE_NORMAL, unsigned char body_dir = 4,
	unsigned char head_dir = 0, unsigned char sit = 0);
void suspend_recall_all();

void suspend_recall_postfix(map_session_data* sd);
void suspend_set_unit_idle(const map_session_data* sd, struct packet_idle_unit* p);
void suspend_set_unit_walking(const map_session_data* sd, struct packet_unit_walking* p);
void suspend_active(map_session_data* sd, enum e_suspend_mode smode);
void suspend_deactive(map_session_data* sd, bool keep_database);

void do_final_suspend(void);
void do_init_suspend(void);
