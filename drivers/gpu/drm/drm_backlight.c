/*
 * DRM Backlight Helpers
 * Copyright (c) 2014 David Herrmann
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 */

#include <linux/backlight.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <drm/drm_backlight.h>
#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_mode_config.h>

/**
 * DOC: Backlight Devices
 *
 * Backlight devices have always been managed as a separate subsystem,
 * independent of DRM. They are usually controlled via separate hardware
 * interfaces than the display controller, so the split works out fine.
 * However, backlight brightness is a property of a display, and thus a
 * property of a DRM connector. We already manage DPMS states via connector
 * properties, so it is natural to keep brightness control at the same place.
 *
 * This DRM backlight interface implements generic backlight properties on
 * connectors. It does not handle any hardware backends but simply forwards
 * the requests to an available and linked backlight device. The links between
 * connectors and backlight devices have to be established by DRM drivers and
 * can be modified by user-space via sysfs (and udev rules). The name of the
 * backlight device can be written to a sysfs attribute called 'backlight'.
 * The device is looked up and linked to the connector (replacing a possible
 * previous backlight device). A 'change' uevent is sent whenever a link is
 * modified.
 *
 * Drivers have to call drm_backlight_alloc() after allocating a connector via
 * drm_connector_init(). This will automatically add a backlight device to the
 * given connector. No hardware device is linked to the connector by default.
 * Drivers can set up a default device via drm_backlight_set_name(), but are
 * free to leave it empty. User-space will then have to set up the link.
 */

struct drm_backlight {
	struct list_head list;
	struct drm_connector *connector;
	char *link_name;
	struct backlight_device *link;
	struct work_struct work;
	unsigned int set_value;
	bool changed : 1;
};

static LIST_HEAD(drm_backlight_list);
static DEFINE_SPINLOCK(drm_backlight_lock);

/* caller must hold @drm_backlight_lock */
static bool __drm_backlight_is_registered(struct drm_backlight *b)
{
	lockdep_assert_held(&drm_backlight_lock);
	/* a device is live if it is linked to @drm_backlight_list */
	return !list_empty(&b->list);
}

/* caller must hold @drm_backlight_lock */
static void __drm_backlight_schedule(struct drm_backlight *b)
{
	lockdep_assert_held(&drm_backlight_lock);
	if (__drm_backlight_is_registered(b))
		schedule_work(&b->work);
}

static void __drm_backlight_worker(struct work_struct *w)
{
	struct drm_backlight *b = container_of(w, struct drm_backlight, work);
	static char *ep[] = { "BACKLIGHT=1", NULL };
	struct backlight_device *bd;
	bool send_uevent;
	unsigned int v;

	scoped_guard(spinlock, &drm_backlight_lock) {
		send_uevent = b->changed;
		b->changed = false;
		v = b->set_value;
		bd = b->link;
		backlight_device_ref(bd);
	}

	if (bd) {
		backlight_set_brightness(bd, v, BACKLIGHT_UPDATE_DRM);
		backlight_device_unref(bd);
	}

	if (send_uevent)
		kobject_uevent_env(&b->connector->kdev->kobj, KOBJ_CHANGE, ep);
}

/* caller must hold @drm_backlight_lock */
static void __drm_backlight_prop_changed(struct drm_backlight *b, uint64_t v)
{
	uint64_t max;

	lockdep_assert_held(&drm_backlight_lock);

	if (!b || !b->link)
		return;

	max = b->link->props.max_brightness;
	if (v >= U16_MAX)
		b->set_value = max;
	else
		b->set_value = (v * max) >> 16;
	__drm_backlight_schedule(b);
}

/* caller must hold @drm_backlight_lock */
static void __drm_backlight_real_changed(struct drm_backlight *b, uint64_t v)
{
	struct drm_mode_config *config = &b->connector->dev->mode_config;
	unsigned int max, set;

	lockdep_assert_held(&drm_backlight_lock);

	if (!b->link)
		return;

	set = v;
	max = b->link->props.max_brightness;
	if (max < 1)
		return;

	if (set >= max)
		set = U16_MAX;
	else if (max <= U16_MAX)
		set = v * ((U16_MAX + max + 1)/(max + 1));
	else
		set = div_u64(v << 16, max);

	drm_object_property_set_value(&b->connector->base,
				      config->brightness_property, set);
}

/* caller must hold @drm_backlight_lock */
static void __drm_backlight_link(struct drm_backlight *b,
				 struct backlight_device *bd)
{
	if (bd == b->link)
		return;

	backlight_device_unref(b->link);
	b->link = bd;
	backlight_device_ref(b->link);
	if (bd)
		__drm_backlight_real_changed(b, bd->props.brightness);
	b->changed = true;
	__drm_backlight_schedule(b);
}

/* caller must hold @drm_backlight_lock */
static void __drm_backlight_lookup(struct drm_backlight *b)
{
	struct backlight_device *bd;

	if (b->link_name)
		bd = backlight_device_lookup(b->link_name);
	else
		bd = NULL;

	__drm_backlight_link(b, bd);
	backlight_device_unref(bd);
}

/**
 * drm_backlight_alloc - add backlight capability to a connector
 * @connector: connector to add backlight to
 *
 * This allocates a new DRM-backlight device and links it to @connector. This
 * *must* be called before registering the connector. The backlight device will
 * be automatically registered in sync with the connector. It will also get
 * removed once the connector is removed.
 *
 * The connector will not have any hardware backlight linked by default. You
 * need to call drm_backlight_set_name() if you want to set a default
 * backlight. User-space can overwrite those via sysfs.
 *
 * Returns: 0 on success, negative error code on failure.
 */
int drm_backlight_alloc(struct drm_connector *connector)
{
	struct drm_mode_config *config = &connector->dev->mode_config;
	struct drm_backlight *b;

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;

	INIT_LIST_HEAD(&b->list);
	INIT_WORK(&b->work, __drm_backlight_worker);
	b->connector = connector;
	connector->backlight = b;

	drm_object_attach_property(&connector->base,
				   config->brightness_property, U16_MAX);

	return 0;
}
EXPORT_SYMBOL(drm_backlight_alloc);

void drm_backlight_free(struct drm_connector *connector)
{
	struct drm_backlight *b = connector->backlight;

	if (!b)
		return;

	WARN_ON(__drm_backlight_is_registered(b));
	WARN_ON(b->link);

	kfree(b->link_name);
	kfree(b);
	connector->backlight = NULL;
}

void drm_backlight_register(struct drm_backlight *b)
{
	if (!b)
		return;

	WARN_ON(__drm_backlight_is_registered(b));

	guard(spinlock)(&drm_backlight_lock);
	list_add(&b->list, &drm_backlight_list);
	__drm_backlight_lookup(b);
}

void drm_backlight_unregister(struct drm_backlight *b)
{
	if (!b)
		return;

	WARN_ON(!__drm_backlight_is_registered(b));

	guard(spinlock)(&drm_backlight_lock);
	list_del_init(&b->list);
	__drm_backlight_link(b, NULL);
	cancel_work_sync(&b->work);
}

/**
 * drm_backlight_get_name - retrieve name of linked backlight device
 * @b: DRM backlight to retrieve name of
 * @buf: target buffer for name
 * @max: size of the target buffer
 *
 * This retrieves the name of the backlight device linked to @b and writes it
 * into @buf. If @buf is NULL or @max is 0, no name will be retrieved, but this
 * function only tests whether a link is set.
 * Otherwise, the name will always be written into @buf and will always be
 * zero-terminated (truncated if too long).
 *
 * If no backlight device is linked to @b, this returns -ENOENT. Otherwise, the
 * length of the written name (excluding the terminating 0 character) is
 * returned.
 * Note that if a device name has been set but the underlying backlight device
 * does not exist, this will still return the linked name. -ENOENT is only
 * returned if no device name has been set, yet (or has been cleared).
 *
 * Returns: On success the length of the written name, on failure a negative
 *          error code.
 */
int drm_backlight_get_name(struct drm_backlight *b, char *buf, size_t max)
{
	int r;

	guard(spinlock)(&drm_backlight_lock);

	if (!b || !b->link_name)
		return -ENOENT;

	if (!buf || !max)
		return -EINVAL;

	r = strlen(b->link_name);

	if (r + 1 > max)
		r = max - 1;
	buf[r] = 0;
	memcpy(buf, b->link_name, r);

	return r;
}
EXPORT_SYMBOL(drm_backlight_get_name);

/**
 * drm_backlight_set_name - Change the device link of a DRM backlight
 * @b: DRM backlight to modify
 * @name: name of backlight device
 *
 * This changes the backlight device-link on @b to the hardware device with
 * name @name. @name is stored on the backlight device, even if no such
 * hardware device is registered, yet. If a backlight device appears later on,
 * it will be automatically linked to all matching DRM backlight devices. If a
 * real hardware backlight device already exists with such a name, it is linked
 * with immediate effect.
 *
 * Whenever a real hardware backlight is linked or unlinked from a DRM connector
 * an uevent with "BACKLIGHT=1" is generated on the connector.
 *
 * Returns: 0 on success, negative error code on failure.
 */
int drm_backlight_set_name(struct drm_backlight *b, const char *name)
{
	char *namecopy;

	if (name && *name) {
		namecopy = kstrdup(name, GFP_KERNEL);
		if (!namecopy)
			return -ENOMEM;
	} else {
		namecopy = NULL;
	}

	guard(spinlock)(&drm_backlight_lock);

	kfree(b->link_name);
	b->link_name = namecopy;
	if (__drm_backlight_is_registered(b))
		__drm_backlight_lookup(b);

	return 0;
}
EXPORT_SYMBOL(drm_backlight_set_name);

void drm_backlight_set_brightness(struct drm_backlight *b, uint64_t value)
{
	guard(spinlock)(&drm_backlight_lock);
	__drm_backlight_prop_changed(b, value);
}

static int drm_backlight_notify(struct notifier_block *self,
				unsigned long event, void *data)
{
	struct backlight_device *bd = data;
	struct drm_backlight *b;
	const char *name;

	guard(spinlock)(&drm_backlight_lock);

	switch (event) {
	case BACKLIGHT_REGISTERED:
		name = dev_name(&bd->dev);
		if (!name)
			break;

		list_for_each_entry(b, &drm_backlight_list, list)
			if (!b->link && b->link_name &&
			    !strcmp(name, b->link_name))
				__drm_backlight_link(b, bd);

		break;
	case BACKLIGHT_UNREGISTERED:
		list_for_each_entry(b, &drm_backlight_list, list)
			if (b->link == bd)
				__drm_backlight_link(b, NULL);

		break;
	}

	return 0;
}

static struct notifier_block drm_backlight_notifier = {
	.notifier_call = drm_backlight_notify,
};

int drm_backlight_init(void)
{
	return backlight_register_notifier(&drm_backlight_notifier);
}

void drm_backlight_exit(void)
{
	backlight_unregister_notifier(&drm_backlight_notifier);
}
