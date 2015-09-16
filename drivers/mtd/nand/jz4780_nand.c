/*
 * JZ4780 NAND driver
 *
 * Copyright (c) 2015 Imagination Technologies
 * Author: Alex Smith <alex.smith@imgtec.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <linux/of_mtd.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/partitions.h>

#include <linux/jz4780-nemc.h>

#include "jz4780_bch.h"

#define DRV_NAME	"jz4780-nand"

#define OFFSET_DATA	0x00000000
#define OFFSET_CMD	0x00400000
#define OFFSET_ADDR	0x00800000

/* Command delay when there is no R/B pin (in microseconds). */
#define RB_DELAY	100

struct jz4780_nand_chip {
	unsigned int bank;
	void __iomem *base;
};

struct jz4780_nand {
	struct mtd_info mtd;
	struct nand_chip chip;

	struct device *dev;
	struct device *bch;

	struct nand_ecclayout ecclayout;

	int busy_gpio;
	int wp_gpio;
	unsigned int busy_gpio_active_low: 1;
	unsigned int wp_gpio_active_low: 1;
	unsigned int reading: 1;

	unsigned int num_banks;
	int selected;
	struct jz4780_nand_chip chips[];
};

static inline struct jz4780_nand *to_jz4780_nand(struct mtd_info *mtd)
{
	return container_of(mtd, struct jz4780_nand, mtd);
}

static void jz4780_nand_select_chip(struct mtd_info *mtd, int chipnr)
{
	struct jz4780_nand *nand = to_jz4780_nand(mtd);
	struct jz4780_nand_chip *chip;

	if (chipnr == -1) {
		/* Ensure the currently selected chip is deasserted. */
		if (nand->selected >= 0) {
			chip = &nand->chips[nand->selected];
			jz4780_nemc_assert(nand->dev, chip->bank, false);
		}
	} else {
		chip = &nand->chips[chipnr];
		nand->chip.IO_ADDR_R = chip->base + OFFSET_DATA;
		nand->chip.IO_ADDR_W = chip->base + OFFSET_DATA;
	}

	nand->selected = chipnr;
}

static void jz4780_nand_cmd_ctrl(struct mtd_info *mtd, int cmd,
				 unsigned int ctrl)
{
	struct jz4780_nand *nand = to_jz4780_nand(mtd);
	struct jz4780_nand_chip *chip;

	if (WARN_ON(nand->selected < 0))
		return;

	chip = &nand->chips[nand->selected];

	if (ctrl & NAND_CTRL_CHANGE) {
		if (ctrl & NAND_ALE)
			nand->chip.IO_ADDR_W = chip->base + OFFSET_ADDR;
		else if (ctrl & NAND_CLE)
			nand->chip.IO_ADDR_W = chip->base + OFFSET_CMD;
		else
			nand->chip.IO_ADDR_W = chip->base + OFFSET_DATA;

		jz4780_nemc_assert(nand->dev, chip->bank, ctrl & NAND_NCE);
	}

	if (cmd != NAND_CMD_NONE)
		writeb(cmd, nand->chip.IO_ADDR_W);
}

static int jz4780_nand_dev_ready(struct mtd_info *mtd)
{
	struct jz4780_nand *nand = to_jz4780_nand(mtd);

	return !(gpio_get_value(nand->busy_gpio) ^ nand->busy_gpio_active_low);
}

static void jz4780_nand_ecc_hwctl(struct mtd_info *mtd, int mode)
{
	struct jz4780_nand *nand = to_jz4780_nand(mtd);

	nand->reading = (mode == NAND_ECC_READ);
}

static int jz4780_nand_ecc_calculate(struct mtd_info *mtd, const uint8_t *dat,
				     uint8_t *ecc_code)
{
	struct jz4780_nand *nand = to_jz4780_nand(mtd);
	struct jz4780_bch_params params;

	/*
	 * Don't need to generate the ECC when reading, BCH does it for us as
	 * part of decoding/correction.
	 */
	if (nand->reading)
		return 0;

	params.size = nand->chip.ecc.size;
	params.bytes = nand->chip.ecc.bytes;
	params.strength = nand->chip.ecc.strength;

	return jz4780_bch_calculate(nand->bch, &params, dat, ecc_code);
}

static int jz4780_nand_ecc_correct(struct mtd_info *mtd, uint8_t *dat,
				   uint8_t *read_ecc, uint8_t *calc_ecc)
{
	struct jz4780_nand *nand = to_jz4780_nand(mtd);
	struct jz4780_bch_params params;

	params.size = nand->chip.ecc.size;
	params.bytes = nand->chip.ecc.bytes;
	params.strength = nand->chip.ecc.strength;

	return jz4780_bch_correct(nand->bch, &params, dat, read_ecc);
}

static void jz4780_nand_init_ecc(struct jz4780_nand *nand, struct device *dev)
{
	struct mtd_info *mtd = &nand->mtd;
	struct nand_chip *chip = &nand->chip;
	struct nand_ecclayout *layout = &nand->ecclayout;
	uint32_t start, i;

	chip->ecc.size = of_get_nand_ecc_step_size(dev->of_node);
	if (chip->ecc.size < 0)
		chip->ecc.size = 1024;

	chip->ecc.strength = of_get_nand_ecc_strength(dev->of_node);
	if (chip->ecc.strength < 0)
		chip->ecc.strength = 24;

	chip->ecc.bytes = fls(1 + 8 * chip->ecc.size) * chip->ecc.strength / 8;

	dev_info(dev, "using %s BCH (strength %d, size %d, bytes %d)\n",
		 (nand->bch) ? "hardware" : "software", chip->ecc.strength,
		 chip->ecc.size, chip->ecc.bytes);

	if (nand->bch) {
		chip->ecc.mode = NAND_ECC_HW;
		chip->ecc.hwctl = jz4780_nand_ecc_hwctl;
		chip->ecc.calculate = jz4780_nand_ecc_calculate;
		chip->ecc.correct = jz4780_nand_ecc_correct;
	} else {
		chip->ecc.mode = NAND_ECC_SOFT_BCH;
	}

	/* Generate ECC layout. ECC codes are right aligned in the OOB area. */
	layout->eccbytes = mtd->writesize / chip->ecc.size * chip->ecc.bytes;
	start = mtd->oobsize - layout->eccbytes;
	for (i = 0; i < layout->eccbytes; i++)
		layout->eccpos[i] = start + i;

	layout->oobfree[0].offset = 2;
	layout->oobfree[0].length = mtd->oobsize - layout->eccbytes - 2;

	chip->ecc.layout = layout;
}

static int jz4780_nand_init_chips(struct jz4780_nand *nand,
				  struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct jz4780_nand_chip *chip;
	const __be32 *prop;
	struct resource *res;
	int i = 0;

	/*
	 * Iterate over each bank assigned to this device and request resources.
	 * The bank numbers may not be consecutive, but nand_scan_ident()
	 * expects chip numbers to be, so fill out a consecutive array of chips
	 * which map chip number to actual bank number.
	 */
	while ((prop = of_get_address(dev->of_node, i, NULL, NULL))) {
		chip = &nand->chips[i];
		chip->bank = of_read_number(prop, 1);

		jz4780_nemc_set_type(nand->dev, chip->bank,
				     JZ4780_NEMC_BANK_NAND);

		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		chip->base = devm_ioremap_resource(dev, res);
		if (IS_ERR(chip->base))
			return PTR_ERR(chip->base);

		i++;
	}

	return 0;
}

static int jz4780_nand_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	unsigned int num_banks;
	struct jz4780_nand *nand;
	struct device_node *bch_np;
	struct mtd_info *mtd;
	struct nand_chip *chip;
	enum of_gpio_flags flags;
	struct mtd_part_parser_data ppdata;
	int ret;

	num_banks = jz4780_nemc_num_banks(dev);

	nand = devm_kzalloc(dev,
			sizeof(*nand) + (sizeof(nand->chips[0]) * num_banks),
			GFP_KERNEL);
	if (!nand)
		return -ENOMEM;

	nand->dev = dev;
	nand->num_banks = num_banks;
	nand->selected = -1;

	/* Look for the BCH controller. */
	bch_np = of_parse_phandle(dev->of_node, "ingenic,bch-controller", 0);
	if (bch_np) {
		ret = jz4780_bch_get(bch_np, &nand->bch);
		of_node_put(bch_np);
		if (ret)
			return ret;
	}

	mtd = &nand->mtd;
	chip = &nand->chip;
	mtd->priv = chip;
	mtd->owner = THIS_MODULE;
	mtd->name = DRV_NAME;
	mtd->dev.parent = dev;

	chip->chip_delay = RB_DELAY;
	chip->options = NAND_NO_SUBPAGE_WRITE;
	chip->bbt_options = NAND_BBT_USE_FLASH;
	chip->select_chip = jz4780_nand_select_chip;
	chip->cmd_ctrl = jz4780_nand_cmd_ctrl;

	nand->busy_gpio = of_get_named_gpio_flags(dev->of_node,
						  "rb-gpios",
						  0, &flags);
	if (gpio_is_valid(nand->busy_gpio)) {
		ret = devm_gpio_request(dev, nand->busy_gpio, "NAND busy");
		if (ret) {
			dev_err(dev, "failed to request busy GPIO %d: %d\n",
				nand->busy_gpio, ret);
			goto err_release_bch;
		}

		nand->busy_gpio_active_low = flags & OF_GPIO_ACTIVE_LOW;
		gpio_direction_input(nand->busy_gpio);

		chip->dev_ready = jz4780_nand_dev_ready;
	}

	nand->wp_gpio = of_get_named_gpio_flags(dev->of_node, "wp-gpios",
						0, &flags);
	if (gpio_is_valid(nand->wp_gpio)) {
		ret = devm_gpio_request(dev, nand->wp_gpio, "NAND WP");
		if (ret) {
			dev_err(dev, "failed to request WP GPIO %d: %d\n",
				nand->wp_gpio, ret);
			goto err_release_bch;
		}

		nand->wp_gpio_active_low = flags & OF_GPIO_ACTIVE_LOW;
		gpio_direction_output(nand->wp_gpio, nand->wp_gpio_active_low);
	}

	ret = jz4780_nand_init_chips(nand, pdev);
	if (ret)
		goto err_release_bch;

	ret = nand_scan_ident(mtd, num_banks, NULL);
	if (ret)
		goto err_release_bch;

	jz4780_nand_init_ecc(nand, dev);

	ret = nand_scan_tail(mtd);
	if (ret)
		goto err_release_bch;

	ppdata.of_node = dev->of_node;
	ret = mtd_device_parse_register(mtd, NULL, &ppdata, NULL, 0);
	if (ret)
		goto err_release_nand;

	platform_set_drvdata(pdev, nand);
	return 0;

err_release_nand:
	nand_release(mtd);

err_release_bch:
	if (nand->bch)
		jz4780_bch_release(nand->bch);

	return ret;
}

static int jz4780_nand_remove(struct platform_device *pdev)
{
	struct jz4780_nand *nand = platform_get_drvdata(pdev);

	if (nand->bch)
		jz4780_bch_release(nand->bch);

	return 0;
}

static const struct of_device_id jz4780_nand_dt_match[] = {
	{ .compatible = "ingenic,jz4780-nand" },
	{},
};
MODULE_DEVICE_TABLE(of, jz4780_nand_dt_match);

static struct platform_driver jz4780_nand_driver = {
	.probe		= jz4780_nand_probe,
	.remove		= jz4780_nand_remove,
	.driver	= {
		.name	= DRV_NAME,
		.of_match_table = of_match_ptr(jz4780_nand_dt_match),
	},
};
module_platform_driver(jz4780_nand_driver);

MODULE_AUTHOR("Alex Smith <alex.smith@imgtec.com>");
MODULE_DESCRIPTION("Ingenic JZ4780 NAND driver");
MODULE_LICENSE("GPL v2");
