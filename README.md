# Xiaomi Book Pro 14 Linux Battery Charge Limit

[简体中文](README.zh-CN.md)

Set a battery charging threshold on the Xiaomi Book Pro 14 from Linux. This project provides a native kernel module with standard sysfs interfaces and a standalone Python script that calls the laptop's ACPI WMI firmware.

The author tested both methods on a Xiaomi Book Pro 14. Other Xiaomi laptops with similar firmware are untested. The module and script send commands to the embedded controller (EC), and incompatible firmware can behave unpredictably. Use at your own risk. Claude Sonnet 4.6 contributed the reverse-engineering analysis.

## Get the source

```bash
git clone https://github.com/Xecuss/mibookpro14-powerlimit.git
cd mibookpro14-powerlimit
```

## Native kernel module

The `xiaomi-wmi-battery` module exposes the battery charge threshold and platform performance profiles through standard Linux interfaces. It can integrate with tools such as TLP and GNOME Power and has no `acpi_call` dependency.

The module checks for the DMI vendor `XIAOMI` and product name `Xiaomi Book Pro 14`. To build it, install a compiler, Make, and the headers or development package for your running kernel. From the repository root:

```bash
make
sudo insmod xiaomi-wmi-battery.ko
```

Once the module loads, you can read or change the charge limit and inspect the available profiles:

```bash
cat /sys/class/power_supply/BAT0/charge_control_end_threshold
echo 80 | sudo tee /sys/class/power_supply/BAT0/charge_control_end_threshold
cat /sys/firmware/acpi/platform_profile_choices
```

The [kernel module guide (Chinese)](KERNEL_MOD_README.md) covers distribution dependencies, DKMS installation, Secure Boot and signing, performance profiles, and module removal. A kernel update may require a rebuild. Charge thresholds are applied to the EC at runtime and may need to be reapplied after power loss or resume.

## Python script with acpi_call

Install Python 3.10 or newer and the `acpi_call` kernel module. Debian and Ubuntu package it as `acpi-call-dkms`, and Arch Linux packages it as `acpi_call`. If your distribution lacks a package for your kernel, use the [acpi_call source and build instructions](https://github.com/nix-community/acpi_call). Secure Boot may require a signed module.

Load the module and read the current threshold on the supported laptop:

```bash
sudo modprobe acpi_call
ls /proc/acpi/call
sudo python3 set_charging_wmi.py get
```

**Before the first write on a system with the factory charging configuration, confirm that `get` reports `[OK]` and a threshold of `100%`.** If it fails or reports an unexpected value, stop and check the model and firmware before writing. The script prints its status messages in Chinese.

```bash
sudo python3 set_charging_wmi.py set 80
sudo python3 set_charging_wmi.py get
sudo python3 set_charging_wmi.py set 100
```

The supported limits are `40`, `50`, `60`, `70`, `80`, `90`, and `100` percent. Setting `100` disables the limit. After a successful write, the script saves the setting in `/var/lib/xiaomi-charging-limit` and reads the threshold back for verification. Use `sudo python3 set_charging_wmi.py test` to check that the `acpi_call` interface is available.

### Reapply the limit at boot

After setting a limit with the script, use its `sync` command to reapply the saved value. For systemd, install the script at a system path and configure `acpi_call` to load at boot. A system path also avoids SELinux restrictions on executing a script from a home directory on Fedora and similar systems.

```bash
sudo install -m 755 set_charging_wmi.py /usr/local/bin/set-charging-limit
echo acpi_call | sudo tee /etc/modules-load.d/acpi_call.conf
```

Create `/etc/systemd/system/xiaomi-charging.service` with these contents:

```ini
[Unit]
Description=Xiaomi Battery Charging Protection
After=systemd-modules-load.service

[Service]
Type=oneshot
ExecStart=/usr/bin/python3 /usr/local/bin/set-charging-limit sync
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now xiaomi-charging.service
```

To remove the script setup, first restore the 100% limit. If you created the service, disable it with `sudo systemctl disable --now xiaomi-charging.service`, remove its unit file, and run `sudo systemctl daemon-reload`. You can then remove the installed script and `/var/lib/xiaomi-charging-limit`. Remove `/etc/modules-load.d/acpi_call.conf` if you added it solely for this script.

## WMI reference

The Python script calls instance `0`, method ID `1`, at ACPI path `\_SB.PC00.WMID.WMAA`. The WMI GUID is `B60BFB48-3E5B-49E4-A0E9-8CFFE1B3434B`.

| Offset | Size | Field | Description |
| --- | --- | --- | --- |
| `0x00` | 2 bytes | `FUN1` | `0xFA00` reads and `0xFB00` writes |
| `0x02` | 2 bytes | `FUN2` | `0x1000` selects the battery subsystem |
| `0x04` | 2 bytes | `FUN3` | `0x0002` selects the charging threshold |
| `0x06` | 4 bytes | `FUN4` | Mode code for writes |

The request uses 10 little-endian bytes, packed as `<HHHi` in Python. In the response, the 16-bit status at offset `0x00` is `0x8000` on success. A read returns the current 32-bit mode code at offset `0x06`.

| Mode code | Charge limit | EC LONL bit 0 | EC HBDA |
| --- | ---: | ---: | ---: |
| `0x00` | 100% | 0 | 100 |
| `0x04` | 90% | 1 | 90 |
| `0x01` | 80% | 1 | 80 |
| `0x05` | 70% | 1 | 70 |
| `0x06` | 60% | 1 | 60 |
| `0x07` | 50% | 1 | 50 |
| `0x08` | 40% | 1 | 40 |

## License and credits

Licensing is per file. The kernel module, its Makefile, and `test_perf_mode_wmi.py` use GPL-2.0. The charging script and this README use MIT. See the individual files for their SPDX identifiers where present.

The performance-mode protocol is based on work by [Meow-Box](https://github.com/leehyukshuai/Meow-Box) (GPL-3.0).
