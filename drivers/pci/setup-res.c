// SPDX-License-Identifier: GPL-2.0
/*
 * Support routines for initializing a PCI subsystem
 *
 * Extruded from code written by
 *      Dave Rusling (david.rusling@reo.mts.dec.com)
 *      David Mosberger (davidm@cs.arizona.edu)
 *	David Miller (davem@redhat.com)
 *
 * Fixed for multiple PCI buses, 1999 Andrea Arcangeli <andrea@suse.de>
 *
 * Nov 2000, Ivan Kokshaysky <ink@jurassic.park.msu.ru>
 *	     Resource sorting
 */

#include <linux/align.h>
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/limits.h>
#include <linux/log2.h>
#include <linux/minmax.h>
#include <linux/pci.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/cache.h>
#include <linux/slab.h>
#include "pci.h"

static void pci_std_update_resource(struct pci_dev *dev, int resno)
{
	struct pci_bus_region region;
	bool disable;
	u16 cmd;
	u32 new, check, mask;
	int reg;
	struct resource *res = pci_resource_n(dev, resno);
	const char *res_name = pci_resource_name(dev, resno);

	/* Per SR-IOV spec 3.4.1.11, VF BARs are RO zero */
	if (dev->is_virtfn)
		return;

	/*
	 * Ignore resources for unimplemented BARs and unused resource slots
	 * for 64 bit BARs.
	 */
	if (!res->flags)
		return;

	if (res->flags & IORESOURCE_UNSET)
		return;

	/*
	 * Ignore non-moveable resources.  This might be legacy resources for
	 * which no functional BAR register exists or another important
	 * system resource we shouldn't move around.
	 */
	if (res->flags & IORESOURCE_PCI_FIXED)
		return;

	pcibios_resource_to_bus(dev->bus, &region, res);
	new = region.start;

	if (res->flags & IORESOURCE_IO) {
		mask = (u32)PCI_BASE_ADDRESS_IO_MASK;
		new |= res->flags & ~PCI_BASE_ADDRESS_IO_MASK;
	} else if (resno == PCI_ROM_RESOURCE) {
		mask = PCI_ROM_ADDRESS_MASK;
	} else {
		mask = (u32)PCI_BASE_ADDRESS_MEM_MASK;
		new |= res->flags & ~PCI_BASE_ADDRESS_MEM_MASK;
	}

	if (resno < PCI_ROM_RESOURCE) {
		reg = PCI_BASE_ADDRESS_0 + 4 * resno;
	} else if (resno == PCI_ROM_RESOURCE) {

		/*
		 * Apparently some Matrox devices have ROM BARs that read
		 * as zero when disabled, so don't update ROM BARs unless
		 * they're enabled.  See
		 * https://lore.kernel.org/r/43147B3D.1030309@vc.cvut.cz/
		 * But we must update ROM BAR for buggy devices where even a
		 * disabled ROM can conflict with other BARs.
		 */
		if (!(res->flags & IORESOURCE_ROM_ENABLE) &&
		    !dev->rom_bar_overlap)
			return;

		reg = dev->rom_base_reg;
		if (res->flags & IORESOURCE_ROM_ENABLE)
			new |= PCI_ROM_ADDRESS_ENABLE;
	} else
		return;

	/*
	 * We can't update a 64-bit BAR atomically, so when possible,
	 * disable decoding so that a half-updated BAR won't conflict
	 * with another device.
	 */
	disable = (res->flags & IORESOURCE_MEM_64) && !dev->mmio_always_on;
	if (disable) {
		pci_read_config_word(dev, PCI_COMMAND, &cmd);
		pci_write_config_word(dev, PCI_COMMAND,
				      cmd & ~PCI_COMMAND_MEMORY);
	}

	pci_write_config_dword(dev, reg, new);
	dev->saved_config_space[reg / 4] = new;
	pci_read_config_dword(dev, reg, &check);

	if ((new ^ check) & mask) {
		pci_err(dev, "%s: error updating (%#010x != %#010x)\n",
			res_name, new, check);
	}

	if (res->flags & IORESOURCE_MEM_64) {
		new = region.start >> 16 >> 16;
		pci_write_config_dword(dev, reg + 4, new);
		dev->saved_config_space[(reg + 4) / 4] = new;
		pci_read_config_dword(dev, reg + 4, &check);
		if (check != new) {
			pci_err(dev, "%s: error updating (high %#010x != %#010x)\n",
				res_name, new, check);
		}
	}

	if (disable)
		pci_write_config_word(dev, PCI_COMMAND, cmd);
}

void pci_update_resource(struct pci_dev *dev, int resno)
{
	if (resno <= PCI_ROM_RESOURCE)
		pci_std_update_resource(dev, resno);
	else if (pci_resource_is_iov(resno))
		pci_iov_update_resource(dev, resno);
}

int pci_claim_resource(struct pci_dev *dev, int resource)
{
	struct resource *res = &dev->resource[resource];
	const char *res_name = pci_resource_name(dev, resource);
	struct resource *root, *conflict;

	if (res->flags & IORESOURCE_UNSET) {
		pci_info(dev, "%s %pR: can't claim; no address assigned\n",
			 res_name, res);
		return -EINVAL;
	}

	/*
	 * If we have a shadow copy in RAM, the PCI device doesn't respond
	 * to the shadow range, so we don't need to claim it, and upstream
	 * bridges don't need to route the range to the device.
	 */
	if (res->flags & IORESOURCE_ROM_SHADOW)
		return 0;

	root = pci_find_parent_resource(dev, res);
	if (!root) {
		pci_info(dev, "%s %pR: can't claim; no compatible bridge window\n",
			 res_name, res);
		res->flags |= IORESOURCE_UNSET;
		return -EINVAL;
	}

	conflict = request_resource_conflict(root, res);
	if (conflict) {
		pci_info(dev, "%s %pR: can't claim; address conflict with %s %pR\n",
			 res_name, res, conflict->name, conflict);
		res->flags |= IORESOURCE_UNSET;
		return -EBUSY;
	}

	pci_dbg(dev, "%s %pR: claiming\n", res_name, res);

	return 0;
}
EXPORT_SYMBOL(pci_claim_resource);

void pci_disable_bridge_window(struct pci_dev *dev)
{
	/* MMIO Base/Limit */
	pci_write_config_dword(dev, PCI_MEMORY_BASE, 0x0000fff0);

	/* Prefetchable MMIO Base/Limit */
	pci_write_config_dword(dev, PCI_PREF_LIMIT_UPPER32, 0);
	pci_write_config_dword(dev, PCI_PREF_MEMORY_BASE, 0x0000fff0);
	pci_write_config_dword(dev, PCI_PREF_BASE_UPPER32, 0xffffffff);
}

/*
 * Generic function that returns a value indicating that the device's
 * original BIOS BAR address was not saved and so is not available for
 * reinstatement.
 *
 * Can be over-ridden by architecture specific code that implements
 * reinstatement functionality rather than leaving it disabled when
 * normal allocation attempts fail.
 */
resource_size_t __weak pcibios_retrieve_fw_addr(struct pci_dev *dev, int idx)
{
	return 0;
}

static int pci_revert_fw_address(struct resource *res, struct pci_dev *dev,
		int resno, resource_size_t size)
{
	struct resource *root, *conflict;
	resource_size_t fw_addr, start, end;
	const char *res_name = pci_resource_name(dev, resno);

	fw_addr = pcibios_retrieve_fw_addr(dev, resno);
	if (!fw_addr)
		return -ENOMEM;

	start = res->start;
	end = res->end;
	resource_set_range(res, fw_addr, size);
	res->flags &= ~IORESOURCE_UNSET;

	root = pci_find_parent_resource(dev, res);
	if (!root) {
		/*
		 * If dev is behind a bridge, accesses will only reach it
		 * if res is inside the relevant bridge window.
		 */
		if (pci_upstream_bridge(dev))
			return -ENXIO;

		/*
		 * On the root bus, assume the host bridge will forward
		 * everything.
		 */
		if (res->flags & IORESOURCE_IO)
			root = &ioport_resource;
		else
			root = &iomem_resource;
	}

	pci_info(dev, "%s: trying firmware assignment %pR\n", res_name, res);
	conflict = request_resource_conflict(root, res);
	if (conflict) {
		pci_info(dev, "%s %pR: conflicts with %s %pR\n", res_name, res,
			 conflict->name, conflict);
		res->start = start;
		res->end = end;
		res->flags |= IORESOURCE_UNSET;
		return -EBUSY;
	}
	return 0;
}

/*
 * TODO: Improve PCI resource assignment interface so that callers outside PCI
 * core do not need to know about this detail.
 */
void pci_init_resreq_data(struct pci_resreq_data *rr, struct pci_dev *dev)
{
	bool dummy;

	memset(rr, 0, sizeof(*rr));
	rr->dev = dev;
	rr->min_align = pci_specified_resource_alignment(dev, &dummy);
}
EXPORT_SYMBOL_GPL(pci_init_resreq_data);

resource_size_t pci_resource_alignment(const struct pci_dev *dev,
				       const struct resource *res)
{
	int resno = pci_resource_num(dev, res);

	if (pci_resource_is_iov(resno))
		return pci_sriov_resource_alignment(dev, resno);
	if (dev->class >> 8 == PCI_CLASS_BRIDGE_CARDBUS)
		return pci_cardbus_resource_alignment(res);
	return resource_alignment(res);
}

static resource_size_t pci_max_natural_size(const struct resource *res,
					    resource_size_t *max_align)
{
	resource_size_t powof2, natural_start, natural_end;
	resource_size_t size = resource_size(res);

	*max_align = 1;
	if (!size)
		return 0;

	powof2 = rounddown_pow_of_two_u64(size);
	natural_start = ALIGN(res->start, powof2);
	/* end = ~0 first overflows, then -1 brings it back */
	if (natural_start >= ALIGN_DOWN(res->end + 1, powof2) - 1) {
		powof2 = max(powof2 / 2, 1U);
		/*
		 * Corner case: with start at an odd alignment multiple, there
		 * are two powof2 sized blocks.
		 */
		natural_start = ALIGN(res->start, powof2);
		natural_end = ALIGN_DOWN(res->end + 1 - powof2, powof2);
		*max_align <<= max(natural_start ? __ffs64(natural_start) : 0,
				   natural_end ? __ffs64(natural_end) : 0);

	} else if (natural_start) {
		*max_align <<= __ffs64(natural_start);
	}

	/* Normal PCI resources cannot have align 1. */
	if (*max_align == 1) {
		/*
		 * Zero address has infinite alignment, return the largest
		 * representable number even if it's not a power of two.
		 */
		*max_align = RESOURCE_SIZE_MAX;
	}

	return powof2;
}

struct free_candidate_info {
	resource_size_t size;
	resource_size_t natural_size;
	resource_size_t max_align;
	resource_size_t left_align;
};

/*
 * Select resource placement based on the remaining free space. Pick the
 * candidate with which the remaining free space has (in decreasing order of
 * priority):
 *
 *   1. the largest contiguous free space,
 *   2. the largest naturally aligning power-of-two-sized address range within,
 *   3. the largest alignment of the start address for the naturally aligning
 *      free space range (from check 2),
 *   4. the largest alignment of the start address of the free space.
 */
static int cmp_free_candidate(const struct free_candidate_info *a,
			      const struct free_candidate_info *b)
{
	if (a->size != b->size)
		return a->size < b->size ? -1 : 1;
	if (a->natural_size != b->natural_size)
		return a->natural_size < b->natural_size ? -1 : 1;
	if (a->max_align != b->max_align)
		return a->max_align < b->max_align ? -1 : 1;
	if (a->left_align != b->left_align)
		return a->left_align < b->left_align ? -1 : 1;

	return 0;
}

/**
 * pci_align_resource - Places resource into empty space range
 * @rr: PCI resource request data
 * @res: Candidate range calculated by caller (not among @rr->dev's resources!)
 * @empty_res: Full free space range
 * @size: Required size for the resource
 * @align: Required alignment (see below for details)
 *
 * Places resource inside @empty_res honoring @size and @align.
 *
 * Returns: start address for the resource.
 */
/*
 * Following candidate logic only applies to mem resources currently. For
 * composite resources not divisible by @align, @align does not apply to
 * non-aligning remainder part giving some leeway for its placement. There
 * are 4 candidate positions:
 *
 *           W0WWW0WWW0WWW
 * 0.         AAAAr
 * 1.        rAAAA
 * 2.             AAAAr
 * 3.                rAAAA
 *
 * W = bridge window
 * 0 = bridge window offset matching align
 * A = aligning part of size (size rounded down by align)
 * r = non-aligning remainder
 *
 * Candidates 0 & 2 and 1 & 3 may degenerate to the same address range.
 *
 * Select resource placement based on the remaining free space. Candidates 0
 * & 1 check only right edge free space and 2 & 3 the left edge.
 */
resource_size_t pci_align_resource(struct pci_resreq_data *rr,
				   const struct resource *res,
				   const struct resource *empty_res,
				   resource_size_t size,
				   resource_size_t align)
{
	struct pci_dev *dev = rr->dev;
	unsigned long type = res->flags & PCI_RES_TYPE_MASK;
	struct free_candidate_info best_info = {};
	resource_size_t aligning, remainder;
	struct resource candidate[4];
	unsigned int i;
	int best = -1;

	if (!(res->flags & IORESOURCE_MEM))
		return res->start;

	/*
	 * Not using large enough align when minimum alignment is in use is
	 * a caller-side problem which likely caused also mis-sizing the
	 * corresponding window so do not attempt to solve it here (nor
	 * possibly make the problem worse by applying remainder relocation
	 * below).
	 *
	 * Practical impact of this is expected to be around zero (or the
	 * callers would have been already fixed).
	 *
	 * TODO: Fix all callers and remove this.
	 */
	if (rr->min_align > align) {
		pci_dbg(dev, "%pR: minimum alignment exceeds align (%llx > %llx)\n",
			res,
			(unsigned long long)rr->min_align,
			(unsigned long long)align);
		return res->start;
	}

	remainder = size - ALIGN_DOWN(size, align);
	aligning = size - remainder;
	if (!aligning)
		return res->start;

	candidate[0] = DEFINE_RES(ALIGN(empty_res->start, align), size, type);
	candidate[1] = DEFINE_RES(ALIGN(empty_res->start, align) - remainder,
				  size, type);
	candidate[2] = DEFINE_RES(ALIGN_DOWN(empty_res->end + 1 - remainder, align) -
				  aligning, size, type);
	candidate[3] = DEFINE_RES(ALIGN_DOWN(empty_res->end + 1, align) - size,
				  size, type);

	for (i = 0; i < ARRAY_SIZE(candidate); i++) {
		struct free_candidate_info info;
		struct resource remaining;

		if ((candidate[i].start > candidate[i].end) ||
		    !__resource_contains_unbound(empty_res, &candidate[i])) {
			pci_dbg(dev, "%pR: candidate %u not within free space %pR\n",
				&candidate[i], i, empty_res);
			continue;
		}

		if (rr->min_align &&
		    !IS_ALIGNED(candidate[i].start, rr->min_align)) {
			pci_dbg(dev, "%pR: candidate %u skipped due to alignment lower bound %llx\n",
				&candidate[i], i,
				(unsigned long long)rr->min_align);
			continue;
		}

		remaining.flags = type;
		if (i <= 1) {
			remaining.start = candidate[i].end + 1;
			remaining.end = empty_res->end;
		} else {
			remaining.start = empty_res->start;
			remaining.end = candidate[i].start - 1;
		}

		info.size = resource_size(&remaining);
		info.natural_size = pci_max_natural_size(&remaining,
							 &info.max_align);
		info.left_align = remaining.start ?
				  (1ULL << __ffs64(remaining.start)) :
				  RESOURCE_SIZE_MAX;
		pci_dbg(dev, "%pR: candidate %u free space size=%llx natural_size=%llx max_align=%llx left_align=%llx\n",
			&candidate[i], i,
			(unsigned long long)info.size,
			(unsigned long long)info.natural_size,
			(unsigned long long)info.max_align,
			(unsigned long long)info.left_align);
		if ((best < 0) || cmp_free_candidate(&info, &best_info) > 0) {
			best = i;
			best_info = info;
		}
	}

	/* None fits? Return address where size definitely won't fit. */
	if (best < 0)
		return empty_res->end;

	pci_dbg(dev, "%pR: candidate %u selected (free space %pR), size: %llx + %llx, align: %llx\n",
		&candidate[best], best, empty_res,
		(unsigned long long)aligning,
		(unsigned long long)remainder,
		(unsigned long long)align);

	return candidate[best].start;
}

/*
 * We don't have to worry about legacy ISA devices, so nothing to do here.
 * This is marked as __weak because multiple architectures define it; it should
 * eventually go away.
 */
resource_size_t __weak pcibios_align_resource(void *data,
					      const struct resource *res,
					      const struct resource *empty_res,
					      resource_size_t size,
					      resource_size_t align)
{
	struct pci_resreq_data *rr = data;

	return pci_align_resource(rr, res, empty_res, size, align);
}

static int __pci_assign_resource(struct pci_bus *bus, struct pci_dev *dev,
		int resno, resource_size_t size, resource_size_t align)
{
	struct resource *res = pci_resource_n(dev, resno);
	struct pci_resreq_data rr;
	resource_size_t min;
	int ret;

	pci_init_resreq_data(&rr, dev);

	min = (res->flags & IORESOURCE_IO) ? PCIBIOS_MIN_IO : PCIBIOS_MIN_MEM;

	/*
	 * First, try exact prefetching match.  Even if a 64-bit
	 * prefetchable bridge window is below 4GB, we can't put a 32-bit
	 * prefetchable resource in it because pbus_size_mem() assumes a
	 * 64-bit window will contain no 32-bit resources.  If we assign
	 * things differently than they were sized, not everything will fit.
	 */
	ret = pci_bus_alloc_resource(bus, res, size, align, min,
				     IORESOURCE_PREFETCH | IORESOURCE_MEM_64,
				     pcibios_align_resource, &rr);
	if (ret == 0)
		return 0;

	/*
	 * If the prefetchable window is only 32 bits wide, we can put
	 * 64-bit prefetchable resources in it.
	 */
	if ((res->flags & (IORESOURCE_PREFETCH | IORESOURCE_MEM_64)) ==
	     (IORESOURCE_PREFETCH | IORESOURCE_MEM_64)) {
		ret = pci_bus_alloc_resource(bus, res, size, align, min,
					     IORESOURCE_PREFETCH,
					     pcibios_align_resource, &rr);
		if (ret == 0)
			return 0;
	}

	/*
	 * If we didn't find a better match, we can put any memory resource
	 * in a non-prefetchable window.  If this resource is 32 bits and
	 * non-prefetchable, the first call already tried the only possibility
	 * so we don't need to try again.
	 */
	if (res->flags & (IORESOURCE_PREFETCH | IORESOURCE_MEM_64))
		ret = pci_bus_alloc_resource(bus, res, size, align, min, 0,
					     pcibios_align_resource, &rr);

	return ret;
}

static int _pci_assign_resource(struct pci_dev *dev, int resno,
				resource_size_t size, resource_size_t min_align)
{
	struct pci_bus *bus;
	int ret;

	bus = dev->bus;
	while ((ret = __pci_assign_resource(bus, dev, resno, size, min_align))) {
		if (!bus->parent || !bus->self->transparent)
			break;
		bus = bus->parent;
	}

	return ret;
}

int pci_assign_resource(struct pci_dev *dev, int resno)
{
	struct resource *res = pci_resource_n(dev, resno);
	const char *res_name = pci_resource_name(dev, resno);
	resource_size_t align, size;
	int ret;

	if (res->flags & IORESOURCE_PCI_FIXED)
		return 0;

	res->flags |= IORESOURCE_UNSET;
	align = pci_resource_alignment(dev, res);
	if (!align) {
		pci_info(dev, "%s %pR: can't assign; bogus alignment\n",
			 res_name, res);
		return -EINVAL;
	}

	size = resource_size(res);
	ret = _pci_assign_resource(dev, resno, size, align);

	/*
	 * If we failed to assign anything, let's try the address
	 * where firmware left it.  That at least has a chance of
	 * working, which is better than just leaving it disabled.
	 */
	if (ret < 0) {
		pci_info(dev, "%s %pR: can't assign; no space\n", res_name, res);
		ret = pci_revert_fw_address(res, dev, resno, size);
	}

	if (ret < 0) {
		pci_info(dev, "%s %pR: failed to assign\n", res_name, res);
		return ret;
	}

	res->flags &= ~IORESOURCE_UNSET;
	res->flags &= ~IORESOURCE_STARTALIGN;
	if (pci_resource_is_bridge_win(resno))
		res->flags &= ~IORESOURCE_DISABLED;

	pci_info(dev, "%s %pR: assigned\n", res_name, res);
	if (resno < PCI_BRIDGE_RESOURCES)
		pci_update_resource(dev, resno);

	return 0;
}
EXPORT_SYMBOL(pci_assign_resource);

int pci_reassign_resource(struct pci_dev *dev, int resno,
			  resource_size_t addsize, resource_size_t min_align)
{
	struct resource *res = pci_resource_n(dev, resno);
	const char *res_name = pci_resource_name(dev, resno);
	unsigned long flags;
	resource_size_t new_size;
	int ret;

	if (res->flags & IORESOURCE_PCI_FIXED)
		return 0;

	flags = res->flags;
	res->flags |= IORESOURCE_UNSET;
	if (!res->parent) {
		pci_info(dev, "%s %pR: can't reassign; unassigned resource\n",
			 res_name, res);
		return -EINVAL;
	}

	new_size = resource_size(res) + addsize;
	ret = _pci_assign_resource(dev, resno, new_size, min_align);
	if (ret) {
		res->flags = flags;
		pci_info(dev, "%s %pR: failed to expand by %#llx\n",
			 res_name, res, (unsigned long long) addsize);
		return ret;
	}

	res->flags &= ~IORESOURCE_UNSET;
	res->flags &= ~IORESOURCE_STARTALIGN;
	pci_info(dev, "%s %pR: reassigned; expanded by %#llx\n",
		 res_name, res, (unsigned long long) addsize);
	if (resno < PCI_BRIDGE_RESOURCES)
		pci_update_resource(dev, resno);

	return 0;
}

int pci_release_resource(struct pci_dev *dev, int resno)
{
	struct resource *res = pci_resource_n(dev, resno);
	const char *res_name = pci_resource_name(dev, resno);
	int ret;

	if (!res->parent)
		return 0;

	pci_info(dev, "%s %pR: releasing\n", res_name, res);

	ret = release_resource(res);
	if (ret)
		return ret;
	res->end = resource_size(res) - 1;
	res->start = 0;
	res->flags |= IORESOURCE_UNSET;

	return 0;
}
EXPORT_SYMBOL(pci_release_resource);

int pci_enable_resources(struct pci_dev *dev, int mask)
{
	u16 cmd, old_cmd;
	int i;
	struct resource *r;
	const char *r_name;

	pci_read_config_word(dev, PCI_COMMAND, &cmd);
	old_cmd = cmd;

	pci_dev_for_each_resource(dev, r, i) {
		if (!(mask & (1 << i)))
			continue;

		r_name = pci_resource_name(dev, i);

		if (!(r->flags & (IORESOURCE_IO | IORESOURCE_MEM)))
			continue;
		if (pci_resource_is_optional(dev, i))
			continue;

		if (i < PCI_BRIDGE_RESOURCES) {
			if (r->flags & IORESOURCE_UNSET) {
				pci_err(dev, "%s %pR: not assigned; can't enable device\n",
					r_name, r);
				return -EINVAL;
			}

			if (!r->parent) {
				pci_err(dev, "%s %pR: not claimed; can't enable device\n",
					r_name, r);
				return -EINVAL;
			}
		}

		if (r->parent) {
			if (r->flags & IORESOURCE_IO)
				cmd |= PCI_COMMAND_IO;
			if (r->flags & IORESOURCE_MEM)
				cmd |= PCI_COMMAND_MEMORY;
		}
	}

	if (cmd != old_cmd) {
		pci_info(dev, "enabling device (%04x -> %04x)\n", old_cmd, cmd);
		pci_write_config_word(dev, PCI_COMMAND, cmd);
	}
	return 0;
}
