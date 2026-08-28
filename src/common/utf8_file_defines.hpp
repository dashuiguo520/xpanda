// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#pragma once

// ===== UTF-8 配置与脚本读取支持 开始 =====
// 修改内容/原因：默认接管服务端文本文件的标准读取函数，使传统 .conf 解析器也能透明读取 UTF-8；无需 Pandas 专属宏开关。
#include "translate.hpp"

#define fgets( BUFFER, MAX_COUNT, STREAM ) utf8_fgets( BUFFER, MAX_COUNT, STREAM )
// ===== UTF-8 配置与脚本读取支持 结束 =====
