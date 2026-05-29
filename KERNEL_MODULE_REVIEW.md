# Xiaomi WMI Battery 内核模块审查

日期：2026-05-29（最后更新：2026-05-30）

审查对象：[xiaomi-wmi-battery.c](xiaomi-wmi-battery.c)

结论：模块在当前环境中可以完成构建。原有 battery 路径的 1 到 4 项风险已修复或注释说明，但新增的 performance mode / platform_profile 实现引入了 2 项新的中风险问题，当前仍待处理。构建依旧零警告（pahole/BTF 提示与本模块无关）。

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

### 5. 中风险：platform_profile 回读路径未兼容 beast 模式，合法当前状态会被误报为错误

涉及位置：

- [xiaomi-wmi-battery.c](xiaomi-wmi-battery.c#L272-L305)（性能模式映射表与 `xmwmi_get_perf()`）
- [test_perf_mode_wmi.py](test_perf_mode_wmi.py#L41-L47)（验证脚本中的有效模式定义）

问题说明：

- 驱动注释明确写到固件还存在 `beast`（`0x0004`）模式，并说明它语义上接近 `turbo` / `PLATFORM_PROFILE_PERFORMANCE`。
- 但当前 `xmwmi_perf_table[]` 只包含 `0x000A`、`0x0002`、`0x0009`、`0x0003`，`xmwmi_get_perf()` 完全依赖该表进行回读映射。
- 因此一旦固件当前返回 `0x0004`，`xmwmi_get_perf()` 会打印 `Unknown perf mode code` 并返回 `-ERANGE`，导致读取 `/sys/firmware/acpi/platform_profile` 失败。

影响：

- 当前性能模式本身是合法状态，但用户态会看到读取失败，而不是当前 profile。
- 如果用户通过热键、Windows、厂商守护进程或其他通道切到 `beast`，Linux 侧的新接口会立即退化为不可读。

建议修改：

- 即使不想把 `beast` 暴露为独立可写选项，`xmwmi_get_perf()` 也至少应把 `0x0004` 映射到 `PLATFORM_PROFILE_PERFORMANCE`。
- 如果后续希望保留该状态的可区分性，可考虑把它作为 hidden choice / 内部别名处理，但不应让合法回读直接失败。

### 6. 中风险：probe 未验证性能模式 WMI 路径可达性，却直接宣称 platform_profile 已启用

涉及位置：

- [xiaomi-wmi-battery.c](xiaomi-wmi-battery.c#L286-L305)（`xmwmi_get_perf()`）
- [xiaomi-wmi-battery.c](xiaomi-wmi-battery.c#L446-L529)（probe 中的平台配置注册与 ready 日志）

问题说明：

- 当前 probe 只调用了一次 `xmwmi_get_threshold()` 来验证充电阈值 WMI 路径，然后就直接注册 `platform_profile` 并打印 `platform_profile enabled`。
- 但新增性能模式接口实际依赖的是另一条 WMI 子路径（`FUN2=0x0800`）；probe 阶段没有执行过一次 `xmwmi_get_perf()`，因此没有验证这条路径是否可达、返回码是否可映射。
- `devm_platform_profile_register()` 只负责注册 class 设备，并不会替代驱动完成底层 WMI 功能自检。

影响：

- 只要出现“充电阈值路径可用，但性能模式路径不可用”或“当前性能模式返回未映射码”的情况，模块仍会 probe 成功并宣称接口已启用。
- 实际失败会推迟到用户第一次读取或设置 `/sys/firmware/acpi/platform_profile` 时暴露，属于新增接口上的“假成功”。

建议修改：

- 在 probe 中于注册 `platform_profile` 前先执行一次 `xmwmi_get_perf()`，只有验证成功后再注册并打印 enabled。
- 如果希望允许模块在“仅电池阈值可用”场景下降级工作，则应在日志中明确说明 `platform_profile` 未启用，而不是统一打印 ready。

## 状态汇总

| # | 原风险级别 | 状态 | 修复方式 |
|---|-----------|------|---------|
| 1 | 高 | ✅ 已修复 | DMI 白名单 + `force` 开关 |
| 2 | 中 | ✅ 已修复 | `hook_err` 字段传播 `add_battery` 失败 |
| 3 | 中 | ✅ 已修复 | probe 入口 `-EBUSY` 拒绝第二实例 |
| 4 | 低 | ✅ 已注释 | 代码注释说明平台级控制语义 |
| 5 | 中 | 待修复 | `xmwmi_get_perf()` 需兼容 `beast=0x0004` 回读 |
| 6 | 中 | 待修复 | probe 需先验证性能模式 WMI 路径，再注册 `platform_profile` |

## 备注

- 当前构建通过（零警告，仅 pahole/BTF 提示与本模块无关）。
- `add_battery` 在框架中是同步调用，`hook_err` 传播路径无竞态。
- 未在真实硬件上做热插拔、多实例、并发卸载，以及 `beast` 模式或性能模式失效路径测试；上述结论以静态分析和当前验证脚本为主。
