# 陪玩机器人系统计划书

- 版本：v1.0（设计稿，未实施）
- 日期：2026-11-11
- 适用范围：rAthena map-server / char-server / SQL
- 前置变更：已移除 population engine（假玩家系统），保留 autoplay

---

## 1. 目标

玩家在 NPC 处花费 zeny 租用一名陪玩机器人，机器人以**真实角色身份**加入玩家所在队伍，跟随玩家战斗、限时到期后自动离队消失。

与旧假玩家系统的本质区别：

| 项目 | 旧 population engine | 新陪玩系统 |
|---|---|---|
| 身份 | 内存伪造 PC，无 char 记录 | 真实 char 记录，有 char_id/account_id |
| 队伍 | 伪造 party_id，无真实队伍 | 真实入队，走 char-server |
| 存续 | 常驻刷人，重启重建 | 按契约租用，到期回收 |
| 数量 | 全图批量生成 | 每玩家 1 个，队伍/地图有上限 |

---

## 2. 需求与验收

| 编号 | 需求 | 验收标准 |
|---|---|---|
| R1 | 必须已有队伍，且队伍未满 | 无队伍、队伍 12 人时拒绝租赁，给出明确提示 |
| R2 | 一人只能租一个 | 同一 char_id 存在未过期契约时拒绝 |
| R3 | 有时间限制，过期自动消失 | 到期后 60 秒内离队、从地图消失、角色池释放 |
| R4 | 满足条件自动入队 | 无需邀请弹窗，租赁成功后队伍面板立即出现机器人 |
| R5 | 队长不能解除其他人的机器人 | 队长踢非自己租用的机器人被拒绝；本人可主动退租 |
| R6 | 队友离队，机器人同时离队 | 租用者退队/被踢/下线/队伍解散时，机器人同步离队并回收 |

补充默认规则（待确认，见第 14 节）：

- 每个队伍最多 2 个机器人，防止 1 真人 + 11 机器人。
- 机器人等级不高于租用者等级 + 10。
- PVP、攻城、 Battleground、活动地图禁止租用与召唤。
- 机器人不参与经验与掉落分配，也不拾取物品。

---

## 3. 决定架构的四个事实

这四点决定了不能沿用旧系统的做法：

1. **队伍成员不存在 party 表里。** `party` 表只有 `party_id/name/exp/item/leader_id/leader_char`（`sql-files/main.sql:974-982`），成员关系存在 `char.party_id`（`main.sql:244`）。char-server 读队伍时直接 `SELECT ... FROM char WHERE party_id = ?`（`src/char/int_party.cpp:233`）。
2. **char-server 有内存缓存。** `inter_party_fromsql` 先查 `party_db` 缓存（`int_party.cpp:206-211`），直接改 SQL 不会生效，必须走 `intif_party_addmember` / `intif_party_leave`。
3. **入队回调要求在线会话。** `party_member_added` 要求 `map_id2sd(account_id)` 存在、`char_id` 匹配且 `party_joining == true`（`src/map/party.cpp:690-696`）。所以机器人必须先有 `map_session_data`，再发起入队。
4. **队长退队会解散整队。** char-server 在队长离开时清空所有成员（`int_party.cpp:655-666`）。机器人回收必须挂在这个流程之前或之中。

**结论：机器人必须有真实 `char` 记录；入队、离队必须经过 char-server，不能只改内存或 SQL。**

---

## 4. 总体架构

```
NPC 租赁
   │  校验队伍/人数/契约/zeny
   ▼
契约服务 ──────► 机器人角色池（char + companion_bot）
   │  占用一个空闲机器人               │
   ▼                                  ▼
生成 map_session_data（真实身份）   写契约 expire_at
   │
   ▼
队伍适配层（intif_party_addmember / intif_party_leave / 踢人拦截）
   │
   ▼
战斗 AI（跟随 / 协战 / 支援 / 死亡重生）
   │
   ▼
计时器（60s tick）──► 到期/欠费/下线 ──► 离队 + 回收 + 释放角色池
```

模块划分：

| 模块 | 职责 | 新增文件 |
|---|---|---|
| 契约与角色池 | 占用/释放机器人、计时、计费 | `src/map/companion.cpp` |
| 队伍适配 | 入队、离队、踢人拦截、退队联动 | `src/map/companion.cpp` + `party.cpp` 钩子 |
| 战斗 AI | 跟随、选怪、技能、支援、重生 | `src/map/companion_ai.cpp` |
| 配置 | 套餐、价格、上限、地图限制 | `db/companion_packages.yml`、`conf/battle/companion.conf` |
| 入口 | NPC 脚本命令、GM 命令 | `src/custom/script.inc`、`src/custom/atcommand.inc` |

---

## 5. 数据设计

### 5.1 角色池表

```sql
CREATE TABLE IF NOT EXISTS `companion_bot` (
  `bot_id`      INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `account_id`  INT UNSIGNED NOT NULL,
  `char_id`     INT UNSIGNED NOT NULL,
  `package_id`  SMALLINT UNSIGNED NOT NULL,
  `name`        VARCHAR(24) NOT NULL,
  `state`       TINYINT UNSIGNED NOT NULL DEFAULT 0,  -- 0 空闲 1 生成中 2 已租用 3 回收中
  `rented_by`   INT UNSIGNED NULL DEFAULT NULL,       -- 租用者 char_id
  `expire_at`   DATETIME NULL DEFAULT NULL,
  PRIMARY KEY (`bot_id`),
  UNIQUE KEY `char_id` (`char_id`),
  UNIQUE KEY `account_id` (`account_id`),
  UNIQUE KEY `rented_by` (`rented_by`),
  KEY `package_state` (`package_id`, `state`)
) ENGINE=InnoDB;
```

`rented_by` 用 `NULL` 表示空闲，MySQL 唯一索引允许多个 NULL，可天然实现"一人一个"。

### 5.2 机器人 char 记录

每个机器人必须在 `char` 表有一行，字段要求：

| 字段 | 值 |
|---|---|
| `char_id` | 保留段 `200000000 + bot_id` |
| `account_id` | 与 char_id 同段，一对一 |
| `name` | 唯一，建议 ASCII 前缀，如 `Companion_Knight_01` |
| `class` | 套餐固定职业 |
| `base_level` / `job_level` | 套餐固定等级（v1 不做动态等级） |
| `last_map` / `save_map` | 任意城镇，仅用于占位 |
| `party_id` | 0（租赁时由 char-server 改写） |
| `online` | 0（租赁期间置 1，见 6.2） |
| `sex` | `M`/`F`，按套餐 |

`char_id` 从 200000000 起，避开真实角色自增区间（`main.sql:296` 当前 `AUTO_INCREMENT=150000`）。账号段与登录服分配的 `[2000000, 100000000]` 不重叠，登录服永远不会发出这些 ID。

### 5.3 套餐配置

`db/companion_packages.yml`：

```yaml
Header:
  Type: COMPANION_PACKAGE_DB
  Version: 1
Body:
  - Id: 1
    Name: "见习剑士"
    Job: SWORDMAN
    BaseLevel: 30
    JobLevel: 20
    Behavior: combat          # combat / support
    Skills:
      - { SkillId: SM_BASH, Level: 5, Cooldown: 3000 }
    GearSet: novice_melee
    Price: 10000              # 每 10 分钟
    MinOwnerLevel: 1
    MaxOwnerLevel: 50
  - Id: 2
    Name: "随行牧师"
    Job: PRIEST
    BaseLevel: 80
    JobLevel: 50
    Behavior: support
    Skills:
      - { SkillId: AL_HEAL, Level: 10, Cooldown: 2000, Target: owner, Condition: owner_hp_below, CondValue: 70 }
      - { SkillId: AL_BLESSING, Level: 10, Cooldown: 10000, Target: owner, Condition: not_owner_status, CondValue: SC_BLESSING }
    GearSet: priest_support
    Price: 20000
    MinOwnerLevel: 60
    MaxOwnerLevel: 99
```

### 5.4 角色池初始化

提供 `sql-files/companion/companion_seed.sql` 或 `tools/companion_seed.py` 生成：

- 每个套餐预置 N 个机器人（建议 10–20，按并发预估）。
- 同时插入 `char` 行与 `companion_bot` 行，二者 `char_id` 一致。
- 幂等：已存在的 `char_id` 跳过。

不采用"租一次建一条 char"的方案，原因：char 表膨胀、char_id 回收复杂、排行榜/邮件/日志污染不可控。

---

## 6. 核心流程

### 6.1 租赁校验（NPC）

按顺序执行，任一失败即返回提示：

1. `sd->status.party_id != 0`，且 `party_search()` 能取到队伍。
2. 队伍总人数 `< MAX_PARTY`（12）。**必须统计 `party.member[i].char_id != 0` 的数量，不能用 `party.count`**（后者是在线人数，`mmo.hpp:726`）。
3. 队伍内机器人数量 `< max_bots_per_party`。
4. `companion_bot` 中该玩家没有未过期契约（`rented_by = sd->status.char_id`）。
5. 玩家等级满足套餐 `MinOwnerLevel`，且 `bot.base_level <= owner.base_level + level_gap_max`。
6. 地图未命中禁用列表（PVP/GVG/BG/活动）。
7. zeny 足够，扣除并记 `LOG_TYPE_NPC`。

### 6.2 自动入队时序

```
NPC/脚本
  └─ companion_hire(package_id)
       ├─ 原子占用：UPDATE companion_bot SET state=1, rented_by=?, expire_at=?
       │            WHERE bot_id=? AND state=0
       ├─ companion_spawn(bot)            // pc_setnewpc + 覆盖 class/level/name + 技能 + 装备
       ├─ bot_sd->party_joining = true
       ├─ bot_sd->party_invite_account = 0   // 抑制"已接受邀请"回执
       ├─ intif_party_addmember(party_id, bot_aid, bot_cid, name)
       │     └─ char-server: UPDATE char SET party_id=? WHERE char_id=?
       │        + 更新 party_db 缓存
       │        + 回包 party_member_added
       ├─ party_member_added: sd->status.party_id = party_id
       ├─ companion_on_joined(bot_sd)
       │     ├─ party_member_joined(*bot_sd)   // 绑定 p->data[i].sd
       │     ├─ p->party.member[i].online = 1  // 面板显示在线
       │     ├─ clif_party_info(*p, nullptr)   // 广播队伍列表
       │     └─ clif_party_hp(*bot_sd)         // 点亮 HP 条
       └─ bot_sd->state = 2，启动契约计时
```

关键点：

- `party_member_added` 只设置 `sd->status.party_id`，**不会绑定 `p->data[i].sd`**，必须补调 `party_member_joined`（`party.cpp:671`），否则机器人收不到队友技能、HP 条也不更新。
- `party_joining` 是入队回调的硬条件，必须在 `intif_party_addmember` 之前置位，否则 char-server 回包会被丢弃。
- `party_invite_account` 必须置 0，否则会给租用者弹一条"XXX 接受了组队邀请"。

失败处理：3 秒内未收到 `party_member_added`，或回包 `flag=1`，执行回滚：释放 shell、退还 zeny、`state=0`、`rented_by=NULL`。

### 6.3 到期与回收

- 契约计时：60 秒 tick 扫描 `expire_at <= NOW()`。
- 回收顺序：
  1. `state=3`（防重入）。
  2. `intif_party_leave(party_id, bot_aid, bot_cid, name, PARTY_MEMBER_WITHDRAW_LEAVE)`。
  3. 等 char-server 回包 `party_member_withdraw` 清空 `p->data[i].sd`；最多等 2 秒。
  4. `chrif_char_offline_nsd(bot_aid, bot_cid)` 置 `char.online=0`。
  5. 释放 shell（`unit_free` + 析构），`charid_db` 移除。
  6. `UPDATE companion_bot SET state=0, rented_by=NULL, expire_at=NULL`。
- 服务器重启：启动时把所有 `state != 0` 的机器人重置为 0，不补租、不退费，并记录日志。

### 6.4 主动退租

NPC 或 `@companion dismiss`：

1. 校验请求者是契约所有者。
2. 执行 6.3 的回收流程。
3. 剩余时间不退费（可配置按分钟折算）。

### 6.5 队员离队联动（R6）

统一入口 `companion_on_member_left(party_id, account_id, char_id)`，挂在以下位置：

| 场景 | 钩子位置 | 处理 |
|---|---|---|
| 玩家主动退队 | `party_leave`（`party.cpp:834`） | 若退队者是租用者，先回收其机器人 |
| 被队长踢出 | `party_removemember`（`party.cpp:764`） | 若目标是租用者，先回收其机器人 |
| 掉线/登出 | `party_member_withdraw`（`party.cpp:885`） | 若目标是租用者，回收机器人 |
| 队伍解散 | `party_broken`（`party.cpp:937`） | 回收该队伍全部机器人 |

防递归：`state=3` 时直接返回；回收流程内再次触发 `party_member_withdraw` 不会二次回收。

### 6.6 踢人限制（R5）

在 `party_removemember` 找到目标成员之后、调用 `intif_party_leave` 之前：

```cpp
if (companion_is_bot(target_char_id) &&
    companion_get_owner(target_char_id) != sd.status.char_id) {
    clif_displaymessage(sd.fd, "不能驱逐其他玩家租用的陪玩机器人。");
    return false;
}
```

覆盖范围：

- 队长通过队伍面板踢人。
- 脚本 `party_delmember`（`script.cpp:28930` 注册）走同一函数，自动生效。
- 队长踢自己租的机器人：允许，等同主动退租。

### 6.7 队长退队/解散

- 队长退队时 char-server 会解散整队（`int_party.cpp:655-666`），因此 `party_leave` 中必须先回收**全队**机器人，再发起退队。
- `party_broken` 回调中做兜底：遍历队伍内机器人，全部回收。

### 6.8 下线与换图

- 租用者下线：`pc_logout` 前回收机器人；不保留剩余时间。
- 租用者换图：AI tick 检测 `bot_sd->m != owner->m`，用 `pc_setpos` 传到主人所在 mapindex；距离 > 12 格同样传送。
- 进入副本：同上。副本地图的 mapindex 在实例存活期间有效，机器人可直接传入。
- 实例空闲销毁可能把机器人算作占用，租用者离开副本时显式回收。

### 6.9 机器人死亡

- 机器人使用正常 PC 死亡流程，不设无敌。
- 死亡后 10 秒在租用者身边复活，恢复 50% HP，契约时间继续走。
- 连续死亡超过阈值（如 5 次/分钟）延长复活时间，防止被当作无限肉盾。

---

## 7. 战斗 AI

独立 `companion_ai.cpp`，每 250ms 一次 tick，只在租用者附近有真人时运行。

状态机：

```
跟随 → 协战 → 支援 → 回归
```

目标优先级：

1. 租用者当前攻击目标（`owner->ud.target`，且是敌对怪物）。
2. 正在攻击租用者的怪物（扫描 10 格，`md->target_id == owner->id`）。
3. 附近 7 格内的怪物（仅 `aggressive: true` 时）。

约束：

- 命中 `mob_ksprotected` 的怪不打（尊重抢怪保护）。
- 不主动攻击 BOSS（可配置），避免拉走归属。
- 不拾取地面物品，不与非租用者交易。
- 支援模式：租用者 HP 低于阈值时优先治疗/上 buff，不追怪。
- 技能释放按套餐配置的 `SkillId/Level/Cooldown/Condition`，无技能时平砍。

---

## 8. 收益与队伍规则改造

机器人是真实队员，会进入 `party_exp_share` 和 `party_share_loot` 的遍历范围，必须显式排除，否则会出现"机器人分走经验、物品进机器人背包后消失"。

| 位置 | 现状 | 改造 |
|---|---|---|
| `party_exp_share`（`party.cpp:1323`） | 遍历 `p->data[i].sd` 均分经验 | 跳过 `companion_is_bot(sd)`，且不计入人数 `c` |
| `party_share_loot`（`party.cpp:1383`） | 轮转/随机把物品给队员 | 跳过机器人，避免物品蒸发 |
| `mob_dead` 伤害汇总 | 机器人伤害计入 `total_damage` | `exp_mode: none` 时排除机器人伤害与 `count` |
| `mob_dead` 击杀者 | `sd` 可能是机器人 | 若是机器人，回退到伤害最高的真人，保证任务/成就/脚本掉落归真人 |
| `get_mvp_player` | 按 `dmg + dmg_tanked` 排序 | 跳过机器人 |
| 掉落归属 `lootdmg` | 前三名锁保护时间 | 跳过机器人，避免真人被锁 3–7 秒 |
| 满员判断 | 脚本侧用 `party.count` | 改用 `char_id != 0` 的总人数 |

推荐 `exp_mode: none`：机器人只提供伤害，不改变经验与掉落总量。若希望"带机器人练级更快"，可切到 `share`，但需要重新评估经济产出。

---

## 9. 配置与入口

### 9.1 `conf/battle/companion.conf`

| 键 | 默认 | 说明 |
|---|---|---|
| `enabled` | yes | 总开关 |
| `max_bots_per_party` | 2 | 单队伍机器人上限 |
| `max_bots_per_map` | 10 | 单地图上限，保护性能 |
| `tick_ms` | 250 | AI tick |
| `billing_interval_sec` | 60 | 计费/到期扫描间隔 |
| `level_gap_max` | 10 | 机器人等级不得超过租用者 +N |
| `follow_distance` | 4 | 跟随时保持的距离 |
| `teleport_distance` | 12 | 超过即传送 |
| `assist_range` | 10 | 协战扫描范围 |
| `aggressive` | no | 是否主动开怪 |
| `exp_mode` | none | none / share |
| `allow_instance` | yes | 是否允许进副本 |
| `owner_afk_timeout_sec` | 300 | 租用者挂机多久后机器人停手 |
| `refund_on_dismiss` | no | 提前退租是否按分钟退费 |

### 9.2 脚本命令

| 命令 | 说明 |
|---|---|
| `companion_hire <package_id>` | 租赁 |
| `companion_info` | 查询剩余时间/套餐 |
| `companion_dismiss` | 主动退租 |
| `companion_renew <package_id>` | 续租 |
| `companion_active` | 返回是否持有机器人（供副本脚本判断） |

### 9.3 GM 命令

| 命令 | 说明 |
|---|---|
| `@companion status` | 查看在线机器人与契约 |
| `@companion reload` | 重载套餐配置 |
| `@companion seed <package_id> <n>` | 补充角色池 |
| `@companion kick <char_id>` | 强制回收 |

---

## 10. 边界与风控

| 项 | 策略 |
|---|---|
| PVP / 攻城 / BG | 禁止租赁；已租用的进入即回收 |
| 副本 | 允许，跟随进入；实例销毁前回收 |
| 交易/邮件/摆摊 | 机器人无客户端，全部拒绝；必要时在 `trade.cpp` 加显式拦截 |
| 组队邀请 | `party_invite` 的 `session_isActive` 已天然拒绝 fd=0 |
| 挂机 | 租用者 5 分钟无操作，机器人停止攻击，只跟随 |
| 刷屏 | 机器人不发言，或仅复用预设台词 |
| 经济 | 租金为 zeny 回收点；`exp_mode: none` 防止机器人变成产出放大器 |
| 性能 | 单地图上限 + 250ms tick + 附近无真人不运行 |
| 排行榜 | 机器人 char 记录需在自定义排行 SQL 中按 account_id 段排除 |

---

## 11. 旧系统移除（已执行）

已通过 `git revert 2dba9858e` 整体回退，并清理本地覆盖配置：

| 项目 | 状态 |
|---|---|
| `src/map/population_engine.cpp` / `.hpp` / `population_engine/`（39 个文件） | 已删除 |
| `db/population_*.yml`（9 个） | 已删除 |
| `conf/battle/population_engine.conf` | 已删除 |
| `npc/custom/population/arena.txt` 与 `scripts_custom.conf` 引用 | 已删除 |
| `sql-files/population_engine/cp_population_stats.sql` | 已删除 |
| 25 个核心文件中的挂钩（clif/battle/pc/map/npc/status/vending/skill/unit/chrif/intif） | 已还原 |
| `@populate` / `@reloadpopenginedb` 注册 | 已移除 |
| `conf/import/battle_conf.txt` 中的 import | 已清理 |
| `conf/import/atcommands.yml` 中的两条帮助 | 已清理 |
| autoplay 挂机系统 | 完整保留 |
| 数据库中的 `cp_population_stats` 表 | **待手工 DROP** |

待执行 SQL：

```sql
DROP TABLE IF EXISTS `cp_population_stats`;
```

---

## 12. 分期计划

| 阶段 | 内容 | 交付物 | 验收 |
|---|---|---|---|
| P1 | 角色池与身份层 | `companion_bot` 表、seed 脚本、`companion_spawn` | 能生成一个真实 char 身份的机器人并在地图站立 |
| P2 | 入队与退队 | `intif_party_addmember` 流程、`party_member_joined` 绑定、踢人拦截 | R4/R5/R6 全部通过 |
| P3 | 契约与回收 | 计时器、NPC 租赁/续租/退租、下线回收 | R1/R2/R3 全部通过 |
| P4 | 战斗 AI | 跟随、协战、支援、死亡重生 | 能跟图、能打怪、牧师能加血 |
| P5 | 收益隔离与边界 | `party_exp_share` / `party_share_loot` / `mob_dead` 改造、地图限制、日志 | 真人经验掉落不缩水，PVP 不可用 |

P1–P3 完成即可上线最小可用版本；P4 决定体验，P5 决定经济安全。

---

## 13. 风险清单

| 风险 | 影响 | 对策 |
|---|---|---|
| char-server 缓存与 SQL 不一致 | 重登后机器人从队伍消失 | 所有队伍变更走 intif，禁止直接改 SQL |
| 队长退队解散队伍 | 机器人残留、契约泄漏 | `party_leave` 先回收全队机器人，`party_broken` 兜底 |
| 回收递归 | 栈溢出/重复释放 | `state=3` 防重入 + 释放后立即置空闲 |
| `party_joining` 未置位 | char-server 回包被丢弃，入队失败 | 入队前强制置位，超时回滚 |
| `p->data[i].sd` 未绑定 | 无 HP 条、队友技能无法作用 | 入队后补调 `party_member_joined` |
| `char.online` 未同步 | 面板显示机器人离线 | 租用时 `chrif_char_online`，回收时 `chrif_char_offline_nsd` |
| char 表污染 | 排行榜/邮件/@who 出现机器人 | 保留 account_id 段，自定义查询排除；名字加固定前缀 |
| 经验/掉落被分走 | 玩家收益缩水 | 第 8 节的四处显式排除 |
| 重启残留 | 机器人卡在队伍里 | 启动时重置所有 `state != 0` 的记录 |
| 机器人被当肉盾 | 玩家无敌刷怪 | 正常死亡、复活延迟递增、不设无敌 |

---

## 14. 待确认

1. 每个队伍最多几个机器人？默认 2 是否合适。
2. 租金价格与时长（按 10 分钟计价，还是按次）。
3. 提前退租是否退费；下线是否保留剩余时间。
4. 机器人是否允许进入副本；是否允许满足副本人数门槛。
5. 机器人伤害是否计入经验总量（`exp_mode` 选 `none` 还是 `share`）。
6. 机器人名字用中文还是 ASCII（客户端编码风险）。

---

## 15. 附：关键函数索引

| 函数 | 位置 |
|---|---|
| `party_create` | `src/map/party.cpp:151` |
| `party_invite` | `src/map/party.cpp:397` |
| `party_member_added` | `src/map/party.cpp:694` |
| `party_member_joined` | `src/map/party.cpp:671` |
| `party_removemember` | `src/map/party.cpp:764` |
| `party_member_withdraw` | `src/map/party.cpp:885` |
| `party_broken` | `src/map/party.cpp:937` |
| `party_exp_share` | `src/map/party.cpp:1323` |
| `party_share_loot` | `src/map/party.cpp:1383` |
| `intif_party_addmember` | `src/map/intif.cpp` |
| `intif_party_leave` | `src/map/intif.cpp` |
| `inter_party_fromsql` | `src/char/int_party.cpp:192` |
| `map_session_data::party_joining` | `src/map/pc.hpp` |
| `chrif_char_online` | `src/map/chrif.cpp:1511` |
| `chrif_char_offline_nsd` | `src/map/chrif.cpp:1469` |
| `MAX_PARTY` | `src/common/mmo.hpp:99` |
