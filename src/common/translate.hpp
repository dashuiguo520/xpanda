// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#pragma once

#include <string>

// ===== 终端动态文本编码桥接 开始 =====
// 修改内容/原因：输出规范化接口接收明确的缓冲区长度，需要标准 size_t 类型声明。
#include <cstddef>
// ===== 终端动态文本编码桥接 结束 =====

// ===== 终端信息翻译机制移植 开始 =====
// 修改内容/原因：声明终端翻译模块的生命周期与统一翻译入口，供 Core 和 ShowMessage 使用。
void do_init_translate();
void do_final_translate();
std::string translate_console_message( const char* original );
// ===== 终端信息翻译机制移植 结束 =====

// ===== 终端动态文本编码桥接 开始 =====
// 修改内容/原因：动态角色名等数据可能使用数据库连接代码页；声明格式化前后转换接口，避免与 UTF-8 翻译模板混合后乱码。
void set_console_data_encoding( const char* encoding );
std::string normalize_console_output( const char* text, size_t length );
// ===== 终端动态文本编码桥接 结束 =====
