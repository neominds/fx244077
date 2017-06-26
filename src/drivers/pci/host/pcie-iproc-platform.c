/*
 * Copyright (C) 2015 Broadcom Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/phy/phy.h>

#include "pcie-iproc.h"

static const struct of_device_id iproc_pcie_of_match_table[] = {
	{
		.compatible = "brcm,iproc-pcie",
		.data = (int *)IPROC_PCIE_PAXB,
	}, {
		.compatible = "brcm,iproc-pcie-paxb-v2",
		.data = (int *)IPROC_PCIE_PAXB_V2,
	}, {
		.compatible = "brcm,iproc-pcie-paxc",
		.data = (int *)IPROC_PCIE_PAXC,
	}, {
		.compatible = "brcm,iproc-pcie-paxc-v2",
		.data = (int *)IPROC_PCIE_PAXC_V2,
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, iproc_pcie_of_match_table);

static int iproc_pcie_pltfm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct of_device_id *of_id;
	struct iproc_pcie *pcie;
	struct device_node *np = dev->of_node;
	struct resource reg;
	resource_size_t iobase = 0;
	LIST_HEAD(res);
	int ret;

	of_id = of_match_device(iproc_pcie_of_match_table, dev);
	if (!of_id)
		return -EINVAL;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	pcie->dev = dev;
	pcie->type = (enum iproc_pcie_type)of_id->data;

	ret = of_address_to_resource(np, 0, &reg);
	if (ret < 0) {
		dev_err(dev, "unable to obtain controller resources\n");
		return ret;
	}

	pcie->base = devm_ioremap(dev, reg.start, resource_size(&reg));
	if (!pcie->base) {
		dev_err(dev, "unable to map controller registers\n");
		return -ENOMEM;
	}
	pcie->base_addr = reg.start;

	if (of_property_read_bool(np, "brcm,pcie-ob")) {
		u32 val;

		ret = of_property_read_u32(np, "brcm,pcie-ob-axi-offset",
					   &val);
		if (ret) {
			dev_err(dev,
				"missing brcm,pcie-ob-axi-offset property\n");
			return ret;
		}
		pcie->ob.axi_offset = val;
		pcie->need_ob_cfg = true;
	}

	/* PHY use is optional */
	pcie->phy = devm_phy_get(dev, "pcie-phy");
	if (IS_ERR(pcie->phy)) {
		if (PTR_ERR(pcie->phy) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		pcie->phy = NULL;
	}

	ret = of_pci_get_host_bridge_resources(np, 0, 0xff, &res, &iobase);
	if (ret) {
		dev_err(dev,
			"unable to get PCI host bridge resources\n");
		return ret;
	}

	/* PAXC doesn't support legacy IRQs, skip mapping */
	switch (pcie->type) {
	case IPROC_PCIE_PAXC:
	case IPROC_PCIE_PAXC_V2:
		break;
	default:
		pcie->map_irq = of_irq_parse_and_map_pci;
	}

	ret = iproc_pcie_setup(pcie, &res);
	if (ret)
		dev_err(dev, "PCIe controller setup failed\n");

	pci_free_resource_list(&res);

	platform_set_drvdata(pdev, pcie);
	return ret;
}

static int iproc_pcie_pltfm_remove(struct platform_device *pdev)
{
	struct iproc_pcie *pcie = platform_get_drvdata(pdev);

	return iproc_pcie_remove(pcie);
}
/* FX 06/24/17 Start */
#ifdef CONFIG_PM
static int iproc_pcie_pltfm_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct iproc_pcie *pcie = platform_get_drvdata(pdev);

	return iproc_pcie_suspend(pcie);
}

static int iproc_pcie_pltfm_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct device_node *np = pdev->dev.of_node;
	struct iproc_pcie *pcie = platform_get_drvdata(pdev);
	resource_size_t iobase = 0;
	LIST_HEAD(res);
	int ret;

	ret = of_pci_get_host_bridge_resources(np, 0, 0xff, &res, &iobase);
	if (ret) {
		dev_err(pcie->dev,
			"unable to get PCI host bridge resources\n");
		return ret;
	}

	return iproc_pcie_resume(pcie, &res);
}

static const struct dev_pm_ops iproc_pcie_pltfm_pm_ops = {
	.suspend_noirq = iproc_pcie_pltfm_suspend,
	.resume_noirq = iproc_pcie_pltfm_resume
};

#define IPROC_PCIE_PLTFM_PM_OPS (&iproc_pcie_pltfm_pm_ops)
#else
#define IPROC_PCIE_PLTFM_PM_OPS NULL
#endif /* CONFIG_PM */
/* FX 06/24/17 End */
static struct platform_driver iproc_pcie_pltfm_driver = {
	.driver = {
		.name = "iproc-pcie",
		.of_match_table = of_match_ptr(iproc_pcie_of_match_table),
		.pm = IPROC_PCIE_PLTFM_PM_OPS /* FX 06/24/17 */
	},
	.probe = iproc_pcie_pltfm_probe,
	.remove = iproc_pcie_pltfm_remove,
};
module_platform_driver(iproc_pcie_pltfm_driver);

MODULE_AUTHOR("Ray Jui <rjui@broadcom.com>");
MODULE_DESCRIPTION("Broadcom iPROC PCIe platform driver");
MODULE_LICENSE("GPL v2");
