/**
 * @file  app_level.c
 * @brief 本地自治的时间侧：全部失联 fail-safe + manual 窗口回归
 *
 * 水位"算"在 service/level_eval.c，"取数+串接"在 level_supervisor.c；
 * 这里只做两件需要时钟和配置写入的事：
 *   · 全部失联时的兜底动作（keep / alarm / off）
 *   · manual 模式到期自动回到 auto，并强制重评估一次
 *
 * 不组 JSON、不发 MQTT —— 下发走 lora_business_apply_gear()，上报由主循环决定。
 */
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

static bool s_off_active;   /* off 策略故障画面生效中(供 status/telemetry) */
static bool s_fs_active;      /* fail-safe 策略已执行中 */
static uint32_t s_fs_last_send_ms;

#include "log.h"
#include "app_core.h"
#include "bsp_tim.h"
#include "config_store.h"
#include "level_supervisor.h"
#include "lora_business.h"

/* ==================================================================
 * 液位传感器【全部失联】时的 fail-safe 动作(文档第 5 章, 用户第 7 条)
 * ==================================================================
 * 【什么时候进来】level_supervisor 判出"曾经有水位、现在一台在线的都没有",
 *   并且在【刚跌进去的那一拍】给 failsafe_enter=true(边沿, 只做一次)。
 * 【做什么】按 rules.sensorOfflinePolicy:
 *     keep  -> 保持当前档位(什么都不发, 只记日志)
 *     alarm -> 强制档位 3(告警)
 *     off   -> ★发【专用故障画面】, 不是 gears[0] 的内容★
 *              屏亮 / 不闪 / 音量 0 / 竖屏 / 两行居中 / 黄黄 /
 *              "设备故障" + "勿依赖提示" / 无 TTS
 *              (见 lora_business_apply_fault_display(); 用户 2026-10-30 定稿)
 * 【文本用什么水位渲染】全失联时没有"当前水位", 用【最后一次已知水位】——
 *   这是唯一合理的选择。仅 keep/alarm 两条会走 gears[] 模板渲染;
 *   off 用的是固定文案, 与 gears[0] 配了什么无关。
 * 【为什么要 mark_sent】强制改档之后去重基准得跟着走 —— 否则传感器恢复回来时会
 *   拿"上一次自动下发的档位"去比, 该发的不发或白重发一条。
 * ================================================================== */
bool app_level_off_failsafe_active(void)
{
    return s_off_active;
}

static uint32_t failsafe_repeat_ms(const config_store_t *cfg)
{
    uint32_t max_s = 60u;           /* 兜底 60s, 与 sensor_offline 默认一致 */
    uint8_t  i;

    for (i = 0u; (i < cfg->sensor_count) && (i < (uint8_t)CFG_SENSOR_MAX); i++)
    {
        if (cfg->sensors[i].enabled == 0u) { continue; }
        if (cfg->sensors[i].report_period_s > max_s) { max_s = cfg->sensors[i].report_period_s; }
    }
    return max_s * 3000u;           /* ×3, 毫秒 */
}

static void failsafe_apply_policy(const config_store_t *cfg, uint8_t policy,
                                  uint8_t gear, bool entering)
{
    char    led1[CFG_LEDTEXT_MAX + 1u];
    char    led2[CFG_LEDTEXT_MAX + 1u];
    char    tts[CFG_TTSTEXT_MAX + 1u];
    int32_t lv = 0;
    uint8_t g  = gear;

    if ((policy == (uint8_t)CFG_OFFLINE_KEEP) && entering)
    {
        return;                     /* keep: 进入时不动屏 */
    }
    if (policy == (uint8_t)CFG_OFFLINE_OFF)
    {
        /* ★★2026-11-03: off = 执行【档位 0】(用户口径)★★
         * 以前这里发的是专用故障画面("设备故障"/"勿依赖提示"); 用户定稿改为
         *   "off 档位就是档位 0" —— 走正常档位下发链路, 内容取 gears[0] 的模板
         *   (出厂默认是空文本 + 关屏, 等价于原来的关屏效果)。
         * 逐台受理结果仍要带给 event.action(trigger=failsafe) 的 results[]。 */
        static lora_gear_result_t res[CFG_DEVICE_MAX];
        uint8_t total = 0u;
        int     sent;

        g = 0u;
        led1[0] = '\0';
        led2[0] = '\0';
        tts[0]  = '\0';
        if (level_supervisor_last(&lv, NULL))
        {
            (void)level_render_template(led1, (uint16_t)sizeof(led1),
                                        cfg->gears[0].led_text1, lv);
            (void)level_render_template(led2, (uint16_t)sizeof(led2),
                                        cfg->gears[0].led_text2, lv);
            (void)level_render_template(tts,  (uint16_t)sizeof(tts),
                                        cfg->gears[0].tts_text,  lv);
        }

        (void)memset(res, 0, sizeof(res));
        sent = lora_business_apply_gear_ex(0u, NULL, led1, led2, tts,
                                           res, (uint8_t)CFG_DEVICE_MAX, &total);
        if (sent > 0)
        {
            level_supervisor_mark_sent(0u, led1, led2);
        }

        if ((sent > 0) && entering)
        {
            uint8_t n = (total > (uint8_t)CFG_DEVICE_MAX)
                            ? (uint8_t)CFG_DEVICE_MAX : total;

            app_event_action_failsafe(res, n);
        }
        return;
    }

    if (!level_supervisor_last(&lv, NULL)) { return; }
    /* alarm 固定 3; keep 用当前自动档位 */
    g = (policy == (uint8_t)CFG_OFFLINE_ALARM) ? 3u : gear;
    if (g >= (uint8_t)CFG_GEAR_COUNT) { g = 0u; }

    led1[0] = '\0';
    led2[0] = '\0';
    tts[0]  = '\0';
    if (g < (uint8_t)CFG_GEAR_COUNT)
    {
        (void)level_render_template(led1, (uint16_t)sizeof(led1), cfg->gears[g].led_text1, lv);
        (void)level_render_template(led2, (uint16_t)sizeof(led2), cfg->gears[g].led_text2, lv);
        (void)level_render_template(tts,  (uint16_t)sizeof(tts),  cfg->gears[g].tts_text,  lv);
    }

    if (lora_business_apply_gear_ex(g, NULL, led1, led2, tts, NULL, 0u, NULL) > 0)
    {
        level_supervisor_mark_sent(g, led1, led2);
    }
}

void app_failsafe_step(const level_decision_t *ld)
{
    const config_store_t *cfg = config_store_get();
    bool manual;
    uint8_t policy;

    if ((cfg == NULL) || (ld == NULL)) { return; }

    manual = (cfg->rules.mode == (uint8_t)CFG_RULE_MANUAL);
    policy = cfg->rules.sensor_offline_policy;

    /* ---- 退出 fail-safe: 强制补发一次正常档位 ---- */
    if (!ld->failsafe)
    {
        if (s_off_active || s_fs_active)
        {
            s_off_active     = false;
            s_fs_active      = false;
            log_info("[LEVEL] 液位恢复 -> 强制补发一次正常档位\r\n");
            level_supervisor_force_dispatch();
        }
        return;
    }

    /* 没有有效水位基线 -> 不动作(从没收到过上报) */
    if (!ld->have_level && !level_supervisor_last(NULL, NULL)) { return; }

    /* ★2026-11-03: manual 模式下【完全】不介入 ★
     * 用户口径: "manual 模式下传感器离线不用管, sensorOfflinePolicy 完全由云端
     *   接管"。所以这里既不覆盖当前档位, 也【不再记"结束后要补做"】——
     *   原来记 s_fs_manual_wait 会让 manual 一结束就立刻按 policy 覆盖一遍,
     *   等于云端刚手动设的档位被"补做"顶掉。
     * 同时把 fail-safe 的"已生效"标记清掉: manual 结束回到 auto 后, 下一拍会按
     *   【当时】的实际状态重新判 —— 那时若仍全失联, 自然重新进入 policy(重新记日志)。
     */
    if (manual)
    {
        if (s_fs_active || s_off_active)
        {
            log_info("[LEVEL] manual 模式: 全部失联的 fail-safe 暂停(由云端接管)\r\n");
        }
        s_fs_active      = false;
        s_off_active     = false;
        return;
    }

    /* ---- 进入 fail-safe ---- */
    if (!s_fs_active)
    {
        log_info("[LEVEL] !! 全部失联 fail-safe: policy=%s gear=%u\r\n",
                 (policy == (uint8_t)CFG_OFFLINE_ALARM) ? "alarm" :
                 (policy == (uint8_t)CFG_OFFLINE_OFF)   ? "off"   : "keep",
                 (unsigned)ld->failsafe_gear);

        s_fs_active       = true;
        s_fs_last_send_ms = get_tick_ms();
        s_off_active      = (policy == (uint8_t)CFG_OFFLINE_OFF);

        /* keep 的"重发当前档位"也在这一个函数里(它自己按 policy 分支) */
        failsafe_apply_policy(cfg, policy, ld->failsafe_gear, true);
        return;
    }

    /* ---- 持续期间: 只有 off 周期重发 ---- */
    if (policy == (uint8_t)CFG_OFFLINE_OFF)
    {
        uint32_t period = failsafe_repeat_ms(cfg);

        if ((uint32_t)(get_tick_ms() - s_fs_last_send_ms) >= period)
        {
            s_fs_last_send_ms = get_tick_ms();
            failsafe_apply_policy(cfg, policy, ld->failsafe_gear, false);
        }
    }
}

/* ==================================================================
 * manual 模式窗口(用户第 6 条)
 * ==================================================================
 * 【分工】"manual 期间抑制自动联动"在 level_supervisor.c 里(它直接读 rules.mode);
 *   这里只管【回归】: manualTimeoutSeconds 到期把 mode 写回 auto, 并强制重评估一次
 *   (文档 5.5: "切回 auto 时网关立即按当前水位重评估一次并刷新联动状态")。
 * 【计时基准】进入 manual 的时刻, 以下三种情况都会重新计时:
 *   ① 刚进入 manual;
 *   ② manual 期间【配置版本变了】(例如 rule.mode / cfg.set 落了盘);
 *   ③ manual 期间又下发了一次 dev.ctrl —— ★2026-11-02 起 dev.ctrl 不再写 Flash、
 *      也就不再抬 configVersion, 所以改成比 level_supervisor_manual_cmd_seq()
 *      这个显式计数器(见文件末尾的说明)。"每次手动控制后重新计时"的语义不变。
 *   ⚠ 代价: 任何别的配置改动也会顺带刷新窗口(已在此注明)。
 * 【0 = 不回归】(文档 3.7: manualTimeoutSeconds 0 = 不回归)。
 * 【为什么放在这里而不是 cloud_proto】回归是"时间"触发的, 主循环才是时间的主人;
 *   cloud_proto 只负责"用户下命令"这一侧。
 * ================================================================== */
static uint32_t s_man_start_ms;      /* 本次 manual 的起点 */
static uint32_t s_man_cfg_ver;       /* 起点时的配置版本(变了就重新计时) */
static bool     s_man_was;           /* 上一拍是不是 manual(用于检测"回到 auto") */
static uint16_t s_man_cmd_seq;       /* 上次看到的"手动控制"计数(见文件末尾说明) */

void app_manual_step(void)
{
    const config_store_t *cfg = config_store_get();
    uint32_t now = get_tick_ms();
    uint32_t ver;

    if (cfg == NULL) { return; }
    ver = config_store_version();

    /* ---- 不在 manual: 若刚从 manual 回来, 强制重评估一次 ---- */
    if (cfg->rules.mode != (uint8_t)CFG_RULE_MANUAL)
    {
        if (s_man_was)
        {
            s_man_was = false;
            log_info("[CTRL] 已回到 auto 模式(自动联动恢复), 强制重评估一次\r\n");
            level_supervisor_force_next();
            level_supervisor_force_dispatch();
        }
        return;
    }

    /* ---- 刚进 manual, 或 manual 期间配置/手动命令又变过 -> (重新)开始计时 ---- */
    if ((!s_man_was) || (ver != s_man_cfg_ver) ||
        (level_supervisor_manual_cmd_seq() != s_man_cmd_seq))
    {
        s_man_was      = true;
        s_man_start_ms = now;
        s_man_cfg_ver  = ver;
        s_man_cmd_seq  = level_supervisor_manual_cmd_seq();
        log_info("[CTRL] 进入 manual 模式: 自动联动已抑制, manualTimeoutSeconds=%us%s\r\n",
                 (unsigned)cfg->rules.manual_timeout_s,
                 (cfg->rules.manual_timeout_s == 0u) ? "(0=不回归)" : "");
        return;
    }

    if (cfg->rules.manual_timeout_s == 0u) { return; }      /* 不回归 */

    if ((uint32_t)(now - s_man_start_ms)
        >= ((uint32_t)cfg->rules.manual_timeout_s * 1000U))
    {
        config_store_t tmp;

        (void)memcpy(&tmp, cfg, sizeof(tmp));
        tmp.rules.mode = (uint8_t)CFG_RULE_AUTO;

        if (config_store_save(&tmp) == CFG_OK)
        {
            /* ★★2026-10-14 修复: 必须把 tmp 提交给"当前配置"实例 ★★
             * ------------------------------------------------------------------
             * 【漏了什么】config_store_save() 只负责【落 Flash】, 它【不更新】
             *   RAM 里那份"当前配置"。调用方必须自己 memcpy 到
             *   config_store_get_mut() —— 对比 cloud_proto.c 的 sys.factory_reset:
             *   那处 save 之后有一句 "落盘成功才提交给当前配置实例" 的 memcpy, 这里漏了。
             *   那处有, 这里漏了。
             *
             * 【后果(2026-10-14 用户实测, 三条症状同时出现)】
             *   ① 云端 status.report 的 mode 恒为 manual —— 因为它读的是 RAM;
             *   ② manual 抑制一直生效 -> 液位变化不再联动声光设备;
             *   ③ 本次 save 抬了配置版本 -> 下一拍 line 682 的
             *      `ver != s_man_cfg_ver` 成立 -> 走"刚进 manual, 重新计时"分支
             *      -> 每 manual_timeout_s 重复一次, 永远回不到 auto。
             *   ⇒ 一个遗漏解释全部三个现象。
             *
             * 【提交之后】下一拍 line 670 会走"已回到 auto"那支:
             *   打 "[CTRL] 已回到 auto 模式(自动联动恢复), 强制重评估一次"
             *   并调 level_supervisor_force_next() -> 档位按当前水位重算下发。 */
            (void)memcpy(config_store_get_mut(), &tmp, sizeof(tmp));

            log_info("[CTRL] manual 超时(%us) -> 自动回归 auto\r\n",
                     (unsigned)cfg->rules.manual_timeout_s);
            /* 不在这里置 s_man_was=false: 下一拍会发现 mode 已经不是 manual,
             * 走上面那支"回到 auto"并强制重评估 —— 一处收口, 不会漏。 */
        }
        else
        {
            log_info("[CTRL] !! manual 超时, 但写回 auto 失败(Flash), 下一拍重试\r\n");
        }
    }
}

/* ==================================================================
 * ★2026-11-02: dev.ctrl 不再写 Flash, "重新计时"改由 service 层的计数器通知★
 * ------------------------------------------------------------------
 * app_manual_step() 原来靠"configVersion 变了"判定"又下发了一次手动控制",
 * 从而把 manualTimeoutSeconds 的窗口重计一次(协议 5.5)。
 * 现在 dev.ctrl 只改 RAM、不抬版本号, 那个隐式信号就没了 —— 改为对比
 * level_supervisor_manual_cmd_seq()(由 cloud 层在 dev.ctrl 里 +1)。
 * 【为什么放 service 层】cloud 层不能 include app/core 的头(依赖方向是
 *   core -> cloud, 反向就成环了); 而 level_supervisor 本来就是 cloud 已依赖的。
 * ================================================================== */
