// SPDX-License-Identifier: GPL-2.0

//! Rust Goldfish address space driver.

#![forbid(unsafe_code)]

use core::{mem::size_of, pin::Pin};
use kernel::{
    alloc::KVVec,
    device::Core,
    devres::Devres,
    error::Error,
    fs::File,
    io::{Io, PhysAddr},
    ioctl,
    miscdevice::{MiscDevice, MiscDeviceOpenContext, MiscDeviceOptions, MiscDeviceRegistration},
    new_condvar, new_mutex,
    page::{Page, PAGE_SIZE},
    pci,
    prelude::*,
    sync::{Arc, ArcBorrow, CondVar, Mutex},
    types::ScopeGuard,
    uaccess::{UserPtr, UserSlice},
    uapi,
};

const GOLDFISH_AS_CONTROL_BAR: u32 = 0;
const GOLDFISH_AS_AREA_BAR: u32 = 1;
const GOLDFISH_AS_VENDOR_ID: u32 = 0x607d;
const GOLDFISH_AS_DEVICE_ID: u32 = 0xf153;
const GOLDFISH_AS_SUPPORTED_REVISION: u8 = 1;
const GOLDFISH_AS_INVALID_HANDLE: u32 = u32::MAX;

const GOLDFISH_ADDRESS_SPACE_IOCTL_MAGIC: u32 = uapi::GOLDFISH_ADDRESS_SPACE_IOCTL_MAGIC as u32;
const GOLDFISH_ADDRESS_SPACE_IOCTL_ALLOCATE_BLOCK: u32 =
    ioctl::_IOWR::<AllocateBlockIoctl>(GOLDFISH_ADDRESS_SPACE_IOCTL_MAGIC, 10);
const GOLDFISH_ADDRESS_SPACE_IOCTL_DEALLOCATE_BLOCK: u32 =
    ioctl::_IOWR::<u64>(GOLDFISH_ADDRESS_SPACE_IOCTL_MAGIC, 11);
const GOLDFISH_ADDRESS_SPACE_IOCTL_PING: u32 =
    ioctl::_IOWR::<PingIoctl>(GOLDFISH_ADDRESS_SPACE_IOCTL_MAGIC, 12);
const GOLDFISH_ADDRESS_SPACE_IOCTL_CLAIM_SHARED: u32 =
    ioctl::_IOWR::<ClaimSharedIoctl>(GOLDFISH_ADDRESS_SPACE_IOCTL_MAGIC, 13);
const GOLDFISH_ADDRESS_SPACE_IOCTL_UNCLAIM_SHARED: u32 =
    ioctl::_IOWR::<u64>(GOLDFISH_ADDRESS_SPACE_IOCTL_MAGIC, 14);

struct Registers;

impl Registers {
    const COMMAND: usize = 0;
    const STATUS: usize = 4;
    const GUEST_PAGE_SIZE: usize = 8;
    const BLOCK_SIZE_LOW: usize = 12;
    const BLOCK_SIZE_HIGH: usize = 16;
    const BLOCK_OFFSET_LOW: usize = 20;
    const BLOCK_OFFSET_HIGH: usize = 24;
    const PING: usize = 28;
    const PING_INFO_ADDR_LOW: usize = 32;
    const PING_INFO_ADDR_HIGH: usize = 36;
    const HANDLE: usize = 40;
    const PHYS_START_LOW: usize = 44;
    const PHYS_START_HIGH: usize = 48;
    const END: usize = 56;
}

#[repr(u32)]
#[derive(Clone, Copy)]
enum CommandId {
    AllocateBlock = 1,
    DeallocateBlock = 2,
    GenHandle = 3,
    DestroyHandle = 4,
    TellPingInfoAddr = 5,
}

type ControlBar = pci::Bar<{ Registers::END }>;

#[derive(Clone, Copy)]
struct Block {
    offset: u64,
    size: u64,
}

struct BlockSet {
    blocks: KVVec<Block>,
}

impl BlockSet {
    fn new() -> Self {
        Self {
            blocks: KVVec::new(),
        }
    }

    fn insert(&mut self, block: Block) -> Result {
        self.blocks.push(block, GFP_KERNEL)?;
        Ok(())
    }

    fn remove(&mut self, offset: u64) -> Result<Block> {
        let index = self
            .blocks
            .iter()
            .position(|block| block.offset == offset)
            .ok_or(ENXIO)?;
        self.blocks.remove(index).map_err(|_| EINVAL)
    }

    fn iter(&self) -> impl Iterator<Item = Block> + '_ {
        self.blocks.iter().copied()
    }

    fn clear(&mut self) {
        let _ = self.take_all();
    }

    fn take_all(&mut self) -> KVVec<Block> {
        let mut blocks = KVVec::new();
        core::mem::swap(&mut blocks, &mut self.blocks);
        blocks
    }
}

#[derive(Clone, Copy, Default)]
struct PingInfoHeader {
    offset: u64,
    size: u64,
    metadata: u64,
    version: u32,
    wait_fd: u32,
    wait_flags: u32,
    direction: u32,
    data_size: u64,
}

impl PingInfoHeader {
    const ENCODED_LEN: usize = 48;

    fn encode(self) -> [u8; Self::ENCODED_LEN] {
        let mut bytes = [0u8; Self::ENCODED_LEN];

        bytes[0..8].copy_from_slice(&self.offset.to_ne_bytes());
        bytes[8..16].copy_from_slice(&self.size.to_ne_bytes());
        bytes[16..24].copy_from_slice(&self.metadata.to_ne_bytes());
        bytes[24..28].copy_from_slice(&self.version.to_ne_bytes());
        bytes[28..32].copy_from_slice(&self.wait_fd.to_ne_bytes());
        bytes[32..36].copy_from_slice(&self.wait_flags.to_ne_bytes());
        bytes[36..40].copy_from_slice(&self.direction.to_ne_bytes());
        bytes[40..48].copy_from_slice(&self.data_size.to_ne_bytes());

        bytes
    }

    fn decode(bytes: &[u8; Self::ENCODED_LEN]) -> Self {
        Self {
            offset: u64::from_ne_bytes(bytes[0..8].try_into().unwrap()),
            size: u64::from_ne_bytes(bytes[8..16].try_into().unwrap()),
            metadata: u64::from_ne_bytes(bytes[16..24].try_into().unwrap()),
            version: u32::from_ne_bytes(bytes[24..28].try_into().unwrap()),
            wait_fd: u32::from_ne_bytes(bytes[28..32].try_into().unwrap()),
            wait_flags: u32::from_ne_bytes(bytes[32..36].try_into().unwrap()),
            direction: u32::from_ne_bytes(bytes[36..40].try_into().unwrap()),
            data_size: u64::from_ne_bytes(bytes[40..48].try_into().unwrap()),
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct AllocateBlockIoctl {
    size: u64,
    offset: u64,
    phys_addr: u64,
}

impl AllocateBlockIoctl {
    const ENCODED_LEN: usize = 24;

    fn encode(self) -> [u8; Self::ENCODED_LEN] {
        let mut bytes = [0u8; Self::ENCODED_LEN];
        bytes[0..8].copy_from_slice(&self.size.to_ne_bytes());
        bytes[8..16].copy_from_slice(&self.offset.to_ne_bytes());
        bytes[16..24].copy_from_slice(&self.phys_addr.to_ne_bytes());
        bytes
    }

    fn decode(bytes: &[u8; Self::ENCODED_LEN]) -> Self {
        Self {
            size: u64::from_ne_bytes(bytes[0..8].try_into().unwrap()),
            offset: u64::from_ne_bytes(bytes[8..16].try_into().unwrap()),
            phys_addr: u64::from_ne_bytes(bytes[16..24].try_into().unwrap()),
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct PingIoctl {
    offset: u64,
    size: u64,
    metadata: u64,
    version: u32,
    wait_fd: u32,
    wait_flags: u32,
    direction: u32,
}

impl PingIoctl {
    const ENCODED_LEN: usize = 40;

    fn encode(self) -> [u8; Self::ENCODED_LEN] {
        let mut bytes = [0u8; Self::ENCODED_LEN];
        bytes[0..8].copy_from_slice(&self.offset.to_ne_bytes());
        bytes[8..16].copy_from_slice(&self.size.to_ne_bytes());
        bytes[16..24].copy_from_slice(&self.metadata.to_ne_bytes());
        bytes[24..28].copy_from_slice(&self.version.to_ne_bytes());
        bytes[28..32].copy_from_slice(&self.wait_fd.to_ne_bytes());
        bytes[32..36].copy_from_slice(&self.wait_flags.to_ne_bytes());
        bytes[36..40].copy_from_slice(&self.direction.to_ne_bytes());
        bytes
    }

    fn decode(bytes: &[u8; Self::ENCODED_LEN]) -> Self {
        Self {
            offset: u64::from_ne_bytes(bytes[0..8].try_into().unwrap()),
            size: u64::from_ne_bytes(bytes[8..16].try_into().unwrap()),
            metadata: u64::from_ne_bytes(bytes[16..24].try_into().unwrap()),
            version: u32::from_ne_bytes(bytes[24..28].try_into().unwrap()),
            wait_fd: u32::from_ne_bytes(bytes[28..32].try_into().unwrap()),
            wait_flags: u32::from_ne_bytes(bytes[32..36].try_into().unwrap()),
            direction: u32::from_ne_bytes(bytes[36..40].try_into().unwrap()),
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct ClaimSharedIoctl {
    offset: u64,
    size: u64,
}

impl ClaimSharedIoctl {
    const ENCODED_LEN: usize = 16;

    fn encode(self) -> [u8; Self::ENCODED_LEN] {
        let mut bytes = [0u8; Self::ENCODED_LEN];
        bytes[0..8].copy_from_slice(&self.offset.to_ne_bytes());
        bytes[8..16].copy_from_slice(&self.size.to_ne_bytes());
        bytes
    }

    fn decode(bytes: &[u8; Self::ENCODED_LEN]) -> Self {
        Self {
            offset: u64::from_ne_bytes(bytes[0..8].try_into().unwrap()),
            size: u64::from_ne_bytes(bytes[8..16].try_into().unwrap()),
        }
    }
}

struct PingState {
    page: Page,
}

impl PingState {
    fn new() -> Result<Self> {
        let mut page = Page::alloc_page(GFP_KERNEL)?;
        page.fill_zero(0, PAGE_SIZE)?;
        Ok(Self { page })
    }

    fn phys_addr(&self) -> PhysAddr {
        self.page.phys_addr()
    }

    fn shared_offset(offset: u64, shared_phys_start: PhysAddr) -> Result<u64> {
        let shared_phys_start = u64::try_from(shared_phys_start).map_err(|_| EOVERFLOW)?;
        offset.checked_add(shared_phys_start).ok_or(EOVERFLOW)
    }

    fn prepare_ping(&mut self, request: &PingIoctl, shared_phys_start: PhysAddr) -> Result {
        let header = PingInfoHeader {
            offset: Self::shared_offset(request.offset, shared_phys_start)?,
            size: request.size,
            metadata: request.metadata,
            version: request.version,
            wait_fd: request.wait_fd,
            wait_flags: request.wait_flags,
            direction: request.direction,
            data_size: 0,
        };

        self.page.fill_zero(0, PAGE_SIZE)?;
        self.page.write_slice(&header.encode(), 0)
    }

    fn finish_ping(&self, request: &mut PingIoctl) -> Result {
        let mut bytes = [0u8; PingInfoHeader::ENCODED_LEN];
        self.page.read_slice(&mut bytes, 0)?;
        let header = PingInfoHeader::decode(&bytes);
        request.offset = header.offset;
        request.size = header.size;
        request.metadata = header.metadata;
        request.version = header.version;
        request.wait_fd = header.wait_fd;
        request.wait_flags = header.wait_flags;
        request.direction = header.direction;
        Ok(())
    }
}

#[pin_data]
struct DeviceRuntime {
    #[pin]
    control_bar: Devres<ControlBar>,
    #[pin]
    shared_bar: Devres<pci::SharedMemoryBar>,
    #[pin]
    registers_lock: Mutex<()>,
    #[pin]
    lifecycle: Mutex<RuntimeLifecycleState>,
    #[pin]
    lifecycle_idle: CondVar,
}

struct RuntimeLifecycleState {
    accepting_new_ops: bool,
    active_ops: usize,
    live_files: KVVec<Arc<FileState>>,
}

struct RuntimeOpGuard {
    runtime: Arc<DeviceRuntime>,
}

impl Drop for RuntimeOpGuard {
    fn drop(&mut self) {
        let mut state = self.runtime.lifecycle.lock();
        state.active_ops -= 1;
        self.runtime.notify_if_idle(&state);
    }
}

impl DeviceRuntime {
    fn new(pdev: &pci::Device<Core>) -> Result<Arc<Self>> {
        Arc::pin_init(
            try_pin_init!(Self {
                control_bar <- pdev.iomap_region_sized::<{ Registers::END }>(
                    GOLDFISH_AS_CONTROL_BAR,
                    c"goldfish_address_space/control",
                ),
                shared_bar <- pdev.memremap_bar(
                    GOLDFISH_AS_AREA_BAR,
                    c"goldfish_address_space/area",
                ),
                registers_lock <- new_mutex!(()),
                lifecycle <- new_mutex!(RuntimeLifecycleState {
                    accepting_new_ops: true,
                    active_ops: 0,
                    live_files: KVVec::new(),
                }),
                lifecycle_idle <- new_condvar!("goldfish_address_space/lifecycle_idle"),
            }),
            GFP_KERNEL,
        )
    }

    fn notify_if_idle(&self, state: &RuntimeLifecycleState) {
        if !state.accepting_new_ops && state.active_ops == 0 {
            self.lifecycle_idle.notify_all();
        }
    }

    fn begin_operation(self: &Arc<Self>) -> Result<RuntimeOpGuard> {
        let mut state = self.lifecycle.lock();
        if !state.accepting_new_ops {
            return Err(ENODEV);
        }

        state.active_ops = state.active_ops.checked_add(1).ok_or(EBUSY)?;
        drop(state);

        Ok(RuntimeOpGuard {
            runtime: self.clone(),
        })
    }

    fn register_live_file(&self, file: Arc<FileState>) -> Result {
        let mut state = self.lifecycle.lock();
        if !state.accepting_new_ops {
            return Err(ENODEV);
        }

        state.live_files.push(file, GFP_KERNEL)?;
        Ok(())
    }

    fn unregister_live_file(&self, file: &Arc<FileState>) {
        let mut state = self.lifecycle.lock();
        let Some(index) = state
            .live_files
            .iter()
            .position(|entry| Arc::ptr_eq(entry, file))
        else {
            return;
        };

        if let Ok(entry) = state.live_files.remove(index) {
            drop(entry);
        }
    }

    fn shutdown(&self) {
        let mut state = self.lifecycle.lock();
        // `unbind()` removes miscdevice reachability before calling `shutdown()`. After that we
        // only need to wait for already-entered syscalls to finish; live files are revoked below,
        // so remove is no longer bounded by userspace deciding to close descriptors.
        state.accepting_new_ops = false;

        while state.active_ops != 0 {
            self.lifecycle_idle.wait(&mut state);
        }

        let mut live_files = KVVec::new();
        core::mem::swap(&mut live_files, &mut state.live_files);
        drop(state);

        for file in &live_files {
            file.revoke_for_shutdown();
        }
    }

    fn control_bar(&self) -> Result<impl core::ops::Deref<Target = ControlBar> + '_> {
        self.control_bar.try_access().ok_or(ENXIO)
    }

    fn shared_bar(&self) -> Result<impl core::ops::Deref<Target = pci::SharedMemoryBar> + '_> {
        self.shared_bar.try_access().ok_or(ENXIO)
    }

    fn run_command_locked(control: &ControlBar, command: CommandId) -> Result {
        control.write32(command as u32, Registers::COMMAND);

        let status = i32::try_from(control.read32(Registers::STATUS)).map_err(|_| EIO)?;
        if status == 0 {
            Ok(())
        } else {
            Err(Error::from_errno(-status))
        }
    }

    fn issue_command_locked(control: &ControlBar, command: CommandId) {
        control.write32(command as u32, Registers::COMMAND);
    }

    fn write_u64(control: &ControlBar, low_offset: usize, high_offset: usize, value: u64) {
        control.write32(value as u32, low_offset);
        control.write32((value >> 32) as u32, high_offset);
    }

    fn read_u64(control: &ControlBar, low_offset: usize, high_offset: usize) -> u64 {
        u64::from(control.read32(low_offset)) | (u64::from(control.read32(high_offset)) << 32)
    }

    fn program_host_visible_state(&self) -> Result {
        let control = self.control_bar()?;
        let shared = self.shared_bar()?;
        let phys_start = u64::try_from(shared.phys_start()).map_err(|_| EOVERFLOW)?;

        control.write32(PAGE_SIZE as u32, Registers::GUEST_PAGE_SIZE);
        Self::write_u64(
            &control,
            Registers::PHYS_START_LOW,
            Registers::PHYS_START_HIGH,
            phys_start,
        );

        Ok(())
    }

    fn shared_phys_start(&self) -> Result<PhysAddr> {
        Ok(self.shared_bar()?.phys_start())
    }

    fn generate_handle(&self) -> Result<u32> {
        let _guard = self.registers_lock.lock();
        let control = self.control_bar()?;

        // The external C driver does not gate `GEN_HANDLE` on the status register and instead
        // validates completion by reading back the handle.
        Self::issue_command_locked(&control, CommandId::GenHandle);

        let handle = control.read32(Registers::HANDLE);
        if handle == GOLDFISH_AS_INVALID_HANDLE {
            return Err(EINVAL);
        }

        Ok(handle)
    }

    fn tell_ping_info_addr(&self, handle: u32, ping_info_phys: PhysAddr) -> Result {
        let _guard = self.registers_lock.lock();
        let control = self.control_bar()?;
        let ping_info_phys = u64::try_from(ping_info_phys).map_err(|_| EOVERFLOW)?;

        control.write32(handle, Registers::HANDLE);
        Self::write_u64(
            &control,
            Registers::PING_INFO_ADDR_LOW,
            Registers::PING_INFO_ADDR_HIGH,
            ping_info_phys,
        );
        // The external C driver validates `TELL_PING_INFO_ADDR` through the echoed physical
        // address rather than through the status register.
        Self::issue_command_locked(&control, CommandId::TellPingInfoAddr);

        let returned = Self::read_u64(
            &control,
            Registers::PING_INFO_ADDR_LOW,
            Registers::PING_INFO_ADDR_HIGH,
        );
        if returned != ping_info_phys {
            return Err(EINVAL);
        }

        Ok(())
    }

    fn destroy_handle(&self, handle: u32) -> Result {
        let _guard = self.registers_lock.lock();
        let control = self.control_bar()?;
        control.write32(handle, Registers::HANDLE);
        Self::issue_command_locked(&control, CommandId::DestroyHandle);
        Ok(())
    }

    fn allocate_block(&self, size: u64) -> Result<Block> {
        let _guard = self.registers_lock.lock();
        let control = self.control_bar()?;

        Self::write_u64(
            &control,
            Registers::BLOCK_SIZE_LOW,
            Registers::BLOCK_SIZE_HIGH,
            size,
        );
        Self::run_command_locked(&control, CommandId::AllocateBlock)?;

        Ok(Block {
            offset: Self::read_u64(
                &control,
                Registers::BLOCK_OFFSET_LOW,
                Registers::BLOCK_OFFSET_HIGH,
            ),
            size: Self::read_u64(
                &control,
                Registers::BLOCK_SIZE_LOW,
                Registers::BLOCK_SIZE_HIGH,
            ),
        })
    }

    fn deallocate_block(&self, offset: u64) -> Result {
        let _guard = self.registers_lock.lock();
        let control = self.control_bar()?;
        Self::write_u64(
            &control,
            Registers::BLOCK_OFFSET_LOW,
            Registers::BLOCK_OFFSET_HIGH,
            offset,
        );
        Self::run_command_locked(&control, CommandId::DeallocateBlock)
    }

    fn ping(&self, handle: u32) -> Result {
        let _guard = self.registers_lock.lock();
        self.control_bar()?.write32(handle, Registers::PING);
        Ok(())
    }

    fn cleanup_file_resources<I>(&self, handle: u32, blocks: I)
    where
        I: IntoIterator<Item = Block>,
    {
        // `unbind()` revokes live files before `disable_device()`, so both the shutdown path and a
        // concurrent `release()` may still legitimately touch the BAR here.
        if let Err(err) = self.destroy_handle(handle) {
            pr_warn!(
                "goldfish_address_space: destroy handle {} failed: {}\n",
                handle,
                err.to_errno()
            );
        }

        for block in blocks {
            if let Err(err) = self.deallocate_block(block.offset) {
                pr_warn!(
                    "goldfish_address_space: deallocate block 0x{:x} failed: {}\n",
                    block.offset,
                    err.to_errno()
                );
            }
        }
    }
}

struct FileResources {
    handle: Option<u32>,
    allocated_blocks: BlockSet,
    shared_blocks: BlockSet,
}

impl FileResources {
    fn new(handle: u32) -> Self {
        Self {
            handle: Some(handle),
            allocated_blocks: BlockSet::new(),
            shared_blocks: BlockSet::new(),
        }
    }
}

#[pin_data]
struct FileState {
    runtime: Arc<DeviceRuntime>,
    #[pin]
    ping: Mutex<PingState>,
    #[pin]
    resources: Mutex<FileResources>,
}

impl FileState {
    fn new(runtime: Arc<DeviceRuntime>, handle: u32, ping: PingState) -> Result<Arc<Self>> {
        Arc::pin_init(
            try_pin_init!(Self {
                runtime: runtime,
                ping <- new_mutex!(ping),
                resources <- new_mutex!(FileResources::new(handle)),
            }),
            GFP_KERNEL,
        )
    }

    fn shared_phys_addr(&self, offset: u64) -> Result<u64> {
        let base = u64::try_from(self.runtime.shared_phys_start()?).map_err(|_| EOVERFLOW)?;
        base.checked_add(offset).ok_or(EOVERFLOW)
    }

    fn allocate_block(
        self: ArcBorrow<'_, Self>,
        mut request: AllocateBlockIoctl,
    ) -> Result<AllocateBlockIoctl> {
        let block = self.runtime.allocate_block(request.size)?;
        let mut resources = self.resources.lock();
        if resources.handle.is_none() {
            drop(resources);
            let _ = self.runtime.deallocate_block(block.offset);
            return Err(ENODEV);
        }

        if let Err(err) = resources.allocated_blocks.insert(block) {
            drop(resources);
            let _ = self.runtime.deallocate_block(block.offset);
            return Err(err);
        }

        request.size = block.size;
        request.offset = block.offset;
        request.phys_addr = self.shared_phys_addr(block.offset)?;
        Ok(request)
    }

    fn deallocate_block(self: ArcBorrow<'_, Self>, offset: u64) -> Result {
        let mut resources = self.resources.lock();
        if resources.handle.is_none() {
            return Err(ENODEV);
        }

        if !resources
            .allocated_blocks
            .iter()
            .any(|block| block.offset == offset)
        {
            return Err(ENXIO);
        }

        self.runtime.deallocate_block(offset)?;
        let _ = resources.allocated_blocks.remove(offset)?;
        Ok(())
    }

    fn claim_shared(
        self: ArcBorrow<'_, Self>,
        request: ClaimSharedIoctl,
    ) -> Result<ClaimSharedIoctl> {
        let mut resources = self.resources.lock();
        if resources.handle.is_none() {
            return Err(ENODEV);
        }

        resources.shared_blocks.insert(Block {
            offset: request.offset,
            size: request.size,
        })?;
        Ok(request)
    }

    fn unclaim_shared(self: ArcBorrow<'_, Self>, offset: u64) -> Result {
        let mut resources = self.resources.lock();
        if resources.handle.is_none() {
            return Err(ENODEV);
        }

        resources.shared_blocks.remove(offset)?;
        Ok(())
    }

    fn ping(self: ArcBorrow<'_, Self>, mut request: PingIoctl) -> Result<PingIoctl> {
        let handle = self.resources.lock().handle.ok_or(ENODEV)?;
        let mut ping = self.ping.lock();
        ping.prepare_ping(&request, self.runtime.shared_phys_start()?)?;
        self.runtime.ping(handle)?;
        ping.finish_ping(&mut request)?;
        Ok(request)
    }

    fn cleanup_resources(&self) {
        let mut resources = self.resources.lock();
        let Some(handle) = resources.handle.take() else {
            return;
        };

        self.runtime
            .cleanup_file_resources(handle, resources.allocated_blocks.iter());
        resources.allocated_blocks.clear();
        resources.shared_blocks.clear();
    }

    fn revoke_for_shutdown(&self) {
        self.cleanup_resources();
    }

    fn release(self: Arc<Self>) {
        self.cleanup_resources();
        self.runtime.unregister_live_file(&self);
    }
}

#[pin_data]
struct GoldfishAddressSpaceDriver {
    runtime: Arc<DeviceRuntime>,
    #[pin]
    misc: MiscDeviceRegistration<GoldfishAddressSpaceMisc>,
}

struct GoldfishAddressSpaceMisc;

#[vtable]
impl MiscDevice for GoldfishAddressSpaceMisc {
    type Ptr = Arc<FileState>;
    type RegistrationData = Arc<DeviceRuntime>;

    fn open(_file: &File, ctx: &MiscDeviceOpenContext<'_, Self>) -> Result<Self::Ptr> {
        let runtime = ctx.data().clone();
        let _op = runtime.begin_operation()?;
        let ping = PingState::new()?;
        let handle = runtime.generate_handle()?;
        let cleanup = ScopeGuard::new_with_data((runtime.clone(), handle), |(runtime, handle)| {
            let _ = runtime.destroy_handle(handle);
        });

        runtime.tell_ping_info_addr(handle, ping.phys_addr())?;
        let state = FileState::new(runtime.clone(), handle, ping)?;
        cleanup.dismiss();

        // Publish the file as a live shutdown owner before returning it to the miscdevice core.
        if let Err(err) = runtime.register_live_file(state.clone()) {
            state.release();
            return Err(err);
        }

        Ok(state)
    }

    fn release(device: Self::Ptr, _file: &File) {
        device.release();
    }

    fn ioctl(
        device: ArcBorrow<'_, FileState>,
        _file: &File,
        cmd: u32,
        arg: usize,
    ) -> Result<isize> {
        let _op = device.runtime.begin_operation()?;
        match cmd {
            GOLDFISH_ADDRESS_SPACE_IOCTL_ALLOCATE_BLOCK => {
                let data = UserSlice::new(UserPtr::from_addr(arg), AllocateBlockIoctl::ENCODED_LEN);
                let (mut reader, mut writer) = data.reader_writer();
                let mut bytes = [0u8; AllocateBlockIoctl::ENCODED_LEN];
                reader.read_slice(&mut bytes)?;
                let request = AllocateBlockIoctl::decode(&bytes);
                let response = device.allocate_block(request)?;
                writer.write_slice(&response.encode())?;
                Ok(0)
            }
            GOLDFISH_ADDRESS_SPACE_IOCTL_DEALLOCATE_BLOCK => {
                let mut reader = UserSlice::new(UserPtr::from_addr(arg), size_of::<u64>()).reader();
                device.deallocate_block(reader.read::<u64>()?)?;
                Ok(0)
            }
            GOLDFISH_ADDRESS_SPACE_IOCTL_PING => {
                let data = UserSlice::new(UserPtr::from_addr(arg), PingIoctl::ENCODED_LEN);
                let (mut reader, mut writer) = data.reader_writer();
                let mut bytes = [0u8; PingIoctl::ENCODED_LEN];
                reader.read_slice(&mut bytes)?;
                let request = PingIoctl::decode(&bytes);
                let response = device.ping(request)?;
                writer.write_slice(&response.encode())?;
                Ok(0)
            }
            GOLDFISH_ADDRESS_SPACE_IOCTL_CLAIM_SHARED => {
                let data = UserSlice::new(UserPtr::from_addr(arg), ClaimSharedIoctl::ENCODED_LEN);
                let (mut reader, mut writer) = data.reader_writer();
                let mut bytes = [0u8; ClaimSharedIoctl::ENCODED_LEN];
                reader.read_slice(&mut bytes)?;
                let request = ClaimSharedIoctl::decode(&bytes);
                let response = device.claim_shared(request)?;
                writer.write_slice(&response.encode())?;
                Ok(0)
            }
            GOLDFISH_ADDRESS_SPACE_IOCTL_UNCLAIM_SHARED => {
                let mut reader = UserSlice::new(UserPtr::from_addr(arg), size_of::<u64>()).reader();
                device.unclaim_shared(reader.read::<u64>()?)?;
                Ok(0)
            }
            _ => Err(ENOTTY),
        }
    }

    #[cfg(CONFIG_COMPAT)]
    fn compat_ioctl(
        device: ArcBorrow<'_, FileState>,
        file: &File,
        cmd: u32,
        arg: usize,
    ) -> Result<isize> {
        Self::ioctl(device, file, cmd, arg)
    }
}

kernel::declare_misc_device_fops!(GoldfishAddressSpaceMisc);

kernel::pci_device_table!(
    PCI_TABLE,
    MODULE_PCI_TABLE,
    <GoldfishAddressSpaceDriver as pci::Driver>::IdInfo,
    [(
        pci::DeviceId::from_id(
            pci::Vendor::from_raw(GOLDFISH_AS_VENDOR_ID as u16),
            GOLDFISH_AS_DEVICE_ID,
        ),
        (),
    )]
);

impl pci::Driver for GoldfishAddressSpaceDriver {
    type IdInfo = ();

    const ID_TABLE: pci::IdTable<Self::IdInfo> = &PCI_TABLE;

    fn probe(pdev: &pci::Device<Core>, _id_info: &Self::IdInfo) -> impl PinInit<Self, Error> {
        pin_init::pin_init_scope(move || {
            if pdev.revision_id() != GOLDFISH_AS_SUPPORTED_REVISION {
                return Err(ENODEV);
            }

            pdev.enable_device_mem()?;
            let enable_guard = ScopeGuard::new(|| pdev.disable_device());

            let runtime = DeviceRuntime::new(pdev)?;
            runtime.program_host_visible_state()?;

            let driver = try_pin_init!(Self {
                runtime: runtime.clone(),
                misc <- MiscDeviceRegistration::register_with_data(
                    MiscDeviceOptions {
                        name: c"goldfish_address_space",
                    },
                    runtime.clone(),
                ),
            });
            enable_guard.dismiss();

            Ok(driver)
        })
    }

    fn unbind(pdev: &pci::Device<Core>, this: Pin<&Self>) {
        let this = this.get_ref();
        // 1. Stop new miscdevice opens from reaching the driver.
        this.misc.deregister();
        // 2. Wait for already-running syscalls, then revoke every still-live file's device-side
        //    state before the PCI function disappears.
        this.runtime.shutdown();
        // 3. Only then disable the PCI function, so post-shutdown release never needs to touch a
        //    disabled device.
        pdev.disable_device();
    }
}

kernel::module_pci_driver! {
    type: GoldfishAddressSpaceDriver,
    name: "goldfish_address_space",
    authors: ["Wenzhao Liao"],
    description: "Rust Goldfish address space driver",
    license: "GPL v2",
}
