// SPDX-License-Identifier: GPL-2.0
/*
 * Xiaomi Laptop WMI Driver
 *
 * Controls battery charge_control_end_threshold and performance modes via
 * WMI method GUID B60BFB48-3E5B-49E4-A0E9-8CFFE1B3434B, which maps to
 * ACPI method \_SB.PC00.WMID.WMAA in ssdt25.dsl (XMCC XMCC1806).
 *
 * Exposes:
 *   /sys/class/power_supply/BAT0/charge_control_end_threshold
 *       read/write; supported values: 40 50 60 70 80 90 100
 *   /sys/firmware/acpi/platform_profile
 *       read/write; supported values: low-power quiet balanced performance
 *
 * Charging protocol: reverse-engineered from XiaomiPCManager 5.8.0.57.
 * Performance mode protocol: discovered via Meow-Box (GPL-3.0,
 *   github.com/leehyukshuai/Meow-Box), verified with acpi_call on hardware.
 */

#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_profile.h>
#include <linux/power_supply.h>
#include <linux/wmi.h>
#include <acpi/battery.h>

#define XMWMI_GUID		"B60BFB48-3E5B-49E4-A0E9-8CFFE1B3434B"
#define XMWMI_INSTANCE		0
#define XMWMI_METHOD_ID		1

/* FUN1: call direction */
#define XMWMI_FUN1_GET		0xFA00u
#define XMWMI_FUN1_SET		0xFB00u

/* FUN2: subsystem selector */
#define XMWMI_FUN2_BATTERY	0x1000u	/* charging threshold */
#define XMWMI_FUN2_PERF		0x0800u	/* performance mode */

/* FUN3: feature / mode selector */
#define XMWMI_FUN3_CHG_THRESH	0x0002u	/* charging threshold */
#define XMWMI_FUN3_PERF_GET	0x0000u	/* perf GET; SET carries mode code in FUN3 */

/* SGER: firmware success code in the return buffer */
#define XMWMI_SGER_OK		0x8000u

/*
 * Input buffer layout for WMAA (10 bytes, little-endian):
 *
 *   Offset  Size  Field
 *     0       2   FUN1  – direction (GET / SET)
 *     2       2   FUN2  – subsystem  (XMWMI_FUN2_BATTERY or XMWMI_FUN2_PERF)
 *     4       2   FUN3  – feature; for perf SET this carries the mode code
 *     6       4   FUN4  – mode code for charging SET, 0 otherwise
 */
struct xmwmi_args {
	__le16 fun1;
	__le16 fun2;
	__le16 fun3;
	__le32 fun4;
} __packed;

/*
 * Return buffer layout from WMAA (firmware returns 32 bytes; we inspect
 * only the first 10), little-endian:
 *
 *   Offset  Size  Field
 *     0       2   SGER  – status (0x8000 = OK)
 *     2       2   echo  – FUN2 echoed back by firmware
 *     4       2   data0 – returned u16 (perf mode code on GET perf)
 *     6       4   frd1  – returned u32 (charge limit code on GET charge)
 */
struct xmwmi_ret {
	__le16 sger;
	__le16 echo;
	__le16 data0;
	__le32 frd1;
} __packed;

/*
 * Mapping between user-visible charge percentage and the firmware mode codes
 * documented in ssdt25.dsl, WMAA Case(0xFB00) / Case(0x1000) / Case(0x02).
 *
 * mode 0x00: disable limit    (LONL bit0 = 0, HBDA = 0x64 / 100%)
 * mode 0x01: 80 %             (HBDA = 0x50)
 * mode 0x04: 90 %             (HBDA = 0x5A)
 * mode 0x05: 70 %             (HBDA = 0x46)
 * mode 0x06: 60 %             (HBDA = 0x3C)
 * mode 0x07: 50 %             (HBDA = 0x32)
 * mode 0x08: 40 %             (HBDA = 0x28)
 */
static const struct xmwmi_thresh_entry {
	int pct;
	u32 mode;
} xmwmi_thresh_table[] = {
	{ 100, 0x00 },
	{  90, 0x04 },
	{  80, 0x01 },
	{  70, 0x05 },
	{  60, 0x06 },
	{  50, 0x07 },
	{  40, 0x08 },
};

struct xiaomi_wmi {
	struct wmi_device *wdev;
	struct mutex lock; /* serialises WMAA calls */
	/*
	 * Set by xmwmi_battery_add() if device_create_file fails.  Checked
	 * immediately after devm_battery_hook_register() in probe, which calls
	 * add_battery synchronously for all already-present batteries before
	 * returning, so reading this field is race-free.
	 */
	int hook_err;
};

/*
 * Module-level pointer to the single active WMI device.
 * A second probe() call is explicitly refused (see xmwmi_wmi_probe).
 */
static struct xiaomi_wmi *xmwmi_data;

/*
 * DMI whitelist: only hardware where the mode code table has been verified
 * against the shipped ACPI firmware (ssdt25.dsl, XMCC XMCC1806).
 * Pass force=1 to override on unlisted hardware at your own risk.
 */
static const struct dmi_system_id xmwmi_dmi_table[] = {
	{
		.ident = "Xiaomi Book Pro 14",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR,   "XIAOMI"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Xiaomi Book Pro 14"),
		},
	},
	{ }
};

static bool force;
module_param(force, bool, 0444);
MODULE_PARM_DESC(force, "Load on hardware not in the DMI whitelist (unsafe)");

/* -------------------------------------------------------------------------
 * Low-level WMI helper
 * ---------------------------------------------------------------------- */

/**
 * xmwmi_invoke() - Execute a WMAA call with explicit FUN parameters.
 * @fun1:      Direction flag (XMWMI_FUN1_GET or XMWMI_FUN1_SET).
 * @fun2:      Subsystem selector (XMWMI_FUN2_BATTERY or XMWMI_FUN2_PERF).
 * @fun3:      Feature selector; for perf-mode SET this carries the mode code.
 * @fun4:      Mode code for charging SET, 0 otherwise.
 * @out_data0: If non-NULL, receives data0 (offset 4, u16) from the response.
 * @out_frd1:  If non-NULL, receives frd1  (offset 6, u32) from the response.
 *
 * Returns 0 on success, -EIO on ACPI or firmware error.
 */
static int xmwmi_invoke(u16 fun1, u16 fun2, u16 fun3, u32 fun4,
			u16 *out_data0, u32 *out_frd1)
{
	struct xmwmi_args args = {
		.fun1 = cpu_to_le16(fun1),
		.fun2 = cpu_to_le16(fun2),
		.fun3 = cpu_to_le16(fun3),
		.fun4 = cpu_to_le32(fun4),
	};
	struct acpi_buffer in  = { sizeof(args), &args };
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	struct xmwmi_ret  *ret;
	acpi_status status;
	int err = 0;

	mutex_lock(&xmwmi_data->lock);
	status = wmidev_evaluate_method(xmwmi_data->wdev,
					XMWMI_INSTANCE, XMWMI_METHOD_ID,
					&in, &out);
	mutex_unlock(&xmwmi_data->lock);

	if (ACPI_FAILURE(status)) {
		dev_err(&xmwmi_data->wdev->dev,
			"wmidev_evaluate_method failed: %s\n",
			acpi_format_exception(status));
		return -EIO;
	}

	obj = out.pointer;
	if (!obj || obj->type != ACPI_TYPE_BUFFER ||
	    obj->buffer.length < sizeof(*ret)) {
		dev_err(&xmwmi_data->wdev->dev,
			"Unexpected WMI return object (type=%d, len=%u)\n",
			obj ? obj->type : -1,
			obj ? obj->buffer.length : 0);
		err = -EIO;
		goto free_out;
	}

	ret = (struct xmwmi_ret *)obj->buffer.pointer;
	if (le16_to_cpu(ret->sger) != XMWMI_SGER_OK) {
		dev_err(&xmwmi_data->wdev->dev,
			"WMAA returned error SGER=0x%04x\n",
			le16_to_cpu(ret->sger));
		err = -EIO;
		goto free_out;
	}

	if (out_data0)
		*out_data0 = le16_to_cpu(ret->data0);
	if (out_frd1)
		*out_frd1 = le32_to_cpu(ret->frd1);

free_out:
	kfree(out.pointer);
	return err;
}

/* -------------------------------------------------------------------------
 * Threshold get / set helpers
 * ---------------------------------------------------------------------- */

static int xmwmi_get_threshold(int *pct_out)
{
	u32 mode;
	int err, i;

	err = xmwmi_invoke(XMWMI_FUN1_GET, XMWMI_FUN2_BATTERY,
			   XMWMI_FUN3_CHG_THRESH, 0, NULL, &mode);
	if (err)
		return err;

	for (i = 0; i < ARRAY_SIZE(xmwmi_thresh_table); i++) {
		if (xmwmi_thresh_table[i].mode == mode) {
			*pct_out = xmwmi_thresh_table[i].pct;
			return 0;
		}
	}

	dev_warn(&xmwmi_data->wdev->dev,
		 "Unknown mode code 0x%02x returned by firmware\n", mode);
	return -ERANGE;
}

static int xmwmi_set_threshold(int pct)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(xmwmi_thresh_table); i++) {
		if (xmwmi_thresh_table[i].pct == pct)
			return xmwmi_invoke(XMWMI_FUN1_SET, XMWMI_FUN2_BATTERY,
					    XMWMI_FUN3_CHG_THRESH,
					    xmwmi_thresh_table[i].mode,
					    NULL, NULL);
	}

	return -EINVAL;
}

/* -------------------------------------------------------------------------
 * Performance mode get / set helpers
 * ---------------------------------------------------------------------- */

/*
 * Mapping between Linux platform_profile options and firmware mode codes
 * (FUN2=0x0800, verified on Xiaomi Book Pro 14 2026 via acpi_call).
 *
 *   battery (0x000A): minimal CPU/GPU limits, fan silent
 *   silent  (0x0002): quiet fan, moderate performance limits
 *   smart   (0x0009): firmware auto-tunes for balanced use (default)
 *   turbo   (0x0003): maximum sustained performance, active cooling
 *
 * The firmware also has "beast" (0x0004, AC-only turbo variant) which
 * maps to the same platform_profile level as turbo; it is omitted here
 * to keep the set of exposed choices unambiguous.
 */
static const struct xmwmi_perf_entry {
	enum platform_profile_option profile;
	u16                          code;
} xmwmi_perf_table[] = {
	{ PLATFORM_PROFILE_LOW_POWER,   0x000A },
	{ PLATFORM_PROFILE_QUIET,       0x0002 },
	{ PLATFORM_PROFILE_BALANCED,    0x0009 },
	{ PLATFORM_PROFILE_PERFORMANCE, 0x0003 },
};

static int xmwmi_get_perf(enum platform_profile_option *profile_out)
{
	u16 code;
	int err, i;

	err = xmwmi_invoke(XMWMI_FUN1_GET, XMWMI_FUN2_PERF,
			   XMWMI_FUN3_PERF_GET, 0, &code, NULL);
	if (err)
		return err;

	for (i = 0; i < ARRAY_SIZE(xmwmi_perf_table); i++) {
		if (xmwmi_perf_table[i].code == code) {
			*profile_out = xmwmi_perf_table[i].profile;
			return 0;
		}
	}

	dev_warn(&xmwmi_data->wdev->dev,
		 "Unknown perf mode code 0x%04x from firmware\n", code);
	return -ERANGE;
}

static int xmwmi_set_perf(enum platform_profile_option profile)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(xmwmi_perf_table); i++) {
		if (xmwmi_perf_table[i].profile == profile)
			return xmwmi_invoke(XMWMI_FUN1_SET, XMWMI_FUN2_PERF,
					    xmwmi_perf_table[i].code, 0,
					    NULL, NULL);
	}

	return -EOPNOTSUPP;
}

/* -------------------------------------------------------------------------
 * platform_profile interface – exposes performance modes via
 * /sys/firmware/acpi/platform_profile
 * ---------------------------------------------------------------------- */

static int xmwmi_pp_probe(void *drvdata, unsigned long *choices)
{
	set_bit(PLATFORM_PROFILE_LOW_POWER,   choices);
	set_bit(PLATFORM_PROFILE_QUIET,       choices);
	set_bit(PLATFORM_PROFILE_BALANCED,    choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);
	return 0;
}

static int xmwmi_pp_get(struct device *dev,
			enum platform_profile_option *profile)
{
	return xmwmi_get_perf(profile);
}

static int xmwmi_pp_set(struct device *dev,
			enum platform_profile_option profile)
{
	return xmwmi_set_perf(profile);
}

static const struct platform_profile_ops xmwmi_pp_ops = {
	.probe       = xmwmi_pp_probe,
	.profile_get = xmwmi_pp_get,
	.profile_set = xmwmi_pp_set,
};

/* -------------------------------------------------------------------------
 * sysfs attribute: charge_control_end_threshold
 * ---------------------------------------------------------------------- */

static ssize_t charge_control_end_threshold_show(struct device *dev,
						  struct device_attribute *attr,
						  char *buf)
{
	int pct, err;

	err = xmwmi_get_threshold(&pct);
	if (err)
		return err;

	return sysfs_emit(buf, "%d\n", pct);
}

static ssize_t charge_control_end_threshold_store(struct device *dev,
						   struct device_attribute *attr,
						   const char *buf, size_t size)
{
	int pct, err;

	if (kstrtoint(buf, 10, &pct))
		return -EINVAL;

	/*
	 * Firmware only supports fixed steps; reject anything else so user
	 * space gets a clear EINVAL rather than silent truncation.
	 * Supported: 40 50 60 70 80 90 100
	 */
	err = xmwmi_set_threshold(pct);
	if (err)
		return err;

	return size;
}

static DEVICE_ATTR_RW(charge_control_end_threshold);

/* -------------------------------------------------------------------------
 * ACPI battery hook – attaches the attribute to every BAT* device
 * ---------------------------------------------------------------------- */

static int xmwmi_battery_add(struct power_supply *bat,
			     struct acpi_battery_hook *hook)
{
	int err;

	err = device_create_file(&bat->dev,
				 &dev_attr_charge_control_end_threshold);
	if (err) {
		dev_err(&bat->dev,
			"Failed to create charge_control_end_threshold: %d\n",
			err);
		/*
		 * Propagate the error so probe() can detect it after
		 * devm_battery_hook_register() returns.  The framework will
		 * call remove_battery for any battery that already succeeded
		 * and then unregister the hook, but it does not surface the
		 * error through the registration call itself.
		 */
		if (xmwmi_data)
			xmwmi_data->hook_err = err;
	}
	return err;
}

static int xmwmi_battery_remove(struct power_supply *bat,
				struct acpi_battery_hook *hook)
{
	device_remove_file(&bat->dev, &dev_attr_charge_control_end_threshold);
	return 0;
}

/*
 * This hook attaches the attribute to every BAT* device enumerated by the
 * ACPI battery driver.  Note: the WMI protocol carries no per-battery
 * selector, so charge_control_end_threshold is effectively a platform-level
 * control regardless of which BAT device is written.  The Xiaomi Book Pro 14
 * has a single battery (BAT0), so this distinction is moot in practice.
 */
static struct acpi_battery_hook xmwmi_battery_hook = {
	.add_battery    = xmwmi_battery_add,
	.remove_battery = xmwmi_battery_remove,
	.name           = "Xiaomi WMI Battery",
};

/* -------------------------------------------------------------------------
 * WMI driver callbacks
 * ---------------------------------------------------------------------- */

static int xmwmi_wmi_probe(struct wmi_device *wdev, const void *ctx)
{
	struct xiaomi_wmi *data;
	int pct, err;

	/* Risk 3: refuse a second simultaneous instance. */
	if (xmwmi_data) {
		dev_err(&wdev->dev, "Only one instance supported; refusing second probe\n");
		return -EBUSY;
	}

	/*
	 * Risk 1: restrict write path to hardware with a verified mode code
	 * table.  Other machines may expose the same GUID with different
	 * firmware semantics, leading to incorrect EC writes.
	 */
	if (!dmi_check_system(xmwmi_dmi_table)) {
		if (!force) {
			dev_err(&wdev->dev,
				"Hardware not in DMI whitelist; refusing to load "
				"(pass force=1 to override)\n");
			return -ENODEV;
		}
		dev_warn(&wdev->dev,
			 "Hardware not in DMI whitelist -- loading anyway (force=1)\n");
	}

	data = devm_kzalloc(&wdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	err = devm_mutex_init(&wdev->dev, &data->lock);
	if (err)
		return err;

	data->wdev = wdev;
	dev_set_drvdata(&wdev->dev, data);
	xmwmi_data = data;

	/* Verify the WMI method is reachable before registering the hook. */
	err = xmwmi_get_threshold(&pct);
	if (err) {
		dev_err(&wdev->dev,
			"Initial WMAA read failed – hardware unsupported?\n");
		xmwmi_data = NULL;
		return err;
	}

	/*
	 * Use the devm variant so the hook lifetime is tied to the WMI device.
	 * devm_battery_hook_register() calls add_battery synchronously for all
	 * already-present batteries (via battery_hook_calibrate) before
	 * returning; if add_battery fails the framework unregisters the hook
	 * internally but still returns 0 here.  We therefore check data->hook_err
	 * afterwards to catch that silent failure path.
	 */
	err = devm_battery_hook_register(&wdev->dev, &xmwmi_battery_hook);
	if (err) {
		dev_err(&wdev->dev, "Failed to register battery hook: %d\n", err);
		xmwmi_data = NULL;
		return err;
	}
	if (data->hook_err) {
		dev_err(&wdev->dev,
			"Battery attribute creation failed: %d\n",
			data->hook_err);
		xmwmi_data = NULL;
		return data->hook_err;
	}

	err = PTR_ERR_OR_ZERO(devm_platform_profile_register(&wdev->dev,
							     "xiaomi-wmi",
							     data,
							     &xmwmi_pp_ops));
	if (err) {
		dev_err(&wdev->dev,
			"Failed to register platform profile: %d\n", err);
		xmwmi_data = NULL;
		return err;
	}

	dev_info(&wdev->dev,
		 "Xiaomi WMI ready (charge threshold: %d%%, platform_profile enabled)\n",
		 pct);
	return 0;
}

static void xmwmi_wmi_remove(struct wmi_device *wdev)
{
	/* battery hook is automatically unregistered by devm cleanup */
	xmwmi_data = NULL;
}

static const struct wmi_device_id xmwmi_id_table[] = {
	{ .guid_string = XMWMI_GUID },
	{ }
};
MODULE_DEVICE_TABLE(wmi, xmwmi_id_table);

static struct wmi_driver xmwmi_driver = {
	.driver = {
		.name = "xiaomi-wmi-battery",
	},
	.id_table = xmwmi_id_table,
	.probe    = xmwmi_wmi_probe,
	.remove   = xmwmi_wmi_remove,
};
module_wmi_driver(xmwmi_driver);

MODULE_AUTHOR("xecus");
MODULE_DESCRIPTION("Xiaomi Laptop WMI battery and performance mode control");
MODULE_LICENSE("GPL");
