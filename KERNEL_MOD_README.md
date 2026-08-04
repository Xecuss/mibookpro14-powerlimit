# 小米笔记本 Linux 内核模块使用说明

`xiaomi-wmi-battery` 是一个原生 Linux 内核驱动，通过 WMI 接口直接管理小米笔记本的充电阈值与性能模式。与 Python 脚本方案相比，内核模块集成于标准 Linux 电源管理子系统，无需额外依赖，开机后即可通过 sysfs 访问。

> **AI 声明**：本文档由 **Claude Sonnet 4.6 High** 编写，由 **GPT 5.4 xHigh** 审核。

---

## ⚠️ 免责声明

**使用本内核模块前，请仔细阅读以下风险说明：**

### 内核模块风险

- 内核模块运行在内核空间（Ring 0），代码错误可能导致**系统立即崩溃（Kernel Panic）**或数据丢失。
- 加载非官方内核模块可能违反发行版的支持条款，并在某些情况下触发 **Secure Boot 拒绝加载**。
- 本模块仅在 **小米 Book Pro 14** 上经过验证；在其他机型上加载可能向 EC（嵌入式控制器）写入错误数据，导致**不可预期的硬件行为**，包括但不限于充电异常、风扇失控等。
- 内核 API 不保证版本兼容性，模块在内核更新后可能需要重新编译。

### ACPI / EC 写入风险

- 本模块通过 WMI 方法直接与 EC 通信，属于底层硬件操作。若平台固件（ACPI DSDT/SSDT）的行为与逆向分析结果不一致，错误写入可能导致**电池保护逻辑失效**，长期使用存在损坏电池的风险。
- `force=1` 参数绕过 DMI 白名单检查，在未经验证的机型上使用属于**高风险操作，后果自负**。
- 充电阈值由 EC 在运行时执行，**系统断电或休眠后不持久化**；每次启动后需重新应用。

**本模块按"现状"（AS IS）提供，作者不承担任何因使用本模块造成的损失。**

---

## Meow-Box 分析说明

性能模式协议（`/sys/firmware/acpi/platform_profile`）的 WMI 子路径（`FUN2=0x0800`）由开源项目 **[Meow-Box](https://github.com/leehyukshuai/Meow-Box)** 首先发现，其逆向分析成果为本模块的性能模式实现提供了关键参考。

| 项目 | 说明 |
|------|------|
| 项目名 | Meow-Box |
| 许可证 | GPL-3.0 |
| 仓库 | [github.com/leehyukshuai/Meow-Box](https://github.com/leehyukshuai/Meow-Box) |
| 贡献内容 | 性能模式 WMI 调用格式（FUN2=0x0800）及模式码（battery/silent/smart/turbo/beast） |

本模块在 Meow-Box 分析的基础上，通过 `acpi_call` 在真实硬件上逐一验证了各模式码，并将其封装为标准 Linux `platform_profile` 接口。

---

## 适用机型

**小米 Book Pro 14**（已通过 DMI 验证）

模块内置 DMI 白名单，仅在以下条件同时满足时才允许加载：

```
DMI_SYS_VENDOR   = "XIAOMI"
DMI_PRODUCT_NAME = "Xiaomi Book Pro 14"
```

其他机型需传入 `force=1` 参数（见下文）。

---

## 暴露的系统接口

### 充电阈值

```
/sys/class/power_supply/BAT0/charge_control_end_threshold
```

- **读取**：返回当前充电上限百分比
- **写入**：设置充电上限，仅支持以下离散值：`40 50 60 70 80 90 100`
- WMI 协议为平台级控制，无电池编号参数；Xiaomi Book Pro 14 仅有 BAT0，不存在多电池歧义

### 性能模式

```
/sys/firmware/acpi/platform_profile
/sys/firmware/acpi/platform_profile_choices
```

| `platform_profile` 值 | 固件模式 | 固件模式码 | 说明 |
|-----------------------|---------|-----------|------|
| `low-power`           | battery | `0x000A`  | 最低 CPU/GPU 限制，风扇静音 |
| `quiet`               | silent  | `0x0002`  | 低转速风扇，适度性能限制 |
| `balanced`            | smart   | `0x0009`  | 固件自动调节（默认） |
| `performance`         | turbo   | `0x0003`  | 最高持续性能，主动散热 |

> **注意**：固件还存在 `beast` 模式（`0x0004`，纯 AC 供电超频变体）。该模式不作为可写选项暴露，但回读时会映射为 `performance`，避免读取失败。

---

## 构建与安装

### 前置条件

```bash
# Debian / Ubuntu
sudo apt install linux-headers-$(uname -r) build-essential

# Fedora
sudo dnf install kernel-devel-$(uname -r) gcc make

# Arch Linux
sudo pacman -S linux-headers base-devel
```

### 一次性构建与加载（不持久）

```bash
git clone <repo-url>
cd xiaomi-power-test

make
sudo insmod xiaomi-wmi-battery.ko
```

验证加载成功：

```bash
dmesg | tail -20
# 应出现类似：
# xiaomi-wmi-battery: Xiaomi WMI ready (charge threshold: 100%, platform_profile enabled)
```

### 通过 DKMS 持久安装（推荐）

DKMS 会在内核更新后自动重新编译模块。

```bash
# 安装 DKMS
sudo apt install dkms          # Debian/Ubuntu
sudo dnf install dkms          # Fedora

# 复制源码到 DKMS 管理目录
sudo cp -r . /usr/src/xiaomi-wmi-battery-1.0

# 注册并构建
sudo dkms add     xiaomi-wmi-battery/1.0
sudo dkms build   xiaomi-wmi-battery/1.0
sudo dkms install xiaomi-wmi-battery/1.0

# 验证
sudo dkms status
# 应显示：xiaomi-wmi-battery/1.0, $(uname -r), x86_64: installed
```

### 在 Fedora（启用 Secure Boot）上的注意事项

Fedora 内核默认启用模块签名验证。使用自编译模块时需要：

1. **方法 A（推荐）**：向 MOK（Machine Owner Key）注册自签名证书

   ```bash
   # 密钥存放目录，可自定义
   MOK_DIR="$HOME/mok-keys"

   # 生成密钥（如尚未生成）
   mkdir -p "$MOK_DIR"
   openssl req -new -x509 -newkey rsa:2048 \
       -keyout "$MOK_DIR/MOK.priv" \
       -outform DER -out "$MOK_DIR/MOK.der" \
       -nodes -days 3650 -subj "/CN=Local Module Signing/"

   # 注册到 MOK
   sudo mokutil --import "$MOK_DIR/MOK.der"
   # 此处会提示输入一个动态密码，请牢记该密码。
   # 重启系统后，会出现 MOK 管理界面（蓝屏的 Shim UEFI key management 界⾯）：
   # 1. 选择 "Enroll MOK"
   # 2. 选择 "Continue"
   # 3. 选择 "Yes"
   # 4. 输入刚才设置的密码进行确认，然后重启。

   # 对已安装模块签名
   sudo /usr/src/kernels/$(uname -r)/scripts/sign-file \
       sha256 "$MOK_DIR/MOK.priv" "$MOK_DIR/MOK.der" \
       /lib/modules/$(uname -r)/updates/xiaomi-wmi-battery.ko
   ```

2. **方法 B**：在 BIOS 中关闭 Secure Boot

### 手动安装（不使用 DKMS）

```bash
make
sudo make install     # 安装到 /lib/modules/$(uname -r)/updates/
sudo depmod -a
sudo modprobe xiaomi-wmi-battery
```

---

## 开机自动加载

```bash
echo "xiaomi-wmi-battery" | sudo tee /etc/modules-load.d/xiaomi-wmi-battery.conf
```

---

## 使用示例

### 充电阈值操作

```bash
# 读取当前充电上限
cat /sys/class/power_supply/BAT0/charge_control_end_threshold

# 设置为 80%（推荐，兼顾续航与电池健康）
echo 80 | sudo tee /sys/class/power_supply/BAT0/charge_control_end_threshold

# 恢复无限制
echo 100 | sudo tee /sys/class/power_supply/BAT0/charge_control_end_threshold
```

支持的值：`40 50 60 70 80 90 100`

### 性能模式操作

```bash
# 查看当前模式
cat /sys/firmware/acpi/platform_profile

# 查看所有可用模式
cat /sys/firmware/acpi/platform_profile_choices

# 切换到性能模式
echo performance | sudo tee /sys/firmware/acpi/platform_profile

# 切换到节能模式
echo low-power | sudo tee /sys/firmware/acpi/platform_profile
```

### 模块参数

```bash
# 在未列入 DMI 白名单的机型上强制加载（高风险）
sudo modprobe xiaomi-wmi-battery force=1
```

---

## 与 Python 脚本方案的对比

| 特性 | 内核模块 | Python 脚本（`set_charging_wmi.py`） |
|------|---------|--------------------------------------|
| 依赖 | 内核头文件 | `acpi_call` 内核模块、Python 3 |
| 接口 | 标准 sysfs（可被 TLP、GNOME 等工具识别） | 命令行手动调用 |
| 性能模式支持 | ✅ `platform_profile` | ❌ 不支持 |
| root 权限 | 仅加载时需要，写操作可通过 udev 规则放开 | 每次操作均需 sudo |
| 适用场景 | 长期使用、系统集成 | 调试、一次性验证 |

---

## 卸载

```bash
# 临时卸载（不影响 DKMS 注册）
sudo modprobe -r xiaomi-wmi-battery

# 通过 DKMS 完全移除
sudo dkms remove xiaomi-wmi-battery/1.0 --all
sudo rm -rf /usr/src/xiaomi-wmi-battery-1.0

# 移除自动加载配置
sudo rm -f /etc/modules-load.d/xiaomi-wmi-battery.conf
sudo depmod -a
```

---

## 许可

本仓库采用**按文件分许可**（per-file licensing）方式，两种许可证之间不存在冲突：

| 文件 | 许可证 | 原因 |
|------|--------|------|
| `xiaomi-wmi-battery.c` | **GPL-2.0** | Linux 内核模块硬性要求：模块需链接内核导出符号（如 `wmidev_evaluate_method`、`devm_battery_hook_register` 等），这些符号均为 GPL-only export；若不声明 GPL，`insmod` 会拒绝加载并报 `loading out-of-tree module taints kernel`。 |
| `set_charging_wmi.py`、`test_perf_mode_wmi.py`、文档等 | **MIT** | 纯用户态代码，与内核无链接关系，可自由选择许可证。 |

MIT 是宽松型许可证，其授权条款与 GPL-2.0 兼容（MIT 代码可被 GPL 项目引用，反之则不然）。两个组件作为独立文件共存于同一仓库，符合常见开源实践（参考 Linux 内核树本身对非内核工具的处理方式）。

性能模式协议参考自 [Meow-Box](https://github.com/leehyukshuai/Meow-Box)（GPL-3.0）。
