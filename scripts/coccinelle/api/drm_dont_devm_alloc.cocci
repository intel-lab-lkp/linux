// SPDX-License-Identifier: GPL-2.0-only
///
/// Find (1) devres-allocated (usually by devm_kzalloc) argument to drm_*_init
/// functions or (2) assignment of devres-allocated pointer to a field of a drm-
/// allocated struct (usually dev_private of drm_device). The mismatch of the
/// lifespan between devres- and drm-managed memory can cause a use-after-free.
//
// Confidence: High
// Copyright: (C) 2025 Oleg Petrov ISPRAS
// Options: --no-includes --include-headers
//

virtual report
virtual org

// find devm-allocated (devres-managed) second arg for drm*init functions
@badarg exists@
position p;
expression devm,e;
@@
// only devm_kzalloc is really used
devm = \(devm_kzalloc\|devm_kcalloc\|devm_kmalloc\|devm_kmalloc_array\)(...);
...
// The kernel-doc comments (v6+) for these 5 functions
// forbid them to use devm-allocated argument.
( drm_connector_init
| drm_crtc_init_with_planes
| drm_connector_init_with_ddc
| drm_encoder_init
| drm_universal_plane_init
// These are the wrappers found in drivers/gpu/drm/*.c
// i.e. these call those above and just pass the second argument.
| drm_bridge_connector_init
| drm_crtc_init
| drm_plane_init
// drm_simple_display_pipe_init // does not apply
| drm_simple_encoder_init
| drm_writeback_connector_init
// mipi_dbi_dev_init_with_formats // does not apply
) ( e,<+...devm@p...+>,...)

// same as above, but with an intermediate local variable
@badarg2 exists@
position p;
expression devm,e;
identifier vitm;
@@
// only devm_kzalloc is really used
devm = \(devm_kzalloc\|devm_kcalloc\|devm_kmalloc\|devm_kmalloc_array\)(...);
...
vitm = <+...devm...+>;
...
// The kernel-doc comments (v6+) for these 5 functions
// forbid them to use devm-allocated argument.
( drm_connector_init
| drm_crtc_init_with_planes
| drm_connector_init_with_ddc
| drm_encoder_init
| drm_universal_plane_init
// These are the wrappers found in drivers/gpu/drm/*.c
// i.e. these call those above and just pass the second argument.
| drm_bridge_connector_init
| drm_crtc_init
| drm_plane_init
// drm_simple_display_pipe_init // does not apply
| drm_simple_encoder_init
| drm_writeback_connector_init
// mipi_dbi_dev_init_with_formats // does not apply
) ( e,<+...devm@p...+>,...)

// find direct assignment of devres-managed memory to drm device
@badfield exists@
position p;
expression drm,devm;
identifier f;
@@
(
drm = \(drm_dev_alloc\|drmm_kzalloc\|drmm_kcalloc\|drmm_kmalloc\|drmm_kmalloc_array\)(...);
...
devm = \(devm_kzalloc\|devm_kcalloc\|devm_kmalloc\|devm_kmalloc_array\)(...);
|
devm = \(devm_kzalloc\|devm_kcalloc\|devm_kmalloc\|devm_kmalloc_array\)(...);
...
drm = \(drm_dev_alloc\|drmm_kzalloc\|drmm_kcalloc\|drmm_kmalloc\|drmm_kmalloc_array\)(...);
)
...
drm->f =@p <+...devm...+>;


@script:python depends on report@
p << badarg.p;
@@
msg = "WARNING devm-allocated argument in a drm-init; use drmm-init family (or drmm-alloc)."
coccilib.report.print_report(p[0], msg)

@script:python depends on org@
p << badarg.p;
@@
msg = "WARNING devm-allocated argument in a drm-init; use drmm-init family (or drmm-alloc)."
coccilib.org.print_report(p[0], msg)

@script:python depends on report@
p << badarg2.p;
@@
msg = "WARNING devm-allocated argument in a drm-init; use drmm-init family (or drmm-alloc)."
coccilib.report.print_report(p[0], msg)

@script:python depends on org@
p << badarg2.p;
@@
msg = "WARNING devm-allocated argument in a drm-init; use drmm-init family (or drmm-alloc)."
coccilib.org.print_report(p[0], msg)

@script:python depends on report@
p << badfield.p;
@@
msg = "WARNING devm-allocated field in a drmm-allocated struct; consider drmm-init family or use drmm-alloc."
coccilib.report.print_report(p[0], msg)

@script:python depends on org@
p << badfield.p;
@@
msg = "WARNING devm-allocated field in a drmm-allocated struct; consider drmm-init family or use drmm-alloc."
coccilib.org.print_report(p[0], msg)
