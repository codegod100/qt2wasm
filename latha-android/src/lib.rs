// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only
//
// latha-android: a latha:gui runner for Android. One NativeActivity, no Java
// and no Qt: the app (a latha:gui component, qt2wasm/gui/wit/gui.wit) runs on
// wasmtime through latha-host (../latha-gui/host, the same host latha-gui
// uses) and we are its window: we copy the frames it paints into the
// activity's ANativeWindow, turn touches into mouse events, and keys (hardware
// or soft keyboard) into key events, with dead-key compositions as
// `app.text`.
//
// Everything runs on android_main's thread, like latha-gui's Qt thread: the
// host's imports are callbacks into `Shared` below, and the loop (`pump`)
// mirrors latha-gui's window.cpp: tick when due, paint at most every 16 ms.
//
// Which app: a wasm file latha was opened with ("Open with" / "Share", see
// `opened_file`) is copied to `<external files dir>/app.wasm` and so becomes
// the app from then on; `<external files dir>/app.wasm` if it exists (adb push
// it to /sdcard/Android/data/uk.nandi.latha/files/app.wasm), else the APK's
// assets/app.wasm. assets/app.args (one per line, optional) are its args;
// the first is its name. An app that names its update manifest (latha:app
// version= and update=, ../latha-gui/host/src/update.rs) gets newer builds
// into files/updates, and the newest copy runs.

use android_activity::input::{
    InputEvent, KeyAction, KeyEvent, KeyMapChar, Keycode, MetaState, MotionAction, MotionEvent,
};
use android_activity::{AndroidApp, AndroidAppWaker, InputStatus, MainEvent, PollEvent, WindowManagerFlags};
use latha_host::{
    latha_close, latha_color_scheme, latha_damage, latha_error, latha_focus, latha_frame_pixels, latha_frame_size,
    latha_key, latha_load, latha_out_set, latha_paint, latha_pointer, latha_resize, latha_start, latha_string_free,
    latha_text, latha_tick, latha_wheel, LathaApp, LathaCallbacks, LathaMove, LathaOptions, LathaOut, LathaPointer,
    LathaRect, LathaStr, LathaWheel,
};
use ndk::configuration::UiModeNight;
use ndk::hardware_buffer_format::HardwareBufferFormat;
use ndk::native_window::NativeWindow;
use std::cell::{Cell, RefCell};
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::io::Read as _;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

const MIN_FRAME: Duration = Duration::from_millis(16);
const CURSOR_TEXT: u32 = 1; // types.cursor: default, text, ...

// --- the host's imports ------------------------------------------------------------

/// What the app asked of us since we last looked. Only android_main's thread
/// touches it, except `woken` + `waker` (latha:proc wakes from any thread).
struct Shared {
    frame_wanted: Cell<bool>,
    tick_at: Cell<Option<Instant>>,
    cursor: Cell<u32>,
    quit: Cell<Option<i32>>,
    dark: Cell<bool>,
    /// 0 the clipboard, 1 the primary selection. In-process only for now.
    clip: RefCell<[String; 2]>,
    woken: AtomicBool,
    waker: AndroidAppWaker,
}

unsafe fn sh<'a>(u: *mut c_void) -> &'a Shared {
    &*(u as *const Shared)
}

unsafe fn s(v: &LathaStr) -> &str {
    if v.ptr.is_null() || v.len == 0 {
        return "";
    }
    std::str::from_utf8_unchecked(std::slice::from_raw_parts(v.ptr, v.len))
}

extern "C" fn cb_request_frame(u: *mut c_void) {
    let h = unsafe { sh(u) };
    h.frame_wanted.set(true);
}
extern "C" fn cb_request_tick(u: *mut c_void, ms: u32) {
    let h = unsafe { sh(u) };
    h.tick_at.set(Some(Instant::now() + Duration::from_millis(ms as u64)));
}
extern "C" fn cb_set_title(_: *mut c_void, t: LathaStr) {
    log::info!("title: {}", unsafe { s(&t) });
}
extern "C" fn cb_set_cursor(u: *mut c_void, shape: u32) {
    unsafe { sh(u) }.cursor.set(shape);
}
extern "C" fn cb_set_min_size(_: *mut c_void, _: u32, _: u32) {}
extern "C" fn cb_clipboard_text(u: *mut c_void, sel: c_int, out: *mut LathaOut) {
    let h = unsafe { sh(u) };
    let c = h.clip.borrow();
    let t = &c[(sel != 0) as usize];
    unsafe { latha_out_set(out, t.as_ptr(), t.len()) };
}
extern "C" fn cb_set_clipboard_text(u: *mut c_void, sel: c_int, t: LathaStr) {
    let h = unsafe { sh(u) };
    h.clip.borrow_mut()[(sel != 0) as usize] = unsafe { s(&t) }.to_owned();
}
extern "C" fn cb_prefers_dark(u: *mut c_void) -> c_int {
    unsafe { sh(u) }.dark.get() as c_int
}
extern "C" fn cb_accent_color(_: *mut c_void, _: *mut u32) -> c_int {
    0
}
extern "C" fn cb_bell(_: *mut c_void) {}
extern "C" fn cb_open_url(_: *mut c_void, url: LathaStr) -> c_int {
    log::info!("open-url (not supported yet): {}", unsafe { s(&url) });
    0
}
extern "C" fn cb_quit(u: *mut c_void, code: i32) {
    unsafe { sh(u) }.quit.set(Some(code));
}
extern "C" fn cb_pty_spawn(
    _: *mut c_void,
    _: *const LathaStr,
    _: usize,
    _: *const LathaStr,
    _: usize,
    _: LathaStr,
    _: u16,
    _: u16,
    err: *mut LathaOut,
) -> i64 {
    let m = "latha-android has no ptys";
    unsafe { latha_out_set(err, m.as_ptr(), m.len()) };
    -1
}
extern "C" fn cb_pty_write(_: *mut c_void, _: u32, _: *const u8, _: usize) {}
extern "C" fn cb_pty_read(_: *mut c_void, _: u32, _: *mut LathaOut) {}
extern "C" fn cb_pty_resize(_: *mut c_void, _: u32, _: u16, _: u16) {}
extern "C" fn cb_pty_exit_code(_: *mut c_void, _: u32, code: *mut i32) -> c_int {
    unsafe { *code = -1 };
    1
}
extern "C" fn cb_pty_close(_: *mut c_void, _: u32) {}
extern "C" fn cb_wake(u: *mut c_void) {
    let h = unsafe { sh(u) };
    h.woken.store(true, Ordering::Release);
    h.waker.wake();
}

fn callbacks(user: *mut c_void) -> LathaCallbacks {
    LathaCallbacks {
        user,
        request_frame: cb_request_frame,
        request_tick: cb_request_tick,
        set_title: cb_set_title,
        set_cursor: cb_set_cursor,
        set_min_size: cb_set_min_size,
        clipboard_text: cb_clipboard_text,
        set_clipboard_text: cb_set_clipboard_text,
        prefers_dark: cb_prefers_dark,
        accent_color: cb_accent_color,
        bell: cb_bell,
        open_url: cb_open_url,
        quit: cb_quit,
        pty_spawn: cb_pty_spawn,
        pty_write: cb_pty_write,
        pty_read: cb_pty_read,
        pty_resize: cb_pty_resize,
        pty_exit_code: cb_pty_exit_code,
        pty_close: cb_pty_close,
        wake: Some(cb_wake),
    }
}

fn lstr(v: &str) -> LathaStr {
    LathaStr { ptr: v.as_ptr(), len: v.len() }
}

unsafe fn take_err(e: *mut c_char) -> String {
    if e.is_null() {
        return "unknown error".into();
    }
    let m = CStr::from_ptr(e).to_string_lossy().into_owned();
    latha_string_free(e);
    m
}

// --- the runner --------------------------------------------------------------------

/// The part of the window the app gets: the activity's content rect (it
/// shrinks while the soft keyboard is up), in window pixels.
#[derive(Clone, Copy, PartialEq, Default, Debug)]
struct View {
    x: i32,
    y: i32,
    w: u32,
    h: u32,
}

struct Runner {
    android: AndroidApp,
    shared: Box<Shared>,
    app: *mut LathaApp,
    dead: bool,
    window: Option<NativeWindow>,
    view: View,
    applied: (u32, u32, f64),
    /// Our copy of the frame: premultiplied BGRA (0xAARRGGBB), fw x fh.
    frame: Vec<u32>,
    fw: u32,
    fh: u32,
    blit_wanted: bool,
    last_paint: Instant,
    tick_now: bool,
    // touch -> mouse
    primary: Option<i32>,
    two_finger: Option<(f32, f32)>,
    last_press: Option<(Instant, f32, f32)>,
    clicks: u32,
    buttons: u32,
    /// when a third finger came down (a quick three-finger tap toggles the keyboard)
    three: Option<Instant>,
    /// where the first finger came down; the press is held back until it moves
    /// past the touch slop or lifts, so a second finger arriving first (scroll,
    /// three-finger tap) leaves no mark
    pending: Option<(f32, f32)>,
    kbd: bool,
    // keys
    accent: Option<char>,
    exit: bool,
}

fn scale_of(app: &AndroidApp) -> f64 {
    app.config().density().map_or(1.0, |d| d as f64 / 160.0)
}

fn dark_of(app: &AndroidApp) -> bool {
    app.config().ui_mode_night() == UiModeNight::Yes
}

impl Runner {
    fn check(&mut self, r: c_int) -> bool {
        if r != 0 && !self.dead {
            self.dead = true;
            let m = unsafe { CStr::from_ptr(latha_error(self.app)) }.to_string_lossy().into_owned();
            log::error!("the app trapped: {m}");
        }
        r == 0 && !self.dead
    }

    fn live(&self) -> bool {
        !self.app.is_null() && !self.dead
    }

    /// Where the app wants its frame shown, in the current window.
    fn compute_view(&self) -> View {
        let Some(w) = &self.window else { return View::default() };
        let (ww, wh) = (w.width().max(0), w.height().max(0));
        let r = self.android.content_rect();
        let (x0, y0) = (r.left.clamp(0, ww), r.top.clamp(0, wh));
        let (x1, y1) = (r.right.clamp(0, ww), r.bottom.clamp(0, wh));
        if x1 - x0 < 16 || y1 - y0 < 16 {
            return View { x: 0, y: 0, w: ww as u32, h: wh as u32 };
        }
        View { x: x0, y: y0, w: (x1 - x0) as u32, h: (y1 - y0) as u32 }
    }

    fn apply_size(&mut self) {
        self.view = self.compute_view();
        self.blit_wanted = true;
        if !self.live() || self.view.w == 0 || self.view.h == 0 {
            return;
        }
        let s = scale_of(&self.android);
        let want = (self.view.w, self.view.h, s);
        if want == self.applied {
            return;
        }
        self.applied = want;
        let r = unsafe { latha_resize(self.app, want.0, want.1, s) };
        if self.check(r) {
            self.shared.frame_wanted.set(true);
            self.tick_now = true;
        }
    }

    fn window_changed(&mut self) {
        self.window = self.android.native_window();
        if let Some(w) = &self.window {
            if let Err(e) = w.set_buffers_geometry(0, 0, Some(HardwareBufferFormat::R8G8B8X8_UNORM)) {
                log::warn!("set_buffers_geometry: {e}");
            }
        }
        self.apply_size();
    }

    // --- loading ---

    /// A gray window while wasmtime compiles the app (seconds the first time).
    fn fill(&self, rgbx: u32) {
        let Some(w) = &self.window else { return };
        let Ok(mut g) = w.lock(None) else { return };
        let (stride, h) = (g.stride(), g.height());
        let p = g.bits() as *mut u32;
        unsafe { std::slice::from_raw_parts_mut(p, stride * h) }.fill(rgbx);
    }

    fn load(&mut self) -> Result<(), String> {
        let files = self.android.internal_data_path().ok_or("no internal data path")?;
        let cache = files.join("cache");
        std::env::set_var("LATHA_GUI_CACHE_DIR", &cache);
        std::env::set_var("HOME", "/data");
        let (given, args) = self.find_app(&files)?;
        // A newer downloaded build of the app (latha-host update.rs) runs instead; the update check below looks
        // for the next one.
        let store = files.join("updates");
        let path = latha_host::update::resolve(&given, &store);
        log::info!("loading {} {:?}", path.display(), args);
        let home = files.join("home");
        let _ = std::fs::create_dir_all(&home);
        let host_dir = home.to_string_lossy().into_owned();
        let dirs = [lstr(&host_dir)];
        let guest = [lstr("/data")];
        let opts = LathaOptions { dirs: dirs.as_ptr(), guest_dirs: guest.as_ptr(), ndirs: 1, no_cache: 0 };
        let cb = callbacks(&*self.shared as *const Shared as *mut c_void);
        let a: Vec<LathaStr> = args.iter().map(|v| lstr(v)).collect();
        let cpath = CString::new(path.to_string_lossy().into_owned()).map_err(|e| e.to_string())?;
        let t = Instant::now();
        let mut err: *mut c_char = std::ptr::null_mut();
        let app = unsafe { latha_load(cpath.as_ptr(), &opts, &cb, a.as_ptr(), a.len(), &mut err) };
        if app.is_null() {
            return Err(unsafe { take_err(err) });
        }
        log::info!("loaded in {:?}", t.elapsed());
        self.app = app;
        let (mut w, mut h) = (0u32, 0u32);
        let mut err: *mut c_char = std::ptr::null_mut();
        match unsafe { latha_start(app, a.as_ptr(), a.len(), &mut w, &mut h, &mut err) } {
            0 => {}
            1 => return Err(unsafe { take_err(err) }),
            _ => {
                self.check(-1);
                return Err("the app trapped in start".into());
            }
        }
        self.apply_size();
        // Look for a newer build of the app (its latha:app version= and update=; at most every few hours); it
        // runs from the next start.
        std::thread::spawn(move || match latha_host::update::check(&given, &store, false) {
            Ok(latha_host::update::Checked::Updated(v, p)) => {
                log::info!("downloaded version {v} of the app ({}); it runs from the next start", p.display())
            }
            Ok(c) => log::info!("update check: {c:?}"),
            Err(e) => log::warn!("update check: {e:#}"),
        });
        Ok(())
    }

    fn find_app(&self, files: &Path) -> Result<(PathBuf, Vec<String>), String> {
        let am = self.android.asset_manager();
        let read_asset = |name: &CStr| -> Option<Vec<u8>> {
            let mut a = am.open(name)?;
            let mut v = Vec::new();
            a.read_to_end(&mut v).ok()?;
            Some(v)
        };
        let mut args: Vec<String> = read_asset(c"app.args")
            .map(|b| String::from_utf8_lossy(&b).lines().map(str::to_owned).filter(|l| !l.is_empty()).collect())
            .unwrap_or_default();
        if let Some(ext) = self.android.external_data_path() {
            let p = ext.join("app.wasm");
            match opened_file(&self.android, &p) {
                Ok(true) => log::info!("opened with a wasm file: now {}", p.display()),
                Ok(false) => {}
                Err(e) => return Err(format!("the file latha was opened with: {e}")),
            }
            if p.is_file() {
                return Ok((p, vec!["app".into()]));
            }
        }
        let bytes = read_asset(c"app.wasm").ok_or("no app: the APK has no assets/app.wasm")?;
        let p = files.join("app.wasm");
        if std::fs::read(&p).ok().as_deref() != Some(&bytes[..]) {
            std::fs::write(&p, &bytes).map_err(|e| format!("{}: {e}", p.display()))?;
        }
        if args.is_empty() {
            args.push("app".into());
        }
        Ok((p, args))
    }

    // --- frames ---

    fn apply_move(&mut self, m: &LathaMove) {
        let (fw, fh) = (self.fw as i32, self.fh as i32);
        let clip = |x: i32, y: i32, w: i32, h: i32| {
            let (x0, y0) = (x.max(0), y.max(0));
            let (x1, y1) = ((x + w).min(fw), (y + h).min(fh));
            (x0, y0, x1, y1)
        };
        let a = &m.area;
        let (ax0, ay0, ax1, ay1) = clip(a.x, a.y, a.width as i32, a.height as i32);
        if ax1 <= ax0 || ay1 <= ay0 || (m.dx == 0 && m.dy == 0) {
            return;
        }
        // dst = area translated by (dx, dy), clipped to area
        let (dx0, dy0) = ((ax0 + m.dx).max(ax0), (ay0 + m.dy).max(ay0));
        let (dx1, dy1) = ((ax1 + m.dx).min(ax1), (ay1 + m.dy).min(ay1));
        if dx1 <= dx0 || dy1 <= dy0 {
            return;
        }
        let w = (dx1 - dx0) as usize;
        let row = |i: i32| {
            let dy = dy0 + i;
            let sy = dy - m.dy;
            let src = (sy * fw + dx0 - m.dx) as usize;
            let dst = (dy * fw + dx0) as usize;
            (src, dst)
        };
        let n = dy1 - dy0;
        let order: Box<dyn Iterator<Item = i32>> = if m.dy > 0 { Box::new((0..n).rev()) } else { Box::new(0..n) };
        for i in order {
            let (src, dst) = row(i);
            self.frame.copy_within(src..src + w, dst);
        }
    }

    fn paint(&mut self) {
        if !self.check(unsafe { latha_paint(self.app) }) {
            return;
        }
        let (mut mp, mut nm, mut rp, mut nr) =
            (std::ptr::null::<LathaMove>(), 0usize, std::ptr::null::<LathaRect>(), 0usize);
        unsafe { latha_damage(self.app, &mut mp, &mut nm, &mut rp, &mut nr) };
        if nm == 0 && nr == 0 {
            return;
        }
        let moves: Vec<LathaMove> = unsafe { std::slice::from_raw_parts(mp, nm) }.to_vec();
        let rects: Vec<LathaRect> = unsafe { std::slice::from_raw_parts(rp, nr) }.to_vec();
        let (mut fw, mut fh) = (0u32, 0u32);
        if !self.check(unsafe { latha_frame_size(self.app, &mut fw, &mut fh) }) {
            return;
        }
        if (fw, fh) != (self.fw, self.fh) {
            let mut f = vec![0xff00_0000u32; fw as usize * fh as usize];
            for y in 0..fh.min(self.fh) as usize {
                let n = fw.min(self.fw) as usize;
                let s = y * self.fw as usize;
                f[y * fw as usize..y * fw as usize + n].copy_from_slice(&self.frame[s..s + n]);
            }
            self.frame = f;
            self.fw = fw;
            self.fh = fh;
        }
        for m in &moves {
            self.apply_move(m);
        }
        for r in &rects {
            let x0 = r.x.max(0) as u32;
            let y0 = r.y.max(0) as u32;
            let x1 = (r.x as i64 + r.width as i64).clamp(0, fw as i64) as u32;
            let y1 = (r.y as i64 + r.height as i64).clamp(0, fh as i64) as u32;
            if x1 <= x0 || y1 <= y0 {
                continue;
            }
            let c = LathaRect { x: x0 as i32, y: y0 as i32, width: x1 - x0, height: y1 - y0 };
            let off = (y0 * fw + x0) as usize;
            let dst = self.frame[off..].as_mut_ptr() as *mut u8;
            let r = unsafe { latha_frame_pixels(self.app, c, dst, fw as usize * 4) };
            if r == -2 {
                log::warn!("frame-pixels: bad data for {x0},{y0} {}x{}", c.width, c.height);
            } else if !self.check(r) {
                return;
            }
        }
        self.blit_wanted = true;
    }

    /// Our frame into the window: BGRA -> RGBX, outside the view black.
    fn blit(&mut self) {
        self.blit_wanted = false;
        let Some(w) = &self.window else { return };
        let Ok(mut g) = w.lock(None) else { return };
        let (stride, ww, wh) = (g.stride(), g.width(), g.height());
        let out = unsafe { std::slice::from_raw_parts_mut(g.bits() as *mut u32, stride * wh) };
        out.fill(0);
        let v = self.view;
        let rows = (self.fh as usize).min(v.h as usize).min(wh.saturating_sub(v.y as usize));
        let cols = (self.fw as usize).min(v.w as usize).min(ww.saturating_sub(v.x as usize));
        for y in 0..rows {
            let src = &self.frame[y * self.fw as usize..y * self.fw as usize + cols];
            let o = (v.y as usize + y) * stride + v.x as usize;
            for (d, &p) in out[o..o + cols].iter_mut().zip(src) {
                *d = (p & 0xff00_ff00) | ((p >> 16) & 0xff) | ((p & 0xff) << 16);
            }
        }
    }

    // --- input ---

    fn pointer(&mut self, kind: u32, button: u32, x: f32, y: f32, clicks: u32) {
        if !self.live() {
            return;
        }
        let p = LathaPointer {
            kind,
            button,
            buttons: self.buttons,
            modifiers: 0,
            x: (x - self.view.x as f32) as f64,
            y: (y - self.view.y as f32) as f64,
            clicks,
        };
        let r = unsafe { latha_pointer(self.app, &p) };
        self.check(r);
    }

    fn wheel(&mut self, x: f32, y: f32, steps_x: f64, steps_y: f64, px: f64, py: f64) {
        if !self.live() {
            return;
        }
        let w = LathaWheel {
            x: (x - self.view.x as f32) as f64,
            y: (y - self.view.y as f32) as f64,
            steps_x,
            steps_y,
            pixels_x: px,
            pixels_y: py,
            modifiers: 0,
        };
        let r = unsafe { latha_wheel(self.app, &w) };
        self.check(r);
    }

    /// The held-back press of the first finger, at the place it came down.
    fn press_pending(&mut self) {
        if let Some((x, y)) = self.pending.take() {
            self.buttons = 1;
            self.pointer(0, 1, x, y, self.clicks);
        }
    }

    /// One finger is the left mouse button; a second finger turns the touch
    /// into a two-finger scroll (the press is let go off-surface, so it does
    /// not click). A mouse's wheel is the wheel.
    fn motion(&mut self, ev: &MotionEvent) {
        let idx = ev.pointer_index();
        match ev.action() {
            MotionAction::Down => {
                let p = ev.pointer_at_index(idx);
                let (x, y) = (p.x(), p.y());
                self.primary = Some(p.pointer_id());
                self.two_finger = None;
                let now = Instant::now();
                self.clicks = match self.last_press {
                    Some((t, lx, ly))
                        if now - t < Duration::from_millis(400) && (lx - x).abs() < 24.0 && (ly - y).abs() < 24.0 =>
                    {
                        self.clicks + 1
                    }
                    _ => 1,
                };
                self.last_press = Some((now, x, y));
                self.pointer(3, 0, x, y, 0); // enter
                self.pointer(2, 0, x, y, 0); // move there first: hover state
                self.pending = Some((x, y));
            }
            MotionAction::PointerDown => {
                if self.primary.is_some() && self.two_finger.is_none() {
                    // let go of the press where nothing is: no click
                    if self.pending.take().is_none() {
                        self.buttons = 0;
                        self.pointer(1, 1, -1e6, -1e6, 0);
                    }
                    self.pointer(4, 0, -1e6, -1e6, 0);
                    self.primary = None;
                }
                self.two_finger = Some(centroid(ev));
                if ev.pointer_count() >= 3 && self.three.is_none() {
                    self.three = Some(Instant::now());
                }
            }
            MotionAction::Move => {
                if let Some((cx, cy)) = self.two_finger {
                    let (nx, ny) = centroid(ev);
                    let (dx, dy) = ((cx - nx) as f64, (cy - ny) as f64);
                    if dx.abs() >= 1.0 || dy.abs() >= 1.0 {
                        self.two_finger = Some((nx, ny));
                        let s = 120.0 * scale_of(&self.android) / 160.0 * 1.333; // ~ px a notch
                        self.wheel(nx, ny, dx / s, dy / s, dx, dy);
                    }
                    return;
                }
                if let Some(id) = self.primary {
                    if let Some(p) = ev.pointers().find(|p| p.pointer_id() == id) {
                        if let Some((px, py)) = self.pending {
                            let slop = 8.0 * scale_of(&self.android) as f32;
                            if (p.x() - px).hypot(p.y() - py) < slop {
                                return;
                            }
                            self.press_pending();
                        }
                        self.pointer(2, 0, p.x(), p.y(), 0);
                    }
                }
            }
            MotionAction::Up | MotionAction::Cancel => {
                // a quick three-finger tap toggles the soft keyboard in any app
                if let Some(t) = self.three.take() {
                    if ev.action() == MotionAction::Up && t.elapsed() < Duration::from_millis(500) {
                        self.set_keyboard(!self.kbd);
                    }
                }
                if let Some(id) = self.primary.take() {
                    let p = ev.pointers().find(|p| p.pointer_id() == id).unwrap_or(ev.pointer_at_index(idx));
                    if ev.action() == MotionAction::Up {
                        self.press_pending(); // a tap: press now, release below
                    } else {
                        self.pending = None; // cancelled before it pressed
                    }
                    self.buttons = 0;
                    self.pointer(1, 1, p.x(), p.y(), 0);
                    self.pointer(4, 0, p.x(), p.y(), 0);
                    self.after_tap();
                }
                self.two_finger = None;
            }
            MotionAction::PointerUp => {
                if self.two_finger.is_some() {
                    // the centroid of the fingers left
                    let rest: Vec<(f32, f32)> =
                        ev.pointers().filter(|p| p.pointer_index() != idx).map(|p| (p.x(), p.y())).collect();
                    if !rest.is_empty() {
                        let n = rest.len() as f32;
                        self.two_finger =
                            Some((rest.iter().map(|p| p.0).sum::<f32>() / n, rest.iter().map(|p| p.1).sum::<f32>() / n));
                    }
                }
            }
            MotionAction::HoverMove | MotionAction::HoverEnter => {
                let p = ev.pointer_at_index(0);
                self.pointer(2, 0, p.x(), p.y(), 0);
            }
            MotionAction::HoverExit => {
                let p = ev.pointer_at_index(0);
                self.pointer(4, 0, p.x(), p.y(), 0);
            }
            MotionAction::Scroll => {
                let p = ev.pointer_at_index(0);
                use android_activity::input::Axis;
                let sx = p.axis_value(Axis::Hscroll) as f64;
                let sy = -p.axis_value(Axis::Vscroll) as f64;
                self.wheel(p.x(), p.y(), sx, sy, 0.0, 0.0);
            }
            _ => {}
        }
    }

    /// The soft keyboard comes up after a tap that left a text cursor (the
    /// app shows `text` over editable text), and goes away otherwise.
    fn after_tap(&mut self) {
        let show = self.shared.cursor.get() == CURSOR_TEXT;
        self.set_keyboard(show);
    }

    fn set_keyboard(&mut self, show: bool) {
        self.kbd = show;
        if show {
            self.android.show_soft_input(true);
        } else {
            self.android.hide_soft_input(true);
        }
    }

    /// true: we used it.
    fn key(&mut self, ev: &KeyEvent) -> bool {
        let code = ev.key_code();
        let pressed = match ev.action() {
            KeyAction::Down => true,
            KeyAction::Up => false,
            _ => return false,
        };
        if code == Keycode::Back {
            if !pressed && self.live() {
                match unsafe { latha_close(self.app) } {
                    1 => self.exit = true,
                    0 => {}
                    _ => {
                        self.check(-1);
                    }
                }
            }
            return true;
        }
        if matches!(code, Keycode::VolumeUp | Keycode::VolumeDown | Keycode::VolumeMute | Keycode::Power) {
            return false;
        }
        if code == Keycode::Menu {
            if pressed {
                self.kbd = true;
                self.android.show_soft_input(false);
            }
            return true;
        }
        if !self.live() {
            return true;
        }
        let meta = ev.meta_state();
        let mods = mods_of(meta);
        let mut keysym = special_keysym(code);
        let mut text = String::new();
        if keysym == 0 {
            if let Ok(map) = self.android.device_key_character_map(ev.device_id()) {
                // ctrl+letter: the map has no character for it
                let plain = MetaState(meta.0 & !(CTRL_MASK | ALT_MASK | META_MASK));
                match map.get(code, plain) {
                    Ok(KeyMapChar::Unicode(c)) => {
                        if let Some(a) = self.accent.filter(|_| pressed) {
                            self.accent = None;
                            if let Ok(Some(k)) = map.get_dead_char(a, c) {
                                let t = k.to_string();
                                let r = unsafe { latha_text(self.app, lstr(&t)) };
                                self.check(r);
                                return true;
                            }
                        }
                        keysym = keysym_of(c);
                        if mods & 2 != 0 && c.is_ascii_alphabetic() {
                            text.push(char::from(c.to_ascii_lowercase() as u8 - b'a' + 1));
                        } else if mods & (4 | 8) == 0 {
                            text.push(c);
                        }
                    }
                    Ok(KeyMapChar::CombiningAccent(a)) => {
                        if pressed {
                            self.accent = Some(a);
                        }
                        return true;
                    }
                    _ => {}
                }
            }
        } else if pressed {
            text = match keysym {
                0xff0d | 0xff8d => "\r".into(),
                0xff08 => "\u{8}".into(),
                0xff09 => "\t".into(),
                0xff1b => "\u{1b}".into(),
                0xffff => "\u{7f}".into(),
                0x20 => " ".into(),
                _ => String::new(),
            };
        }
        if keysym == 0 && text.is_empty() {
            return false;
        }
        if !pressed {
            text.clear();
        }
        let scancode = ev.scan_code().max(0) as u32 + 8;
        let r = unsafe {
            latha_key(self.app, pressed as c_int, (ev.repeat_count() > 0) as c_int, keysym, scancode, mods, lstr(&text))
        };
        self.check(r);
        // the soft keyboard sends down + up at once; repaint between keys
        true
    }

    // --- the loop ---

    fn pump(&mut self) {
        if !self.live() {
            return;
        }
        let now = Instant::now();
        if self.shared.woken.swap(false, Ordering::AcqRel) {
            self.tick_now = true;
        }
        if self.shared.quit.get().is_none()
            && (self.tick_now || self.shared.tick_at.get().is_some_and(|t| now >= t))
        {
            self.tick_now = false;
            let next = unsafe { latha_tick(self.app) };
            if next == -2 {
                self.check(-1);
                return;
            }
            self.shared
                .tick_at
                .set((next >= 0).then(|| Instant::now() + Duration::from_millis(next as u64)));
        }
        if let Some(code) = self.shared.quit.get() {
            log::info!("the app quit with {code}");
            self.exit = true;
            return;
        }
        if self.shared.frame_wanted.get() && self.applied.0 != 0 && self.window.is_some() {
            if self.last_paint.elapsed() >= MIN_FRAME {
                self.shared.frame_wanted.set(false);
                self.last_paint = Instant::now();
                self.paint();
            }
        }
        if self.blit_wanted && self.window.is_some() {
            self.blit();
        }
    }

    /// How long poll_events may sleep.
    fn timeout(&self) -> Option<Duration> {
        if !self.live() {
            return None;
        }
        if self.tick_now || self.shared.woken.load(Ordering::Acquire) {
            return Some(Duration::ZERO);
        }
        let mut t: Option<Duration> = None;
        let mut at = |d: Duration| t = Some(t.map_or(d, |t| t.min(d)));
        if let Some(when) = self.shared.tick_at.get() {
            at(when.saturating_duration_since(Instant::now()));
        }
        if self.shared.frame_wanted.get() && self.window.is_some() && self.applied.0 != 0 {
            at(MIN_FRAME.saturating_sub(self.last_paint.elapsed()));
        }
        t
    }
}

fn centroid(ev: &MotionEvent) -> (f32, f32) {
    let (mut x, mut y, mut n) = (0.0, 0.0, 0.0);
    for p in ev.pointers() {
        x += p.x();
        y += p.y();
        n += 1.0;
    }
    if n == 0.0 {
        (0.0, 0.0)
    } else {
        (x / n, y / n)
    }
}

const SHIFT_MASK: u32 = 0x1;
const ALT_MASK: u32 = 0x2;
const CTRL_MASK: u32 = 0x1000;
const META_MASK: u32 = 0x10000;

fn mods_of(m: MetaState) -> u32 {
    let mut b = 0;
    if m.0 & SHIFT_MASK != 0 {
        b |= 1;
    }
    if m.0 & CTRL_MASK != 0 {
        b |= 2;
    }
    if m.0 & ALT_MASK != 0 {
        b |= 4;
    }
    if m.0 & META_MASK != 0 {
        b |= 8;
    }
    b
}

/// X keysym of a character: Latin-1 as is, the rest as 0x01000000 + code.
fn keysym_of(c: char) -> u32 {
    let u = c as u32;
    if (0x20..=0x7e).contains(&u) || (0xa0..=0xff).contains(&u) {
        // keysyms for letters are the lowercase ones; shift is a modifier
        if c.is_ascii_uppercase() {
            u + 0x20
        } else {
            u
        }
    } else {
        0x0100_0000 + u
    }
}

fn special_keysym(k: Keycode) -> u32 {
    match k {
        Keycode::Enter => 0xff0d,
        Keycode::NumpadEnter => 0xff8d,
        Keycode::Del => 0xff08,
        Keycode::ForwardDel => 0xffff,
        Keycode::Tab => 0xff09,
        Keycode::Escape => 0xff1b,
        Keycode::Space => 0x20,
        Keycode::DpadLeft => 0xff51,
        Keycode::DpadUp => 0xff52,
        Keycode::DpadRight => 0xff53,
        Keycode::DpadDown => 0xff54,
        Keycode::MoveHome => 0xff50,
        Keycode::MoveEnd => 0xff57,
        Keycode::PageUp => 0xff55,
        Keycode::PageDown => 0xff56,
        Keycode::Insert => 0xff63,
        Keycode::ShiftLeft => 0xffe1,
        Keycode::ShiftRight => 0xffe2,
        Keycode::CtrlLeft => 0xffe3,
        Keycode::CtrlRight => 0xffe4,
        Keycode::AltLeft => 0xffe9,
        Keycode::AltRight => 0xffea,
        Keycode::MetaLeft => 0xffeb,
        Keycode::MetaRight => 0xffec,
        Keycode::F1 => 0xffbe,
        Keycode::F2 => 0xffbf,
        Keycode::F3 => 0xffc0,
        Keycode::F4 => 0xffc1,
        Keycode::F5 => 0xffc2,
        Keycode::F6 => 0xffc3,
        Keycode::F7 => 0xffc4,
        Keycode::F8 => 0xffc5,
        Keycode::F9 => 0xffc6,
        Keycode::F10 => 0xffc7,
        Keycode::F11 => 0xffc8,
        Keycode::F12 => 0xffc9,
        _ => 0,
    }
}

// --- "Open with" -------------------------------------------------------------------

/// If the activity was started with a file (ACTION_VIEW's data, or
/// ACTION_SEND's EXTRA_STREAM), copy it to `dest` through the ContentResolver
/// (content:// and file:// alike) and say so. Not a wasm module (no `\0asm`
/// magic): an error, and `dest` is left as it was. The intent is then cleared,
/// so a recreated activity does not copy it again.
///
/// NativeActivity has no onNewIntent, so this is only seen at a cold start: a
/// latha already running keeps its app (leave it with Back, then open again).
fn opened_file(android: &AndroidApp, dest: &Path) -> Result<bool, String> {
    use jni::objects::{JObject, JString, JValue};
    let vm = unsafe { jni::JavaVM::from_raw(android.vm_as_ptr() as *mut jni::sys::JavaVM) }.map_err(|e| e.to_string())?;
    let mut env = vm.attach_current_thread().map_err(|e| e.to_string())?;
    let act = unsafe { JObject::from_raw(android.activity_as_ptr() as jni::sys::jobject) };
    let r = (|| -> jni::errors::Result<Option<String>> {
        let intent = env.call_method(&act, "getIntent", "()Landroid/content/Intent;", &[])?.l()?;
        if intent.is_null() {
            return Ok(None);
        }
        let action = env.call_method(&intent, "getAction", "()Ljava/lang/String;", &[])?.l()?;
        let action: String = if action.is_null() { String::new() } else { env.get_string(&JString::from(action))?.into() };
        let uri = match action.as_str() {
            "android.intent.action.VIEW" => env.call_method(&intent, "getData", "()Landroid/net/Uri;", &[])?.l()?,
            "android.intent.action.SEND" => {
                let k = env.new_string("android.intent.extra.STREAM")?;
                env.call_method(&intent, "getParcelableExtra", "(Ljava/lang/String;)Landroid/os/Parcelable;", &[JValue::Object(&k)])?
                    .l()?
            }
            _ => JObject::null(),
        };
        if uri.is_null() {
            return Ok(None);
        }
        let shown = env.call_method(&uri, "toString", "()Ljava/lang/String;", &[])?.l()?;
        let shown: String = env.get_string(&JString::from(shown))?.into();
        log::info!("opened with {action} {shown}");
        let resolver = env.call_method(&act, "getContentResolver", "()Landroid/content/ContentResolver;", &[])?.l()?;
        let input = env
            .call_method(&resolver, "openInputStream", "(Landroid/net/Uri;)Ljava/io/InputStream;", &[JValue::Object(&uri)])?
            .l()?;
        // Copy beside dest, check, then rename over it.
        let tmp = dest.with_extension("wasm.part");
        let mut out = match std::fs::File::create(&tmp) {
            Ok(f) => f,
            Err(e) => return Ok(Some(format!("{}: {e}", tmp.display()))),
        };
        let jbuf = env.new_byte_array(1 << 20)?;
        let mut buf = vec![0i8; 1 << 20];
        let mut head = Vec::new();
        let mut total = 0usize;
        let mut io_err = None;
        loop {
            let n = env.call_method(&input, "read", "([B)I", &[JValue::Object(&jbuf)])?.i()?;
            if n < 0 {
                break;
            }
            let n = n as usize;
            env.get_byte_array_region(&jbuf, 0, &mut buf[..n])?;
            let bytes = unsafe { std::slice::from_raw_parts(buf.as_ptr() as *const u8, n) };
            if head.len() < 8 {
                head.extend_from_slice(&bytes[..n.min(8 - head.len())]);
            }
            if let Err(e) = std::io::Write::write_all(&mut out, bytes) {
                io_err = Some(e);
                break;
            }
            total += n;
        }
        env.call_method(&input, "close", "()V", &[])?;
        // Seen: don't copy it again if the activity is recreated.
        let empty = env.new_object("android/content/Intent", "()V", &[])?;
        env.call_method(&act, "setIntent", "(Landroid/content/Intent;)V", &[JValue::Object(&empty)])?;
        drop(out);
        let err = if let Some(e) = io_err {
            Some(format!("{}: {e}", tmp.display()))
        } else if !head.starts_with(b"\0asm") {
            Some(format!("{shown} is not a wasm file"))
        } else {
            std::fs::rename(&tmp, dest).err().map(|e| format!("{}: {e}", dest.display()))
        };
        let _ = std::fs::remove_file(&tmp);
        match err {
            Some(e) => Ok(Some(e)),
            None => {
                log::info!("copied {total} bytes");
                Ok(Some(String::new()))
            }
        }
    })();
    match r {
        Ok(None) => Ok(false),
        Ok(Some(e)) if e.is_empty() => Ok(true),
        Ok(Some(e)) => Err(e),
        Err(e) => {
            if env.exception_check().unwrap_or(false) {
                let _ = env.exception_describe();
                let _ = env.exception_clear();
            }
            Err(format!("JNI: {e}"))
        }
    }
}

#[no_mangle]
fn android_main(android: AndroidApp) {
    android_logger::init_once(
        android_logger::Config::default().with_max_level(log::LevelFilter::Info).with_tag("latha"),
    );
    android.set_window_flags(WindowManagerFlags::KEEP_SCREEN_ON, WindowManagerFlags::empty());
    let shared = Box::new(Shared {
        frame_wanted: Cell::new(false),
        tick_at: Cell::new(None),
        cursor: Cell::new(0),
        quit: Cell::new(None),
        dark: Cell::new(dark_of(&android)),
        clip: RefCell::new([String::new(), String::new()]),
        woken: AtomicBool::new(false),
        waker: android.create_waker(),
    });
    let mut r = Runner {
        android: android.clone(),
        shared,
        app: std::ptr::null_mut(),
        dead: false,
        window: None,
        view: View::default(),
        applied: (0, 0, 0.0),
        frame: Vec::new(),
        fw: 0,
        fh: 0,
        blit_wanted: false,
        last_paint: Instant::now() - MIN_FRAME,
        tick_now: false,
        primary: None,
        two_finger: None,
        last_press: None,
        clicks: 0,
        buttons: 0,
        three: None,
        pending: None,
        kbd: false,
        accent: None,
        exit: false,
    };
    let mut tried = false;
    while !r.exit {
        let mut input = false;
        let mut destroy = false;
        let mut win = false;
        let mut size = false;
        let mut focus: Option<bool> = None;
        let mut config = false;
        android.poll_events(r.timeout(), |ev| {
            if let PollEvent::Main(m) = ev {
                match m {
                    MainEvent::InputAvailable => input = true,
                    MainEvent::InitWindow { .. } | MainEvent::TerminateWindow { .. } => win = true,
                    MainEvent::WindowResized { .. } | MainEvent::ContentRectChanged { .. } | MainEvent::InsetsChanged { .. } => {
                        size = true
                    }
                    MainEvent::RedrawNeeded { .. } => r.blit_wanted = true,
                    MainEvent::GainedFocus => focus = Some(true),
                    MainEvent::LostFocus => focus = Some(false),
                    MainEvent::ConfigChanged { .. } => config = true,
                    MainEvent::Destroy => destroy = true,
                    _ => {}
                }
            }
        });
        if destroy {
            break;
        }
        if win {
            // TerminateWindow: native_window() is None once the callback returned
            r.window_changed();
        }
        if size || config {
            r.apply_size();
        }
        if config {
            let d = dark_of(&android);
            if d != r.shared.dark.get() {
                r.shared.dark.set(d);
                if r.live() {
                    let c = unsafe { latha_color_scheme(r.app, d as c_int) };
                    r.check(c);
                }
            }
        }
        if !tried && r.window.is_some() {
            tried = true;
            r.fill(0xff30_3030);
            if let Err(e) = r.load() {
                log::error!("cannot run the app: {e}");
                r.fill(0xff20_20a0); // red: see `adb logcat -s latha`
            }
        }
        if let (Some(f), true) = (focus, r.live()) {
            let c = unsafe { latha_focus(r.app, f as c_int) };
            r.check(c);
        }
        if input {
            if let Ok(mut it) = android.input_events_iter() {
                loop {
                    let more = it.next(|ev| match ev {
                        InputEvent::MotionEvent(m) => {
                            r.motion(m);
                            InputStatus::Handled
                        }
                        InputEvent::KeyEvent(k) => {
                            if r.key(k) {
                                InputStatus::Handled
                            } else {
                                InputStatus::Unhandled
                            }
                        }
                        InputEvent::TextEvent(_) => InputStatus::Handled,
                        _ => InputStatus::Unhandled,
                    });
                    if !more {
                        break;
                    }
                }
            }
        }
        r.pump();
    }
    if !r.app.is_null() {
        unsafe { latha_host::latha_free(r.app) };
        r.app = std::ptr::null_mut();
    }
}
