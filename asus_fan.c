/**
 *  ASUS Fan control module, verified for following models:
 *  - N551JK 
 *  - ....
 *
 *  Just 'make' and copy the fan.ko file to /lib/modules/`uname -r`/...
 *  If the modules loads succesfully it will bring up a "thermal_cooling_device"
 *  like /sys/devices/virtual/thermal/cooling_deviceX/ mostly providing
 *  cur_state / max_state
 *
 *  PLEASE USE WITH CAUTION, you can easily overheat your machine with a wrong
 *  manually set fan speed...
 *
**/
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/device.h>

#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/platform_device.h>

MODULE_AUTHOR("Felipe Contreras <felipe.contreras@gmail.com>");
MODULE_AUTHOR("Markus Meissner <coder@safemailbox.de>");
MODULE_AUTHOR("Bernd Kast <kastbernd@gmx.de>");
MODULE_DESCRIPTION("ASUS fan driver (ACPI)");
MODULE_LICENSE("GPL");

//These defines are taken from asus-nb-wmi
#define to_platform_driver(drv)					\
	(container_of((drv), struct platform_driver, driver))
#define to_asus_fan_driver(pdrv)					\
	(container_of((pdrv), struct asus_fan_driver, platform_driver))

#define	DRIVER_NAME "asus_fan"

struct asus_fan_driver {
	const char		*name;
	struct module		*owner;

	// 'fan_states' save last (manually) set fan state/speed
	int fan_state;
	// 'fan_manual_mode' keeps whether this fan is manually controlled
	bool fan_manual_mode;


	int (*probe) (struct platform_device *device);

	struct platform_driver	platform_driver;
	struct platform_device *platform_device;
};

struct asus_fan {
	struct platform_device *platform_device;

	struct asus_fan_driver *driver;
        struct asus_fan_driver *driver_gfx;
};


//////
////// GLOBALS
//////

// 'fan_states' save last (manually) set fan state/speed
static int fan_states[2] = {-1, -1};
// 'fan_manual_mode' keeps whether this fan is manually controlled
static bool fan_manual_mode[2] = {false, false};

// 'true' - if current system was identified and thus a second fan is available
static bool has_gfx_fan;

// params struct used frequently for acpi-call-construction
static struct acpi_object_list params;

// max fan speed default
static int max_fan_speed_default = 255;
// ... user-defined max value
static int max_fan_speed_setting = 255;
//// fan "name" 
// regular fan name
static char *fan_desc = "CPU Fan";
// gfx-card fan name
static char *gfx_fan_desc = "GFX Fan";

//this speed will be reported as the minimal for the fans
static int fan_minimum = 10;
static int fan_minimum_gfx = 10;

static struct asus_fan_driver asus_fan_driver = {
	.name = DRIVER_NAME,
	.owner = THIS_MODULE,
};
bool used;

static struct attribute *platform_attributes[] = {
	NULL
};
    static struct attribute_group platform_attribute_group = {
	.attrs = platform_attributes
};

//////
////// FUNCTION PROTOTYPES
//////

// hidden fan api funcs used for both (wrap into them)
static int __fan_get_cur_state( int fan,
                               unsigned long *state);
static int __fan_set_cur_state(int fan,
                               unsigned long state);

// regular fan api funcs
static ssize_t fan_get_cur_state(	struct device *dev,
				struct device_attribute *attr,
				char *buf);
static ssize_t fan_set_cur_state(	struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count);

// gfx fan api funcs
static ssize_t fan_get_cur_state_gfx(	struct device *dev,
					struct device_attribute *attr,
					char *buf);
static ssize_t  fan_set_cur_state_gfx(	struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count);

// generic fan func (no sense as long as auto-mode is bound to both or none of
// the fans...
// - force 'reset' of max-speed (if reset == true) and change to auto-mode
static int fan_set_max_speed(unsigned long state, bool reset);
// acpi-readout
static int fan_get_max_speed(unsigned long *state);

// set fan(s) to automatic mode
static int fan_set_auto(void);

// set fan with index 'fan' to 'speed'
// - includes manual mode activation
static int fan_set_speed(int fan, int speed);

// housekeeping (module) stuff...
static void __exit fan_exit(void);
static int __init fan_init(void);

//TODO: some functions don't have PROTOTYPES by now

//////
////// IMPLEMENTATIONS
//////
static int __fan_get_cur_state(int fan,
                               unsigned long *state) {

  // struct acpi_object_list params;
  union acpi_object args[1];
  unsigned long long value;
  acpi_status ret;

  // getting current fan 'speed' as 'state',
  params.count = ARRAY_SIZE(args);
  params.pointer = args;
  // Args:
  // - get speed from the fan with index 'fan'
  args[0].type = ACPI_TYPE_INTEGER;
  args[0].integer.value = fan;

  // fan does not report during manual speed setting - so fake it!
  if (fan_manual_mode[fan]) {
    *state = fan_states[fan];
    return 0;
  }

  // acpi call
  ret = acpi_evaluate_integer(NULL, "\\_TZ.RFAN", &params, &value);
  if(ret != AE_OK)
    return ret;
//TODO: do that nicer
  if(true) //on N551JK: multiply with 4 thus read out and commanded speeds equal
  {
      value++;
      value *= 4;
  }
  *state = value;
  return 0;
}


static int __fan_set_cur_state(int fan,
                               unsigned long state) {

  fan_states[fan] = state;

  // setting fan to automatic, if cur_state is set to (0x0100) 256
  if (state == 256) {
    fan_manual_mode[fan] = false;
    fan_states[fan] = -1;
    return fan_set_auto();
  } else {
    fan_manual_mode[fan] = true;
    return fan_set_speed(fan, state);
  }
}

static int fan_set_speed(int fan, int speed) {
  // struct acpi_object_list params;
  union acpi_object args[2];
  unsigned long long value;

  // set speed to 'speed' for given 'fan'-index
  // -> automatically switch to manual mode!
  params.count = ARRAY_SIZE(args);
  params.pointer = args;
  // Args:
  // fan index
  // - add '1' to index as '0' has a special meaning (auto-mode)
  args[0].type = ACPI_TYPE_INTEGER;
  args[0].integer.value = fan + 1;
  // target fan speed
  // - between 0x00 and MAX (0 - MAX)
  //   - 'MAX' is usually 0xFF (255)
  //   - should be getable with fan_get_max_speed()
  args[1].type = ACPI_TYPE_INTEGER;
  args[1].integer.value = speed;
  // acpi call
  return acpi_evaluate_integer(NULL, "\\_SB.PCI0.LPCB.EC0.SFNV", &params,
                               &value);
}

//TODO: calculate rpms for manual mode
static unsigned long long __fan_rpm(int fan)
{
  struct acpi_object_list params;
  union acpi_object args[1];
  unsigned long long value;
  acpi_status ret;

  // getting current fan 'speed' as 'state',
  params.count = ARRAY_SIZE(args);
  params.pointer = args;
  // Args:
  // - get speed from the fan with index 'fan'
  args[0].type = ACPI_TYPE_INTEGER;
  args[0].integer.value = fan;

    // acpi call
  ret = acpi_evaluate_integer(NULL, "\\_SB.PCI0.LPCB.EC0.TACH", &params, &value);
  if(ret != AE_OK)
    return 0; 
  return value;
}
static ssize_t fan_rpm(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
  return sprintf(buf, "%llu\n", __fan_rpm(0));
 
}
static ssize_t fan_rpm_gfx(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
  return sprintf(buf, "%llu\n", __fan_rpm(1));
 
}


static ssize_t fan_get_cur_state(	struct device *dev,
					struct device_attribute *attr,
					char *buf) {
  unsigned long state = 0;
  __fan_get_cur_state(0, &state);
  return sprintf(buf, "%lu\n", state);
}

static ssize_t fan_get_cur_state_gfx(	struct device *dev,
					struct device_attribute *attr,
					char *buf) {
   unsigned long state = 0;
  __fan_get_cur_state(1, &state);
  return sprintf(buf, "%lu\n", state);
}


static ssize_t fan_set_cur_state_gfx(	struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count) {
    int state;
    kstrtouint(buf, 10, &state);
  __fan_set_cur_state( 1, state);
  return count;
}

static ssize_t fan_set_cur_state(	struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count) {
  int state;
  kstrtouint(buf, 10, &state);
  __fan_set_cur_state(0, state);
  return count;
}

// Reading the correct max fan speed does not work!
// Setting a max value has the obvious effect, thus we 'fake'
// the 'get_max' function
static int fan_get_max_speed(unsigned long *state) {

  *state = max_fan_speed_setting;
  return 0;
}

static int fan_set_max_speed(unsigned long state, bool reset) {
  union acpi_object args[1];
  unsigned long long value;
  acpi_status ret;
  int arg_qmod = 1;

  // if reset is 'true' ignore anything else and reset to
  // -> auto-mode with max-speed
  // -> use "SB.ARKD.QMOD" _without_ "SB.QFAN",
  //    which seems not writeable as expected
  if (reset) {
    state = 255;
    arg_qmod = 2;
    // Activate the set maximum speed setting
    // Args:
    // 0 - just returns
    // 1 - sets quiet mode to QFAN value
    // 2 - sets quiet mode to 0xFF (that's the default value)
    params.count = ARRAY_SIZE(args);
    params.pointer = args;
    // pass arg
    args[0].type = ACPI_TYPE_INTEGER;
    args[0].integer.value = arg_qmod;

    // acpi call
    ret = acpi_evaluate_integer(NULL, "\\_SB.ATKD.QMOD", &params, &value);
    if(ret != AE_OK) {
      printk(KERN_INFO
             "asus-fan (set_max_speed) - set max fan speed(s) failed (force "
             "reset)! errcode: %d",
             ret);
      return ret;
    }

    // if reset was not forced, set max fan speed to 'state'
  } else {
    // is applied automatically on any available fan
    // - docs say it should affect manual _AND_ automatic mode
    // Args:
    // - from 0x00 to 0xFF (0 - 255)
    params.count = ARRAY_SIZE(args);
    params.pointer = args;
    // pass arg
    args[0].type = ACPI_TYPE_INTEGER;
    args[0].integer.value = state;

    // acpi call
    ret = acpi_evaluate_integer(NULL, "\\_SB.PCI0.LPCB.EC0.ST98", &params, &value);
    if(ret != AE_OK) {
      printk(KERN_INFO
             "asus-fan (set_max_speed) - set max fan speed(s) failed (no "
             "reset)! errcode: %d",
             ret);
      return ret;
    }
  }

  // keep set max fan speed for the get_max
  max_fan_speed_setting = state;

  return ret;
}

static int fan_set_auto() {
  union acpi_object args[2];
  unsigned long long value;
  acpi_status ret;

  // setting (both) to auto-mode simultanously
  fan_manual_mode[0] = false;
  fan_states[0] = -1;
  if (has_gfx_fan) {
    fan_states[1] = -1;
    fan_manual_mode[1] = false;
  }

  // acpi call to call auto-mode for all fans!
  params.count = ARRAY_SIZE(args);
  params.pointer = args;
  // special fan-id == 0 must be used
  args[0].type = ACPI_TYPE_INTEGER;
  args[0].integer.value = 0;
  // speed has to be set to zero
  args[1].type = ACPI_TYPE_INTEGER;
  args[1].integer.value = 0;

  // acpi call
  ret = acpi_evaluate_integer(NULL, "\\_SB.PCI0.LPCB.EC0.SFNV", &params, &value);
  if(ret != AE_OK) {
    printk(KERN_INFO
           "asus-fan (set_auto) - failed reseting fan(s) to auto-mode! "
           "errcode: %d - DANGER! OVERHEAT? DANGER!",
           ret);
    return ret;
  }

  return ret;
}


static ssize_t fan_label(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
  return sprintf(buf, "%s\n",fan_desc);
}

static ssize_t fan_label_gfx(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
  return sprintf(buf, "%s\n",gfx_fan_desc);
}

static ssize_t fan_min(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
  return sprintf(buf, "%d\n", fan_minimum);
}

static ssize_t fan_min_gfx(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
  return sprintf(buf, "%d\n", fan_minimum_gfx);
}


static DEVICE_ATTR(pwm1, S_IWUSR | S_IRUGO, fan_get_cur_state, fan_set_cur_state);
static DEVICE_ATTR(fan1_min, S_IRUGO, fan_min, NULL);
static DEVICE_ATTR(fan1_input, S_IRUGO, fan_rpm, NULL);
static DEVICE_ATTR(fan1_label, S_IRUGO, fan_label, NULL);

static DEVICE_ATTR(pwm2, S_IWUSR | S_IRUGO, fan_get_cur_state_gfx, fan_set_cur_state_gfx);
static DEVICE_ATTR(fan2_min, S_IRUGO, fan_min_gfx, NULL);
static DEVICE_ATTR(fan2_input, S_IRUGO, fan_rpm_gfx, NULL);
static DEVICE_ATTR(fan2_label, S_IRUGO, fan_label_gfx, NULL);

static struct attribute *hwmon_attributes[] = {
	&dev_attr_pwm1.attr,
	&dev_attr_fan1_min.attr,
	&dev_attr_fan1_input.attr,
	&dev_attr_fan1_label.attr,
	NULL
};

static struct attribute *hwmon_gfx_attributes[] = {
  	&dev_attr_pwm1.attr,
	&dev_attr_fan1_min.attr,
	&dev_attr_fan1_input.attr,
	&dev_attr_fan1_label.attr,
	&dev_attr_pwm2.attr,
	&dev_attr_fan2_min.attr,
	&dev_attr_fan2_input.attr,
	&dev_attr_fan2_label.attr,
	NULL
};

static umode_t asus_hwmon_sysfs_is_visible(struct kobject *kobj,
					  struct attribute *attr, int idx)
{
	return 1;
}

static struct attribute_group hwmon_attribute_group = {
	.is_visible = asus_hwmon_sysfs_is_visible,
	.attrs = hwmon_attributes
};
__ATTRIBUTE_GROUPS(hwmon_attribute);

static struct attribute_group hwmon_gfx_attribute_group = {
	.is_visible = asus_hwmon_sysfs_is_visible,
	.attrs = hwmon_gfx_attributes
};
__ATTRIBUTE_GROUPS(hwmon_gfx_attribute);

static int asus_fan_hwmon_init(struct asus_fan *asus)
{
	struct device *hwmon;
	if(!has_gfx_fan)
	{
	  hwmon = hwmon_device_register_with_groups(&asus->platform_device->dev,
						    "asus_fan", asus,
						    hwmon_attribute_groups);
	  if (IS_ERR(hwmon)) {
		  pr_err("Could not register asus hwmon device\n");
		  return PTR_ERR(hwmon);
	  }
	}
	else
	{
	  hwmon = hwmon_device_register_with_groups(&asus->platform_device->dev,
						    "asus_fan", asus,
						    hwmon_gfx_attribute_groups);
	  if (IS_ERR(hwmon)) {
		  pr_err("Could not register asus hwmon device\n");
		  return PTR_ERR(hwmon);
	  }
	}
	return 0;
}

static void asus_fan_sysfs_exit(struct platform_device *device)
{
	sysfs_remove_group(&device->dev.kobj, &platform_attribute_group);
}

static int asus_fan_probe(struct platform_device *pdev)
{
	struct platform_driver *pdrv = to_platform_driver(pdev->dev.driver);
	struct asus_fan_driver *wdrv = to_asus_fan_driver(pdrv);
	
        struct asus_fan *asus;
	int err = 0;

	asus = kzalloc(sizeof(struct asus_fan), GFP_KERNEL);
	if (!asus)
		return -ENOMEM;

	asus->driver = wdrv;
	asus->platform_device = pdev;
	wdrv->platform_device = pdev;
	platform_set_drvdata(asus->platform_device, asus);

        sysfs_create_group(&asus->platform_device->dev.kobj, &platform_attribute_group);

	err = asus_fan_hwmon_init(asus);
	if (err)
		goto fail_hwmon;
	return 0;

fail_hwmon:
	asus_fan_sysfs_exit(asus->platform_device);
	kfree(asus);
	return err;

}

static int asus_fan_remove(struct platform_device *device)
{
	struct asus_fan *asus;

	asus = platform_get_drvdata(device);
	asus_fan_sysfs_exit(asus->platform_device);
	kfree(asus);
	return 0;
}

int __init_or_module asus_fan_register_driver(struct asus_fan_driver *driver)
{
	struct platform_driver *platform_driver;
	struct platform_device *platform_device;

	if (used)
        {
		return -EBUSY;
        }
	platform_driver = &driver->platform_driver;
	platform_driver->remove = asus_fan_remove;
	platform_driver->driver.owner = driver->owner;
	platform_driver->driver.name = driver->name;

	platform_device = platform_create_bundle(platform_driver,
						 asus_fan_probe,
						 NULL, 0, NULL, 0);
	if (IS_ERR(platform_device))
        {
                return PTR_ERR(platform_device);
        }

	used = true;
	return 0;
}

static int __init fan_init(void) {
  acpi_status ret;

  // identify system/model/platform
  if (!strcmp(dmi_get_system_info(DMI_SYS_VENDOR), "ASUSTeK COMPUTER INC.")) {
    const char *name = dmi_get_system_info(DMI_PRODUCT_NAME);

    // catching all (supported) Zenbooks _without_ a dedicated gfx-card
    if (!strcmp(name, "UX31E") || !strcmp(name, "UX21") ||
        !strcmp(name, "UX301LA") || !strcmp(name, "UX21A") ||
        !strcmp(name, "UX31A") || !strcmp(name, "UX32A") ||
        !strcmp(name, "UX42VS") || !strcmp(name, "UX302LA") ||
        !strcmp(name, "N551JK") || !strcmp(name, "N56JN")) {
      has_gfx_fan = false;

      // this branch represents the (supported) Zenbooks with a dedicated
      // gfx-card
    } else if (!strcmp(name, "UX32VD") || !strcmp(name, "UX52VS") ||
               !strcmp(name, "UX500VZ") || !strcmp(name, "NX500")) {
      printk(
          KERN_INFO
          "asus-fan (init) - found dedicated gfx-card - second fan usable!\n");
      has_gfx_fan = true;

      // product not supported by this driver...
    } else {
      printk(KERN_INFO "asus-fan (init) - product name: '%s' unknown!\n", name);
      printk(KERN_INFO "asus-fan (init) - aborting!\n");
      return -ENODEV;
    }
    // not an ASUSTeK system ...
  } else
    return -ENODEV;

  ret = asus_fan_register_driver(&asus_fan_driver);
  if (ret != AE_OK) {
    printk(KERN_INFO
           "asus-fan (init) - set max speed to: '%d' failed! errcode: %d",
           max_fan_speed_default, ret);
    return ret;
  }
  
  // set max-speed back to 'default'
  ret = fan_set_max_speed(max_fan_speed_default, false);
  if (ret != AE_OK) {
    printk(KERN_INFO
           "asus-fan (init) - set max speed to: '%d' failed! errcode: %d",
           max_fan_speed_default, ret);
    return ret;
  }

  // force sane enviroment / init with automatic fan controlling
  if ((ret = fan_set_auto()) != AE_OK) {
    printk(
        KERN_INFO
        "asus-fan (init) - set auto-mode speed to active, failed! errcode: %d",
        ret);
    return ret;
  }

  printk(KERN_INFO "asus-fan (init) - finished init\n");
  return 0;
}

void asus_fan_unregister_driver(struct asus_fan_driver *driver)
{
	platform_device_unregister(driver->platform_device);
	platform_driver_unregister(&driver->platform_driver);
	used = false;
}

static void __exit fan_exit(void) {
  fan_set_auto();
  asus_fan_unregister_driver(&asus_fan_driver);
  used = false;

  printk(KERN_INFO "asus-fan (exit) - module unloaded - cleaning up...\n");
}

module_init(fan_init);
module_exit(fan_exit);
