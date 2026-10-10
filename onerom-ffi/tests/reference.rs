//! Builds the images the One ROM CLI built on a Mac (scratch/onerom) through
//! the C ABI and checks they are byte-identical. Needs the files fetched by
//! hand into ../scratch/onerom (not in git: base firmware, plugin, test ROMs).
use onerom_ffi::*;
use std::ffi::c_char;

const DIR: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/../scratch/onerom");

fn s(buf: &[u8]) -> String {
    String::from_utf8_lossy(&buf[..buf.iter().position(|&b| b == 0).unwrap_or(buf.len())]).into_owned()
}

fn build(json: &str, board: &str) -> Vec<u8> {
    let mut err = [0u8; 512];
    let b = unsafe { ort_builder_new(json.as_ptr() as *const c_char, json.len(), 0, 8, 0, err.as_mut_ptr() as *mut c_char, 512) };
    assert!(!b.is_null(), "{}", s(&err));
    let mut files = [0u8; 4096];
    unsafe { ort_builder_files(b, files.as_mut_ptr() as *mut c_char, 4096) };
    for line in s(&files).lines() {
        let (id, src) = line.split_once('\t').unwrap();
        // The Tab5 keeps downloads in a cache on the card; here they sit beside the configs.
        let local = match src.rsplit_once("/plugins/system/") {
            Some((_, rest)) => format!("{}-{}.bin", rest.split('/').next().unwrap(), rest.split('/').nth(1).unwrap().trim_start_matches('v')),
            None => src.to_string(),
        };
        let data = std::fs::read(format!("{DIR}/{local}")).unwrap();
        let rc = unsafe { ort_builder_add_file(b, id.parse().unwrap(), data.as_ptr(), data.len(), err.as_mut_ptr() as *mut c_char, 512) };
        assert_eq!(rc, 0, "{}", s(&err));
    }
    let board_c = format!("{board}\0");
    let (mut m, mut ml, mut i, mut il) = (std::ptr::null_mut(), 0usize, std::ptr::null_mut(), 0usize);
    let rc = unsafe { ort_builder_build(b, board_c.as_ptr() as *const c_char, c"M".as_ptr(), &mut m, &mut ml, &mut i, &mut il, err.as_mut_ptr() as *mut c_char, 512) };
    assert_eq!(rc, 0, "{}", s(&err));
    let fw = std::fs::read(format!("{DIR}/fw-0.8.0.bin")).unwrap();
    let (mut o, mut ol) = (std::ptr::null_mut(), 0usize);
    assert_eq!(unsafe { ort_assemble(fw.as_ptr(), fw.len(), m, ml, i, il, &mut o, &mut ol) }, 0);
    let out = unsafe { std::slice::from_raw_parts(o, ol) }.to_vec();
    unsafe {
        ort_free(m, ml);
        ort_free(i, il);
        ort_free(o, ol);
        ort_builder_free(b);
    }
    out
}

fn check(json_file: &str, reference: &str) {
    let path = format!("{DIR}/{reference}");
    if !std::path::Path::new(&path).exists() {
        eprintln!("skipping: {path} not present");
        return;
    }
    let json = std::fs::read_to_string(format!("{DIR}/{json_file}")).unwrap();
    let ours = build(&json, "fire-28-c");
    let theirs = std::fs::read(path).unwrap();
    assert_eq!(ours.len(), theirs.len(), "length");
    if let Some(at) = ours.iter().zip(&theirs).position(|(a, b)| a != b) {
        panic!("first difference at {at:#x}");
    }
}

#[test]
fn single_27c512_with_usb_plugin() {
    check("tab5-1.json", "ref1.bin");
}

#[test]
fn chip_types_for_fire_28() {
    let mut out = [0u8; 4096];
    unsafe { ort_chip_types(c"fire-28-c".as_ptr(), out.as_mut_ptr() as *mut c_char, 4096) };
    let list = s(&out);
    assert!(list.lines().any(|l| l == "27C512"), "{list}");
}

#[test]
fn two_slots_one_file_duplicated() {
    check("tab5-2.json", "ref2.bin");
}

#[test]
fn pickers_choose_what_the_cli_chose() {
    let fw = match std::fs::read_to_string(format!("{DIR}/releases.json")) { Ok(s) => s, Err(_) => return };
    let mut out = [0u8; 512];
    unsafe { ort_pick_firmware(fw.as_ptr() as *const c_char, fw.len(), c"fire-28-c".as_ptr(), out.as_mut_ptr() as *mut c_char, 512) };
    assert_eq!(s(&out), "version=0.8.0\nurl=https://images.onerom.org/v0.8.0/fire/rp2350/firmware.bin\n");
    let pl = std::fs::read_to_string(format!("{DIR}/usb-releases.json")).unwrap();
    unsafe { ort_pick_plugin(pl.as_ptr() as *const c_char, pl.len(), c"https://images.onerom.org/plugins/system/usb".as_ptr(), 0, 8, 0, out.as_mut_ptr() as *mut c_char, 512) };
    assert!(s(&out).starts_with("version=0.3.2\nurl=https://images.onerom.org/plugins/system/usb/v0.3.2/plugin.bin\nsha256=86854e8b"), "{}", s(&out));
    unsafe { ort_chip_info(c"2364".as_ptr(), out.as_mut_ptr() as *mut c_char, 512) };
    assert!(s(&out).contains("size=8192") && s(&out).contains("line=cs1:config"), "{}", s(&out));
}

unsafe extern "C" fn read_image(ctx: *mut std::ffi::c_void, addr: u32, buf: *mut u8, len: u32) -> i32 {
    let img = unsafe { &*(ctx as *const Vec<u8>) };
    let Some(off) = addr.checked_sub(0x1000_0000) else { return -1 };
    let (off, len) = (off as usize, len as usize);
    let out = unsafe { std::slice::from_raw_parts_mut(buf, len) };
    for (i, b) in out.iter_mut().enumerate() {
        *b = *img.get(off + i).unwrap_or(&0xFF);
    }
    0
}

#[test]
fn parse_reference_image_as_flash() {
    let img = match std::fs::read(format!("{DIR}/ref2.bin")) { Ok(v) => v, Err(_) => return };
    let mut out = [0u8; 2048];
    unsafe { ort_parse_device(read_image, &img as *const Vec<u8> as *mut _, out.as_mut_ptr() as *mut c_char, 2048) };
    let t = s(&out);
    eprintln!("{t}");
    assert!(t.contains("recognised=1") && t.contains("board=fire-28-c") && t.contains("version=0.8.0"), "{t}");
    assert!(t.contains("\t2764\t8192\t") && t.contains("\t27C256\t32768\t"), "{t}");
}

#[test]
fn layout_and_select_pins() {
    let mut out = [0u8; 256];
    unsafe { ort_layout(c"fire-28-c".as_ptr(), out.as_mut_ptr() as *mut c_char, 256) };
    let t = s(&out);
    eprintln!("{t}");
    assert!(t.contains("firmware_size=49152") && t.contains("metadata_len=16384") && t.contains("sel_pins="), "{t}");
}
