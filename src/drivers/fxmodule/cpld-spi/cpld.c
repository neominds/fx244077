// -*- tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
/*
 * Copyright (C) 2017 Fuji Xerox Co.,Ltd. All rights reserved.
 *
 * Author:
 *   Tomohiro Masubuchi <Tomohiro.Masubuchi@fujixerox.co.jp>, Aug 2015
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * The full GNU General Public License is included in this distribution in the
 * file called COPYING.
 */

//#define DEBUG

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <asm/io.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>

#define DRIVER_NAME         "cplddrv"

struct cpld {
	struct spi_device *spi;
} cpld;

int cpld_reg_read(u8 reg)
{
	u8 txbuf[3] = { 0x03, reg, 0x00 };
	u8 rxbuf[3] = { 0 };
	struct spi_transfer t = {
		.tx_buf = txbuf,
		.rx_buf = rxbuf,
		.len = 3,
	};
	int ret;

//	printk("%s: tx:%02x %02x %02x\n", __func__, txbuf[0], txbuf[1], txbuf[2]);
	ret = spi_sync_transfer(cpld.spi, &t, 1);
//	printk("%s: ret=%d rx:%02x %02x %02x\n", __func__, ret, rxbuf[0], rxbuf[1], rxbuf[2]);
	if (ret < 0)
		return ret;

	return rxbuf[2];
}

int cpld_reg_write(u8 reg, u8 val)
{
	u8 txbuf[3] = { 0x2, reg, val };
	int ret;

//	printk("%s: tx:%02x %02x %02x\n", __func__, txbuf[0], txbuf[1], txbuf[2]);
	ret = spi_write(cpld.spi, txbuf, 3);
//	printk("%s: ret=%d\n", __func__, ret);
	return ret;
}

static ssize_t cpld_regs_show(struct device *dev,
			      struct device_attribute *attr, char *buff)
{
	char *ptr = buff;
	int i;

	for (i = 0; i <= 0x1f; i++) {
		if (i % 16 == 0)
			ptr += sprintf(ptr, "%02x:", i);
		ptr += sprintf(ptr, " %02x", cpld_reg_read(i));
		if (i % 16 == 15)
			ptr += sprintf(ptr, "\n");
	}
	if (i % 16)
		ptr += sprintf(ptr, "\n");
	return ptr - buff;
}

static ssize_t cpld_regs_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	unsigned int reg;
	unsigned int value;
	char *ptr;
	int ret;

	reg = simple_strtoul(buf, &ptr, 0);
	if (*ptr != ' ')
		return -EINVAL;

	ptr++;
	value = simple_strtoul(ptr, &ptr, 0);
	if (*ptr > ' ')
		return -EINVAL;

	if (reg >= 0x20 || value >= 0x100)
		return -EINVAL;

	printk("Write 0x%02x <= 0x%02x\n", reg, value);
	ret = cpld_reg_write(reg, value);
	if (ret < 0)
		return ret;

	return count;
}

static DEVICE_ATTR(regs, 0600, cpld_regs_show, cpld_regs_store);

static int
cpld_spi_probe(struct spi_device *spi)
{
	int ret;

	printk("%s:\n", __func__);

	cpld.spi = spi;

//	spi_ci.irq = spi->irq;
///	spi_ci.client = spi;
//	spi_ci.dev = &spi->dev;

	ret = device_create_file(&spi->dev, &dev_attr_regs);
	if (ret)
		dev_err(&spi->dev, "failed to create debug regs file\n");

	return 0;
//	return cpld_probe(&spi_ci, id->driver_data);
}

static int cpld_spi_remove(struct spi_device *spi)
{
//	struct cpld *cpld = spi_get_drvdata(spi);
	printk("%s:\n", __func__);

	return 0;
//	return cpld_remove(cpld);
}


static const struct of_device_id cpld_spi_of_match[] = {
	{ .compatible = "fx,cpld", },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, cpld_spi_of_match);

static const struct spi_device_id cpld_spi_id[] = {
	{ "cpld", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, cpld_id);

static struct spi_driver cpld_spi_driver = {
	.driver = {
		.name	= "cpld-spi",
		.of_match_table = of_match_ptr(cpld_spi_of_match),
#ifdef CONFIG_PM
//		.pm	= &cpld_dev_pm_ops,
#endif
	},
	.probe		= cpld_spi_probe,
	.remove		= cpld_spi_remove,
	.id_table	= cpld_spi_id,
};

/**
 * cpld_init() - Register CPLD pci driver
 */
static int __init cpld_init(void)
{
	int status = 0;

	printk("%s:\n", __func__);
	pr_info("cplddrv: started\n");

	status = spi_register_driver(&cpld_spi_driver);
	if (status < 0) {
		pr_err("cplddrv: failed to pci_register_driver() status = %d\n",
		       status);
	}
	return status;
}
module_init(cpld_init);

/**
 * cpld_exit() - Unregister CPLD pci driver
 */
static void __exit cpld_exit(void)
{
	printk("%s:\n", __func__);
	spi_unregister_driver(&cpld_spi_driver);
}
module_exit(cpld_exit);

MODULE_DESCRIPTION ("CPLD Driver");
MODULE_AUTHOR ("Fuji Xerox Co.,Ltd.");
MODULE_LICENSE ("GPL");
