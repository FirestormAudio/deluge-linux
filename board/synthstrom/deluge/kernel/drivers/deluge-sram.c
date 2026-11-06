// SPDX-License-Identifier: GPL-2.0
/*
 * Synthstrom Deluge userspace on-chip SRAM region.
 *
 * Exposes a fixed on-chip SRAM carveout (DT reg) as /dev/deluge-sram, mmap-able
 * into userspace (the Spark synth) for hot DSP data — wavetables, delay lines —
 * that benefits from living on-chip: never evicted by SDRAM traffic and with a
 * deterministic refill on a cache miss.
 *
 * The mapping is CACHED (normal write-back memory), which is what a pure-CPU
 * DSP working set wants: L1/L2 speed, with the SRAM backing bounding worst-case
 * refill latency. This is safe here because the carveout's physical address is
 * outside the kernel's RAM banks (the 64 MB SDRAM), so there is NO conflicting
 * cacheable linear-map alias of these pages. Two rules the DT must uphold:
 *   - the region must not overlap the non-cached mmio-sram DMA pool (mismatched
 *     cache attributes on the same PA are architecturally UNPREDICTABLE on ARM);
 *   - userspace must not DMA into it (no device shares this cached view).
 */

#include <linux/deluge-sram.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>

struct deluge_sram {
	struct miscdevice misc;
	phys_addr_t phys;
	size_t size;
};

static inline struct deluge_sram *to_deluge_sram(struct file *file)
{
	return container_of(file->private_data, struct deluge_sram, misc);
}

/*
 * Map the SRAM carveout to userspace CACHED: leave vma->vm_page_prot at its
 * default (normal write-back memory) — do NOT apply pgprot_noncached. Only
 * whole/partial mappings within the region are allowed.
 */
static int deluge_sram_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct deluge_sram *s = to_deluge_sram(file);
	size_t size = vma->vm_end - vma->vm_start;
	unsigned long off = (unsigned long)vma->vm_pgoff << PAGE_SHIFT;

	if (off >= s->size || size > s->size - off)
		return -EINVAL;

	return remap_pfn_range(vma, vma->vm_start,
			       (s->phys >> PAGE_SHIFT) + vma->vm_pgoff,
			       size, vma->vm_page_prot);
}

static long deluge_sram_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	struct deluge_sram *s = to_deluge_sram(file);
	struct deluge_sram_info info = {
		.phys = (u32)s->phys,
		.size = (u32)s->size,
	};

	switch (cmd) {
	case DELUGE_SRAM_GET_INFO:
		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}
	return -ENOTTY;
}

static const struct file_operations deluge_sram_fops = {
	.owner		= THIS_MODULE,
	.mmap		= deluge_sram_mmap,
	.unlocked_ioctl	= deluge_sram_ioctl,
};

static int deluge_sram_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct deluge_sram *s;
	struct resource *res;
	int ret;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	s->phys = res->start;
	s->size = resource_size(res);

	s->misc.minor = MISC_DYNAMIC_MINOR;
	s->misc.name = "deluge-sram";
	s->misc.fops = &deluge_sram_fops;
	ret = misc_register(&s->misc);
	if (ret)
		return dev_err_probe(dev, ret, "misc_register failed\n");

	platform_set_drvdata(pdev, s);
	dev_info(dev, "userspace SRAM %pa size %#zx -> /dev/deluge-sram (cached)\n",
		 &s->phys, s->size);
	return 0;
}

static void deluge_sram_remove(struct platform_device *pdev)
{
	struct deluge_sram *s = platform_get_drvdata(pdev);

	misc_deregister(&s->misc);
}

static const struct of_device_id deluge_sram_of[] = {
	{ .compatible = "synthstrom,deluge-sram" },
	{ }
};
MODULE_DEVICE_TABLE(of, deluge_sram_of);

static struct platform_driver deluge_sram_driver = {
	.probe	= deluge_sram_probe,
	.remove	= deluge_sram_remove,
	.driver	= {
		.name		= "deluge-sram",
		.of_match_table	= deluge_sram_of,
	},
};
module_platform_driver(deluge_sram_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Synthstrom Deluge userspace on-chip SRAM region (/dev/deluge-sram)");
