// latha-gui test app (tests/deep-stack.sh): `start` recurses DEPTH calls deep
// (args[1], default 1000000) before it says its size, as LibWeb layout and
// LibJS do on real pages; latha-gui's default wasm stack has room for that,
// LATHA_GUI_WASM_STACK_MB=1 does not. Then it paints one green frame.
wit_bindgen::generate!({
    path: "../../../qt2wasm/gui/wit",
    world: "latha:gui/gui",
});

use exports::latha::gui::app::Guest;
use latha::gui::types::{Damage, Rect, Size};
use std::sync::atomic::{AtomicU32, Ordering};

static W: AtomicU32 = AtomicU32::new(64);
static H: AtomicU32 = AtomicU32::new(48);
static DIRTY: AtomicU32 = AtomicU32::new(1);

#[inline(never)]
fn down(n: u64) -> u64 {
    if n == 0 {
        return 0;
    }
    // Not a tail call: every level keeps its frame.
    std::hint::black_box(down(std::hint::black_box(n - 1))) + 1
}

struct App;

impl Guest for App {
    fn start(args: Vec<String>) -> Result<Size, String> {
        let depth = args.get(1).and_then(|a| a.parse().ok()).unwrap_or(1_000_000u64);
        let got = down(depth);
        eprintln!("deep-stack: recursed {got} calls");
        Ok(Size { width: 64, height: 48 })
    }
    fn resize(width: u32, height: u32, _scale: f64) {
        W.store(width, Ordering::Relaxed);
        H.store(height, Ordering::Relaxed);
        DIRTY.store(1, Ordering::Relaxed);
    }
    fn key(_: latha::gui::types::KeyEvent) {}
    fn text(_: String) {}
    fn pointer(_: latha::gui::types::PointerEvent) {}
    fn wheel(_: latha::gui::types::WheelEvent) {}
    fn focus(_: bool) {}
    fn color_scheme(_: bool) {}
    fn close() -> bool {
        true
    }
    fn tick() -> Option<u32> {
        None
    }
    fn paint() -> Damage {
        let (w, h) = (W.load(Ordering::Relaxed), H.load(Ordering::Relaxed));
        let rects = if DIRTY.swap(0, Ordering::Relaxed) != 0 { vec![Rect { x: 0, y: 0, width: w, height: h }] } else { vec![] };
        Damage { moves: vec![], rects }
    }
    fn frame_size() -> Size {
        Size { width: W.load(Ordering::Relaxed), height: H.load(Ordering::Relaxed) }
    }
    fn frame_pixels(r: Rect) -> String {
        // Premultiplied BGRA, opaque green.
        let px = [0x40u8, 0xa0, 0x40, 0xff];
        let bytes: Vec<u8> = px.iter().copied().cycle().take((r.width * r.height * 4) as usize).collect();
        b64(&bytes)
    }
}

fn b64(b: &[u8]) -> String {
    const T: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut s = String::with_capacity(b.len().div_ceil(3) * 4);
    for c in b.chunks(3) {
        let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        s.push(T[(n >> 18) as usize & 63] as char);
        s.push(T[(n >> 12) as usize & 63] as char);
        s.push(if c.len() > 1 { T[(n >> 6) as usize & 63] as char } else { '=' });
        s.push(if c.len() > 2 { T[n as usize & 63] as char } else { '=' });
    }
    s
}

export!(App);
