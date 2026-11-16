/**
 * \file  xf_log.h
 * \brief 关键日志落盘（LOG 环）+ 掉电可查
 *
 * 【为什么要这个】
 *   日志口(USART1)只在现场有串口线时才看得见。设备装到井下/野外后出问题，
 *   重启一次 RAM 里的东西全没了，事后无法追溯"到底什么时候断的、断之前发生了什么"。
 *   LOG 环把关键日志按行写进外挂 Flash，重启后还能 `#flash log dump` 读出来。
 *
 * 【挂载点】log.c 的 fputc() 逐字符喂进来，遇 '\n' 提交一行。
 *   好处：300+ 处 log_info() 调用一个都不用改。
 *   代价：fputc 里只做"攒行 + 入 RAM 队列"，绝不直接写 Flash（写一次要几毫秒，
 *         扇区擦除 45 ms，绝不能卡在日志输出中间）。
 *
 * 【与 xf_ring_push 的递归风险 ★必读★】
 *   xf_ring_push() 内部失败时会调用 log_info() 打印原因，而 log_info 又走 fputc
 *   → 又喂进本模块 → 又去 push → 无限递归。
 *   本模块用 s_writing 标志在"正在写 Flash"期间屏蔽入队，把这个环切断。
 *
 * 【记录格式】52 字节，正好等于 XF_REC_MAX_PAYLOAD（64 B 槽 - 12 B 头）。
 *   文本上限 40 字节(UTF-8)，超长在**字符边界**截断并置 flags.bit0。
 */
#ifndef __XF_LOG_H__
#define __XF_LOG_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 单条日志文本上限（字节）。超长在 UTF-8 字符边界截断 */
#define XF_LOG_TEXT_MAX     40u

/** 落盘记录（必须正好 52 字节，见 xf_log.c 的编译期断言） */
typedef struct
{
    uint32_t ts;        /**< 当时 UTC 秒；0 = 时间未同步 */
    uint32_t tick_ms;   /**< 开机毫秒；ts==0 时用它定位 */
    uint8_t  len;       /**< text 有效字节数 */
    uint8_t  flags;     /**< bit0 = 文本被截断 */
    uint8_t  rsv[2];    /**< 对齐保留，写 0 */
    char     text[XF_LOG_TEXT_MAX];
} xf_log_rec_t;

/** 开机调用一次；Flash 不可用时自动降级为 no-op */
void xf_log_init(void);

/**
 * @brief 注入时间源（返回当前 UTC 秒）。未注入时 ts 记 0，只留 tick_ms。
 * @note  用回调而不是直接 include app_state.h，是为了让本模块与 core 层解耦，
 *        主机单测也能塞一个假时钟进来。一般传 app_now_unix。
 */
void xf_log_set_clock(uint32_t (*fn)(void));

/** log.c 的 fputc() 逐字符喂入 */
void xf_log_feed_char(char c);

/** 主循环调用：把 RAM 队列里的行写进 LOG 环（每次最多写 2 条，避免长阻塞） */
void xf_log_poll(void);

/** 还没写进 Flash 的行数 */
uint32_t xf_log_pending(void);

/** LOG 环里现有的条数 */
uint32_t xf_log_count(void);

/** 因超速/队列满/写失败被丢弃的累计条数 */
uint32_t xf_log_dropped(void);

/**
 * @brief 打印最新 n 条（n=0 表示全部，最多 XF_LOG_MAX_DUMP 条）
 * @note  从新到旧打印，每条带序号/时间/文本。
 */
#define XF_LOG_MAX_DUMP     128u
void xf_log_dump(uint32_t n);

/** 清空 LOG 环（不动 EVENT/TELEM） */
bool xf_log_clear(void);

/** 开关落盘（默认开）。关掉时 fputc 的喂入直接丢弃，零开销 */
void xf_log_set_enable(bool on);
bool xf_log_enabled(void);

/**
 * @brief 切换"直接写盘"模式（启动阶段专用）
 *
 * @param on true  = 每行立刻写 Flash（阻塞 0.7~45 ms），不经队列；
 *           false = 进 RAM 队列，由主循环 xf_log_poll() 消费（稳态用）。
 *
 * 【为什么需要它】启动阶段主循环还没跑，xf_log_poll() 一次都不会被调用，
 *   而 [BOOT]/[XFLASH]/[G4]/[LORA] 恰恰在几毫秒内打上百行 —— 队列再大也会被
 *   挤爆（实测丢 200+ 条）。启动时没有实时性要求，多花一两百毫秒换来完整的
 *   启动日志是划算的。`xf_log_init()` 默认置 true，进主循环前置 false。
 *
 * 正常业务代码永远不需要调用它。
 */
void xf_log_set_direct(bool on);

/**
 * @brief 暂停 / 恢复捕获
 *
 * @note 【用途】周期性快照日志不该进取证环。
 *   `[STAT]` / `[STAT2]` 每秒各一条(= 17 万条/天)，会把 LOG 环彻底淹没，
 *   真正有价值的"断网/重启/告警"全被挤出去 —— 现场实测：环里 110 条全是
 *   [STAT]，一条有效事件都没留下。所以 app_status_report() 首尾包一层。
 *
 *   pause 时会把当前行缓冲清空（说明这半行也不要了），队列里已有的不受影响。
 *   与 xf_log_set_enable(false) 的区别：那个是彻底关掉落盘（人类主动关），
 *   这个是代码内部临时屏蔽，语义不同所以单独给接口。
 */
void xf_log_pause_capture(void);
void xf_log_resume_capture(void);

#ifdef __cplusplus
}
#endif

#endif /* __XF_LOG_H__ */
