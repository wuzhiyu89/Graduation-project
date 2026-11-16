/* ============================================================================
 * fake_lora_business.c —— 主机测试用的 lora_business 替身(实现)
 *   实现 cloud_proto.c 真正引用的那些符号:
 *     · lora_business_unbind()          —— sen.unbind 的异步入口
 *     · lora_business_open_bind_window() —— sen.bind 的 pair_hook 后端
 *     · lora_business_set_sensor_params() —— sen.set 的 0x01 参数下发
 *     · lora_business_device_state*() / lora_business_ensure_slaves()
 *   详见 fake_lora_business.h 的说明。
 * ========================================================================== */
#include "fake_lora_business.h"

#include <string.h>

#include "config_store.h"
#include "lora_business.h"

int      fake_unbind_calls;
char     fake_unbind_last_uid[27];
int      fake_unbind_result;
int      fake_unbind_simulate_finalize;

int      fake_pair_calls;
char     fake_pair_serial[24];
char     fake_pair_name[40];
uint16_t fake_pair_window;

/* ---- ★2026-10-05: sen.set -> LoRa 0x01 参数下发 ---- */
int      fake_sensor_param_calls;
char     fake_sensor_param_uid[27];
uint32_t fake_sensor_param_report_s;
uint16_t fake_sensor_param_thresh_mm;
int      fake_sensor_param_ret  = 1;

void fake_lora_business_reset(void)
{
    fake_device_state              = LORA_DEV_STATE_UNKNOWN;
    fake_unbind_calls              = 0;
    fake_unbind_last_uid[0]        = '\0';
    fake_unbind_result             = 0;
    fake_unbind_simulate_finalize  = 0;
    fake_pair_calls                = 0;
    fake_pair_serial[0]            = '\0';
    fake_pair_name[0]              = '\0';
    fake_pair_window               = 0u;

    fake_sensor_param_calls        = 0;
    fake_sensor_param_uid[0]       = '\0';
    fake_sensor_param_report_s     = 0u;
    fake_sensor_param_thresh_mm    = 0u;
    fake_sensor_param_ret          = 1;
}

/* ---- 被 cloud_proto.c 调用(sen.set): 把平台参数打包成 0x01 帧下发 ---- */
bool lora_business_set_sensor_params(const char *uid_hex,
                                     uint32_t report_period_s,
                                     uint16_t alarm_thresh_mm)
{
    fake_sensor_param_calls++;
    (void)strncpy(fake_sensor_param_uid, (uid_hex != NULL) ? uid_hex : "",
                  sizeof(fake_sensor_param_uid) - 1u);
    fake_sensor_param_uid[sizeof(fake_sensor_param_uid) - 1u] = '\0';
    fake_sensor_param_report_s  = report_period_s;
    fake_sensor_param_thresh_mm = alarm_thresh_mm;
    return (fake_sensor_param_ret != 0);
}

/* ---- 被 cloud_proto.c 调用: 发起解绑(异步) ---- */
int lora_business_unbind(const char *uid_hex)
{
    fake_unbind_calls++;

    if (uid_hex != NULL)
    {
        (void)strncpy(fake_unbind_last_uid, uid_hex, sizeof(fake_unbind_last_uid) - 1u);
        fake_unbind_last_uid[sizeof(fake_unbind_last_uid) - 1u] = '\0';
    }
    else
    {
        fake_unbind_last_uid[0] = '\0';
    }

    /* 模拟"业务层收到设备 0x80 ACK 后的收尾": 与 lora_business.c 的
     * _unbind_finalize() 做的是同一件事(删 config_store 含 gears 清理 + 落盘),
     * 好让用例能继续验证这条真实代码路径的落盘效果。 */
    if ((fake_unbind_result == 0) && (fake_unbind_simulate_finalize != 0)
        && (fake_unbind_last_uid[0] != '\0'))
    {
        (void)config_store_unbind_device(fake_unbind_last_uid, 0u);
    }

    return fake_unbind_result;
}

/* ---- 被 main.c 注册成 pair_hook; test_cloud 里由用例自己注册, 这里给个同签名实现 ----
 * 毕设版: 绑定固定为液位传感器, 签名已从 5 参数收敛为 3 参数。 */
bool lora_business_open_bind_window(const char *serial, const char *name,
                                    uint16_t window_s)
{
    fake_pair_calls++;
    (void)strncpy(fake_pair_serial, (serial != NULL) ? serial : "",
                  sizeof(fake_pair_serial) - 1u);
    fake_pair_serial[sizeof(fake_pair_serial) - 1u] = '\0';
    (void)strncpy(fake_pair_name, (name != NULL) ? name : "",
                  sizeof(fake_pair_name) - 1u);
    fake_pair_name[sizeof(fake_pair_name) - 1u] = '\0';
    fake_pair_window    = window_s;
    return true;
}

/* 替身默认回 UNKNOWN(=> online=false), 测试可用 fake_device_state 改成 ONLINE/OFFLINE。 */
lora_dev_state_t fake_device_state = LORA_DEV_STATE_UNKNOWN;

lora_dev_state_t lora_business_device_state(const char *uid_hex)
{
    (void)uid_hex;
    return fake_device_state;
}
lora_dev_state_t lora_business_device_state_bytes(const uint8_t uid[LORA_UID_BYTES])
{
    (void)uid;
    return fake_device_state;
}

void lora_business_ensure_slaves(void) { }
