// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL

#ifndef COMMON_AUTOPLAY_HPP
#define COMMON_AUTOPLAY_HPP

#include "cbasetypes.hpp"

#define AUTOPLAY_CONFIG_VERSION 11
#define AUTOPLAY_MAX_SKILLS_PER_TARGET 5
#define AUTOPLAY_MAX_SKILLS (AUTOPLAY_MAX_SKILLS_PER_TARGET * 5)
#define AUTOPLAY_MAX_RECOVERY_ITEMS 5
#define AUTOPLAY_MAX_MONSTER_RULES 10
#define AUTOPLAY_MAX_DISCARD_ITEMS 10
#define AUTOPLAY_MAX_LOOT_ITEMS 10
#define AUTOPLAY_AUTOLOOT_TYPE_MASK 0x1DFD

// Versioned fixed-size configuration shared by map, char, and web control.

enum e_autoplay_state : uint8 {
	AUTOPLAY_STOPPED = 0,
	AUTOPLAY_LOADING,
	AUTOPLAY_IDLE,
	AUTOPLAY_RECOVERING,
	AUTOPLAY_SEARCHING,
	AUTOPLAY_MOVING,
	AUTOPLAY_ATTACKING,
	AUTOPLAY_CASTING,
	AUTOPLAY_TELEPORTING,
	AUTOPLAY_PAUSED,
	AUTOPLAY_ERROR,
};

enum e_autoplay_control : uint8 {
	AUTOPLAY_CONTROL_START = 1,
	AUTOPLAY_CONTROL_PAUSE,
	AUTOPLAY_CONTROL_RESUME,
	AUTOPLAY_CONTROL_STOP,
};

enum e_autoplay_stop_reason : uint8 {
	AUTOPLAY_STOP_NONE = 0,
	AUTOPLAY_STOP_USER,
	AUTOPLAY_STOP_DEAD,
	AUTOPLAY_STOP_MAP,
	AUTOPLAY_STOP_INTERACTION,
	AUTOPLAY_STOP_CONFIG,
	AUTOPLAY_STOP_RESOURCE,
	AUTOPLAY_STOP_PARTY,
};

enum e_autoplay_death_action : uint8 {
	AUTOPLAY_DEATH_STOP = 0,
	AUTOPLAY_DEATH_RESPAWN,
	AUTOPLAY_DEATH_TOKEN_7621,
	AUTOPLAY_DEATH_TOKEN_25310,
	AUTOPLAY_DEATH_TOKEN_6316,
};

enum e_autoplay_mode : uint8 {
	AUTOPLAY_MODE_SEARCH = 0,
	AUTOPLAY_MODE_FIXED,
};

enum e_autoplay_boss_action : uint8 {
	AUTOPLAY_BOSS_AVOID = 1,
	AUTOPLAY_BOSS_RETALIATE,
	AUTOPLAY_BOSS_PRIORITY,
};

enum e_autoplay_follow_offline_action : uint8 {
	AUTOPLAY_FOLLOW_OFFLINE_WAIT = 0,
	AUTOPLAY_FOLLOW_OFFLINE_CONTINUE,
	AUTOPLAY_FOLLOW_OFFLINE_RETURN,
	AUTOPLAY_FOLLOW_OFFLINE_LOGOUT,
};

enum e_autoplay_monster_action : uint8 {
	AUTOPLAY_MONSTER_PRIORITY = 1,
	AUTOPLAY_MONSTER_EXCLUDE,
	AUTOPLAY_MONSTER_AVOID,
};

enum e_autoplay_skill_target : uint8 {
	AUTOPLAY_SKILL_ENEMY = 0,
	AUTOPLAY_SKILL_SELF_BUFF,
	AUTOPLAY_SKILL_SELF_HEAL,
	AUTOPLAY_SKILL_PARTY_BUFF,
	AUTOPLAY_SKILL_PARTY_HEAL,
};

struct s_autoplay_skill_config {
	uint16 skill_id;
	uint16 skill_lv;
	uint8 enabled;
	uint8 target_type;
	uint8 hp_min;
	uint8 hp_max;
};

struct s_autoplay_recovery_config {
	uint32 item_id;
	uint8 enabled;
	uint8 resource_type;
	uint8 trigger_below;
	uint8 reserved;
};

struct s_autoplay_monster_config {
	uint32 mob_id;
	uint8 action;
	uint8 reserved[3];
};

struct s_autoplay_discard_config {
	uint32 item_id;
	uint32 keep_amount;
};

struct s_autoplay_config {
	uint16 version = AUTOPLAY_CONFIG_VERSION;
	uint16 search_range = 12;
	uint32 revision = 0;
	uint16 autoloot_rate = 10000;
	uint16 teleport_skill_id = 0;
	uint32 teleport_item_id = 0;
	uint32 teleport_min_interval = 10000;
	uint8 normal_attack = 1;
	uint8 hp_recover_below = 50;
	uint8 sp_recover_below = 20;
	uint8 emergency_hp_below = 15;
	uint8 death_action = 0;
	uint8 skill_count = 0;
	uint8 recovery_count = 0;
	uint8 monster_count = 0;
	s_autoplay_skill_config skills[AUTOPLAY_MAX_SKILLS]{};
	s_autoplay_recovery_config recovery_items[AUTOPLAY_MAX_RECOVERY_ITEMS]{};
	s_autoplay_monster_config monster_rules[AUTOPLAY_MAX_MONSTER_RULES]{};
	uint32 follow_char_id = 0;
	uint16 follow_distance = 3;
	uint8 follow_attack = 1;
	uint8 auto_accept_party = 0;
	uint8 inventory_stop_at = 0;
	uint8 discard_count = 0;
	uint16 autoloot_types = 0;
	uint8 autoloot_item_count = 0;
	uint8 sit_recovery = 0;
	uint8 sit_hp_below = 30;
	uint8 sit_sp_below = 20;
	s_autoplay_discard_config discard_items[AUTOPLAY_MAX_DISCARD_ITEMS]{};
	uint32 autoloot_items[AUTOPLAY_MAX_LOOT_ITEMS]{};
	uint16 no_monster_teleport_seconds = 0;
	uint8 surrounded_teleport_count = 0;
	uint8 mode = AUTOPLAY_MODE_SEARCH;
	uint8 boss_action = AUTOPLAY_BOSS_RETALIATE;
	uint8 miss_teleport = 0;
	uint8 storage_weight_at = 70;
	uint8 follow_offline_action = AUTOPLAY_FOLLOW_OFFLINE_WAIT;
};

static_assert(sizeof(s_autoplay_config) == 492, "autoplay wire config size changed");

#endif
