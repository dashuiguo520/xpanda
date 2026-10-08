#ifndef CUSTOM_AUTOPLAY_PACKETS_HPP
#define CUSTOM_AUTOPLAY_PACKETS_HPP

#include <common/autoplay.hpp>

// Project-local client protocol: keep this range separate from upstream packets.
constexpr uint32 AUTOPLAY_PROTOCOL_MAGIC = 0x594c5041; // APLY on the wire
constexpr uint16 AUTOPLAY_PROTOCOL_VERSION = 1;

// ===== 挂机系统：开始 =====
#if !defined(sun) && (!defined(__NETBSD__) || __NetBSD_Version__ >= 600000000)
#pragma pack(push, 1)
#endif
struct PACKET_CZ_AUTOPLAY_CONFIG_GET { uint16 packetType; uint32 magic; uint16 protocolVersion; } __attribute__((packed));
DEFINE_PACKET_HEADER(CZ_AUTOPLAY_CONFIG_GET, 0x7e00);
struct PACKET_CZ_AUTOPLAY_CONFIG_SAVE { uint16 packetType; s_autoplay_config config; } __attribute__((packed));
DEFINE_PACKET_HEADER(CZ_AUTOPLAY_CONFIG_SAVE, 0x7e01);
struct PACKET_CZ_AUTOPLAY_CONTROL { uint16 packetType; uint8 control; } __attribute__((packed));
DEFINE_PACKET_HEADER(CZ_AUTOPLAY_CONTROL, 0x7e02);
struct PACKET_CZ_AUTOPLAY_STATUS_GET { uint16 packetType; } __attribute__((packed));
DEFINE_PACKET_HEADER(CZ_AUTOPLAY_STATUS_GET, 0x7e03);
struct PACKET_ZC_AUTOPLAY_CONFIG { uint16 packetType; uint8 result; s_autoplay_config config; } __attribute__((packed));
DEFINE_PACKET_HEADER(ZC_AUTOPLAY_CONFIG, 0x7e04);
struct PACKET_ZC_AUTOPLAY_RESULT { uint16 packetType; uint8 operation; uint8 result; uint32 revision; } __attribute__((packed));
DEFINE_PACKET_HEADER(ZC_AUTOPLAY_RESULT, 0x7e05);
struct PACKET_ZC_AUTOPLAY_STATUS { uint16 packetType; uint8 state; uint8 stop_reason; uint32 target_id; uint32 kills; uint32 loot_count; } __attribute__((packed));
DEFINE_PACKET_HEADER(ZC_AUTOPLAY_STATUS, 0x7e06);
struct PACKET_CZ_AUTOPLAY_WALK_TO { uint16 packetType; uint16 x; uint16 y; } __attribute__((packed));
DEFINE_PACKET_HEADER(CZ_AUTOPLAY_WALK_TO, 0x7e07);
struct PACKET_ZC_AUTOPLAY_WALK_STEP { uint16 packetType; uint16 from_x; uint16 from_y; uint16 to_x; uint16 to_y; uint16 duration; } __attribute__((packed));
DEFINE_PACKET_HEADER(ZC_AUTOPLAY_WALK_STEP, 0x7e08);
#if !defined(sun) && (!defined(__NETBSD__) || __NetBSD_Version__ >= 600000000)
#pragma pack(pop)
#endif
static_assert(sizeof(PACKET_CZ_AUTOPLAY_CONFIG_GET) == 8);
static_assert(sizeof(PACKET_CZ_AUTOPLAY_CONFIG_SAVE) == 494);
static_assert(sizeof(PACKET_CZ_AUTOPLAY_CONTROL) == 3);
static_assert(sizeof(PACKET_CZ_AUTOPLAY_STATUS_GET) == 2);
static_assert(sizeof(PACKET_ZC_AUTOPLAY_CONFIG) == 495);
static_assert(sizeof(PACKET_ZC_AUTOPLAY_RESULT) == 8);
static_assert(sizeof(PACKET_ZC_AUTOPLAY_STATUS) == 16);
static_assert(sizeof(PACKET_CZ_AUTOPLAY_WALK_TO) == 6);
static_assert(sizeof(PACKET_ZC_AUTOPLAY_WALK_STEP) == 12);
// ===== 挂机系统：结束 =====

#endif
