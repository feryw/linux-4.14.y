/*
 * GICv3 ITS emulation
 *
 * Copyright (C) 2015,2016 ARM Ltd.
 * Author: Andre Przywara <andre.przywara@arm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/cpu.h>
#include <linux/kvm.h>
#include <linux/kvm_host.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/uaccess.h>
#include <linux/list_sort.h>

#include <linux/irqchip/arm-gic-v3.h>

#include <asm/kvm_emulate.h>
#include <asm/kvm_arm.h>
#include <asm/kvm_mmu.h>

#include "vgic.h"
#include "vgic-mmio.h"

static int vgic_its_save_tables_v0(struct vgic_its *its);
static int vgic_its_restore_tables_v0(struct vgic_its *its);
static int vgic_its_commit_v0(struct vgic_its *its);
static int update_lpi_config(struct kvm *kvm, struct vgic_irq *irq,
			     struct kvm_vcpu *filter_vcpu);

/*
 * Creates a new (reference to a) struct vgic_irq for a given LPI.
 * If this LPI is already mapped on another ITS, we increase its refcount
 * and return a pointer to the existing structure.
 * If this is a "new" LPI, we allocate and initialize a new struct vgic_irq.
 * This function returns a pointer to the _unlocked_ structure.
 */
static struct vgic_irq *vgic_add_lpi(struct kvm *kvm, u32 intid,
				     struct kvm_vcpu *vcpu)
{
	struct vgic_dist *dist = &kvm->arch.vgic;
	struct vgic_irq *irq = vgic_get_irq(kvm, NULL, intid), *oldirq;
	int ret;

	/* In this case there is no put, since we keep the reference. */
	if (irq)
		return irq;

	irq = kzalloc(sizeof(struct vgic_irq), GFP_KERNEL);
	if (!irq)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&irq->lpi_list);
	INIT_LIST_HEAD(&irq->ap_list);
	spin_lock_init(&irq->irq_lock);

	irq->config = VGIC_CONFIG_EDGE;
	kref_init(&irq->refcount);
	irq->intid = intid;
	irq->target_vcpu = vcpu;

	spin_lock(&dist->lpi_list_lock);

	/*
	 * There could be a race with another vgic_add_lpi(), so we need to
	 * check that we don't add a second list entry with the same LPI.
	 */
	list_for_each_entry(oldirq, &dist->lpi_list_head, lpi_list) {
		if (oldirq->intid != intid)
			continue;

		/* Someone was faster with adding this LPI, lets use that. */
		kfree(irq);
		irq = oldirq;

		/*
		 * This increases the refcount, the caller is expected to
		 * call vgic_put_irq() on the returned pointer once it's
		 * finished with the IRQ.
		 */
		vgic_get_irq_kref(irq);

		goto out_unlock;
	}

	list_add_tail(&irq->lpi_list, &dist->lpi_list_head);
	dist->lpi_list_count++;

out_unlock:
	spin_unlock(&dist->lpi_list_lock);

	/*
	 * We "cache" the configuration table entries in our struct vgic_irq's.
	 * However we only have those structs for mapped IRQs, so we read in
	 * the respective config data from memory here upon mapping the LPI.
	 */
	ret = update_lpi_config(kvm, irq, NULL);
	if (ret)
		return ERR_PTR(ret);

	ret = vgic_v3_lpi_sync_pending_status(kvm, irq);
	if (ret)
		return ERR_PTR(ret);

	return irq;
}

struct its_device {
	struct list_head dev_list;

	/* the head for the list of ITTEs */
	struct list_head itt_head;
	u32 num_eventid_bits;
	gpa_t itt_addr;
	u32 device_id;
};

#define COLLECTION_NOT_MAPPED ((u32)~0)

struct its_collection {
	struct list_head coll_list;

	u32 collection_id;
	u32 target_addr;
};

#define its_is_collection_mapped(coll) ((coll) && \
				((coll)->target_addr != COLLECTION_NOT_MAPPED))

struct its_ite {
	struct list_head ite_list;

	struct vgic_irq *irq;
	struct its_collection *collection;
	u32 event_id;
};

/**
 * struct vgic_its_abi - ITS abi ops and settings
 * @cte_esz: collection table entry size
 * @dte_esz: device table entry size
 * @ite_esz: interrupt translation table entry size
 * @save tables: save the ITS tables into guest RAM
 * @restore_tables: restore the ITS internal structs from tables
 *  stored in guest RAM
 * @commit: initialize the registers which expose the ABI settings,
 *  especially the entry sizes
 */
struct vgic_its_abi {
	int cte_esz;
	int dte_esz;
	int ite_esz;
	int (*save_tables)(struct vgic_its *its);
	int (*restore_tables)(struct vgic_its *its);
	int (*commit)(struct vgic_its *its);
};

static const struct vgic_its_abi its_table_abi_versions[] = {
	[0] = {.cte_esz = 8, .dte_esz = 8, .ite_esz = 8,
	 .save_tables = vgic_its_save_tables_v0,
	 .restore_tables = vgic_its_restore_tables_v0,
	 .commit = vgic_its_commit_v0,
	},
};

#define NR_ITS_ABIS	ARRAY_SIZE(its_table_abi_versions)

inline const struct vgic_its_abi *vgic_its_get_abi(struct vgic_its *its)
{
	return &its_table_abi_versions[its->abi_rev];
}

int vgic_its_set_abi(struct vgic_its *its, int rev)
{
	const struct vgic_its_abi *abi;

	its->abi_rev = rev;
	abi = vgic_its_get_abi(its);
	return abi->commit(its);
}

/*
 * Find and returns a device in the device table for an ITS.
 * Must be called with the its_lock mutex held.
 */
static struct its_device *find_its_device(struct vgic_its *its, u32 device_id)
{
	struct its_device *device;

	list_for_each_entry(device, &its->device_list, dev_list)
		if (device_id == device->device_id)
			return device;

	return NULL;
}

/*
 * Find and returns an interrupt translation table entry (ITTE) for a given
 * Device ID/Event ID pair on an ITS.
 * Must be called with the its_lock mutex held.
 */
static struct its_ite *find_ite(struct vgic_its *its, u32 device_id,
				  u32 event_id)
{
	struct its_device *device;
	struct its_ite *ite;

	device = find_its_device(its, device_id);
	if (device == NULL)
		return NULL;

	list_for_each_entry(ite, &device->itt_head, ite_list)
		if (ite->event_id == event_id)
			return ite;

	return NULL;
}

/* To be used as an iterator this macro misses the enclosing parentheses */
#define for_each_lpi_its(dev, ite, its) \
	list_for_each_entry(dev, &(its)->device_list, dev_list) \
		list_for_each_entry(ite, &(dev)->itt_head, ite_list)

/*
 * We only implement 48 bits of PA at the moment, although the ITS
 * supports more. Let's be restrictive here.
 */
#define BASER_ADDRESS(x)	((x) & GENMASK_ULL(47, 16))
#define CBASER_ADDRESS(x)	((x) & GENMASK_ULL(47, 12))

#define GIC_LPI_OFFSET 8192

#define VITS_TYPER_IDBITS 16
#define VITS_TYPER_DEVBITS 16
#define VITS_DTE_MAX_DEVID_OFFSET	(BIT(14) - 1)
#define VITS_ITE_MAX_EVENTID_OFFSET	(BIT(16) - 1)

/*
 * Finds and returns a collection in the ITS collection table.
 * Must be called with the its_lock mutex held.
 */
static struct its_collection *find_collection(struct vgic_its *its, int coll_id)
{
	struct its_collection *collection;

	list_for_each_entry(collection, &its->collection_list, coll_list) {
		if (coll_id == collection->collection_id)
			return collection;
	}

	return NULL;
}

#define LPI_PROP_ENABLE_BIT(p)	((p) & LPI_PROP_ENABLED)
#define LPI_PROP_PRIORITY(p)	((p) & 0xfc)

/*
 * Reads the configuration data for a given LPI from guest memory and
 * updates the fields in struct vgic_irq.
 * If filter_vcpu is not NULL, applies only if the IRQ is targeting this
 * VCPU. Unconditionally applies if filter_vcpu is NULL.
 */
static int update_lpi_config(struct kvm *kvm, struct vgic_irq *irq,
			     struct kvm_vcpu *filter_vcpu)
{
	u64 propbase = GICR_PROPBASER_ADDRESS(kvm->arch.vgic.propbaser);
	u8 prop;
	int ret;

	ret = kvm_read_guest(kvm, propbase + irq->intid - GIC_LPI_OFFSET,
			     &prop, 1);

	if (ret)
		return ret;

	spin_lock(&irq->irq_lock);

	if (!filter_vcpu || filter_vcpu == irq->target_vcpu) {
		irq->priority = LPI_PROP_PRIORITY(prop);
		irq->enabled = LPI_PROP_ENABLE_BIT(prop);

		vgic_queue_irq_unlock(kvm, irq);
	} else {
		spin_unlock(&irq->irq_lock);
	}

	return 0;
}

/*
 * Create a snapshot of the current LPIs targeting @vcpu, so that we can
 * enumerate those LPIs without holding any lock.
 * Returns their number and puts the kmalloc'ed array into intid_ptr.
 */
static int vgic_copy_lpi_list(struct kvm_vcpu *vcpu, u32 **intid_ptr)
{
	struct vgic_dist *dist = &vcpu->kvm->arch.vgic;
	struct vgic_irq *irq;
	u32 *intids;
	int irq_count = dist->lpi_list_count, i = 0;

	/*
	 * We use the current value of the list length, which may change
	 * after the kmalloc. We don't care, because the guest shouldn't
	 * change anything while the command handling is still running,
	 * and in the worst case we would miss a new IRQ, which one wouldn't
	 * expect to be covered by this command anyway.
	 */
	intids = kmalloc_array(irq_count, sizeof(intids[0]), GFP_KERNEL);
	if (!intids)
		return -ENOMEM;

	spin_lock(&dist->lpi_list_lock);
	list_for_each_entry(irq, &dist->lpi_list_head, lpi_list) {
		/* We don't need to "get" the IRQ, as we hold the list lock. */
		if (irq->target_vcpu != vcpu)
			continue;
		intids[i++] = irq->intid;
	}
	spin_unlock(&dist->lpi_list_lock);

	*intid_ptr = intids;
	return i;
}

/*
 * Promotes the ITS view of affinity of an ITTE (which redistributor this LPI
 * is targeting) to the VGIC's view, which deals with target VCPUs.
 * Needs to be called whenever either the collection for a LPIs has
 * changed or the collection itself got retargeted.
 */
static void update_affinity_ite(struct kvm *kvm, struct its_ite *ite)
{
	struct kvm_vcpu *vcpu;

	if (!its_is_collection_mapped(ite->collection))
		return;

	vcpu = kvm_get_vcpu(kvm, ite->collection->target_addr);

	spin_lock(&ite->irq->irq_lock);
	ite->irq->target_vcpu = vcpu;
	spin_unlock(&ite->irq->irq_lock);
}

/*
 * Updates the target VCPU for every LPI targeting this collection.
 * Must be called with the its_lock mutex held.
 */
static void update_affinity_collection(struct kvm *kvm, struct vgic_its *its,
				       struct its_collection *coll)
{
	struct its_device *device;
	struct its_ite *ite;

	for_each_lpi_its(device, ite, its) {
		if (!ite->collection || coll != ite->collection)
			continue;

		update_affinity_ite(kvm, ite);
	}
}

static u32 max_lpis_propbaser(u64 propbaser)
{
	int nr_idbits = (propbaser & 0x1f) + 1;

	return 1U << min(nr_idbits, INTERRUPT_ID_BITS_ITS);
}

/*
 * Sync the pending table pending bit of LPIs targeting @vcpu
 * with our own data structures. This relies on the LPI being
 * mapped before.
 */
static int its_sync_lpi_pending_table(struct kvm_vcpu *vcpu)
{
	gpa_t pendbase = GICR_PENDBASER_ADDRESS(vcpu->arch.vgic_cpu.pendbaser);
	struct vgic_irq *irq;
	int last_byte_offset = -1;
	int ret = 0;
	u32 *intids;
	int nr_irqs, i;

	nr_irqs = vgic_copy_lpi_list(vcpu, &intids);
	if (nr_irqs < 0)
		return nr_irqs;

	for (i = 0; i < nr_irqs; i++) {
		int byte_offset, bit_nr;
		u8 pendmask;

		byte_offset = intids[i] / BITS_PER_BYTE;
		bit_nr = intids[i] % BITS_PER_BYTE;

		/*
		 * For contiguously allocated LPIs chances are we just read
		 * this very same byte in the last iteration. Reuse that.
		 */
		if (byte_offset != last_byte_offset) {
			ret = kvm_read_guest(vcpu->kvm, pendbase + byte_offset,
					     &pendmask, 1);
			if (ret) {
				kfree(intids);
				return ret;
			}
			last_byte_offset = byte_offset;
		}

		irq = vgic_get_irq(vcpu->kvm, NULL, intids[i]);
		spin_lock(&irq->irq_lock);
		irq->pending_latch = pendmask & (1U << bit_nr);
		vgic_queue_irq_unlock(vcpu->kvm, irq);
		vgic_put_irq(vcpu->kvm, irq);
	}

	kfree(intids);

	return ret;
}

static unsigned long vgic_mmio_read_its_typer(struct kvm *kvm,
					      struct vgic_its *its,
					      gpa_t addr, unsigned int len)
{
	const struct vgic_its_abi *abi = vgic_its_get_abi(its);
	u64 reg = GITS_TYPER_PLPIS;

	/*
	 * We use linear CPU numbers for redistributor addressing,
	 * so GITS_TYPER.PTA is 0.
	 * Also we force all PROPBASER registers to be the same, so
	 * CommonLPIAff is 0 as well.
	 * To avoid memory waste in the guest, we keep the number of IDBits and
	 * DevBits low - as least for the time being.
	 */
	reg |= GIC_ENCODE_SZ(VITS_TYPER_DEVBITS, 5) << GITS_TYPER_DEVBITS_SHIFT;
	reg |= GIC_ENCODE_SZ(VITS_TYPER_IDBITS, 5) << GITS_TYPER_IDBITS_SHIFT;
	reg |= GIC_ENCODE_SZ(abi->ite_esz, 4) << GITS_TYPER_ITT_ENTRY_SIZE_SHIFT;

	return extract_bytes(reg, addr & 7, len);
}

static unsigned long vgic_mmio_read_its_iidr(struct kvm *kvm,
					     struct vgic_its *its,
					     gpa_t addr, unsigned int len)
{
	u32 val;

	val = (its->abi_rev << GITS_IIDR_REV_SHIFT) & GITS_IIDR_REV_MASK;
	val |= (PRODUCT_ID_KVM << GITS_IIDR_PRODUCTID_SHIFT) | IMPLEMENTER_ARM;
	return val;
}

static int vgic_mmio_uaccess_write_its_iidr(struct kvm *kvm,
					    struct vgic_its *its,
					    gpa_t addr, unsigned int len,
					    unsigned long val)
{
	u32 rev = GITS_IIDR_REV(val);

	if (rev >= NR_ITS_ABIS)
		return -EINVAL;
	return vgic_its_set_abi(its, rev);
}

static unsigned long vgic_mmio_read_its_idregs(struct kvm *kvm,
					       struct vgic_its *its,
					       gpa_t addr, unsigned int len)
{
	switch (addr & 0xffff) {
	case GITS_PIDR0:
		return 0x92;	/* part number, bits[7:0] */
	case GITS_PIDR1:
		return 0xb4;	/* part number, bits[11:8] */
	case GITS_PIDR2:
		return GIC_PIDR2_ARCH_GICv3 | 0x0b;
	case GITS_PIDR4:
		return 0x40;	/* This is a 64K software visible page */
	/* The following are the ID registers for (any) GIC. */
	case GITS_CIDR0:
		return 0x0d;
	case GITS_CIDR1:
		return 0xf0;
	case GITS_CIDR2:
		return 0x05;
	case GITS_CIDR3:
		return 0xb1;
	}

	return 0;
}

/*
 * Find the target VCPU and the LPI number for a given devid/eventid pair
 * and make this IRQ pending, possibly injecting it.
 * Must be called with the its_lock mutex held.
 * Returns 0 on success, a positive error value for any ITS mapping
 * related errors and negative error values for generic errors.
 */
static int vgic_its_trigger_msi(struct kvm *kvm, struct vgic_its *its,
				u32 devid, u32 eventid)
{
	struct kvm_vcpu *vcpu;
	struct its_ite *ite;

	if (!its->enabled)
		return -EBUSY;

	ite = find_ite(its, devid, eventid);
	if (!ite || !its_is_collection_mapped(ite->collection))
		return E_ITS_INT_UNMAPPED_INTERRUPT;

	vcpu = kvm_get_vcpu(kvm, ite->collection->target_addr);
	if (!vcpu)
		return E_ITS_INT_UNMAPPED_INTERRUPT;

	if (!vcpu->arch.vgic_cpu.lpis_enabled)
		return -EBUSY;

	spin_lock(&ite->irq->irq_lock);
	ite->irq->pending_latch = true;
	vgic_queue_irq_unlock(kvm, ite->irq);

	return 0;
}

static struct vgic_io_device *vgic_get_its_iodev(struct kvm_io_device *dev)
{
	struct vgic_io_device *iodev;

	if (dev->ops != &kvm_io_gic_ops)
		return NULL;

	iodev = container_of(dev, struct vgic_io_device, dev);

	if (iodev->iodev_type != IODEV_ITS)
		return NULL;

	return iodev;
}

/*
 * Queries the KVM IO bus framework to get the ITS pointer from the given
 * doorbell address.
 * We then call vgic_its_trigger_msi() with the decoded data.
 * According to the KVM_SIGNAL_MSI API description returns 1 on success.
 */
int vgic_its_inject_msi(struct kvm *kvm, struct kvm_msi *msi)
{
	u64 address;
	struct kvm_io_device *kvm_io_dev;
	struct vgic_io_device *iodev;
	int ret;

	if (!vgic_has_its(kvm))
		return -ENODEV;

	if (!(msi->flags & KVM_MSI_VALID_DEVID))
		return -EINVAL;

	address = (u64)msi->address_hi << 32 | msi->address_lo;

	kvm_io_dev = kvm_io_bus_get_dev(kvm, KVM_MMIO_BUS, address);
	if (!kvm_io_dev)
		return -EINVAL;

	iodev = vgic_get_its_iodev(kvm_io_dev);
	if (!iodev)
		return -EINVAL;

	mutex_lock(&iodev->its->its_lock);
	ret = vgic_its_trigger_msi(kvm, iodev->its, msi->devid, msi->data);
	mutex_unlock(&iodev->its->its_lock);

	if (ret < 0)
		return ret;

	/*
	 * KVM_SIGNAL_MSI demands a return value > 0 for success and 0
	 * if the guest has blocked the MSI. So we map any LPI mapping
	 * related error to that.
	 */
	if (ret)
		return 0;
	else
		return 1;
}

/* Requires the its_lock to be held. */
static void its_free_ite(struct kvm *kvm, struct its_ite *ite)
{
	list_del(&ite->ite_list);

	/* This put matches the get in vgic_add_lpi. */
	if (ite->irq)
		vgic_put_irq(kvm, ite->irq);

	kfree(ite);
}

static u64 its_cmd_mask_field(u64 *its_cmd, int word, int shift, int size)
{
	return (le64_to_cpu(its_cmd[word]) >> shift) & (BIT_ULL(size) - 1);
}

#define its_cmd_get_command(cmd)	its_cmd_mask_field(cmd, 0,  0,  8)
#define its_cmd_get_deviceid(cmd)	its_cmd_mask_field(cmd, 0, 32, 32)
#define its_cmd_get_size(cmd)		(its_cmd_mask_field(cmd, 1,  0,  5) + 1)
#define its_cmd_get_id(cmd)		its_cmd_mask_field(cmd, 1,  0, 32)
#define its_cmd_get_physical_id(cmd)	its_cmd_mask_field(cmd, 1, 32, 32)
#define its_cmd_get_collection(cmd)	its_cmd_mask_field(cmd, 2,  0, 16)
#define its_cmd_get_ittaddr(cmd)	(its_cmd_mask_field(cmd, 2,  8, 44) << 8)
#define its_cmd_get_target_addr(cmd)	its_cmd_mask_field(cmd, 2, 16, 32)
#define its_cmd_get_validbit(cmd)	its_cmd_mask_field(cmd, 2, 63,  1)

/*
 * The DISCARD command frees an Interrupt Translation Table Entry (ITTE).
 * Must be called with the its_lock mutex held.
 */
static int vgic_its_cmd_handle_discard(struct kvm *kvm, struct vgic_its *its,
				       u64 *its_cmd)
{
	u32 device_id = its_cmd_get_deviceid(its_cmd);
	u32 event_id = its_cmd_get_id(its_cmd);
	struct its_ite *ite;


	ite = find_ite(its, device_id, event_id);
	if (ite && ite->collection) {
		/*
		 * Though the spec talks about removing the pending state, we
		 * don't bother here since we clear the ITTE anyway and the
		 * pending state is a property of the ITTE struct.
		 */
		its_free_ite(kvm, ite);
		return 0;
	}

	return E_ITS_DISCARD_UNMAPPED_INTERRUPT;
}

/*
 * The MOVI command moves an ITTE to a different collection.
 * Must be called with the its_lock mutex held.
 */
static int vgic_its_cmd_handle_movi(struct kvm *kvm, struct vgic_its *its,
				    u64 *its_cmd)
{
	u32 device_id = its_cmd_get_deviceid(its_cmd);
	u32 event_id = its_cmd_get_id(its_cmd);
	u32 coll_id = its_cmd_get_collection(its_cmd);
	struct kvm_vcpu *vcpu;
	struct its_ite *ite;
	struct its_collection *collection;

	ite = find_ite(its, device_id, event_id);
	if (!ite)
		return E_ITS_MOVI_UNMAPPED_INTERRUPT;

	if (!its_is_collection_mapped(ite->collection))
		return E_ITS_MOVI_UNMAPPED_COLLECTION;

	collection = find_collection(its, coll_id);
	if (!its_is_collection_mapped(collection))
		return E_ITS_MOVI_UNMAPPED_COLLECTION;

	ite->collection = collection;
	vcpu = kvm_get_vcpu(kvm, collection->target_addr);

	spin_lock(&ite->irq->irq_lock);
	ite->irq->target_vcpu = vcpu;
	spin_unlock(&ite->irq->irq_lock);

	return 0;
}

/*
 * Check whether an ID can be stored into the corresponding guest table.
 * For a direct table this is pretty easy, but gets a bit nasty for
 * indirect tables. We check whether the resulting guest physical address
 * is actually valid (covered by a memslot and guest accessible).
 * For this we have to read the respective first level entry.
 */
static bool vgic_its_check_id(struct vgic_its *its, u64 baser, u32 id,
			      gpa_t *eaddr)
{
	int l1_tbl_size = GITS_BASER_NR_PAGES(baser) * SZ_64K;
	u64 indirect_ptr, type = GITS_BASER_TYPE(baser);
	int esz = GITS_BASER_ENTRY_SIZE(baser);
	int index;
	gfn_t gfn;

	switch (type) {
	case GITS_BASER_TYPE_DEVICE:
		if (id >= BIT_ULL(VITS_TYPER_DEVBITS))
			return false;
		break;
	case GITS_BASER_TYPE_COLLECTION:
		/* as GITS_TYPER.CIL == 0, ITS supports 16-bit collection ID */
		if (id >= BIT_ULL(16))
			return false;
		break;
	default:
		return false;
	}

	if (!(baser & GITS_BASER_INDIRECT)) {
		phys_addr_t addr;

		if (id >= (l1_tbl_size / esz))
			return false;

		addr = BASER_ADDRESS(baser) + id * esz;
		gfn = addr >> PAGE_SHIFT;

		if (eaddr)
			*eaddr = addr;
		return kvm_is_visible_gfn(its->dev->kvm, gfn);
	}

	/* calculate and check the index into the 1st level */
	index = id / (SZ_64K / esz);
	if (index >= (l1_tbl_size / sizeof(u64)))
		return false;

	/* Each 1st level entry is represented by a 64-bit value. */
	if (kvm_read_guest(its->dev->kvm,
			   BASER_ADDRESS(baser) + index * sizeof(indirect_ptr),
			   &indirect_ptr, sizeof(indirect_ptr)))
		return false;

	indirect_ptr = le64_to_cpu(indirect_ptr);

	/* check the valid bit of the first level entry */
	if (!(indirect_ptr & BIT_ULL(63)))
		return false;

	/*
	 * Mask the guest physical address and calculate the frame number.
	 * Any address beyond our supported 48 bits of PA will be caught
	 * by the actual check in the final step.
	 */
	indirect_ptr &= GENMASK_ULL(51, 16);

	/* Find the address of the actual entry */
	index = id % (SZ_64K / esz);
	indirect_ptr += index * esz;
	gfn = indirect_ptr >> PAGE_SHIFT;

	if (eaddr)
		*eaddr = indirect_ptr;
	return kvm_is_visible_gfn(its->dev->kvm, gfn);
}

static int vgic_its_alloc_collection(struct vgic_its *its,
				     struct its_collection **colp,
				     u32 coll_id)
{
	struct its_collection *collection;

	if (!vgic_its_check_id(its, its->baser_coll_table, coll_id, NULL))
		return E_ITS_MAPC_COLLECTION_OOR;

	collection = kzalloc(sizeof(*collection), GFP_KERNEL);
	if (!collection)
		return -ENOMEM;

	collection->collection_id = coll_id;
	collection->target_addr = COLLECTION_NOT_MAPPED;

	list_add_tail(&collection->coll_list, &its->collection_list);
	*colp = collection;

	return 0;
}

static void vgic_its_free_collection(struct vgic_its *its, u32 coll_id)
{
	struct its_collection *collection;
	struct its_device *device;
	struct its_ite *ite;

	/*
	 * Clearing the mapping for that collection ID removes the
	 * entry from the list. If there wasn't any before, we can
	 * go home early.
	 */
	collection = find_collection(its, coll_id);
	if (!collection)
		return;

	for_each_lpi_its(device, ite, its)
		if (ite->collection &&
		    ite->collection->collection_id == coll_id)
			ite->collection = NULL;

	list_del(&collection->coll_list);
	kfree(collection);
}

/* Must be called with its_lock mutex held */
static struct its_ite *vgic_its_alloc_ite(struct its_device *device,
					  struct its_collection *collection,
					  u32 event_id)
{
	struct its_ite *ite;

	ite = kzalloc(sizeof(*ite), GFP_KERNEL);
	if (!ite)
		return ERR_PTR(-ENOMEM);

	ite->event_id	= event_id;
	ite->collection = collection;

	list_add_tail(&ite->ite_list, &device->itt_head);
	return ite;
}

/*
 * The MAPTI and MAPI commands map LPIs to ITTEs.
 * Must be called with its_lock mutex held.
 */
static int vgic_its_cmd_handle_mapi(struct kvm *kvm, struct vgic_its *its,
				    u64 *its_cmd)
{
	u32 device_id = its_cmd_get_deviceid(its_cmd);
	u32 event_id = its_cmd_get_id(its_cmd);
	u32 coll_id = its_cmd_get_collection(its_cmd);
	struct its_ite *ite;
	struct kvm_vcpu *vcpu = NULL;
	struct its_device *device;
	struct its_collection *collection, *new_coll = NULL;
	struct vgic_irq *irq;
	int lpi_nr;

	device = find_its_device(its, device_id);
	if (!device)
		return E_ITS_MAPTI_UNMAPPED_DEVICE;

	if (event_id >= BIT_ULL(device->num_eventid_bits))
		return E_ITS_MAPTI_ID_OOR;

	if (its_cmd_get_command(its_cmd) == GITS_CMD_MAPTI)
		lpi_nr = its_cmd_get_physical_id(its_cmd);
	else
		lpi_nr = event_id;
	if (lpi_nr < GIC_LPI_OFFSET ||
	    lpi_nr >= max_lpis_propbaser(kvm->arch.vgic.propbaser))
		return E_ITS_MAPTI_PHYSICALID_OOR;

	/* If there is an existing mapping, behavior is UNPREDICTABLE. */
	if (find_ite(its, device_id, event_id))
		return 0;

	collection = find_collection(its, coll_id);
	if (!collection) {
		int ret = vgic_its_alloc_collection(its, &collection, coll_id);
		if (ret)
			return ret;
		new_coll = collection;
	}

	ite = vgic_its_alloc_ite(device, collection, event_id);
	if (IS_ERR(ite)) {
		if (new_coll)
			vgic_its_free_collection(its, coll_id);
		return PTR_ERR(ite);
	}

	if (its_is_collection_mapped(collection))
		vcpu = kvm_get_vcpu(kvm, collection->target_addr);

	irq = vgic_add_lpi(kvm, lpi_nr, vcpu);
	if (IS_ERR(irq)) {
		if (new_coll)
			vgic_its_free_collection(its, coll_id);
		its_free_ite(kvm, ite);
		return PTR_ERR(irq);
	}
	ite->irq = irq;

	return 0;
}

/* Requires the its_lock to be held. */
static void vgic_its_unmap_device(struct kvm *kvm, struct its_device *device)
{
	struct its_ite *ite, *temp;

	/*
	 * The spec says that unmapping a device with still valid
	 * ITTEs associated is UNPREDICTABLE. We remove all ITTEs,
	 * since we cannot leave the memory unreferenced.
	 */
	list_for_each_entry_safe(ite, temp, &device->itt_head, ite_list)
		its_free_ite(kvm, ite);

	list_del(&device->dev_list);
	kfree(device);
}

/* Must be called with its_lock mutex held */
static struct its_device *vgic_its_alloc_device(struct vgic_its *its,
						u32 device_id, gpa_t itt_addr,
						u8 num_eventid_bits)
{
	struct its_device *device;

	device = kzalloc(sizeof(*device), GFP_KERNEL);
	if (!device)
		return ERR_PTR(-ENOMEM);

	device->device_id = device_id;
	device->itt_addr = itt_addr;
	device->num_eventid_bits = num_eventid_bits;
	INIT_LIST_HEAD(&device->itt_head);

	list_add_tail(&device->dev_list, &its->device_list);
	return device;
}

/*
 * MAPD maps or unmaps a device ID to Interrupt Translation Tables (ITTs).
 * Must be called with the its_lock mutex held.
 */
static int vgic_its_cmd_handle_mapd(struct kvm *kvm, struct vgic_its *its,
				    u64 *its_cmd)
{
	u32 device_id = its_cmd_get_deviceid(its_cmd);
	bool valid = its_cmd_get_validbit(its_cmd);
	u8 num_eventid_bits = its_cmd_get_size(its_cmd);
	gpa_t itt_addr = its_cmd_get_ittaddr(its_cmd);
	struct its_device *device;

	if (!vgic_its_check_id(its, its->baser_device_table, device_id, NULL))
		return E_ITS_MAPD_DEVICE_OOR;

	if (valid && num_eventid_bits > VITS_TYPER_IDBITS)
		return E_ITS_MAPD_ITTSIZE_OOR;

	device = find_its_device(its, device_id);

	/*
	 * The spec says that calling MAPD on an already mapped device
	 * invalidates all cached data for this device. We implement this
	 * by removing the mapping and re-establishing it.
	 */
	if (device)
		vgic_its_unmap_device(kvm, device);

	/*
	 * The spec does not say whether unmapping a not-mapped device
	 * is an error, so we are done in any case.
	 */
	if (!valid)
		return 0;

	device = vgic_its_alloc_device(its, device_id, itt_addr,
				       num_eventid_bits);
	if (IS_ERR(device))
		return PTR_ERR(device);

	return 0;
}

/*
 * The MAPC command maps collection IDs to redistributors.
 * Must be called with the its_lock mutex held.
 */
static int vgic_its_cmd_handle_mapc(struct kvm *kvm, struct vgic_its *its,
				    u64 *its_cmd)
{
	u16 coll_id;
	u32 target_addr;
	struct its_collection *collection;
	bool valid;

	valid = its_cmd_get_validbit(its_cmd);
	coll_id = its_cmd_get_collection(its_cmd);
	target_addr = its_cmd_get_target_addr(its_cmd);

	if (target_addr >= atomic_read(&kvm->online_vcpus))
		return E_ITS_MAPC_PROCNUM_OOR;

	if (!valid) {
		vgic_its_free_collection(its, coll_id);
	} else {
		collection = find_collection(its, coll_id);

		if (!collection) {
			int ret;

			ret = vgic_its_alloc_collection(its, &collection,
							coll_id);
			if (ret)
				return ret;
			collection->target_addr = target_addr;
		} else {
			collection->target_addr = target_addr;
			update_affinity_collection(kvm, its, collection);
		}
	}

	return 0;
}

/*
 * The CLEAR command removes the pending state for a particular LPI.
 * Must be called with the its_lock mutex held.
 */
static int vgic_its_cmd_handle_clear(struct kvm *kvm, struct vgic_its *its,
				     u64 *its_cmd)
{
	u32 device_id = its_cmd_get_deviceid(its_cmd);
	u32 event_id = its_cmd_get_id(its_cmd);
	struct its_ite *ite;


	ite = find_ite(its, device_id, event_id);
	if (!ite)
		return E_ITS_CLEAR_UNMAPPED_INTERRUPT;

	ite->irq->pending_latch = false;

	return 0;
}

/*
 * The INV command syncs the configuration bits from the memory table.
 * Must be called with the its_lock mutex held.
 */
static int vgic_its_cmd_handle_inv(struct kvm *kvm, struct vgic_its *its,
				   u64 *its_cmd)
{
	u32 device_id = its_cmd_get_deviceid(its_cmd);
	u32 event_id = its_cmd_get_id(its_cmd);
	struct its_ite *ite;


	ite = find_ite(its, device_id, event_id);
	if (!ite)
		return E_ITS_INV_UNMAPPED_INTERRUPT;

	return update_lpi_config(kvm, ite->irq, NULL);
}

/*
 * The INVALL command requests flushing of all IRQ data in this collection.
 * Find the VCPU mapped to that collection, then iterate over the VM's list
 * of mapped LPIs and update the configuration for each IRQ which targets
 * the specified vcpu. The configuration will be read from the in-memory
 * configuration table.
 * Must be called with the its_lock mutex held.
 */
static int vgic_its_cmd_handle_invall(struct kvm *kvm, struct vgic_its *its,
				      u64 *its_cmd)
{
	u32 coll_id = its_cmd_get_collection(its_cmd);
	struct its_collection *collection;
	struct kvm_vcpu *vcpu;
	struct vgic_irq *irq;
	u32 *intids;
	int irq_count, i;

	collection = find_collection(its, coll_id);
	if (!its_is_collection_mapped(collection))
		return E_ITS_INVALL_UNMAPPED_COLLECTION;

	vcpu = kvm_get_vcpu(kvm, collection->target_addr);

	irq_count = vgic_copy_lpi_list(vcpu, &intids);
	if (irq_count < 0)
		return irq_count;

	for (i = 0; i < irq_count; i++) {
		irq = vgic_get_irq(kvm, NULL, intids[i]);
		if (!irq)
			continue;
		update_lpi_config(kvm, irq, vcpu);
		vgic_put_irq(kvm, irq);
	}

	kfree(intids);

	return 0;
}

/*
 * The MOVALL command moves the pending state of all IRQs targeting one
 * redistributor to another. We don't hold the pending state in the VCPUs,
 * but in the IRQs instead, so there is really not much to do for us here.
 * However the spec says that no IRQ must target the old redistributor
 * afterwards, so we make sure that no LPI is using the associated target_vcpu.
 * This command affects all LPIs in the system that target that redistributor.
 */
static int vgic_its_cmd_handle_movall(struct kvm *kvm, struct vgic_its *its,
				      u64 *its_cmd)
{
	struct vgic_dist *dist = &kvm->arch.vgic;
	u32 target1_addr = its_cmd_get_target_addr(its_cmd);
	u32 target2_addr = its_cmd_mask_field(its_cmd, 3, 16, 32);
	struct kvm_vcpu *vcpu1, *vcpu2;
	struct vgic_irq *irq;

	if (target1_addr >= atomic_read(&kvm->online_vcpus) ||
	    target2_addr >= atomic_read(&kvm->online_vcpus))
		return E_ITS_MOVALL_PROCNUM_OOR;

	if (target1_addr == target2_addr)
		return 0;

	vcpu1 = kvm_get_vcpu(kvm, target1_addr);
	vcpu2 = kvm_get_vcpu(kvm, target2_addr);

	spin_lock(&dist->lpi_list_lock);

	list_for_each_entry(irq, &dist->lpi_list_head, lpi_list) {
		spin_lock(&irq->irq_lock);

		if (irq->target_vcpu == vcpu1)
			irq->target_vcpu = vcpu2;

		spin_unlock(&irq->irq_lock);
	}

	spin_unlock(&dist->lpi_list_lock);

	return 0;
}

/*
 * The INT command injects the LPI associated with that DevID/EvID pair.
 * Must be called with the its_lock mutex held.
 */
static int vgic_its_cmd_handle_int(struct kvm *kvm, struct vgic_its *its,
				   u64 *its_cmd)
{
	u32 msi_data = its_cmd_get_id(its_cmd);
	u64 msi_devid = its_cmd_get_deviceid(its_cmd);

	return vgic_its_trigger_msi(kvm, its, msi_devid, msi_data);
}

/*
 * This function is called with the its_cmd lock held, but the ITS data
 * structure lock dropped.
 */
static int vgic_its_handle_command(struct kvm *kvm, struct vgic_its *its,
				   u64 *its_cmd)
{
	int ret = -ENODEV;

	mutex_lock(&its->its_lock);
	switch (its_cmd_get_command(its_cmd)) {
	case GITS_CMD_MAPD:
		ret = vgic_its_cmd_handle_mapd(kvm, its, its_cmd);
		break;
	case GITS_CMD_MAPC:
		ret = vgic_its_cmd_handle_mapc(kvm, its, its_cmd);
		break;
	case GITS_CMD_MAPI:
		ret = vgic_its_cmd_handle_mapi(kvm, its, its_cmd);
		break;
	case GITS_CMD_MAPTI:
		ret = vgic_its_cmd_handle_mapi(kvm, its, its_cmd);
		break;
	case GITS_CMD_MOVI:
		ret = vgic_its_cmd_handle_movi(kvm, its, its_cmd);
		break;
	case GITS_CMD_DISCARD:
		ret = vgic_its_cmd_handle_discard(kvm, its, its_cmd);
		break;
	case GITS_CMD_CLEAR:
		ret = vgic_its_cmd_handle_clear(kvm, its, its_cmd);
		break;
	case GITS_CMD_MOVALL:
		ret = vgic_its_cmd_handle_movall(kvm, its, its_cmd);
		break;
	case GITS_CMD_INT:
		ret = vgic_its_cmd_handle_int(kvm, its, its_cmd);
		break;
	case GITS_CMD_INV:
		ret = vgic_its_cmd_handle_inv(kvm, its, its_cmd);
		break;
	case GITS_CMD_INVALL:
		ret = vgic_its_cmd_handle_invall(kvm, its, its_cmd);
		break;
	case GITS_CMD_SYNC:
		/* we ignore this command: we are in sync all of the time */
		ret = 0;
		break;
	}
	mutex_unlock(&its->its_lock);

	return ret;
}

static u64 vgic_sanitise_its_baser(u64 reg)
{
	reg = vgic_sanitise_field(reg, GITS_BASER_SHAREABILITY_MASK,
				  GITS_BASER_SHAREABILITY_SHIFT,
				  vgic_sanitise_shareability);
	reg = vgic_sanitise_field(reg, GITS_BASER_INNER_CACHEABILITY_MASK,
				  GITS_BASER_INNER_CACHEABILITY_SHIFT,
				  vgic_sanitise_inner_cacheability);
	reg = vgic_sanitise_field(reg, GITS_BASER_OUTER_CACHEABILITY_MASK,
				  GITS_BASER_OUTER_CACHEABILITY_SHIFT,
				  vgic_sanitise_outer_cacheability);

	/* Bits 15:12 contain bits 51:48 of the PA, which we don't support. */
	reg &= ~GENMASK_ULL(15, 12);

	/* We support only one (ITS) page size: 64K */
	reg = (reg & ~GITS_BASER_PAGE_SIZE_MASK) | GITS_BASER_PAGE_SIZE_64K;

	return reg;
}

static u64 vgic_sanitise_its_cbaser(u64 reg)
{
	reg = vgic_sanitise_field(reg, GITS_CBASER_SHAREABILITY_MASK,
				  GITS_CBASER_SHAREABILITY_SHIFT,
				  vgic_sanitise_shareability);
	reg = vgic_sanitise_field(reg, GITS_CBASER_INNER_CACHEABILITY_MASK,
				  GITS_CBASER_INNER_CACHEABILITY_SHIFT,
				  vgic_sanitise_inner_cacheability);
	reg = vgic_sanitise_field(reg, GITS_CBASER_OUTER_CACHEABILITY_MASK,
				  GITS_CBASER_OUTER_CACHEABILITY_SHIFT,
				  vgic_sanitise_outer_cacheability);

	/*
	 * Sanitise the physical address to be 64k aligned.
	 * Also limit the physical addresses to 48 bits.
	 */
	reg &= ~(GENMASK_ULL(51, 48) | GENMASK_ULL(15, 12));

	return reg;
}

static unsigned long vgic_mmio_read_its_cbaser(struct kvm *kvm,
					       struct vgic_its *its,
					       gpa_t addr, unsigned int len)
{
	return extract_bytes(its->cbaser, addr & 7, len);
}

static void vgic_mmio_write_its_cbaser(struct kvm *kvm, struct vgic_its *its,
				       gpa_t addr, unsigned int len,
				       unsigned long val)
{
	/* When GITS_CTLR.Enable is 1, this register is RO. */
	if (its->enabled)
		return;

	mutex_lock(&its->cmd_lock);
	its->cbaser = update_64bit_reg(its->cbaser, addr & 7, len, val);
	its->cbaser = vgic_sanitise_its_cbaser(its->cbaser);
	its->creadr = 0;
	/*
	 * CWRITER is architecturally UNKNOWN on reset, but we need to reset
	 * it to CREADR to make sure we start with an empty command buffer.
	 */
	its->cwriter = its->creadr;
	mutex_unlock(&its->cmd_lock);
}

#define ITS_CMD_BUFFER_SIZE(baser)	((((baser) & 0xff) + 1) << 12)
#define ITS_CMD_SIZE			32
#define ITS_CMD_OFFSET(reg)		((reg) & GENMASK(19, 5))

/* Must be called with the cmd_lock held. */
static void vgic_its_process_commands(struct kvm *kvm, struct vgic_its *its)
{
	gpa_t cbaser;
	u64 cmd_buf[4];

	/* Commands are only processed when the ITS is enabled. */
	if (!its->enabled)
		return;

	cbaser = CBASER_ADDRESS(its->cbaser);

	while (its->cwriter != its->creadr) {
		int ret = kvm_read_guest(kvm, cbaser + its->creadr,
					 cmd_buf, ITS_CMD_SIZE);
		/*
		 * If kvm_read_guest() fails, this could be due to the guest
		 * programming a bogus value in CBASER or something else going
		 * wrong from which we cannot easily recover.
		 * According to section 6.3.2 in the GICv3 spec we can just
		 * ignore that command then.
		 */
		if (!ret)
			vgic_its_handle_command(kvm, its, cmd_buf);

		its->creadr += ITS_CMD_SIZE;
		if (its->creadr == ITS_CMD_BUFFER_SIZE(its->cbaser))
			its->creadr = 0;
	}
}

/*
 * By writing to CWRITER the guest announces new commands to be processed.
 * To avoid any races in the first place, we take the its_cmd lock, which
 * protects our ring buffer variables, so that there is only one user
 * per ITS handling commands at a given time.
 */
static void vgic_mmio_write_its_cwriter(struct kvm *kvm, struct vgic_its *its,
					gpa_t addr, unsigned int len,
					unsigned long val)
{
	u64 reg;

	if (!its)
		return;

	mutex_lock(&its->cmd_lock);

	reg = update_64bit_reg(its->cwriter, addr & 7, len, val);
	reg = ITS_CMD_OFFSET(reg);
	if (reg >= ITS_CMD_BUFFER_SIZE(its->cbaser)) {
		mutex_unlock(&its->cmd_lock);
		return;
	}
	its->cwriter = reg;

	vgic_its_process_commands(kvm, its);

	mutex_unlock(&its->cmd_lock);
}

static unsigned long vgic_mmio_read_its_cwriter(struct kvm *kvm,
						struct vgic_its *its,
						gpa_t addr, unsigned int len)
{
	return extract_bytes(its->cwriter, addr & 0x7, len);
}

static unsigned long vgic_mmio_read_its_creadr(struct kvm *kvm,
					       struct vgic_its *its,
					       gpa_t addr, unsigned int len)
{
	return extract_bytes(its->creadr, addr & 0x7, len);
}

static int vgic_mmio_uaccess_write_its_creadr(struct kvm *kvm,
					      struct vgic_its *its,
					      gpa_t addr, unsigned int len,
					      unsigned long val)
{
	u32 cmd_offset;
	int ret = 0;

	mutex_lock(&its->cmd_lock);

	if (its->enabled) {
		ret = -EBUSY;
		goto out;
	}

	cmd_offset = ITS_CMD_OFFSET(val);
	if (cmd_offset >= ITS_CMD_BUFFER_SIZE(its->cbaser)) {
		ret = -EINVAL;
		goto out;
	}

	its->creadr = cmd_offset;
out:
	mutex_unlock(&its->cmd_lock);
	return ret;
}

#define BASER_INDEX(addr) (((addr) / sizeof(u64)) & 0x7)
static unsigned long vgic_mmio_read_its_baser(struct kvm *kvm,
					      struct vgic_its *its,
					      gpa_t addr, unsigned int len)
{
	u64 reg;

	switch (BASER_INDEX(addr)) {
	case 0:
		reg = its->baser_device_table;
		break;
	case 1:
		reg = its->baser_coll_table;
		break;
	default:
		reg = 0;
		break;
	}

	return extract_bytes(reg, addr & 7, len);
}

#define GITS_BASER_RO_MASK	(GENMASK_ULL(52, 48) | GENMASK_ULL(58, 56))
static void vgic_mmio_write_its_baser(struct kvm *kvm,
				      struct vgic_its *its,
				      gpa_t addr, unsigned int len,
				      unsigned long val)
{
	const struct vgic_its_abi *abi = vgic_its_get_abi(its);
	u64 entry_size, device_type;
	u64 reg, *regptr, clearbits = 0;

	/* When GITS_CTLR.Enable is 1, we ignore write accesses. */
	if (its->enabled)
		return;

	switch (BASER_INDEX(addr)) {
	case 0:
		regptr = &its->baser_device_table;
		entry_size = abi->dte_esz;
		device_type = GITS_BASER_TYPE_DEVICE;
		break;
	case 1:
		regptr = &its->baser_coll_table;
		entry_size = abi->cte_esz;
		device_type = GITS_BASER_TYPE_COLLECTION;
		clearbits = GITS_BASER_INDIRECT;
		break;
	default:
		return;
	}

	reg = update_64bit_reg(*regptr, addr & 7, len, val);
	reg &= ~GITS_BASER_RO_MASK;
	reg &= ~clearbits;

	reg |= (entry_size - 1) << GITS_BASER_ENTRY_SIZE_SHIFT;
	reg |= device_type << GITS_BASER_TYPE_SHIFT;
	reg = vgic_sanitise_its_baser(reg);

	*regptr = reg;
}

static unsigned long vgic_mmio_read_its_ctlr(struct kvm *vcpu,
					     struct vgic_its *its,
					     gpa_t addr, unsigned int len)
{
	u32 reg = 0;

	mutex_lock(&its->cmd_lock);
	if (its->creadr == its->cwriter)
		reg |= GITS_CTLR_QUIESCENT;
	if (its->enabled)
		reg |= GITS_CTLR_ENABLE;
	mutex_unlock(&its->cmd_lock);

	return reg;
}

static void vgic_mmio_write_its_ctlr(struct kvm *kvm, struct vgic_its *its,
				     gpa_t addr, unsigned int len,
				     unsigned long val)
{
	mutex_lock(&its->cmd_lock);

	/*
	 * It is UNPREDICTABLE to enable the ITS if any of the CBASER or
	 * device/collection BASER are invalid
	 */
	if (!its->enabled && (val & GITS_CTLR_ENABLE) &&
		(!(its->baser_device_table & GITS_BASER_VALID) ||
		 !(its->baser_coll_table & GITS_BASER_VALID) ||
		 !(its->cbaser & GITS_CBASER_VALID)))
		goto out;

	its->enabled = !!(val & GITS_CTLR_ENABLE);

	/*
	 * Try to process any pending commands. This function bails out early
	 * if the ITS is disabled or no commands have been queued.
	 */
	vgic_its_process_commands(kvm, its);

out:
	mutex_unlock(&its->cmd_lock);
}

#define REGISTER_ITS_DESC(off, rd, wr, length, acc)		\
{								\
	.reg_offset = off,					\
	.len = length,						\
	.access_flags = acc,					\
	.its_read = rd,						\
	.its_write = wr,					\
}

#define REGISTER_ITS_DESC_UACCESS(off, rd, wr, uwr, length, acc)\
{								\
	.reg_offset = off,					\
	.len = length,						\
	.access_flags = acc,					\
	.its_read = rd,						\
	.its_write = wr,					\
	.uaccess_its_write = uwr,				\
}

static void its_mmio_write_wi(struct kvm *kvm, struct vgic_its *its,
			      gpa_t addr, unsigned int len, unsigned long val)
{
	/* Ignore */
}

static struct vgic_register_region its_registers[] = {
	REGISTER_ITS_DESC(GITS_CTLR,
		vgic_mmio_read_its_ctlr, vgic_mmio_write_its_ctlr, 4,
		VGIC_ACCESS_32bit),
	REGISTER_ITS_DESC_UACCESS(GITS_IIDR,
		vgic_mmio_read_its_iidr, its_mmio_write_wi,
		vgic_mmio_uaccess_write_its_iidr, 4,
		VGIC_ACCESS_32bit),
	REGISTER_ITS_DESC(GITS_TYPER,
		vgic_mmio_read_its_typer, its_mmio_write_wi, 8,
		VGIC_ACCESS_64bit | VGIC_ACCESS_32bit),
	REGISTER_ITS_DESC(GITS_CBASER,
		vgic_mmio_read_its_cbaser, vgic_mmio_write_its_cbaser, 8,
		VGIC_ACCESS_64bit | VGIC_ACCESS_32bit),
	REGISTER_ITS_DESC(GITS_CWRITER,
		vgic_mmio_read_its_cwriter, vgic_mmio_write_its_cwriter, 8,
		VGIC_ACCESS_64bit | VGIC_ACCESS_32bit),
	REGISTER_ITS_DESC_UACCESS(GITS_CREADR,
		vgic_mmio_read_its_creadr, its_mmio_write_wi,
		vgic_mmio_uaccess_write_its_creadr, 8,
		VGIC_ACCESS_64bit | VGIC_ACCESS_32bit),
	REGISTER_ITS_DESC(GITS_BASER,
		vgic_mmio_read_its_baser, vgic_mmio_write_its_baser, 0x40,
		VGIC_ACCESS_64bit | VGIC_ACCESS_32bit),
	REGISTER_ITS_DESC(GITS_IDREGS_BASE,
		vgic_mmio_read_its_idregs, its_mmio_write_wi, 0x30,
		VGIC_ACCESS_32bit),
};

/* This is called on setting the LPI enable bit in the redistributor. */
void vgic_enable_lpis(struct kvm_vcpu *vcpu)
{
	if (!(vcpu->arch.vgic_cpu.pendbaser & GICR_PENDBASER_PTZ))
		its_sync_lpi_pending_table(vcpu);
}

static int vgic_register_its_iodev(struct kvm *kvm, struct vgic_its *its,
				   u64 addr)
{
	struct vgic_io_device *iodev = &its->iodev;
	int ret;

	mutex_lock(&kvm->slots_lock);
	if (!IS_VGIC_ADDR_UNDEF(its->vgic_its_base)) {
		ret = -EBUSY;
		goto out;
	}

	its->vgic_its_base = addr;
	iodev->regions = its_registers;
	iodev->nr_regions = ARRAY_SIZE(its_registers);
	kvm_iodevice_init(&iodev->dev, &kvm_io_gic_ops);

	iodev->base_addr = its->vgic_its_base;
	iodev->iodev_type = IODEV_ITS;
	iodev->its = its;
	ret = kvm_io_bus_register_dev(kvm, KVM_MMIO_BUS, iodev->base_addr,
				      KVM_VGIC_V3_ITS_SIZE, &iodev->dev);
out:
	mutex_unlock(&kvm->slots_lock);

	return ret;
}

#define INITIAL_BASER_VALUE						  \
	(GIC_BASER_CACHEABILITY(GITS_BASER, INNER, RaWb)		| \
	 GIC_BASER_CACHEABILITY(GITS_BASER, OUTER, SameAsInner)		| \
	 GIC_BASER_SHAREABILITY(GITS_BASER, InnerShareable)		| \
	 GITS_BASER_PAGE_SIZE_64K)

#define INITIAL_PROPBASER_VALUE						  \
	(GIC_BASER_CACHEABILITY(GICR_PROPBASER, INNER, RaWb)		| \
	 GIC_BASER_CACHEABILITY(GICR_PROPBASER, OUTER, SameAsInner)	| \
	 GIC_BASER_SHAREABILITY(GICR_PROPBASER, InnerShareable))

static int vgic_its_create(struct kvm_device *dev, u32 type)
{
	struct vgic_its *its;

	if (type != KVM_DEV_TYPE_ARM_VGIC_ITS)
		return -ENODEV;

	its = kzalloc(sizeof(struct vgic_its), GFP_KERNEL);
	if (!its)
		return -ENOMEM;

	mutex_init(&its->its_lock);
	mutex_init(&its->cmd_lock);

	its->vgic_its_base = VGIC_ADDR_UNDEF;

	INIT_LIST_HEAD(&its->device_list);
	INIT_LIST_HEAD(&its->collection_list);

	dev->kvm->arch.vgic.msis_require_devid = true;
	dev->kvm->arch.vgic.has_its = true;
	its->enabled = false;
	its->dev = dev;

	its->baser_device_table = INITIAL_BASER_VALUE			|
		((u64)GITS_BASER_TYPE_DEVICE << GITS_BASER_TYPE_SHIFT);
	its->baser_coll_table = INITIAL_BASER_VALUE |
		((u64)GITS_BASER_TYPE_COLLECTION << GITS_BASER_TYPE_SHIFT);
	dev->kvm->arch.vgic.propbaser = INITIAL_PROPBASER_VALUE;

	dev->private = its;

	return vgic_its_set_abi(its, NR_ITS_ABIS - 1);
}

static void vgic_its_free_device(struct kvm *kvm, struct its_device *dev)
{
	struct its_ite *ite, *tmp;

	list_for_each_entry_safe(ite, tmp, &dev->itt_head, ite_list)
		its_free_ite(kvm, ite);
	list_del(&dev->dev_list);
	kfree(dev);
}

static void vgic_its_destroy(struct kvm_device *kvm_dev)
{
	struct kvm *kvm = kvm_dev->kvm;
	struct vgic_its *its = kvm_dev->private;
	struct list_head *cur, *temp;

	/*
	 * We may end up here without the lists ever having been initialized.
	 * Check this and bail out early to avoid dereferencing a NULL pointer.
	 */
	if (!its->device_list.next)
		return;

	mutex_lock(&its->its_lock);
	list_for_each_safe(cur, temp, &its->device_list) {
		struct its_device *dev;

		dev = list_entry(cur, struct its_device, dev_list);
		vgic_its_free_device(kvm, dev);
	}

	list_for_each_safe(cur, temp, &its->collection_list) {
		struct its_collection *coll;

		coll = list_entry(cur, struct its_collection, coll_list);
		list_del(cur);
		kfree(coll);
	}
	mutex_unlock(&its->its_lock);

	kfree(its);
}

int vgic_its_has_attr_regs(struct kvm_device *dev,
			   struct kvm_device_attr *attr)
{
	const struct vgic_register_region *region;
	gpa_t offset = attr->attr;
	int align;

	align = (offset < GITS_TYPER) || (offset >= GITS_PIDR4) ? 0x3 : 0x7;

	if (offset & align)
		return -EINVAL;

	region = vgic_find_mmio_region(its_registers,
				       ARRAY_SIZE(its_registers),
				       offset);
	if (!region)
		return -ENXIO;

	return 0;
}

int vgic_its_attr_regs_access(struct kvm_device *dev,
			      struct kvm_device_attr *attr,
			      u64 *reg, bool is_write)
{
	const struct vgic_register_region *region;
	struct vgic_its *its;
	gpa_t addr, offset;
	unsigned int len;
	int align, ret = 0;

	its = dev->private;
	offset = attr->attr;

	/*
	 * Although the spec supports upper/lower 32-bit accesses to
	 * 64-bit ITS registers, the userspace ABI requires 64-bit
	 * accesses to all 64-bit wide registers. We therefore only
	 * support 32-bit accesses to GITS_CTLR, GITS_IIDR and GITS ID
	 * registers
	 */
	if ((offset < GITS_TYPER) || (offset >= GITS_PIDR4))
		align = 0x3;
	else
		align = 0x7;

	if (offset & align)
		return -EINVAL;

	mutex_lock(&dev->kvm->lock);

	if (IS_VGIC_ADDR_UNDEF(its->vgic_its_base)) {
		ret = -ENXIO;
		goto out;
	}

	region = vgic_find_mmio_region(its_registers,
				       ARRAY_SIZE(its_registers),
				       offset);
	if (!region) {
		ret = -ENXIO;
		goto out;
	}

	if (!lock_all_vcpus(dev->kvm)) {
		ret = -EBUSY;
		goto out;
	}

	addr = its->vgic_its_base + offset;

	len = region->access_flags & VGIC_ACCESS_64bit ? 8 : 4;

	if (is_write) {
		if (region->uaccess_its_write)
			ret = region->uaccess_its_write(dev->kvm, its, addr,
							len, *reg);
		else
			region->its_write(dev->kvm, its, addr, len, *reg);
	} else {
		*reg = region->its_read(dev->kvm, its, addr, len);
	}
	unlock_all_vcpus(dev->kvm);
out:
	mutex_unlock(&dev->kvm->lock);
	return ret;
}

static u32 compute_next_devid_offset(struct list_head *h,
				     struct its_device *dev)
{
	struct its_device *next;
	u32 next_offset;

	if (list_is_last(&dev->dev_list, h))
		return 0;
	next = list_next_entry(dev, dev_list);
	next_offset = next->device_id - dev->device_id;

	return min_t(u32, next_offset, VITS_DTE_MAX_DEVID_OFFSET);
}

static u32 compute_next_eventid_offset(struct list_head *h, struct its_ite *ite)
{
	struct its_ite *next;
	u32 next_offset;

	if (list_is_last(&ite->ite_list, h))
		return 0;
	next = list_next_entry(ite, ite_list);
	next_offset = next->event_id - ite->event_id;

	return min_t(u32, next_offset, VITS_ITE_MAX_EVENTID_OFFSET);
}

/**
 * entry_fn_t - Callback called on a table entry restore path
 * @its: its handle
 * @id: id of the entry
 * @entry: pointer to the entry
 * @opaque: pointer to an opaque data
 *
 * Return: < 0 on error, 0 if last element was identified, id offset to next
 * element otherwise
 */
typedef int (*entry_fn_t)(struct vgic_its *its, u32 id, void *entry,
			  void *opaque);

/**
 * scan_its_table - Scan a contiguous table in guest RAM and applies a function
 * to each entry
 *
 * @its: its handle
 * @base: base gpa of the table
 * @size: size of the table in bytes
 * @esz: entry size in bytes
 * @start_id: the ID of the first entry in the table
 * (non zero for 2d level tables)
 * @fn: function to apply on each entry
 *
 * Return: < 0 on error, 0 if last element was identified, 1 otherwise
 * (the last element may not be found on second level tables)
 */
static int scan_its_table(struct vgic_its *its, gpa_t base, int size, int esz,
			  int start_id, entry_fn_t fn, void *opaque)
{
	struct kvm *kvm = its->dev->kvm;
	unsigned long len = size;
	int id = start_id;
	gpa_t gpa = base;
	char entry[esz];
	int ret;

	memset(entry, 0, esz);

	while (len > 0) {
		int next_offset;
		size_t byte_offset;

		ret = kvm_read_guest(kvm, gpa, entry, esz);
		if (ret)
			return ret;

		next_offset = fn(its, id, entry, opaque);
		if (next_offset <= 0)
			return next_offset;

		byte_offset = next_offset * esz;
		id += next_offset;
		gpa += byte_offset;
		len -= byte_offset;
	}
	return 1;
}

/**
 * vgic_its_save_ite - Save an interrupt translation entry at @gpa
 */
static int vgic_its_save_ite(struct vgic_its *its, struct its_device *dev,
			      struct its_ite *ite, gpa_t gpa, int ite_esz)
{
	struct kvm *kvm = its->dev->kvm;
	u32 next_offset;
	u64 val;

	next_offset = compute_next_eventid_offset(&dev->itt_head, ite);
	val = ((u64)next_offset << KVM_ITS_ITE_NEXT_SHIFT) |
	       ((u64)ite->irq->intid << KVM_ITS_ITE_PINTID_SHIFT) |
		ite->collection->collection_id;
	val = cpu_to_le64(val);
	return kvm_write_guest(kvm, gpa, &val, ite_esz);
}

/**
 * vgic_its_restore_ite - restore an interrupt translation entry
 * @event_id: id used for indexing
 * @ptr: pointer to the ITE entry
 * @opaque: pointer to the its_device
 */
static int vgic_its_restore_ite(struct vgic_its *its, u32 event_id,
				void *ptr, void *opaque)
{
	struct its_device *dev = (struct its_device *)opaque;
	struct its_collection *collection;
	struct kvm *kvm = its->dev->kvm;
	struct kvm_vcpu *vcpu = NULL;
	u64 val;
	u64 *p = (u64 *)ptr;
	struct vgic_irq *irq;
	u32 coll_id, lpi_id;
	struct its_ite *ite;
	u32 offset;

	val = *p;

	val = le64_to_cpu(val);

	coll_id = val & KVM_ITS_ITE_ICID_MASK;
	lpi_id = (val & KVM_ITS_ITE_PINTID_MASK) >> KVM_ITS_ITE_PINTID_SHIFT;

	if (!lpi_id)
		return 1; /* invalid entry, no choice but to scan next entry */

	if (lpi_id < VGIC_MIN_LPI)
		return -EINVAL;

	offset = val >> KVM_ITS_ITE_NEXT_SHIFT;
	if (event_id + offset >= BIT_ULL(dev->num_eventid_bits))
		return -EINVAL;

	collection = find_collection(its, coll_id);
	if (!collection)
		return -EINVAL;

	ite = vgic_its_alloc_ite(dev, collection, event_id);
	if (IS_ERR(ite))
		return PTR_ERR(ite);

	if (its_is_collection_mapped(collection))
		vcpu = kvm_get_vcpu(kvm, collection->target_addr);

	irq = vgic_add_lpi(kvm, lpi_id, vcpu);
	if (IS_ERR(irq))
		return PTR_ERR(irq);
	ite->irq = irq;

	return offset;
}

static int vgic_its_ite_cmp(void *priv, struct list_head *a,
			    struct list_head *b)
{
	struct its_ite *itea = container_of(a, struct its_ite, ite_list);
	struct its_ite *iteb = container_of(b, struct its_ite, ite_list);

	if (itea->event_id < iteb->event_id)
		return -1;
	else
		return 1;
}

static int vgic_its_save_itt(struct vgic_its *its, struct its_device *device)
{
	const struct vgic_its_abi *abi = vgic_its_get_abi(its);
	gpa_t base = device->itt_addr;
	struct its_ite *ite;
	int ret;
	int ite_esz = abi->ite_esz;

	list_sort(NULL, &device->itt_head, vgic_its_ite_cmp);

	list_for_each_entry(ite, &device->itt_head, ite_list) {
		gpa_t gpa = base + ite->event_id * ite_esz;

		ret = vgic_its_save_ite(its, device, ite, gpa, ite_esz);
		if (ret)
			return ret;
	}
	return 0;
}

/**
 * vgic_its_restore_itt - restore the ITT of a device
 *
 * @its: its handle
 * @dev: device handle
 *
 * Return 0 on success, < 0 on error
 */
static int vgic_its_restore_itt(struct vgic_its *its, struct its_device *dev)
{
	const struct vgic_its_abi *abi = vgic_its_get_abi(its);
	gpa_t base = dev->itt_addr;
	int ret;
	int ite_esz = abi->ite_esz;
	size_t max_size = BIT_ULL(dev->num_eventid_bits) * ite_esz;

	ret = scan_its_table(its, base, max_size, ite_esz, 0,
			     vgic_its_restore_ite, dev);

	/* scan_its_table returns +1 if all ITEs are invalid */
	if (ret > 0)
		ret = 0;

	return ret;
}

/**
 * vgic_its_save_dte - Save a device table entry at a given GPA
 *
 * @its: ITS handle
 * @dev: ITS device
 * @ptr: GPA
 */
static int vgic_its_save_dte(struct vgic_its *its, struct its_device *dev,
			     gpa_t ptr, int dte_esz)
{
	struct kvm *kvm = its->dev->kvm;
	u64 val, itt_addr_field;
	u32 next_offset;

	itt_addr_field = dev->itt_addr >> 8;
	next_offset = compute_next_devid_offset(&its->device_list, dev);
	val = (1ULL << KVM_ITS_DTE_VALID_SHIFT |
	       ((u64)next_offset << KVM_ITS_DTE_NEXT_SHIFT) |
	       (itt_addr_field << KVM_ITS_DTE_ITTADDR_SHIFT) |
		(dev->num_eventid_bits - 1));
	val = cpu_to_le64(val);
	return kvm_write_guest(kvm, ptr, &val, dte_esz);
}

/**
 * vgic_its_restore_dte - restore a device table entry
 *
 * @its: its handle
 * @id: device id the DTE corresponds to
 * @ptr: kernel VA where the 8 byte DTE is located
 * @opaque: unused
 *
 * Return: < 0 on error, 0 if the dte is the last one, id offset to the
 * next dte otherwise
 */
static int vgic_its_restore_dte(struct vgic_its *its, u32 id,
				void *ptr, void *opaque)
{
	struct its_device *dev;
	gpa_t itt_addr;
	u8 num_eventid_bits;
	u64 entry = *(u64 *)ptr;
	bool valid;
	u32 offset;
	int ret;

	entry = le64_to_cpu(entry);

	valid = entry >> KVM_ITS_DTE_VALID_SHIFT;
	num_eventid_bits = (entry & KVM_ITS_DTE_SIZE_MASK) + 1;
	itt_addr = ((entry & KVM_ITS_DTE_ITTADDR_MASK)
			>> KVM_ITS_DTE_ITTADDR_SHIFT) << 8;

	if (!valid)
		return 1;

	/* dte entry is valid */
	offset = (entry & KVM_ITS_DTE_NEXT_MASK) >> KVM_ITS_DTE_NEXT_SHIFT;

	dev = vgic_its_alloc_device(its, id, itt_addr, num_eventid_bits);
	if (IS_ERR(dev))
		return PTR_ERR(dev);

	ret = vgic_its_restore_itt(its, dev);
	if (ret) {
		vgic_its_free_device(its->dev->kvm, dev);
		return ret;
	}

	return offset;
}

static int vgic_its_device_cmp(void *priv, struct list_head *a,
			       struct list_head *b)
{
	struct its_device *deva = container_of(a, struct its_device, dev_list);
	struct its_device *devb = container_of(b, struct its_device, dev_list);

	if (deva->device_id < devb->device_id)
		return -1;
	else
		return 1;
}

/**
 * vgic_its_save_device_tables - Save the device table and all ITT
 * into guest RAM
 *
 * L1/L2 handling is hidden by vgic_its_check_id() helper which directly
 * returns the GPA of the device entry
 */
static int vgic_its_save_device_tables(struct vgic_its *its)
{
	const struct vgic_its_abi *abi = vgic_its_get_abi(its);
	u64 baser = its->baser_device_table;
	struct its_device *dev;
	int dte_esz = abi->dte_esz;

	if (!(baser & GITS_BASER_VALID))
		return 0;

	list_sort(NULL, &its->device_list, vgic_its_device_cmp);

	list_for_each_entry(dev, &its->device_list, dev_list) {
		int ret;
		gpa_t eaddr;

		if (!vgic_its_check_id(its, baser,
				       dev->device_id, &eaddr))
			return -EINVAL;

		ret = vgic_its_save_itt(its, dev);
		if (ret)
			return ret;

		ret = vgic_its_save_dte(its, dev, eaddr, dte_esz);
		if (ret)
			return ret;
	}
	return 0;
}

/**
 * handle_l1_dte - callback used for L1 device table entries (2 stage case)
 *
 * @its: its handle
 * @id: index of the entry in the L1 table
 * @addr: kernel VA
 * @opaque: unused
 *
 * L1 table entries are scanned by steps of 1 entry
 * Return < 0 if error, 0 if last dte was found when scanning the L2
 * table, +1 otherwise (meaning next L1 entry must be scanned)
 */
static int handle_l1_dte(struct vgic_its *its, u32 id, void *addr,
			 void *opaque)
{
	const struct vgic_its_abi *abi = vgic_its_get_abi(its);
	int l2_start_id = id * (SZ_64K / abi->dte_esz);
	u64 entry = *(u64 *)addr;
	int dte_esz = abi->dte_esz;
	gpa_t gpa;
	int ret;

	entry = le64_to_cpu(entry);

	if (!(entry & KVM_ITS_L1E_VALID_MASK))
		return 1;

	gpa = entry & KVM_ITS_L1E_ADDR_MASK;

	ret = scan_its_table(its, gpa, SZ_64K, dte_esz,
			     l2_start_id, vgic_its_restore_dte, NULL);

	return ret;
}

/**
 * vgic_its_restore_device_tables - Restore the device table and all ITT
 * from guest RAM to internal data structs
 */
static int vgic_its_restore_device_tables(struct vgic_its *its)
{
	const struct vgic_its_abi *abi = vgic_its_get_abi(its);
	u64 baser = its->baser_device_table;
	int l1_esz, ret;
	int l1_tbl_size = GITS_BASER_NR_PAGES(baser) * SZ_64K;
	gpa_t l1_gpa;

	if (!(baser & GITS_BASER_VALID))
		return 0;

	l1_gpa = BASER_ADDRESS(baser);

	if (baser & GITS_BASER_INDIRECT) {
		l1_esz = GITS_LVL1_ENTRY_SIZE;
		ret = scan_its_table(its, l1_gpa, l1_tbl_size, l1_esz, 0,
				     handle_l1_dte, NULL);
	} else {
		l1_esz = abi->dte_esz;
		ret = scan_its_table(its, l1_gpa, l1_tbl_size, l1_esz, 0,
				     vgic_its_restore_dte, NULL);
	}

	/* scan_its_table returns +1 if all entries are invalid */
	if (ret > 0)
		ret = 0;

	return ret;
}

static int vgic_its_save_cte(struct vgic_its *its,
			     struct its_collection *collection,
			     gpa_t gpa, int esz)
{
	u64 val;

	val = (1ULL << KVM_ITS_CTE_VALID_SHIFT |
	       ((u64)collection->target_addr << KVM_ITS_CTE_RDBASE_SHIFT) |
	       collection->collection_id);
	val = cpu_to_le64(val);
	return kvm_write_guest(its->dev->kvm, gpa, &val, esz);
}

static int vgic_its_restore_cte(struct vgic_its *its, gpa_t gpa, int esz)
{
	struct its_collection *collection;
	struct kvm *kvm = its->dev->kvm;
	u32 target_addr, coll_id;
	u64 val;
	int ret;

	BUG_ON(esz > sizeof(val));
	ret = kvm_read_guest(kvm, gpa, &val, esz);
	if (ret)
		return ret;
	val = le64_to_cpu(val);
	if (!(val & KVM_ITS_CTE_VALID_MASK))
		return 0;

	target_addr = (u32)(val >> KVM_ITS_CTE_RDBASE_SHIFT);
	coll_id = val & KVM_ITS_CTE_ICID_MASK;

	if (target_addr >= atomic_read(&kvm->online_vcpus))
		return -EINVAL;

	collection = find_collection(its, coll_id);
	if (collection)
		return -EEXIST;
	ret = vgic_its_alloc_collection(its, &collection, coll_id);
	if (ret)
		return ret;
	collection->target_addr = target_addr;
	return 1;
}

/**
 * vgic_its_save_collection_table - Save the collection table into
 * guest RAM
 */
static int vgic_its_save_collection_table(struct vgic_its *its)
{
	const struct vgic_its_abi *abi = vgic_its_get_abi(its);
	u64 baser = its->baser_coll_table;
	gpa_t gpa = BASER_ADDRESS(baser);
	struct its_collection *collection;
	u64 val;
	size_t max_size, filled = 0;
	int ret, cte_esz = abi->cte_esz;

	if (!(baser & GITS_BASER_VALID))
		return 0;

	max_size = GITS_BASER_NR_PAGES(baser) * SZ_64K;

	list_for_each_entry(collection, &its->collection_list, coll_list) {
		ret = vgic_its_save_cte(its, collection, gpa, cte_esz);
		if (ret)
			return ret;
		gpa += cte_esz;
		filled += cte_esz;
	}

	if (filled == max_size)
		return 0;

	/*
	 * table is not fully filled, add a last dummy element
	 * with valid bit unset
	 */
	val = 0;
	BUG_ON(cte_esz > sizeof(val));
	ret = kvm_write_guest(its->dev->kvm, gpa, &val, cte_esz);
	return ret;
}

/**
 * vgic_its_restore_collection_table - reads the collection table
 * in guest memory and restores the ITS internal state. Requires the
 * BASER registers to be restored before.
 */
static int vgic_its_restore_collection_table(struct vgic_its *its)
{
	const struct vgic_its_abi *abi = vgic_its_get_abi(its);
	u64 baser = its->baser_coll_table;
	int cte_esz = abi->cte_esz;
	size_t max_size, read = 0;
	gpa_t gpa;
	int ret;

	if (!(baser & GITS_BASER_VALID))
		return 0;

	gpa = BASER_ADDRESS(baser);

	max_size = GITS_BASER_NR_PAGES(baser) * SZ_64K;

	while (read < max_size) {
		ret = vgic_its_restore_cte(its, gpa, cte_esz);
		if (ret <= 0)
			break;
		gpa += cte_esz;
		read += cte_esz;
	}

	if (ret > 0)
		return 0;

	return ret;
}

/**
 * vgic_its_save_tables_v0 - Save the ITS tables into guest ARM
 * according to v0 ABI
 */
static int vgic_its_save_tables_v0(struct vgic_its *its)
{
	struct kvm *kvm = its->dev->kvm;
	int ret;

	mutex_lock(&kvm->lock);
	mutex_lock(&its->its_lock);

	if (!lock_all_vcpus(kvm)) {
		mutex_unlock(&its->its_lock);
		mutex_unlock(&kvm->lock);
		return -EBUSY;
	}

	ret = vgic_its_save_device_tables(its);
	if (ret)
		goto out;

	ret = vgic_its_save_collection_table(its);

out:
	unlock_all_vcpus(kvm);
	mutex_unlock(&its->its_lock);
	mutex_unlock(&kvm->lock);
	return ret;
}

/**
 * vgic_its_restore_tables_v0 - Restore the ITS tables from guest RAM
 * to internal data structs according to V0 ABI
 *
 */
static int vgic_its_restore_tables_v0(struct vgic_its *its)
{
	struct kvm *kvm = its->dev->kvm;
	int ret;

	mutex_lock(&kvm->lock);
	mutex_lock(&its->its_lock);

	if (!lock_all_vcpus(kvm)) {
		mutex_unlock(&its->its_lock);
		mutex_unlock(&kvm->lock);
		return -EBUSY;
	}

	ret = vgic_its_restore_collection_table(its);
	if (ret)
		goto out;

	ret = vgic_its_restore_device_tables(its);
out:
	unlock_all_vcpus(kvm);
	mutex_unlock(&its->its_lock);
	mutex_unlock(&kvm->lock);

	return ret;
}

static int vgic_its_commit_v0(struct vgic_its *its)
{
	const struct vgic_its_abi *abi;

	abi = vgic_its_get_abi(its);
	its->baser_coll_table &= ~GITS_BASER_ENTRY_SIZE_MASK;
	its->baser_device_table &= ~GITS_BASER_ENTRY_SIZE_MASK;

	its->baser_coll_table |= (GIC_ENCODE_SZ(abi->cte_esz, 5)
					<< GITS_BASER_ENTRY_SIZE_SHIFT);

	its->baser_device_table |= (GIC_ENCODE_SZ(abi->dte_esz, 5)
					<< GITS_BASER_ENTRY_SIZE_SHIFT);
	return 0;
}

static int vgic_its_has_attr(struct kvm_device *dev,
			     struct kvm_device_attr *attr)
{
	switch (attr->group) {
	case KVM_DEV_ARM_VGIC_GRP_ADDR:
		switch (attr->attr) {
		case KVM_VGIC_ITS_ADDR_TYPE:
			return 0;
		}
		break;
	case KVM_DEV_ARM_VGIC_GRP_CTRL:
		switch (attr->attr) {
		case KVM_DEV_ARM_VGIC_CTRL_INIT:
			return 0;
		case KVM_DEV_ARM_ITS_SAVE_TABLES:
			return 0;
		case KVM_DEV_ARM_ITS_RESTORE_TABLES:
			return 0;
		}
		break;
	case KVM_DEV_ARM_VGIC_GRP_ITS_REGS:
		return vgic_its_has_attr_regs(dev, attr);
	}
	return -ENXIO;
}

static int vgic_its_set_attr(struct kvm_device *dev,
			     struct kvm_device_attr *attr)
{
	struct vgic_its *its = dev->private;
	int ret;

	switch (attr->group) {
	case KVM_DEV_ARM_VGIC_GRP_ADDR: {
		u64 __user *uaddr = (u64 __user *)(long)attr->addr;
		unsigned long type = (unsigned long)attr->attr;
		u64 addr;

		if (type != KVM_VGIC_ITS_ADDR_TYPE)
			return -ENODEV;

		if (copy_from_user(&addr, uaddr, sizeof(addr)))
			return -EFAULT;

		ret = vgic_check_ioaddr(dev->kvm, &its->vgic_its_base,
					addr, SZ_64K);
		if (ret)
			return ret;

		return vgic_register_its_iodev(dev->kvm, its, addr);
	}
	case KVM_DEV_ARM_VGIC_GRP_CTRL: {
		const struct vgic_its_abi *abi = vgic_its_get_abi(its);

		switch (attr->attr) {
		case KVM_DEV_ARM_VGIC_CTRL_INIT:
			/* Nothing to do */
			return 0;
		case KVM_DEV_ARM_ITS_SAVE_TABLES:
			return abi->save_tables(its);
		case KVM_DEV_ARM_ITS_RESTORE_TABLES:
			return abi->restore_tables(its);
		}
	}
	case KVM_DEV_ARM_VGIC_GRP_ITS_REGS: {
		u64 __user *uaddr = (u64 __user *)(long)attr->addr;
		u64 reg;

		if (get_user(reg, uaddr))
			return -EFAULT;

		return vgic_its_attr_regs_access(dev, attr, &reg, true);
	}
	}
	return -ENXIO;
}

static int vgic_its_get_attr(struct kvm_device *dev,
			     struct kvm_device_attr *attr)
{
	switch (attr->group) {
	case KVM_DEV_ARM_VGIC_GRP_ADDR: {
		struct vgic_its *its = dev->private;
		u64 addr = its->vgic_its_base;
		u64 __user *uaddr = (u64 __user *)(long)attr->addr;
		unsigned long type = (unsigned long)attr->attr;

		if (type != KVM_VGIC_ITS_ADDR_TYPE)
			return -ENODEV;

		if (copy_to_user(uaddr, &addr, sizeof(addr)))
			return -EFAULT;
		break;
	}
	case KVM_DEV_ARM_VGIC_GRP_ITS_REGS: {
		u64 __user *uaddr = (u64 __user *)(long)attr->addr;
		u64 reg;
		int ret;

		ret = vgic_its_attr_regs_access(dev, attr, &reg, false);
		if (ret)
			return ret;
		return put_user(reg, uaddr);
	}
	default:
		return -ENXIO;
	}

	return 0;
}

static struct kvm_device_ops kvm_arm_vgic_its_ops = {
	.name = "kvm-arm-vgic-its",
	.create = vgic_its_create,
	.destroy = vgic_its_destroy,
	.set_attr = vgic_its_set_attr,
	.get_attr = vgic_its_get_attr,
	.has_attr = vgic_its_has_attr,
};

int kvm_vgic_register_its_device(void)
{
	return kvm_register_device_ops(&kvm_arm_vgic_its_ops,
				       KVM_DEV_TYPE_ARM_VGIC_ITS);
}
