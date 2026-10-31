# 求生之路 2（L4D2）反作弊检测机制源码解析

这份报告只针对《求生之路 2》的 `left4dead2` 服务器分支，解析本机两套 SourceMod 源码：

| 工程 | 源码根目录 | 源码版本 | 主要日志 |
| --- | --- | --- | --- |
| Lilac / Little Anti-Cheat | `C:\Users\Administrator\Desktop\脚本优化\sm-plugin-lilac-master\sm-plugin-lilac-master` | `1.7.11` | `addons/sourcemod/logs/lilac.log` |
| SMAC / SourceMod Anti-Cheat | `C:\Users\Administrator\Desktop\脚本优化\smac-master` | `0.8.6.4` | `addons/sourcemod/logs/SMAC.log` |

本文的结论来自源码执行路径和常量，不把日志文案当作实际动作。配置项分为“源码默认值”和“当前仓库配置值”两栏；实际服务器启动后以 ConVar 最终值为准。

## 1. 先给结论

两套插件都是服务器侧检测。它们接收客户端 `usercmd`、击杀事件、命令和客户端 ConVar，经过时间窗与状态门控后记录疑点或正式 detection，再按模块开关执行拦截、Kick 或 Ban。没有把客户端进程完整性当作判断依据，因此配置、日志和输入历史必须一起审计。

在 L4D2 中真正工作的检测面如下：

| 检测面 | Lilac | SMAC | 主要动作 |
| --- | --- | --- | --- |
| 角度/自瞄 | Aimbot、Aimlock；基础角度检测主动跳过 | Aimbot、EyeTest | 累计后 Ban 或只记录 |
| 跳跃/连发 | Bhop、Macro | AutoTrigger | 计数、Kick 或 Ban |
| 命令与输入完整性 | Backtrack 修补、Chat/Name 过滤 | EyeTest、Commands、Client Protection | 丢弃命令、拦截、Kick/Ban |
| 客户端配置 | Lilac ConVar | SMAC ConVar Checker | 非法值通常直接 Ban，查询失败 Kick |
| 网络参数 | Ping、Lerp、NoLerp | 无同类公共计数器 | Kick、临时 Ban 或修补 |
| 信息隔离 | 无实体传输墙透保护 | Wallhack | `SetTransmit` 隐藏实体 |
| L4D2 漏洞修补 | 输入历史和回溯修补 | `smac_l4d2_fixes` | 直接抑制异常输入/伤害 |

“疑点”表示一次特征命中；“正式 detection”表示已经经过模块自己的累计门控；“处置”表示实际调用 Kick/Ban 或返回 `Plugin_Handled`。三者在源码中不是同一个层级。

## 2. 共用执行模型

### 2.1 L4D2 识别和入口

- Lilac 在 `addons/sourcemod/scripting/lilac.sp:84-100` 读取 `GetGameFolderName()`；字符串等于 `left4dead2` 时写入 `GAME_L4D2`。
- SMAC 在 `addons/sourcemod/scripting/smac.sp:76-80` 将 `left4dead2` 映射为 `Game_L4D2`。
- Lilac 版本和全局常量位于 `addons/sourcemod/scripting/lilac/lilac_globals.sp:22,99-106,119-123`；SMAC 版本由 `smac.inc` 的 `SMAC_VERSION` 提供，仓库源码为 `0.8.6.4`。

如果游戏目录识别失败，L4D2 专用门控不会生效。部署后第一项检查应是服务器控制台显示的游戏类型和插件版本。

### 2.2 Lilac 的 330 格 usercmd 环形历史

`lilac_globals.sp:22` 定义 `CMD_LENGTH = 330`。`lilac.sp:301-350` 的 `OnPlayerRunCmd` 每次保存：

- 角度 `playerinfo_angles`；
- 按钮 `playerinfo_buttons`；
- 动作边沿 `playerinfo_actions`；
- usercmd 处理时间；
- tickcount。

同一入口随后调用 Macro、Bhop、角度修补和 Backtrack 逻辑。击杀事件再从环形缓冲区回看射击前历史。玩家连接、生成、传送和插件初始化时会清理大部分数组。

源码审计项：`OnPlayerRunCmd` 中的 `static lbuttons[MAXPLAYERS+1]` 用于按钮边沿，但槽位复用时没有在该函数中显式清零。`lilac_reset_client()` 会重置模块数组，仍建议在实际换槽测试首个命令的 Bhop/Macro 边沿状态。

### 2.3 定时任务

Lilac 在 `lilac.sp:149-155` 创建以下重复定时器：

| 定时器 | 周期 | 作用 |
| --- | ---: | --- |
| `timer_query` | 5 秒 | 每名玩家查询一条客户端 ConVar |
| `timer_check_ping` | 5 秒 | 出站延迟计数 |
| `timer_check_lerp` | 5 秒 | `m_fLerpTime` 与 NoLerp |
| `timer_check_aimlock` | 0.5 秒 | Aimlock 队列扫描 |
| `timer_decrement_macro` | 300 秒 | Macro 两类计数各减 1 |

SMAC 的主要定时器：命令计数每 1 秒清零，AutoTrigger 计数每 4 秒衰减，ConVar 查询首重试 20 秒、后续每 10 秒，Speedhack 每 0.1 秒补充 tick，Spinhack 每 1 秒检查，Client Protection 每 10 秒衰减名称和成就计数。

### 2.4 统一动作层

Lilac 的 `lilac_ban_client()` 位于 `lilac_stock.sp:219-318`；SMAC 通过 `SMAC_CheatDetected` forward 进入 `SMAC_OnCheatDetected`，再由 `SMAC_Ban` 处理。共享 forward 可被其他插件拦截，所以日志中出现 detection 后仍需检查动作开关。

| 状态 | 含义 |
| --- | --- |
| 记录疑点 | 只写日志、数据库或管理员通知，不改变玩家状态 |
| `Plugin_Handled` | 当前 usercmd/命令被丢弃，属于即时输入保护 |
| Kick | 连接被断开，通常用于查询失败、命令滥用或插值异常 |
| Ban | 调用 SourceBans、MaterialAdmin、BaseBans 或 `BanClient` |
| 临时 Ban | 只在指定时间内阻止重新连接，例如 Lilac 高 Ping 3 分钟 |

### 2.5 误报门控的共通原则

源码已经放入多层门控，运营时按下列顺序判读：

1. 玩家是否是 Bot、是否存活、是否已在有效队伍。
2. 是否刚出生、刚传送、刚换队、正在忙碌动画或操控载具。
3. 是否达到距离、历史长度、网络丢包、延迟稳定性等前置条件。
4. 是一次疑点还是累计到正式 detection。
5. 对应 ConVar 是否开启动作。

只看到一条 `suspected` 日志时，先取相邻 usercmd、队伍、武器、延迟和地图事件，再决定是否人工处置。

## 3. Lilac：L4D2 配置基线

下表来自 `addons/sourcemod/scripting/lilac/lilac_config.sp:23-136` 和 `lilac.sp:120-128`。除非服务器配置覆盖，启动时采用源码默认值。

| ConVar | 源码默认 | L4D2 作用 |
| --- | ---: | --- |
| `lilac_enable` | `1` | 总开关 |
| `lilac_ban` | `1` | 统一 Ban 开关；`0` 保留检测和日志 |
| `lilac_ban_length` | `0` | 分钟，`0` 为永久；Bhop/Macro 有独立覆盖 |
| `lilac_log` | `1` | 写 Lilac 日志 |
| `lilac_log_extra` | `1` | Ban 或检测时附加玩家上下文 |
| `lilac_log_misc` | `0` | 查询失败、Ping、插值等杂项日志 |
| `lilac_aimbot` | `5` | 第 5 个正式 Aimbot detection 进入 Ban 判断；`1` 为只记录 |
| `lilac_aimbot_autoshoot` | `1` | 开启 Autoshoot 子特征 |
| `lilac_aimlock` | `10` | 第 10 个正式 Aimlock detection 进入 Ban 判断 |
| `lilac_aimlock_light` | `1` | 每轮最多处理 5 名候选玩家 |
| `lilac_bhop` | `5` | Medium 预设；负值为日志模式，`0` 关闭 |
| `lilac_macro` | `0` | 默认关闭；`-1` 只记录，`1` 开启，`2` 开启但不写普通日志 |
| `lilac_macro_method` | `0` | `0` Kick，`1` 走 Lilac Ban 链 |
| `lilac_macro_mode` | `0` | `0` 两类都查，`1` Auto-Jump，`2` Auto-Shoot |
| `lilac_macro_warning` | `1` | 宏疑点提示对象 |
| `lilac_convar` | `1` | 客户端 ConVar 检查；负值只记录 |
| `lilac_nolerp` | `1` | NoLerp 检查；负值只记录 |
| `lilac_max_lerp` | `105` | 毫秒上限；低于 105 会被源码视为关闭 |
| `lilac_max_ping` | `0` | 小于 100 时模块关闭 |
| `lilac_max_ping_spec` | `0` | 至少 30 才启用高 Ping 观察队列 |
| `lilac_backtrack_patch` | `0` | 回溯 tick 修补，默认关闭 |
| `lilac_backtrack_tolerance` | `0` | 合法 tick 允许的偏差 |
| `lilac_angles` | `1` | L4D2 主动跳过角度检测，其他入口仍保留开关 |
| `lilac_angles_patch` | `1` | 仍会限制 pitch 并清零 roll |
| `lilac_chatclear` | `1` | 聊天换行/清屏过滤 |
| `lilac_filter_chat` | `1` | 非法聊天字符拦截 |
| `lilac_filter_name` | `2` | 名称非法字符过滤，换行进入 Ban 链 |
| `lilac_loss_fix` | `1` | 丢包超过 50% 时跳过部分瞄准特征 |

### 3.1 Lilac 的负值、零值和最小值

- `lilac_enable=0` 时所有 Lilac 检测入口直接返回。
- Aimbot/Aimlock 设置为大于 1 且小于 5 时，配置回调会强制改为 5；设置为 1 是日志模式。
- Bhop 的负值只记录，绝对值仍选择对应预设；`0` 关闭。
- ConVar、NoLerp、Chat、Name 的 `-1` 是日志模式，`0` 关闭。
- `lilac_ban=0` 会让统一 Ban 函数直接返回；模块仍可能写出“detected and banned”格式的日志，因此必须看实际 Ban 后端和玩家连接状态。

## 4. Lilac L4D2 逐模块解析

### 4.1 Aimbot：击杀事件回看角度和射击边沿

**源码**：`addons/sourcemod/scripting/lilac/lilac_aimbot.sp:35-381`，常量见 `lilac_globals.sp:99-109`。

#### 入口与时间窗

1. `player_death` 事件进入 `event_death_shared()`。
2. 攻击者必须是有效、存活、非 Bot，且进入服务器时间不少于约 `10.1s`；已经标记为 Aimbot Ban 的玩家跳过。
3. 击杀者与受害者眼睛距离小于 `350` 单位时，跳过 Snap 和 Angle-Repeat 的部分判断，但仍可能保留 Total-Delta 或 Autoshoot。
4. 事件延迟 `0.5s` 创建 DataPack 定时器。回看历史时，射击命令至少要发生在当前时间前约 `0.3s`；找不到射击边沿则使用事件发生时的环形索引。
5. 找到射击后回看射击前约 `0.5s` 的角度历史。历史源为 Lilac 的 330 格环形缓冲。

#### 五个特征

| 标志 | 精确条件 |
| --- | --- |
| Aim-Snap | 当前瞄准误差 `<` 上一次误差的 `20%`，且相邻角度变化 `> 10°` |
| Aim-Snap2 | 当前误差 `<` 上一次误差的 `10%`，且相邻角度变化 `> 5°` |
| Angle-Repeat | 射击前后邻近角度差 `< 10°`；射击 tick 与邻近角度差 `> 0.5°`；后者还要大于周边变化的 `5` 倍 |
| Autoshoot | 射击前后 3 个 tick 中恰好有 1 个 `IN_ATTACK`；连续出现两次，或与其他标志叠加，才加入 Autoshoot 标志 |
| Total-Delta | 回看窗口内累计角度变化 `> 450°`；常量为 `180 * 2.5` |

高丢包门控位于 `lilac_stock.sp:396-404`：`GetClientAvgLoss(client, NetFlow_Both) > 0.5` 且 `lilac_loss_fix=1` 时，清除 Snap/Autoshoot/Repeat，只保留 Total-Delta。这是最重要的网络误报保护之一。

#### 计数、衰减与处置

- 第一次正式 Aimbot 命中只增加计数，不写完整疑似日志；第二次开始管理员才看到计数。
- 每次正式命中创建 `600s` 衰减定时器；到期计数减 1。
- `lilac_aimbot=5` 且计数达到 5 才进入 Ban 判断；低于 5 只记录/告警。
- `lilac_ban=0` 或 `lilac_aimbot=1` 时只保留日志路径。

#### 误报判断

应同时查看击杀距离、受害者是否为特殊感染者、射击前 0.5 秒的角度序列、出站丢包和武器类型。L4D2 分支没有像 SMAC 那样的“生还者忙碌”专用门控，移动平台、近距离击杀和高延迟回放需要人工复核。

### 4.2 Aimlock：轻量队列加锁定持续时间

**源码**：`addons/sourcemod/scripting/lilac/lilac_aimlock.sp:47-264`。

#### 扫描和候选队列

- 主定时器每 `0.5s` 扫描；`lilac_aimlock_light=1` 时每轮最多处理 `5` 名玩家。
- `lilac_aimlock_light_test()` 回看最近 `0.5s` 角度，相邻变化 `>20°` 就把玩家放入约 `200s` 的候选队列。
- `lilac_is_player_in_aimlock_que()` 还会把已经有 Aimlock detection、Aimbot 已达到 2 次、最近 180 秒已有一次 Aimlock 的玩家保留在队列中；源码对入场时间 `<240s` 的玩家返回“在队列”，因此新加入玩家在轻量模式下会被优先纳入观察。
- 玩家最近传送/出生的普通跳过门槛约 `2s`；轻量测试单独使用约 `3s` 门槛。

#### 单次锁定条件

对不同队伍、存活且距离不小于 `300` 单位的目标，回看约 `0.6s`（`0.5 + 0.1`）：

1. 当前瞄准误差小于 `5°` 时累加连续锁定 tick。
2. 当前误差降到上一误差的 `10%` 以下。
3. 相邻角度跳变 `>20°`。
4. 连续锁定超过 `0.1s`。

目标距离小于 `300` 时本轮直接停止该玩家判断，以避免近身模型和碰撞造成锁定假象。

#### 正式 detection

- 单次锁定只是 suspicion；只有两次 suspicion 间隔小于 `180s` 才产生一次正式 Aimlock detection。
- 正式 detection 每次创建 `600s` 衰减计时器；首次正式命中只累计，第二次开始显示告警。
- 默认 `lilac_aimlock=10`；源码最小 Ban 阈值为 `5`。

人工复核时要取目标距离、玩家传送时间戳、0.6 秒角度序列和队伍状态。轻量模式限制每轮人数，CPU 与延迟高峰时可能让候选延后处理，但不会改变单次锁定公式。

### 4.3 Bhop：落地沿、空中 tick 和双层阈值

**源码**：`lilac_bhop.sp:39-177`，预设 `lilac_config.sp:767-809`，tick 率初始化 `lilac.sp:155-171`。

#### 事件定义

- 只在 `IN_JUMP` 从 0 变 1 的新按下沿处理。
- 同时要求实体带有 `FL_ONGROUND`，因此空中连续按键不会直接计为完美跳。
- 每次有效跳跃设置 `next_bhop = 当前 tick + ceil(tick_rate * 0.3)`，并增加 `perfect_bhops`。
- 落地后检查本轮最小条件；达到最大完美跳数时走即时 Ban 分支。

#### tick 率下限

| 服务器 tick 率 | `min` 最小值 | `max` 最小值 | `total` 最小值 |
| --- | ---: | ---: | ---: |
| `>50` | 5 | 10 | 1 |
| `<=50` | 10 | 20 | 3 |

`air = ceil(tick_rate * 0.3)`。默认 Medium 预设为：`min=7`、`jump buffer=-1`、`max=20`、`total=3`。Low/Custom 基准为 `min=10`、`jump buffer=5`、`max=25`、`total=5`；High 基准为 `min=5`、`jump buffer=8`、`max=20`、`total=1`，所有值仍会与 tick 率下限比较。

#### 处置

- 达到 `max`：立刻写 detection 并调用 Bhop Ban。
- 达到 `min`：增加一次 detection；每 `600s` 减 1；达到 `total` 后 Ban。
- 默认 `lilac_bhop=5` 对应 Medium，Bhop 的 Ban 覆盖为 `24*30*60 = 43200` 分钟，约 30 天。
- `sv_autobunnyhopping` 变化回调会更新 `force_disable_bhop`（`lilac_config.sp:733-737`）。初始化和插件热加载时应确认该值已同步，否则服务器自带自动跳跃可能产生大量正常命中。

#### 误报控制

不要只按“连续跳跃”判定；需要把 tick 率、`jump_ticks`、`perfect_bhops`、地面标志和 `sv_autobunnyhopping` 一起记录。低 tick 率服务器自动提高最小值，正是源码针对 L4D2 物理节奏的门控。

### 4.4 Macro：每秒按下沿达到 20 次

**源码**：`lilac_macro.sp:39-208`。

- 模块只在 `tick_rate` 为 `60..200` 时工作；否则 `macro_max=0`，模块实际停止计数。
- 每名玩家按服务器 tick 率建立一秒环形数组，统计 `IN_JUMP` 和 `IN_ATTACK` 的新按下沿，不统计按钮持续按住的 tick。
- 一秒内按下沿达到 `20`（`macro_max`）产生一次疑点，并立即清空该类一秒历史，避免同一批输入重复计数。
- 两类可分别启用：`lilac_macro_mode=1` 只看 Auto-Jump，`2` 只看 Auto-Shoot，`0` 两类都看。
- 每类正式计数每 `300s` 减 1；第一次只累计，第二次开始日志/提示；达到第 `5` 次进入处置。
- `lilac_macro_method=0` 调用 `KickClient`，`1` 调用 Lilac Ban 链；`lilac_macro=-1` 为日志模式，默认 `0` 关闭。

检查时区分“按下沿”与“持续按住”，并确认服务器 tick 率在 60 到 200 之间。高频武器、绑定脚本和网络重发都应通过原始按钮序列复核。

### 4.5 ConVar Checker：逐条查询、非法值单次进入封禁链

**源码**：`lilac_convar.sp:31-204`；全局常量 `lilac_globals.sp:99-101`。

#### 查询调度

- 每 `5s` 查询一名玩家的一条规则，按数组索引循环。
- 玩家入场时间少于 `60s` 时跳过查询。
- `sv_cheats` 非零，或最近 `30s` 内发生变化时，整个查询循环暂停。
- 一次正常响应会将 `query_failed` 清零并推进到下一条规则。
- 连续未响应时 `query_failed` 每轮加 1；条件是 `query_failed > 24`，所以第 25 次失败才 Kick。源码日志文本按 `24*5=120s` 计算，定时器调度从第一次请求开始实际通常接近 125 秒，报告中两者都要保留。

#### Lilac 规则表

| 分组 | 规则和值 |
| --- | --- |
| 时间/输入 | `cl_clock_correction >= 1`、`cl_cmdrate >= 10`、`cl_phys_timescale = 1`、`host_timescale = 1` |
| 视角 | `cl_pitchdown = 89`、`cl_pitchup = 89` |
| 调试开关 | `cl_leveloverview=0`、`cl_overdraw_test=0`、`cl_showevents=0`、`mem_force_flush=0` |
| 材质/填充 | `mat_fillrate=0`、`mat_proxy=0`、`mat_wireframe=0` |
| 网络模拟 | `net_blockmsg=0`、`net_droppackets=0`、`net_fakejitter=0`、`net_fakelag=0`、`net_fakeloss=0` |
| 画面比例与可见性 | `r_aspectratio=0`、`r_ClipAreaPortals=1`、`r_colorstaticprops=0`、`r_DispWalkable=0`、`r_DrawBeams=1`、`r_drawbrushmodels=1`、`r_drawclipbrushes=0`、`r_drawdecals=1` |
| 模型/世界 | `r_drawentities=1`、`r_drawmodelstatsoverlay=0`、`r_drawopaqueworld=1`、`r_drawothermodels=1`、`r_drawparticles=1`、`r_drawrenderboxes=0`、`r_drawtranslucentworld=1` |
| 线框/天空 | `r_modelwireframedecal=0`、`r_portalsopenall=0`、`r_shadowwireframe=0`、`r_showenvcubemap=0`、`r_skybox=1`、`r_visocclusion=0`、`vcollide_wireframe=0` |
| 声音/作弊 | `snd_show=0`、`snd_visualize=0`、`sv_cheats=0` |

收到响应但值不符合规则时，源码立即调用 `lilac_forward_allow_cheat_detection`，通过后写 `cvar_invalid` 并进入 `CHEAT_CONVAR` Ban 链；没有多次值累计。`sv_cheats` 变化的 30 秒暂停是为了避免服务器切换调试状态时误判。

### 4.6 Lerp 与 NoLerp

**源码**：`lilac_lerp.sp:33-127`。

- 定时器周期 `5s`，读取 `m_fLerpTime`。
- 当 `lerp*1000 > lilac_max_lerp` 且 `lilac_max_lerp >= 105` 时直接 Kick；默认上限 `105ms`。
- NoLerp 基准为 `min_lerp_possible = 1 / sv_maxupdaterate`。
- 当 `lerp <= min_lerp_possible * 0.95` 时进入 `CHEAT_NOLERP` Ban 链。
- 插值 ratio 小于 `1` 时，源码将 `ignore_nolerp_all` 置为真并停用整体 NoLerp；`min_lerp_possible < 0.005`、玩家单独标记忽略或插件重载初始化也会跳过。

Lerp 超限是 Kick 路径，NoLerp 是检测后 Ban 路径；两者不要在日志中混为同一种动作。先核对服务器的 `sv_maxupdaterate`、插值 ratio 和插件加载时机。

### 4.7 Ping：100 秒高延迟临时 Ban

**源码**：`lilac_ping.sp:20-98`。

- `lilac_max_ping < 100` 时整个模块关闭；默认 `0`。
- 玩家入场未满 `120s` 时跳过。
- 使用 `GetClientAvgLatency(client, NetFlow_Outgoing) * 1000`，即出站平均延迟。
- 每 `5s` 超过阈值，`ping_high` 加 1；低于阈值时通过 toggle 机制大约每 `10s` 减 1。
- `ping_high >= 20`，约连续高 Ping `100s`，调用 `BanClient` 对 AuthID 临时 Ban `3` 分钟。该路径绕过统一 `lilac_ban` 开关。
- `lilac_max_ping_spec >= 30` 时，计数达到 `max_ping_spec/5` 会移到观察队并提示剩余时间；恢复后允许回队。

日志中出现 “high ping” 时先确认是 AuthID 临时 Ban，不能按永久封禁统计。丢包、路由切换、服务器负载和真实出站延迟都要纳入复核。

### 4.8 Backtrack patch：修补异常 tick，不产生 detection

**源码**：`lilac_backtrack.sp:30-115`。

合法条件：

```text
abs((previous_tickcount + 1) - current_tickcount) <= lilac_backtrack_tolerance
```

- 默认 `lilac_backtrack_patch=0`、`lilac_backtrack_tolerance=0`。
- 玩家传送后约 `2s` 跳过。
- 发现非法 tick 后进入约 `1.1s` timeout；按出站延迟换算锁定 tick，并把偏移限制在约 `+-0.2s` 的 tick 范围。
- 只改写返回给游戏的 tickcount，没有累计 detection，也不调用 Ban。

Lilac 配置说明明确建议默认关闭以减少与 SMAC EyeTest 的输入处理冲突。若启用，必须记录两个插件加载顺序和谁最终返回 `Plugin_Handled`。

### 4.9 基础角度入口与角度修补是两件事

**源码**：检测 `lilac_angles.sp:19-51`，修补 `:53-65`，调用点 `lilac.sp:339-345`。

- `lilac_angles_check()` 在 `GAME_L4D2` 直接 `return`，因此 L4D2 不执行基础 pitch/roll 越界检测。
- `lilac_angles_patch()` 没有同样的 L4D2 return：仍把 pitch 限制到 `+/-89.01`，并把 roll 清零。
- `lilac_angles=1` 不会改变“检测入口跳过”的事实；`lilac_angles_patch=1` 仍可能修改输入。

这也是 Lilac 与 SMAC 同时加载时的冲突点：报告中必须把“角度检测关闭”和“角度修补开启”分开记录。

### 4.10 Chat/Name 字符过滤

**源码**：`lilac_string.sp:20-214,230-281`。

- 过滤非法 UTF-8、控制字符、换行/回车、私用区和不可见字符。
- U+FDFD 重复超过 `3` 个时标记 wide-char spam。
- 聊天换行进入 `CHEAT_CHATCLEAR`；`lilac_chatclear=1` 时进入 Ban 链。
- `lilac_filter_name=2` 时名称换行进入 `CHEAT_NEWLINE_NAME` Ban 链；其他非法名称通常 Kick。
- `lilac_filter_chat=1` 直接阻断异常聊天文本；它是输入层过滤，不是瞄准行为 detection。

人工复核应保留原始字节、UTF-8 解码结果和触发字符，避免把正常中文姓名误归为非法字符。

### 4.11 Lilac 统一 Ban 链和覆盖时间

`lilac_ban_client()` 的顺序是：

1. MaterialAdmin（启用且 native 存在）；
2. SourceBans++；
3. 旧 SourceBans；
4. SourceMod BaseBans 的 `BanClient`。

成功调用后约 `5s` 创建 Kick 定时器。`lilac_ban_length=0` 表示全局永久；Bhop 覆盖 `43200` 分钟，Macro 覆盖 `15` 分钟。`lilac_ban=0` 会在链首直接返回，日志中的 Ban 文案不等于玩家已写入封禁后端。

## 5. SMAC L4D2 配置和统一入口

### 5.1 源码默认值与仓库配置值

源码默认值来自各 `.sp` 的 `SMAC_CreateConVar`；当前仓库配置来自 `C:\Users\Administrator\Desktop\脚本优化\smac-master\cfg\sourcemod\smac.cfg`。

| ConVar | 源码默认 | 当前仓库值 | 作用 |
| --- | ---: | ---: | --- |
| `smac_aimbot_ban` | `0` | `6` | Aimbot detection 达到此数才 Ban；`0` 只记录 |
| `smac_autotrigger_ban` | `0` | 未覆盖 | AutoTrigger 是否自动 Ban |
| `smac_eyetest_ban` | `0` | 未覆盖 | EyeTest 是否自动 Ban |
| `smac_eyetest_compat` | `1` | 未覆盖 | `0` 才启用按钮篡改子检查 |
| `smac_antispam_cmds` | `20` | `20` | 每秒允许的命令数 |
| `smac_antispam_connect` | `2` | `2` | 同 IP 重连时间窗 |
| `smac_ban_duration` | `0` | `0` | 分钟，`0` 为永久 |
| `smac_log_verbose` | `0` | `1` | 日志附加上下文 |
| `smac_validate_auth` | `0` | `0` | 10 秒未认证是否 Kick |
| `smac_wallhack` | `1` | 未覆盖 | 是否启用信息隔离 |
| `smac_wallhack_maxtraces` | `1280` | 未覆盖 | 每 tick 最大 trace 数 |

### 5.2 统一 forward 和 Ban

- `smac.sp:404-443` 的 `SMAC_CheatDetected` 先触发共享 forward；被其他插件拦截后，模块动作不会继续。
- `smac.sp:326-355` 的 `SMAC_Ban` 优先 SourceBans，失败后调用 `BanClient`，随后 Kick 在线玩家。
- `smac_ban_duration=0` 为永久；自动 Ban 多数模块默认关闭，必须按模块 ConVar 判断。
- 日志固定为 `addons/sourcemod/logs/SMAC.log`；仓库将 `smac_log_verbose` 设为 1 时，会记录地图、位置、角度、武器、队伍和延迟等上下文。

## 6. SMAC L4D2 逐模块解析

### 6.1 Aimbot：45 度相邻角度跳变

**源码**：`smac_aimbot.sp:37-44,57-65,178-188,327-426`。

#### 采样和触发

- 保存最多约 `500ms` 的眼角历史，数组上限 `64` 条。
- 击杀事件触发分析；受害者与攻击者距离必须 `>=200` 单位。
- 相邻历史角度差大于 `45°`（`AIM_ANGLE_CHANGE`）产生一次候选。
- L4D2 门控只分析生还者队伍 2，且 `L4D2_IsSurvivorBusy(client)` 为假；出生和传送会清空历史，并在约 `0.1s` 后再次清空。

#### 计数和动作

- 首次命中只建立计数；每次命中创建 `600s` 衰减计时器。
- 源码把 `smac_aimbot_ban` 的正值小于 `4` 强制抬到 `4`；`4` 是源码允许的最低 Ban 门槛。
- 当前仓库值为 `6`，因此第 6 个未衰减 detection 才调用 `SMAC_Ban`。
- 近距离、忙碌状态、传送后的清空和首个命中抑制共同降低误报；观察日志时仍要检查 0.5 秒角度序列。

### 6.2 AutoTrigger：精确落地帧和攻击边沿

**源码**：`smac_autotrigger.sp:37-39,55-59,86-193`。

- Bhop 子类使用 `MIN_JUMP_TIME=0.500s`：落地后等待精确允许帧，新的 `IN_JUMP` 按下沿命中一次。
- AutoFire 子类统计 `IN_ATTACK` 的按下/释放边沿；阈值 `g_iAttackMax = RoundToNearest(tickrate/3)`。
- 每类累计到 `TRIGGER_DETECTIONS=20` 触发 `Detection_AutoTrigger`，触发后该类计数归零。
- 每 `4s` 两类计数各减 1；`smac_autotrigger_ban=1` 才自动 Ban，源码默认 0。

精确帧检测对服务器 tick 率和地面状态敏感。复核时要记录 `fCheckTime`、按钮边沿和 `FL_ONGROUND`，不要只统计跳跃次数。

### 6.3 EyeTest：usercmd 复用、tickcount 和角度完整性

**源码**：`smac_eyetest.sp:84-85,113-115,157-428`。

#### 命令序列

需要 `FEATURECAP_PLAYERRUNCMD_11PARAMS`；不满足时插件能力不可用。满足时：

- `cmdnum <= 0` 直接返回 `Plugin_Handled`。
- `tickcount <= 0` 进入 resetting 状态，先等待序列恢复。
- 存活状态下 `cmdnum` 递减，触发 `Detection_UserCmdReuse`，阻断当前命令；检测后约 `30s` 冷却。
- 相同 `cmdnum` 时，tickcount 既不是上一次值、也不是上一次值加 1、也不是当前服务器 tick，触发 `Detection_UserCmdTamperingTickcount` 并阻断。
- `smac_eyetest_compat=0` 时才比较移动/计分按钮变化，触发 `Detection_UserCmdTamperingButtons`；默认兼容值为 1。
- 攻击命令跳号时随机化 seed，是被动扰动，不是累计 Ban 计数。

#### L4D2 角度区间和门控

- pitch 合法开区间：`(-135, 135)`。
- yaw 合法开区间：`(-270, 420)`。
- 进入机枪时跳过角度违规判断。
- 只检查生还者队伍 2；`L4D2_IsSurvivorBusy`、`FL_FROZEN`、`FL_ATCONTROLS` 时跳过。
- 违规进入 `Detection_Eyeangles`；`smac_eyetest_ban=1` 才自动 Ban，源码默认 0；违规后约 30 秒不重复报告。

### 6.4 Speedhack：tick 预算和命令丢弃

**源码**：`smac_speedhack.sp:40-129`。

- 每 `0.1s` 根据引擎时间补充 tick，余额上限 `ceil(tickrate*2)`。
- 存活玩家每个 `OnPlayerRunCmd` 消耗 1 tick；余额为 0 时返回 `Plugin_Handled`，过量命令被丢弃。
- 余额耗尽且出站延迟变化不超过 `5ms` 时，检测计数加 1；达到 `30` 次报告 `Detection_Speedhack`。
- 报告后设置约 `30s` 冷却；模块主要是限流和日志，没有独立自动 Ban 路径。

误报重点是延迟突变：源码只有在延迟稳定时才累计，报告时要同时查看 `GetClientLatency(NetFlow_Outgoing)` 序列。

### 6.5 Spinhack：每秒 1440 度、连续 15 秒

**源码**：`smac_spinhack.sp:37-124`。

- 每个 usercmd 累计 yaw 变化，按下 `IN_LEFT` 或 `IN_RIGHT` 时排除该段变化。
- 每秒累计角度超过 `1440°`，连续秒数加 1；低于阈值立即清零。
- 连续 `15s` 且客户端 `sensitivity <= 6` 时报告 `Detection_Spinhack`。
- 只有日志和管理员通知，没有独立 Ban 分支。

### 6.6 ConVar Checker：比较器、重试和复制保护

**源码**：`smac_cvars.sp:41-44,76-150,369-580`。

#### 查询时间线

1. 玩家通过认证后，先以 `0.1s` 重复定时器取下一条规则。
2. 首次未响应等待 `20s`，后续重试每 `10s`。
3. `MAX_REQUERY_ATTEMPTS=4`，条件为 `>4`，第 5 次重试仍失败时 Kick。
4. 从第一次请求到第 5 次失败约 `20 + 4*10 = 60s`，再加调度误差。

支持比较器：`Equal`、`StringEqual`、`Replicated`、`Greater`、`Less`、`Between`、`Outside`、`NonExist`。违规动作由规则注册时指定，常见为 Ban 或 Kick；查询无响应固定 Kick。

#### L4D2 实际注册规则

- 不应存在的作弊/插件 ConVar：`0penscript`、`aim_bot`、`aim_fov`、`bat_version`、`beetlesmod_version`、`est_version`、`eventscripts_ver`、`fm_attackmode`、`lua-engine`、`lua_open`、`mani_admin_plugin_version`、`maniadminhacker`、`maniadmintakeover`、`metamod_version`、`openscript`、`openscript_version`、`runnscript`、`smadmintakeover`、`sourcemod_version`、`tb_enabled`、`zb_version`。其中规则表给每项标注 Ban 或 Kick 动作。
- 固定值规则：`cl_clock_correction=1`、`cl_leveloverview=0`、`cl_overdraw_test=0`、`cl_phys_timescale=1`、`cl_showevents=0`、`fog_enable=1`、`mat_fillrate=0`、`mat_measurefillrate=0`、`mat_proxy=0`、`mat_showlowresimage=0`、`mat_wireframe=0`、`mem_force_flush=0`、`r_aspectratio=0`、`r_colorstaticprops=0`、`r_dispwalkable=0`、`r_drawbeams=1`、`r_drawbrushmodels=1`、`r_drawclipbrushes=0`、`r_drawdecals=1`、`r_drawentities=1`、`r_drawmodelstatsoverlay=0`、`r_drawopaqueworld=1`、`r_drawothermodels=1`、`r_drawparticles=1`、`r_drawrenderboxes=0`、`r_drawskybox=1`、`r_drawtranslucentworld=1`、`r_shadowwireframe=0`、`r_skybox=1`、`r_visocclusion=0`、`snd_show=0`、`snd_visualize=0`、`vcollide_wireframe=0`。
- 复制型规则：`host_timescale`、`mp_fadetoblack`、`sv_allowminmodels`、`sv_cheats`、`sv_competitive_minspec`、`sv_consistency`、`sv_footsteps`；服务端变化会更新期望值并向客户端重发，复制窗口约 `30s`。
- 数值边界：`mat_dxlevel >= 80`。

客户端响应成功后，比较器通过即进入下一条；失败则构造 `Detection_CvarViolation`，由该规则的 Action 决定 Mute、Kick 或 Ban。`sv_cheats` 变为非零时服务端立即强制回 0，并避免把调试切换期间的返回值当作作弊。

### 6.7 Commands：命令黑名单、参数阻断和每秒 21 次门槛

**源码**：`smac_commands.sp:56-85,90-180,292-447`。

#### 命令频率

- `smac_antispam_cmds` 默认和当前仓库值均为 `20`。
- 每秒定时器把 `g_iCmdCount` 清零；代码先 `++count` 再比较 `> limit`，因此第 `21` 个未忽略命令触发 `Detection_CommandSpamming`，随后 Kick。
- L4D2 将 `choose_closedoor`、`choose_opendoor` 放入忽略表。
- L4D2 明确跳过通用 `give` 参数监听，以保留服务器配置流程；不能把它写成“保留 give 检测”。

#### 黑名单动作

- 大量调试、实体、服务器状态命令为 `Action_Block`，只阻断当前命令。
- `changelevel` 为 `Action_Ban`。
- `speed.toggle` 为 `Action_Kick`。
- 命令动作经过 `SMAC_CheatDetected` forward 后才执行，所以仍需检查共享拦截插件。

#### `ent_create`/`ent_fire` 参数

- 参数字符串长度大于 `500` 直接阻断。
- 包含 `admin`、`alias`、`logic_*`、`ma_`、`meta`、`mp_`、`point_clientcommand`、`point_servercommand`、`quit`、`rcon`、`restart`、`sm`、`sv_`、`taketimer` 等危险子串时阻断。
- `say`/`say_team` 文本含控制字符，或空格数超过 `64`（第 65 个起）时阻断并提示玩家。

### 6.8 Client Protection：连接、名称和成就滥用

**源码**：`smac_client.sp:56-74,91-184,261-425`。

- 地图开始后前 `20s` 不启动连接 spam 计时，给正常连接留出窗口。
- `smac_antispam_connect=2` 时，同一 IP 在 2 秒窗口内重复连接，调用 IP 临时 Ban `60s`；有连接扩展时在预连接阶段处理，否则在连接后处理。
- `smac_validate_auth=1` 时，入场 `10s` 仍未授权的玩家 Kick；当前仓库值为 `0`。
- 名称必须至少 2 字节；首尾空格、`&`、控制字符、`%`、`0x7F`、非法 UTF-8 和黑名单 Unicode 被拒绝。
- 名称变更累计达到 `5` 触发 `Detection_NameChangeSpam` 并 Kick；每 `10s` 计数减 1。
- 成就事件累计达到 `5` 时返回 `Plugin_Handled`；每 `10s` 计数减 1。

这些是输入滥用保护，和瞄准 detection 独立。名称误判需要保留 UTF-8 字节和 Unicode 码点作为证据。

### 6.9 Wallhack：信息隔离而不是累计 Ban

**源码**：`smac_wallhack.sp:95-145,243-252,542-655,770-814`。

- `smac_wallhack=1` 默认启用；`smac_wallhack_maxtraces=1280` 限制每 tick trace 数。
- 需要 `FEATURECAP_PLAYERRUNCMD_11PARAMS`。
- 通过 `SDKHook_SetTransmit` 控制玩家和武器实体是否发送给客户端，属于信息隔离，不产生传统 detection count。
- 维护 PVS/FOV/可见性缓存，缓存约 `0.75s`；每次更新综合移动预测、延迟补偿、模型中心、武器尖端、外框 4 点和内框 4 点 trace。
- L4D2 门控：主要被测者为队伍 2；`L4D_IsPlayerGhost` 目标直接按不可见处理；感染者忙碌或生还者忙碌时按可见处理；监听 `player_first_spawn` 和 `ghost_spawn_time`；忽略 `UI/Pickup_GuitarRiff10.wav`。
- `smac_wallhack=0` 时停止信息隔离；降低 `maxtraces` 可减少 CPU，但可见性缓存更新会更滞后。

验证 Wallhack 应看客户端是否收到实体，而不是寻找 Ban 日志；可用服务器录制和实体传输日志对照 PVS/FOV 状态。

### 6.10 L4D2 Fixes：三个明确的输入/漏洞修补

**源码**：`smac_l4d2_fixes.sp:38-40,46-55,80-163`，插件只接受 `Engine_Left4Dead2`。

1. **换队伤害窗口**：队伍变化后 `RECENT_TEAM_CHANGE_TIME=1.0s` 内，`SDKHook_OnTakeDamage` 对客户端受害者返回 `Plugin_Handled`。
2. **手枪交互窗口**：使用 `CPistol` 后 `RESET_USE_TIME=0.5s` 内清除 `IN_USE`。
3. **Tank 双攻击输入**：实体 `m_zombieClass == 8` 同时带 `IN_ATTACK` 和 `IN_ATTACK2` 时清除 `IN_ATTACK2`。

这三个路径直接修补游戏状态，不进入累计检测计数。日志中没有 detection 并不表示修补未执行，应通过输入前后值和伤害事件验证。

## 7. 两套系统的 L4D2 对照

| 能力 | Lilac | SMAC | 关键差异 |
| --- | --- | --- | --- |
| Aimbot | 多特征；击杀前 0.5 秒；默认 5 次 | 相邻跳变 `>45°`；默认仓库值 6 次 | Lilac 特征细，SMAC 生还者/忙碌门控更明确 |
| Aimlock | 独立模块；两次疑点/180 秒；默认 10 次 | 无独立 Aimlock | Lilac 的 0.5 秒扫描和 200 秒候选队列是核心 |
| Bhop | Medium `min7/max20/total3`，最大值即时 Ban | AutoTrigger 精确落地帧，20 次事件 | 计数单位不同，不能直接相加 |
| Macro/AutoFire | 一秒按下沿 `>=20`，第 5 次处置，默认关闭 | 攻击边沿，约 tickrate/3 形成事件，20 次处置 | Lilac 统计频率，SMAC 统计边沿序列 |
| ConVar | 每 5 秒一条，非法值单次 Ban，第 25 次无响应 Kick | 首次 20 秒、后续 10 秒，第 5 次重试 Kick | 两套规则表和值不完全相同 |
| 角度 | L4D2 检测入口跳过，但 patch 仍可能改 pitch/roll | EyeTest 检查 pitch/yaw，带队伍和忙碌门控 | 同时启用时先确认输入处理顺序 |
| Backtrack/Speed | Lilac 修补 tick；无专用 speed detection | EyeTest 检查 tick，Speedhack 限流 | 可能同时返回 `Plugin_Handled` |
| Wallhack | 无实体传输隔离 | SetTransmit + trace + 缓存 | SMAC 是信息防护层，不是累计 Ban |
| Ping/Lerp | Ping、Lerp、NoLerp 专项策略 | 无对应公共模块 | 网络条件会影响 Lilac 误报门控 |

## 8. 低误报部署方法

### 8.1 先记录最终配置

启动后记录以下命令输出，而不是只看文件默认值：

```text
sm plugins list
smac_aimbot_ban
smac_autotrigger_ban
smac_eyetest_ban
smac_eyetest_compat
smac_antispam_cmds
smac_antispam_connect
smac_ban_duration
smac_log_verbose
lilac_enable
lilac_ban
lilac_aimbot
lilac_aimlock
lilac_bhop
lilac_macro
lilac_convar
lilac_max_lerp
lilac_max_ping
lilac_backtrack_patch
lilac_backtrack_tolerance
```

### 8.2 分阶段打开动作

建议先保留日志和输入保护，观察至少一张完整战役：

1. Lilac：`lilac_ban 0`，保留 Aimbot/Aimlock/Bhop/ConVar 日志；确认 `sv_maxupdaterate`、tick 率、丢包门控。
2. SMAC：保留 `smac_aimbot_ban 0`、`smac_autotrigger_ban 0`、`smac_eyetest_ban 0`，将 `smac_eyetest_compat` 保持 `1`；先分析日志。
3. 确认没有批量误报后，再逐模块设置 Ban 阈值。Aimbot 至少保留 Lilac 的 5 次或 SMAC 的 4 次源码下限，不把单条疑点当作永久 Ban 依据。
4. 开启 Wallhack 时监控 CPU 和实体传输；`maxtraces` 调整要同时观察可见性延迟。

### 8.3 每条疑点应保存的证据

| 模块 | 最小证据 |
| --- | --- |
| Lilac Aimbot | detection 编号、Delta、TotalDelta、flags、击杀距离、武器、丢包 |
| Lilac Aimlock | 两次 suspicion 时间、目标距离、0.6 秒角度序列、传送时间 |
| Lilac Bhop/Macro | tick 率、按钮边沿、地面标志、窗口内计数 |
| Lilac ConVar | cvar 名、原始值、查询结果和 `sv_cheats` 时间戳 |
| Lilac Ping/Lerp | 出站延迟、丢包、`m_fLerpTime`、`sv_maxupdaterate` |
| SMAC Aimbot/EyeTest | cmdnum、tickcount、角度、队伍、忙碌/冻结标志、冷却时间 |
| SMAC Commands/Client | 原始命令、参数长度、每秒序号、名称 UTF-8 字节 |
| SMAC Wallhack | PVS/FOV、trace 点、SetTransmit 决定和缓存时间 |

### 8.4 两套插件同时加载的注意点

- 两套插件都会挂 `OnPlayerRunCmd`；SMAC Speedhack/EyeTest 和 Lilac Backtrack 都可能返回 `Plugin_Handled`。记录插件加载顺序，并在测试服务器复现命令序列。
- Lilac 角度 patch 默认开启，SMAC EyeTest 仍会看到 patch 后角度；若要分析原始角度，先单独记录 Lilac patch 开关。
- 两套 ConVar Checker 都会查询客户端。检查同一时间是否有服务器调试切换，避免将查询暂停窗口解释为作弊。
- 统一 Ban 后端可能重复写入；日志检索时以 SteamID、时间和 detection 类型去重。

## 9. 日志判读和验证命令

### 9.1 日志分类

- Lilac：`addons/sourcemod/logs/lilac.log`；数据库启用时还会写 `database_log` 记录。
- SMAC：`addons/sourcemod/logs/SMAC.log`；`smac_log_verbose=1` 时附加位置、角度、武器、队伍和延迟。
- `suspected` 表示达到模块日志门槛，不等价于 Ban；`Detection_*` 表示共享 forward 已触发；Kick/Ban 需要继续看到对应动作日志或后端记录。

### 9.2 本机源码核对命令

在仓库根目录运行：

```powershell
$lilac = 'C:\Users\Administrator\Desktop\脚本优化\sm-plugin-lilac-master\sm-plugin-lilac-master'
$smac = 'C:\Users\Administrator\Desktop\脚本优化\smac-master'

rg -n 'left4dead2|GAME_L4D2|CMD_LENGTH|QUERY_MAX_FAILURES|AIMBOT_MAX_TOTAL_DELTA|AIMLOCK_BAN_MIN' `
  "$lilac\addons\sourcemod\scripting"
rg -n 'Game_L4D2|AIM_ANGLE_CHANGE|TRIGGER_DETECTIONS|MAX_REQUERY_ATTEMPTS|RECENT_TEAM_CHANGE_TIME|smac_wallhack_maxtraces' `
  "$smac\addons\sourcemod\scripting"
Get-Content "$smac\cfg\sourcemod\smac.cfg" -Encoding UTF8
```

检查 Markdown 文件本身：

```powershell
Get-Item -LiteralPath 'C:\Users\Administrator\Desktop\脚本优化\README.md' |
  Select-Object FullName,Length,LastWriteTime
(Get-Content -LiteralPath 'C:\Users\Administrator\Desktop\脚本优化\README.md' -Encoding UTF8).Count
$f = (Select-String -LiteralPath 'C:\Users\Administrator\Desktop\脚本优化\README.md' -Pattern '^```' -Encoding UTF8).Count
"fence_count=$f even=$($f % 2 -eq 0)"
```

## 10. 源码索引

### Lilac

- 入口与历史：`addons/sourcemod/scripting/lilac.sp:84-174,301-350`
- 常量和状态：`addons/sourcemod/scripting/lilac/lilac_globals.sp:22,99-175`
- Aimbot：`addons/sourcemod/scripting/lilac/lilac_aimbot.sp:35-381`
- Aimlock：`addons/sourcemod/scripting/lilac/lilac_aimlock.sp:47-264`
- Bhop：`addons/sourcemod/scripting/lilac/lilac_bhop.sp:39-177`
- Macro：`addons/sourcemod/scripting/lilac/lilac_macro.sp:39-208`
- ConVar：`addons/sourcemod/scripting/lilac/lilac_convar.sp:31-204`
- Lerp/NoLerp：`addons/sourcemod/scripting/lilac/lilac_lerp.sp:33-127`
- Ping：`addons/sourcemod/scripting/lilac/lilac_ping.sp:20-98`
- Backtrack：`addons/sourcemod/scripting/lilac/lilac_backtrack.sp:30-115`
- Chat/Name：`addons/sourcemod/scripting/lilac/lilac_string.sp:20-281`
- 配置和 Ban 链：`addons/sourcemod/scripting/lilac/lilac_config.sp:23-142`、`lilac_stock.sp:219-318`

### SMAC

- 核心入口和统一处置：`addons/sourcemod/scripting/smac.sp:76-123,326-443`
- Aimbot：`addons/sourcemod/scripting/smac_aimbot.sp:37-44,178-426`
- AutoTrigger：`addons/sourcemod/scripting/smac_autotrigger.sp:37-193`
- EyeTest：`addons/sourcemod/scripting/smac_eyetest.sp:84-85,157-428`
- Speedhack：`addons/sourcemod/scripting/smac_speedhack.sp:40-129`
- Spinhack：`addons/sourcemod/scripting/smac_spinhack.sp:37-124`
- ConVar Checker：`addons/sourcemod/scripting/smac_cvars.sp:41-44,76-580`
- Commands：`addons/sourcemod/scripting/smac_commands.sp:56-447`
- Client Protection：`addons/sourcemod/scripting/smac_client.sp:56-425`
- Wallhack：`addons/sourcemod/scripting/smac_wallhack.sp:95-145,243-252,542-655,770-814`
- L4D2 修补：`addons/sourcemod/scripting/smac_l4d2_fixes.sp:38-163`
- 当前配置：`cfg/sourcemod/smac.cfg:8-47`

## 11. 最终判定标准

1. 先确认插件识别到 `left4dead2`，再确认模块开关和最终 ConVar 值。
2. 一次角度跳变、一次完美跳、一次非法值或一次命令阻断，含义不同；只有源码明确进入 detection 或动作函数，才算对应模块完成判定。
3. 有时间窗的模块必须按衰减计数计算：Lilac Aimbot/Aimlock/Bhop/SMAC Aimbot 为 `600s`，Lilac Macro 为 `300s`，SMAC AutoTrigger 为 `4s`，SMAC 命令为 `1s`，SMAC 名称/成就为 `10s`。
4. 误报复核的核心数据是队伍/忙碌/传送状态、距离、tick 率、出站延迟、丢包、原始按钮边沿和角度历史。
5. 处置开关必须单独确认：Lilac `lilac_ban`、模块负值日志模式、SMAC 的各模块 Ban ConVar、共享 forward 和实际 Ban 后端。

按上述链路，L4D2 服务器可以把“输入异常识别”“已知配置和命令阻断”“实体信息隔离”“L4D2 漏洞修补”分开运营，既保留制止外挂的有效动作，也让每个阈值和误报来源可追溯到源码。
