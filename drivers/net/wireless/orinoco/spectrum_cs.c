/*
 * Driver for 802.11b cards using RAM-loadable Symbol firmware, such as
 * Symbol Wireless Networker LA4137, CompactFlash cards by Socket
 * Communications and Intel PRO/Wireless 2011B.
 *
 * The driver implements Symbol firmware download.  The rest is handled
 * in hermes.c and main.c.
 *
 * Utilities for downloading the Symbol firmware are available at
 * http://sourceforge.net/projects/orinoco/
 *
 * Copyright (C) 2002-2005 Pavel Roskin <proski@gnu.org>
 * Portions based on orinoco_cs.c:
 * 	Copyright (C) David Gibson, Linuxcare Australia
 * Portions based on Spectrum24tDnld.c from original spectrum24 driver:
 * 	Copyright (C) Symbol Technologies.
 *
 * See copyright notice in file main.c.
 */

#define DRIVER_NAME "spectrum_cs"
#define PFX DRIVER_NAME ": "

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <pcmcia/cs_types.h>
#include <pcmcia/cs.h>
#include <pcmcia/cistpl.h>
#include <pcmcia/cisreg.h>
#include <pcmcia/ds.h>

#include "orinoco.h"

/********************************************************************/
/* Module stuff							    */
/********************************************************************/

MODULE_AUTHOR("Pavel Roskin <proski@gnu.org>");
MODULE_DESCRIPTION("Driver for Symbol Spectrum24 Trilogy cards with firmware downloader");
MODULE_LICENSE("Dual MPL/GPL");

/* Module parameters */

/* Some D-Link cards have buggy CIS. They do work at 5v properly, but
 * don't have any CIS entry for it. This workaround it... */
static int ignore_cis_vcc; /* = 0 */
module_param(ignore_cis_vcc, int, 0);
MODULE_PARM_DESC(ignore_cis_vcc, "Allow voltage mismatch between card and socket");

/********************************************************************/
/* Data structures						    */
/********************************************************************/

/* PCMCIA specific device information (goes in the card field of
 * struct orinoco_private */
struct orinoco_pccard {
	struct pcmcia_device	*p_dev;
	dev_node_t node;
};

/********************************************************************/
/* Function prototypes						    */
/********************************************************************/

static int spectrum_cs_config(struct pcmcia_device *link);
static void spectrum_cs_release(struct pcmcia_device *link);

/* Constants for the CISREG_CCSR register */
#define HCR_RUN		0x07	/* run firmware after reset */
#define HCR_IDLE	0x0E	/* don't run firmware after reset */
#define HCR_MEM16	0x10	/* memory width bit, should be preserved */


#define CS_CHECK(fn, ret) \
  do { last_fn = (fn); if ((last_ret = (ret)) != 0) goto cs_failed; } while (0)

/*
 * Reset the card using configuration registers COR and CCSR.
 * If IDLE is 1, stop the firmware, so that it can be safely rewritten.
 */
static int
spectrum_reset(struct pcmcia_device *link, int idle)
{
	int last_ret, last_fn;
	conf_reg_t reg;
	u_int save_cor;

	/* Doing it if hardware is gone is guaranteed crash */
	if (!pcmcia_dev_present(link))
		return -ENODEV;

	/* Save original COR value */
	reg.Function = 0;
	reg.Action = CS_READ;
	reg.Offset = CISREG_COR;
	CS_CHECK(AccessConfigurationRegister,
		 pcmcia_access_configuration_register(link, &reg));
	save_cor = reg.Value;

	/* Soft-Reset card */
	reg.Action = CS_WRITE;
	reg.Offset = CISREG_COR;
	reg.Value = (save_cor | COR_SOFT_RESET);
	CS_CHECK(AccessConfigurationRegister,
		 pcmcia_access_configuration_register(link, &reg));
	udelay(1000);

	/* Read CCSR */
	reg.Action = CS_READ;
	reg.Offset = CISREG_CCSR;
	CS_CHECK(AccessConfigurationRegister,
		 pcmcia_access_configuration_register(link, &reg));

	/*
	 * Start or stop the firmware.  Memory width bit should be
	 * preserved from the value we've just read.
	 */
	reg.Action = CS_WRITE;
	reg.Offset = CISREG_CCSR;
	reg.Value = (idle ? HCR_IDLE : HCR_RUN) | (reg.Value & HCR_MEM16);
	CS_CHECK(AccessConfigurationRegister,
		 pcmcia_access_configuration_register(link, &reg));
	udelay(1000);

	/* Restore original COR configuration index */
	reg.Action = CS_WRITE;
	reg.Offset = CISREG_COR;
	reg.Value = (save_cor & ~COR_SOFT_RESET);
	CS_CHECK(AccessConfigurationRegister,
		 pcmcia_access_configuration_register(link, &reg));
	udelay(1000);
	return 0;

cs_failed:
	cs_error(link, last_fn, last_ret);
	return -ENODEV;
}

/********************************************************************/
/* Device methods     						    */
/********************************************************************/

static int
spectrum_cs_hard_reset(struct orinoco_private *priv)
{
	struct orinoco_pccard *card = priv->card;
	struct pcmcia_device *link = card->p_dev;

	/* Soft reset using COR and HCR */
	spectrum_reset(link, 0);

	return 0;
}

static int
spectrum_cs_stop_firmware(struct orinoco_private *priv, int idle)
{
	struct orinoco_pccard *card = priv->card;
	struct pcmcia_device *link = card->p_dev;

	return spectrum_reset(link, idle);
}

/********************************************************************/
/* PCMCIA stuff     						    */
/********************************************************************/

/*
 * This creates an "instance" of the driver, allocating local data
 * structures for one device.  The device is registered with Card
 * Services.
 *
 * The dev_link structure is initialized, but we don't actually
 * configure the card at this point -- we wait until we receive a card
 * insertion event.  */
static int
spectrum_cs_probe(struct pcmcia_device *link)
{
	struct orinoco_private *priv;
	struct orinoco_pccard *card;

	priv = alloc_orinocodev(sizeof(*card), &handle_to_dev(link),
				spectrum_cs_hard_reset,
				spectrum_cs_stop_firmware);
	if (!priv)
		return -ENOMEM;
	card = priv->card;

	/* Link both structures together */
	card->p_dev = link;
	link->priv = priv;

	/* Interrupt setup */
	link->irq.Attributes = IRQ_TYPE_DYNAMIC_SHARING | IRQ_HANDLE_PRESENT;
	link->irq.IRQInfo1 = IRQ_LEVEL_ID;
	link->irq.Handler = orinoco_interrupt;
	link->irq.Instance = priv;

	/* General socket configuration defaults can go here.  In this
	 * client, we assume very little, and rely on the CIS for
	 * almost everything.  In most clients, many details (i.e.,
	 * number, sizes, and attributes of IO windows) are fixed by
	 * the nature of the device, and can be hard-wired here. */
	link->conf.Attributes = 0;
	link->conf.IntType = INT_MEMORY_AND_IO;

	return spectrum_cs_config(link);
}				/* spectrum_cs_attach */

/*
 * This deletes a driver "instance".  The device is de-registered with
 * Card Services.  If it has been released, all local data structures
 * are freed.  Otherwise, the structures will be freed when the device
 * is released.
 */
static void spectrum_cs_detach(struct pcmcia_device *link)
{
	struct orinoco_private *priv = link->priv;

	if (link->dev_node)
		orinoco_if_del(priv);

	spectrum_cs_release(link);

	free_orinocodev(priv);
}				/* spectrum_cs_detach */

/*
 * spectrum_cs_config() is scheduled to run after a CARD_INSERTION
 * event is received, to configure the PCMCIA socket, and to make the
 * device available to the system.
 */

static int spectrum_cs_config_check(struct pcmcia_device *p_dev,
				    cistpl_cftable_entry_t *cfg,
				    cistpl_cftable_entry_t *dflt,
				    unsigned int vcc,
				    void *priv_data)
{
	if (cfg->index == 0)
		goto next_entry;

	/* Use power settings for Vcc and Vpp if present */
	/* Note that the CIS values need to be rescaled */
	if (cfg->vcc.present & (1 << CISTPL_POWER_VNOM)) {
		if (vcc != cfg->vcc.param[CISTPL_POWER_VNOM] / 10000) {
			DEBUG(2, "%s: Vcc mismatch (vcc = %d, CIS = %d)\n",
			      __func__, vcc,
			      cfg->vcc.param[CISTPL_POWER_VNOM] / 10000);
			if (!ignore_cis_vcc)
				goto next_entry;
		}
	} else if (dflt->vcc.present & (1 << CISTPL_POWER_VNOM)) {
		if (vcc != dflt->vcc.param[CISTPL_POWER_VNOM] / 10000) {
			DEBUG(2, "%s: Vcc mismatch (vcc = %d, CIS = %d)\n",
			      __func__, vcc,
			      dflt->vcc.param[CISTPL_POWER_VNOM] / 10000);
			if (!ignore_cis_vcc)
				goto next_entry;
		}
	}

	if (cfg->vpp1.present & (1 << CISTPL_POWER_VNOM))
		p_dev->conf.Vpp =
			cfg->vpp1.param[CISTPL_POWER_VNOM] / 10000;
	else if (dflt->vpp1.present & (1 << CISTPL_POWER_VNOM))
		p_dev->conf.Vpp =
			dflt->vpp1.param[CISTPL_POWER_VNOM] / 10000;

	/* Do we need to allocate an interrupt? */
	p_dev->conf.Attributes |= CONF_ENABLE_IRQ;

	/* IO window settings */
	p_dev->io.NumPorts1 = p_dev->io.NumPorts2 = 0;
	if ((cfg->io.nwin > 0) || (dflt->io.nwin > 0)) {
		cistpl_io_t *io = (cfg->io.nwin) ? &cfg->io : &dflt->io;
		p_dev->io.Attributes1 = IO_DATA_PATH_WIDTH_AUTO;
		if (!(io->flags & CISTPL_IO_8BIT))
			p_dev->io.Attributes1 = IO_DATA_PATH_WIDTH_16;
		if (!(io->flags & CISTPL_IO_16BIT))
			p_dev->io.Attributes1 = IO_DATA_PATH_WIDTH_8;
		p_dev->io.IOAddrLines = io->flags & CISTPL_IO_LINES_MASK;
		p_dev->io.BasePort1 = io->win[0].base;
		p_dev->io.NumPorts1 = io->win[0].len;
		if (io->nwin > 1) {
			p_dev->io.Attributes2 = p_dev->io.Attributes1;
			p_dev->io.BasePort2 = io->win[1].base;
			p_dev->io.NumPorts2 = io->win[1].len;
		}

		/* This reserves IO space but doesn't actually enable it */
		if (pcmcia_request_io(p_dev, &p_dev->io) != 0)
			goto next_entry;
	}
	return 0;

next_entry:
	pcmcia_disable_device(p_dev);
	return -ENODEV;
};

static int
spectrum_cs_config(struct pcmcia_device *link)
{
	struct orinoco_private *priv = link->priv;
	struct orinoco_pccard *card = priv->card;
	hermes_t *hw = &priv->hw;
	int last_fn, last_ret;
	void __iomem *mem;

	/*
	 * In this loop, we scan the CIS for configuration table
	 * entries, each of which describes a valid card
	 * configuration, including voltage, IO window, memory window,
	 * and interrupt settings.
	 *
	 * We make no assumptions about the card to be configured: we
	 * use just the information available in the CIS.  In an ideal
	 * world, this would work for any PCMCIA card, but it requires
	 * a complete and accurate CIS.  In practice, a driver usually
	 * "knows" most of these things without consulting the CIS,
	 * and most client drivers will only use the CIS to fill in
	 * implementation-defined details.
	 */
	last_ret = pcmcia_loop_config(link, spectrum_cs_config_check, NULL);
	if (last_ret) {
		if (!ignore_cis_vcc)
			printk(KERN_ERR PFX "GetNextTuple(): No matching "
			       "CIS configuration.  Maybe you need the "
			       "ignore_cis_vcc=1 parameter.\n");
		cs_error(link, RequestIO, last_ret);
		goto failed;
	}

	/*
	 * Allocate an interrupt line.  Note that this does not assign
	 * a handler to the interrupt, unless the 'Handler' member of
	 * the irq structure is initialized.
	 */
	CS_CHECK(RequestIRQ, pcmcia_request_irq(link, &link->irq));

	/* We initialize the hermes structure before completing PCMCIA
	 * configuration just in case the interrupt handler gets
	 * called. */
	mem = ioport_map(link->io.BasePort1, link->io.NumPorts1);
	if (!mem)
		goto cs_failed;

	hermes_struct_init(hw, mem, HERMES_16BIT_REGSPACING);

	/*
	 * This actually configures the PCMCIA socket -- setting up
	 * the I/O windows and the interrupt mapping, and putting the
	 * card and host interface into "Memory and IO" mode.
	 */
	CS_CHECK(RequestConfiguration,
		 pcmcia_request_configuration(link, &link->conf));

	/* Ok, we have the configuration, prepare to register the netdev */
	card->node.major = card->node.minor = 0;

	/* Reset card */
	if (spectrum_cs_hard_reset(priv) != 0)
		goto failed;

	/* Initialise the main driver */
	if (orinoco_init(priv) != 0) {
		printk(KERN_ERR PFX "orinoco_init() failed\n");
		goto failed;
	}

	/* Register an interface with the stack */
	if (orinoco_if_add(priv, link->io.BasePort1,
			   link->irq.AssignedIRQ) != 0) {
		printk(KERN_ERR PFX "orinoco_if_add() failed\n");
		goto failed;
	}

	/* At this point, the dev_node_t structure(s) needs to be
	 * initialized and arranged in a linked list at link->dev_node. */
	strcpy(card->node.dev_name, priv->ndev->name);
	link->dev_node = &card->node; /* link->dev_node being non-NULL is also
				       * used to indicate that the
				       * net_device has been registered */
	return 0;

 cs_failed:
	cs_error(link, last_fn, last_ret);

 failed:
	spectrum_cs_release(link);
	return -ENODEV;
}				/* spectrum_cs_config */

/*
 * After a card is removed, spectrum_cs_release() will unregister the
 * device, and release the PCMCIA configuration.  If the device is
 * still open, this will be postponed until it is closed.
 */
static void
spectrum_cs_release(struct pcmcia_device *link)
{
	struct orinoco_private *priv = link->priv;
	unsigned long flags;

	/* We're committed to taking the device away now, so mark the
	 * hardware as unavailable */
	spin_lock_irqsave(&priv->lock, flags);
	priv->hw_unavailable++;
	spin_unlock_irqrestore(&priv->lock, flags);

	pcmcia_disable_device(link);
	if (priv->hw.iobase)
		ioport_unmap(priv->hw.iobase);
}				/* spectrum_cs_release */


static int
spectrum_cs_suspend(struct pcmcia_device *link)
{
	struct orinoco_private *priv = link->priv;
	int err = 0;

	/* Mark the device as stopped, to block IO until later */
	orinoco_down(priv);

	return err;
}

static int
spectrum_cs_resume(struct pcmcia_device *link)
{
	struct orinoco_private *priv = link->priv;
	int err = orinoco_up(priv);

	return err;
}


/********************************************************************/
/* Module initialization					    */
/********************************************************************/

/* Can't be declared "const" or the whole __initdata section will
 * become const */
static char version[] __initdata = DRIVER_NAME " " DRIVER_VERSION
	" (Pavel Roskin <proski@gnu.org>,"
	" David Gibson <hermes@gibson.dropbear.id.au>, et al)";

static struct pcmcia_device_id spectrum_cs_ids[] = {
	PCMCIA_DEVICE_MANF_CARD(0x026c, 0x0001), /* Symbol Spectrum24 LA4137 */
	PCMCIA_DEVICE_MANF_CARD(0x0104, 0x0001), /* Socket Communications CF */
	PCMCIA_DEVICE_PROD_ID12("Intel", "PRO/Wireless LAN PC Card", 0x816cc815, 0x6fbf459a), /* 2011B, not 2011 */
	PCMCIA_DEVICE_NULL,
};
MODULE_DEVICE_TABLE(pcmcia, spectrum_cs_ids);

static struct pcmcia_driver orinoco_driver = {
	.owner		= THIS_MODULE,
	.drv		= {
		.name	= DRIVER_NAME,
	},
	.probe		= spectrum_cs_probe,
	.remove		= spectrum_cs_detach,
	.suspend	= spectrum_cs_suspend,
	.resume		= spectrum_cs_resume,
	.id_table       = spectrum_cs_ids,
};

static int __init
init_spectrum_cs(void)
{
	printk(KERN_DEBUG "%s\n", version);

	return pcmcia_register_driver(&orinoco_driver);
}

static void __exit
exit_spectrum_cs(void)
{
	pcmcia_unregister_driver(&orinoco_driver);
}

module_init(init_spectrum_cs);
module_exit(exit_spectrum_cs);
