/*
 * Zynq R5 Remote Processor driver
 *
 * Copyright (C) 2015 Jason Wu <j.wu@xilinx.com>
 * Copyright (C) 2015 Xilinx, Inc.
 *
 * Based on origin OMAP and Zynq Remote Processor driver
 *
 * Copyright (C) 2012 Michal Simek <monstr@monstr.eu>
 * Copyright (C) 2012 PetaLogix
 * Copyright (C) 2011 Texas Instruments, Inc.
 * Copyright (C) 2011 Google, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/remoteproc.h>
#include <linux/interrupt.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <asm/cacheflush.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/delay.h>

#include "remoteproc_internal.h"

/* Register offset definitions for RPU. */
#define RPU_GLBL_CNTL_OFFSET	0x00000000 /* RPU control */
#define RPU_0_CFG_OFFSET	0x00000100 /* RPU0 configuration */
#define RPU_1_CFG_OFFSET	0x00000200 /* RPU1 Configuration */
/* Boot memory bit. high for OCM, low for TCM */
#define VINITHI_BIT		BIT(2)
/* CPU halt bit, high: processor is running. low: processor is halt */
#define nCPUHALT_BIT		BIT(0)
/* RPU mode, high: split mode. low: lock step mode */
#define SLSPLIT_BIT		BIT(3)
/* Clamp mode. high: split mode. low: lock step mode */
#define SLCLAMP_BIT		BIT(4)
/* TCM mode. high: combine RPU TCMs. low: split TCM for RPU1 and RPU0 */
#define TCM_COMB_BIT		BIT(6)

/* Clock controller low power domain (CRL_APB) for RPU */
#define CPU_R5_CTRL_OFFSET	0x00000090 /* RPU Global Control*/
#define RST_LPD_TOP_OFFSET	0x0000023C /* LPD block */
#define RPU0_RESET_BIT		BIT(0) /* RPU CPU0 reset bit */

/* IPI reg offsets */
#define TRIG_OFFSET		0x00000000
#define OBS_OFFSET		0x00000004
#define ISR_OFFSET		0x00000010
#define IMR_OFFSET		0x00000014
#define IER_OFFSET		0x00000018
#define IDR_OFFSET		0x0000001C
#define IPI_ALL_MASK		0x0F0F0301

#define MAX_INSTANCES		2 /* Support upto 2 RPU */

/* Store rproc for IPI handler */
static struct platform_device *remoteprocdev[MAX_INSTANCES];

/* Register access macros */
#define reg_read(base, reg) \
	readl(((void __iomem *)(base)) + (reg))
#define reg_write(base, reg, val) \
	writel((val), ((void __iomem *)(base)) + (reg))

#define DEFAULT_FIRMWARE_NAME	"rproc-rpu-fw"

/* Module parameter */
static char *firmware;

struct zynqmp_r5_rproc_pdata;

/**
 * struct ipi_ops - IPI operation handlers
 * @clear:          Clear IPI
 * @reset:          Reset IPI channel
 * @set_mask:       Destination mask
 * @trigger:        Trigger IPI
 */
struct ipi_ops {
	void (*clear)(struct zynqmp_r5_rproc_pdata *pdata);
	void (*reset)(struct zynqmp_r5_rproc_pdata *pdata);
	void (*set_mask)(struct zynqmp_r5_rproc_pdata *pdata);
	void (*trigger)(struct zynqmp_r5_rproc_pdata *pdata);
};

/**
 * struct rpu_ops - RPU operation handlers
 * @bootdev:        Boot device
 * @core_conf:      Core configuration
 * @halt:           Enable/Disable halt
 * @en_reset:       Enable/Disable reset
 */
struct rpu_ops {
	void (*bootdev)(struct zynqmp_r5_rproc_pdata *pdata);
	void (*core_conf)(struct zynqmp_r5_rproc_pdata *pdata);
	void (*halt)(struct zynqmp_r5_rproc_pdata *pdata, bool do_halt);
	void (*en_reset)(struct zynqmp_r5_rproc_pdata *pdata, bool do_reset);
};

/* enumerations for RPU/IPI control methods */
enum control_method {
	SMC = 0,
	HVC,
	HW,
};

/* enumerations for R5 boot device */
enum rpu_bootmem {
	TCM = 0,
	OCM,
};

/* enumerations for R5 core configurations */
enum rpu_core_conf {
	LOCK_STEP = 0,
	SPLIT,
};

/**
 * struct zynqmp_r5_rproc_pdata - zynqmp rpu remote processor instance state
 * @rproc: rproc handle
 * @ipi_ops: IPI related operation handlers
 * @rpu_ops: RPU related operation handlers
 * @workqueue: workqueue for the RPU remoteproc
 * @rpu_base: virt ptr to RPU control address registers
 * @crl_apb_base: virt ptr to CRL_APB address registers for RPU
 * @ipi_base: virt ptr to IPI channel address registers for APU
 * @rpu_mode: RPU core configuration
 * @rpu_id: RPU CPU id
 * @bootmem: RPU boot memory device used
 * @vring0: IRQ number used for vring0
 * @ipi_dest_mask: IPI destination mask for the IPI channel
 */
struct zynqmp_r5_rproc_pdata {
	struct rproc *rproc;
	struct ipi_ops *ipi_ops;
	struct rpu_ops *rpu_ops;
	struct work_struct workqueue;
	void __iomem *rpu_base;
	void __iomem *crl_apb_base;
	void __iomem *ipi_base;
	enum rpu_core_conf rpu_mode;
	enum rpu_bootmem bootmem;
	u32 ipi_dest_mask;
	u32 rpu_id;
	u32 vring0;
};

/*
 * TODO: Update HW RPU operation when the driver is ready
 */
static void hw_r5_boot_dev(struct zynqmp_r5_rproc_pdata *pdata)
{
	u32 tmp;
	u32 offset = RPU_1_CFG_OFFSET;

	pr_debug("%s: R5 ID: %d, boot_dev %d\n",
			 __func__, pdata->rpu_id, pdata->bootmem);
	if (pdata->rpu_id == 0)
		offset = RPU_0_CFG_OFFSET;

	tmp = reg_read(pdata->rpu_base, offset);
	if (pdata->bootmem == OCM)
		tmp |= VINITHI_BIT;
	else
		tmp &= ~VINITHI_BIT;
	reg_write(pdata->rpu_base, offset, tmp);
}

static void hw_r5_reset(struct zynqmp_r5_rproc_pdata *pdata,
						bool do_reset)
{
	u32 tmp;

	pr_debug("%s: R5 ID: %d, reset %d\n", __func__, pdata->rpu_id,
			 do_reset);
	tmp = reg_read(pdata->crl_apb_base, RST_LPD_TOP_OFFSET);
	if (do_reset)
		tmp |= (RPU0_RESET_BIT << pdata->rpu_id);
	else
		tmp &= ~(RPU0_RESET_BIT << pdata->rpu_id);
	reg_write(pdata->crl_apb_base, RST_LPD_TOP_OFFSET, tmp);
}

static void hw_r5_halt(struct zynqmp_r5_rproc_pdata *pdata,
						bool do_halt)
{
	u32 tmp;
	u32 offset = RPU_1_CFG_OFFSET;

	pr_debug("%s: R5 ID: %d, halt %d\n", __func__, pdata->rpu_id,
			 do_halt);
	if (pdata->rpu_id == 0)
		offset = RPU_0_CFG_OFFSET;

	tmp = reg_read(pdata->rpu_base, offset);
	if (do_halt)
		tmp &= ~nCPUHALT_BIT;
	else
		tmp |= nCPUHALT_BIT;
	reg_write(pdata->rpu_base, offset, tmp);
}

static void hw_r5_core_config(struct zynqmp_r5_rproc_pdata *pdata)
{
	u32 tmp;

	pr_debug("%s: mode: %d\n", __func__, pdata->rpu_mode);
	tmp = reg_read(pdata->rpu_base, 0);
	if (pdata->rpu_mode == SPLIT) {
		tmp |= SLSPLIT_BIT;
		tmp &= ~TCM_COMB_BIT;
		tmp &= ~SLCLAMP_BIT;
	} else {
		tmp &= ~SLSPLIT_BIT;
		tmp |= TCM_COMB_BIT;
		tmp |= SLCLAMP_BIT;
	}
	reg_write(pdata->rpu_base, 0, tmp);
}

static struct rpu_ops rpu_hw_ops = {
	.bootdev       = hw_r5_boot_dev,
	.core_conf     = hw_r5_core_config,
	.halt          = hw_r5_halt,
	.en_reset         = hw_r5_reset,
};

static void smc_r5_boot_dev(struct zynqmp_r5_rproc_pdata *pdata)
{
	pr_err("%s: atf smc to be implemented\n", __func__);
}

static void smc_r5_reset(struct zynqmp_r5_rproc_pdata *pdata,
						bool do_reset)
{
	pr_err("%s: atf smc to be implemented\n", __func__);
}

static void smc_r5_halt(struct zynqmp_r5_rproc_pdata *pdata,
						bool do_halt)
{
	pr_err("%s: atf smc to be implemented\n", __func__);
}

static void smc_r5_core_config(struct zynqmp_r5_rproc_pdata *pdata)
{
	pr_err("%s: atf smc to be implemented\n", __func__);
}

static struct rpu_ops rpu_smc_ops = {
	.bootdev       = smc_r5_boot_dev,
	.core_conf     = smc_r5_core_config,
	.halt          = smc_r5_halt,
	.en_reset      = smc_r5_reset,
};

static void hvc_r5_boot_dev(struct zynqmp_r5_rproc_pdata *pdata)
{
	pr_err("%s: hypervisor hvc to be implemented\n", __func__);
}

static void hvc_r5_reset(struct zynqmp_r5_rproc_pdata *pdata, bool do_reset)
{
	pr_err("%s: hypervisor hvc to be implemented\n", __func__);
}

static void hvc_r5_halt(struct zynqmp_r5_rproc_pdata *pdata, bool do_halt)
{
	pr_err("%s: hypervisor hvc to be implemented\n", __func__);
}

static void hvc_r5_core_config(struct zynqmp_r5_rproc_pdata *pdata)
{
	pr_err("%s: hypervisor hvc to be implemented\n", __func__);
}

static struct rpu_ops rpu_hvc_ops = {
	.bootdev       = hvc_r5_boot_dev,
	.core_conf     = hvc_r5_core_config,
	.halt          = hvc_r5_halt,
	.en_reset      = hvc_r5_reset,
};

/*
 * TODO: Update HW ipi operation when the driver is ready
 */
static void hw_clear_ipi(struct zynqmp_r5_rproc_pdata *pdata)
{
	pr_debug("%s: irq issuer %08x clear IPI\n", __func__,
			 pdata->ipi_dest_mask);
	reg_write(pdata->ipi_base, ISR_OFFSET, pdata->ipi_dest_mask);
}

static void hw_ipi_reset(struct zynqmp_r5_rproc_pdata *pdata)
{
	reg_write(pdata->ipi_base, IDR_OFFSET, IPI_ALL_MASK);
	reg_write(pdata->ipi_base, ISR_OFFSET, IPI_ALL_MASK);
	/* add delay to allow ipi to be settle */
	udelay(10);
	pr_debug("IPI reset done\n");
}

static void hw_set_ipi_mask(struct zynqmp_r5_rproc_pdata *pdata)
{
	pr_debug("%s: set IPI mask %08x\n", __func__, pdata->ipi_dest_mask);
	reg_write(pdata->ipi_base, IER_OFFSET, pdata->ipi_dest_mask);
}

static void hw_trigger_ipi(struct zynqmp_r5_rproc_pdata *pdata)
{
	pr_debug("%s: dest %08x\n", __func__, pdata->ipi_dest_mask);
	reg_write(pdata->ipi_base, TRIG_OFFSET, pdata->ipi_dest_mask);
}

static void ipi_init(struct zynqmp_r5_rproc_pdata *pdata)
{
	pr_debug("%s\n", __func__);
	pdata->ipi_ops->reset(pdata);
	pdata->ipi_ops->set_mask(pdata);
}

static struct ipi_ops ipi_hw_ops = {
	.clear          = hw_clear_ipi,
	.reset          = hw_ipi_reset,
	.set_mask       = hw_set_ipi_mask,
	.trigger        = hw_trigger_ipi,
};

static void smc_clear_ipi(struct zynqmp_r5_rproc_pdata *pdata)
{
	pr_err("%s: atf smc to be implemented\n", __func__);
}

static void smc_ipi_reset(struct zynqmp_r5_rproc_pdata *pdata)
{
	pr_err("%s: atf smc to be implemented\n", __func__);
}

static void smc_set_ipi_mask(struct zynqmp_r5_rproc_pdata *pdata)
{
	pr_err("%s: atf smc to be implemented\n", __func__);
}

static void smc_trigger_ipi(struct zynqmp_r5_rproc_pdata *pdata)
{
	pr_err("%s: atf smc to be implemented\n", __func__);
}

static struct ipi_ops ipi_smc_ops = {
	.clear          = smc_clear_ipi,
	.reset          = smc_ipi_reset,
	.set_mask       = smc_set_ipi_mask,
	.trigger        = smc_trigger_ipi,
};

static void hvc_clear_ipi(struct zynqmp_r5_rproc_pdata *pdata)
{
	pr_err("%s: hypervisor hvc to be implemented\n", __func__);
}

static void hvc_ipi_reset(struct zynqmp_r5_rproc_pdata *pdata)
{
	pr_err("%s: hypervisor hvc to be implemented\n", __func__);
}

static void hvc_set_ipi_mask(struct zynqmp_r5_rproc_pdata *pdata)
{
	pr_err("%s: hypervisor hvc to be implemented\n", __func__);
}

static void hvc_trigger_ipi(struct zynqmp_r5_rproc_pdata *pdata)
{
	pr_err("%s: hypervisor hvc to be implemented\n", __func__);
}

static struct ipi_ops ipi_hvc_ops = {
	.clear          = hvc_clear_ipi,
	.reset          = hvc_ipi_reset,
	.set_mask       = hvc_set_ipi_mask,
	.trigger        = hvc_trigger_ipi,
};

static void handle_event(struct zynqmp_r5_rproc_pdata *local)
{
	flush_cache_all();

	if (rproc_vq_interrupt(local->rproc, 0) == IRQ_NONE)
		dev_dbg(&remoteprocdev[local->rpu_id]->dev, \
			"no message found in vqid 0\n");
}

static void handle_event0(struct work_struct *work)
{
	struct zynqmp_r5_rproc_pdata *local = platform_get_drvdata(remoteprocdev[0]);

	handle_event(local);
}

static void handle_event1(struct work_struct *work)
{
	struct zynqmp_r5_rproc_pdata *local = platform_get_drvdata(remoteprocdev[1]);

	handle_event(local);
}

static int zynqmp_r5_rproc_start(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;
	struct platform_device *pdev = to_platform_device(dev);
	struct zynqmp_r5_rproc_pdata *local = platform_get_drvdata(pdev);

	dev_dbg(dev, "%s\n", __func__);
	/* limit to two RPU support */
	if (local->rpu_id == 0)
		INIT_WORK(&local->workqueue, handle_event0);
	else
		INIT_WORK(&local->workqueue, handle_event1);

	flush_cache_all();
	remoteprocdev[local->rpu_id] = pdev;

	/* Set up R5 */
	local->rpu_ops->core_conf(local);
	local->rpu_ops->en_reset(local, true);
	local->rpu_ops->halt(local, true);
	local->rpu_ops->bootdev(local);
	/* Add delay before release from halt and reset */
	udelay(500);
	local->rpu_ops->en_reset(local, false);
	local->rpu_ops->halt(local, false);

	ipi_init(local);
	return 0;
}

/* kick a firmware */
static void zynqmp_r5_rproc_kick(struct rproc *rproc, int vqid)
{
	struct device *dev = rproc->dev.parent;
	struct platform_device *pdev = to_platform_device(dev);
	struct zynqmp_r5_rproc_pdata *local = platform_get_drvdata(pdev);

	dev_dbg(dev, "KICK Firmware to start send messages vqid %d\n", vqid);

	/*
	 * send irq to R5 firmware
	 * Currently vqid is not used because we only got one.
	 */
	local->ipi_ops->trigger(local);
}

/* power off the remote processor */
static int zynqmp_r5_rproc_stop(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;
	struct platform_device *pdev = to_platform_device(dev);
	struct zynqmp_r5_rproc_pdata *local = platform_get_drvdata(pdev);

	dev_dbg(dev, "%s\n", __func__);

	local->rpu_ops->en_reset(local, true);
	local->rpu_ops->halt(local, true);

	local->ipi_ops->reset(local);

	return 0;
}

static void *zynqmp_r5_kva_to_guest_addr_kva(struct rproc *rproc,
				void *va, struct virtqueue *vq)
{
	struct rproc_vring *rvring;

	rvring = (struct rproc_vring *)(vq->priv);

	/*
	 * Remoteproc uses dma_alloc_coherent to set up the address of vring.
	 * It assumes the remote has the same memory address mapping for
	 * vring.
	 */
	return (void *)(phys_to_virt(rvring->dma) + (va - rvring->va));
}

static struct rproc_ops zynqmp_r5_rproc_ops = {
	.start		= zynqmp_r5_rproc_start,
	.stop		= zynqmp_r5_rproc_stop,
	.kick		= zynqmp_r5_rproc_kick,
	.kva_to_guest_addr_kva = zynqmp_r5_kva_to_guest_addr_kva,
};

static irqreturn_t r5_remoteproc_interrupt(int irq, void *dev_id)
{
	struct device *dev = dev_id;
	struct platform_device *pdev = to_platform_device(dev);
	struct zynqmp_r5_rproc_pdata *local = platform_get_drvdata(pdev);

	dev_dbg(dev, "KICK Linux because of pending message(irq%d)\n", irq);

	schedule_work(&local->workqueue);

	local->ipi_ops->clear(local);

	dev_dbg(dev, "KICK Linux handled\n");
	return IRQ_HANDLED;
}

static int zynqmp_r5_remoteproc_probe(struct platform_device *pdev)
{
	const unsigned char *prop;
	struct resource *res;
	int ret = 0;
	int method = 0;
	struct zynqmp_r5_rproc_pdata *local;

	local = devm_kzalloc(&pdev->dev, sizeof(struct zynqmp_r5_rproc_pdata),
				 GFP_KERNEL);
	if (!local)
		return -ENOMEM;

	platform_set_drvdata(pdev, local);

	/* Declare vring for firmware */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "vring0");
	if (!res) {
		dev_err(&pdev->dev, "invalid address for vring0\n");
		return -ENXIO;
	}

	ret = dma_declare_coherent_memory(&pdev->dev, res->start,
		res->start, resource_size(res),
		DMA_MEMORY_IO | DMA_MEMORY_EXCLUSIVE);
	if (!(ret & DMA_MEMORY_IO)) {
		dev_err(&pdev->dev, "dma_declare_coherent_memory failed %x - %x\n",
			(u32)res->start, (u32)res->end);
		ret = -ENOMEM;
		goto err_exit;
	}

	/* FIXME: it may need to extend to 64/48 bit */
	ret = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "dma_set_coherent_mask: %d\n", ret);
		goto dma_mask_fault;
	}

	prop = of_get_property(pdev->dev.of_node, "core_conf", NULL);
	if (!prop) {
		dev_warn(&pdev->dev, "default core_conf used: lock-step\n");
		prop = "lock-step";
	}

	dev_info(&pdev->dev, "RPU core_conf: %s\n", prop);
	if (!strcmp(prop, "split0")) {
		local->rpu_mode = SPLIT;
		local->rpu_id = 0;
	} else if (!strcmp(prop, "split1")) {
		local->rpu_mode = SPLIT;
		local->rpu_id = 1;
	} else if (!strcmp(prop, "lock-step")) {
		local->rpu_mode = LOCK_STEP;
		local->rpu_id = 0;
	} else {
		dev_err(&pdev->dev, "Invalid core_conf mode provided - %s , %d\n",
			prop, local->rpu_mode);
		goto dma_mask_fault;
	}

	prop = of_get_property(pdev->dev.of_node, "method", NULL);
	if (!prop) {
		dev_warn(&pdev->dev, "default method used: smc\n");
		prop = "smc";
	}

	dev_info(&pdev->dev, "IPI/RPU control method: %s\n", prop);
	if (!strcmp(prop, "direct")) {
		method = HW;
		local->ipi_ops = &ipi_hw_ops;
		local->rpu_ops = &rpu_hw_ops;
	} else if (!strcmp(prop, "hvc")) {
		method = HVC;
		local->ipi_ops = &ipi_hvc_ops;
		local->rpu_ops = &rpu_hvc_ops;
	} else if (!strcmp(prop, "smc")) {
		method = SMC;
		local->ipi_ops = &ipi_smc_ops;
		local->rpu_ops = &rpu_smc_ops;
	} else {
		dev_err(&pdev->dev, "Invalid method provided - %s\n",
			prop);
		goto dma_mask_fault;
	}

	/* Handle direct hardware access */
	/* (TODO: remove once RPU and IPI drivers are ready ) */
	if (method == HW) {
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
			"rpu_base");
		local->rpu_base = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(local->rpu_base)) {
			dev_err(&pdev->dev, "Unable to map RPU I/O memory\n");
			ret = PTR_ERR(local->rpu_base);
			goto dma_mask_fault;
		}

		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
			"apb_base");
		local->crl_apb_base = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(local->crl_apb_base)) {
			dev_err(&pdev->dev, "Unable to map CRL_APB I/O memory\n");
			ret = PTR_ERR(local->crl_apb_base);
			goto dma_mask_fault;
		}

		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ipi");
		local->ipi_base = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(local->ipi_base)) {
			pr_err("%s: Unable to map IPI\n", __func__);
			ret = PTR_ERR(local->ipi_base);
			goto dma_mask_fault;
		}
	}

	prop = of_get_property(pdev->dev.of_node, "bootmem", NULL);
	if (!prop) {
		dev_warn(&pdev->dev, "default bootmem property used: tcm\n");
		prop = "tcm";
	}

	dev_info(&pdev->dev, "RPU bootmem: %s\n", prop);
	if (!strcmp(prop, "tcm")) {
		local->bootmem = TCM;
	} else if (!strcmp(prop, "ocm")) {
		local->bootmem = OCM;
	} else {
		dev_err(&pdev->dev, "Invalid R5 bootmem property - %s\n",
			prop);
		goto dma_mask_fault;
	}

	/* IPI IRQ */
	local->vring0 = platform_get_irq(pdev, 0);
	if (local->vring0 < 0) {
		ret = local->vring0;
		dev_err(&pdev->dev, "unable to find IPI IRQ\n");
		goto dma_mask_fault;
	}
	ret = devm_request_irq(&pdev->dev, local->vring0,
		r5_remoteproc_interrupt, 0, dev_name(&pdev->dev),
		&pdev->dev);
	if (ret) {
		dev_err(&pdev->dev, "IRQ %d already allocated\n",
			local->vring0);
		goto dma_mask_fault;
	}
	dev_dbg(&pdev->dev, "vring0 irq: %d\n", local->vring0);

	ret = of_property_read_u32(pdev->dev.of_node, "ipi_dest_mask",
		&local->ipi_dest_mask);
	if (ret < 0) {
		dev_warn(&pdev->dev, "default ipi_dest_mask used: 0x100\n");
		local->ipi_dest_mask = 0x100;
	}
	dev_info(&pdev->dev, "ipi_dest_mask: 0x%x\n", local->ipi_dest_mask);

	/* Module param firmware first */
	prop = of_get_property(pdev->dev.of_node, "firmware", NULL);
	if (firmware)
		prop = firmware;
	else if (!prop)
		prop = DEFAULT_FIRMWARE_NAME;

	if (prop) {
		dev_dbg(&pdev->dev, "Using firmware: %s\n", prop);
		local->rproc = rproc_alloc(&pdev->dev, dev_name(&pdev->dev),
			&zynqmp_r5_rproc_ops, prop, sizeof(struct rproc));
		if (!local->rproc) {
			dev_err(&pdev->dev, "rproc allocation failed\n");
			goto rproc_fault;
		}

		ret = rproc_add(local->rproc);
		if (ret) {
			dev_err(&pdev->dev, "rproc registration failed\n");
			goto rproc_fault;
		}
	} else {
		ret = -ENODEV;
	}

	return ret;

rproc_fault:
	rproc_put(local->rproc);

dma_mask_fault:
	dma_release_declared_memory(&pdev->dev);

err_exit:
	return 0;
}

static int zynqmp_r5_remoteproc_remove(struct platform_device *pdev)
{
	struct zynqmp_r5_rproc_pdata *local = platform_get_drvdata(pdev);

	dev_info(&pdev->dev, "%s\n", __func__);

	rproc_del(local->rproc);
	rproc_put(local->rproc);

	dma_release_declared_memory(&pdev->dev);

	return 0;
}

/* Match table for OF platform binding */
static const struct of_device_id zynqmp_r5_remoteproc_match[] = {
	{ .compatible = "xlnx,zynqmp-r5-remoteproc-1.0", },
	{ /* end of list */ },
};
MODULE_DEVICE_TABLE(of, zynqmp_r5_remoteproc_match);

static struct platform_driver zynqmp_r5_remoteproc_driver = {
	.probe = zynqmp_r5_remoteproc_probe,
	.remove = zynqmp_r5_remoteproc_remove,
	.driver = {
		.name = "zynqmp_r5_remoteproc",
		.of_match_table = zynqmp_r5_remoteproc_match,
	},
};
module_platform_driver(zynqmp_r5_remoteproc_driver);

module_param(firmware, charp, 0);
MODULE_PARM_DESC(firmware, "Override the firmware image name.");

MODULE_AUTHOR("Jason Wu <j.wu@xilinx.com>");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("ZynqMP R5 remote processor control driver");
