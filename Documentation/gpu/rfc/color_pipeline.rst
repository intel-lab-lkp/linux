========================
Linux Color Pipeline API
========================

What problem are we solving?
============================

We would like to support pre-, and post-blending complex color
transformations in display controller hardware in order to allow for
HW-supported HDR use-cases, as well as to provide support to
color-managed applications, such as video or image editors.

It is possible to support an HDR output on HW supporting the Colorspace
and HDR Metadata drm_connector properties, but that requires the
compositor or application to render and compose the content into one
final buffer intended for display. Doing so is costly.

Most modern display HW offers various 1D LUTs, 3D LUTs, matrices, and other
operations to support color transformations. These operations are often
implemented in fixed-function HW and therefore much more power efficient than
performing similar operations via shaders or CPU.

We would like to make use of this HW functionality to support complex color
transformations with no, or minimal CPU or shader load.


How are other OSes solving this problem?
========================================

The most widely supported use-cases regard HDR content, whether video or
gaming.

Most OSes will specify the source content format (color gamut, encoding transfer
function, and other metadata, such as max and average light levels) to a driver.
Drivers will then program their fixed-function HW accordingly to map from a
source content buffer's space to a display's space.

When fixed-function HW is not available the compositor will assemble a shader to
ask the GPU to perform the transformation from the source content format to the
display's format.

A compositor's mapping function and a driver's mapping function are usually
entirely separate concepts. On OSes where a HW vendor has no insight into
closed-source compositor code such a vendor will tune their color management
code to visually match the compositor's. On other OSes, where both mapping
functions are open to an implementer they will ensure both mappings match.

This results in mapping algorithm lock-in, meaning that no-one alone can
experiment with or introduce new mapping algorithms and achieve
consistent results regardless of which implementation path is taken.

Why is Linux different?
=======================

Unlike other OSes, where there is one compositor for one or more drivers, on
Linux we have a many-to-many relationship. Many compositors; many drivers.
In addition each compositor vendor or community has their own view of how
color management should be done. This is what makes Linux so beautiful.

This means that a HW vendor can now no longer tune their driver to one
compositor, as tuning it to one could make it look fairly different from
another compositor's color mapping.

We need a better solution.


Descriptive API
===============

An API that describes the source and destination colorspaces is a descriptive
API. It describes the input and output color spaces but does not describe
how precisely they should be mapped. Such a mapping includes many minute
design decision that can greatly affect the look of the final result.

It is not feasible to describe such mapping with enough detail to ensure the
same result from each implementation. In fact, these mappings are a very active
research area.


Prescriptive API
================

A prescriptive API describes not the source and destination colorspaces. It
instead prescribes a recipe for how to manipulate pixel values to arrive at the
desired outcome.

This recipe is generally an ordered list of straight-forward operations,
with clear mathematical definitions, such as 1D LUTs, 3D LUTs, matrices,
or other operations that can be described in a precise manner.


The Color Pipeline API
======================

HW color management pipelines can significantly differ between HW
vendors in terms of availability, ordering, and capabilities of HW
blocks. This makes a common definition of color management blocks and
their ordering nigh impossible. Instead we are defining an API that
allows user space to discover the HW capabilities in a generic manner,
agnostic of specific drivers and hardware.


drm_colorop Object & IOCTLs
===========================

To support the definition of color pipelines we define the DRM core
object type drm_colorop. Individual drm_colorop objects will be chained
via the NEXT property of a drm_colorop to constitute a color pipeline.
Each drm_colorop object is unique, i.e., even if multiple color
pipelines have the same operation they won't share the same drm_colorop
object to describe that operation.

Note that drivers are not expected to map drm_colorop objects statically
to specific HW blocks. The mapping of drm_colorop objects is entirely a
driver-internal detail and can be as dynamic or static as a driver needs
it to be. See more in the Driver Implementation Guide section below.

Just like other DRM objects the drm_colorop objects are discovered via
IOCTLs:

DRM_IOCTL_MODE_GETCOLOROPRESOURCES: This IOCTL is used to retrieve the
number of all drm_colorop objects.

DRM_IOCTL_MODE_GETCOLOROP: This IOCTL is used to read one drm_colorop.
It includes the ID for the colorop object, as well as the plane_id of
the associated plane. All other values should be registered as
properties.

Each drm_colorop has three core properties:

TYPE: The type of transformation, such as
* enumerated curve
* custom (uniform) 1D LUT
* 3x3 matrix
* 3x4 matrix
* 3D LUT
* etc.

Depending on the type of transformation other properties will describe
more details.

BYPASS: A boolean property that can be used to easily put a block into
bypass mode. While setting other properties might fail atomic check,
setting the BYPASS property to true should never fail. The BYPASS
property is not mandatory for a colorop, as long as the entire pipeline
can get bypassed by setting the COLOR_PIPELINE on a plane to '0'.

NEXT: The ID of the next drm_colorop in a color pipeline, or 0 if this
drm_colorop is the last in the chain.

An example of a drm_colorop object might look like one of these::

    /* 1D enumerated curve */
    Color operation 42
    ├─ "TYPE": immutable enum {1D enumerated curve, 1D LUT, 3x3 matrix, 3x4 matrix, 3D LUT, etc.} = 1D enumerated curve
    ├─ "BYPASS": bool {true, false}
    ├─ "CURVE_1D_TYPE": enum {sRGB EOTF, sRGB inverse EOTF, PQ EOTF, PQ inverse EOTF, …}
    └─ "NEXT": immutable color operation ID = 43

    /* custom 4k entry 1D LUT */
    Color operation 52
    ├─ "TYPE": immutable enum {1D enumerated curve, 1D LUT, 3x3 matrix, 3x4 matrix, 3D LUT, etc.} = 1D LUT
    ├─ "BYPASS": bool {true, false}
    ├─ "LUT_1D_SIZE": immutable range = 4096
    ├─ "LUT_1D": blob
    └─ "NEXT": immutable color operation ID = 0

    /* 17^3 3D LUT */
    Color operation 72
    ├─ "TYPE": immutable enum {1D enumerated curve, 1D LUT, 3x3 matrix, 3x4 matrix, 3D LUT, etc.} = 3D LUT
    ├─ "BYPASS": bool {true, false}
    ├─ "LUT_3D_SIZE": immutable range = 17
    ├─ "LUT_3D": blob
    └─ "NEXT": immutable color operation ID = 73


COLOR_PIPELINE Plane Property
=============================

Color Pipelines are created by a driver and advertised via a new
COLOR_PIPELINE enum property on each plane. Values of the property
always include '0', which is the default and means all color processing
is disabled. Additional values will be the object IDs of the first
drm_colorop in a pipeline. A driver can create and advertise none, one,
or more possible color pipelines. A DRM client will select a color
pipeline by setting the COLOR PIPELINE to the respective value.

The COLOR_PIPELINE property is only exposed when the
DRM_CLIENT_CAP_PLANE_COLOR_PIPELINE is set. Drivers are expected to
reject setting any existing pre-blend color operations when this cap is
set, such as COLOR_RANGE or COLOR_ENCODING. If drivers want to support
COLOR_RANGE or COLOR_ENCODING functionality when the color pipeline
client cap is set, they are expected to expose colorops in the pipeline
to allow for the appropriate color transformation.

An example of a COLOR_PIPELINE property on a plane might look like this::

    Plane 10
    ├─ "type": immutable enum {Overlay, Primary, Cursor} = Primary
    ├─ …
    └─ "color_pipeline": enum {0, 42, 52} = 0


Color Pipeline Discovery
========================

A DRM client wanting color management on a drm_plane will:

1. Read all drm_colorop objects
2. Get the COLOR_PIPELINE property of the plane
3. iterate all COLOR_PIPELINE enum values
4. for each enum value walk the color pipeline (via the NEXT pointers)
   and see if the available color operations are suitable for the
   desired color management operations

An example of chained properties to define an AMD pre-blending color
pipeline might look like this::

    Plane 10
    ├─ "TYPE" (immutable) = Primary
    └─ "COLOR_PIPELINE": enum {0, 44} = 0

    Color operation 44
    ├─ "TYPE" (immutable) = 1D enumerated curve
    ├─ "BYPASS": bool
    ├─ "CURVE_1D_TYPE": enum {sRGB EOTF, PQ EOTF} = sRGB EOTF
    └─ "NEXT" (immutable) = 45

    Color operation 45
    ├─ "TYPE" (immutable) = 3x4 Matrix
    ├─ "BYPASS": bool
    ├─ "MATRIX_3_4": blob
    └─ "NEXT" (immutable) = 46

    Color operation 46
    ├─ "TYPE" (immutable) = 1D enumerated curve
    ├─ "BYPASS": bool
    ├─ "CURVE_1D_TYPE": enum {sRGB Inverse EOTF, PQ Inverse EOTF} = sRGB EOTF
    └─ "NEXT" (immutable) = 47

    Color operation 47
    ├─ "TYPE" (immutable) = 1D LUT
    ├─ "LUT_1D_SIZE": immutable range = 4096
    ├─ "LUT_1D_DATA": blob
    └─ "NEXT" (immutable) = 48

    Color operation 48
    ├─ "TYPE" (immutable) = 3D LUT
    ├─ "LUT_3D_SIZE" (immutable) = 17
    ├─ "LUT_3D_DATA": blob
    └─ "NEXT" (immutable) = 49

    Color operation 49
    ├─ "TYPE" (immutable) = 1D enumerated curve
    ├─ "BYPASS": bool
    ├─ "CURVE_1D_TYPE": enum {sRGB EOTF, PQ EOTF} = sRGB EOTF
    └─ "NEXT" (immutable) = 0


Color Pipeline Programming
==========================

Once a DRM client has found a suitable pipeline it will:

1. Set the COLOR_PIPELINE enum value to the one pointing at the first
   drm_colorop object of the desired pipeline
2. Set the properties for all drm_colorop objects in the pipeline to the
   desired values, setting BYPASS to true for unused drm_colorop blocks,
   and false for enabled drm_colorop blocks
3. Perform atomic_check/commit as desired

To configure the pipeline for an HDR10 PQ plane and blending in linear
space, a compositor might perform an atomic commit with the following
property values::

    Plane 10
    └─ "COLOR_PIPELINE" = 42

    Color operation 42 (input CSC)
    └─ "BYPASS" = true

    Color operation 44 (DeGamma)
    └─ "BYPASS" = true

    Color operation 45 (gamut remap)
    └─ "BYPASS" = true

    Color operation 46 (shaper LUT RAM)
    └─ "BYPASS" = true

    Color operation 47 (3D LUT RAM)
    └─ "LUT_3D_DATA" = Gamut mapping + tone mapping + night mode

    Color operation 48 (blend gamma)
    └─ "CURVE_1D_TYPE" = PQ EOTF


Driver Implementer's Guide
==========================

What does this all mean for driver implementations? As noted above the
colorops can map to HW directly but don't need to do so. Here are some
suggestions on how to think about creating your color pipelines:

- Try to expose pipelines that use already defined colorops, even if
  your hardware pipeline is split differently. This allows existing
  userspace to immediately take advantage of the hardware.

- Additionally, try to expose your actual hardware blocks as colorops.
  Define new colorop types where you believe it can offer significant
  benefits if userspace learns to program them.

- Avoid defining new colorops for compound operations with very narrow
  scope. If you have a hardware block for a special operation that
  cannot be split further, you can expose that as a new colorop type.
  However, try to not define colorops for "use cases", especially if
  they require you to combine multiple hardware blocks.

- Design new colorops as prescriptive, not descriptive; by the
  mathematical formula, not by the assumed input and output.

A defined colorop type must be deterministic. The exact behavior of the
colorop must be documented entirely, whether via a mathematical formula
or some other description. Its operation can depend only on its
properties and input and nothing else, allowed error tolerance
notwithstanding.


Driver Forward/Backward Compatibility
=====================================

As this is uAPI drivers can't regress color pipelines that have been
introduced for a given HW generation. New HW generations are free to
abandon color pipelines advertised for previous generations.
Nevertheless, it can be beneficial to carry support for existing color
pipelines forward as those will likely already have support in DRM
clients.

Introducing new colorops to a pipeline is fine, as long as they can be
disabled or are purely informational. DRM clients implementing support
for the pipeline can always skip unknown properties as long as they can
be confident that doing so will not cause unexpected results.

If a new colorop doesn't fall into one of the above categories
(bypassable or informational) the modified pipeline would be unusable
for user space. In this case a new pipeline should be defined.


References
==========

1. https://lore.kernel.org/dri-devel/QMers3awXvNCQlyhWdTtsPwkp5ie9bze_hD5nAccFW7a_RXlWjYB7MoUW_8CKLT2bSQwIXVi5H6VULYIxCdgvryZoAoJnC5lZgyK1QWn488=@emersion.fr/