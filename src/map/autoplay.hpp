#ifndef MAP_AUTOPLAY_HPP
#define MAP_AUTOPLAY_HPP

#include <memory>

#include <common/autoplay.hpp>
#include <common/timer.hpp>

class map_session_data;

enum e_autoplay_transition_kind : uint8 {
	AUTOPLAY_TRANSITION_NONE = 0,
	AUTOPLAY_TRANSITION_WARP,
	AUTOPLAY_TRANSITION_WARPER,
};

struct s_autoplay_transition {
	uint16 destination_mapindex = 0;
	int16 source_map = -1;
	int16 source_x = 0;
	int16 source_y = 0;
	int16 destination_x = 0;
	int16 destination_y = 0;
	int32 npc_id = 0;
	e_autoplay_transition_kind kind = AUTOPLAY_TRANSITION_NONE;
};

struct s_autoplay_session {
	s_autoplay_config config{};
	s_autoplay_config pending_config{};
	e_autoplay_state state = AUTOPLAY_LOADING;
	e_autoplay_stop_reason stop_reason = AUTOPLAY_STOP_NONE;
	int32 target_id = 0;
	int32 avoid_id = 0;
	t_tick next_decision_tick = 0;
	t_tick next_action_tick = 0;
	t_tick last_teleport_tick = 0;
	t_tick no_target_since = 0;
	uint8 consecutive_misses = 0;
	uint32 kills = 0;
	uint32 loot_count = 0;
	uint32 path_failures = 0;
	uint32 follow_char_id = 0;
	uint16 previous_autoloot = 0;
	uint16 previous_autoloottype = 0;
	uint32 previous_autolootids[AUTOPLAY_MAX_LOOT_ITEMS]{};
	bool previous_autolooting = false;
	int16 origin_map = -1;
	int16 origin_x = 0;
	int16 origin_y = 0;
	s_autoplay_transition last_transition{};
	s_autoplay_transition return_transition{};
	s_autoplay_transition return_step{};
	int16 return_route_map = -1;
	s_autoplay_transition follow_step{};
	int16 follow_route_map = -1;
	s_autoplay_transition pending_follow_transition{};
	t_tick pending_follow_expires = 0;
	s_autoplay_transition storage_step{};
	int16 storage_route_map = -1;
	bool loaded = false;
	bool active = false;
	bool save_pending = false;
	bool autoloot_overridden = false;
	bool waiting_return = false;
	bool sitting_recovery = false;
	bool storage_trip = false;
	bool storage_returning = false;
	bool storage_blocked = false;
	bool storage_moved = false;
	bool storage_by_weight = false;
	bool follow_offline_handled = false;
};

void do_init_autoplay();
void do_final_autoplay();
void autoplay_attach(map_session_data& sd);
void autoplay_detach(map_session_data& sd);
void autoplay_set_follow(map_session_data& sd, uint32 char_id);
void autoplay_sync_party_follow(map_session_data& sd, uint32 preferred_char_id = 0);
void autoplay_notify_map_change(map_session_data& sd, uint16 destination_mapindex, int16 destination_x, int16 destination_y);
bool autoplay_control(map_session_data& sd, e_autoplay_control control);
void autoplay_config_loaded(map_session_data& sd, const s_autoplay_config& config);
void autoplay_config_saved(map_session_data& sd, bool success, uint32 revision);
void autoplay_record_kill(map_session_data& sd);
void autoplay_record_loot(map_session_data& sd, uint32 amount);
void autoplay_record_attack_result(map_session_data& sd, bool missed);

#endif
