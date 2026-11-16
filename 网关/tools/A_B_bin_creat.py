import os
import re
import shutil
import subprocess
import traceback
import hashlib

# ===================== 这里手动改 =====================
VERSION = "1.0.2"       # 版本号，跑之前自己改（末位奇数烧 B、偶数烧 A）
BUILD_SLOT = "A"        # 填 "A" 或者 "B"
# ======================================================

# 分区配置（唯一真相源是 app/common/flash_map.h 的 FM_MAP_DUAL_SLOT 分支）
# start_addr = 该槽的链接基址；vect_off = 相对 FLASH_BASE(0x08000000) 的偏移，
# 两者必须满足 vect_off = start_addr - 0x08000000，否则 App 的 VTOR 会指错。
# 运行期由 verify_slot_cfg() 直接读 flash_map.h 校验，改分区表后本表漏改会直接报错。
SLOT_CFG = {
    "A": {"start_addr": "0x8004000", "vect_off": "0x4000"},
    "B": {"start_addr": "0x803C000", "vect_off": "0x3C000"},
}
FLASH_BASE = 0x08000000

# 用途说明（2026-11-12 核对 bootloader.c 后更正，旧注释把两种用途混为一谈）：
#   A 变体：链接在 0x08004000，正常 App，也是 OTA 下发的载荷形态
#           （BL 把暂存区整段搬到 BL_APP_ADDR=A 槽后跳转，见 bl_copy_to_app）。
#   B 变体：链接在 0x0803C000，只用于【直接烧到 B 槽运行】的 A/B 测试
#           （bl_jump_app 在 A 向量表不可信时会 bl_jump_try(FM_SLOT_B_ADDR)）。
#   ⚠ B 变体【不能】当 OTA 载荷下发：BL 会把它搬到 A 槽，绝对地址引用全部失效。
#     要给平台下发 OTA 包，请用 BUILD_SLOT = "A"。

# 路径配置
# 本脚本放在 tools\ 下，仓库根是它的上一级；放在别处时退化为脚本所在目录。
_here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(_here) if os.path.basename(_here).lower() == "tools" else _here
proj_path = os.path.join(root, "MDK-ARM", "CDC.uvprojx")
bin_folder = os.path.join(root, "MDK-ARM", "bin")
src_bin = os.path.join(bin_folder, "CDC.bin")
uv4_path = r"D:\Keil_v5\UV4\UV4.exe"
log_path = os.path.join(os.environ["TEMP"], "gateway_build_ab.log")


def calc_md5(filepath):
    h = hashlib.md5()
    with open(filepath, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()


def read_flash_map():
    """读 app/common/flash_map.h 的模式 2 分支，返回真实的槽地址/尺寸。"""
    path = os.path.join(root, "app", "common", "flash_map.h")
    if not os.path.exists(path):
        raise RuntimeError("找不到分区表: %s" % path)
    with open(path, encoding="utf-8", errors="ignore") as f:
        text = f.read()
    m = re.search(r"#elif\s*\(FM_MAP_MODE\s*==\s*FM_MAP_DUAL_SLOT\)(.*?)#else",
                  text, re.S)
    if m is None:
        raise RuntimeError("flash_map.h 里找不到 FM_MAP_DUAL_SLOT 分支")
    seg = m.group(1)

    def macro(name):
        mm = re.search(r"#define\s+%s\s+(0x[0-9A-Fa-f]+)U?" % name, seg)
        if mm is None:
            raise RuntimeError("flash_map.h 模式 2 分支里找不到 %s" % name)
        return int(mm.group(1), 16)

    return {"A": macro("FM_SLOT_A_ADDR"),
            "B": macro("FM_SLOT_B_ADDR"),
            "SIZE": macro("FM_SLOT_SIZE")}


def verify_slot_cfg():
    """脚本里的槽地址必须与分区表一致 —— 用代码校验代替"注释里提醒同步改"。"""
    fm = read_flash_map()
    for slot in ("A", "B"):
        want_addr = fm[slot]
        want_vect = want_addr - FLASH_BASE
        got_addr = int(SLOT_CFG[slot]["start_addr"], 16)
        got_vect = int(SLOT_CFG[slot]["vect_off"], 16)
        if (got_addr, got_vect) != (want_addr, want_vect):
            raise RuntimeError(
                "SLOT_CFG[%s] 与 flash_map.h 不一致: 脚本 0x%X/0x%X, 分区表 0x%X/0x%X；"
                "改了 flash_map.h 请同步改本脚本" %
                (slot, got_addr, got_vect, want_addr, want_vect))
    return fm


def verify_project_rom_size(proj_text, slot_size):
    """两个 Target 的 IROM Size 必须等于 FM_SLOT_SIZE（本脚本只改起始地址，不改 Size）。"""
    sizes = re.findall(
        r"<OCR_RVCT4>\s*<Type>1</Type>\s*"
        r"<StartAddress>0x[0-9A-Fa-f]+</StartAddress>\s*"
        r"<Size>(0x[0-9A-Fa-f]+)</Size>", proj_text)
    if len(sizes) != 2:
        raise RuntimeError("工程里应有 2 个 Target 的 OCR_RVCT4 IROM Size，实际 %d 个" % len(sizes))
    bad = [s for s in sizes if int(s, 16) != slot_size]
    if bad:
        raise RuntimeError("Keil IROM Size %s 与 flash_map.h 的 FM_SLOT_SIZE 0x%X 不一致，"
                           "请先改 CDC.uvprojx 的两个 Target" % (bad, slot_size))


def main():
    if BUILD_SLOT not in SLOT_CFG:
        raise RuntimeError(f"BUILD_SLOT={BUILD_SLOT!r} 非法，只能是 {list(SLOT_CFG)}")

    fm = verify_slot_cfg()
    print("分区表校验 OK: A=0x%X  B=0x%X  槽大小=0x%X" %
          (fm["A"], fm["B"], fm["SIZE"]))
    if BUILD_SLOT == "B":
        print("⚠ B 变体只用于直接烧 B 槽运行，不能作为 OTA 载荷下发（见文件头说明）")
    cfg = SLOT_CFG[BUILD_SLOT]
    # OTA 目标目录: OTA\固件版本\A 或 B(编译成功后才创建, 失败不留空目录)
    ota_target_dir = os.path.join(root, "OTA", "固件版本", BUILD_SLOT)
    os.makedirs(bin_folder, exist_ok=True)

    if not os.path.exists(uv4_path):
        raise RuntimeError(f"找不到 UV4.exe: {uv4_path}，请改脚本顶部的 uv4_path")
    if not os.path.exists(proj_path):
        raise RuntimeError(f"找不到工程文件: {proj_path}")

    # 备份原始工程二进制
    with open(proj_path, "rb") as f:
        orig_bytes = f.read()
    bom = len(orig_bytes) >= 3 and orig_bytes[0] == 0xEF and orig_bytes[1] == 0xBB and orig_bytes[2] == 0xBF
    if bom:
        text = orig_bytes[3:].decode("utf-8")
    else:
        text = orig_bytes.decode("utf-8")
    encoding = "utf-8-sig" if bom else "utf-8"
    verify_project_rom_size(text, fm["SIZE"])

    try:
        t = text
        # 和原ps脚本完全一致的替换逻辑，不改动规则
        t = t.replace('<StartAddress>0x8004000</StartAddress>', f"<StartAddress>{cfg['start_addr']}</StartAddress>")
        t = t.replace('<StartAddress>0x8000000</StartAddress>', f"<StartAddress>{cfg['start_addr']}</StartAddress>")
        # 工程里残留的任何 VECT_TAB_OFFSET 旧值一律改成目标值
        # （原来是两条写死旧值的 replace，改分区表时极易漏改；正则不会漏）
        t = re.sub(r'VECT_TAB_OFFSET=0x[0-9A-Fa-f]+',
                   'VECT_TAB_OFFSET=%s' % cfg['vect_off'], t)

        verEsc = 'MAIN_FW_VERSION=\\"%s\\"' % VERSION
        if 'MAIN_FW_VERSION=' in t:
            # 工程里已经有这个宏(哪怕值是旧版本) -> 就地替换, 两个 target 都换
            t = re.sub(r'MAIN_FW_VERSION=\\?"[^"]*\\?"', lambda m: verEsc, t)
        else:
            t = t.replace('MAIN_OTA_AUTO_REBOOT=1</Define>',
                          'MAIN_OTA_AUTO_REBOOT=1,%s</Define>' % verEsc)

        # 替换必须真的生效, 否则宁可不编 —— 避免"编出来还是旧地址/旧版本"
        if '<StartAddress>%s</StartAddress>' % cfg["start_addr"] not in t:
            raise RuntimeError("IROM 起始地址替换失败: 工程结构与脚本预期不符")
        if 'VECT_TAB_OFFSET=%s' % cfg["vect_off"] not in t:
            raise RuntimeError("VECT_TAB_OFFSET 替换失败: 工程结构与脚本预期不符")
        if verEsc not in t:
            raise RuntimeError("MAIN_FW_VERSION 替换失败: 工程结构与脚本预期不符")

        # 写回临时修改后的工程
        with open(proj_path, "w", encoding=encoding) as f:
            f.write(t)

        # 清理旧日志、旧bin
        if os.path.exists(log_path):
            os.remove(log_path)
        if os.path.exists(src_bin):
            os.remove(src_bin)

        # 调用UV4编译，和原来参数一样
        proc = subprocess.Popen(
            [uv4_path, "-j0", "-b", proj_path, "-o", log_path],
            cwd=os.path.dirname(proj_path),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE
        )
        # communicate 会持续把管道读空, 避免 UV4 输出过多写满管道导致死锁
        try:
            out, err = proc.communicate(timeout=600)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.communicate()
            raise RuntimeError("UV4 编译超时(600 s), 已强制终止")
        console = (out or b"").decode("utf-8", "ignore") + (err or b"").decode("utf-8", "ignore")

        # UV4 退出码: 0=无错无警告, 1=有警告, >=2=有错误
        if proc.returncode >= 2:
            with open(log_path, "r", encoding="utf-8", errors="ignore") as f:
                lines = f.readlines()
                print("==== Compile log tail ====")
                print("".join(lines[-20:]))
            if console.strip():
                print("==== UV4 console tail ====")
                print("\n".join(console.splitlines()[-20:]))
            raise RuntimeError(f"{BUILD_SLOT} build failed, UV4 exit {proc.returncode}")
        if proc.returncode == 1:
            print("⚠ UV4 返回 1(有警告), 固件已生成; 需要零警告请查看编译日志")

        if not os.path.exists(src_bin):
            raise RuntimeError(f"编译成功但没找到 {src_bin}, 检查 fromelf 后置命令")

        # 同时留两份: OTA 目录(给平台下发) + MDK-ARM\bin(本地烧录)
        name = f"CDC_{BUILD_SLOT}_{VERSION}.bin"
        dst_ota = os.path.join(ota_target_dir, name)
        dst_local = os.path.join(bin_folder, name)
        os.makedirs(ota_target_dir, exist_ok=True)
        shutil.copy2(src_bin, dst_ota)
        shutil.copy2(src_bin, dst_local)
        size = os.path.getsize(dst_ota)
        md5sum = calc_md5(dst_ota)

        print(f"✅ {BUILD_SLOT}固件编译完成")
        print(f"OTA 目录: {dst_ota}")
        print(f"本地备份: {dst_local}")
        print(f"固件大小: {size} bytes")
        print(f"MD5校验: {md5sum}")

    finally:
        # 强制恢复原始工程文件，和原ps逻辑一致
        with open(proj_path, "wb") as f:
            f.write(orig_bytes)
        print(f"\n✅ 工程文件已恢复原状")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        traceback.print_exc()
        exit(1)
