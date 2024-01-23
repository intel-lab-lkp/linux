// SPDX-License-Identifier: GPL-2.0

#ifndef SVC_I3C_H
#define SVC_I3C_H

int svc_i3c_master_probe(struct platform_device *pdev);
void svc_i3c_master_remove(struct platform_device *pdev);
int svc_i3c_master_runtime_suspend(struct device *dev);
int svc_i3c_master_runtime_resume(struct device *dev);

int svc_i3c_target_probe(struct platform_device *pdev);
void svc_i3c_target_remove(struct platform_device *pdev);

#endif