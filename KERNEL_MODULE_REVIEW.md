# Xiaomi WMI Battery 内核模块审查

日期：2026-05-29（最后更新：2026-05-29）

审查对象：[xiaomi-wmi-battery.c](xiaomi-wmi-battery.c)

结论：模块在当前环境中可以完成构建，所有风险点已评估并修复或注释说明。当前构建零警告（pahole/BTF 提示与本模块无关）。

## 已完成的检查

- 在仓库根目录执行 `make`，模块可成功构建。
- 构建过程中仅出现 pahole 和 vmlinux 缺失导致的 BTF 提示，不属于本模块逻辑错误。
- 本次审查以静态代码分析为主，未在真实硬件上做热插拔、多实例和并发卸载测试。

## 风险点

### 1. ~~高风险~~：硬件匹配边界过宽，写入路径可能作用于未验证平台 ✅ 已修复

涉及位置（修复后）：

- [xiaomi-wmi-battery.c](xiaomi-wmi-battery.c#L107-L125)（DMI 白名单）
- [xiaomi-wmi-battery.c](xiaomi-wmi-battery.c#L326-L340)（probe 中的 DMI 检查 + force 开关）

修复方式：

- 加入了 `xmwmi_dmi_table[]`，仅允许 `sys_vendor=XIAOMI` + `product_name=Xiaomi Book Pro 14` 通过。
- probe 中调用 `dmi_check_system()`；不在白名单时拒绝加载（返回 `-ENODEV`），除非传入 `force=1`。
- `force=1` 路径会打印 `dev_warn`，明确标识为未验证平台。

### 2. ~~中风险~~：battery hook 注册失败后，probe 仍然会报告成功 ✅ 已修复

涉及位置（修复后）：

- [xiaomi-wmi-battery.c](xiaomi-wmi-battery.c#L97-L106)（`struct xiaomi_wmi` 新增 `hook_err` 字段）
- [xiaomi-wmi-battery.c](xiaomi-wmi-battery.c#L285-L304)（`xmwmi_battery_add` 写入 `hook_err`）
- [xiaomi-wmi-battery.c](xiaomi-wmi-battery.c#L373-L393)（probe 在 `devm_battery_hook_register` 后检查 `hook_err`）

问题说明（完整）：

- `add_battery` 失败时，battery hook 框架（`battery_hook_calibrate`）会内部撤销 hook，但 `devm_battery_hook_register()` 本身仍返回 0。
- 第一轮修复（切换到 `devm_battery_hook_register`）仅解决了 hook 生命周期绑定问题，并未解决错误传播问题——probe 依然无法感知 `device_create_file` 失败，仍然打印 ready 并返回成功（"假成功"）。

修复方式：

- `struct xiaomi_wmi` 新增 `int hook_err` 字段，作为 `add_battery` → probe 的错误传递通道。
- `xmwmi_battery_add()` 在 `device_create_file` 失败时写入 `xmwmi_data->hook_err`，并附加注释说明原因。
- probe 在 `devm_battery_hook_register()` 返回后立即检查 `data->hook_err`；若非零，打印 `dev_err` 并返回该错误码，使 insmod 失败。
- 此方案无竞态风险：`devm_battery_hook_register()` 在 `battery_hook_calibrate()` 内同步调用 `add_battery`，返回时 `hook_err` 已稳定。

### 3. ~~中风险~~：全局单例设计没有被代码强制约束 ✅ 已修复

涉及位置（修复后）：

- [xiaomi-wmi-battery.c](xiaomi-wmi-battery.c#L320-L323)（probe 入口处的实例检查）

修复方式：

- 在 probe 入口添加 `if (xmwmi_data) return -EBUSY;`，显式拒绝第二个实例并打印 `dev_err`。
- 备注：`xmwmi_data` 本身未加 spinlock 保护，若未来需要做严格并发保证，需额外同步。在当前单 CPU 探测场景下已足够。

### 4. 低风险：多电池场景下的语义可能误导用户态 ✅ 已注释说明

涉及位置（修复后）：

- [xiaomi-wmi-battery.c](xiaomi-wmi-battery.c#L298-L304)（`xmwmi_battery_hook` 上方注释）

处理方式：

- 已添加代码注释：WMI 协议无电池编号参数，阈值控制为平台级；Xiaomi Book Pro 14 实机仅有 BAT0，多电池歧义在此机型上不构成实际问题。
- 不做代码修改：只注册到 BAT0 需要 `power_supply_get_by_name`，引入额外复杂度，收益不大。

## 修复汇总

| # | 原风险级别 | 状态 | 修复方式 |
|---|-----------|------|---------|
| 1 | 高 | ✅ 已修复 | DMI 白名单 + `force` 开关 |
| 2 | 中 | ✅ 已修复 | `hook_err` 字段传播 `add_battery` 失败 |
| 3 | 中 | ✅ 已修复 | probe 入口 `-EBUSY` 拒绝第二实例 |
| 4 | 低 | ✅ 已注释 | 代码注释说明平台级控制语义 |

## 备注

- 当前构建通过（零警告，仅 pahole/BTF 提示与本模块无关）。
- `add_battery` 在框架中是同步调用，`hook_err` 传播路径无竞态。
- 未在真实硬件上做热插拔、多实例和并发卸载测试；上述修复均基于静态分析和 kernel 源码逻辑。
