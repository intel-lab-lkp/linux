/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2015 - 2025 Beijing WangXun Technology Co., Ltd. */

#ifndef _WX_VF_COMMON_H_
#define _WX_VF_COMMON_H_

int wxvf_suspend(struct device *dev_d);
void wxvf_shutdown(struct pci_dev *pdev);
int wxvf_resume(struct device *dev_d);
void wxvf_remove(struct pci_dev *pdev);
int wx_request_msix_irqs_vf(struct wx *wx);
void wx_negotiate_api_vf(struct wx *wx);
void wx_reset_vf(struct wx *wx);
void wx_set_rx_mode_vf(struct net_device *netdev);
void wx_configure_vf(struct wx *wx);
int wx_set_mac_vf(struct net_device *netdev, void *p);
void wx_get_mac_link_vf(struct phylink_config *config,
			struct phylink_link_state *state);
int wxvf_open(struct net_device *netdev);
int wxvf_close(struct net_device *netdev);
void wxvf_mac_config(struct phylink_config *config, unsigned int mode,
		     const struct phylink_link_state *state);
void wxvf_mac_link_down(struct phylink_config *config, unsigned int mode,
			phy_interface_t interface);
void wxvf_mac_link_up(struct phylink_config *config, struct phy_device *phy,
		      unsigned int mode, phy_interface_t interface,
		      int speed, int duplex, bool tx_pause, bool rx_pause);

#endif /* _WX_VF_COMMON_H_ */
