# Xiaomi WMI Battery 内核模块审查

日期：2026-05-29（最后更新：2026-05-30）

审查对象：[xiaomi-wmi-battery.c](xiaomi-wmi-battery.c)

结论：模块在当前环境中可以完成构建。当前审查中识别出的 1 到 6 项风险均已修复或按机型条件做了明确说明；本轮复查未发现新的静态实现问题。构建依旧零警告（pahole/BTF 提示与本模块无关）。

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

### 5. ~~中风险~~：platform_profile 回读路径未兼容 beast 模式，合法当前状态会被误报为错误 ✅ 已修复

涉及位置（修复后）：

- [xiaomi-wmi-battery.c](xiaomi-wmi-battery.c#L263-L315)（性能模式映射表与 `xmwmi_get_perf()`）
- [test_perf_mode_wmi.py](test_perf_mode_wmi.py#L40-L51)（验证脚本中的有效模式定义，已包含 `beast`）

修复方式：

- `xmwmi_get_perf()` 在常规映射表之外，显式把 `beast=0x0004` 视为合法固件状态并映射到 `PLATFORM_PROFILE_PERFORMANCE`。
- 这样即使当前机器是通过热键、Windows 或厂商软件切到 `beast`，Linux 侧读取 `platform_profile` 也不会因未知模式码而失败。
- `test_perf_mode_wmi.py` 里的模式定义本来就已包含 `beast`，脚本与驱动现在保持一致。

复查结论：

- 这条回读失败路径已经消除。
- 现有实现仍然没有把 `beast` 暴露成独立可写 profile，但这属于接口设计选择，不再是读取正确性问题。

### 6. ~~中风险~~：probe 未验证性能模式 WMI 路径可达性，却直接宣称 platform_profile 已启用 ✅ 已修复

- 涉及位置（修复后）：

- [xiaomi-wmi-battery.c](xiaomi-wmi-battery.c#L456-L560)（probe 主路径）
- [xiaomi-wmi-battery.c](xiaomi-wmi-battery.c#L526-L559)（性能模式路径探测与降级处理）

- 修复方式：

- probe 现在会在注册 `platform_profile` 之前先执行一次 `xmwmi_get_perf()`。
- 如果性能模式 WMI 子路径不可达或返回不可识别值，驱动会打印 `platform_profile not registered`，然后以“仅充电阈值接口可用”的降级模式继续加载。
- 只有在 `xmwmi_get_perf()` 成功后，才会调用 `devm_platform_profile_register()` 并打印 `platform_profile enabled`。

- 复查结论：

- “平台配置接口假成功”的问题已经消除。
- 当前实现的降级行为也更合理：充电阈值路径正常时，模块不会因为性能模式路径不可用而整体探测失败。

## 状态汇总

| # | 原风险级别 | 状态 | 修复方式 |
|---|-----------|------|---------|
| 1 | 高 | ✅ 已修复 | DMI 白名单 + `force` 开关 |
| 2 | 中 | ✅ 已修复 | `hook_err` 字段传播 `add_battery` 失败 |
| 3 | 中 | ✅ 已修复 | probe 入口 `-EBUSY` 拒绝第二实例 |
| 4 | 低 | ✅ 已注释 | 代码注释说明平台级控制语义 |
| 5 | 中 | ✅ 已修复 | `xmwmi_get_perf()` 已兼容 `beast=0x0004` 回读 |
| 6 | 中 | ✅ 已修复 | probe 已先验证性能模式 WMI 路径，再决定是否注册 `platform_profile` |

## 备注

- 当前构建通过（零警告，仅 pahole/BTF 提示与本模块无关）。
- `add_battery` 在框架中是同步调用，`hook_err` 传播路径无竞态。
- `beast` 模式回读兼容和 `platform_profile` 预探测逻辑已在代码路径上闭合，本轮静态复查未发现新的功能性问题。
- 未在真实硬件上做热插拔、多实例、并发卸载，以及性能模式失效路径的主动故障注入测试；上述结论以静态分析和当前验证脚本为主。
