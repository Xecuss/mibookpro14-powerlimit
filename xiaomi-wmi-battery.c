// SPDX-License-Identifier: GPL-2.0
/*
 * Xiaomi Laptop WMI Charging Threshold Driver
 *
 * Controls battery charge_control_end_threshold via the WMI method GUID
 *   B60BFB48-3E5B-49E4-A0E9-8CFFE1B3434B
 * which maps to ACPI method \_SB.PC00.WMID.WMAA in ssdt25.dsl (XMCC XMCC1806).
 *
 * Once loaded, /sys/class/power_supply/BAT0/charge_control_end_threshold
 * becomes available for read/write (supported values: 40 50 60 70 80 90 100).
 *
 * Based on reverse engineering of XiaomiPCManager 5.8.0.57 and validated
 * with acpi_call on hardware.
 */

#include <linux/acpi.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/wmi.h>
#include <acpi/battery.h>

#define XMWMI_GUID		"B60BFB48-3E5B-49E4-A0E9-8CFFE1B3434B"
#define XMWMI_INSTANCE		0
#define XMWMI_METHOD_ID		1

/* FUN1: call direction */
#define XMWMI_FUN1_SET		0xFB00u	/* write charging threshold */
#define XMWMI_FUN1_GET		0xFA00u	/* read  charging threshold */

/* FUN2: subsystem selector */
#define XMWMI_FUN2_BATTERY	0x1000u

/* FUN3: feature selector (charging threshold) */
#define XMWMI_FUN3_CHG_THRESH	0x0002u

/* SGER: firmware success code in the return buffer */
#define XMWMI_SGER_OK		0x8000u

/*
 * Input buffer layout for WMAA (10 bytes, little-endian):
 *
 *   Offset  Size  Field
 *     0       2   FUN1  – direction (GET / SET)
 *     2       2   FUN2  – subsystem  (0x1000 = battery)
 *     4       2   FUN3  – feature    (0x0002 = charge threshold)
 *     6       4   FUN4  – mode code  (SET: mode, GET: 0)
 */
struct xmwmi_args {
	__le16 fun1;
	__le16 fun2;
	__le16 fun3;
	__le32 fun4;
} __packed;

/*
 * Return buffer layout from WMAA (10 bytes, little-endian):
 *
 *   Offset  Size  Field
 *     0       2   SGER  – status (0x8000 = OK)
 *     2       4   (reserved / other fields)
 *     6       4   FRD1  – returned data (current mode code on GET)
 */
struct xmwmi_ret {
	__le16 sger;
	u8     reserved[4];
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
};

/* Module-level pointer; only one such device can be present at a time. */
static struct xiaomi_wmi *xmwmi_data;

/* -------------------------------------------------------------------------
 * Low-level WMI helper
 * ---------------------------------------------------------------------- */

/**
 * xmwmi_call() - Execute a WMAA call and optionally return FRD1.
 * @fun1:    Direction flag (XMWMI_FUN1_GET or XMWMI_FUN1_SET).
 * @fun4:    Mode code for SET, 0 for GET.
 * @out_val: If non-NULL, receives the FRD1 field from the return buffer.
 *
 * Returns 0 on success, -EIO on ACPI or firmware error.
 */
static int xmwmi_call(u16 fun1, u32 fun4, u32 *out_val)
{
	struct xmwmi_args args = {
		.fun1 = cpu_to_le16(fun1),
		.fun2 = cpu_to_le16(XMWMI_FUN2_BATTERY),
		.fun3 = cpu_to_le16(XMWMI_FUN3_CHG_THRESH),
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

	if (out_val)
		*out_val = le32_to_cpu(ret->frd1);

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

	err = xmwmi_call(XMWMI_FUN1_GET, 0, &mode);
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
			return xmwmi_call(XMWMI_FUN1_SET,
					  xmwmi_thresh_table[i].mode, NULL);
	}

	return -EINVAL;
}

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
	return device_create_file(&bat->dev,
				  &dev_attr_charge_control_end_threshold);
}

static int xmwmi_battery_remove(struct power_supply *bat,
				struct acpi_battery_hook *hook)
{
	device_remove_file(&bat->dev, &dev_attr_charge_control_end_threshold);
	return 0;
}

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

	data = devm_kzalloc(&wdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	err = devm_mutex_init(&wdev->dev, &data->lock);
	if (err)
		return err;

	data->wdev  = wdev;
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

	battery_hook_register(&xmwmi_battery_hook);

	dev_info(&wdev->dev,
		 "Xiaomi WMI charging control ready (current threshold: %d%%)\n",
		 pct);
	return 0;
}

static void xmwmi_wmi_remove(struct wmi_device *wdev)
{
	battery_hook_unregister(&xmwmi_battery_hook);
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
MODULE_DESCRIPTION("Xiaomi Laptop WMI charging threshold control");
MODULE_LICENSE("GPL");
