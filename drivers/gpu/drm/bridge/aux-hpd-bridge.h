/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AUX_HPD_BRIDGE_H
#define AUX_HPD_BRIDGE_H

#if IS_REACHABLE(CONFIG_TYPEC)
int drm_aux_hpd_typec_dp_bridge_init(void);
void drm_aux_hpd_typec_dp_bridge_exit(void);
#else
static inline int drm_aux_hpd_typec_dp_bridge_init(void) { return 0; }
static inline void drm_aux_hpd_typec_dp_bridge_exit(void) { }
#endif /* IS_REACHABLE(CONFIG_TYPEC) */

#endif /* AUX_HPD_BRIDGE_H */
