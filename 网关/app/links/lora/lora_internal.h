/**
 * @file  lora_internal.h
 * @brief links/lora 内部接口（lora_business.c 与拆出来的状态机文件之间）
 *
 * 只允许 app/links/lora/*.c include；对外 API 一律在 lora_business.h。
 */
#ifndef LORA_INTERNAL_H
#define LORA_INTERNAL_H

#include <stdint.h>
#include "lora_business.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 本网关的 13 字节地址（lora_business_init 时从 param_save 读出） */
extern uint8_t g_gw_uid[LORA_UID_BYTES];

/* ==================== 从机表（lora_slave.c） ====================
 * 表是动态的：init 时从 config_store 装载，运行期由绑定/解绑增删。
 * 除只读遍历（get_slave_list）外，请不要直接改 g_slave_list / g_slave_count。 */
extern lora_slave_t g_slave_list[];
extern uint8_t      g_slave_count;

/** 按 13 字节地址查索引；0xFF = 不在表里 */
uint8_t lora_slave_find(const uint8_t uid[LORA_UID_BYTES]);

/** 收到帧时"摸一下"：标记在线 + 记 last_recv_tick；0xFF = 不在表里 */
uint8_t lora_slave_touch(const uint8_t uid[LORA_UID_BYTES]);

/** 往表尾追加一台（毕设版固定为液位传感器）；返回索引，-1 = 表满/已在表里 */
int lora_slave_add(const uint8_t uid[LORA_UID_BYTES]);

/** 删除指定地址（末尾元素前移补洞）；0 = 成功，-1 = 不在表里 */
int lora_slave_del(const uint8_t uid[LORA_UID_BYTES]);

/** 从 config_store 重新装载整张表（init 时调一次） */
void lora_slave_load(void);

/** 设备地址首位 -> 类型（1 网关 / 2 液位），现场规则 */
uint8_t lora_dev_type_of_uid(const uint8_t uid[LORA_UID_BYTES]);

/** ASCII hex 文本 -> 二进制（旧格式下行的兼容分支用）；-1 = 非法 */
int lora_hex_str2bin(const char *hex, int hex_len, uint8_t *out, int out_max);

/* ==================== 下发队列（lora_downlink.c） ====================
 * 投递在 data_cache，真正的空口发送 / ACK 重传 / 应答时序都在这个文件里。 */
/** 把某台从机的待发指令发出去（组帧 + 发送）；true = 已发出 */
bool lora_tx_downlink(uint8_t idx);

/** 每秒一次：ACK 超时重传 */
void lora_downlink_logic(void);

/** 收到请求帧后"先回 0x80、再等 3s 下发"的两步时序 */
void lora_reply_schedule(const uint8_t dev_uid[LORA_UID_BYTES], uint8_t func, uint8_t idx);
void lora_reply_logic(void);

/** 4G/MQTT 下行回调（注册给 g4_urc；唯一的下行入口） */
void lora_mqtt_recv_cb(const char *topic, const uint8_t *data, uint16_t len);

/* ==================== 绑定 0x04（lora_bind.c） ====================
 * 窗口只在云端 sen.bind 时打开（平时收到 0x04 一律丢弃）。 */
/** 窗口是否开着（任意外部状态），on_frame 据此决定要不要把 0x04 交给握手 */
bool lora_bind_accepts_0x04(void);
/** 是否正在等这台设备回 0x80 确认 */
bool lora_bind_waiting_dev_ack(const uint8_t uid[LORA_UID_BYTES]);

void lora_bind_handle_0x04(const lora_frame_t *frame);
void lora_bind_handle_dev_ack(const lora_frame_t *frame);
void lora_bind_logic(void);

/** 上报一次绑定/解绑事件（绑定与解绑共用这个出口） */
void lora_bind_fire_event(const char *uid_hex, lora_bind_event_t event);

/* ==================== 解绑 0x05（lora_unbind.c） ====================
 * 两阶段状态机；0x05 不进 data_cache，解绑期间目标从机被当作"锁"。 */
/** 这条 ack 是否属于正在进行的解绑（是则交给 lora_unbind_on_ack） */
bool lora_unbind_owns_ack(uint8_t slave_idx);
void lora_unbind_on_ack(void);
void lora_unbind_logic(void);

/**
 * @brief 液位解绑的"窗口机会"：目标设备刚上报完、指令窗口已打开
 *
 * 由 lora_reply_logic() 在"回完 0x80 再等 3s"那一步调用 —— 只有正在解绑
 * 该设备时才返回 true，并当场把 0x05 发出去（复用窗口，不再盲发）。
 */
bool lora_unbind_window_hit(uint8_t slave_idx);

/**
 * @brief 解绑中的设备发来 0x04(gw=广播) —— 它已清网关值, 立即判解绑成功
 *
 * 只认 WAIT_CLEAR / PHASE2 两个阶段(阶段1 还没确认它收到过 0x05)。
 * 由 lora_business.c 的 on_frame 在"0x04 + gw 全 FF + 目标是解绑对象"时调用。
 */
void lora_unbind_dev_cleared(void);

/** 0x08 出厂字段应答（on_frame 里 0x08 分支调用） */
void lora_field_handle_resp(const lora_frame_t *frame, uint8_t idx);

/** 写字段期间设备回的 0x80/0x81；true = 已被字段状态机消费 */
bool lora_field_on_ack(const lora_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* LORA_INTERNAL_H */
