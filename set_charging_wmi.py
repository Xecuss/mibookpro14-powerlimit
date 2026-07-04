#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
小米笔记本 Linux 充电阈值控制脚本
通过 acpi_call 内核模块调用 ACPI WMI 方法 WMAA

WMI GUID: B60BFB48-3E5B-49E4-A0E9-8CFFE1B3434B
ACPI 方法: \\_SB.PC00.WMID.WMAA
来源: acpi/ssdt25.dsl (XMCC XMCC1806, 已反编译验证)

EC 寄存器映射（由 WMAA 写入）:
  LONL (bit 0): 充电限制使能标志
  HBDA: 充电上限百分比（十进制直接值）

用法:
  sudo python3 set_charging_wmi.py set 80    # 设置 80% 上限
  sudo python3 set_charging_wmi.py set 100   # 关闭限制（100%）
  sudo python3 set_charging_wmi.py get       # 读取当前阈值
  sudo python3 set_charging_wmi.py sync      # 恢复上次保存的充电限制
  sudo python3 set_charging_wmi.py test      # 测试 acpi_call 是否可用

依赖: acpi-call-dkms 已安装并加载
  sudo apt install acpi-call-dkms  (Debian/Ubuntu)
  sudo pacman -S acpi_call          (Arch)
  sudo modprobe acpi_call
"""

import sys
import struct
import os

ACPI_METHOD = r"\_SB.PC00.WMID.WMAA"
ACPI_CALL_DEV = "/proc/acpi/call"
CHARGING_LIMIT_STATE_FILE = "/var/lib/xiaomi-charging-limit"

# 充电阈值模式码表（与 ssdt25.dsl WMAA Case(0xFB00)/Case(0x1000)/Case(0x02) 对应）
THRESHOLD_MAP = {
    100: 0x00,  # 关闭限制（LONL bit0=0, HBDA=0x64）
    90:  0x04,  # HBDA=0x5A
    80:  0x01,  # HBDA=0x50
    70:  0x05,  # HBDA=0x46
    60:  0x06,  # HBDA=0x3C
    50:  0x07,  # HBDA=0x32
    40:  0x08,  # HBDA=0x28
}

# 反向映射（读取用）
MODE_TO_PCT = {v: k for k, v in THRESHOLD_MAP.items()}


def build_set_buf(fun4: int) -> bytes:
    """
    构造 WMAA 写入充电阈值的 10 字节输入缓冲
      FUN1=0xFB00 (写)  FUN2=0x1000 (电池子系统)
      FUN3=0x0002 (充电阈值)  FUN4=模式码
    """
    return struct.pack("<HHHi", 0xFB00, 0x1000, 0x0002, fun4)


def build_get_buf() -> bytes:
    """
    构造 WMAA 读取充电阈值的 10 字节输入缓冲
      FUN1=0xFA00 (读)  FUN2=0x1000  FUN3=0x0002  FUN4=0（不使用）
    """
    return struct.pack("<HHHi", 0xFA00, 0x1000, 0x0002, 0)


def buf_to_acpi_args(buf: bytes) -> str:
    """将字节缓冲转换为 acpi_call 的 Buffer 参数格式"""
    hex_vals = ",".join(f"0x{b:02X}" for b in buf)
    return "{" + hex_vals + "}"


def call_acpi(method: str, instance: int, method_id: int, buf: bytes) -> str:
    """
    通过 /proc/acpi/call 调用 ACPI 方法
    写入格式: METHOD Instance MethodID Buffer
    """
    args = buf_to_acpi_args(buf)
    cmd = f"{method} {instance} {method_id} {args}"

    if not os.path.exists(ACPI_CALL_DEV):
        raise RuntimeError(
            f"{ACPI_CALL_DEV} 不存在。请先安装并加载 acpi_call 模块：\n"
            "  sudo modprobe acpi_call"
        )

    with open(ACPI_CALL_DEV, "w") as f:
        f.write(cmd)

    with open(ACPI_CALL_DEV, "r") as f:
        result = f.read().strip()

    return result


def parse_result(result: str) -> bytes | None:
    """尝试将 acpi_call 返回值解析为字节（返回缓冲 RETS）"""
    # acpi_call 对 Buffer 返回的格式为 { 0xXX, 0xXX, ... }
    # strip() 不会移除 null 字节，需单独处理（acpi_call 返回 C 风格 null 结尾字符串）
    result = result.strip().rstrip('\x00').strip()
    if result.startswith("{") and result.endswith("}"):
        inner = result[1:-1]
        try:
            parts = [p.strip() for p in inner.split(",") if p.strip()]
            return bytes(int(p, 16) for p in parts)
        except ValueError:
            return None
    return None


def cmd_set(pct: int):
    if pct not in THRESHOLD_MAP:
        valid = sorted(THRESHOLD_MAP.keys(), reverse=True)
        print(f"错误：不支持的百分比 {pct}%")
        print(f"支持的值：{valid}")
        sys.exit(1)

    fun4 = THRESHOLD_MAP[pct]
    buf = build_set_buf(fun4)

    print(f"[SET] 充电上限 → {pct}%  (FUN4=0x{fun4:02X})")
    print(f"[SET] ACPI 调用: {ACPI_METHOD} 0 1 {buf_to_acpi_args(buf)}")

    result = call_acpi(ACPI_METHOD, 0, 1, buf)
    print(f"[SET] 返回值: {result}")

    # 检查 SGER 字段是否为 0x8000（成功）
    rbuf = parse_result(result)
    if rbuf and len(rbuf) >= 2:
        sger = struct.unpack_from("<H", rbuf, 0)[0]
        if sger == 0x8000:
            print(f"[SET] [OK] 写入成功  SGER=0x{sger:04X}")
        else:
            print(f"[SET] [FAIL] SGER=0x{sger:04X}（预期 0x8000）")
            return
    else:
        print("[SET] 无法解析返回缓冲，请手动检查结果")
        return

    # 保存状态到文件
    try:
        with open(CHARGING_LIMIT_STATE_FILE, "w") as f:
            f.write(str(pct))
        print(f"[SET] 状态已保存至 {CHARGING_LIMIT_STATE_FILE}")
    except Exception as e:
        print(f"[SET] 警告：无法保存状态: {e}")

    # 自动回读验证
    print()
    current = cmd_get()
    if current == pct:
        print(f"[SET] 验证通过：充电上限已确认为 {pct}%")
    elif current is not None:
        print(f"[SET] 警告：回读值 {current}% 与期望 {pct}% 不符")


def cmd_get() -> int | None:
    """读取当前充电阈值，返回百分比（失败返回 None）"""
    buf = build_get_buf()

    print(f"[GET] ACPI 调用: {ACPI_METHOD} 0 1 {buf_to_acpi_args(buf)}")

    result = call_acpi(ACPI_METHOD, 0, 1, buf)
    print(f"[GET] 返回值: {result}")

    rbuf = parse_result(result)
    if rbuf and len(rbuf) >= 10:
        sger = struct.unpack_from("<H", rbuf, 0)[0]
        frd1 = struct.unpack_from("<I", rbuf, 6)[0]
        ok = "[OK]" if sger == 0x8000 else f"[FAIL 0x{sger:04X}]"
        pct = MODE_TO_PCT.get(frd1)
        pct_str = f"{pct}%" if pct is not None else f"未知 (FRD1=0x{frd1:02X})"
        print(f"[GET] {ok}  当前充电上限: {pct_str}")
        return pct
    else:
        print("[GET] 无法解析返回缓冲")
        return None


def cmd_test():
    print("[TEST] 检查 acpi_call 是否可用...")
    if not os.path.exists(ACPI_CALL_DEV):
        print(f"[TEST] ❌ {ACPI_CALL_DEV} 不存在")
        print("[TEST] 请执行: sudo modprobe acpi_call")
        sys.exit(1)

    # 用 GET 命令做一次无副作用的测试调用
    buf = build_get_buf()
    try:
        result = call_acpi(ACPI_METHOD, 0, 1, buf)
        print(f"[TEST] ✅ acpi_call 可用，测试结果: {result}")
    except Exception as e:
        print(f"[TEST] ❌ 调用失败: {e}")
        sys.exit(1)


def cmd_sync():
    """读取保存的充电限制状态，恢复设置"""
    if not os.path.exists(CHARGING_LIMIT_STATE_FILE):
        print(f"[SYNC] 未找到状态文件 {CHARGING_LIMIT_STATE_FILE}")
        print("[SYNC] 请先使用 'set' 命令设置充电限制")
        sys.exit(1)

    try:
        with open(CHARGING_LIMIT_STATE_FILE, "r") as f:
            content = f.read().strip()
        pct = int(content)
        print(f"[SYNC] 读取保存的充电限制: {pct}%")
    except ValueError:
        print(f"[SYNC] 错误：状态文件内容无效: {content}")
        sys.exit(1)
    except Exception as e:
        print(f"[SYNC] 错误：无法读取状态文件: {e}")
        sys.exit(1)

    # 调用 cmd_set 恢复设置
    print(f"[SYNC] 正在恢复充电限制...")
    cmd_set(pct)


def main():
    if os.geteuid() != 0:
        print("错误：需要 root 权限（sudo）")
        sys.exit(1)

    valid = sorted(THRESHOLD_MAP.keys(), reverse=True)

    if len(sys.argv) < 2:
        # 无参数：直接读取当前值
        cmd_get()
        print()
        print(f"用法: {sys.argv[0]} set <百分比>  （可选值: {valid}）")
        return

    cmd = sys.argv[1].lower()

    if cmd == "set":
        if len(sys.argv) < 3:
            print(f"用法: set_charging_wmi.py set <百分比>")
            print(f"支持: {valid}")
            sys.exit(1)
        try:
            pct = int(sys.argv[2])
        except ValueError:
            print(f"错误：'{sys.argv[2]}' 不是有效的整数")
            sys.exit(1)
        cmd_set(pct)

    elif cmd == "get":
        cmd_get()

    elif cmd == "sync":
        cmd_sync()

    elif cmd == "test":
        cmd_test()

    else:
        # 直接传数字也视为 set
        try:
            pct = int(sys.argv[1])
            cmd_set(pct)
        except ValueError:
            print(f"未知命令: {cmd}")
            print(f"用法: get | set <百分比> | sync | test")
            print(f"支持百分比: {valid}")
            sys.exit(1)


if __name__ == "__main__":
    main()
