// SPDX-License-Identifier: GPL-2.0
/* Lock down the kernel
 *
 * Copyright (C) 2016 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licence
 * as published by the Free Software Foundation; either version
 * 2 of the Licence, or (at your option) any later version.
 */

#include <linux/security.h>
#include <linux/export.h>
#include <linux/lsm_hooks.h>
#include <linux/init.h>
#include <uapi/linux/lsm.h>

#if IS_ENABLED(CONFIG_X86)
#include <asm/iommu.h>
#endif
#include <linux/iommu.h>

static enum lockdown_reason kernel_locked_down;

static const enum lockdown_reason lockdown_levels[] = {LOCKDOWN_NONE,
						 LOCKDOWN_INTEGRITY_MAX,
						 LOCKDOWN_CONFIDENTIALITY_MAX};

/*
 * Put the kernel into lock-down mode.
 */
static int lock_kernel_down(const char *where, enum lockdown_reason level)
{
	if (kernel_locked_down >= level)
		return -EPERM;

	kernel_locked_down = level;
	pr_notice("Kernel is locked down from %s; see man kernel_lockdown.7\n",
		  where);
	return 0;
}

static int __init lockdown_param(char *level)
{
	if (!level)
		return -EINVAL;

	if (strcmp(level, "integrity") == 0)
		lock_kernel_down("command line", LOCKDOWN_INTEGRITY_MAX);
	else if (strcmp(level, "confidentiality") == 0)
		lock_kernel_down("command line", LOCKDOWN_CONFIDENTIALITY_MAX);
	else
		return -EINVAL;

	return 0;
}

early_param("lockdown", lockdown_param);

/**
 * lockdown_is_locked_down - Find out if the kernel is locked down
 * @what: Tag to use in notice generated if lockdown is in effect
 */
static int lockdown_is_locked_down(enum lockdown_reason what)
{
	if (WARN(what >= LOCKDOWN_CONFIDENTIALITY_MAX,
		 "Invalid lockdown reason"))
		return -EPERM;

	if (kernel_locked_down >= what) {
		if (lockdown_reasons[what])
			pr_notice_ratelimited("Lockdown: %s: %s is restricted; see man kernel_lockdown.7\n",
				  current->comm, lockdown_reasons[what]);
		return -EPERM;
	}

	return 0;
}

static struct security_hook_list lockdown_hooks[] __ro_after_init = {
	LSM_HOOK_INIT(locked_down, lockdown_is_locked_down),
};

static const struct lsm_id lockdown_lsmid = {
	.name = "lockdown",
	.id = LSM_ID_LOCKDOWN,
};

static int __init lockdown_lsm_init(void)
{
#if defined(CONFIG_LOCK_DOWN_KERNEL_FORCE_INTEGRITY)
	lock_kernel_down("Kernel configuration", LOCKDOWN_INTEGRITY_MAX);
#elif defined(CONFIG_LOCK_DOWN_KERNEL_FORCE_CONFIDENTIALITY)
	lock_kernel_down("Kernel configuration", LOCKDOWN_CONFIDENTIALITY_MAX);
#endif

	/*
	 * If lockdown was activated (via command line or kernel config)
	 * and IOMMU is disabled, refuse to boot. Without IOMMU, any
	 * PCI/PCIe device can perform DMA to arbitrary physical memory,
	 * completely bypassing lockdown protections.
	 *
	 * On x86, 'no_iommu' is set early by the 'iommu=off' cmdline
	 * option during parse_early_param(), before this init function.
	 */
#if IS_ENABLED(CONFIG_X86)
	if (kernel_locked_down > LOCKDOWN_NONE && no_iommu) {
		pr_err("FATAL: Lockdown is active but IOMMU is disabled.\n");
		pr_err("FATAL: PCI devices can perform DMA to arbitrary memory,\n");
		pr_err("FATAL: bypassing all lockdown protections.\n");
		panic("Lockdown enabled but IOMMU is disabled (no_iommu)\n");
	}
#endif

	security_add_hooks(lockdown_hooks, ARRAY_SIZE(lockdown_hooks),
			   &lockdown_lsmid);
	return 0;
}

static ssize_t lockdown_read(struct file *filp, char __user *buf, size_t count,
			     loff_t *ppos)
{
	char temp[80] = "";
	int i, offset = 0;

	for (i = 0; i < ARRAY_SIZE(lockdown_levels); i++) {
		enum lockdown_reason level = lockdown_levels[i];

		if (lockdown_reasons[level]) {
			const char *label = lockdown_reasons[level];

			if (kernel_locked_down == level)
				offset += sprintf(temp+offset, "[%s] ", label);
			else
				offset += sprintf(temp+offset, "%s ", label);
		}
	}

	/* Convert the last space to a newline if needed. */
	if (offset > 0)
		temp[offset-1] = '\n';

	return simple_read_from_buffer(buf, count, ppos, temp, strlen(temp));
}

static ssize_t lockdown_write(struct file *file, const char __user *buf,
			      size_t n, loff_t *ppos)
{
	char *state;
	int i, len, err = -EINVAL;

	state = memdup_user_nul(buf, n);
	if (IS_ERR(state))
		return PTR_ERR(state);

	len = strlen(state);
	if (len && state[len-1] == '\n') {
		state[len-1] = '\0';
		len--;
	}

	for (i = 0; i < ARRAY_SIZE(lockdown_levels); i++) {
		enum lockdown_reason level = lockdown_levels[i];
		const char *label = lockdown_reasons[level];

		if (label && !strcmp(state, label))
			err = lock_kernel_down("securityfs", level);
	}

	kfree(state);
	return err ? err : n;
}

static const struct file_operations lockdown_ops = {
	.read  = lockdown_read,
	.write = lockdown_write,
};

static int __init lockdown_secfs_init(void)
{
	struct dentry *dentry;

	dentry = securityfs_create_file("lockdown", 0644, NULL, NULL,
					&lockdown_ops);
	return PTR_ERR_OR_ZERO(dentry);
}

#ifdef CONFIG_SECURITY_LOCKDOWN_LSM_EARLY
DEFINE_EARLY_LSM(lockdown) = {
#else
DEFINE_LSM(lockdown) = {
#endif
	.id = &lockdown_lsmid,
	.init = lockdown_lsm_init,
	.initcall_core = lockdown_secfs_init,
};

/*
 * Late init call to verify IOMMU availability when lockdown is active.
 * This catches cases where:
 *  - IOMMU hardware is absent on the platform
 *  - IOMMU was disabled by driver initialization failure
 *  - IOMMU was not detected (e.g. broken firmware, missing ACPI tables)
 *
 * On x86, we also check no_iommu here as a double-check. The early check
 * in lockdown_lsm_init() catches 'iommu=off' from the cmdline before
 * lockdown_lsm_init() runs, but this late check covers any missed cases.
 */
static int __init lockdown_check_iommu(void)
{
	if (kernel_locked_down > LOCKDOWN_NONE) {
		/*
		 * Check generic IOMMU availability (all architectures).
		 * iommu_is_available() returns true if any IOMMU driver
		 * registered successfully.
		 */
		if (!iommu_is_available()) {
			pr_err("FATAL: Lockdown is active but no IOMMU driver\n");
			pr_err("FATAL: registered. PCI devices can DMA to\n");
			pr_err("FATAL: arbitrary memory, bypassing lockdown.\n");
			panic("Lockdown enabled but IOMMU not available\n");
		}
#if IS_ENABLED(CONFIG_X86)
		/*
		 * Also double-check x86-specific flag.
		 */
		if (no_iommu) {
			pr_err("FATAL: Lockdown is active but IOMMU disabled\n");
			pr_err("FATAL: via kernel command line (iommu=off).\n");
			panic("Lockdown enabled but IOMMU disabled (iommu=off)\n");
		}
#endif
	}
	return 0;
}
late_initcall(lockdown_check_iommu);
