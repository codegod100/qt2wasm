// SPDX-FileCopyrightText: 2026 Kwi WebAssembly port contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only
//
// latha-host: runs one latha:gui component (gui/wit/gui.wit, either world) on
// wasmtime and hands it to a native GUI through a small C API (latha_host.h).
// The GUI (latha-gui, C++/Qt) owns the window, the loop, the clipboard and the
// ptys; it gives us function pointers for the `host` and `pty` imports and
// calls the `app` exports through latha_* below. Everything runs on the
// GUI's thread: an import is a direct call back into the GUI.
//
// Pixels: `frame-pixels` returns base64 (strings lift in one copy); we decode
// it straight into the GUI's frame buffer (latha_frame_pixels), so a frame
// row is never copied twice on our side.

use base64::Engine as _;
use std::ffi::{c_char, c_int, c_void, CStr};
use std::path::{Path, PathBuf};
use wasmtime::component::{Component, HasSelf, Linker, ResourceTable};
use wasmtime::error::Context as _;
use wasmtime::{format_err, Config, Engine, Result, Store};
use wasmtime_wasi::{FsPerms, WasiCtx, WasiCtxBuilder, WasiCtxView, WasiView};

wasmtime::component::bindgen!({
    path: "../../qt2wasm/gui/wit",
    world: "latha:gui/gui-pty",
});

use exports::latha::gui::app as wapp;
use latha::gui::types as wt;

const B64: base64::engine::GeneralPurpose = base64::engine::general_purpose::STANDARD;

// --- C types ---------------------------------------------------------------------

/// A borrowed string (not NUL terminated).
#[repr(C)]
pub struct LathaStr {
    pub ptr: *const u8,
    pub len: usize,
}

impl LathaStr {
    fn of(s: &str) -> LathaStr {
        LathaStr { ptr: s.as_ptr(), len: s.len() }
    }
}

/// Where a callback puts bytes it returns: fill it with `latha_out_set`.
pub struct LathaOut {
    data: Vec<u8>,
}

#[repr(C)]
pub struct LathaCallbacks {
    pub user: *mut c_void,
    pub request_frame: extern "C" fn(*mut c_void),
    pub request_tick: extern "C" fn(*mut c_void, u32),
    pub set_title: extern "C" fn(*mut c_void, LathaStr),
    pub set_cursor: extern "C" fn(*mut c_void, u32),
    pub set_min_size: extern "C" fn(*mut c_void, u32, u32),
    /// `selection`: 0 the clipboard, 1 the primary selection.
    pub clipboard_text: extern "C" fn(*mut c_void, c_int, *mut LathaOut),
    pub set_clipboard_text: extern "C" fn(*mut c_void, c_int, LathaStr),
    pub prefers_dark: extern "C" fn(*mut c_void) -> c_int,
    pub bell: extern "C" fn(*mut c_void),
    pub quit: extern "C" fn(*mut c_void, i32),
    /// argv / env (name, value pairs: 2 * envc strings) / cwd (ptr NULL: none).
    /// Returns the pty id (>= 0), or -1 with the message in `err`.
    pub pty_spawn: extern "C" fn(
        *mut c_void,
        *const LathaStr,
        usize,
        *const LathaStr,
        usize,
        LathaStr,
        u16,
        u16,
        *mut LathaOut,
    ) -> i64,
    pub pty_write: extern "C" fn(*mut c_void, u32, *const u8, usize),
    /// Raw output since the last read (we base64 it).
    pub pty_read: extern "C" fn(*mut c_void, u32, *mut LathaOut),
    pub pty_resize: extern "C" fn(*mut c_void, u32, u16, u16),
    /// 1 and `*code` once it ended and its output was read, else 0.
    pub pty_exit_code: extern "C" fn(*mut c_void, u32, *mut i32) -> c_int,
    pub pty_close: extern "C" fn(*mut c_void, u32),
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct LathaRect {
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct LathaMove {
    pub area: LathaRect,
    pub dx: i32,
    pub dy: i32,
}

/// Modifier bits: 1 shift, 2 ctrl, 4 alt, 8 meta, 16 keypad.
/// Button bits: 1 left, 2 right, 4 middle, 8 back, 16 forward.
/// Buttons (one): 0 none, 1 left, 2 right, 3 middle, 4 back, 5 forward.
/// Pointer kinds: 0 press, 1 release, 2 move, 3 enter, 4 leave.
#[repr(C)]
pub struct LathaPointer {
    pub kind: u32,
    pub button: u32,
    pub buttons: u32,
    pub modifiers: u32,
    pub x: f64,
    pub y: f64,
    pub clicks: u32,
}

#[repr(C)]
pub struct LathaWheel {
    pub x: f64,
    pub y: f64,
    pub steps_x: f64,
    pub steps_y: f64,
    pub pixels_x: f64,
    pub pixels_y: f64,
    pub modifiers: u32,
}

#[repr(C)]
pub struct LathaOptions {
    /// Directories the app may open: `dirs[i]` on the host appears as
    /// `guest_dirs[i]` (read-write).
    pub dirs: *const LathaStr,
    pub guest_dirs: *const LathaStr,
    pub ndirs: usize,
    /// 0: keep compiled code in $XDG_CACHE_HOME/latha-gui (the default).
    pub no_cache: c_int,
}

// --- the store's state: the imports ------------------------------------------------

struct Host {
    cb: LathaCallbacks,
    wasi: WasiCtx,
    table: ResourceTable,
}

// SAFETY: the store and its callbacks only ever run on the GUI's thread.
unsafe impl Send for Host {}

impl WasiView for Host {
    fn ctx(&mut self) -> WasiCtxView<'_> {
        WasiCtxView { ctx: &mut self.wasi, table: &mut self.table }
    }
}

fn out_string(o: LathaOut) -> String {
    String::from_utf8(o.data).unwrap_or_else(|e| String::from_utf8_lossy(e.as_bytes()).into_owned())
}

impl wt::Host for Host {}

impl latha::gui::host::Host for Host {
    fn request_frame(&mut self) {
        (self.cb.request_frame)(self.cb.user)
    }
    fn request_tick(&mut self, delay_ms: u32) {
        (self.cb.request_tick)(self.cb.user, delay_ms)
    }
    fn set_title(&mut self, title: String) {
        (self.cb.set_title)(self.cb.user, LathaStr::of(&title))
    }
    fn set_cursor(&mut self, shape: wt::Cursor) {
        (self.cb.set_cursor)(self.cb.user, shape as u32)
    }
    fn set_min_size(&mut self, min: wt::Size) {
        (self.cb.set_min_size)(self.cb.user, min.width, min.height)
    }
    fn clipboard_text(&mut self) -> String {
        let mut o = LathaOut { data: Vec::new() };
        (self.cb.clipboard_text)(self.cb.user, 0, &mut o);
        out_string(o)
    }
    fn set_clipboard_text(&mut self, text: String) {
        (self.cb.set_clipboard_text)(self.cb.user, 0, LathaStr::of(&text))
    }
    fn primary_text(&mut self) -> String {
        let mut o = LathaOut { data: Vec::new() };
        (self.cb.clipboard_text)(self.cb.user, 1, &mut o);
        out_string(o)
    }
    fn set_primary_text(&mut self, text: String) {
        (self.cb.set_clipboard_text)(self.cb.user, 1, LathaStr::of(&text))
    }
    fn prefers_dark(&mut self) -> bool {
        (self.cb.prefers_dark)(self.cb.user) != 0
    }
    fn bell(&mut self) {
        (self.cb.bell)(self.cb.user)
    }
    fn quit(&mut self, code: i32) {
        (self.cb.quit)(self.cb.user, code)
    }
}

impl latha::gui::pty::Host for Host {
    fn spawn(
        &mut self,
        argv: Vec<String>,
        env: Vec<(String, String)>,
        cwd: Option<String>,
        cols: u16,
        rows: u16,
    ) -> Result<u32, String> {
        let a: Vec<LathaStr> = argv.iter().map(|s| LathaStr::of(s)).collect();
        let mut e: Vec<LathaStr> = Vec::with_capacity(env.len() * 2);
        for (k, v) in &env {
            e.push(LathaStr::of(k));
            e.push(LathaStr::of(v));
        }
        let c = match &cwd {
            Some(s) => LathaStr::of(s),
            None => LathaStr { ptr: std::ptr::null(), len: 0 },
        };
        let mut err = LathaOut { data: Vec::new() };
        let id = (self.cb.pty_spawn)(self.cb.user, a.as_ptr(), a.len(), e.as_ptr(), env.len(), c, cols, rows, &mut err);
        if id < 0 {
            Err(out_string(err))
        } else {
            Ok(id as u32)
        }
    }
    fn write(&mut self, id: u32, data: Vec<u8>) {
        (self.cb.pty_write)(self.cb.user, id, data.as_ptr(), data.len())
    }
    fn read(&mut self, id: u32) -> String {
        let mut o = LathaOut { data: Vec::new() };
        (self.cb.pty_read)(self.cb.user, id, &mut o);
        if o.data.is_empty() {
            String::new()
        } else {
            B64.encode(&o.data)
        }
    }
    fn resize(&mut self, id: u32, cols: u16, rows: u16) {
        (self.cb.pty_resize)(self.cb.user, id, cols, rows)
    }
    fn exit_code(&mut self, id: u32) -> Option<i32> {
        let mut code = 0i32;
        if (self.cb.pty_exit_code)(self.cb.user, id, &mut code) != 0 {
            Some(code)
        } else {
            None
        }
    }
    fn close(&mut self, id: u32) {
        (self.cb.pty_close)(self.cb.user, id)
    }
}

// --- the app ---------------------------------------------------------------------------

pub struct LathaApp {
    store: Store<Host>,
    bindings: GuiPty,
    moves: Vec<LathaMove>,
    rects: Vec<LathaRect>,
    error: Vec<u8>, // NUL terminated
    dead: bool,
    has_pty: bool,
    scratch: Vec<u8>,
}

fn cache_dir() -> Option<PathBuf> {
    if let Some(d) = std::env::var_os("LATHA_GUI_CACHE_DIR") {
        return Some(PathBuf::from(d));
    }
    if let Some(d) = std::env::var_os("XDG_CACHE_HOME") {
        if !d.is_empty() {
            return Some(PathBuf::from(d).join("latha-gui"));
        }
    }
    std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".cache").join("latha-gui"))
}

/// Compiled code is cached by (our version, the engine's settings, the
/// bytes): ~seconds to compile a Qt app, milliseconds to load it again.
fn load_component(engine: &Engine, bytes: &[u8], use_cache: bool) -> Result<Component> {
    use sha2::{Digest, Sha256};
    use std::hash::{Hash, Hasher};
    let cached = if use_cache { cache_dir() } else { None };
    let path = cached.map(|dir| {
        struct H<'a>(&'a mut Sha256);
        impl Hasher for H<'_> {
            fn finish(&self) -> u64 {
                0
            }
            fn write(&mut self, b: &[u8]) {
                self.0.update(b)
            }
        }
        let mut h = Sha256::new();
        h.update(env!("CARGO_PKG_VERSION").as_bytes());
        engine.precompile_compatibility_hash().hash(&mut H(&mut h));
        h.update(bytes);
        let key: String = h.finalize().iter().map(|b| format!("{b:02x}")).collect();
        dir.join(format!("{key}.cwasm"))
    });
    if let Some(p) = &path {
        if p.exists() {
            // SAFETY: we wrote this file with Component::serialize for an
            // engine with the same compatibility hash (it is in the key).
            match unsafe { Component::deserialize_file(engine, p) } {
                Ok(c) => return Ok(c),
                Err(_) => {
                    let _ = std::fs::remove_file(p);
                }
            }
        }
    }
    let component = Component::new(engine, bytes)?;
    if let Some(p) = &path {
        if let Ok(data) = component.serialize() {
            if let Some(dir) = p.parent() {
                let _ = std::fs::create_dir_all(dir);
            }
            let tmp = p.with_extension(format!("tmp{}", std::process::id()));
            if std::fs::write(&tmp, data).is_ok() {
                let _ = std::fs::rename(&tmp, p);
            }
        }
    }
    Ok(component)
}

fn imports_pty(component: &Component, engine: &Engine) -> bool {
    component
        .component_type()
        .imports(engine)
        .any(|(name, _)| name.starts_with("latha:gui/pty"))
}

fn load(path: &Path, opts: Option<&LathaOptions>, cb: LathaCallbacks, args: &[String]) -> Result<LathaApp> {
    let bytes = std::fs::read(path).with_context(|| format!("reading {}", path.display()))?;
    if bytes.len() >= 8 && bytes[0..4] == *b"\0asm" && bytes[4] == 1 && bytes[6] == 0 {
        return Err(format_err!(
            "{} is a core module, not a component (wrap it with `wasm-tools component new`)",
            path.display()
        ));
    }
    let mut config = Config::new();
    config.wasm_component_model(true);
    config.parallel_compilation(true);
    let engine = Engine::new(&config)?;
    let use_cache = opts.map_or(true, |o| o.no_cache == 0) && std::env::var("LATHA_GUI_CACHE").as_deref() != Ok("0");
    let component = load_component(&engine, &bytes, use_cache)?;
    let has_pty = imports_pty(&component, &engine);

    let mut linker: Linker<Host> = Linker::new(&engine);
    wasmtime_wasi::p2::add_to_linker_sync(&mut linker)?;
    GuiPty::add_to_linker::<Host, HasSelf<Host>>(&mut linker, |h| h)?;

    let mut wasi = WasiCtxBuilder::new();
    wasi.inherit_stdio().args(args);
    // Our environment, minus Qt's own settings: they are for this window,
    // not for the app's Qt (QT_QPA_PLATFORM=offscreen would kill it).
    for (k, v) in std::env::vars() {
        if !(k.starts_with("QT_") || k.starts_with("QML") || k.starts_with("QSG_")) {
            wasi.env(&k, &v);
        }
    }
    if let Some(o) = opts {
        for i in 0..o.ndirs {
            let host = unsafe { str_of(&*o.dirs.add(i)) };
            let guest = unsafe { str_of(&*o.guest_dirs.add(i)) };
            wasi.preopened_dir(host, guest, FsPerms::ReadWrite)
                .with_context(|| format!("--dir {host}"))?;
        }
    }
    let host = Host { cb, wasi: wasi.build(), table: ResourceTable::new() };
    let mut store = Store::new(&engine, host);
    let bindings = GuiPty::instantiate(&mut store, &component, &linker)?;
    Ok(LathaApp {
        store,
        bindings,
        moves: Vec::new(),
        rects: Vec::new(),
        error: vec![0],
        dead: false,
        has_pty,
        scratch: Vec::new(),
    })
}

unsafe fn str_of(s: &LathaStr) -> &str {
    if s.ptr.is_null() || s.len == 0 {
        return "";
    }
    std::str::from_utf8_unchecked(std::slice::from_raw_parts(s.ptr, s.len))
}

fn set_err(err: *mut *mut c_char, msg: &str) {
    if err.is_null() {
        return;
    }
    let mut v = msg.replace('\0', " ").into_bytes();
    v.push(0);
    let b = v.into_boxed_slice();
    unsafe { *err = Box::into_raw(b) as *mut c_char };
}

impl LathaApp {
    /// Run one export call; a trap kills the instance (every later call
    /// fails at once) and leaves its message in latha_error.
    fn call<R>(&mut self, f: impl FnOnce(&wapp::Guest, &mut Store<Host>) -> Result<R>) -> Option<R> {
        if self.dead {
            return None;
        }
        let guest = self.bindings.latha_gui_app() as *const wapp::Guest;
        // SAFETY: `guest` lives in self.bindings, which `f` cannot reach.
        match f(unsafe { &*guest }, &mut self.store) {
            Ok(r) => Some(r),
            Err(e) => {
                self.dead = true;
                let mut v = format!("{e:?}").replace('\0', " ").into_bytes();
                v.push(0);
                self.error = v;
                None
            }
        }
    }
}

fn mods(bits: u32) -> wt::Modifiers {
    let mut m = wt::Modifiers::empty();
    if bits & 1 != 0 {
        m |= wt::Modifiers::SHIFT;
    }
    if bits & 2 != 0 {
        m |= wt::Modifiers::CTRL;
    }
    if bits & 4 != 0 {
        m |= wt::Modifiers::ALT;
    }
    if bits & 8 != 0 {
        m |= wt::Modifiers::META;
    }
    if bits & 16 != 0 {
        m |= wt::Modifiers::KEYPAD;
    }
    m
}

fn buttons(bits: u32) -> wt::Buttons {
    let mut b = wt::Buttons::empty();
    if bits & 1 != 0 {
        b |= wt::Buttons::LEFT;
    }
    if bits & 2 != 0 {
        b |= wt::Buttons::RIGHT;
    }
    if bits & 4 != 0 {
        b |= wt::Buttons::MIDDLE;
    }
    if bits & 8 != 0 {
        b |= wt::Buttons::BACK;
    }
    if bits & 16 != 0 {
        b |= wt::Buttons::FORWARD;
    }
    b
}

// --- C API -----------------------------------------------------------------------------

/// Load and instantiate `path` (a component for latha:gui or latha:gui-pty).
/// `args` (the app's name first) become its WASI arguments. NULL on error,
/// with a message in `*err` (free it with latha_string_free).
#[no_mangle]
pub unsafe extern "C" fn latha_load(
    path: *const c_char,
    opts: *const LathaOptions,
    cb: *const LathaCallbacks,
    args: *const LathaStr,
    nargs: usize,
    err: *mut *mut c_char,
) -> *mut LathaApp {
    let p = CStr::from_ptr(path).to_string_lossy().into_owned();
    let a: Vec<String> = (0..nargs).map(|i| str_of(&*args.add(i)).to_owned()).collect();
    let cb = std::ptr::read(cb);
    match load(Path::new(&p), opts.as_ref(), cb, &a) {
        Ok(app) => Box::into_raw(Box::new(app)),
        Err(e) => {
            set_err(err, &format!("{e:#}"));
            std::ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn latha_free(app: *mut LathaApp) {
    if !app.is_null() {
        drop(Box::from_raw(app));
    }
}

#[no_mangle]
pub unsafe extern "C" fn latha_string_free(s: *mut c_char) {
    if !s.is_null() {
        let len = CStr::from_ptr(s).to_bytes().len();
        drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(s as *mut u8, len + 1)));
    }
}

/// Fill a callback's out parameter.
#[no_mangle]
pub unsafe extern "C" fn latha_out_set(out: *mut LathaOut, data: *const u8, len: usize) {
    let o = &mut *out;
    o.data.clear();
    if len > 0 {
        o.data.extend_from_slice(std::slice::from_raw_parts(data, len));
    }
}

/// Whether the component imports latha:gui/pty.
#[no_mangle]
pub unsafe extern "C" fn latha_has_pty(app: *const LathaApp) -> c_int {
    (*app).has_pty as c_int
}

/// The last trap's message ("" if none). Once an app trapped, it is dead.
#[no_mangle]
pub unsafe extern "C" fn latha_error(app: *const LathaApp) -> *const c_char {
    (*app).error.as_ptr() as *const c_char
}

/// 0: ok with the wished size; 1: the app's own error in `*err`; -1: trap.
#[no_mangle]
pub unsafe extern "C" fn latha_start(
    app: *mut LathaApp,
    args: *const LathaStr,
    nargs: usize,
    w: *mut u32,
    h: *mut u32,
    err: *mut *mut c_char,
) -> c_int {
    let a = &mut *app;
    let v: Vec<String> = (0..nargs).map(|i| str_of(&*args.add(i)).to_owned()).collect();
    match a.call(|g, s| g.call_start(s, &v)) {
        Some(Ok(size)) => {
            *w = size.width;
            *h = size.height;
            0
        }
        Some(Err(msg)) => {
            set_err(err, &msg);
            1
        }
        None => -1,
    }
}

macro_rules! ok {
    ($e:expr) => {
        if $e.is_some() {
            0
        } else {
            -1
        }
    };
}

#[no_mangle]
pub unsafe extern "C" fn latha_resize(app: *mut LathaApp, w: u32, h: u32, scale: f64) -> c_int {
    ok!((*app).call(|g, s| g.call_resize(s, w, h, scale)))
}

#[no_mangle]
pub unsafe extern "C" fn latha_key(
    app: *mut LathaApp,
    pressed: c_int,
    repeat: c_int,
    keysym: u32,
    scancode: u32,
    modifiers: u32,
    text: LathaStr,
) -> c_int {
    let a = &mut *app;
    let ev = wapp::KeyEvent {
        pressed: pressed != 0,
        repeat: repeat != 0,
        keysym,
        scancode,
        modifiers: mods(modifiers),
        text: str_of(&text).to_owned(),
    };
    ok!(a.call(|g, s| g.call_key(s, &ev)))
}

#[no_mangle]
pub unsafe extern "C" fn latha_text(app: *mut LathaApp, text: LathaStr) -> c_int {
    let t = str_of(&text).to_owned();
    ok!((*app).call(|g, s| g.call_text(s, &t)))
}

#[no_mangle]
pub unsafe extern "C" fn latha_pointer(app: *mut LathaApp, p: *const LathaPointer) -> c_int {
    let p = &*p;
    let kind = match p.kind {
        0 => wt::PointerKind::Press,
        1 => wt::PointerKind::Release,
        3 => wt::PointerKind::Enter,
        4 => wt::PointerKind::Leave,
        _ => wt::PointerKind::Move,
    };
    let button = match p.button {
        1 => wt::Button::Left,
        2 => wt::Button::Right,
        3 => wt::Button::Middle,
        4 => wt::Button::Back,
        5 => wt::Button::Forward,
        _ => wt::Button::None,
    };
    let ev = wapp::PointerEvent {
        kind,
        button,
        buttons: buttons(p.buttons),
        modifiers: mods(p.modifiers),
        x: p.x,
        y: p.y,
        clicks: p.clicks,
    };
    ok!((*app).call(|g, s| g.call_pointer(s, ev)))
}

#[no_mangle]
pub unsafe extern "C" fn latha_wheel(app: *mut LathaApp, w: *const LathaWheel) -> c_int {
    let w = &*w;
    let ev = wapp::WheelEvent {
        x: w.x,
        y: w.y,
        steps_x: w.steps_x,
        steps_y: w.steps_y,
        pixels_x: w.pixels_x,
        pixels_y: w.pixels_y,
        modifiers: mods(w.modifiers),
    };
    ok!((*app).call(|g, s| g.call_wheel(s, ev)))
}

#[no_mangle]
pub unsafe extern "C" fn latha_focus(app: *mut LathaApp, focused: c_int) -> c_int {
    ok!((*app).call(|g, s| g.call_focus(s, focused != 0)))
}

#[no_mangle]
pub unsafe extern "C" fn latha_color_scheme(app: *mut LathaApp, dark: c_int) -> c_int {
    ok!((*app).call(|g, s| g.call_color_scheme(s, dark != 0)))
}

/// 1: the host may close; 0: not now; -1: trap.
#[no_mangle]
pub unsafe extern "C" fn latha_close(app: *mut LathaApp) -> c_int {
    match (*app).call(|g, s| g.call_close(s)) {
        Some(true) => 1,
        Some(false) => 0,
        None => -1,
    }
}

/// The ms until the app wants the next tick, -1 for none, -2 on a trap.
#[no_mangle]
pub unsafe extern "C" fn latha_tick(app: *mut LathaApp) -> i64 {
    match (*app).call(|g, s| g.call_tick(s)) {
        Some(Some(ms)) => ms as i64,
        Some(None) => -1,
        None => -2,
    }
}

/// Paint; the damage stays readable through latha_damage until the next
/// paint. 0 ok, -1 trap.
#[no_mangle]
pub unsafe extern "C" fn latha_paint(app: *mut LathaApp) -> c_int {
    let a = &mut *app;
    let Some(d) = a.call(|g, s| g.call_paint(s)) else {
        return -1;
    };
    let r = |r: &wt::Rect| LathaRect { x: r.x, y: r.y, width: r.width, height: r.height };
    a.moves.clear();
    a.moves.extend(d.moves.iter().map(|m| LathaMove { area: r(&m.area), dx: m.dx, dy: m.dy }));
    a.rects.clear();
    a.rects.extend(d.rects.iter().map(r));
    0
}

#[no_mangle]
pub unsafe extern "C" fn latha_damage(
    app: *const LathaApp,
    moves: *mut *const LathaMove,
    nmoves: *mut usize,
    rects: *mut *const LathaRect,
    nrects: *mut usize,
) {
    let a = &*app;
    *moves = a.moves.as_ptr();
    *nmoves = a.moves.len();
    *rects = a.rects.as_ptr();
    *nrects = a.rects.len();
}

#[no_mangle]
pub unsafe extern "C" fn latha_frame_size(app: *mut LathaApp, w: *mut u32, h: *mut u32) -> c_int {
    match (*app).call(|g, s| g.call_frame_size(s)) {
        Some(sz) => {
            *w = sz.width;
            *h = sz.height;
            0
        }
        None => -1,
    }
}

/// Copy the app's pixels of `r` into `dst` (row `y` at `dst + y * stride`),
/// 4 bytes a pixel, premultiplied BGRA. 0 ok, -1 trap, -2 bad data.
#[no_mangle]
pub unsafe extern "C" fn latha_frame_pixels(app: *mut LathaApp, r: LathaRect, dst: *mut u8, stride: usize) -> c_int {
    let a = &mut *app;
    let rect = wt::Rect { x: r.x, y: r.y, width: r.width, height: r.height };
    let Some(s) = a.call(|g, st| g.call_frame_pixels(st, rect)) else {
        return -1;
    };
    let row = r.width as usize * 4;
    let rows = r.height as usize;
    if row == 0 || rows == 0 {
        return 0;
    }
    // One decode into a scratch buffer, then a memcpy a row when the
    // destination is not contiguous.
    if stride == row {
        let out = std::slice::from_raw_parts_mut(dst, row * rows);
        return match B64.decode_slice(s.as_bytes(), out) {
            Ok(n) if n == row * rows => 0,
            _ => -2,
        };
    }
    let mut buf = std::mem::take(&mut a.scratch);
    buf.clear();
    let ok = B64.decode_vec(s.as_bytes(), &mut buf).is_ok() && buf.len() == row * rows;
    if ok {
        for y in 0..rows {
            std::ptr::copy_nonoverlapping(buf.as_ptr().add(y * row), dst.add(y * stride), row);
        }
    }
    buf.clear();
    a.scratch = buf;
    if ok {
        0
    } else {
        -2
    }
}
