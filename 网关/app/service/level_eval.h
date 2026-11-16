/**
 * @file  level_eval.h
 * @brief 边缘水位评估的【纯逻辑】: 阈值判状态 + confirmCount 防抖 + 有效水位 + 事件去抖
 *
 * ---------------------------------------------------------------------------
 * 【为什么单独一个模块, 而且一行 LoRa 都不碰】
 *   文档 4 章把边缘水位评估定为"自治核心", 但它其实只做三件事: 比较、计数、状态迁移。
 *   真正的输入(每台传感器最近水位)来自 LoRa 上报路径 —— 而这部分正是现场正在排障的
 *   地方。如果算法和取数揉在一起, 就只能等 LoRa 通了才能动手。
 *   所以这里把算法独立出来: 水位由调用方(以后是 LoRa 上报处理)以【数组】传进来,
 *   于是它能像 sensor_offline.c 那样在主机上跑完整单测。
 *
 * 【单位约定: 一律用 mm(整数)】
 *   配置里的阈值是 cm(alarmCm/warningCm/resolveCm/minChangeCm), 而文档的 levelCm
 *   是一位小数(例 12.3)。本模块内部统一按【mm】比较(比较时把 cm*10),
 *   这样既不丢 0.1cm 的分辨率, 也不用浮点。
 *
 * 【状态边界严格照文档 3.4 的表, 不留解释空间】
 *   Alarm    level >= alarmCm                 -> 档位 3
 *   Warning  warningCm < level <  alarmCm     -> 档位 2
 *   Normal   resolveCm < level <= warningCm   -> 档位 1
 *   Resolved level <= resolveCm               -> 档位 0
 *   (注意两处等号: level == warningCm 归 Normal; level == resolveCm 归 Resolved)
 * ---------------------------------------------------------------------------
 */
#ifndef __LEVEL_EVAL_H__
#define __LEVEL_EVAL_H__

#include <stdint.h>
#include <stdbool.h>
#include "config_store.h"       /* 只用 cfg_thresholds_t 这个类型, 不调它的函数 */

#ifdef __cplusplus
extern "C" {
#endif

/* 文档 3.4 的四个状态 + 一个"还没测到过任何水位"的初值 */
typedef enum
{
    LEVEL_ST_UNKNOWN  = 0,      /* 上电至今没有过有效水位 */
    LEVEL_ST_RESOLVED = 1,      /* <= resolveCm  -> 档位 0 */
    LEVEL_ST_NORMAL   = 2,      /* (resolve, warning] -> 档位 1 */
    LEVEL_ST_WARNING  = 3,      /* (warning, alarm)   -> 档位 2 */
    LEVEL_ST_ALARM    = 4       /* >= alarmCm          -> 档位 3 */
} level_state_t;

/** 文档 3.4 的状态 -> 档位映射(固定, 不可配) */
uint8_t level_state_to_gear(level_state_t st);

/**
 * @brief 把传感器上报值换算成【水深(mm)】—— 纯函数, ⚠️ 当前固件【不调用】⚠️
 *
 * @param measure_mode      cfg_measure_mode_t; 非 distance 一律按 level 处理(兜底不猜)
 * @param install_height_cm 探头安装高度(cm), 仅 distance 模式用
 * @param calib_offset_mm   校准偏移(mm), 加在换算结果上
 * @param raw_mm            传感器上报的原始值(mm)
 * @return 水深(mm)。**distance 模式可为负, 不钳位**
 *
 * @warning ★2026-10-30 起网关【不再做换算】★
 *   液位传感器固件自己实现了 measureMode / installHeightCm / calibOffsetMm
 *   (0x01 帧 [6] / [7-8] / [9-10]), 并在设备内部换算, 0x02 帧报上来的已经
 *   是液位值。网关若再算一次 = 重复扣减安装高度, distance 模式下水位系统性偏低。
 *   所以 `data_cache_up_push()` 已改成原值透传, 本函数【没有任何调用点】。
 *   保留它只为: ① 单测仍覆盖换算边界(负值/校准/未配置); ② 万一将来要接
 *   不做换算的第三方传感器, 逻辑不用重写。**新增代码不要再调它。**
 *
 * @note 【口径】level 模式原样返回 —— 传感器报的就是水深, 网关不做二次加工。
 */
int32_t level_convert_mm(uint8_t measure_mode, uint16_t install_height_cm,
                         int16_t calib_offset_mm, uint16_t raw_mm);

/**
 * @brief 水位(mm) -> 候选状态(文档 3.4 的表)
 * @note  t == NULL 时返回 LEVEL_ST_UNKNOWN(不猜)
 */
level_state_t level_state_of(int32_t level_mm, const cfg_thresholds_t *t);

/**
 * @brief 有效水位 = 所有"在线且 enabled"的传感器里取最大值(文档 3.3 末)
 * @param levels_mm 每台的水位(mm)
 * @param select    每台是否参与(1 = 在线且 enabled) —— 【把"在线/enabled"的判断
 *                  留给调用方】, 因为在线状态在 LoRa 层、enabled 在配置里,
 *                  本模块不该去碰它们
 * @param n         台数
 * @param out_mm    输出: 有效水位(mm)
 * @return true = 至少有一台参与(此时有效水位才有意义); false = 一台都没有
 * @note  文档 5 章"传感器全部失联"的 fail-safe 由调用方按 rules.sensorOfflinePolicy
 *        处理, 本函数只回答"有没有有效水位"。
 */
bool level_effective(const int32_t *levels_mm, const uint8_t *select,
                     uint8_t n, int32_t *out_mm);

/* ---------------- confirmCount 防抖(文档 3.4 的 confirmCount 字段) ---------------- */

typedef struct
{
    level_state_t confirmed;    /* 已确认的状态 */
    level_state_t candidate;    /* 正在累计的候选状态 */
    uint8_t       confirm_cnt;  /* 候选连续出现了几次 */
    bool          have;         /* 是否已经确认过状态(上电初次不算"迁移") */
} level_eval_t;

void level_eval_init(level_eval_t *e);

/**
 * @brief 喂一次有效水位
 * @param t           阈值(其中的 confirm_count 决定要连续几次)
 * @param level_mm    本次有效水位(mm)
 * @param out_state   输出: 本次【确认迁移】后的状态(只在返回 true 时有意义)
 * @return true = 状态发生了【已确认的迁移】(调用方才该去下发联动/上报事件);
 *         false = 没到迁移条件(候选还没攒够次数, 或状态没变)
 * @note  confirmCount == 1(出厂默认) 时等价于"立刻迁移"。
 *        上电后第一次确认【也算迁移】(have 由 false 变 true) —— 因为网关需要按
 *        当前水位把档位摆到正确位置, 文档 5 章"网关上电: 取最近已知水位恢复当前档位"。
 */
bool level_eval_feed(level_eval_t *e, const cfg_thresholds_t *t,
                     int32_t level_mm, level_state_t *out_state);

/* ---------------- 事件去抖(minChangeCm + eventMinIntervalSeconds) ---------------- */

typedef struct
{
    int32_t  last_mm;           /* 上次生成事件时的水位 */
    uint32_t last_ms;           /* 上次生成事件的毫秒时基 */
    bool     have;              /* 还没生成过事件 */
} level_event_gate_t;

void level_event_gate_init(level_event_gate_t *g);

/**
 * @brief 该不该生成一条水位类事件?(文档 3.4: minChangeCm 与 eventMinIntervalSeconds)
 * @note  两条都要满足: 水位相对上次事件的变化量 >= minChangeCm, 且距上次事件
 *        至少 eventMinIntervalSeconds 秒。首次(have==false)恒为 true。
 *        时间比较用无符号差值, 毫秒回绕不会判错。
 */
bool level_event_should_fire(const level_event_gate_t *g, const cfg_thresholds_t *t,
                             int32_t level_mm, uint32_t now_ms);

/** 真的生成了一条事件之后调用, 记下基准 */
void level_event_mark(level_event_gate_t *g, int32_t level_mm, uint32_t now_ms);

/* ---------------- {level} 模板渲染(文档 3.5) ---------------- */

/**
 * @brief 把档位模板里的 {level} 替换成当前水位, 得到实际要下发/显示的文本
 * @param tmpl    档位的 ledTextTemplate / ttsTextTemplate
 * @param level_mm 当前有效水位(mm)
 * @return 写入长度; 0 失败(缓冲不够 / 参数错)
 *
 * @note  ① 文档 3.5 说 {level} 替换为【当前水位 cm 整数】—— 这里按 mm/10 取整,
 *         且是【向下取整】(floor), 例 123mm -> "12"、-5mm -> "-1"。
 *         ★2026-10-28(A1): distance 模式的换算结果【可以是负数】, 那时"截断"
 *           会把 -1~-9mm 显示成 0, 比实际高一档; 所以统一按 floor 处理。
 *        ② 模板里可以出现多次 {level}, 全部替换。
 *        ③ 不认识的花括号(如 "{x}")【原样保留】, 不报错也不吞掉 —— 免得把
 *          用户写的文本悄悄改掉。
 *        ④ 缓冲不够时返回 0 并把 out 置空(绝不发半截文本给 LED)。
 */
uint16_t level_render_template(char *out, uint16_t cap, const char *tmpl, int32_t level_mm);

/* ---------------- "这次要不要下发"的判据(文档 4 章末) ---------------- */

/*
 * 文档 4 章明确了两条防重发规则:
 *   · "档位未变化时不重复下发联动动作"(对齐 Worker 的 _lastLedStates 防重发机制);
 *   · "水位在档位内变化时, LED 文本中的 {level} 按需刷新(文本实际变化才下发)"。
 * 也就是: 档位变了 -> 一定发; 档位没变但渲染后的文本变了 -> 也发; 否则不发。
 * 失败重试的约定: ★ 下发【失败】时不要调 level_dispatch_mark() ★ ——
 *   这样缓存里仍是"上次成功发出去的那份", 下一页自然会重发(文档: 下发失败则清除缓存下次重试)。
 */
typedef struct
{
    bool    have;                            /* 有没有成功下发过 */
    uint8_t gear;
    /* ★2026-10-08: 单段 -> 两段★ 只记"上次【成功下发】的文本", 两段都要比 */
    char    led_text1[CFG_LEDTEXT_MAX + 1u];
    char    led_text2[CFG_LEDTEXT_MAX + 1u];
} level_dispatch_state_t;

void level_dispatch_init(level_dispatch_state_t *d);

/** true = 这次该下发(档位变了 / 任一段文本变了 / 从没成功发过) */
bool level_dispatch_should_send(const level_dispatch_state_t *d, uint8_t gear,
                                const char *led_text1, const char *led_text2);

/** 下发【成功】之后调用, 把这份记为已发 */
void level_dispatch_mark(level_dispatch_state_t *d, uint8_t gear,
                         const char *led_text1, const char *led_text2);

/* ---------------- 档位 -> LoRa 动作载荷(自治"最后一跳"的组帧, 2026-09-30 第 2 条) ----------
 *
 * 【为什么放在这个模块】
 *   本模块已经负责"档位 -> 动作内容"(状态->档位映射 + {level} 模板渲染)。把"动作内容 ->
 *   线上字节"也放这里, 整条链就是纯逻辑, 可以在主机上跑完整单测 —— 而 lora_business.c
 *   依赖射频, 主机上链不进来。
 *
 * 【★ payload 布局(以 level_gear_build_payload() 实际实现为准) ★】
 *   固定头 5 字节 + 三段「1 字节长度 + 内容」:
 *       [0] 总电源(1 开屏 / 0 关屏)
 *           ★2026-11-02: 不再是"档位 0 一律关"★ 现在按这一档有没有 LED 内容决定:
 *           配了文本 / 非 off 的颜色 / 要闪灯 => 1; 三样全空(出厂"解除"档) => 0。
 *           语音(TTS)与屏电源无关, 只配 TTS 的档位不开屏。
 *       [1] 报警灯光模式(0关/1常亮/2慢闪/3快闪, 只取 gears[].soundMode)
 *       [2] 音色 voice(gears[].voice, 0~5; ★2026-11-11 接入空口★)
 *           0 艾佳 / 1 艾诚 / 2 艾达 / 3 艾琪 / 4 唐老鸭 / 5 艾彤
 *       [3] 功放音量(devices[].volume, 0~100)
 *       [4] LED 屏模式(位定义见 level_eval.c: bit7 竖/横, bit5/4 两半屏对齐,
 *           bit3~0 两半屏颜色)
 *       [5]        N = 文本1长度        (0 = 清屏不显示)
 *       [6 ..]     文本1 (N 字节 UTF-8)
 *       [6+N]      M = 文本2长度
 *       [7+N ..]   文本2 (M 字节 UTF-8)
 *       [7+N+M]    K = TTS 长度         (0 = 不播报)
 *       [8+N+M ..] TTS (K 字节 UTF-8)
 *   总长 = 5 + (1+N) + (1+M) + (1+K) = 8 + N + M + K
 *   ⚠ 2026-10-28 更正: 这里曾残留"9 字节头(0 总电源…8 LED文本长度)"的旧协议描述,
 *     那是 main.c 早期 rawtest 探针的格式, 现行实现是定长头 + 三段长度前置,
 *     不要按 9 字节头去对齐设备偏移表。
 *   ⚠ 2026-11-11: 定长头由 4 字节扩到 5 字节(新增 [2] 音色)。此前 voice 只存在
 *     配置结构里、没进载荷, 平台"改音色成功"实际不改变设备播报。设备侧偏移表
 *     对应关系: 新 [2]=音色, 原 [2] 音量 -> [3], 原 [3] LED屏模式 -> [4],
 *     原 [4] 文本1长度 -> [5], 其后整体 +1。
 *   ⚠ 设备侧那张偏移表把"文本2"起点写成 【8+N】、TTS 长度写成 【8+N+M】、
 *     TTS 写成 【9+N+M】, 比本实现整体多 1 —— 这是【老笔误】(2026-10-08 已确认):
 *     它同时标注"6+N~7+N 占 1 字节", 1 个字节不可能跨两个偏移。正确是
 *     文本2 起点 7+N、TTS 长度 7+N+M、TTS 起点 8+N+M, 否则长度字节和文本之间
 *     会凭空多一个未定义字节, 设备会读到一段垃圾文本。
 *
 * 【文本编码】按用户口径: 一个汉字 3 字节、ASCII 1 字节、原样搬运 UTF-8
 *   (用 utils 的 utf8_char_len 逐字判定)。放行时不转 GB2312(2026-10-28 裁决:
 *   代码正确, 不做转码)。放不下时【在字符边界截断】——
 *   绝不允许出现半个汉字(设备侧乱码, 比少几个字难查得多)。
 * ------------------------------------------------------------------------- */

/** 0x03 载荷上限 206 字节 = 空口单帧 data 上限 207 - func 字段 1 字节。
 *  (下发缓存 data_cache 的 payload[211] 比它多 4 字节余量, 不构成约束) */
#define LEVEL_GEAR_PAYLOAD_MAX   206U
/** 固定头 5 字节([0]总电源 [1]灯光模式 [2]音量 [3]音色 [4]LED屏模式)。
 *  ★★注意是 5, 不是 4 也不是 6★★ 《声光报警器对接》的偏移表是:
 *      [5] = 文本1长度 —— 也就是【第一个长度字节在固定头之外】。
 *    于是总长 = 5 + 1(N) + N + 1(M) + M + 1(K) + K = 8 + N+M+K。
 *  ⚠️ 2026-11-11 由 4 改 5: 在 [2] 音量之后插入 [3] 音色 voice。
 *    设备侧偏移表须同步: 原 [3] LED屏模式 -> [4], 原 [4] 文本1长度 -> [5], 其后 +1。
 *  ⚠️ 2026-10-08 这里一度写成 5(当时把 N 也算进固定头), 会让整帧【从头错 1 字节】——
 *    是 test_level 的用例把它抓出来的。下面的 #error 就是为了钉死这个关系。 */
#define LEVEL_GEAR_HDR_LEN        5U

/* ★2026-10-28 定案: 三段文本【各自独立】限长, 取消合计预算★
 *     文本1 ≤ CFG_LEDTEXT_MAX(48) / 文本2 ≤ CFG_LEDTEXT_MAX(48) / TTS ≤ CFG_TTSTEXT_MAX(88)
 * 最坏总长 = 5 + 3 + 48 + 48 + 88 = 192 ≤ LEVEL_GEAR_PAYLOAD_MAX(206), 装得下。
 * 下面的 #error 把"三段用满仍不超上限"钉在编译期: 以后谁调大单段上限,
 * 一旦越界直接编不过, 而不是到现场才发现尾部 TTS 被截掉。 */
#if ((LEVEL_GEAR_HDR_LEN + 3u + CFG_LEDTEXT_MAX + CFG_LEDTEXT_MAX + CFG_TTSTEXT_MAX) > \
     LEVEL_GEAR_PAYLOAD_MAX)
#error "三段文本用满时(48+48+88)超出 0x03 载荷上限 206"
#endif

/**
 * @brief 按档位定义组一帧 0x03 载荷(纯函数, 无副作用)
 * @param g         档位定义(不可 NULL)
 * @param volume    该联动设备的音量(devices[].volume)
 * @param led_text1 已渲染的【第一个半屏】文本(竖屏=左 / 横屏=上; 可 NULL/空)
 * @param led_text2 已渲染的【第二个半屏】文本(竖屏=右 / 横屏=下; 可 NULL/空)
 *                  ★任一段为空 => 设备按全屏显示★(文档 §1.9, 设备从长度=0 推断)
 * @param tts_text  已渲染的语音文本(可 NULL/空; **空 = 不播报**, 与 soundMode 无关)
 * @param out       输出缓冲
 * @param cap       缓冲容量(>LEVEL_GEAR_PAYLOAD_MAX 时按上限截)
 * @return 载荷字节数; 0 = 参数错
 */
uint8_t level_gear_build_payload(const cfg_gear_t *g, uint8_t volume,
                                 const char *led_text1, const char *led_text2,
                                 const char *tts_text,
                                 uint8_t *out, uint16_t cap);

/**
 * @brief 选出本档位要下发的联动设备下标(启用的 + 命中 targets 的)
 * @param cfg 全量配置(不可 NULL)
 * @param gear 档位 0~3
 * @param only_uid 只发这一台(26 位 hex 串, 13 字节 UID); **NULL = 按 gears[gear].targets 选**
 *                 —— dev.ctrl(手动控制) 用前者, 自动联动用后者
 * @param out_idx 输出: devices[] 下标数组
 * @param cap     out_idx 容量
 * @return 选中的台数(可能 > cap —— 此时只写了前 cap 个, 调用方应察觉)
 * @note  a) only_uid == NULL 时: targets_all=1 -> 所有 enabled 的联动设备;
 *           否则按 target_uid[] 逐个比对。
 *        b) only_uid != NULL 时: 只看这一台, 且它必须 enabled(停用的不接动作)。
 *        c) 只做纯比较, 不调 config_store 的任何函数(保持主机可测)。
 */
uint8_t level_gear_collect_targets(const config_store_t *cfg, uint8_t gear,
                                   const char *only_uid,
                                   uint8_t *out_idx, uint8_t cap);

/**
 * @brief 传感器【全部失联】时的 fail-safe 档位(文档 3.7 的 sensorOfflinePolicy)
 * @param policy   cfg_offline_policy_t: 0=keep / 1=alarm / 2=off
 * @param cur_gear 当前档位(keep 时原样返回)
 * @return 应当执行的档位 0~3
 * @note  文档 3.7 原文:
 *          keep  保持当前档位
 *          alarm 强制升到档位 3
 *          off   执行档位 0
 *        越界的 policy 按 keep 处理(不猜、不误动作)。
 * @note  "全部失联"这件事由调用方判定(它才知道谁在线), 本函数只做映射。
 */
uint8_t level_failsafe_gear(uint8_t policy, uint8_t cur_gear);

#ifdef __cplusplus
}
#endif

#endif /* __LEVEL_EVAL_H__ */
