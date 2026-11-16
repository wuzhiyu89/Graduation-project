#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 Keil 的 CDC.map 里算出真实的资源账本。

用法:
    python tools/res_report.py                # 用默认的 MDK-ARM/Listings/CDC.map
    python tools/res_report.py <mapfile>      # 指定 map

输出:
    1. Flash / RAM 的总量、上限、余量、占用率
    2. Flash 里"代码 / 字符串 / 其它"的拆分
    3. 单文件 Flash 排行、RAM 大块排行
    4. 超限告警(尤其: RAM 上限按芯片实际 192KB 判定, 不看 scatter 里写的大数)

维护提示:
    - 预算口径与 docs/资源预算规划-20261112.md 一致, 改一边要同步另一边
    - 芯片 RAM 按 192 KB 硬编码, 换芯片要改 RAM_TOTAL
"""

import os
import re
import sys
from collections import defaultdict

FLASH_TOTAL = 0x38000          # Slot A 224 KB (闪存分区模式 2, 2026-11-12 重分区)
RAM_TOTAL = 192 * 1024         # 芯片实际 192 KB —— scatter 现在也已是 0x30000

# map 里明细行的格式: 地址 加载地址/COMPRESSED/- 长度 类型 属性 索引 段名 目标文件
ROW = re.compile(
    r"^\s+(0x[0-9a-f]{8})\s+(?:0x[0-9a-f]{8}|COMPRESSED|-)\s+"
    r"(0x[0-9a-f]{8})\s+(\w+)\s+(\w+)\s+(\d+)\s+(\S+)\s+(\S+)\s*$"
)
OBJ = re.compile(
    r"^\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\S+\.o)\s*$"
)


def kb(n):
    return n / 1024.0


def parse(path):
    lines = open(path, encoding="utf-8", errors="ignore").read().split("\n")

    def region(name):
        """返回 (起始行, 该执行域声明的字节数)"""
        for i, l in enumerate(lines):
            if "Execution Region " + name in l:
                m = re.search(r"Size:\s*0x([0-9a-f]+)", l)
                return i, (int(m.group(1), 16) if m else 0)
        return None, 0

    def grab(start):
        """只收本执行域的行, 遇到下一个 Execution Region 就停"""
        out = []
        for l in lines[start:]:
            if "Execution Region " in l and out:
                break
            m = ROW.match(l)
            if m:
                out.append((m.group(6), m.group(7), int(m.group(2), 16)))
        return out

    i_rom, size_rom = region("ER_IROM1")
    i_ram, size_ram = region("RW_IRAM1")
    rom = grab(i_rom) if i_rom is not None else []
    ram = grab(i_ram) if i_ram is not None else []

    # Image component sizes 段: Code / RO / RW / ZI
    objs = []
    for l in lines:
        m = OBJ.match(l)
        if m:
            objs.append((m.group(7), int(m.group(1)), int(m.group(3)), int(m.group(4)), int(m.group(5))))

    totals = {"ro_region": size_rom, "rw_region": size_ram}
    for l in lines:
        for key, tag in (("ro", "Total RO  Size"), ("rw", "Total RW  Size"), ("rom", "Total ROM Size")):
            if tag in l:
                mm = re.search(r"(\d+)\s*\(", l)
                if mm:
                    totals[key] = int(mm.group(1))
    return rom, ram, objs, totals


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "MDK-ARM", "Listings", "CDC.map")
    if not os.path.exists(path):
        print("找不到 map 文件: %s" % path)
        return 1

    rom, ram, objs, totals = parse(path)
    rom_used = totals.get("rom", sum(s for _, _, s in rom))
    ram_used = sum(s for _, _, s in ram)
    # ---- 1. 总量 ----
    print("=" * 62)
    print("资源账本  (%s)" % os.path.basename(path))
    print("=" * 62)
    print("%-6s %10s %10s %10s %8s %10s" % ("", "已用 B", "上限 B", "余量 B", "占用", "余量 KB"))
    for tag, used, cap in (("Flash", rom_used, FLASH_TOTAL), ("RAM", ram_used, RAM_TOTAL)):
        left = cap - used
        flag = "" if left >= 0 else "  <<< 超限"
        print("%-6s %10d %10d %10d %7.1f%% %10.1f%s"
              % (tag, used, cap, left, used * 100.0 / cap, kb(left), flag))
    print()

    # ---- 2. Flash 拆分 ----
    strings = sum(s for sec, _, s in rom if sec.startswith(".rodata.str"))
    text = sum(s for sec, _, s in rom if sec.startswith(".text"))
    other = totals.get("ro_region", len(rom)) - text - strings
    print("Flash 拆分:")
    for name, v in (("代码 .text", text), ("字符串", strings), ("其它(表/向量/库)", other)):
        print("   %-18s %8d B  %6.1f KB  %5.1f%%" % (name, v, kb(v), v * 100.0 / max(rom_used, 1)))
    print()

    # ---- 3. 单文件 Flash 排行 ----
    fobj = sorted(objs, key=lambda x: -(x[1] + x[2]))
    print("Flash 单文件 TOP 12 (Code+RO):")
    for name, code, ro, rw, zi in fobj[:12]:
        print("   %-24s %8d B  %6.1f KB  %5.1f%%" % (name, code + ro, kb(code + ro),
                                                     (code + ro) * 100.0 / max(rom_used, 1)))
    print()

    # ---- 4. RAM 大块 ----
    rlist = sorted(ram, key=lambda x: -x[2])
    print("RAM 大块 (>=1KB):")
    show = [r for r in rlist if r[2] >= 1024]
    for sec, obj, sz in show:
        print("   %8d B  %-22s %s" % (sz, (obj + " " + sec)[:22], obj))
    big = sum(r[2] for r in show)
    print("   ---- >=1KB 合计 %d B (%.1f KB), 占 RAM %.1f%%"
          % (big, kb(big), big * 100.0 / max(ram_used, 1)))
    print()

    # ---- 5. 告警 ----
    print("检查:")
    ok = True
    if rom_used > FLASH_TOTAL:
        print("   [!!] Flash 超限 %d B" % (rom_used - FLASH_TOTAL)); ok = False
    elif FLASH_TOTAL - rom_used < 8192:
        print("   [!!] Flash 余量不足 8 KB, 下一个功能很可能放不下"); ok = False
    if ram_used > RAM_TOTAL:
        print("   [!!] RAM 超过芯片实际的 192 KB, 上板会 HardFault"); ok = False
    elif RAM_TOTAL - ram_used < 8192:
        print("   [!!] RAM 余量不足 8 KB"); ok = False
    if ok:
        print("   [OK] 两项都在预算内")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
