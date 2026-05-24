# 小米笔记本 Linux 充电阈值控制

通过直接调用 ACPI WMI 方法，在 Linux 下控制小米笔记本充电上限。

> **免责声明**：本脚本仅在作者自己的 **小米 Book Pro 14** 上验证通过。在其他机型上使用可能导致不可预期的行为，使用风险自负。
>
> **AI 声明**：本项目的逆向工程分析由 **Claude Sonnet 4.6** 完成。

---

## 适用机型

**小米 Book Pro 14**（已验证）

其他搭载相同 EC 固件的小米笔记本可能也适用，但未经测试。

---

## WMI 接口说明

### WMI 设备

| 字段 | 值 |
|------|----|
| **GUID** | `B60BFB48-3E5B-49E4-A0E9-8CFFE1B3434B` |
| **ACPI 路径** | `\_SB.PC00.WMID.WMAA` |
| **Instance** | `0` |
| **MethodID** | `1` |

### 输入缓冲格式（10 字节，小端序）

```
偏移  大小   字段   说明
0x00  2字节  FUN1   功能大类：0xFB00 = 写入 / 0xFA00 = 读取
0x02  2字节  FUN2   子系统：0x1000 = 电池
0x04  2字节  FUN3   功能编号：0x0002 = 充电阈值
0x06  4字节  FUN4   参数值（模式码，见下表）
```

Python 构造方式：
```python
import struct
buf = struct.pack("<HHHi", FUN1, 0x1000, 0x0002, FUN4)
```

### 充电模式码（FUN4）

| FUN4 | 充电上限 | EC 寄存器 LONL bit0 | EC 寄存器 HBDA |
|------|---------|---------------------|----------------|
| `0x00` | 100%（关闭限制） | 0 | 100 |
| `0x04` | 90% | 1 | 90 |
| `0x01` | 80% | 1 | 80 |
| `0x05` | 70% | 1 | 70 |
| `0x06` | 60% | 1 | 60 |
| `0x07` | 50% | 1 | 50 |
| `0x08` | 40% | 1 | 40 |

### 输出缓冲格式

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0x00 | 2字节 | SGER | 状态码：`0x8000` = 成功，其他值 = 失败 |
| 0x06 | 4字节 | FRD1 | 当前模式码（仅 GET 时有意义） |

**成功标志**：返回缓冲前两字节（小端序 `uint16`）== `0x8000`

---

## 前置条件

1. **安装 `acpi_call` 内核模块**

   ```bash
   # Debian / Ubuntu
   sudo apt install acpi-call-dkms

   # Arch Linux
   sudo pacman -S acpi_call

   # Fedora / RHEL
   # 官方仓库通常不提供预编译包，需从源码手动编译：
   sudo dnf install kernel-devel dkms
   git clone https://github.com/nix-community/acpi_call.git
   cd acpi_call && make && sudo make install
   ```

   > **Fedora 注意**：由于 Fedora 内核通常启用了 Secure Boot 和模块签名验证，加载未签名的自编译模块可能需要额外配置或在 BIOS 中关闭 Secure Boot。

2. **加载模块**

   ```bash
   sudo modprobe acpi_call
   ```

3. **验证模块已加载**（应有 `/proc/acpi/call` 文件）

   ```bash
   ls /proc/acpi/call
   ```

---

## 安装

```bash
git clone <repo-url>
cd xiaomi-power-test
```

---

## 使用说明

所有操作均需 **root 权限**（`sudo`）。

### ⚠️ 重要：操作前必须先验证 GET

**在执行任何 `set` 操作之前，必须先运行 `get` 并确认返回值为 `100%`**。
若当前值不是 100%，说明硬件环境或 ACPI 路径可能不匹配，请勿继续设置。

```bash
sudo python3 set_charging_wmi.py get
```

预期输出（确认为 100% 后方可进行 set）：
```
[GET] ACPI 调用: \_SB.PC00.WMID.WMAA 0 1 {...}
[GET] 返回值: { 0x00, 0x80, ... }
[GET] [OK]  当前充电上限: 100%
```

### 读取当前阈值

```bash
sudo python3 set_charging_wmi.py get
```

### 设置充电上限

```bash
# 设置为 80%（推荐，兼顾续航与电池健康）
sudo python3 set_charging_wmi.py set 80

# 支持的值：100 / 90 / 80 / 70 / 60 / 50 / 40
sudo python3 set_charging_wmi.py set 100   # 恢复无限制
```

设置成功后脚本会自动回读验证：
```
[SET] 充电上限 → 80%  (FUN4=0x01)
[SET] ACPI 调用: \_SB.PC00.WMID.WMAA 0 1 {...}
[SET] 返回值: { 0x00, 0x80, ... }
[SET] [OK] 写入成功  SGER=0x8000
...
[GET] [OK]  当前充电上限: 80%
[SET] 验证通过：充电上限已确认为 80%
```

### 测试环境是否就绪

```bash
sudo python3 set_charging_wmi.py test
```

---

## 开机自动应用（可选）

创建 systemd 服务以在每次开机时自动恢复充电限制设置：

> **SELinux 注意（Fedora / RHEL 等）**：如果脚本放在家目录（如 `/home/user/`），通过 systemd 执行时 SELinux 可能会因文件 context 不符而拒绝访问 `/proc/acpi/call`。建议将脚本安装到 `/usr/local/bin/`：
> ```bash
> sudo install -m 755 set_charging_wmi.py /usr/local/bin/set-charging-limit
> ```
> 然后在 systemd 服务中使用 `/usr/local/bin/set-charging-limit` 替代家目录路径。

```bash
sudo nano /etc/systemd/system/xiaomi-charging.service
```

```ini
[Unit]
Description=Xiaomi Battery Charging Protection
After=network.target

[Service]
Type=oneshot
ExecStart=/usr/bin/python3 /usr/local/bin/set-charging-limit sync
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable xiaomi-charging.service
sudo systemctl start xiaomi-charging.service
```

> **注意**：充电限制由软件在运行时推送给 EC 硬件，**不会持久化**。系统重启或休眠唤醒后需重新应用。

> **前提：确保 `acpi_call` 模块开机自动加载**
>
> systemd 服务执行时 `acpi_call` 模块必须已加载，否则 `/proc/acpi/call` 不存在导致服务失败。需要：
>
> 1. 确保模块在内核更新后仍能自动编译（各发行版方式不同，参考 [DKMS 文档](https://github.com/dell/dkms)）
> 2. 将模块加入开机自动加载：
>    ```bash
>    echo "acpi_call" | sudo tee /etc/modules-load.d/acpi_call.conf
>    ```

---

## 卸载 / 移除

**1. 恢复充电无限制**

```bash
sudo python3 set_charging_wmi.py set 100
```

**2. 移除 systemd 服务**（如已配置）

```bash
sudo systemctl disable --now xiaomi-charging.service
sudo rm /etc/systemd/system/xiaomi-charging.service
sudo systemctl daemon-reload
```

**3. 删除状态文件**

```bash
sudo rm -f /var/lib/xiaomi-charging-limit
```

**4. 移除脚本**（如已安装到系统目录）

```bash
sudo rm -f /usr/local/bin/set-charging-limit
```

**5. 取消模块自动加载**（如已配置）

```bash
sudo rm -f /etc/modules-load.d/acpi_call.conf
```

---

## 许可

本项目代码以 MIT 许可证发布，仅供学习与个人使用。
