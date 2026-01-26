// SPDX-License-Identifier: GPL-2.0-only

//! Simple framebuffer driver in Rust (TEST ONLY)
//!
//! **WARNING**: This driver is for testing purposes only and should not be used in production systems.
//!
//! This is a test driver for the Rust framebuffer framework abstraction. It is used to validate
//! and test the Rust framebuffer API implementation. The driver has incomplete functionality and
//! is not suitable for real-world use. For production use, please use FB_SIMPLE instead.
//!
//! **Limitations**:
//! - No clock management support
//! - No power management support
//! - No regulator management support
//! - No kernel parameter parsing support
//! - No `devm_aperture_acquire_for_platform_device` support
//!
//! This driver assumes that the display hardware has been initialized before the kernel boots,
//! and the kernel will simply render to the pre-allocated frame buffer surface. Configuration
//! regarding surface address, size, and format must be provided through device tree or platform data.

use kernel::{
    bindings, c_str,
    device::Core,
    devres::Devres,
    fb,
    io::{
        mem::IoMem,
        resource::{Flags, Region, Resource},
        PhysAddr, ResourceSize,
    },
    macros::vtable,
    of, platform,
    prelude::*,
    str::CStr,
    sync::aref::ARef,
};

/// Pseudo palette size for framebuffer
const PSEUDO_PALETTE_SIZE: usize = 16;

/// Number of supported framebuffer formats
const SIMPLEFB_FORMAT_COUNT: usize = 11;

/// Initialize fixed screen information template
fn init_simplefb_fix() -> fb::FixScreenInfo {
    let mut fix = fb::FixScreenInfo::new_zeroed();

    // Set the initial values
    fix.set_id(c_str!("simplefb-rust"));
    fix.set_type(fb::types::FB_TYPE_PACKED_PIXELS);
    fix.set_visual(fb::visual::FB_VISUAL_TRUECOLOR);
    fix.set_accel(fb::accel::FB_ACCEL_NONE);

    fix
}

/// Initialize variable screen information template
fn init_simplefb_var() -> fb::VarScreenInfo {
    let mut var = fb::VarScreenInfo::new_zeroed();

    // Set the initial values
    var.set_height(u32::MAX);
    var.set_width(u32::MAX);
    var.set_activate(fb::activate::FB_ACTIVATE_NOW);
    var.set_vmode(fb::vmode::FB_VMODE_NONINTERLACED);

    var
}

/// Framebuffer pixel format descriptor.
struct SimplefbFormat {
    name: &'static CStr,
    bits_per_pixel: u32,
    red: fb::Bitfield,
    green: fb::Bitfield,
    blue: fb::Bitfield,
    transp: fb::Bitfield,
    fourcc: u32,
}

impl SimplefbFormat {
    const fn new(
        name: &'static CStr,
        bits_per_pixel: u32,
        red: fb::Bitfield,
        green: fb::Bitfield,
        blue: fb::Bitfield,
        transp: fb::Bitfield,
        fourcc: u32,
    ) -> Self {
        Self {
            name,
            bits_per_pixel,
            red,
            green,
            blue,
            transp,
            fourcc,
        }
    }

    const fn name(&self) -> &'static CStr {
        self.name
    }

    const fn bits_per_pixel(&self) -> u32 {
        self.bits_per_pixel
    }

    const fn red(&self) -> fb::Bitfield {
        self.red
    }

    const fn green(&self) -> fb::Bitfield {
        self.green
    }

    const fn blue(&self) -> fb::Bitfield {
        self.blue
    }

    const fn transp(&self) -> fb::Bitfield {
        self.transp
    }

    #[allow(dead_code)]
    const fn fourcc(&self) -> u32 {
        self.fourcc
    }
}

/// Supported framebuffer formats.
///
/// This matches the format array from `include/linux/platform_data/simplefb.h`.
const SIMPLEFB_FORMATS: [SimplefbFormat; SIMPLEFB_FORMAT_COUNT] = [
    SimplefbFormat::new(
        c_str!("r5g6b5"),
        16,
        fb::Bitfield::new(11, 5, 0),
        fb::Bitfield::new(5, 6, 0),
        fb::Bitfield::new(0, 5, 0),
        fb::Bitfield::new(0, 0, 0),
        0x36314752, // DRM_FORMAT_RGB565 = fourcc_code('R', 'G', '1', '6')
    ),
    SimplefbFormat::new(
        c_str!("r5g5b5a1"),
        16,
        fb::Bitfield::new(11, 5, 0),
        fb::Bitfield::new(6, 5, 0),
        fb::Bitfield::new(1, 5, 0),
        fb::Bitfield::new(0, 1, 0),
        0x31354152, // DRM_FORMAT_RGBA5551 = fourcc_code('R', 'A', '1', '5')
    ),
    SimplefbFormat::new(
        c_str!("x1r5g5b5"),
        16,
        fb::Bitfield::new(10, 5, 0),
        fb::Bitfield::new(5, 5, 0),
        fb::Bitfield::new(0, 5, 0),
        fb::Bitfield::new(0, 0, 0),
        0x35315258, // DRM_FORMAT_XRGB1555 = fourcc_code('X', 'R', '1', '5')
    ),
    SimplefbFormat::new(
        c_str!("a1r5g5b5"),
        16,
        fb::Bitfield::new(10, 5, 0),
        fb::Bitfield::new(5, 5, 0),
        fb::Bitfield::new(0, 5, 0),
        fb::Bitfield::new(15, 1, 0),
        0x35315241, // DRM_FORMAT_ARGB1555 = fourcc_code('A', 'R', '1', '5')
    ),
    SimplefbFormat::new(
        c_str!("r8g8b8"),
        24,
        fb::Bitfield::new(16, 8, 0),
        fb::Bitfield::new(8, 8, 0),
        fb::Bitfield::new(0, 8, 0),
        fb::Bitfield::new(0, 0, 0),
        0x34324752, // DRM_FORMAT_RGB888 = fourcc_code('R', 'G', '2', '4')
    ),
    SimplefbFormat::new(
        c_str!("x8r8g8b8"),
        32,
        fb::Bitfield::new(16, 8, 0),
        fb::Bitfield::new(8, 8, 0),
        fb::Bitfield::new(0, 8, 0),
        fb::Bitfield::new(0, 0, 0),
        0x34325258, // DRM_FORMAT_XRGB8888 = fourcc_code('X', 'R', '2', '4')
    ),
    SimplefbFormat::new(
        c_str!("a8r8g8b8"),
        32,
        fb::Bitfield::new(16, 8, 0),
        fb::Bitfield::new(8, 8, 0),
        fb::Bitfield::new(0, 8, 0),
        fb::Bitfield::new(24, 8, 0),
        0x34325241, // DRM_FORMAT_ARGB8888 = fourcc_code('A', 'R', '2', '4')
    ),
    SimplefbFormat::new(
        c_str!("x8b8g8r8"),
        32,
        fb::Bitfield::new(0, 8, 0),
        fb::Bitfield::new(8, 8, 0),
        fb::Bitfield::new(16, 8, 0),
        fb::Bitfield::new(0, 0, 0),
        0x34324258, // DRM_FORMAT_XBGR8888 = fourcc_code('X', 'B', '2', '4')
    ),
    SimplefbFormat::new(
        c_str!("a8b8g8r8"),
        32,
        fb::Bitfield::new(0, 8, 0),
        fb::Bitfield::new(8, 8, 0),
        fb::Bitfield::new(16, 8, 0),
        fb::Bitfield::new(24, 8, 0),
        0x34324241, // DRM_FORMAT_ABGR8888 = fourcc_code('A', 'B', '2', '4')
    ),
    SimplefbFormat::new(
        c_str!("x2r10g10b10"),
        32,
        fb::Bitfield::new(20, 10, 0),
        fb::Bitfield::new(10, 10, 0),
        fb::Bitfield::new(0, 10, 0),
        fb::Bitfield::new(0, 0, 0),
        0x30335258, // DRM_FORMAT_XRGB2101010 = fourcc_code('X', 'R', '3', '0')
    ),
    SimplefbFormat::new(
        c_str!("a2r10g10b10"),
        32,
        fb::Bitfield::new(20, 10, 0),
        fb::Bitfield::new(10, 10, 0),
        fb::Bitfield::new(0, 10, 0),
        fb::Bitfield::new(30, 2, 0),
        0x30335241, // DRM_FORMAT_ARGB2101010 = fourcc_code('A', 'R', '3', '0')
    ),
];

/// Find a format by name.
fn find_format(name: &CStr) -> Option<&'static SimplefbFormat> {
    SIMPLEFB_FORMATS
        .iter()
        .find(|format| format.name().to_bytes() == name.to_bytes())
}

/// Platform data for simple framebuffer devices.
struct SimplefbPlatformData {
    width: u32,
    height: u32,
    stride: u32,
    format: &'static CStr,
}

impl SimplefbPlatformData {
    /// Extract platform data from a device.
    ///
    /// # Safety
    ///
    /// * The platform data type must be `simplefb_platform_data`.
    /// * The platform data structure must be properly initialized.
    unsafe fn from_device(pdev: &platform::Device<Core>) -> Result<Self> {
        let dev = pdev.as_ref();
        // SAFETY: The caller guarantees the platform data type matches.
        let pd_opaque = unsafe { dev.platdata::<bindings::simplefb_platform_data>()? };
        // SAFETY: The caller guarantees the platform data structure is properly initialized.
        let pd_raw = unsafe { &*pd_opaque.get() };

        let format_cstr = if pd_raw.format.is_null() {
            return Err(ENODEV);
        } else {
            // SAFETY: `format` is not null (checked above) and points to a valid C string.
            unsafe { CStr::from_char_ptr(pd_raw.format) }
        };

        Ok(Self {
            width: pd_raw.width,
            height: pd_raw.height,
            stride: pd_raw.stride,
            format: format_cstr,
        })
    }

    const fn width(&self) -> u32 {
        self.width
    }

    const fn height(&self) -> u32 {
        self.height
    }

    const fn stride(&self) -> u32 {
        self.stride
    }

    const fn format(&self) -> &'static CStr {
        self.format
    }
}

/// Parsed framebuffer parameters.
struct SimplefbParams {
    width: u32,
    height: u32,
    stride: u32,
    format: &'static SimplefbFormat,
}

/// Driver-specific data for the simple framebuffer.
#[pin_data]
struct SimplefbData {
    palette: [u32; PSEUDO_PALETTE_SIZE],
    #[allow(dead_code)] // Reserved for future devm_aperture_acquire_for_platform_device support
    base: PhysAddr,
    #[allow(dead_code)] // Reserved for future devm_aperture_acquire_for_platform_device support
    size: ResourceSize,
    #[allow(dead_code)] // Used via Drop trait for automatic resource cleanup
    mem: Option<Region>,
    #[pin]
    #[allow(dead_code)]
    // I/O memory mapping; ensures iounmap happens before release_mem_region via drop order
    _iomem: Pin<KBox<Devres<IoMem<0>>>>,
}

/// Simple framebuffer operations implementation
struct SimplefbOps;

#[vtable]
impl fb::Operations for SimplefbOps {
    type Data = SimplefbData;

    fn read(
        device: &fb::Device<impl fb::Driver<Data = Self::Data>>,
        buf: &mut [u8],
        ppos: &mut kernel::fs::file::Offset,
    ) -> Result<usize> {
        fb::fb_io_read(device, buf, ppos)
    }

    fn write(
        device: &fb::Device<impl fb::Driver<Data = Self::Data>>,
        buf: &[u8],
        ppos: &mut kernel::fs::file::Offset,
    ) -> Result<usize> {
        fb::fb_io_write(device, buf, ppos)
    }

    fn setcolreg(
        device: &fb::Device<impl fb::Driver<Data = Self::Data>>,
        regno: u32,
        red: u32,
        green: u32,
        blue: u32,
        _transp: u32,
    ) -> Result {
        if regno >= PSEUDO_PALETTE_SIZE as u32 {
            return Err(EINVAL);
        }

        let var = device.var();
        let red_len = var.red().length();
        let green_len = var.green().length();
        let blue_len = var.blue().length();
        let red_offset = var.red().offset();
        let green_offset = var.green().offset();
        let blue_offset = var.blue().offset();

        let cr = red >> (16 - red_len);
        let cg = green >> (16 - green_len);
        let cb = blue >> (16 - blue_len);

        let mut value = (cr << red_offset) | (cg << green_offset) | (cb << blue_offset);

        let transp_len = var.transp().length();
        if transp_len > 0 {
            let transp_offset = var.transp().offset();
            let mask = ((1u32 << transp_len) - 1) << transp_offset;
            value |= mask;
        }

        // Access the palette through the driver data
        // SAFETY: device.pseudo_palette() returns a valid pointer to the palette array
        unsafe {
            let palette = device.pseudo_palette() as *mut u32;
            *palette.add(regno as usize) = value;
        }

        Ok(())
    }

    fn fillrect(device: &fb::Device<impl fb::Driver<Data = Self::Data>>, rect: &fb::FillRect) {
        fb::cfb_fillrect(device, rect);
    }

    fn copyarea(device: &fb::Device<impl fb::Driver<Data = Self::Data>>, area: &fb::CopyArea) {
        fb::cfb_copyarea(device, area);
    }

    fn imageblit(device: &fb::Device<impl fb::Driver<Data = Self::Data>>, image: &fb::Image) {
        fb::cfb_imageblit(device, image);
    }

    fn mmap(
        device: &fb::Device<impl fb::Driver<Data = Self::Data>>,
        vma: &kernel::mm::virt::VmaNew,
    ) -> Result {
        fb::fb_io_mmap(device, vma)
    }
}

/// Framebuffer driver type.
struct SimplefbDriverImpl;

#[vtable]
impl fb::Driver for SimplefbDriverImpl {
    type Data = SimplefbData;
    type Ops = SimplefbOps;

    const INFO: fb::DriverInfo = fb::DriverInfo {
        name: c_str!("simplefb-rust"),
        desc: c_str!("Simple framebuffer driver"),
    };
}

/// Platform driver data.
struct SimplefbDriver {
    _pdev: ARef<platform::Device>,
}

impl SimplefbDriver {
    /// Map framebuffer memory with write-combining cache policy.
    ///
    /// Returns a tuple of the IoMem handle and the virtual address (screen_base).
    fn map_framebuffer_memory(
        pdev: &platform::Device<Core>,
    ) -> Result<(Pin<KBox<Devres<IoMem<0>>>>, *mut u8)> {
        let dev = pdev.as_ref();

        let io_request = pdev.io_request_by_index(0).ok_or_else(|| {
            dev_err!(dev, "[rust] No memory resource for framebuffer\n");
            ENODEV
        })?;

        // Map with write-combining. The Devres must remain at a fixed address for the devres
        // callback to work correctly, so we keep it in KBox.
        let iomem_init = io_request.iomap_wc();
        let iomem_kbox = KBox::pin_init(iomem_init, GFP_KERNEL)?;

        let screen_base = {
            let io = iomem_kbox.access(dev)?;
            io.addr() as *mut u8
        };

        Ok((iomem_kbox, screen_base))
    }

    /// Parse platform data.
    fn parse_pd(pdev: &platform::Device<Core>) -> Result<SimplefbParams> {
        let dev = pdev.as_ref();

        // SAFETY: The platform data type matches simplefb_platform_data for this device.
        let pd = unsafe { SimplefbPlatformData::from_device(pdev)? };

        let width = pd.width();
        let height = pd.height();
        let stride = pd.stride();
        let format_cstr = pd.format();

        let format = find_format(format_cstr).ok_or_else(|| {
            dev_err!(dev, "[rust] Invalid format value\n");
            EINVAL
        })?;

        Ok(SimplefbParams {
            width,
            height,
            stride,
            format,
        })
    }

    /// Parse device tree properties.
    fn parse_dt(pdev: &platform::Device<Core>) -> Result<SimplefbParams> {
        let dev = pdev.as_ref();
        let fwnode = dev.fwnode().ok_or(ENODEV)?;

        let width: u32 = fwnode.property_read(c_str!("width")).required_by(dev)?;
        let height: u32 = fwnode.property_read(c_str!("height")).required_by(dev)?;
        let stride: u32 = fwnode.property_read(c_str!("stride")).required_by(dev)?;

        let format_name = fwnode
            .property_read::<kernel::str::CString>(c_str!("format"))
            .required_by(dev)?;

        let format = find_format(&format_name).ok_or_else(|| {
            dev_err!(dev, "[rust] Invalid format value\n");
            EINVAL
        })?;

        Ok(SimplefbParams {
            width,
            height,
            stride,
            format,
        })
    }

    /// Probe the framebuffer device.
    fn probe_internal(pdev: &platform::Device<Core>) -> Result {
        let dev = pdev.as_ref();

        let params = Self::parse_pd(pdev).or_else(|_| {
            if dev.fwnode().is_some() {
                Self::parse_dt(pdev)
            } else {
                Err(ENODEV)
            }
        })?;

        let resource = pdev.resource_by_index(0).ok_or_else(|| {
            dev_err!(dev, "[rust] No memory resource\n");
            EINVAL
        })?;

        let mem_region = resource.request_region(
            resource.start(),
            resource.size(),
            c_str!("simplefb").to_cstring()?,
            Flags::IORESOURCE_MEM,
        );

        let mem: &Resource = match &mem_region {
            Some(region) => region as &Resource,
            None => {
                dev_warn!(
                    dev,
                    "[rust] simplefb: cannot reserve video memory at 0x{:x}-0x{:x}\n",
                    resource.start(),
                    resource.start() + resource.size() - 1
                );
                resource
            }
        };

        let memory_start = mem.start();
        let memory_size = mem.size();

        let (iomem_kbox, screen_base) = Self::map_framebuffer_memory(pdev)?;

        let fb_device = fb::Device::<SimplefbDriverImpl>::new(
            dev,
            try_pin_init!(SimplefbData {
                palette: [0u32; PSEUDO_PALETTE_SIZE],
                base: memory_start,
                size: memory_size,
                mem: mem_region,
                _iomem: iomem_kbox,
            }),
        )?;

        let mut fix_template = init_simplefb_fix();
        let fmt = params.format;
        let mut var_template = init_simplefb_var();

        fix_template.set_smem_start(memory_start as usize);
        fix_template.set_smem_len(memory_size as u32);
        fix_template.set_line_length(params.stride);

        var_template.set_xres(params.width);
        var_template.set_yres(params.height);
        var_template.set_xres_virtual(params.width);
        var_template.set_yres_virtual(params.height);
        var_template.set_bits_per_pixel(fmt.bits_per_pixel());
        var_template.set_red(fmt.red());
        var_template.set_green(fmt.green());
        var_template.set_blue(fmt.blue());
        var_template.set_transp(fmt.transp());

        // SAFETY: We have exclusive access to the fb_device during initialization,
        // before it is registered with the framebuffer subsystem.
        unsafe {
            fb_device.configure_fix(|fix| {
                *fix = fix_template.into_raw();
            });

            fb_device.configure_var(|var| {
                *var = var_template.into_raw();
            });

            fb_device.set_screen_base(screen_base);
            fb_device.set_pseudo_palette(fb_device.data().palette.as_ptr() as *mut _);
        }

        dev_info!(
            dev,
            "[rust] framebuffer at 0x{:x}, 0x{:x} bytes\n",
            fb_device.fix().smem_start(),
            fb_device.fix().smem_len()
        );

        dev_info!(
            dev,
            "[rust] format={}, mode={}x{}x{}, linelength={}\n",
            params.format.name(),
            fb_device.var().xres(),
            fb_device.var().yres(),
            fb_device.var().bits_per_pixel(),
            fb_device.fix().line_length()
        );

        // TODO: Implement devm_aperture_acquire_for_platform_device to manage framebuffer
        // memory ownership and prevent conflicts with dedicated drivers (e.g., DRM).

        if let Err(err) = fb::Registration::new_foreign_owned(&fb_device, pdev.as_ref()) {
            dev_err!(
                dev,
                "[rust] Unable to register simplefb: {}\n",
                err.to_errno()
            );
            return Err(err);
        }

        dev_info!(dev, "[rust] fb{}: simplefb registered!\n", fb_device.node());

        Ok(())
    }
}

kernel::of_device_table!(
    OF_TABLE,
    MODULE_OF_TABLE,
    <SimplefbDriver as platform::Driver>::IdInfo,
    [(of::DeviceId::new(c_str!("simple-framebuffer")), ())]
);

impl platform::Driver for SimplefbDriver {
    type IdInfo = ();
    const OF_ID_TABLE: Option<of::IdTable<Self::IdInfo>> = Some(&OF_TABLE);

    fn probe(
        pdev: &platform::Device<Core>,
        _id_info: Option<&Self::IdInfo>,
    ) -> impl PinInit<Self, Error> {
        Self::probe_internal(pdev)?;
        Ok(Self { _pdev: pdev.into() })
    }
}

kernel::module_platform_driver! {
    type: SimplefbDriver,
    name: "simple-framebuffer",
    authors: ["pengfuyuan <pengfuyuan@kylinos.cn>", "Rust port"],
    description: "Simple framebuffer driver (Rust)",
    license: "GPL v2",
}
