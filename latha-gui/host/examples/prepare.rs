// Times latha_prepare (fetch from a prebuilt store or compile): `cargo run --release --example prepare APP.wasm`.
extern "C" fn progress(_: *mut std::ffi::c_void, stage: i32, done: u64, total: u64) {
    eprint!("\rstage {stage}: {done}/{total}      ");
}
fn main() {
    let path = std::ffi::CString::new(std::env::args().nth(1).expect("usage: prepare APP.wasm")).unwrap();
    let t = std::time::Instant::now();
    let needs = unsafe { latha_host::latha_needs_prepare(path.as_ptr(), 0) };
    println!("needs prepare: {needs} ({:.2} s)", t.elapsed().as_secs_f64());
    let t = std::time::Instant::now();
    let mut err = std::ptr::null_mut();
    let r = unsafe { latha_host::latha_prepare(path.as_ptr(), 0, Some(progress), std::ptr::null_mut(), &mut err) };
    eprintln!();
    println!("prepare: {r} in {:.1} s", t.elapsed().as_secs_f64());
    let t = std::time::Instant::now();
    let needs = unsafe { latha_host::latha_needs_prepare(path.as_ptr(), 0) };
    println!("needs prepare after: {needs} ({:.2} s)", t.elapsed().as_secs_f64());
}
