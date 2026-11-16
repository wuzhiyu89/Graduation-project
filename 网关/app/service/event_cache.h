#ifndef __EVENT_CACHE_H__
#define __EVENT_CACHE_H__

#include <stdint.h>
#include <stdbool.h>

/* ==========================================================================
 * 断线事件队列 —— ★2026-11-11 起直接落在外挂 Flash 的 EVENT 环★
 * ==========================================================================
 * 【为什么存"紧凑记录"而不是现成的 JSON 文本】
 *   若每条事件存它渲染好的完整对象文本(实测约 192 B), 2 MB 只能放 1 万条,
 *   而且写一次要擦 3 个扇区; 紧凑记录 28 B, 一个 64 B 槽装得下, 2 MB ≈ 3.2 万条。
 *   补报时再由渲染层用 cloud_proto_build_*_data() 还原。
 *   ⚠️ 2026-10-28 更正: 原来这里按"16 B/条 = 8 KB"算, 是 UID 还是 3 字节时的账;
 *      UID 扩到 13 字节后单条变 28 B —— 做预算时别再按 8 KB 算。
 *
 * 【★2026-11-11: 存储从"RAM 环 256 条 + 满时溢出 Flash"改成"Flash 单写"★】
 *   用户拍板方案 A（Flash 就是队列），四条口径见 docs/缓存分层规划.md：
 *     Q1 在线正常时不写 Flash（只有发不出去的才入队，见 app_events.c）；
 *     Q2 补报严格按 ts 递增 —— 单写天然满足；
 *     Q3 不考虑老板子 —— 不再保留 RAM 后端；
 *     Q4 事件与遥测各一个环，不合并（两者 topic、消费者都不同）。
 *   改之前的老毛病：RAM 装尾段、Flash 装头段，补报从 RAM 队头开始 ⇒ 顺序倒挂，
 *   且"刚断网、RAM 还没满"时一条都没进 Flash（最该保护的窗口反而最脆弱）。
 *   改之后：2 MB / 64 B ≈ 32,672 条，按 200 条/天算约 160 天；掉电最多丢 1 条。
 *
 * 【保真边界 —— 刻意写清楚, 别当成"完整保留"】
 *   · ★ `ts` 是【原始发生时刻】, 补报不改 ★ —— 这是文档明确要求的,
 *     也是这个模块存在的第一理由;
 *   · `msgId` —— **序号部分存下来了**(第 12 条, 2026-09-30), 见下面专门一段。
 *   · `event.action` 的 `results[]`(多台设备的执行结果): 用 `seq` 把
 *     **同一次动作产生的多条记录**归为一组, 渲染时合成一个 results 数组。
 *     所以一次动作涉及 N 台设备就占 N 条记录(而不是 1 条)。
 *   · `event.buffered` 本身**不进缓存** —— 它就是补报的载体, 不该递归缓存。
 *
 * 【两种"丢"是不同的事, 别混】
 *   1. 槽满淘汰最旧: **断网期间超量**时的必然取舍。旧数据可以丢,
 *      但必须可观察 —— `event_cache_dropped()` 如实计数, 不静默丢。
 *   2. **掉电丢失**: 现在最多丢"待提交槽"里那 1 条（见下），不再是整批。
 * ==========================================================================
 * 【msgId 的保真(第 12 条, 2026-09-30)】
 *   用户清单第 12 条: "buffered 保留原 msgId"。
 *   原方案是"不存 msgId、补报时重新分配" —— 于是同一个事件在线时叫 `s007`、
 *   补报时变成 `b012`, 平台按 msgId 做幂等/去重时对不上(该去重的去不掉,
 *   或者把同一条当成两条)。
 *
 *   【怎么做到补报不换号又不多吃 RAM】
 *   本工程的 msgId 统一为 8 位小写十六进制(`utils_gen_msg_id_seq`)。补报不能重新
 *   随机, 所以按 `kind` 选 salt、用记录里的 2 B 序号重建:
 *       `uint16_t msg_seq` = 2 B/条, 256 条 = **512 B**
 *   —— 而不是存 16 B 的整个字符串。同一条记录无论补报几次, ID 都相同 ✓。
 *
 *   ⚠️ 边界: `msg_seq == 0` = "这条记录没带序号"(老记录/未赋值), 补报时回落到
 *      `utils_gen_msg_id()` 重新分配 —— 宁可换一个 id, 也不发一个假的 id。
 *   ⚠️ 序号在生产者侧按 `% 1000` 回绕, 与在线路径同一个限制。
 * ========================================================================== */

/* 容量不再是编译期常量 —— 由 EVENT 分区大小决定（2 MB / 64 B ≈ 32,672 条）。
 * 用 event_cache_capacity() 取，别写死数字。 */

/* 事件种类(渲染层据此选择 cloud_proto_build_*_data) */
typedef enum
{
    EC_KIND_STATUS_CHANGE = 1,  /* -> event.status_change: a=to状态, b=from状态 */
    EC_KIND_ACTION        = 2,  /* -> event.action:        a=档位gear, b=trigger */
    EC_KIND_DEVICE        = 3   /* -> event.device:        a=事件码, b=设备类型 */
} ec_kind_t;

/* 设备类型(仅 EC_KIND_DEVICE 用) */
typedef enum
{
    EC_DEV_SENSOR = 1,
    EC_DEV_DEVICE = 2
} ec_dev_kind_t;

/* 状态码(仅 EC_KIND_STATUS_CHANGE 用; 与 level_state_t 的语义一致但不依赖它,
 * 免得事件缓存被水位模块的改动牵着走) */
typedef enum
{
    EC_ST_RESOLVED = 1,
    EC_ST_NORMAL   = 2,
    EC_ST_WARNING  = 3,
    EC_ST_ALARM    = 4
} ec_state_t;

/* 触发来源(仅 EC_KIND_ACTION 用) */
typedef enum
{
    EC_TRIG_AUTO     = 1,
    EC_TRIG_MANUAL   = 2,
    EC_TRIG_FAILSAFE = 3
} ec_trigger_t;

/* ★ 设备事件码(仅 EC_KIND_DEVICE 的 a 字段用)★
 * v57 只写了"a = 事件码"却没定义过码值 —— 等于留了个洞: 谁都能存进一个
 * 渲染层不认识的值, 而**要到渲染时才发现**(那时已经在往平台发了)。
 * 这里把码值钉死; 字符串与 cloud_proto.h 的 cloud_device_event_t.event
 * 一一对应(文档 7.4), 渲染层按这张表转成文本。
 * 顺序刻意用显式赋值, 不依赖编译器, 免得以后插值把线上码值改掉。 */
typedef enum
{
    EC_DEV_EV_BOUND       = 1,      /* "bound" */
    EC_DEV_EV_BIND_FAILED = 2,      /* "bind_failed" */
    EC_DEV_EV_UNBOUND     = 3,      /* "unbound" */
    EC_DEV_EV_ONLINE      = 4,      /* "online" */
    EC_DEV_EV_OFFLINE     = 5,      /* "offline" */
    EC_DEV_EV_LOW_VOLTAGE = 6       /* "low_voltage" */
} ec_dev_event_t;

/* 28 字节整(4+4+1+1+1+1+13+2 = 27 -> 对齐到 28; UID 占 13 字节, 见下)
 * ⚠️ 2026-10-28 更正: 原注释按"4+4+1+1+1+3+1 = 15 -> 16"算, UID 已扩到 13 字节。 */
typedef struct
{
    uint32_t ts;            /* 事件【发生】时刻的 Unix 秒(补报不改) */
    /* ★2026-10-08: 由 `level_mm` 改名为 `aux`★
     * 原名叫 level_mm, 但它对 ACTION / DEVICE 两类记录根本没在用 ——
     * 而 ACTION 又要借它存"执行结果码"(见下面两个访问器)。
     * 叫 level_mm 会让人以为"动作事件也有水位", 所以改成中性名 aux(辅助),
     * 并把每种 kind 的含义钉在这里 —— 【不要靠记, 看这张表】:
     *     STATUS_CHANGE : 迁移后的有效水位(mm)   -> 渲染成 event 里的 levelCm
     *     ACTION        : 本台设备的执行结果码    -> results[].code
     *                     0 = 成功; 非 0 = 失败(走标准码表, 见 cloud_proto.h;
     *                     等不到 0x82/0x83 = CLOUD_EC_LORA_TIMEOUT 1005)
     *     DEVICE        : 未使用, 恒 0
     * ⚠️ 读写 ACTION 的那一格【必须】走 ec_action_set_code() / ec_action_code_of()。 */
    int32_t  aux;
    uint8_t  kind;          /* ec_kind_t */
    uint8_t  a;             /* 见上面每种 kind 的说明 */
    uint8_t  b;             /* 见上面每种 kind 的说明 */
    uint8_t  seq;           /* 同一次动作的多条结果共用; 0 = 不分组 */
    uint8_t  uid[13];        /* 关联设备 13 字节 uid; 无关联填 0 */
    uint16_t msg_seq;       /* ★ 第 12 条: 该事件 msgId 的【三位序号部分】 ★
                             * 前缀由 kind 推出(s status_change / v device),
                             * 补报时重建出的 id 与首次分配时逐字节一致。
                             * 0 = 没带(补报回落到旧的 b<自增> 行为, 见文件头说明)。 */
} ec_record_t;

/** 清空队列（恢复出厂 / 换绑）—— 连"待提交槽"一起清 */
void event_cache_reset(void);

/**
 * @brief 主循环 1 s 节拍调一次：把"待提交槽"写进 Flash
 *
 * 【为什么需要它】可合并的记录（低电量 / 离线）先攥在 RAM 里等"同一条再来"，
 *   免得在环里堆出一串重复。但如果它一直不来，这条就永远不落盘 ——
 *   掉电就丢了。所以 1 s 到点强制提交。
 *   不调用它的后果只有一个：掉电可能丢最后那一条持续状态（下次上报会再来）。
 */
void event_cache_poll(void);

/**
 * @brief 存入一条事件记录
 * @return true 已写入 Flash; false 参数非法(kind 不认识) 或 Flash 写失败
 * @note  槽满时**淘汰最旧**并让 event_cache_dropped() +1（可观察，不静默丢）。
 */
bool event_cache_push(const ec_record_t *rec);

/* ==========================================================================
 * ★ 2026-10-28 新增: "同一设备只保留最新一条"的入库方式 ★
 *   (最初只给低电量用; 2026-11-02 起 offline 也走这条)
 * ==========================================================================
 * 【为什么需要 —— 低电量/离线都不是"一次动作", 是"一段持续状态"】
 *   协议 V0.5 定稿: 低电期间**每次 0x02 上报都产生一条** low_voltage
 *   (固件不去抖, 告警收敛归平台); 失联事件也是**每 3×上报周期重复一次**。
 *   在线时没问题, 但断线时就麻烦了:
 *   300s 周期下 1 小时就 12 条、4 小时 48 条, 全都指向同一件事,
 *   却要吃掉环里的一大段槽位, 把真正有价值的 status_change 挤掉。
 *   所以离线时收敛: **同一设备的低电量 / 离线, 只保留最新一条**
 *   (在线时仍逐条上报, 不受影响)。
 *
 * 【怎么合并 —— ★2026-11-11 改成"待提交槽"★】
 *   以前：在 RAM 环里就地覆写成新记录。改成 Flash 单写后行不通了 ——
 *   NOR 只能 1→0，就地改写必须先擦 4 KB，会连带擦掉同扇区另外 63 条还能用的记录。
 *   现在：可合并的记录先攥在 RAM 的"待提交槽"(28 B，只一条)，同一台设备同一种
 *   状态再来就替换它；直到"来了别的记录"或"1 s 到点(event_cache_poll)"才写进环。
 *   效果与就地覆写一致（每条持续状态只占一个槽），而且永不为了改写去擦扇区。
 *   ⚠ offline 那条约束仍然成立: 只有"环里最后一条"才合并 —— 待提交槽按定义
 *     就是最后一条；前面已有 online 说明中间恢复过, 那次掉线是新的, 会追加。
 *
 * 【只对低电量与离线生效 —— 其它事件码一律退化成 event_cache_push()】
 *   bound/unbound 必须成对、online 必须成序, 合并会让平台看不出
 *   "掉线又恢复"。所以合并白名单 = LOW_VOLTAGE + OFFLINE(2026-11-02 G2 加入)。
 *   OFFLINE 额外一条约束: **只有它是环里最后一条时才合并**, 前面已有 online 的话
 *   说明中间恢复过, 那次掉线是新的, 必须追加(否则时间顺序会被就地覆写抹平)。
 * ========================================================================== */

/** event_cache_push_latest() 的结果 —— 让调用方能如实记录"合并"而不是"新增" */
typedef enum
{
    EC_PUSH_FAIL   = 0,     /* rec 为 NULL / kind 不认识 */
    EC_PUSH_ADDED  = 1,     /* 新增一条(槽满时仍按 push 的规则淘汰最旧并计数) */
    EC_PUSH_MERGED = 2      /* 同一设备的同类状态已存在 -> 就地刷新成最新, 条数不变 */
} ec_push_result_t;

/**
 * @brief 存入一条事件; **低电量 / 离线**按"同一设备只留最新一条"合并
 * @return EC_PUSH_ADDED / EC_PUSH_MERGED / EC_PUSH_FAIL
 * @note  不在白名单里的事件完全等价于 event_cache_push(), 返回值只会是 ADDED / FAIL。
 * @note  合并**不会**触发槽满淘汰(不占新槽), event_cache_dropped() 不变。
 */
ec_push_result_t event_cache_push_latest(const ec_record_t *rec);

/** 只取最旧一条, 【不出队】(与 data_cache 的 peek/drop 同一套用法) */
bool event_cache_peek(ec_record_t *out);

/** 丢掉最旧一条(补报【成功】之后才调) */
void event_cache_drop(void);

/** 取并出队一条 */
bool event_cache_pop(ec_record_t *out);

/** 当前条数(含"待提交槽"那一条; 0 = 没有待补报的) */
uint16_t event_cache_count(void);

/** 因槽满淘汰 / 写失败丢掉的总条数(累计, 仅 reset 清零) —— 非 0 说明断线太久 */
uint32_t event_cache_dropped(void);

/** 容量(= EVENT 环的槽数; 外挂 Flash 不可用时为 0) */
uint16_t event_cache_capacity(void);

/**
 * @brief 本页最多能安全取走多少条记录（★ 不把同一次动作的 seq 组切两半 ★）
 * @param max_events 本页条数上限（0 -> 返回 0）
 * @return 可安全取走的条数；0 = 缓存空
 *
 * 【为什么需要它 —— 别自己用 max_events 直接取】
 *   `event.action` 的 `results[]`（多台设备的执行结果）是靠 `seq` 把**连续几条记录**
 *   归成一组后再渲染的。若一页恰好从组中间切断，渲染出来的 `results[]` 就是**残缺的**，
 *   而且现象很隐蔽：平台收到的是一条"少报了几台设备"的动作事件，看起来完全正常。
 *   所以分页边界必须落在组边界上。
 *
 * 【两种越界，刻意选后者】
 *   · 若队头那一组本身就比 `max_events` 大（比如一次动作涉及 8 台设备而一页只想要 5 条），
 *     本函数**整组返回**（返回值 > max_events）。
 *     理由：宁可这一页超限，也不能让缓存永远取不走 —— 卡住 = 补报永远发不完 = 事件越积越多。
 *   · 只有"不足一页"时（条数 <= max_events）直接返回全部条数。
 *
 * 【调用方契约】
 *   渲染并发布**成功**之后，必须 `event_cache_drop()` **恰好返回值这么多次**；
 *   失败则一次都不要 drop（与 data_cache 的 peek/drop 纪律一致）。
 */
uint16_t event_cache_page_count(uint16_t max_events);

/**
 * @brief 这条记录"能不能被渲染"？（a/b 的取值是否落在已定义的码值空间内）
 * @return true 合法; false 非法(NULL / kind 不认识 / a 或 b 越界)
 *
 * 【它和 event_cache_push() 的分工 —— 刻意的】
 *   `push()` 只管"kind 认不认识"(存储原语, 保持最小), 而 `a`/`b` 的合法性
 *   由**产生端**在入库前用本函数检查。
 *   这样做的理由: 一旦把校验塞进 push, 所有既有测试与将来的接线都得跟着改;
 *   更重要的是, 校验点应落在**记录被构造出来的地方**(那时还知道上下文,
 *   能顺手记日志说明为什么丢), 而不是在存储层"静默拒收"。
 *   渲染层在补报前也该再查一次(缓存可能是上一版固件留下的),
 *   对非法记录的处理约定是: **跳过 + 计数, 不静默发出去**。
 */
bool event_cache_validate(const ec_record_t *rec);

/* ==========================================================================
 * ★ EC_KIND_ACTION 的字段复用（v64 明确下来，别让接线时再踩）★
 * ==========================================================================
 * 一次联动动作会产生【每台设备一条记录】:
 *     uid   = 该设备(13 字节)      a = 档位 gear        b = 触发来源 trigger
 *     seq   = 同一次动作共用      ts = 动作发生时刻
 * 但 results[] 需要的是 (uid, **执行码**) 成对 —— 而 16 B 的记录里
 * **没有给执行码留独立字段**。对动作事件而言 `level_mm` 本来就是空的
 * (动作没有水位), 所以这里**显式约定**: 动作记录的 `level_mm` 装的就是
 * 该设备的执行码(0 = 成功, 非 0 见错误码表)。
 *
 * 为什么不干脆加一个字段: 记录会从 28 B 涨到 32 B(对齐), 256 条就从 7 KB 变 8 KB;
 * 而 `level_mm` 在这里确实是浪费的 4 字节。**复用比加字段省 2 KB, 代价是必须
 * 走下面两个访问器** —— 所以请一律用它们读写, 不要直接碰 `level_mm`,
 * 否则"这到底是水位还是执行码"就会变成靠人记的隐性知识。
 * ========================================================================== */

/** 取动作记录里承载的执行码(等价于 rec->aux, 但意图明确) */
int32_t ec_action_code_of(const ec_record_t *rec);

/** 把执行码写进动作记录(等价于 rec->aux = code) */
void ec_action_set_code(ec_record_t *rec, int32_t code);

/** 一台设备的动作执行结果(渲染 event.action 的 results[] 时用) */
typedef struct
{
    uint8_t uid[13];
    int32_t code;
} ec_action_pair_t;

/**
 * @brief 把【队头那一组】动作记录收成 (uid, 执行码) 对
 * @param out        输出数组
 * @param max_pairs  out 能装几对(调用方应 >= 一次动作可能涉及的最大设备数)
 * @return 收进来的条数(= 本组大小); **0 = 队头不是成组的动作记录, 或本组装不下**
 *
 * 【为什么单独一个函数】
 *   `event.action` 的 `results[]` 必须是**完整的一组**: 少报一台设备的事件
 *   看起来完全正常, 现场根本查不出来。分组规则(同 kind + 同 seq + 连续)放在这里
 *   一次测透, 调用方就只剩"取一次 -> 渲染 -> 发布", 没有第二次做错的机会。
 *
 * 【★ 装不下就返回 0, 绝不只报前几台 ★】
 *   若本组条数 > max_pairs, 返回 0 而不是截断 —— 与 render_cached_event 的
 *   "宁可这条不补报, 也不发半条"是同一条原则。
 *   `seq == 0` 的动作记录**不成组**(避免把不相关的事件粘在一起), 返回 0。
 *
 * 【调用方契约】本函数**只看不出队**; 只有补报**成功**之后才按返回值调用
 * `event_cache_drop()` 相同次数。
 */
uint8_t event_cache_peek_action_group(ec_action_pair_t *out, uint8_t max_pairs);

/**
 * @brief 取环形里第 idx 条(0 = 最旧)的副本, **不出队**
 * @return true 取到; false idx 越界或 out 为空
 *
 * 【为什么组装器需要它】
 *   `event_cache_peek()` 只看**队头**。而补报一页要逐条渲染页内的每一条记录
 *   (从第 0 条到第 n-1 条), 又**不能中途出队** —— 只有整页发布成功之后才允许
 *   drop。所以需要一个"按索引看"的接口。
 */
bool event_cache_peek_at(uint16_t idx, ec_record_t *out);

/**
 * @brief 从第 idx 条开始, 把同一 seq 的动作记录收成 (uid, 执行码) 对
 * @return 收进来的条数; **0 = 第 idx 条不是成组的动作记录, 或本组装不下**
 *
 * 【与 event_cache_peek_action_group() 的区别】
 *   后者只从**队头**开始找。但一页里的动作组**不一定在队头** ——
 *   组边界只保证"不跨页", 不保证"在页首"。所以组装器必须用本函数按索引找。
 *   两者共用同一套规则(同 kind + 同 seq + 连续; 装不下返回 0 而不截断)。
 */
uint8_t event_cache_group_at(uint16_t idx, ec_action_pair_t *out, uint8_t max_pairs);

#endif /* __EVENT_CACHE_H__ */
