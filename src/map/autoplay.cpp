#include "autoplay.hpp"

#include <algorithm>
#include <climits>
#include <cstring>
#include <queue>
#include <vector>

#include <common/random.hpp>
#include <common/showmsg.hpp>

#include "battle.hpp"
#include "clif.hpp"
#include "intif.hpp"
#include "map.hpp"
#include "mob.hpp"
#include "npc.hpp"
#include "party.hpp"
#include "pc.hpp"
#include "skill.hpp"
#include "status.hpp"
#include "unit.hpp"

#define AUTOPLAY_INTERVAL 250
#define AUTOPLAY_BATCH_SIZE 50
static size_t autoplay_cursor = 0;

// Dispatch online characters in batches and reuse native movement and combat entry points.
struct s_target_search {
	map_session_data* sd;
	const s_autoplay_config* config;
	block_list* target;
	int32 nearest_distance;
	bool priority;
};

struct s_avoid_search {
	map_session_data* sd;
	const s_autoplay_config* config;
	block_list* threat;
	int32 nearest_distance;
};

struct s_sit_danger_search {
	map_session_data* sd;
	bool found;
};

static uint8 autoplay_monster_action(const s_autoplay_config& config, int32 mob_id) {
	for (uint8 i = 0; i < config.monster_count && i < AUTOPLAY_MAX_MONSTER_RULES; ++i)
		if (config.monster_rules[i].mob_id == static_cast<uint32>(mob_id))
			return config.monster_rules[i].action;
	return 0;
}

static int32 autoplay_target_sub(block_list* bl, va_list ap) {
	auto* search = va_arg(ap, s_target_search*);
	if (bl == nullptr || bl->type != BL_MOB || status_isdead(*bl))
		return 0;
	if (battle_check_target(search->sd, bl, BCT_ENEMY) <= 0)
		return 0;
	const uint8 action = autoplay_monster_action(*search->config, static_cast<mob_data*>(bl)->mob_id);
	if (action == AUTOPLAY_MONSTER_EXCLUDE || action == AUTOPLAY_MONSTER_AVOID)
		return 0;
	const bool priority = action == AUTOPLAY_MONSTER_PRIORITY;
	const int32 target_distance = distance_bl(search->sd, bl);
	if ((!priority && search->priority) || (priority == search->priority && target_distance >= search->nearest_distance) || !unit_can_reach_bl(search->sd, bl, 0, 0, nullptr, nullptr))
		return 0;
	search->target = bl;
	search->nearest_distance = target_distance;
	search->priority = priority;
	return 1;
}

static int32 autoplay_avoid_sub(block_list* bl, va_list ap) {
	auto* search = va_arg(ap, s_avoid_search*);
	if (bl == nullptr || bl->type != BL_MOB || status_isdead(*bl) || battle_check_target(search->sd, bl, BCT_ENEMY) <= 0 ||
		autoplay_monster_action(*search->config, static_cast<mob_data*>(bl)->mob_id) != AUTOPLAY_MONSTER_AVOID)
		return 0;
	const int32 threat_distance = distance_bl(search->sd, bl);
	if (threat_distance >= search->nearest_distance)
		return 0;
	search->threat = bl;
	search->nearest_distance = threat_distance;
	return 1;
}

static bool autoplay_avoid(map_session_data& sd, s_autoplay_session& session) {
	s_avoid_search search{ &sd, &session.config, nullptr, session.config.search_range + 1 };
	map_foreachinrange(autoplay_avoid_sub, &sd, session.config.search_range, BL_MOB, &search);
	if (search.threat == nullptr) {
		session.avoid_id = 0;
		return false;
	}
	if (session.avoid_id == search.threat->id && unit_is_walking(&sd))
		return true;

	const int32 radius = std::clamp<int32>(session.config.search_range / 2, 3, 8);
	int16 best_x = sd.x, best_y = sd.y;
	int64 best_distance = -1;
	for (int32 dx = -1; dx <= 1; ++dx) {
		for (int32 dy = -1; dy <= 1; ++dy) {
			if (dx == 0 && dy == 0)
				continue;
			const int16 x = static_cast<int16>(std::clamp<int32>(sd.x + dx * radius, 1, map[sd.m].xs - 2));
			const int16 y = static_cast<int16>(std::clamp<int32>(sd.y + dy * radius, 1, map[sd.m].ys - 2));
			const int64 distance = static_cast<int64>(x - search.threat->x) * (x - search.threat->x) + static_cast<int64>(y - search.threat->y) * (y - search.threat->y);
			if (distance > best_distance && unit_can_reach_pos(&sd, x, y, 0)) {
				best_x = x;
				best_y = y;
				best_distance = distance;
			}
		}
	}

	unit_stop_attack(&sd);
	session.target_id = 0;
	session.avoid_id = search.threat->id;
	session.state = AUTOPLAY_MOVING;
	if (best_distance >= 0) {
		unit_stop_walking(&sd, USW_FORCE_STOP | USW_RELEASE_TARGET);
		if (unit_walktoxy(&sd, best_x, best_y, 0))
			return true;
	}
	session.state = AUTOPLAY_IDLE;
	++session.path_failures;
	return true;
}

static bool autoplay_forbidden(const map_session_data& sd) {
	return !sd.state.active || sd.state.warping || sd.npc_id != 0 || sd.st != nullptr ||
		sd.state.vending || sd.state.buyingstore || sd.state.storage_flag != 0 ||
		sd.state.mail_writing || sd.state.banking ||
		map_getmapflag(sd.m, MF_PVP) || map_getmapflag(sd.m, MF_GVG) ||
		map_getmapflag(sd.m, MF_GVG_TE) || map_getmapflag(sd.m, MF_NOAUTOPLAY);
}

static bool autoplay_is_custom_warper(npc_data* npc) {
	if (npc == nullptr)
		return false;
	npc_data* source = npc->master_nd != nullptr ? npc->master_nd : npc;
	return npc->subtype == NPCTYPE_SCRIPT && !npc->is_invisible && std::strcmp(source->name, "Warper") == 0;
}

static npc_data* autoplay_custom_warper(map_session_data& sd) {
	npc_data* npc = map_id2nd(sd.npc_id);
	return autoplay_is_custom_warper(npc) ? npc : nullptr;
}

static npc_data* autoplay_nearest_warper(map_session_data& sd) {
	npc_data* nearest = nullptr;
	int32 nearest_distance = INT_MAX;
	for (int32 i = map[sd.m].npc_num_area; i < map[sd.m].npc_num; ++i) {
		npc_data* npc = map[sd.m].npc[i];
		const int32 npc_distance = npc != nullptr ? distance_bl(&sd, npc) : INT_MAX;
		if (autoplay_is_custom_warper(npc) && npc_distance < nearest_distance && unit_can_reach_bl(&sd, npc, 0, 0, nullptr, nullptr)) {
			nearest = npc;
			nearest_distance = npc_distance;
		}
	}
	return nearest;
}

static npc_data* autoplay_touching_warp(const map_session_data& sd) {
	for (int32 i = 0; i < map[sd.m].npc_num_warp; ++i) {
		npc_data* warp = map[sd.m].npc[i];
		if (warp != nullptr && !warp->is_invisible && sd.x >= warp->x - warp->u.warp.xs && sd.x <= warp->x + warp->u.warp.xs &&
			sd.y >= warp->y - warp->u.warp.ys && sd.y <= warp->y + warp->u.warp.ys)
			return warp;
	}
	return nullptr;
}

static bool autoplay_map_allowed(int16 map_id) {
	return map_id >= 0 && map_id < map_num && map[map_id].cell != nullptr &&
		!map_getmapflag(map_id, MF_PVP) && !map_getmapflag(map_id, MF_GVG) &&
		!map_getmapflag(map_id, MF_GVG_TE) && !map_getmapflag(map_id, MF_NOAUTOPLAY);
}

static npc_data* autoplay_next_warp(int16 source_map, int16 destination_map) {
	if (!autoplay_map_allowed(source_map) || !autoplay_map_allowed(destination_map))
		return nullptr;
	std::vector<bool> visited(map_num, false);
	std::vector<int32> first_warp(map_num, 0);
	std::queue<int16> pending;
	visited[source_map] = true;
	pending.push(source_map);
	while (!pending.empty()) {
		const int16 current_map = pending.front();
		pending.pop();
		for (int32 i = 0; i < map[current_map].npc_num_warp; ++i) {
			npc_data* warp = map[current_map].npc[i];
			if (warp == nullptr || warp->subtype != NPCTYPE_WARP || warp->is_invisible)
				continue;
			const int16 next_map = map_mapindex2mapid(warp->u.warp.mapindex);
			if (!autoplay_map_allowed(next_map) || visited[next_map])
				continue;
			visited[next_map] = true;
			first_warp[next_map] = current_map == source_map ? warp->id : first_warp[current_map];
			if (next_map == destination_map)
				return map_id2nd(first_warp[next_map]);
			pending.push(next_map);
		}
	}
	return nullptr;
}

static bool autoplay_map_has_warper(int16 map_id) {
	if (!autoplay_map_allowed(map_id))
		return false;
	for (int32 i = map[map_id].npc_num_area; i < map[map_id].npc_num; ++i)
		if (autoplay_is_custom_warper(map[map_id].npc[i]))
			return true;
	return false;
}

static npc_data* autoplay_next_warp_to_warper(int16 source_map) {
	if (!autoplay_map_allowed(source_map))
		return nullptr;
	std::vector<bool> visited(map_num, false);
	std::vector<int32> first_warp(map_num, 0);
	std::queue<int16> pending;
	visited[source_map] = true;
	pending.push(source_map);
	while (!pending.empty()) {
		const int16 current_map = pending.front();
		pending.pop();
		for (int32 i = 0; i < map[current_map].npc_num_warp; ++i) {
			npc_data* warp = map[current_map].npc[i];
			if (warp == nullptr || warp->subtype != NPCTYPE_WARP || warp->is_invisible)
				continue;
			const int16 next_map = map_mapindex2mapid(warp->u.warp.mapindex);
			if (!autoplay_map_allowed(next_map) || visited[next_map])
				continue;
			visited[next_map] = true;
			first_warp[next_map] = current_map == source_map ? warp->id : first_warp[current_map];
			if (autoplay_map_has_warper(next_map))
				return map_id2nd(first_warp[next_map]);
			pending.push(next_map);
		}
	}
	return nullptr;
}

static void autoplay_set_warp_step(s_autoplay_transition& step, npc_data& warp) {
	step.destination_mapindex = warp.u.warp.mapindex;
	step.source_map = warp.m;
	step.source_x = warp.x;
	step.source_y = warp.y;
	step.destination_x = warp.u.warp.x;
	step.destination_y = warp.u.warp.y;
	step.npc_id = warp.id;
	step.kind = AUTOPLAY_TRANSITION_WARP;
}

static bool autoplay_walk_transition(map_session_data& sd, s_autoplay_session& session, const s_autoplay_transition& transition);

static bool autoplay_route_to_map(map_session_data& sd, s_autoplay_session& session, int16 destination_map,
	int16 destination_x, int16 destination_y, s_autoplay_transition& step, int16& route_map,
	const s_autoplay_transition* preferred = nullptr) {
	if (sd.m == destination_map) {
		step = {};
		route_map = -1;
		return false;
	}
	if (!autoplay_map_allowed(destination_map)) {
		session.state = AUTOPLAY_PAUSED;
		session.stop_reason = AUTOPLAY_STOP_MAP;
		return true;
	}
	if (step.source_map != sd.m || route_map != destination_map) {
		step = {};
		step.source_map = sd.m;
		route_map = destination_map;
		if (npc_data* warp = autoplay_next_warp(sd.m, destination_map))
			autoplay_set_warp_step(step, *warp);
		else if (preferred != nullptr && preferred->source_map == sd.m &&
			map_mapindex2mapid(preferred->destination_mapindex) == destination_map)
			step = *preferred;
		else if (npc_data* warper = autoplay_nearest_warper(sd)) {
			step.destination_mapindex = map_id2index(destination_map);
			step.source_x = warper->x;
			step.source_y = warper->y;
			step.destination_x = destination_x;
			step.destination_y = destination_y;
			step.npc_id = warper->id;
			step.kind = AUTOPLAY_TRANSITION_WARPER;
		} else if (npc_data* warp = autoplay_next_warp_to_warper(sd.m))
			autoplay_set_warp_step(step, *warp);
	}
	if (step.kind == AUTOPLAY_TRANSITION_NONE) {
		session.state = AUTOPLAY_PAUSED;
		session.stop_reason = AUTOPLAY_STOP_MAP;
		return true;
	}
	return autoplay_walk_transition(sd, session, step);
}

static bool autoplay_walk_transition(map_session_data& sd, s_autoplay_session& session, const s_autoplay_transition& transition) {
	unit_stop_attack(&sd);
	session.target_id = 0;
	session.state = AUTOPLAY_MOVING;
	if (transition.source_map != sd.m || !autoplay_map_allowed(map_mapindex2mapid(transition.destination_mapindex))) {
		session.state = AUTOPLAY_PAUSED;
		session.stop_reason = AUTOPLAY_STOP_MAP;
		return true;
	}
	npc_data* npc = map_id2nd(transition.npc_id);
	if (npc == nullptr || npc->m != sd.m) {
		session.state = AUTOPLAY_PAUSED;
		session.stop_reason = AUTOPLAY_STOP_MAP;
		return true;
	}
	if (transition.kind == AUTOPLAY_TRANSITION_WARP) {
		if (sd.x >= npc->x - npc->u.warp.xs && sd.x <= npc->x + npc->u.warp.xs &&
			sd.y >= npc->y - npc->u.warp.ys && sd.y <= npc->y + npc->u.warp.ys) {
			npc_touch_areanpc(&sd, sd.m, sd.x, sd.y, npc);
			return true;
		}
	} else if (transition.kind == AUTOPLAY_TRANSITION_WARPER && check_distance_bl(&sd, npc, 3)) {
		pc_setregstr(&sd, add_str("@autoplay_warp_map$"), mapindex_id2name(transition.destination_mapindex));
		pc_setreg(&sd, add_str("@autoplay_warp_x"), transition.destination_x);
		pc_setreg(&sd, add_str("@autoplay_warp_y"), transition.destination_y);
		npc_event(&sd, "Warper::OnAutoplayWarp", 0);
		return true;
	}
	if (!unit_is_walking(&sd) && !unit_walktoxy(&sd, transition.source_x, transition.source_y, 0)) {
		session.state = AUTOPLAY_PAUSED;
		session.stop_reason = AUTOPLAY_STOP_MAP;
		++session.path_failures;
	}
	return true;
}

static bool autoplay_stand(map_session_data& sd, s_autoplay_session& session) {
	if (!session.sitting_recovery)
		return true;
	if (pc_issit(&sd) && !pc_setstand(&sd, false))
		return false;
	if (!pc_issit(&sd))
		skill_sit(&sd, false);
	session.sitting_recovery = false;
	return true;
}

static bool autoplay_use_recovery(map_session_data& sd, s_autoplay_session& session) {
	const auto& status = sd.battle_status;
	const uint8 hp = status.max_hp > 0 ? static_cast<uint8>(status.hp * 100 / status.max_hp) : 0;
	const uint8 sp = status.max_sp > 0 ? static_cast<uint8>(status.sp * 100 / status.max_sp) : 0;
	for (size_t i = 0; i < session.config.recovery_count && i < 5; ++i) {
		const auto& rule = session.config.recovery_items[i];
		const uint8 value = rule.resource_type == 1 ? sp : hp;
		if (!rule.enabled || value > rule.trigger_below)
			continue;
		for (int32 index = 0; index < MAX_INVENTORY; ++index) {
			if (sd.inventory.u.items_inventory[index].nameid == rule.item_id && sd.inventory.u.items_inventory[index].amount > 0) {
				if (!autoplay_stand(sd, session))
					return true;
				if (pc_useitem(&sd, index)) {
					session.state = AUTOPLAY_RECOVERING;
					return true;
				}
			}
		}
	}
	return false;
}

static int32 autoplay_sit_danger_sub(block_list* bl, va_list ap) {
	auto* search = va_arg(ap, s_sit_danger_search*);
	if (bl == nullptr || bl->type != BL_MOB || status_isdead(*bl) || battle_check_target(search->sd, bl, BCT_ENEMY) <= 0)
		return 0;
	const auto* md = static_cast<mob_data*>(bl);
	if (md->target_id == search->sd->id || md->attacked_id == search->sd->id || md->norm_attacked_id == search->sd->id || status_has_mode(&md->status, MD_AGGRESSIVE)) {
		search->found = true;
		return 1;
	}
	return 0;
}

static uint8 autoplay_sp_percent(const map_session_data& sd) {
	return sd.battle_status.max_sp > 0 ? static_cast<uint8>(sd.battle_status.sp * 100 / sd.battle_status.max_sp) : 0;
}

static bool autoplay_manage_sit_recovery(map_session_data& sd, s_autoplay_session& session) {
	if (!session.config.sit_recovery && !session.sitting_recovery)
		return false;
	s_sit_danger_search search{ &sd, false };
	map_foreachinrange(autoplay_sit_danger_sub, &sd, session.config.search_range, BL_MOB, &search);
	const bool danger = search.found || sd.ud.attacktimer != INVALID_TIMER || sd.ud.skilltimer != INVALID_TIMER || session.target_id != 0;
	const uint8 hp = sd.battle_status.max_hp > 0 ? static_cast<uint8>(sd.battle_status.hp * 100 / sd.battle_status.max_hp) : 0;
	const uint8 sp = autoplay_sp_percent(sd);

	if (session.sitting_recovery) {
		if (!pc_issit(&sd)) {
			session.sitting_recovery = false;
			return false;
		}
		if (!session.config.sit_recovery || danger || (hp >= 100 && sp >= 100)) {
			autoplay_stand(sd, session);
			session.state = AUTOPLAY_IDLE;
			return true;
		}
		session.state = AUTOPLAY_RECOVERING;
		return true;
	}

	if (!session.config.sit_recovery || (hp > session.config.sit_hp_below && sp > session.config.sit_sp_below) || danger)
		return false;
	if ((battle_config.basic_skill_check && pc_checkskill(&sd, NV_BASIC) < 3 && pc_checkskill(&sd, SU_BASIC_SKILL) < 1) ||
		(sd.sc.opt1 && sd.sc.opt1 != OPT1_STONEWAIT && sd.sc.opt1 != OPT1_BURNING) ||
		(!sd.sc.empty() && (sd.sc.getSCE(SC_DANCING) || (sd.sc.getSCE(SC_GRAVITATION) && sd.sc.getSCE(SC_GRAVITATION)->val3 == BCT_SELF))) ||
		(sd.state.block_action & PCBLOCK_SITSTAND))
		return false;

	unit_stop_attack(&sd);
	unit_stop_walking(&sd, USW_FIXPOS | USW_MOVE_FULL_CELL);
	session.target_id = 0;
	pc_setsit(&sd);
	skill_sit(&sd, true);
	clif_sitting(sd);
	session.sitting_recovery = true;
	session.state = AUTOPLAY_RECOVERING;
	return true;
}

static bool autoplay_manage_inventory(map_session_data& sd, s_autoplay_session& session) {
	if (session.config.inventory_stop_at == 0 || sd.status.inventory_slots == 0)
		return false;
	size_t used = 0;
	for (int32 i = 0; i < sd.status.inventory_slots; ++i)
		used += sd.inventory.u.items_inventory[i].nameid != 0;
	if (used * 100 < static_cast<size_t>(sd.status.inventory_slots) * session.config.inventory_stop_at)
		return false;
	for (uint8 rule = 0; rule < session.config.discard_count; ++rule) {
		const auto& discard = session.config.discard_items[rule];
		uint32 total = 0;
		for (int32 i = 0; i < sd.status.inventory_slots; ++i) {
			const item& entry = sd.inventory.u.items_inventory[i];
			if (entry.nameid == discard.item_id)
				total += entry.amount;
		}
		if (total <= discard.keep_amount)
			continue;
		for (int32 i = 0; i < sd.status.inventory_slots; ++i) {
			const item& entry = sd.inventory.u.items_inventory[i];
			if (entry.nameid != discard.item_id || entry.equip != 0 || entry.equipSwitch != 0 || entry.favorite != 0 || entry.expire_time != 0)
				continue;
			const int16 amount = static_cast<int16>(std::min<uint32>(entry.amount, total - discard.keep_amount));
			if (amount > 0 && pc_dropitem(&sd, i, amount)) {
				session.state = AUTOPLAY_RECOVERING;
				return true;
			}
		}
	}
	unit_stop_attack(&sd);
	session.target_id = 0;
	session.state = AUTOPLAY_PAUSED;
	session.stop_reason = AUTOPLAY_STOP_RESOURCE;
	return true;
}

static void autoplay_apply_autoloot(map_session_data& sd, const s_autoplay_config& config) {
	sd.state.autoloot = std::min<uint16>(config.autoloot_rate, 10000);
	sd.state.autoloottype = config.autoloot_types;
	std::fill(std::begin(sd.state.autolootid), std::end(sd.state.autolootid), 0);
	for (uint8 i = 0; i < config.autoloot_item_count && i < AUTOPLAY_MAX_LOOT_ITEMS; ++i)
		sd.state.autolootid[i] = config.autoloot_items[i];
	sd.state.autolooting = config.autoloot_item_count > 0;
}

static void autoplay_restore_autoloot(map_session_data& sd, s_autoplay_session& session) {
	if (!session.autoloot_overridden)
		return;
	sd.state.autoloot = session.previous_autoloot;
	sd.state.autoloottype = session.previous_autoloottype;
	std::copy(std::begin(session.previous_autolootids), std::end(session.previous_autolootids), std::begin(sd.state.autolootid));
	sd.state.autolooting = session.previous_autolooting;
	session.autoloot_overridden = false;
}

static bool autoplay_emergency_teleport(map_session_data& sd, s_autoplay_session& session, t_tick tick) {
	const auto& status = sd.battle_status;
	const uint8 hp = status.max_hp > 0 ? static_cast<uint8>(status.hp * 100 / status.max_hp) : 0;
	if (hp > session.config.emergency_hp_below || DIFF_TICK(tick, session.last_teleport_tick) < session.config.teleport_min_interval)
		return false;
	if (!autoplay_stand(sd, session))
		return true;
	if (session.config.teleport_skill_id != 0) {
		const uint16 learned = pc_checkskill(&sd, session.config.teleport_skill_id);
		if (learned > 0 && unit_skilluse_id(&sd, sd.id, session.config.teleport_skill_id, learned)) {
			session.last_teleport_tick = tick;
			session.next_action_tick = std::max(tick, sd.ud.canact_tick);
			session.state = AUTOPLAY_CASTING;
			return true;
		}
	}
	if (session.config.teleport_item_id != 0) {
		for (int32 index = 0; index < MAX_INVENTORY; ++index) {
			if (sd.inventory.u.items_inventory[index].nameid == session.config.teleport_item_id && pc_useitem(&sd, index)) {
				session.last_teleport_tick = tick;
				session.state = AUTOPLAY_RECOVERING;
				return true;
			}
		}
	}
	return false;
}

static uint16 autoplay_skill_level(map_session_data& sd, const s_autoplay_skill_config& rule) {
	const uint16 learned = pc_checkskill(&sd, rule.skill_id);
	if (!rule.enabled || learned == 0)
		return 0;
	const uint16 level = std::min<uint16>(learned, rule.skill_lv);
	return static_cast<int64>(status_get_sp(&sd)) >= skill_get_requirement(&sd, rule.skill_id, level).sp ? level : 0;
}

static uint8 autoplay_hp_percent(const map_session_data& sd) {
	return sd.battle_status.max_hp > 0 ? static_cast<uint8>(sd.battle_status.hp * 100 / sd.battle_status.max_hp) : 0;
}

static map_session_data* autoplay_follow_target(map_session_data& sd, uint32 follow_char_id) {
	map_session_data* target = follow_char_id != 0 ? map_charid2sd(follow_char_id) : nullptr;
	return target != &sd && target != nullptr && target->state.active && !status_isdead(*target) && sd.status.party_id != 0 && target->status.party_id == sd.status.party_id && target->m == sd.m ? target : nullptr;
}

static map_session_data* autoplay_party_skill_target(map_session_data& sd, const s_autoplay_skill_config& rule, uint16 level) {
	party_data* party = party_search(sd.status.party_id);
	if (party == nullptr)
		return nullptr;
	const int32 range = std::max(1, skill_get_range2(&sd, rule.skill_id, level, true));
	map_session_data* best = nullptr;
	uint8 best_hp = 101;
	for (const auto& member : party->data) {
		map_session_data* target = member.sd;
		if (target == nullptr || target == &sd || target->m != sd.m || status_isdead(*target) || !battle_check_range(&sd, target, range))
			continue;
		const uint8 hp = autoplay_hp_percent(*target);
		if (hp < rule.hp_min || hp > rule.hp_max)
			continue;
		if (rule.target_type == AUTOPLAY_SKILL_PARTY_BUFF) {
			const sc_type status = skill_get_sc(rule.skill_id);
			if (status == SC_NONE || target->sc.getSCE(status) != nullptr)
				continue;
		}
		if (hp < best_hp) {
			best = target;
			best_hp = hp;
		}
	}
	return best;
}

static bool autoplay_cast_support(map_session_data& sd, s_autoplay_session& session) {
	for (size_t i = 0; i < session.config.skill_count && i < AUTOPLAY_MAX_SKILLS; ++i) {
		const auto& rule = session.config.skills[i];
		if (rule.target_type == AUTOPLAY_SKILL_ENEMY)
			continue;
		const uint16 level = autoplay_skill_level(sd, rule);
		if (level == 0)
			continue;
		map_session_data* target = &sd;
		if (rule.target_type >= AUTOPLAY_SKILL_PARTY_BUFF) {
			target = autoplay_party_skill_target(sd, rule, level);
			if (target == nullptr)
				continue;
		} else {
			const uint8 hp = autoplay_hp_percent(sd);
			if (hp < rule.hp_min || hp > rule.hp_max)
				continue;
		}
		if (rule.target_type == AUTOPLAY_SKILL_SELF_BUFF) {
			const sc_type status = skill_get_sc(rule.skill_id);
			if (status == SC_NONE || sd.sc.getSCE(status) != nullptr)
				continue;
		}
		const bool started = skill_get_inf(rule.skill_id) & INF_GROUND_SKILL
			? unit_skilluse_pos(&sd, target->x, target->y, rule.skill_id, level) != 0
			: unit_skilluse_id(&sd, target->id, rule.skill_id, level) != 0;
		if (started) {
			session.next_action_tick = std::max(gettick(), sd.ud.canact_tick);
			session.state = AUTOPLAY_CASTING;
			return true;
		}
	}
	return false;
}

static bool autoplay_follow(map_session_data& sd, s_autoplay_session& session, map_session_data& target) {
	if (distance_bl(&sd, &target) <= session.config.follow_distance)
		return false;
	unit_stop_attack(&sd);
	session.target_id = 0;
	session.state = AUTOPLAY_MOVING;
	unit_data* ud = unit_bl2ud(&sd);
	if ((!unit_is_walking(&sd) || ud == nullptr || ud->target_to != target.id) && !unit_walktobl(&sd, &target, session.config.follow_distance, 0)) {
		session.state = AUTOPLAY_PAUSED;
		session.stop_reason = AUTOPLAY_STOP_PARTY;
		++session.path_failures;
	}
	return true;
}

static bool autoplay_cast(map_session_data& sd, s_autoplay_session& session, block_list& target) {
	for (size_t i = 0; i < session.config.skill_count && i < 5; ++i) {
		const auto& rule = session.config.skills[i];
		if (rule.target_type != AUTOPLAY_SKILL_ENEMY)
			continue;
		const uint16 level = autoplay_skill_level(sd, rule);
		if (level == 0)
			continue;
		const bool started = skill_get_inf(rule.skill_id) & INF_GROUND_SKILL
			? unit_skilluse_pos(&sd, target.x, target.y, rule.skill_id, level) != 0
			: unit_skilluse_id(&sd, target.id, rule.skill_id, level) != 0;
		if (started) {
			session.next_action_tick = std::max(gettick(), sd.ud.canact_tick);
			session.state = AUTOPLAY_CASTING;
			return true;
		}
	}
	return false;
}

static int32 autoplay_skill_range(map_session_data& sd, const s_autoplay_session& session) {
	for (size_t i = 0; i < session.config.skill_count && i < 5; ++i) {
		const auto& rule = session.config.skills[i];
		if (rule.target_type != AUTOPLAY_SKILL_ENEMY)
			continue;
		if (const uint16 level = autoplay_skill_level(sd, rule); level > 0)
			return std::max(1, skill_get_range2(&sd, rule.skill_id, level, true));
	}
	return 0;
}

static bool autoplay_move_to_range(map_session_data& sd, s_autoplay_session& session, block_list& target, int32 range) {
	if (battle_check_range(&sd, &target, range))
		return false;
	session.state = AUTOPLAY_MOVING;
	unit_data* ud = unit_bl2ud(&sd);
	if ((!unit_is_walking(&sd) || ud == nullptr || ud->target_to != target.id) && !unit_walktobl(&sd, &target, range, 0)) {
		session.target_id = 0;
		++session.path_failures;
	}
	return true;
}

static void autoplay_roam(map_session_data& sd, s_autoplay_session& session) {
	session.state = AUTOPLAY_MOVING;
	if (unit_is_walking(&sd))
		return;
	const int32 radius = std::clamp<int32>(session.config.search_range, 3, 12);
	for (int32 attempt = 0; attempt < 8; ++attempt) {
		const int16 x = static_cast<int16>(std::clamp<int32>(sd.x + rnd_value(-radius, radius), 1, map[sd.m].xs - 2));
		const int16 y = static_cast<int16>(std::clamp<int32>(sd.y + rnd_value(-radius, radius), 1, map[sd.m].ys - 2));
		if ((x != sd.x || y != sd.y) && unit_can_reach_pos(&sd, x, y, 0) && unit_walktoxy(&sd, x, y, 0))
			return;
	}
	session.state = AUTOPLAY_IDLE;
	++session.path_failures;
}

static void autoplay_tick_character(map_session_data& sd, t_tick tick) {
	if (!sd.autoplay || !sd.autoplay->active)
		return;
	auto& session = *sd.autoplay;
	if (DIFF_TICK(session.next_decision_tick, tick) > 0)
		return;
	session.next_decision_tick = tick + AUTOPLAY_INTERVAL;
	if (status_isdead(sd)) {
		session.sitting_recovery = false;
		unit_stop_attack(&sd);
		session.target_id = 0;
		if (session.config.death_action == AUTOPLAY_DEATH_RESPAWN) {
			session.waiting_return = true;
			session.state = AUTOPLAY_TELEPORTING;
			pc_respawn(&sd, CLR_OUTSIGHT);
			return;
		}
		autoplay_restore_autoloot(sd, session);
		session.active = false;
		session.state = AUTOPLAY_STOPPED;
		session.stop_reason = AUTOPLAY_STOP_DEAD;
		return;
	}
	if (session.waiting_return) {
		if (sd.state.warping)
			return;
		if (sd.m != session.origin_map) {
			autoplay_route_to_map(sd, session, session.origin_map, session.origin_x, session.origin_y,
				session.return_step, session.return_route_map, &session.return_transition);
			return;
		}
		if (!check_distance_xy(sd.x, sd.y, session.origin_x, session.origin_y, session.config.search_range)) {
			session.state = AUTOPLAY_MOVING;
			if (!unit_is_walking(&sd) && !unit_walktoxy(&sd, session.origin_x, session.origin_y, 0)) {
				session.state = AUTOPLAY_PAUSED;
				session.stop_reason = AUTOPLAY_STOP_MAP;
				++session.path_failures;
			}
			return;
		}
		session.waiting_return = false;
		session.return_step = {};
		session.return_route_map = -1;
	}
	if (autoplay_forbidden(sd)) {
		autoplay_stand(sd, session);
		session.state = AUTOPLAY_PAUSED;
		session.stop_reason = AUTOPLAY_STOP_INTERACTION;
		return;
	}
	session.stop_reason = AUTOPLAY_STOP_NONE;
	if (session.pending_follow_transition.kind != AUTOPLAY_TRANSITION_NONE) {
		if (DIFF_TICK(tick, session.pending_follow_expires) > 0 || sd.m != session.pending_follow_transition.source_map)
			session.pending_follow_transition = {};
		else {
			autoplay_walk_transition(sd, session, session.pending_follow_transition);
			return;
		}
	}
	// Do not decide while casting; post-cast delay blocks actions but not search or movement.
	if (sd.ud.skilltimer != INVALID_TIMER)
		return;
	// Cast end can extend the native delay after an autoplay skill was started.
	const bool action_delayed = DIFF_TICK(tick, std::max(session.next_action_tick, sd.ud.canact_tick)) < 0;
	if (!action_delayed && autoplay_emergency_teleport(sd, session, tick))
		return;
	if (!action_delayed && autoplay_use_recovery(sd, session))
		return;
	if (autoplay_manage_sit_recovery(sd, session))
		return;
	if (autoplay_manage_inventory(sd, session))
		return;
	map_session_data* follow = nullptr;
	if (session.follow_char_id != 0) {
		party_data* party = party_search(sd.status.party_id);
		const party_member* follow_member = nullptr;
		if (party != nullptr) {
			auto member = std::find_if(&party->party.member[0], &party->party.member[MAX_PARTY], [&](const party_member& entry) {
				return entry.char_id == session.follow_char_id;
			});
			if (member != &party->party.member[MAX_PARTY])
				follow_member = member;
		}
		const bool no_longer_party_members = sd.status.party_id == 0 || (party != nullptr && follow_member == nullptr);
		if (no_longer_party_members)
			autoplay_set_follow(sd, 0);
		else {
			map_session_data* online_follow = map_charid2sd(session.follow_char_id);
			int16 follow_map = -1;
			int16 follow_x = 0;
			int16 follow_y = 0;
			if (online_follow != nullptr) {
				if (online_follow->state.active && !status_isdead(*online_follow) && online_follow->status.party_id == sd.status.party_id) {
					follow_map = online_follow->m;
					follow_x = online_follow->x;
					follow_y = online_follow->y;
					follow = autoplay_follow_target(sd, session.follow_char_id);
				}
			} else if (follow_member != nullptr && follow_member->online) {
				follow_map = map_mapindex2mapid(mapindex_name2idx(follow_member->map, nullptr));
			}
			if (follow_map >= 0 && follow_map != sd.m) {
				autoplay_route_to_map(sd, session, follow_map, follow_x, follow_y,
					session.follow_step, session.follow_route_map);
				return;
			}
			if (follow != nullptr) {
				session.follow_step = {};
				session.follow_route_map = -1;
			}
		}
		if (!no_longer_party_members && follow == nullptr) {
			session.state = AUTOPLAY_PAUSED;
			session.stop_reason = AUTOPLAY_STOP_PARTY;
			return;
		}
	}
	if (!action_delayed && autoplay_cast_support(sd, session))
		return;
	if (autoplay_avoid(sd, session))
		return;
	if (follow != nullptr && autoplay_follow(sd, session, *follow))
		return;
	if (follow != nullptr && !session.config.follow_attack) {
		session.target_id = 0;
		session.state = AUTOPLAY_IDLE;
		unit_stop_attack(&sd);
		return;
	}

	block_list* target = session.target_id != 0 ? map_id2bl(session.target_id) : nullptr;
	bool target_acquired = false;
	if (target == nullptr || target->m != sd.m || status_isdead(*target) || battle_check_target(&sd, target, BCT_ENEMY) <= 0) {
		s_target_search search{ &sd, &session.config, nullptr, session.config.search_range + 1, false };
		session.state = AUTOPLAY_SEARCHING;
		map_foreachinrange(autoplay_target_sub, &sd, session.config.search_range, BL_MOB, &search);
		target = search.target;
		session.target_id = target ? target->id : 0;
		target_acquired = target != nullptr;
	}
	if (target == nullptr) {
		if (follow != nullptr)
			session.state = AUTOPLAY_IDLE;
		else
			autoplay_roam(sd, session);
		return;
	}
	// Drop the roaming path before the skill or normal attack establishes a chase.
	if (target_acquired && unit_is_walking(&sd))
		unit_stop_walking(&sd, USW_FORCE_STOP | USW_RELEASE_TARGET);
	if (const int32 range = autoplay_skill_range(sd, session); range > 0 && autoplay_move_to_range(sd, session, *target, range))
		return;
	if (action_delayed)
		return;
	if (autoplay_cast(sd, session, *target))
		return;
	if (session.config.normal_attack) {
		if (autoplay_move_to_range(sd, session, *target, status_get_range(&sd)))
			return;
		unit_attack(&sd, target->id, 1);
		session.state = AUTOPLAY_ATTACKING;
	}
}

static int32 autoplay_timer(int32 tid, t_tick tick, int32 id, intptr_t data) {
	std::vector<map_session_data*> active_characters;
	s_mapiterator* iter = mapit_getallusers();
	for (auto* sd = static_cast<map_session_data*>(mapit_first(iter)); mapit_exists(iter); sd = static_cast<map_session_data*>(mapit_next(iter)))
		if (sd->autoplay && sd->autoplay->active)
			active_characters.push_back(sd);
	mapit_free(iter);
	if (active_characters.empty()) {
		autoplay_cursor = 0;
		return 0;
	}
	const size_t count = std::min(static_cast<size_t>(AUTOPLAY_BATCH_SIZE), active_characters.size());
	for (size_t i = 0; i < count; ++i) {
		if (autoplay_cursor >= active_characters.size())
			autoplay_cursor = 0;
		autoplay_tick_character(*active_characters[autoplay_cursor++], tick);
	}
	return 0;
}
void do_init_autoplay() {
	add_timer_func_list(autoplay_timer, "autoplay_timer");
	add_timer_interval(gettick() + AUTOPLAY_INTERVAL, autoplay_timer, 0, 0, AUTOPLAY_INTERVAL);
}

void do_final_autoplay() {
	autoplay_cursor = 0;
}

void autoplay_attach(map_session_data& sd) {
	if (!sd.autoplay)
		sd.autoplay = std::make_shared<s_autoplay_session>();
}

void autoplay_detach(map_session_data& sd) {
	if (sd.autoplay != nullptr) {
		autoplay_stand(sd, *sd.autoplay);
		autoplay_restore_autoloot(sd, *sd.autoplay);
		sd.autoplay->follow_char_id = 0;
		sd.autoplay->active = false;
	}
}

void autoplay_set_follow(map_session_data& sd, uint32 char_id) {
	if (!sd.autoplay)
		return;
	auto& session = *sd.autoplay;
	if (session.follow_char_id != char_id) {
		session.follow_step = {};
		session.follow_route_map = -1;
		session.pending_follow_transition = {};
	}
	session.follow_char_id = char_id;
	if (!session.loaded || session.config.follow_char_id == char_id)
		return;
	session.config.follow_char_id = char_id;
	if (session.save_pending)
		session.pending_config.follow_char_id = char_id;
	intif_autoplay_save(sd.status.char_id, session.save_pending ? session.pending_config : session.config);
}

void autoplay_notify_map_change(map_session_data& sd, uint16 destination_mapindex, int16 destination_x, int16 destination_y) {
	if (!sd.state.active || sd.m < 0 || destination_mapindex == 0 || destination_mapindex == sd.mapindex)
		return;
	autoplay_attach(sd);
	s_autoplay_transition transition{};
	transition.destination_mapindex = destination_mapindex;
	transition.source_map = sd.m;
	transition.destination_x = destination_x;
	transition.destination_y = destination_y;
	if (npc_data* warper = autoplay_custom_warper(sd)) {
		transition.source_x = warper->x;
		transition.source_y = warper->y;
		transition.npc_id = warper->id;
		transition.kind = AUTOPLAY_TRANSITION_WARPER;
	} else if (npc_data* warp = autoplay_touching_warp(sd)) {
		transition.source_x = warp->x;
		transition.source_y = warp->y;
		transition.npc_id = warp->id;
		transition.kind = AUTOPLAY_TRANSITION_WARP;
	}
	if (transition.kind == AUTOPLAY_TRANSITION_NONE)
		return;
	sd.autoplay->last_transition = transition;
	if (sd.status.party_id == 0)
		return;
	party_data* party = party_search(sd.status.party_id);
	if (party == nullptr)
		return;
	const t_tick expires = gettick() + 60000;
	for (auto& member : party->data) {
		map_session_data* follower = member.sd;
		if (follower == nullptr || follower == &sd || follower->m != sd.m || !follower->autoplay ||
			!follower->autoplay->active || follower->autoplay->follow_char_id != sd.status.char_id)
			continue;
		auto& session = *follower->autoplay;
		session.pending_follow_transition = transition;
		session.pending_follow_expires = expires;
	}
}

bool autoplay_control(map_session_data& sd, e_autoplay_control control) {
	autoplay_attach(sd);
	auto& session = *sd.autoplay;
	if (!session.loaded && control != AUTOPLAY_CONTROL_STOP)
		return false;
	if ((control == AUTOPLAY_CONTROL_START || control == AUTOPLAY_CONTROL_RESUME) &&
		(!pc_has_permission(&sd, PC_PERM_AUTOPLAY) || status_isdead(sd) || autoplay_forbidden(sd)))
		return false;
	if (control == AUTOPLAY_CONTROL_START || control == AUTOPLAY_CONTROL_RESUME) {
		session.follow_char_id = session.config.follow_char_id;
		session.origin_map = sd.m;
		session.origin_x = sd.x;
		session.origin_y = sd.y;
		session.return_transition = map_mapindex2mapid(session.last_transition.destination_mapindex) == sd.m ? session.last_transition : s_autoplay_transition{};
		session.return_step = {};
		session.return_route_map = -1;
		session.follow_step = {};
		session.follow_route_map = -1;
		session.pending_follow_transition = {};
		session.waiting_return = false;
	}
	switch (control) {
		case AUTOPLAY_CONTROL_START:
		case AUTOPLAY_CONTROL_RESUME:
			if (!session.autoloot_overridden) {
				session.previous_autoloot = sd.state.autoloot;
				session.previous_autoloottype = sd.state.autoloottype;
				std::copy(std::begin(sd.state.autolootid), std::end(sd.state.autolootid), std::begin(session.previous_autolootids));
				session.previous_autolooting = sd.state.autolooting;
				session.autoloot_overridden = true;
			}
			autoplay_apply_autoloot(sd, session.config);
			session.active = true;
			session.next_action_tick = 0;
			session.state = AUTOPLAY_IDLE;
			session.stop_reason = AUTOPLAY_STOP_NONE;
			return true;
		case AUTOPLAY_CONTROL_PAUSE:
			autoplay_stand(sd, session);
			session.active = false;
			session.state = AUTOPLAY_PAUSED;
			unit_stop_attack(&sd);
			return true;
		case AUTOPLAY_CONTROL_STOP:
			autoplay_detach(sd);
			session.waiting_return = false;
			session.state = AUTOPLAY_STOPPED;
			session.stop_reason = AUTOPLAY_STOP_USER;
			session.target_id = 0;
			unit_stop_attack(&sd);
			return true;
	}
	return false;
}

void autoplay_config_loaded(map_session_data& sd, const s_autoplay_config& config) {
	autoplay_attach(sd);
	sd.autoplay->config = config;
	sd.autoplay->follow_char_id = config.follow_char_id;
	sd.autoplay->loaded = true;
	sd.autoplay->state = AUTOPLAY_STOPPED;
}

void autoplay_config_saved(map_session_data& sd, bool success, uint32 revision) {
	if (!sd.autoplay) return;
	if (success && sd.autoplay->save_pending) {
		sd.autoplay->config = sd.autoplay->pending_config;
		sd.autoplay->config.revision = revision;
		if (sd.autoplay->active)
			autoplay_apply_autoloot(sd, sd.autoplay->config);
	}
	if (success)
		sd.autoplay->config.revision = revision;
	sd.autoplay->save_pending = false;
}
