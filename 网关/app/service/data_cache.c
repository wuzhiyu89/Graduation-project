/**
 * @file  data_cache.c
 * @brief 设备数据缓存 —— ★2026-10-08 起只剩【下发缓存】★
 *
 * 【本次删掉了什么, 为什么】
 *   原来这里还有一套"上行传感器上报环形缓存"(`g_cache_up_buf` + peek/pop/drop/
 *   get_count)和一个 `data_cache_poll()` —— 后者的唯一作用是把队列里最旧的一条交给
 *   `lora_business_publish_report()` 组帧成 hex, 发到那条"默认 MQTT 主题"
 *   `mod/mod_upload`。
 *
 *   甲方明确要求不再使用 `mod/mod_upload`, 历史残留一并清除; 而那条通道本来就和
 *   `rt/gw/{gwId}/telemetry` 是【同一份数据的第二条路】—— 留着只会让平台侧
 *   面对"两个主题各来一遍、可能还不一致"。所以整条 hex 上行通道删掉了。
 *
 * 【上行数据没有丢, 一点都没少】
 *   `data_cache_up_push()` 保留着 —— 因为它【同时】是 `last_value` 快照表的唯一漏斗
 *   (`last_value_update_ex`)。`mod/telemetry` 的 JSON 正是从那张快照表取值的,
 *   所以上游 0x02 解析、快照更新、心跳/周期上报这条主路径【完全不受影响】。
 *   变的是: 不再往一个 FIFO 里塞、也没有人再把它发成 hex。
 */
#include "data_cache.h"
#include "last_value.h"     /* 按 uid 的"最后值"快照表(纯逻辑模块, 主机可测) */
#include "bsp_tim.h"        /* get_tick_ms() */
#include <string.h>

/* 下发：网关待下发指令缓存（普通数组，非环形；同一个uid只允许存在一条待下发） */
static cache_down_item_t g_cache_down_buf[DATA_CACHE_DOWN_MAX_ITEM];

void data_cache_init(void)
{
    memset(g_cache_down_buf, 0, sizeof(g_cache_down_buf));

    last_value_reset();     /* 最后值快照表与缓存同生命周期 */
}

/********************* 上行: 只更新"最后值"快照 *********************/
void data_cache_up_push(const uint8_t *uid, uint16_t level_mm, uint16_t bat_pct,
                        uint32_t time_mdhhm, int8_t rssi_dbm)
{
    /* 【记录"最后值"快照】供 sen.query 按 uid 查最近一次上报,
     * 也供 main.c 组装 rt/gw/{gwId}/telemetry 的 JSON。
     *
     * 本函数是【所有传感器上报的唯一漏斗】, 所以放在这里 —— 调用方一行都不用改。
     * 注: 形参 bat_pct 是电量百分比(见调用点 bat_pct);
     *     uid 的 const 与 rssi_dbm 一路是 2026-09-30 第 10 条加的, 供遥测的
     *     sensors[].rssi 用。
     *
     * ★★2026-10-30 口径变更: 网关【不再换算】, 只透传设备报上来的液位★★
     *
     *   【为什么撤掉 A1 的换算】
     *     用户核对设备端 0x01 参数表后确认: 液位传感器固件【自己】实现了
     *       [6]   测量模式 0=液位 / 1=距离
     *       [7-8] 安装高度 cm(安装面到池底)  [9-10] 校准偏移 mm(有符号)
     *     并在设备内部把 distance 换算成液位, 0x02 帧里报上来的【已经是液位值】。
     *     网关再做一次 水深 = 安装高度 − 距离 + 校准 ⇒ 重复扣减, distance 模式下
     *     平台看到的水位会系统性偏低, 而且越低越多, 现场极难联想到换算。
     *
     *   【现在的口径】
     *     level_mm 与 raw_mm 恒相等 —— 两者都等于设备报的值, 网关一个字节都不加工。
     *     telemetry 的 rawMm / levelCm、分级、{level} 渲染、sen.query 的 lastLevelCm
     *     全部同源, 不会出现"平台和屏幕显示不一致"。
     *
     *   【配置里那三个字段仍然有效】
     *     sensors[].measureMode / installHeightCm / calibOffsetMm 照旧存储、照旧可被
     *     sen.set / cfg.set 修改, 也照旧通过 0x01 下发给传感器 —— 只是【网关自己不用
     *     它们算】, 真正应用它们的是设备固件。所以别删这三个字段。 */
    (void)last_value_update_full(uid, (int32_t)level_mm, level_mm, bat_pct,
                                 time_mdhhm, rssi_dbm, get_tick_ms());
}

/********************* 下发缓存实现 *********************/
int data_cache_down_push(uint8_t *uid, uint8_t *buf, uint8_t len)
{
    if (len > sizeof(g_cache_down_buf[0].payload)) return -1;
    /* 先判断该uid是否已经存在待下发条目，存在则覆盖旧的 */
    for(uint8_t i = 0; i < DATA_CACHE_DOWN_MAX_ITEM; i++)
    {
        if(g_cache_down_buf[i].valid == true && (memcmp(g_cache_down_buf[i].dev_uid, uid, sizeof(((last_value_t*)0)->uid)) == 0))
        {
            memcpy(g_cache_down_buf[i].dev_uid, uid, sizeof(((last_value_t*)0)->uid));
            memcpy(g_cache_down_buf[i].payload, buf, len);
            g_cache_down_buf[i].pay_len = len;
            g_cache_down_buf[i].valid = true;
            return 0;
        }
    }
    /* 查找空闲槽 */
    for(uint8_t i = 0; i < DATA_CACHE_DOWN_MAX_ITEM; i++)
    {
        if(g_cache_down_buf[i].valid == false)
        {
            memcpy(g_cache_down_buf[i].dev_uid, uid, sizeof(((last_value_t*)0)->uid));
            memcpy(g_cache_down_buf[i].payload, buf, len);
            g_cache_down_buf[i].pay_len = len;
            g_cache_down_buf[i].valid = true;
            return 0;
        }
    }
    return -1; /* 缓存满 */
}

bool data_cache_down_fetch_by_uid(uint8_t *uid, cache_down_item_t *out_item)
{
    for(uint8_t i = 0; i < DATA_CACHE_DOWN_MAX_ITEM; i++)
    {
        if(g_cache_down_buf[i].valid == true && (memcmp(g_cache_down_buf[i].dev_uid, uid, sizeof(((last_value_t*)0)->uid))==0))
        {
            memcpy(out_item, &g_cache_down_buf[i], sizeof(cache_down_item_t));
            return true;
        }
    }
    return false;
}

void data_cache_down_del_by_uid(uint8_t *uid)
{
    for(uint8_t i = 0; i < DATA_CACHE_DOWN_MAX_ITEM; i++)
    {
        if(g_cache_down_buf[i].valid == true && (memcmp(g_cache_down_buf[i].dev_uid, uid, sizeof(((last_value_t*)0)->uid))==0))
        {
            memset(&g_cache_down_buf[i],0,sizeof(cache_down_item_t));
            g_cache_down_buf[i].valid = false;
            return;
        }
    }
}

/* "删之前先确认缓存里还是刚发出去的那一条"：
 * 等待设备 ACK 期间，云端可能又下发一条新指令覆盖同一 uid 的缓存条目。
 * 若按 uid 无条件删，会把新指令一起删掉；因此只有 func 和载荷都一致时才删。 */
bool data_cache_down_del_if_match(uint8_t *uid, uint8_t func, const uint8_t *data, uint8_t len)
{
    for(uint8_t i = 0; i < DATA_CACHE_DOWN_MAX_ITEM; i++)
    {
        cache_down_item_t *it = &g_cache_down_buf[i];

        if(it->valid != true || (memcmp(it->dev_uid, uid, sizeof(((last_value_t*)0)->uid)) != 0))
        {
            continue;
        }

        /* payload[0] = func, payload[1..] = data */
        if((it->pay_len != (uint8_t)(len + 1U)) || (it->payload[0] != func))
        {
            return false;           /* 已经是另一条指令了 -> 不删 */
        }
        if(len > 0U)
        {
            /* data == NULL 时无法确认"是不是同一条" -> 不能删 */
            if((data == NULL) || (memcmp(&it->payload[1], data, len) != 0))
            {
                return false;
            }
        }

        memset(it, 0, sizeof(cache_down_item_t));
        it->valid = false;
        return true;
    }
    return false;                   /* 没这条(可能已被清掉) */
}

uint8_t data_cache_down_get_count(void)
{
    uint8_t cnt = 0;

    for(uint8_t i = 0; i < DATA_CACHE_DOWN_MAX_ITEM; i++)
    {
        if(g_cache_down_buf[i].valid) cnt++;
    }
    return cnt;
}
