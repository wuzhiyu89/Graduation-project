/**
 * @file  app_loop.c
 * @brief 主循环（裸机协作式调度，1 ms 时基分频）
 *
 * 节拍表见 docs/ARCHITECTURE.md 第 6 节：
 *   1ms   射频中断/帧分发/重传 · 5ms AT+OTA 状态机 · 10ms 控制台 ·
 *   1s    状态打印/水位评估/联动/补报/心跳准备 · 心跳按配置周期
 *
 * 原则：每一拍都只推进一步，不允许阻塞；末尾与 1 ms 对齐让中断能抢占。
 */
#include "n32h47x_48x.h"
#include <stdint.h>

#include "log.h"
#include "app_config.h"
#include "app_core.h"
#include "app_mgmt.h"       /* ★任务三 T3-4: MQTT ONLINE 上升沿上报 checkin★ */
#include "app_state.h"
#include "bsp_tim.h"
#include "cloud_proto.h"
#include "config_store.h"
#include "g4_at.h"
#include "g4_urc.h"
#include "level_eval.h"     /* level_state_of / level_state_to_gear: 给状态打标签 */
#include "last_value.h"     /* last_value_get: 算有效水位 */
#include "sensor_offline.h" /* sensor_offline_threshold_ms: 在线判据 */
#include "lora_business.h"
#include "ota_health.h"
#include "flash_map.h"
#include "ota_port.h"
#include "ota_http.h"
#include "xflash_store.h"   /* xf_ring_poll: 外挂 Flash 环指针按 30s 空闲落盘 */
#include "xf_log.h"         /* xf_log_poll: 日志行落进 LOG 环 */
#include "event_cache.h"    /* event_cache_poll: 待提交槽 1s 到点落盘 */

/**
 * @brief 刷新"当前水位状态/档位"标签(毕设版: 纯数据, 无联动)
 *
 * 从各在线且启用的液位传感器里取最大水位, 按阈值映射成状态与档位,
 * 存进 app_state 供 status.report / telemetry 打标签。
 * 不防抖、不下发、不产生事件 —— 那些联动逻辑已随一体机删除。
 */
static void loop_refresh_level_label(void)
{
    const config_store_t *cfg = config_store_get();
    int32_t  levels[CFG_SENSOR_MAX];
    uint8_t  select[CFG_SENSOR_MAX];
    int32_t  eff = 0;
    uint8_t  i;
    uint8_t  n;
    uint32_t now = get_tick_ms();

    if (cfg == NULL) { return; }

    n = (cfg->sensor_count > (uint8_t)CFG_SENSOR_MAX) ? (uint8_t)CFG_SENSOR_MAX
                                                      : cfg->sensor_count;
    for (i = 0u; i < n; i++)
    {
        const cfg_sensor_t *s = &cfg->sensors[i];
        uint8_t      uid[CFG_UID_BYTES];
        last_value_t lv;

        levels[i] = 0;
        select[i] = 0u;
        if (s->enabled == 0u) { continue; }
        if (!cfg_uid_to_bytes(s->uid, uid)) { continue; }
        if (!last_value_get(uid, &lv)) { continue; }
        if ((uint32_t)(now - lv.rx_tick_ms) >= sensor_offline_threshold_ms(s->report_period_s))
        {
            continue;
        }
        levels[i] = lv.level_mm;
        select[i] = 1u;
    }

    if (level_effective(levels, select, n, &eff))
    {
        level_state_t st = level_state_of(eff, &cfg->thresholds);
        app_note_level((uint8_t)st, level_state_to_gear(st));
    }
}

void app_loop_run(void)
{
    uint32_t now;
#if MAIN_G4_ENABLE
    uint32_t t_g4;
#endif
#if MAIN_G4_ENABLE && MAIN_GW_ONLINE_ENABLE
    uint32_t t_online;
#endif
    uint32_t t_console;
    uint32_t t_status;

    /* ==================== 主循环 ==================== */
    now        = get_tick_ms();
    t_console  = now;
    t_status   = now;
#if MAIN_G4_ENABLE
    t_g4       = now;
#endif
#if MAIN_G4_ENABLE && MAIN_GW_ONLINE_ENABLE
    t_online   = now;
#endif

    while (1)
    {
        now = get_tick_ms();

        /* ★★2026-10-11: 健康判定(每圈都调, 但代价只有 3 次比较)★★
         * 【判据(三条缺一不可)】
         *   ① 4G 在线                      -> g4_at_is_online()
         *   ② 至少一条上行被 broker puback  -> g4_urc_puback_count() > 0
         *      ★ 用 puback 而不是"发布成功": 模组回 OK 只说明它自己收下了,
         *        puback 才是"云端确实收到了"的证据(见上面 [STAT2] 的注释)。
         *   ③ uptime > 60s(OTA_HEALTH_MIN_UPTIME_S) -> 挡"起来就崩"
         * 三条都满足就把本槽"定案"(写一次 Flash), 之后本函数立即变成空操作。
         * 【为什么每圈调而不是挂 1s 节拍】: 定案后 s_done=true 直接 return,
         *   定案前也只是 3 次比较 —— 比"再找一个 1s 节拍插入点"简单且不易错。 */
#if (FM_HAS_BOOTLOADER == 0)
        ota_health_poll(g4_at_is_online(),
                        (g4_urc_puback_count() > 0u),
                        (uint32_t)(now / 1000u));
#else
        /* A/B: pending 槽跑够 60s + 联网 + 有上行确认 -> 转正(由 BL 下次启动沿用) */
        ota_port_boot_poll(g4_at_is_online(),
                           (g4_urc_puback_count() > 0u),
                           (uint32_t)(now / 1000u));
#endif

        /* (A) 每 1ms: LoRa radio 中断处理 + 帧解析分发 + 从机表/重传/上行缓存 */
#if MAIN_LORA_ENABLE
        lora_business_poll();
#endif

        /* (A2) sys.reboot 的延迟重启(v37+): 命令只登记"什么时候重启", 真正的软件复位
         *      在这里做 —— 这样 cloud_proto 不用直接碰 NVIC_SystemReset, 主机单测
         *      也能把这条命令的逻辑验完。纯比较, 每个循环跑一次的开销可以忽略。 */
        if (cloud_proto_reboot_due(app_now_unix()))
        {
            log_info("[SYS] sys.reboot 到期, 执行软件复位(NVIC_SystemReset)...\r\n");
            delay_ms(50);               /* 让上面那行日志从串口发完 */
            NVIC_SystemReset();
        }

        /* (B) 每 5ms: 收包泵 + 云端下行投递 + 非阻塞 AT 状态机 + 发布子流程
         *     ⚠️ 这里【不会阻塞】: 每条 AT 命令每次只推进一步, 收包由 g4_io_pump()
         *        一次性消费干净。模组不应答也只是状态机停在原地重试, 主循环节拍不变。
         *        模组没调通就把 MAIN_G4_ENABLE 改成 0。 */
#if MAIN_G4_ENABLE
        if ((uint32_t)(now - t_g4) >= TICK_G4_POLL_MS)
        {
            t_g4 = now;
            g4_at_poll();

            /* OTA over HTTP 必须跟【同一节拍】推进, 不能放到下面 1s 的 (D) 块:
             * 下载状态机每拍要发一条 AT(HEADER/REQUEST) 并消化 URC 字节队列,
             * 而模组按 fragment 配置以 200 B/块往外吐。若 1s 才推一次, 就变成
             * 200 B/s —— 64 KB 要 5 分钟以上, 还能把 8192 的环形缓冲灌爆。
             * 本函数纯粹是状态机推进, 不阻塞、空闲时什么都不做。 */
            ota_http_poll();
        }
#endif

        /* (C) 每 10ms: 日志口手敲 AT 转发给 4G 模组(台架调试)
         *     注: 手敲命令会插入 AT 流, 可能被状态机的关键字匹配"看到",
         *         排障时尽量避开正在握手/发布的时间窗口。 */
        if ((uint32_t)(now - t_console) >= TICK_CONSOLE_MS)
        {
            t_console = now;
            app_console_poll();
        }

        /* (D) 每 1s: 状态上报 + LED 心跳 */
        if ((uint32_t)(now - t_status) >= TICK_STATUS_MS)
        {
            t_status = now;

            app_status_report();

            /* ---- ★2026-11-12 任务三 T3-4: 开机报到 ----
             * 放在 1s 块里, 不在每圈调用: 内部只看 g4_at_is_online() 的上升沿,
             * 空闲时是 3 次比较, 但不占发布槽。上升沿发生在【两条 MQTTSUB 全部
             * 成功 + REG_ARM 之后】(g4_at.c), 所以此刻管理面应答通道已可用。
             * 与心跳(E 块)可能同拍撞发布槽: 两边都自带重试(checkin 3 拍 /
             * 心跳 2s 后重试), 不需要在这里加互斥或延时。 */
            app_mgmt_online_poll(g4_at_is_online());

            /* ---- 水位状态标签 + 遥测上行(毕设版: 纯数据, 无联动) ----
             * 有效水位(max) -> 阈值判状态 -> 存状态/档位标签;
             * 没有防抖、没有档位下发、没有 state_change 事件、没有 fail-safe、
             * 没有 manual 窗口 —— 这些一体机联动逻辑已全部删除。 */
            loop_refresh_level_label();
            app_telemetry_step();

            /* ---- 断线补报: 每次最多补一页(16 条), 缓存空了立刻返回 (v70) ---- */
            app_backfill_poll();

            /* ---- ★2026-11-11: 待补报队列的两个节拍动作 ----
             * ① event_cache_poll(): 把"待提交槽"里的最后一条持续状态(低电量/离线)
             *    写进 Flash。合并靠它收敛 —— 不调的话那条要等到下一条事件才落盘。
             * ② xf_ring_poll(): 环的 head/tail 空闲 30 s 后落一次 META。
             *    记录本身在写入瞬间就已进 Flash, META 只是指针; 掉电后开机由
             *    xf_ring_load() 顺着 head 往前扫描补回来(见 xflash_store.c)。
             * Flash 不在时两者都是空操作。 */
            event_cache_poll();
            xf_ring_poll();

            /* ---- ★2026-11-11: 日志落盘 ----
             * fputc 里只把整行塞进 RAM 队列（那里绝不能阻塞）；真正的 Flash 写
             * 在这里做，每次最多 2 条，把页编程/换扇区擦除的耗时摊到主循环里。
             * 队列空时是一次判断就返回，稳态零开销。 */
            xf_log_poll();

            /* ---- ★2026-10-18: 逐条液位上报的补报(一次一条) ----
             * 与上面的"事件补报"分开跑: 两者缓存不同、topic 不同。
             * 先事件后遥测 —— 事件量小、时效性强; 平台侧各自按 ts 排序。 */
            app_telemetry_backfill_poll();

#if MAIN_LORA_ENABLE
            /* ---- 液位失联检测的【定时兜底腿】(2026-09-27 用户拍板加) ----
             * 【为什么光有"上报驱动"不够】原口径是"每收到一路液位上报就顺带检查
             * 全部液位从机", 但那只在【至少有一台还在报】时才成立。现场只挂了一台
             * 液位(210001), 它彻底不报时【没有任何帧能触发检查】—— 而"唯一那台死了"
             * 恰恰是最该报的 case, 于是最需要它的场景反而检测不到。
             * 所以再加这条腿: 复用【已有的 1s 节拍】, 【不新增任何定时器】。
             * check 函数是幂等的(只在"在线->失联"跳变时发一次事件), 每秒调用不会
             * 造成重复上报; 上报驱动那条腿保留不动, 两条腿互为补充。 */
            lora_business_check_sensor_offline();

            /* *2026-10-18: 出厂字段操作(0x06 重发 / 0x07 超时)推进
             * 1 秒粒度足够: 写字段要 5s 才补发一条, 读字段要等 12s。 */
            lora_business_field_poll();
#endif

#if MAIN_HEARTBEAT_LED
            GPIO_TogglePin(GPIOA, GPIO_PIN_8);
#endif

            /* OTA over HTTP 的进度上报 + 就绪后复位。
             * 放在 1s 块里是【刻意的】: 文档 7.6 要求 ota.progress 每 10% 或 10s
             * 一条, 状态机本身的推进在 5ms 块里(见上面 (B)), 两件事分开。
             * 本函数只读状态 + 发 MQTT, 不发 AT、不阻塞; 空闲时第一件事就是 return。
             * 刻意【不】加 #if MAIN_G4_ENABLE: 那样在 4G 关掉时它就成了没人调的
             * static 函数, 编译器会报 unused-function。 */
            app_ota_progress_poll();
        }

        /* (E) 网关在线心跳 —— 往云端 pub 一条 status.report(或裸 "GW_ONLINE")
         *
         * 为什么要有: 云端只认"有没有收到报文"。业务数据都来自下属从机, 从机可能
         * 几十分钟不上报, 这段时间云端会以为网关掉线了。这条心跳就是网关的
         * "我还活着"信号, 同时它也是 4G 链路的探针 —— 发不出去会累加 g4_at 里的
         * 发布失败计数, 连续失败就自动重新建链(见 g4_pub_mark_fail)。
         *
         * 【第 5 条 2026-09-30】周期改成读配置 network.statusIntervalSeconds
         *   (文档 3.2: 10~600s, 默认 60, 生效方式"立即")。以前这里是编译期宏,
         *   平台改了配置【一点反应都没有】。读不到/越界才回落到 MAIN_GW_ONLINE_MS。 */
#if MAIN_G4_ENABLE && MAIN_GW_ONLINE_ENABLE
        {
            uint32_t hb_period_ms = app_cfg_period_ms(config_store_get()->network.status_interval_s,
                                                      MAIN_GW_ONLINE_MS);

            /* ★2026-11-04(1103 联调 §六): 上线立刻发一条 status —— 但**不再**为了
             * "盖掉 retained 遗嘱"★
             *
             * 【原来为什么有这个】以前 status 与 lwt 都 retained。上电重连时 broker
             *   会先代发上一次会话欠下的 {"online":false}(见下), 平台按"最后一条
             *   retained"判在线, 于是要立刻补一条 retained status 把它盖回去, 否则
             *   平台会显示离线最多一个心跳周期。
             *
             * 【现在为什么不 retained】全线弃用 retained(§六, 见 cloud_profile 的
             *   s_doc_retained): broker 不再保存任何快照, 也就没有"要盖掉的旧状态"。
             *   平台按"最新消息 + 超时窗口"判在线。
             *
             * 【那这条"立刻发"还留不留】留。理由与 retained 无关: 平台判在线的依据
             *   就是收到新消息, 连上后 100ms 内发一条, 比等满一个周期(默认 30~60s)
             *   再发体验好得多 —— 现场最忌讳"上电后先显示离线半分钟"。
             *   代价只是一条报文, 与 retained 的存储/清理负担无关。
             *
             * 【仍在的现象】上电后可能先收到一条 {"online":false}, 那是【上一次】
             *   掉线时 broker 代发的遗嘱(本机 clientId=IMEI 固定, 旧会话被踢时才发出),
             *   属于历史消息, 不是本次状态; 平台应以后续 status 为准。 */
            {
                static bool s_hb_was_online;
                bool        online_now = g4_at_is_online();

                if (online_now && (!s_hb_was_online))
                {
                    t_online = now - hb_period_ms;      /* 让下面的判据立刻成立 */
                }
                s_hb_was_online = online_now;
            }

            /* ★2026-10-07: OTA 下载期间心跳【让路】★
             *
             * 现场(19:09) 实测的死法: 心跳/事件发布和 OTA 共用同一条 AT 引擎, 而
             * 发布一旦撞上 '>' 丢失, 光是等 '>' 就要占住引擎 15 秒
             * (G4_PUB_PROMPT_TIMEOUT_MS); 超时后立刻重发, 又一个 15 秒 —— 连着几轮
             * 就把 OTA 的下一片 AT+MHTTPHEADER 一直挤在门外:
             *   19:09:20  第一片 8192 收满(percent 52)
             *   19:09:20 ~ 19:10:20  【60 秒里一条 AT 都没发出去, 全是 MQTTPUB 在重试】
             *   19:10:20  片内超时(60s) -> 回滚到 7680 重下 7764 字节
             * 而就在这段"抢引擎"的时间里串口还丢了字节(丢字节 1->5),
             * 最终 MD5 对不上(算出 dd4a7606..., 云端 d9c8750b...) —— 前功尽弃。
             *
             * OTA 期间 ota.progress 仍然每 10 秒上报一次, 云端不会以为网关死了,
             * 所以这里让路是安全的; 升级一结束(READY/FAILED)就自动恢复。 */
            if (g4_at_is_online() && (!ota_http_busy()) &&
                ((uint32_t)(now - t_online) >= hb_period_ms))
            {
            sys_err_t hb_res;

#if MAIN_STATUS_JSON_ENABLE
            /* P1-8: 心跳载荷换成文档要求的 status.report JSON(替代裸 "GW_ONLINE") */
            hb_res = app_publish_status_json();
#else
            static const char gw_online[] = "GW_ONLINE";
            hb_res = g4_at_mqtt_publish(NULL, (uint8_t *)gw_online,
                                        (uint16_t)(sizeof(gw_online) - 1u));
#endif
            if (hb_res == SYS_OK)
            {
                t_online = now;
#if MAIN_STATUS_JSON_ENABLE
                log_info("[GW] status.report 已发 (距上次发布成功 %lus)\r\n",
                         (unsigned long)(g4_at_pub_age_ms() / 1000u));
#else
                log_info("[GW] 在线心跳已发: GW_ONLINE (距上次发布成功 %lus)\r\n",
                         (unsigned long)(g4_at_pub_age_ms() / 1000u));
#endif
            }
            else
            {
                /* 【这一条以前是静默的, 而它就是"4G 好像不会保活"最可能的真相】:
                 * 原来失败时什么都不打印, 于是"心跳发不出去"这件事在现场完全看不见,
                 * 只能看到云端那边这台设备慢慢不理人了。
                 * 现在: ① 失败一定打日志; ② 不等满 30s, 2s 后就重试;
                 *       ③ g4_at 里【连续 3 次】(G4_PUB_FAIL_MAX)发布失败就判链路断,
                 *          走 g4_link_recover 直接 4G_RST 断电重启模组重建连接;
                 *       ④ 另有保活停滞看门狗(5 × 心跳周期 = 150s 没成功过一次)兜底。 */
                t_online = now - hb_period_ms + 2000U;
                log_info("[GW] !! 在线心跳【发布登记失败】(上一条还在发 / 状态机不在 ONLINE), "
                         "2s 后重试; 距上次成功 %lus\r\n",
                         (g4_at_pub_age_ms() == 0xFFFFFFFFu)
                             ? 0UL : (unsigned long)(g4_at_pub_age_ms() / 1000u));
            }
            }
        }
#endif

        /* 与 1ms 时基对齐, 避免空转(中断仍可抢占) */
        while (get_tick_ms() == now)
        {
        }
    }
}
