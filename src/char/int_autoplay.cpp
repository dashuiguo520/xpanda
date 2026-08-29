#include "int_autoplay.hpp"

#include <cstdlib>
#include <cstring>

#include <common/autoplay.hpp>
#include <common/socket.hpp>
#include <common/sql.hpp>

#include "inter.hpp"

// Load and save autoplay configuration in character-scoped tables.
namespace {
template <typename T> void read_column(int32 column, T& value) {
	char* data = nullptr; size_t length = 0;
	Sql_GetData(sql_handle, column, &data, &length);
	value = static_cast<T>(strtoul(data, nullptr, 10));
}

bool load_config(uint32 char_id, s_autoplay_config& config) {
	config = s_autoplay_config{};
	config.version = AUTOPLAY_CONFIG_VERSION;
	config.search_range = 12;
	config.normal_attack = 1;
	config.hp_recover_below = 50;
	config.sp_recover_below = 30;
	config.emergency_hp_below = 15;
	config.teleport_min_interval = 10000;
	config.follow_distance = 3;
	config.follow_attack = 1;
	config.boss_action = AUTOPLAY_BOSS_RETALIATE;
	config.storage_weight_at = 70;
	if (SQL_ERROR == Sql_Query(sql_handle, "SELECT `revision`,`search_range`,`normal_attack`,`autoloot_rate`,`hp_recover_below`,`sp_recover_below`,`emergency_hp_below`,`teleport_skill_id`,`teleport_item_id`,`teleport_min_interval`,`death_action`,`follow_char_id`,`follow_distance`,`follow_attack`,`auto_accept_party`,`inventory_stop_at`,`autoloot_types`,`sit_recovery`,`sit_hp_below`,`sit_sp_below`,`no_monster_teleport_seconds`,`surrounded_teleport_count`,`mode`,`boss_action`,`miss_teleport`,`storage_weight_at` FROM `autoplay_config` WHERE `char_id`='%u'", char_id)) return false;
	if (SQL_SUCCESS == Sql_NextRow(sql_handle)) {
		read_column(0, config.revision); read_column(1, config.search_range); read_column(2, config.normal_attack); read_column(3, config.autoloot_rate); read_column(4, config.hp_recover_below); read_column(5, config.sp_recover_below); read_column(6, config.emergency_hp_below); read_column(7, config.teleport_skill_id); read_column(8, config.teleport_item_id); read_column(9, config.teleport_min_interval); read_column(10, config.death_action); read_column(11, config.follow_char_id); read_column(12, config.follow_distance); read_column(13, config.follow_attack); read_column(14, config.auto_accept_party); read_column(15, config.inventory_stop_at); read_column(16, config.autoloot_types); read_column(17, config.sit_recovery); read_column(18, config.sit_hp_below); read_column(19, config.sit_sp_below); read_column(20, config.no_monster_teleport_seconds); read_column(21, config.surrounded_teleport_count); read_column(22, config.mode); read_column(23, config.boss_action); read_column(24, config.miss_teleport); read_column(25, config.storage_weight_at);
	}
	Sql_FreeResult(sql_handle);
	if (SQL_ERROR == Sql_Query(sql_handle, "SELECT `skill_id`,`skill_lv`,`enabled`,`target_type`,`hp_min`,`hp_max` FROM `autoplay_skill` WHERE `char_id`='%u' ORDER BY `slot` LIMIT %u", char_id, AUTOPLAY_MAX_SKILLS)) return false;
	while (config.skill_count < AUTOPLAY_MAX_SKILLS && SQL_SUCCESS == Sql_NextRow(sql_handle)) { auto& r = config.skills[config.skill_count++]; read_column(0, r.skill_id); read_column(1, r.skill_lv); read_column(2, r.enabled); read_column(3, r.target_type); read_column(4, r.hp_min); read_column(5, r.hp_max); }
	Sql_FreeResult(sql_handle);
	if (SQL_ERROR == Sql_Query(sql_handle, "SELECT `item_id`,`enabled`,`resource_type`,`trigger_below` FROM `autoplay_recovery_item` WHERE `char_id`='%u' ORDER BY `slot` LIMIT 5", char_id)) return false;
	while (config.recovery_count < 5 && SQL_SUCCESS == Sql_NextRow(sql_handle)) { auto& r = config.recovery_items[config.recovery_count++]; read_column(0, r.item_id); read_column(1, r.enabled); read_column(2, r.resource_type); read_column(3, r.trigger_below); }
	Sql_FreeResult(sql_handle);
	if (SQL_ERROR == Sql_Query(sql_handle, "SELECT `mob_id`,`action` FROM `autoplay_monster_rule` WHERE `char_id`='%u' ORDER BY `slot` LIMIT 10", char_id)) return false;
	while (config.monster_count < AUTOPLAY_MAX_MONSTER_RULES && SQL_SUCCESS == Sql_NextRow(sql_handle)) { auto& r = config.monster_rules[config.monster_count++]; read_column(0, r.mob_id); read_column(1, r.action); }
	Sql_FreeResult(sql_handle);
	if (SQL_ERROR == Sql_Query(sql_handle, "SELECT `item_id`,`keep_amount` FROM `autoplay_discard_item` WHERE `char_id`='%u' ORDER BY `slot` LIMIT 10", char_id)) return false;
	while (config.discard_count < AUTOPLAY_MAX_DISCARD_ITEMS && SQL_SUCCESS == Sql_NextRow(sql_handle)) { auto& r = config.discard_items[config.discard_count++]; read_column(0, r.item_id); read_column(1, r.keep_amount); }
	Sql_FreeResult(sql_handle);
	if (SQL_ERROR == Sql_Query(sql_handle, "SELECT `item_id` FROM `autoplay_loot_item` WHERE `char_id`='%u' ORDER BY `slot` LIMIT 10", char_id)) return false;
	while (config.autoloot_item_count < AUTOPLAY_MAX_LOOT_ITEMS && SQL_SUCCESS == Sql_NextRow(sql_handle)) read_column(0, config.autoloot_items[config.autoloot_item_count++]);
	Sql_FreeResult(sql_handle);
	return true;
}

bool save_config(uint32 char_id, s_autoplay_config& config) {
	if (config.version != AUTOPLAY_CONFIG_VERSION || config.search_range < 1 || config.search_range > 30 || config.death_action > AUTOPLAY_DEATH_TOKEN_6316 || config.mode > AUTOPLAY_MODE_FIXED || config.boss_action < AUTOPLAY_BOSS_AVOID || config.boss_action > AUTOPLAY_BOSS_PRIORITY || config.miss_teleport > 1 || (config.storage_weight_at != 0 && (config.storage_weight_at < 50 || config.storage_weight_at > 100)) || config.skill_count > AUTOPLAY_MAX_SKILLS || config.recovery_count > AUTOPLAY_MAX_RECOVERY_ITEMS || config.monster_count > AUTOPLAY_MAX_MONSTER_RULES || config.follow_distance < 1 || config.follow_distance > 12 || config.follow_attack > 1 || config.auto_accept_party > 1 || config.sit_recovery > 1 || config.sit_hp_below < 1 || config.sit_hp_below > 100 || config.sit_sp_below < 1 || config.sit_sp_below > 100 || config.surrounded_teleport_count > 20 || (config.inventory_stop_at != 0 && (config.inventory_stop_at < 50 || config.inventory_stop_at > 100)) || (config.autoloot_types & ~AUTOPLAY_AUTOLOOT_TYPE_MASK) != 0 || config.discard_count > AUTOPLAY_MAX_DISCARD_ITEMS || config.autoloot_item_count > AUTOPLAY_MAX_LOOT_ITEMS) return false;
	uint8 skill_counts[AUTOPLAY_SKILL_PARTY_HEAL + 1]{};
	for (uint8 i = 0; i < config.skill_count; ++i) {
		const auto& skill = config.skills[i];
		if (skill.target_type > AUTOPLAY_SKILL_PARTY_HEAL || ++skill_counts[skill.target_type] > AUTOPLAY_MAX_SKILLS_PER_TARGET || skill.hp_min > skill.hp_max || skill.hp_max > 100) return false;
	}
	for (uint8 i = 0; i < config.monster_count; ++i) if (config.monster_rules[i].mob_id == 0 || config.monster_rules[i].action < AUTOPLAY_MONSTER_PRIORITY || config.monster_rules[i].action > AUTOPLAY_MONSTER_AVOID) return false;
	for (uint8 i = 0; i < config.discard_count; ++i) if (config.discard_items[i].item_id == 0) return false;
	for (uint8 i = 0; i < config.autoloot_item_count; ++i) if (config.autoloot_items[i] == 0) return false;
	if (SQL_ERROR == Sql_Query(sql_handle, "START TRANSACTION")) return false;
	bool ok = SQL_SUCCESS == Sql_Query(sql_handle, "INSERT INTO `autoplay_config` (`char_id`,`revision`,`search_range`,`normal_attack`,`autoloot_rate`,`hp_recover_below`,`sp_recover_below`,`emergency_hp_below`,`teleport_skill_id`,`teleport_item_id`,`teleport_min_interval`,`death_action`,`follow_char_id`,`follow_distance`,`follow_attack`,`auto_accept_party`,`inventory_stop_at`,`autoloot_types`,`sit_recovery`,`sit_hp_below`,`sit_sp_below`,`no_monster_teleport_seconds`,`surrounded_teleport_count`,`mode`,`boss_action`,`miss_teleport`,`storage_weight_at`) VALUES ('%u',1,'%u','%u','%u','%u','%u','%u','%u','%u','%u','%u','%u','%u','%u','%u','%u','%u','%u','%u','%u','%u','%u','%u','%u','%u','%u') ON DUPLICATE KEY UPDATE `revision`=`revision`+1,`search_range`=VALUES(`search_range`),`normal_attack`=VALUES(`normal_attack`),`autoloot_rate`=VALUES(`autoloot_rate`),`hp_recover_below`=VALUES(`hp_recover_below`),`sp_recover_below`=VALUES(`sp_recover_below`),`emergency_hp_below`=VALUES(`emergency_hp_below`),`teleport_skill_id`=VALUES(`teleport_skill_id`),`teleport_item_id`=VALUES(`teleport_item_id`),`teleport_min_interval`=VALUES(`teleport_min_interval`),`death_action`=VALUES(`death_action`),`follow_char_id`=VALUES(`follow_char_id`),`follow_distance`=VALUES(`follow_distance`),`follow_attack`=VALUES(`follow_attack`),`auto_accept_party`=VALUES(`auto_accept_party`),`inventory_stop_at`=VALUES(`inventory_stop_at`),`autoloot_types`=VALUES(`autoloot_types`),`sit_recovery`=VALUES(`sit_recovery`),`sit_hp_below`=VALUES(`sit_hp_below`),`sit_sp_below`=VALUES(`sit_sp_below`),`no_monster_teleport_seconds`=VALUES(`no_monster_teleport_seconds`),`surrounded_teleport_count`=VALUES(`surrounded_teleport_count`),`mode`=VALUES(`mode`),`boss_action`=VALUES(`boss_action`),`miss_teleport`=VALUES(`miss_teleport`),`storage_weight_at`=VALUES(`storage_weight_at`)", char_id, config.search_range, config.normal_attack, config.autoloot_rate, config.hp_recover_below, config.sp_recover_below, config.emergency_hp_below, config.teleport_skill_id, config.teleport_item_id, config.teleport_min_interval, config.death_action, config.follow_char_id, config.follow_distance, config.follow_attack, config.auto_accept_party, config.inventory_stop_at, config.autoloot_types, config.sit_recovery, config.sit_hp_below, config.sit_sp_below, config.no_monster_teleport_seconds, config.surrounded_teleport_count, config.mode, config.boss_action, config.miss_teleport, config.storage_weight_at);
	ok &= SQL_SUCCESS == Sql_Query(sql_handle, "DELETE FROM `autoplay_skill` WHERE `char_id`='%u'", char_id);
	for (uint8 i = 0; ok && i < config.skill_count; ++i) { const auto& r = config.skills[i]; ok &= SQL_SUCCESS == Sql_Query(sql_handle, "INSERT INTO `autoplay_skill` VALUES ('%u','%u','%u','%u','%u','%u','%u','%u')", char_id, i, r.skill_id, r.skill_lv, r.enabled, r.target_type, r.hp_min, r.hp_max); }
	ok &= SQL_SUCCESS == Sql_Query(sql_handle, "DELETE FROM `autoplay_recovery_item` WHERE `char_id`='%u'", char_id);
	for (uint8 i = 0; ok && i < config.recovery_count; ++i) { const auto& r = config.recovery_items[i]; ok &= SQL_SUCCESS == Sql_Query(sql_handle, "INSERT INTO `autoplay_recovery_item` VALUES ('%u','%u','%u','%u','%u','%u')", char_id, i, r.item_id, r.enabled, r.resource_type, r.trigger_below); }
	ok &= SQL_SUCCESS == Sql_Query(sql_handle, "DELETE FROM `autoplay_monster_rule` WHERE `char_id`='%u'", char_id);
	for (uint8 i = 0; ok && i < config.monster_count; ++i) { const auto& r = config.monster_rules[i]; ok &= SQL_SUCCESS == Sql_Query(sql_handle, "INSERT INTO `autoplay_monster_rule` VALUES ('%u','%u','%u','%u')", char_id, i, r.mob_id, r.action); }
	ok &= SQL_SUCCESS == Sql_Query(sql_handle, "DELETE FROM `autoplay_discard_item` WHERE `char_id`='%u'", char_id);
	for (uint8 i = 0; ok && i < config.discard_count; ++i) { const auto& r = config.discard_items[i]; ok &= SQL_SUCCESS == Sql_Query(sql_handle, "INSERT INTO `autoplay_discard_item` VALUES ('%u','%u','%u','%u')", char_id, i, r.item_id, r.keep_amount); }
	ok &= SQL_SUCCESS == Sql_Query(sql_handle, "DELETE FROM `autoplay_loot_item` WHERE `char_id`='%u'", char_id);
	for (uint8 i = 0; ok && i < config.autoloot_item_count; ++i) ok &= SQL_SUCCESS == Sql_Query(sql_handle, "INSERT INTO `autoplay_loot_item` VALUES ('%u','%u','%u')", char_id, i, config.autoloot_items[i]);
	if (ok) ok = SQL_SUCCESS == Sql_Query(sql_handle, "COMMIT"); else Sql_Query(sql_handle, "ROLLBACK");
	if (ok && SQL_SUCCESS == Sql_Query(sql_handle, "SELECT `revision` FROM `autoplay_config` WHERE `char_id`='%u'", char_id) && SQL_SUCCESS == Sql_NextRow(sql_handle)) read_column(0, config.revision);
	Sql_FreeResult(sql_handle);
	return ok;
}
}

int32 inter_autoplay_parse_frommap(int32 fd) {
	const uint16 command = RFIFOW(fd, 0); const uint32 char_id = RFIFOL(fd, 2);
	if (command == 0x30B0) { s_autoplay_config config{}; const bool ok = load_config(char_id, config); WFIFOHEAD(fd, 7 + sizeof(config)); WFIFOW(fd, 0) = 0x38B0; WFIFOL(fd, 2) = char_id; WFIFOB(fd, 6) = ok; memcpy(WFIFOP(fd, 7), &config, sizeof(config)); WFIFOSET(fd, 7 + sizeof(config)); return 1; }
	if (command == 0x30B1) { s_autoplay_config config{}; memcpy(&config, RFIFOP(fd, 6), sizeof(config)); const bool ok = save_config(char_id, config); WFIFOHEAD(fd, 11); WFIFOW(fd, 0) = 0x38B1; WFIFOL(fd, 2) = char_id; WFIFOB(fd, 6) = ok; WFIFOL(fd, 7) = config.revision; WFIFOSET(fd, 11); return 1; }
	return 0;
}
