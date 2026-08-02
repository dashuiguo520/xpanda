// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#ifndef MSG_CONF_HPP
#define	MSG_CONF_HPP

#include <config/core.hpp>

enum lang_types {
	LANG_CHS = 0x01,
	LANG_CHT = 0x02,
	LANG_MAX
};

#ifndef LANG_ENABLE
// 除了英语, 我们额外启用两种语言, 分别是: 简体中文, 繁体中文 [Sola丶小克]
#define LANG_ENABLE (LANG_CHS | LANG_CHT)
#endif

#define ALL_EXTEND_MSG 4000
#define ALL_EXTEND_FIRST_MSG 3000

//read msg in table
const char* _msg_txt(int32 msg_number,int32 size, char ** msg_table);
//store msg from txtfile into msg_table
int32 _msg_config_read(const char* cfgName,int32 size, char ** msg_table);
//clear msg_table
void _do_final_msg(int32 size, char ** msg_table);
//Lookups
int32 msg_langstr2langtype(char * langtype);
const char* msg_langtype2langstr(int32 langtype);
// Verify that the choosen langtype is enabled.
int32 msg_checklangtype(int32 lang, bool display);

#endif	/* MSG_CONF_HPP */
