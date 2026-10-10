//! One ROM's host-side tools, compiled for the Tab5 (ESP32-P4) and called from
//! C++: the image builder (onerom-gen) that turns a config and ROM files into
//! the metadata and ROM data that follow the base firmware in flash, and the
//! firmware parser (onerom-fw-parser) that says what is on a device.
//!
//! Everything crosses the boundary as plain bytes and text. Text results are
//! "key=value" lines. Buffers this library returns are freed with
//! `ort_free`. Errors are written into a caller's buffer as text.
//!
//! On the Tab5 the allocator is ESP-IDF's heap (PSRAM), and a panic is
//! reported through `ort_panic` (supplied by the firmware) and never returns.

#![cfg_attr(not(feature = "std"), no_std)]

extern crate alloc;

use alloc::boxed::Box;
use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;
use core::ffi::{c_char, c_void};
use core::fmt::Write as _;

use onerom_config::fw::{FirmwareProperties, FirmwareVersion, ServeAlg};
use onerom_config::hw::{Board, BoardSize};
use onerom_config::mcu::Variant;
use onerom_gen::{Builder, FileData};

// ---- platform glue (Tab5) ----------------------------------------------------

#[cfg(not(feature = "std"))]
mod platform {
    use core::alloc::{GlobalAlloc, Layout};

    unsafe extern "C" {
        fn heap_caps_aligned_alloc(alignment: usize, size: usize, caps: u32) -> *mut u8;
        fn heap_caps_free(ptr: *mut u8);
        fn ort_panic(msg: *const u8, len: usize) -> !;
    }
    const MALLOC_CAP_SPIRAM: u32 = 1 << 10;
    const MALLOC_CAP_8BIT: u32 = 1 << 2;

    struct Heap;
    unsafe impl GlobalAlloc for Heap {
        unsafe fn alloc(&self, l: Layout) -> *mut u8 {
            unsafe { heap_caps_aligned_alloc(l.align().max(4), l.size(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) }
        }
        unsafe fn dealloc(&self, p: *mut u8, _: Layout) {
            unsafe { heap_caps_free(p) }
        }
    }
    #[global_allocator]
    static HEAP: Heap = Heap;

    #[panic_handler]
    fn panic(info: &core::panic::PanicInfo) -> ! {
        let mut s = alloc::string::String::new();
        let _ = core::fmt::write(&mut s, format_args!("{info}"));
        unsafe { ort_panic(s.as_ptr(), s.len()) }
    }
}

// ---- helpers -----------------------------------------------------------------

/// Copies `s` into a C buffer, truncated and NUL-terminated. Returns the full
/// length (so a caller can tell it was cut short).
fn put(s: &str, out: *mut c_char, cap: usize) -> usize {
    if !out.is_null() && cap > 0 {
        let n = s.len().min(cap - 1);
        unsafe {
            core::ptr::copy_nonoverlapping(s.as_ptr(), out as *mut u8, n);
            *out.add(n) = 0;
        }
    }
    s.len()
}

unsafe fn text<'a>(p: *const c_char, len: usize) -> Option<&'a str> {
    if p.is_null() {
        return None;
    }
    core::str::from_utf8(unsafe { core::slice::from_raw_parts(p as *const u8, len) }).ok()
}

unsafe fn cstr<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        return None;
    }
    let mut n = 0;
    while unsafe { *p.add(n) } != 0 {
        n += 1;
    }
    unsafe { text(p, n) }
}

/// Hands a Vec to C: pointer and length; freed with `ort_free(ptr, len)`.
fn give(v: Vec<u8>, ptr: *mut *mut u8, len: *mut usize) {
    let mut b = v.into_boxed_slice();
    unsafe {
        *len = b.len();
        *ptr = b.as_mut_ptr();
    }
    core::mem::forget(b);
}

/// Runs a future that never waits (everything here is synchronous underneath).
fn block_on<F: core::future::Future>(f: F) -> F::Output {
    use core::task::{Context, Poll, RawWaker, RawWakerVTable, Waker};
    fn noop(_: *const ()) {}
    fn clone(p: *const ()) -> RawWaker {
        RawWaker::new(p, &VTABLE)
    }
    static VTABLE: RawWakerVTable = RawWakerVTable::new(clone, noop, noop, noop);
    let waker = unsafe { Waker::from_raw(RawWaker::new(core::ptr::null(), &VTABLE)) };
    let mut cx = Context::from_waker(&waker);
    let mut f = core::pin::pin!(f);
    loop {
        if let Poll::Ready(v) = f.as_mut().poll(&mut cx) {
            return v;
        }
    }
}

fn board_size(s: &str) -> Option<BoardSize> {
    s.parse().ok()
}

// ---- version / board queries -------------------------------------------------

/// Library and generator versions, for the about box and logs.
#[unsafe(no_mangle)]
pub extern "C" fn ort_versions(out: *mut c_char, cap: usize) -> usize {
    let s = format!(
        "onerom-gen={}\nmetadata={}\nmax_firmware={}\n",
        onerom_gen::crate_version(),
        onerom_gen::metadata_version(),
        onerom_fw_parser::MAX_VERSION
    );
    put(&s, out, cap)
}

/// The chip types `board` serves natively, one per line. 0 if the board is unknown.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ort_chip_types(board: *const c_char, out: *mut c_char, cap: usize) -> usize {
    let Some(board) = (unsafe { cstr(board) }).and_then(Board::try_from_str) else {
        return put("", out, cap);
    };
    let mut s = String::new();
    for name in board.supported_chip_type_names() {
        let _ = writeln!(s, "{name}");
    }
    put(&s, out, cap)
}

/// What the screen needs to know about chip type `name`: lines of
/// size (bytes), pins, and one "line=<name>:<kind>" per control line, kind
/// being "config" (the user picks active-low/high), "low" or "high" (fixed).
/// 0 if the type is unknown.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ort_chip_info(name: *const c_char, out: *mut c_char, cap: usize) -> usize {
    use onerom_config::chip::{ChipType, ControlLineType};
    let Some(t) = (unsafe { cstr(name) }).and_then(ChipType::try_from_str) else {
        return put("", out, cap);
    };
    let mut s = String::new();
    let _ = writeln!(s, "name={}\nsize={}\npins={}", t.name(), t.size_bytes(), t.chip_pins());
    for l in t.control_lines() {
        let kind = match l.line_type {
            ControlLineType::Configurable => "config",
            ControlLineType::FixedActiveLow => "low",
            _ => "high",
        };
        let _ = writeln!(s, "line={}:{}{}", l.name, kind, if l.allow_ignore { ":ignore" } else { "" });
    }
    put(&s, out, cap)
}

// ---- release manifests (images.onerom.org) -----------------------------------

fn version_of(s: &str) -> Option<FirmwareVersion> {
    FirmwareVersion::try_from_str(s.trim_start_matches('v')).ok()
}

/// From the firmware manifest (releases.json), the newest release that has
/// `board`: "version=..\nurl=..". 0 if none.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ort_pick_firmware(
    manifest: *const c_char,
    len: usize,
    board: *const c_char,
    out: *mut c_char,
    cap: usize,
) -> usize {
    let (Some(m), Some(board)) = (unsafe { text(manifest, len) }, unsafe { cstr(board) }) else {
        return put("", out, cap);
    };
    let Ok(v) = serde_json::from_str::<serde_json::Value>(m) else {
        return put("", out, cap);
    };
    let mut best: Option<(FirmwareVersion, String)> = None;
    for r in v["releases"].as_array().into_iter().flatten() {
        let Some(ver_s) = r["version"].as_str() else { continue };
        let Some(ver) = version_of(ver_s) else { continue };
        let path = r["path"].as_str().unwrap_or(ver_s);
        for b in r["boards"].as_array().into_iter().flatten() {
            if b["name"].as_str() != Some(board) {
                continue;
            }
            let Some(mcu) = b["mcus"].as_array().into_iter().flatten().find(|m| m["name"].as_str() == Some("rp2350")) else {
                continue;
            };
            let bpath = b["path"].as_str().unwrap_or(board);
            let mpath = mcu["path"].as_str().unwrap_or("rp2350");
            let url = format!("https://images.onerom.org/{path}/{bpath}/{mpath}/firmware.bin");
            if best.as_ref().is_none_or(|(bv, _)| ver > *bv) {
                best = Some((ver, url));
            }
        }
    }
    match best {
        Some((v, url)) => put(&format!("version={}.{}.{}\nurl={url}\n", v.major(), v.minor(), v.patch()), out, cap),
        None => put("", out, cap),
    }
}

/// From a plugin's manifest (plugins/<type>/<name>/releases.json), the newest
/// release that runs on firmware maj.min.patch:
/// "version=..\nurl=..\nsha256=..". 0 if none.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ort_pick_plugin(
    manifest: *const c_char,
    len: usize,
    base_url: *const c_char,
    maj: u16,
    min: u16,
    patch: u16,
    out: *mut c_char,
    cap: usize,
) -> usize {
    let (Some(m), Some(base)) = (unsafe { text(manifest, len) }, unsafe { cstr(base_url) }) else {
        return put("", out, cap);
    };
    let Ok(v) = serde_json::from_str::<serde_json::Value>(m) else {
        return put("", out, cap);
    };
    let fw = FirmwareVersion::new(maj, min, patch, 0);
    let mut best: Option<(FirmwareVersion, String)> = None;
    for r in v["releases"].as_array().into_iter().flatten() {
        let Some(ver_s) = r["version"].as_str() else { continue };
        let Some(ver) = version_of(ver_s) else { continue };
        if let Some(min_fw) = r["min_fw_version"].as_str().and_then(version_of) {
            if min_fw > fw {
                continue;
            }
        }
        if let Some(max_fw) = r["max_fw_version"].as_str().and_then(version_of) {
            if max_fw < fw {
                continue;
            }
        }
        let path = r["path"].as_str().unwrap_or(ver_s);
        let file = r["filename"].as_str().unwrap_or("plugin.bin");
        let sha = r["sha256"].as_str().unwrap_or("");
        let line = format!("version={ver_s}\nurl={base}/{path}/{file}\nsha256={sha}\n");
        if best.as_ref().is_none_or(|(bv, _)| ver > *bv) {
            best = Some((ver, line));
        }
    }
    put(best.map(|b| b.1).as_deref().unwrap_or(""), out, cap)
}

// ---- builder -----------------------------------------------------------------

pub struct OrtBuilder {
    b: Builder,
    version: FirmwareVersion,
}

/// Parses a config (One ROM's JSON) for firmware `maj.min.patch`. Null on
/// failure, with the reason in `err`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ort_builder_new(
    json: *const c_char,
    json_len: usize,
    maj: u16,
    min: u16,
    patch: u16,
    err: *mut c_char,
    err_cap: usize,
) -> *mut OrtBuilder {
    let Some(json) = (unsafe { text(json, json_len) }) else {
        put("config is not UTF-8", err, err_cap);
        return core::ptr::null_mut();
    };
    let version = FirmwareVersion::new(maj, min, patch, 0);
    match Builder::from_json(version, Variant::RP2350.family(), json) {
        Ok(mut b) => {
            // Licences belong to published ROM sets fetched by URL; a file on
            // the card has none, and accepting is the CLI's --yes.
            for l in b.licenses() {
                let _ = b.accept_license(&l);
            }
            Box::into_raw(Box::new(OrtBuilder { b, version }))
        }
        Err(e) => {
            put(&format!("{e:?}"), err, err_cap);
            core::ptr::null_mut()
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ort_builder_free(b: *mut OrtBuilder) {
    if !b.is_null() {
        drop(unsafe { Box::from_raw(b) });
    }
}

/// The files the config needs, one per line: "id\tsource".
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ort_builder_files(b: *mut OrtBuilder, out: *mut c_char, cap: usize) -> usize {
    let b = unsafe { &mut *b };
    let mut s = String::new();
    for f in b.b.file_specs() {
        let _ = writeln!(s, "{}\t{}", f.id, f.source);
    }
    put(&s, out, cap)
}

/// Supplies file `id`'s bytes. 0 on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ort_builder_add_file(
    b: *mut OrtBuilder,
    id: usize,
    data: *const u8,
    len: usize,
    err: *mut c_char,
    err_cap: usize,
) -> i32 {
    let b = unsafe { &mut *b };
    let data = unsafe { core::slice::from_raw_parts(data, len) }.to_vec();
    match b.b.add_file(FileData::new(id, data)) {
        Ok(()) => 0,
        Err(e) => {
            put(&format!("{e:?}"), err, err_cap);
            1
        }
    }
}

/// Builds the metadata and ROM data for `board` ("fire-28-c") and `size`
/// ("M"/"L"). The caller lays them out after the base firmware with
/// `ort_assemble`. 0 on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ort_builder_build(
    b: *mut OrtBuilder,
    board: *const c_char,
    size: *const c_char,
    meta: *mut *mut u8,
    meta_len: *mut usize,
    image: *mut *mut u8,
    image_len: *mut usize,
    err: *mut c_char,
    err_cap: usize,
) -> i32 {
    let b = unsafe { &mut *b };
    let Some(board) = (unsafe { cstr(board) }).and_then(Board::try_from_str) else {
        put("unknown board", err, err_cap);
        return 1;
    };
    let size = (unsafe { cstr(size) }).and_then(board_size).unwrap_or(BoardSize::M);
    let props = match FirmwareProperties::new(b.version, board, Variant::RP2350, ServeAlg::default(), true) {
        Ok(p) => p.with_board_size(size),
        Err(e) => {
            put(&format!("{e:?}"), err, err_cap);
            return 1;
        }
    };
    match b.b.build(props) {
        Ok((m, i)) => {
            give(m, meta, meta_len);
            give(i, image, image_len);
            0
        }
        Err(e) => {
            put(&format!("{e:?}"), err, err_cap);
            1
        }
    }
}

/// A short description of what the config serves on `board`, for the screen.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ort_builder_description(
    b: *mut OrtBuilder,
    board: *const c_char,
    out: *mut c_char,
    cap: usize,
) -> usize {
    let b = unsafe { &mut *b };
    let d = (unsafe { cstr(board) })
        .and_then(Board::try_from_str)
        .and_then(|bd| b.b.description_for_board(bd).ok())
        .unwrap_or_else(|| b.b.description());
    put(&d, out, cap)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn ort_free(p: *mut u8, len: usize) {
    if !p.is_null() {
        drop(unsafe { Box::from_raw(core::ptr::slice_from_raw_parts_mut(p, len)) });
    }
}

/// The flashable image: base firmware padded to the firmware region, then
/// the metadata padded to its region, then the ROM data (as onerom-fw's
/// assemble_firmware). Freed with `ort_free`. 0 on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ort_assemble(
    fw: *const u8,
    fw_len: usize,
    meta: *const u8,
    meta_len: usize,
    image: *const u8,
    image_len: usize,
    out: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    const FIRMWARE_SIZE: usize = onerom_gen::FIRMWARE_SIZE;
    const MAX_METADATA_LEN: usize = onerom_gen::MAX_METADATA_LEN;
    if fw_len > FIRMWARE_SIZE || meta_len > MAX_METADATA_LEN {
        return 1;
    }
    let fw = unsafe { core::slice::from_raw_parts(fw, fw_len) };
    let mut buf = Vec::with_capacity(FIRMWARE_SIZE + MAX_METADATA_LEN + image_len);
    buf.extend_from_slice(fw);
    if meta_len == 0 {
        give(buf, out, out_len);
        return 0;
    }
    buf.resize(FIRMWARE_SIZE, 0xFF);
    buf.extend_from_slice(unsafe { core::slice::from_raw_parts(meta, meta_len) });
    if image_len > 0 {
        buf.resize(FIRMWARE_SIZE + MAX_METADATA_LEN, 0xFF);
        buf.extend_from_slice(unsafe { core::slice::from_raw_parts(image, image_len) });
    }
    give(buf, out, out_len);
    0
}

// ---- firmware parser ---------------------------------------------------------

/// Reads `len` bytes at absolute address `addr` from the device (picoboot
/// READ). 0 on success.
pub type OrtReadFn = unsafe extern "C" fn(ctx: *mut c_void, addr: u32, buf: *mut u8, len: u32) -> i32;

struct CbReader {
    f: OrtReadFn,
    ctx: *mut c_void,
}
unsafe impl Send for CbReader {}

impl airfrog_rpc::io::Reader for CbReader {
    type Error = i32;
    async fn read(&mut self, addr: u32, buf: &mut [u8]) -> Result<(), i32> {
        match unsafe { (self.f)(self.ctx, addr, buf.as_mut_ptr(), buf.len() as u32) } {
            0 => Ok(()),
            e => Err(e),
        }
    }
    fn update_base_address(&mut self, _: u32) {}
}

/// Parses what is on a device through `read` and describes it as lines:
/// recognised, board, version, running, mcu, size, slot, usb, errors.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ort_parse_device(read: OrtReadFn, ctx: *mut c_void, out: *mut c_char, cap: usize) -> usize {
    let mut r = CbReader { f: read, ctx };
    let parsed = block_on(
        onerom_fw_parser::Parser::with_base_flash_address(&mut r, 0x1000_0000, 0x2000_0000).parse_device(),
    );
    let mut s = String::new();
    let _ = writeln!(s, "recognised={}", parsed.is_recognised() as u8);
    if let Some(b) = parsed.get_board() {
        let _ = writeln!(s, "board={}", b.name());
    }
    if let Some(v) = parsed.version() {
        let _ = writeln!(s, "version={}.{}.{}", v.major(), v.minor(), v.patch());
    }
    let _ = writeln!(s, "running={}", parsed.is_running() as u8);
    if let Some(m) = parsed.mcu_name() {
        let _ = writeln!(s, "mcu={m}");
    }
    if let Some(sz) = parsed.runtime_board_size() {
        let _ = writeln!(s, "size={sz:?}");
    }
    if let Some(i) = parsed.active_slot_index() {
        let _ = writeln!(s, "slot={i}");
    }
    let _ = writeln!(s, "usb={}", parsed.is_usb_run_capable() as u8);
    // One line per ROM in each slot: "slot=index\tuser\tkind\tactive\ttype\tsize\tfile"
    // (user is the jumper-selected number, -1 for a plugin).
    for slot in parsed.slots() {
        let kind = match slot.kind {
            onerom_fw_parser::device::SlotKind::Plugin => "plugin",
            _ => "rom",
        };
        let user = slot.user_index.map(|u| u as i64).unwrap_or(-1);
        for rom in slot.roms() {
            let _ = writeln!(
                s,
                "slot={}\t{}\t{}\t{}\t{}\t{}\t{}",
                slot.slot_index,
                user,
                kind,
                slot.active as u8,
                rom.rom_type,
                rom.size,
                rom.filename.unwrap_or("")
            );
        }
    }
    for e in parsed.parse_errors() {
        let _ = writeln!(s, "error={e:?}");
    }
    put(&s, out, cap)
}
