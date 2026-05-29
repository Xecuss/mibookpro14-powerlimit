#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
小米笔记本 Linux 性能模式探测脚本 (验证用)
通过 acpi_call 调用 WMAA，使用 FUN2=0x0800 子系统

WMI GUID: B60BFB48-3E5B-49E4-A0E9-8CFFE1B3434B
ACPI 方法: \\_SB.PC00.WMID.WMAA  (InstanceID=0, MethodID=1)

协议发现来源: Meow-Box 项目 (GPL-3.0, https://github.com/leehyukshuai/Meow-Box)
  BatteryControlService.cs 中的 FUN2=0x0800 调用及模式码映射
  本脚本未复制任何 Meow-Box 代码，仅使用其揭示的硬件协议参数

协议要点（与充电接口的区别）:
  GET: FUN1=0xFA00, FUN2=0x0800, FUN3=0x0000, FUN4=0
       → 当前模式码在返回缓冲 offset 4 处 (ushort, 即 Data0)
  SET: FUN1=0xFB00, FUN2=0x0800, FUN3=<模式码>, FUN4=0
       → 模式码放在 FUN3，FUN4 恒为 0

用法:
  sudo python3 test_perf_mode_wmi.py get          # 读取当前性能模式
  sudo python3 test_perf_mode_wmi.py set smart     # 设置为智能模式
  sudo python3 test_perf_mode_wmi.py set silent    # 设置为静谧模式
  sudo python3 test_perf_mode_wmi.py set battery   # 设置为省电模式
  sudo python3 test_perf_mode_wmi.py set turbo     # 设置为极速模式
  sudo python3 test_perf_mode_wmi.py set beast     # 设置为狂暴模式
  sudo python3 test_perf_mode_wmi.py dump          # 原始 GET 响应全量 dump（用于验证 offset）

依赖: acpi_call 内核模块已加载
  sudo modprobe acpi_call
"""

import sys
import struct
import os

ACPI_METHOD  = r"\_SB.PC00.WMID.WMAA"  # noqa: W605 — raw string, backslash is literal
ACPI_CALL_DEV = "/proc/acpi/call"

# 性能模式名 → FUN3 模式码（来自 Meow-Box BatteryControlCatalog.GetPerformanceRawCode）
PERF_MODE_MAP: dict[str, int] = {
    "battery": 0x000A,   # 省电 / Battery Saver
    "silent":  0x0002,   # 静谧
    "smart":   0x0009,   # 智能（默认）
    "turbo":   0x0003,   # 极速
    "beast":   0x0004,   # 狂暴
}

# 反向映射（GET 响应解析用）
CODE_TO_MODE: dict[int, str] = {v: k for k, v in PERF_MODE_MAP.items()}


# ---------------------------------------------------------------------------
# 缓冲构造
# ---------------------------------------------------------------------------

def build_get_buf() -> bytes:
    """
    GET 查询：FUN1=0xFA00, FUN2=0x0800, FUN3=0x0000, FUN4=0
    期望返回：当前模式码在 offset 4 (ushort, Data0)
    """
    return struct.pack("<HHHi", 0xFA00, 0x0800, 0x0000, 0)


def build_set_buf(mode_code: int) -> bytes:
    """
    SET 写入：FUN1=0xFB00, FUN2=0x0800, FUN3=<模式码>, FUN4=0
    注意：模式码在 FUN3，与充电接口的 FUN4 位置不同
    """
    return struct.pack("<HHHi", 0xFB00, 0x0800, mode_code, 0)


# ---------------------------------------------------------------------------
# acpi_call 通信
# ---------------------------------------------------------------------------

def buf_to_acpi_args(buf: bytes) -> str:
    hex_vals = ",".join(f"0x{b:02X}" for b in buf)
    return "{" + hex_vals + "}"


def call_acpi(buf: bytes) -> str:
    if not os.path.exists(ACPI_CALL_DEV):
        raise RuntimeError(
            f"{ACPI_CALL_DEV} 不存在，请先加载模块：\n"
            "  sudo modprobe acpi_call"
        )
    cmd = f"{ACPI_METHOD} 0 1 {buf_to_acpi_args(buf)}"
    with open(ACPI_CALL_DEV, "w") as f:
        f.write(cmd)
    with open(ACPI_CALL_DEV, "r") as f:
        return f.read().strip().rstrip("\x00").strip()


def parse_result(result: str) -> bytes | None:
    if result.startswith("{") and result.endswith("}"):
        inner = result[1:-1]
        try:
            parts = [p.strip() for p in inner.split(",") if p.strip()]
            return bytes(int(p, 16) for p in parts)
        except ValueError:
            return None
    return None


# ---------------------------------------------------------------------------
# 命令实现
# ---------------------------------------------------------------------------

def cmd_get() -> str | None:
    buf = build_get_buf()
    print(f"[GET] 调用: {ACPI_METHOD} 0 1 {buf_to_acpi_args(buf)}")

    result = call_acpi(buf)
    print(f"[GET] 原始返回: {result}")

    rbuf = parse_result(result)
    if rbuf is None or len(rbuf) < 6:
        print(f"[GET] 无法解析返回缓冲（长度={len(rbuf) if rbuf else 0}，需要至少 6 字节）")
        return None

    sger   = struct.unpack_from("<H", rbuf, 0)[0]   # offset 0: 状态字
    data0  = struct.unpack_from("<H", rbuf, 4)[0]   # offset 4: 当前模式码 (Data0)

    ok_str = "[OK]" if sger == 0x8000 else f"[FAIL SGER=0x{sger:04X}]"
    mode   = CODE_TO_MODE.get(data0)
    mode_str = mode if mode else f"未知 (0x{data0:04X})"

    print(f"[GET] {ok_str}  SGER=0x{sger:04X}  Data0(offset4)=0x{data0:04X}  → 性能模式: {mode_str}")
    return mode


def cmd_set(mode_name: str):
    mode_name = mode_name.lower()
    if mode_name not in PERF_MODE_MAP:
        print(f"错误：未知模式 '{mode_name}'")
        print(f"支持: {list(PERF_MODE_MAP.keys())}")
        sys.exit(1)

    code = PERF_MODE_MAP[mode_name]
    buf  = build_set_buf(code)

    print(f"[SET] 目标模式: {mode_name}  FUN3(模式码)=0x{code:04X}")
    print(f"[SET] 调用: {ACPI_METHOD} 0 1 {buf_to_acpi_args(buf)}")

    result = call_acpi(buf)
    print(f"[SET] 原始返回: {result}")

    rbuf = parse_result(result)
    if rbuf and len(rbuf) >= 2:
        sger = struct.unpack_from("<H", rbuf, 0)[0]
        if sger == 0x8000:
            print(f"[SET] [OK] 写入成功  SGER=0x{sger:04X}")
        else:
            print(f"[SET] [FAIL] SGER=0x{sger:04X}（预期 0x8000）")
            return
    else:
        print("[SET] 无法解析返回缓冲")
        return

    # 回读验证
    print()
    current = cmd_get()
    if current == mode_name:
        print(f"[SET] 验证通过：性能模式已确认为 '{mode_name}'")
    elif current is not None:
        print(f"[SET] 警告：回读模式 '{current}' 与期望 '{mode_name}' 不符")


def cmd_dump():
    """
    原始响应全量 dump，用于验证各 offset 的含义。
    同时也探测 Data1（offset 6）是否含有有效数据，
    以防模式码实际在 frd1 而不是 Data0。
    """
    buf = build_get_buf()
    print(f"[DUMP] 调用: {ACPI_METHOD} 0 1 {buf_to_acpi_args(buf)}")

    result = call_acpi(buf)
    print(f"[DUMP] 原始返回: {result}\n")

    rbuf = parse_result(result)
    if rbuf is None:
        print("[DUMP] 无法解析为字节数组")
        return

    print(f"[DUMP] 返回缓冲长度: {len(rbuf)} 字节")
    print(f"[DUMP] 十六进制: {' '.join(f'{b:02X}' for b in rbuf)}\n")

    # 按 Meow-Box ParseResponse 的 offset 解析
    if len(rbuf) >= 2:
        sger = struct.unpack_from("<H", rbuf, 0)[0]
        print(f"  offset 0 (Status / SGER):         0x{sger:04X}  {'← OK' if sger == 0x8000 else '← FAIL'}")
    if len(rbuf) >= 4:
        func = struct.unpack_from("<H", rbuf, 2)[0]
        print(f"  offset 2 (Function echo):         0x{func:04X}")
    if len(rbuf) >= 6:
        data0 = struct.unpack_from("<H", rbuf, 4)[0]
        mode0 = CODE_TO_MODE.get(data0, "未知")
        print(f"  offset 4 (Data0 / 性能模式候选):  0x{data0:04X}  → '{mode0}'")
    if len(rbuf) >= 10:
        data1 = struct.unpack_from("<I", rbuf, 6)[0]
        mode1 = CODE_TO_MODE.get(data1, "未知")
        print(f"  offset 6 (Data1 / frd1 候选):     0x{data1:08X}  → '{mode1}'")
    if len(rbuf) >= 14:
        data2 = struct.unpack_from("<I", rbuf, 10)[0]
        print(f"  offset 10 (Data2):                0x{data2:08X}")
    if len(rbuf) >= 18:
        data3 = struct.unpack_from("<I", rbuf, 14)[0]
        print(f"  offset 14 (Data3):                0x{data3:08X}")

    print()
    print("[DUMP] 提示：若 offset 4 命中已知模式，则 Data0 解析正确（与 Meow-Box 一致）")
    print("[DUMP] 若 offset 6 命中，则驱动需改为读 frd1（与充电接口相同位置）")


# ---------------------------------------------------------------------------
# 入口
# ---------------------------------------------------------------------------

def main():
    if os.geteuid() != 0:
        print("错误：需要 root 权限（sudo）")
        sys.exit(1)

    if len(sys.argv) < 2:
        cmd_get()
        print()
        print(f"用法: {sys.argv[0]} get | set <模式> | dump")
        print(f"模式: {list(PERF_MODE_MAP.keys())}")
        return

    cmd = sys.argv[1].lower()

    if cmd == "get":
        cmd_get()
    elif cmd == "set":
        if len(sys.argv) < 3:
            print(f"用法: {sys.argv[0]} set <模式>")
            print(f"模式: {list(PERF_MODE_MAP.keys())}")
            sys.exit(1)
        cmd_set(sys.argv[2])
    elif cmd == "dump":
        cmd_dump()
    else:
        print(f"未知命令: {cmd}")
        print(f"支持: get, set <模式>, dump")
        sys.exit(1)


if __name__ == "__main__":
    main()
