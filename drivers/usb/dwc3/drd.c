// SPDX-License-Identifier: GPL-2.0
/*
 * drd.c - DesignWare USB3 DRD Controller Dual-role support
 *
 * Copyright (C) 2017 Texas Instruments Incorporated - https://www.ti.com
 *
 * Authors: Roger Quadros <rogerq@ti.com>
 */

#include <linux/extcon.h>
#include <linux/gpio/consumer.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#include "debug.h"
#include "core.h"
#include "gadget.h"

static void dwc3_otg_disable_events(struct dwc3 *dwc, u32 disable_mask)
{
	u32 reg = dwc3_readl(dwc, DWC3_OEVTEN);

	reg &= ~(disable_mask);
	dwc3_writel(dwc, DWC3_OEVTEN, reg);
}

static void dwc3_otg_enable_events(struct dwc3 *dwc, u32 enable_mask)
{
	u32 reg = dwc3_readl(dwc, DWC3_OEVTEN);

	reg |= (enable_mask);
	dwc3_writel(dwc, DWC3_OEVTEN, reg);
}

static void dwc3_otg_clear_events(struct dwc3 *dwc)
{
	u32 reg = dwc3_readl(dwc, DWC3_OEVT);

	dwc3_writel(dwc, DWC3_OEVTEN, reg);
}

#define DWC3_OTG_ALL_EVENTS	(DWC3_OEVTEN_XHCIRUNSTPSETEN | \
		DWC3_OEVTEN_DEVRUNSTPSETEN | DWC3_OEVTEN_HIBENTRYEN | \
		DWC3_OEVTEN_CONIDSTSCHNGEN | DWC3_OEVTEN_HRRCONFNOTIFEN | \
		DWC3_OEVTEN_HRRINITNOTIFEN | DWC3_OEVTEN_ADEVIDLEEN | \
		DWC3_OEVTEN_ADEVBHOSTENDEN | DWC3_OEVTEN_ADEVHOSTEN | \
		DWC3_OEVTEN_ADEVHNPCHNGEN | DWC3_OEVTEN_ADEVSRPDETEN | \
		DWC3_OEVTEN_ADEVSESSENDDETEN | DWC3_OEVTEN_BDEVBHOSTENDEN | \
		DWC3_OEVTEN_BDEVHNPCHNGEN | DWC3_OEVTEN_BDEVSESSVLDDETEN | \
		DWC3_OEVTEN_BDEVVBUSCHNGEN)

static irqreturn_t dwc3_otg_thread_irq(int irq, void *_dwc)
{
	struct dwc3 *dwc = _dwc;

	spin_lock(&dwc->lock);
	if (dwc->otg_restart_host) {
		dwc3_otg_host_init(dwc);
		dwc->otg_restart_host = false;
	}

	spin_unlock(&dwc->lock);

	dwc3_set_mode(dwc, DWC3_GCTL_PRTCAP_OTG);

	return IRQ_HANDLED;
}

static irqreturn_t dwc3_otg_irq(int irq, void *_dwc)
{
	u32 reg;
	struct dwc3 *dwc = _dwc;
	irqreturn_t ret = IRQ_NONE;

	reg = dwc3_readl(dwc, DWC3_OEVT);
	if (reg) {
		/* ignore non OTG events, we can't disable them in OEVTEN */
		if (!(reg & DWC3_OTG_ALL_EVENTS)) {
			dwc3_writel(dwc, DWC3_OEVT, reg);
			return IRQ_NONE;
		}

		if (dwc->current_otg_role == DWC3_OTG_ROLE_HOST &&
		    !(reg & DWC3_OEVT_DEVICEMODE))
			dwc->otg_restart_host = true;
		dwc3_writel(dwc, DWC3_OEVT, reg);
		ret = IRQ_WAKE_THREAD;
	}

	return ret;
}

static void dwc3_otgregs_init(struct dwc3 *dwc)
{
	u32 reg;

	/*
	 * Prevent host/device reset from resetting OTG core.
	 * If we don't do this then xhci_reset (USBCMD.HCRST) will reset
	 * the signal outputs sent to the PHY, the OTG FSM logic of the
	 * core and also the resets to the VBUS filters inside the core.
	 */
	reg = dwc3_readl(dwc, DWC3_OCFG);
	reg |= DWC3_OCFG_SFTRSTMASK;
	dwc3_writel(dwc, DWC3_OCFG, reg);

	/* Disable hibernation for simplicity */
	reg = dwc3_readl(dwc, DWC3_GCTL);
	reg &= ~DWC3_GCTL_GBLHIBERNATIONEN;
	dwc3_writel(dwc, DWC3_GCTL, reg);

	/*
	 * Initialize OTG registers as per
	 * Figure 11-4 OTG Driver Overall Programming Flow
	 */
	/* OCFG.SRPCap = 0, OCFG.HNPCap = 0 */
	reg = dwc3_readl(dwc, DWC3_OCFG);
	reg &= ~(DWC3_OCFG_SRPCAP | DWC3_OCFG_HNPCAP);
	dwc3_writel(dwc, DWC3_OCFG, reg);
	/* OEVT = FFFF */
	dwc3_otg_clear_events(dwc);
	/* OEVTEN = 0 */
	dwc3_otg_disable_events(dwc, DWC3_OTG_ALL_EVENTS);
	/* OEVTEN.ConIDStsChngEn = 1. Instead we enable all events */
	dwc3_otg_enable_events(dwc, DWC3_OTG_ALL_EVENTS);
	/*
	 * OCTL.PeriMode = 1, OCTL.DevSetHNPEn = 0, OCTL.HstSetHNPEn = 0,
	 * OCTL.HNPReq = 0
	 */
	reg = dwc3_readl(dwc, DWC3_OCTL);
	reg |= DWC3_OCTL_PERIMODE;
	reg &= ~(DWC3_OCTL_DEVSETHNPEN | DWC3_OCTL_HSTSETHNPEN |
		 DWC3_OCTL_HNPREQ);
	dwc3_writel(dwc, DWC3_OCTL, reg);
}

static int dwc3_otg_get_irq(struct dwc3 *dwc)
{
	struct platform_device *dwc3_pdev = to_platform_device(dwc->dev);
	int irq;

	irq = platform_get_irq_byname_optional(dwc3_pdev, "otg");
	if (irq > 0)
		goto out;

	if (irq == -EPROBE_DEFER)
		goto out;

	irq = platform_get_irq_byname_optional(dwc3_pdev, "dwc_usb3");
	if (irq > 0)
		goto out;

	if (irq == -EPROBE_DEFER)
		goto out;

	irq = platform_get_irq(dwc3_pdev, 0);
	if (irq > 0)
		goto out;

	if (!irq)
		irq = -EINVAL;

out:
	return irq;
}

void dwc3_otg_init(struct dwc3 *dwc)
{
	u32 reg;

	/*
	 * As per Figure 11-4 OTG Driver Overall Programming Flow,
	 * block "Initialize GCTL for OTG operation".
	 */
	/* GCTL.PrtCapDir=2'b11 */
	dwc3_set_prtcap(dwc, DWC3_GCTL_PRTCAP_OTG, true);
	/* GUSB2PHYCFG0.SusPHY=0 */
	reg = dwc3_readl(dwc, DWC3_GUSB2PHYCFG(0));
	reg &= ~DWC3_GUSB2PHYCFG_SUSPHY;
	dwc3_writel(dwc, DWC3_GUSB2PHYCFG(0), reg);

	/* Initialize OTG registers */
	dwc3_otgregs_init(dwc);
}

void dwc3_otg_exit(struct dwc3 *dwc)
{
	/* disable all OTG IRQs */
	dwc3_otg_disable_events(dwc, DWC3_OTG_ALL_EVENTS);
	/* clear all events */
	dwc3_otg_clear_events(dwc);
}

/* should be called before Host controller driver is started */
void dwc3_otg_host_init(struct dwc3 *dwc)
{
	u32 reg;

	/* As per Figure 11-10 A-Device Flow Diagram */
	/* OCFG.HNPCap = 0, OCFG.SRPCap = 0. Already 0 */

	/*
	 * OCTL.PeriMode=0, OCTL.TermSelDLPulse = 0,
	 * OCTL.DevSetHNPEn = 0, OCTL.HstSetHNPEn = 0
	 */
	reg = dwc3_readl(dwc, DWC3_OCTL);
	reg &= ~(DWC3_OCTL_PERIMODE | DWC3_OCTL_TERMSELIDPULSE |
			DWC3_OCTL_DEVSETHNPEN | DWC3_OCTL_HSTSETHNPEN);
	dwc3_writel(dwc, DWC3_OCTL, reg);

	/*
	 * OCFG.DisPrtPwrCutoff = 0/1
	 */
	reg = dwc3_readl(dwc, DWC3_OCFG);
	reg &= ~DWC3_OCFG_DISPWRCUTTOFF;
	dwc3_writel(dwc, DWC3_OCFG, reg);

	/*
	 * OCFG.SRPCap = 1, OCFG.HNPCap = GHWPARAMS6.HNP_CAP
	 * We don't want SRP/HNP for simple dual-role so leave
	 * these disabled.
	 */

	/*
	 * OEVTEN.OTGADevHostEvntEn = 1
	 * OEVTEN.OTGADevSessEndDetEvntEn = 1
	 * We don't want HNP/role-swap so leave these disabled.
	 */

	/* GUSB2PHYCFG.ULPIAutoRes = 1/0, GUSB2PHYCFG.SusPHY = 1 */
	if (!dwc->dis_u2_susphy_quirk) {
		reg = dwc3_readl(dwc, DWC3_GUSB2PHYCFG(0));
		reg |= DWC3_GUSB2PHYCFG_SUSPHY;
		dwc3_writel(dwc, DWC3_GUSB2PHYCFG(0), reg);
	}

	/* Set Port Power to enable VBUS: OCTL.PrtPwrCtl = 1 */
	reg = dwc3_readl(dwc, DWC3_OCTL);
	reg |= DWC3_OCTL_PRTPWRCTL;
	dwc3_writel(dwc, DWC3_OCTL, reg);
}

/* should be called after Host controller driver is stopped */
static void dwc3_otg_host_exit(struct dwc3 *dwc)
{
	u32 reg;

	/*
	 * Exit from A-device flow as per
	 * Figure 11-4 OTG Driver Overall Programming Flow
	 */

	/*
	 * OEVTEN.OTGADevBHostEndEvntEn=0, OEVTEN.OTGADevHNPChngEvntEn=0
	 * OEVTEN.OTGADevSessEndDetEvntEn=0,
	 * OEVTEN.OTGADevHostEvntEn = 0
	 * But we don't disable any OTG events
	 */

	/* OCTL.HstSetHNPEn = 0, OCTL.PrtPwrCtl=0 */
	reg = dwc3_readl(dwc, DWC3_OCTL);
	reg &= ~(DWC3_OCTL_HSTSETHNPEN | DWC3_OCTL_PRTPWRCTL);
	dwc3_writel(dwc, DWC3_OCTL, reg);
}

/* should be called before the gadget controller driver is started */
static void dwc3_otg_device_init(struct dwc3 *dwc)
{
	u32 reg;

	/* As per Figure 11-20 B-Device Flow Diagram */

	/*
	 * OCFG.HNPCap = GHWPARAMS6.HNP_CAP, OCFG.SRPCap = 1
	 * but we keep them 0 for simple dual-role operation.
	 */
	reg = dwc3_readl(dwc, DWC3_OCFG);
	/* OCFG.OTGSftRstMsk = 0/1 */
	reg |= DWC3_OCFG_SFTRSTMASK;
	dwc3_writel(dwc, DWC3_OCFG, reg);
	/*
	 * OCTL.PeriMode = 1
	 * OCTL.TermSelDLPulse = 0/1, OCTL.HNPReq = 0
	 * OCTL.DevSetHNPEn = 0, OCTL.HstSetHNPEn = 0
	 */
	reg = dwc3_readl(dwc, DWC3_OCTL);
	reg |= DWC3_OCTL_PERIMODE;
	reg &= ~(DWC3_OCTL_TERMSELIDPULSE | DWC3_OCTL_HNPREQ |
			DWC3_OCTL_DEVSETHNPEN | DWC3_OCTL_HSTSETHNPEN);
	dwc3_writel(dwc, DWC3_OCTL, reg);
	/* OEVTEN.OTGBDevSesVldDetEvntEn = 1 */
	dwc3_otg_enable_events(dwc, DWC3_OEVTEN_BDEVSESSVLDDETEN);
	/* GUSB2PHYCFG.ULPIAutoRes = 0, GUSB2PHYCFG0.SusPHY = 1 */
	if (!dwc->dis_u2_susphy_quirk) {
		reg = dwc3_readl(dwc, DWC3_GUSB2PHYCFG(0));
		reg |= DWC3_GUSB2PHYCFG_SUSPHY;
		dwc3_writel(dwc, DWC3_GUSB2PHYCFG(0), reg);
	}
	/* GCTL.GblHibernationEn = 0. Already 0. */
}

/* should be called after the gadget controller driver is stopped */
static void dwc3_otg_device_exit(struct dwc3 *dwc)
{
	u32 reg;

	/*
	 * Exit from B-device flow as per
	 * Figure 11-4 OTG Driver Overall Programming Flow
	 */

	/*
	 * OEVTEN.OTGBDevHNPChngEvntEn = 0
	 * OEVTEN.OTGBDevVBusChngEvntEn = 0
	 * OEVTEN.OTGBDevBHostEndEvntEn = 0
	 */
	dwc3_otg_disable_events(dwc, DWC3_OEVTEN_BDEVHNPCHNGEN |
				DWC3_OEVTEN_BDEVVBUSCHNGEN |
				DWC3_OEVTEN_BDEVBHOSTENDEN);

	/* OCTL.DevSetHNPEn = 0, OCTL.HNPReq = 0, OCTL.PeriMode=1 */
	reg = dwc3_readl(dwc, DWC3_OCTL);
	reg &= ~(DWC3_OCTL_DEVSETHNPEN | DWC3_OCTL_HNPREQ);
	reg |= DWC3_OCTL_PERIMODE;
	dwc3_writel(dwc, DWC3_OCTL, reg);
}

void dwc3_otg_update(struct dwc3 *dwc, bool ignore_idstatus)
{
	int ret;
	u32 reg;
	int id;
	unsigned long flags;
	int i;

	if (dwc->dr_mode != USB_DR_MODE_OTG)
		return;

	/* don't do anything if debug user changed role to not OTG */
	if (dwc->current_dr_role != DWC3_GCTL_PRTCAP_OTG)
		return;

	if (!ignore_idstatus) {
		reg = dwc3_readl(dwc, DWC3_OSTS);
		id = !!(reg & DWC3_OSTS_CONIDSTS);

		dwc->desired_otg_role = id ? DWC3_OTG_ROLE_DEVICE :
					DWC3_OTG_ROLE_HOST;
	}

	if (dwc->desired_otg_role == dwc->current_otg_role)
		return;

	switch (dwc->current_otg_role) {
	case DWC3_OTG_ROLE_HOST:
		dwc3_host_exit(dwc);
		spin_lock_irqsave(&dwc->lock, flags);
		dwc3_otg_host_exit(dwc);
		spin_unlock_irqrestore(&dwc->lock, flags);
		break;
	case DWC3_OTG_ROLE_DEVICE:
		dwc3_gadget_exit(dwc);
		spin_lock_irqsave(&dwc->lock, flags);
		dwc3_event_buffers_cleanup(dwc);
		dwc3_otg_device_exit(dwc);
		spin_unlock_irqrestore(&dwc->lock, flags);
		break;
	default:
		break;
	}

	spin_lock_irqsave(&dwc->lock, flags);

	dwc->current_otg_role = dwc->desired_otg_role;

	spin_unlock_irqrestore(&dwc->lock, flags);

	switch (dwc->desired_otg_role) {
	case DWC3_OTG_ROLE_HOST:
		spin_lock_irqsave(&dwc->lock, flags);
		dwc3_otgregs_init(dwc);
		dwc3_otg_host_init(dwc);
		spin_unlock_irqrestore(&dwc->lock, flags);
		dwc3_pre_set_role(dwc, USB_ROLE_HOST);
		ret = dwc3_host_init(dwc);
		if (ret) {
			dev_err(dwc->dev, "failed to initialize host\n");
		} else {
			if (dwc->usb2_phy)
				otg_set_vbus(dwc->usb2_phy->otg, true);
			for (i = 0; i < dwc->num_usb2_ports; i++) {
				if (dwc->usb2_generic_phy[i]) {
					phy_set_mode(dwc->usb2_generic_phy[i],
						     PHY_MODE_USB_HOST);
				}
			}
		}
		break;
	case DWC3_OTG_ROLE_DEVICE:
		spin_lock_irqsave(&dwc->lock, flags);
		dwc3_otgregs_init(dwc);
		dwc3_otg_device_init(dwc);
		dwc3_event_buffers_setup(dwc);
		spin_unlock_irqrestore(&dwc->lock, flags);

		if (dwc->usb2_phy)
			otg_set_vbus(dwc->usb2_phy->otg, false);
		if (dwc->usb2_generic_phy[0])
			phy_set_mode(dwc->usb2_generic_phy[0], PHY_MODE_USB_DEVICE);
		dwc3_pre_set_role(dwc, USB_ROLE_DEVICE);
		ret = dwc3_gadget_init(dwc);
		if (ret)
			dev_err(dwc->dev, "failed to initialize peripheral\n");
		break;
	default:
		break;
	}
}

static void dwc3_drd_update(struct dwc3 *dwc)
{
	int id;

	if (dwc->edev) {
		id = extcon_get_state(dwc->edev, EXTCON_USB_HOST);
		if (id < 0)
			id = 0;
		dwc3_set_mode(dwc, id ?
			      DWC3_GCTL_PRTCAP_HOST :
			      DWC3_GCTL_PRTCAP_DEVICE);
	}
}

static int dwc3_drd_notifier(struct notifier_block *nb,
			     unsigned long event, void *ptr)
{
	struct dwc3 *dwc = container_of(nb, struct dwc3, edev_nb);
	u32 mode = event ? DWC3_GCTL_PRTCAP_HOST : DWC3_GCTL_PRTCAP_DEVICE;
	enum usb_role role = mode == DWC3_GCTL_PRTCAP_HOST ?
				     USB_ROLE_HOST : USB_ROLE_DEVICE;

	dwc3_pre_set_role(dwc, role);
	dwc3_set_mode(dwc, mode);

	return NOTIFY_DONE;
}

#if IS_ENABLED(CONFIG_USB_ROLE_SWITCH)
#define ROLE_SWITCH 1
static int dwc3_usb_role_switch_set(struct usb_role_switch *sw,
				    enum usb_role role)
{
	struct dwc3 *dwc = usb_role_switch_get_drvdata(sw);
	u32 mode;

	switch (role) {
	case USB_ROLE_HOST:
		mode = DWC3_GCTL_PRTCAP_HOST;
		break;
	case USB_ROLE_DEVICE:
		mode = DWC3_GCTL_PRTCAP_DEVICE;
		break;
	default:
		if (dwc->role_switch_default_mode == USB_DR_MODE_HOST)
			mode = DWC3_GCTL_PRTCAP_HOST;
		else
			mode = DWC3_GCTL_PRTCAP_DEVICE;
		break;
	}

	dwc3_pre_set_role(dwc, role);
	dwc3_set_mode(dwc, mode);
	return 0;
}

static enum usb_role dwc3_usb_role_switch_get(struct usb_role_switch *sw)
{
	struct dwc3 *dwc = usb_role_switch_get_drvdata(sw);
	unsigned long flags;
	enum usb_role role;

	spin_lock_irqsave(&dwc->lock, flags);
	switch (dwc->current_dr_role) {
	case DWC3_GCTL_PRTCAP_HOST:
		role = USB_ROLE_HOST;
		break;
	case DWC3_GCTL_PRTCAP_DEVICE:
		role = USB_ROLE_DEVICE;
		break;
	case DWC3_GCTL_PRTCAP_OTG:
		role = dwc->current_otg_role;
		break;
	default:
		if (dwc->role_switch_default_mode == USB_DR_MODE_HOST)
			role = USB_ROLE_HOST;
		else
			role = USB_ROLE_DEVICE;
		break;
	}
	spin_unlock_irqrestore(&dwc->lock, flags);
	return role;
}

static int dwc3_mux_role_set(struct usb_role_switch *sw, enum usb_role role)
{
	struct dwc3_role_port *port = usb_role_switch_get_drvdata(sw);
	struct dwc3 *dwc = port->dwc;
	struct dwc3_role_mux *mux = dwc->role_mux;
	enum usb_role selected_role;
	unsigned long flags;
	int selected, ret;
	u32 mode;

	if (role < USB_ROLE_NONE || role > USB_ROLE_DEVICE)
		return -EINVAL;

	mutex_lock(&mux->lock);
	if (mux->stopping) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (READ_ONCE(mux->error)) {
		ret = READ_ONCE(mux->error);
		goto out;
	}

	port->role = role;
	if (mux->ports[0].role != USB_ROLE_NONE)
		selected = 0;
	else if (mux->ports[1].role != USB_ROLE_NONE)
		selected = 1;
	else
		selected = mux->desired_port;
	selected_role = mux->ports[selected].role;
	if (selected == mux->desired_port && selected_role == mux->desired_role) {
		ret = 0;
		goto out;
	}

	if (selected_role == USB_ROLE_HOST ||
	    (selected_role == USB_ROLE_NONE &&
	     dwc->role_switch_default_mode == USB_DR_MODE_HOST))
		mode = DWC3_GCTL_PRTCAP_HOST;
	else
		mode = DWC3_GCTL_PRTCAP_DEVICE;

	spin_lock_irqsave(&dwc->lock, flags);
	mux->desired_port = selected;
	mux->desired_role = selected_role;
	dwc->desired_dr_role = mode;
	spin_unlock_irqrestore(&dwc->lock, flags);
	queue_work(system_freezable_wq, &dwc->drd_work);
	flush_work(&dwc->drd_work);
	ret = READ_ONCE(mux->error);
out:
	mutex_unlock(&mux->lock);
	return ret;
}

static enum usb_role dwc3_mux_role_get(struct usb_role_switch *sw)
{
	struct dwc3_role_port *port = usb_role_switch_get_drvdata(sw);
	struct dwc3_role_mux *mux = port->dwc->role_mux;
	unsigned long flags;
	enum usb_role role = USB_ROLE_NONE;

	mutex_lock(&mux->lock);
	spin_lock_irqsave(&port->dwc->lock, flags);
	if (!READ_ONCE(mux->error) && mux->active_port == port->state)
		role = port->role;
	spin_unlock_irqrestore(&port->dwc->lock, flags);
	mutex_unlock(&mux->lock);
	return role;
}

static void dwc3_role_mux_unregister(struct dwc3 *dwc)
{
	struct dwc3_role_mux *mux = dwc->role_mux;
	unsigned int i;

	if (!mux)
		return;

	mutex_lock(&mux->lock);
	mux->stopping = true;
	mutex_unlock(&mux->lock);
	for (i = 0; i < ARRAY_SIZE(mux->ports); i++) {
		usb_role_switch_unregister(mux->ports[i].sw);
		mux->ports[i].sw = NULL;
		fwnode_handle_put(mux->ports[i].fwnode);
		mux->ports[i].fwnode = NULL;
	}
}

/* Returns one when the optional two-connector mux is configured. */
static int dwc3_setup_role_mux(struct dwc3 *dwc, u32 mode)
{
	struct usb_role_switch_desc desc = {};
	struct fwnode_handle *ep;
	struct dwc3_role_mux *mux;
	struct gpio_desc *select;
	char name[64];
	u32 state;
	int ret, i;

	select = devm_gpiod_get_optional(dwc->dev, "port-select", GPIOD_ASIS);
	if (IS_ERR(select))
		return PTR_ERR(select);
	if (!select)
		return 0;
	ret = gpiod_get_value_cansleep(select);
	if (ret < 0)
		return ret;

	mux = devm_kzalloc(dwc->dev, sizeof(*mux), GFP_KERNEL);
	if (!mux)
		return -ENOMEM;
	mux->select = select;
	mux->desired_port = ret;
	mux->active_port = -1;
	mutex_init(&mux->lock);
	dwc->role_mux = mux;

	fwnode_graph_for_each_endpoint(dev_fwnode(dwc->dev), ep) {
		if (!fwnode_property_present(ep, "usb-role-switch"))
			continue;
		ret = fwnode_property_read_u32(ep, "mux-state", &state);
		if (ret || state >= ARRAY_SIZE(mux->ports)) {
			ret = -EINVAL;
			goto put_ep;
		}
		if (mux->ports[state].fwnode) {
			ret = -EINVAL;
			goto put_ep;
		}
		mux->ports[state].fwnode = fwnode_handle_get(ep);
		mux->ports[state].state = state;
		mux->ports[state].dwc = dwc;
	}
	for (i = 0; i < ARRAY_SIZE(mux->ports); i++) {
		if (!mux->ports[i].fwnode) {
			ret = -EINVAL;
			goto err;
		}
	}

	/* All state is initialized before either role provider becomes visible. */
	dwc3_set_mode(dwc, mode);
	for (i = 0; i < ARRAY_SIZE(mux->ports); i++) {
		snprintf(name, sizeof(name), "%s-port%d", dev_name(dwc->dev), i);
		desc.name = name;
		desc.fwnode = mux->ports[i].fwnode;
		desc.set = dwc3_mux_role_set;
		desc.get = dwc3_mux_role_get;
		desc.driver_data = &mux->ports[i];
		mux->ports[i].sw = usb_role_switch_register(dwc->dev, &desc);
		if (IS_ERR(mux->ports[i].sw)) {
			ret = PTR_ERR(mux->ports[i].sw);
			goto err;
		}
	}
	return 1;

put_ep:
	fwnode_handle_put(ep);
err:
	dwc3_drd_exit(dwc);
	return ret;
}

static int dwc3_setup_role_switch(struct dwc3 *dwc)
{
	struct usb_role_switch_desc dwc3_role_switch = {NULL};
	u32 mode;
	int ret;

	dwc->role_switch_default_mode = usb_get_role_switch_default_mode(dwc->dev);
	if (dwc->role_switch_default_mode == USB_DR_MODE_HOST) {
		mode = DWC3_GCTL_PRTCAP_HOST;
	} else {
		dwc->role_switch_default_mode = USB_DR_MODE_PERIPHERAL;
		mode = DWC3_GCTL_PRTCAP_DEVICE;
	}
	ret = dwc3_setup_role_mux(dwc, mode);
	if (ret)
		return ret < 0 ? ret : 0;

	dwc3_set_mode(dwc, mode);

	dwc3_role_switch.fwnode = dev_fwnode(dwc->dev);
	dwc3_role_switch.set = dwc3_usb_role_switch_set;
	dwc3_role_switch.get = dwc3_usb_role_switch_get;
	dwc3_role_switch.driver_data = dwc;
	dwc3_role_switch.allow_userspace_control = true;
	dwc->role_sw = usb_role_switch_register(dwc->dev, &dwc3_role_switch);
	if (IS_ERR(dwc->role_sw))
		return PTR_ERR(dwc->role_sw);

	if (dwc->dev->of_node) {
		/* populate connector entry */
		ret = devm_of_platform_populate(dwc->dev);

		if (ret) {
			usb_role_switch_unregister(dwc->role_sw);
			dwc->role_sw = NULL;
			dev_err(dwc->dev, "DWC3 platform devices creation failed: %i\n", ret);
			return ret;
		}
	}

	return 0;
}
#else
#define ROLE_SWITCH 0
#define dwc3_setup_role_switch(x) 0
static inline void dwc3_role_mux_unregister(struct dwc3 *dwc) {}
#endif

int dwc3_drd_init(struct dwc3 *dwc)
{
	int ret, irq;

	if (ROLE_SWITCH &&
	    device_property_read_bool(dwc->dev, "usb-role-switch"))
		return dwc3_setup_role_switch(dwc);

	if (dwc->edev) {
		dwc->edev_nb.notifier_call = dwc3_drd_notifier;
		ret = extcon_register_notifier(dwc->edev, EXTCON_USB_HOST,
					       &dwc->edev_nb);
		if (ret < 0) {
			dev_err(dwc->dev, "couldn't register cable notifier\n");
			return ret;
		}

		dwc3_drd_update(dwc);
	} else {
		dwc3_set_prtcap(dwc, DWC3_GCTL_PRTCAP_OTG, true);

		/* use OTG block to get ID event */
		irq = dwc3_otg_get_irq(dwc);
		if (irq < 0)
			return irq;

		dwc->otg_irq = irq;

		/* disable all OTG IRQs */
		dwc3_otg_disable_events(dwc, DWC3_OTG_ALL_EVENTS);
		/* clear all events */
		dwc3_otg_clear_events(dwc);

		ret = request_threaded_irq(dwc->otg_irq, dwc3_otg_irq,
					   dwc3_otg_thread_irq,
					   IRQF_SHARED, "dwc3-otg", dwc);
		if (ret) {
			dev_err(dwc->dev, "failed to request irq #%d --> %d\n",
				dwc->otg_irq, ret);
			ret = -ENODEV;
			return ret;
		}

		dwc3_otg_init(dwc);
		dwc3_set_mode(dwc, DWC3_GCTL_PRTCAP_OTG);
	}

	return 0;
}

void dwc3_drd_exit(struct dwc3 *dwc)
{
	unsigned long flags;

	dwc3_role_mux_unregister(dwc);
	if (dwc->role_sw)
		usb_role_switch_unregister(dwc->role_sw);

	if (dwc->edev)
		extcon_unregister_notifier(dwc->edev, EXTCON_USB_HOST,
					   &dwc->edev_nb);

	cancel_work_sync(&dwc->drd_work);

	/* debug user might have changed role, clean based on current role */
	switch (dwc->current_dr_role) {
	case DWC3_GCTL_PRTCAP_HOST:
		dwc3_host_exit(dwc);
		break;
	case DWC3_GCTL_PRTCAP_DEVICE:
		dwc3_gadget_exit(dwc);
		dwc3_event_buffers_cleanup(dwc);
		break;
	case DWC3_GCTL_PRTCAP_OTG:
		dwc3_otg_exit(dwc);
		spin_lock_irqsave(&dwc->lock, flags);
		dwc->desired_otg_role = DWC3_OTG_ROLE_IDLE;
		spin_unlock_irqrestore(&dwc->lock, flags);
		dwc3_otg_update(dwc, 1);
		break;
	default:
		break;
	}

	if (dwc->otg_irq)
		free_irq(dwc->otg_irq, dwc);
}
