// SPDX-FileCopyrightText: 2026 Kwi WebAssembly port contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only
//
// latha:proc (qt2wasm/proc/wit/proc.wit, core ABI in qt2wasm/proc/latha_proc.h):
// wasm "processes" and the channels between them.
//
// A process is a WASI program (a core module with preview 1, or a command
// component) in its own Store on its own native thread, with a big stack
// (wasm frames live on it; Ladybird's interpreter recurses deeply). All
// processes of one run share an Engine, its compiled-module cache and the
// epoch ticker that lets `kill` stop a busy child.
//
// A channel is two Endpoints; a message is bytes + Endpoints. Each process has
// a Bell that every endpoint it owns rings when a message (or a close) arrives
// for it, so `wait` on several handles sleeps on one Condvar. An Endpoint is
// closed when its last reference goes: the handle table entry, or the queued
// message carrying it.

use std::collections::{HashMap, VecDeque};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::{Arc, Condvar, Mutex, OnceLock, Weak};
use std::time::{Duration, Instant};

use wasmtime::component::{Component, HasSelf, ResourceTable};
use wasmtime::error::Context as _;
use wasmtime::{format_err, Caller, Config, Engine, Extern, Linker, Module, Result, Store, UpdateDeadline};
use wasmtime_wasi::p1::WasiP1Ctx;
use wasmtime_wasi::{FsPerms, I32Exit, WasiCtx, WasiCtxBuilder, WasiCtxView, WasiView};

pub mod wit {
    wasmtime::component::bindgen!({
        path: "../../qt2wasm/proc/wit",
        world: "latha:proc/proc",
    });
}
use wit::latha::proc::channels as wch;

pub const MAX_BYTES: usize = 16 << 20;
pub const MAX_HANDLES: usize = 256;
/// The wasm stack of a process (wasmtime's max_wasm_stack). The thread gets
/// this plus room for the host's own frames.
pub const WASM_STACK: usize = 256 << 20;
const THREAD_STACK: usize = WASM_STACK + (64 << 20);
/// Exit code of a process that trapped (as abort(): 128 + SIGABRT).
pub const TRAP_EXIT: i32 = 134;

// --- bells and endpoints --------------------------------------------------------------

/// Rung whenever something arrives for one of a process's endpoints.
pub struct Bell {
    gen: Mutex<u64>,
    cv: Condvar,
    /// Called (from any thread) after a ring: latha-gui wakes its loop.
    hook: Mutex<Option<Box<dyn Fn() + Send + Sync>>>,
}

impl Bell {
    fn new() -> Arc<Bell> {
        Arc::new(Bell { gen: Mutex::new(0), cv: Condvar::new(), hook: Mutex::new(None) })
    }
    fn ring(&self) {
        *self.gen.lock().unwrap() += 1;
        self.cv.notify_all();
        if let Some(h) = &*self.hook.lock().unwrap() {
            h()
        }
    }
    fn generation(&self) -> u64 {
        *self.gen.lock().unwrap()
    }
    /// Sleep until the bell rings after `gen`, or `deadline`. false: timed out.
    fn sleep(&self, gen: u64, deadline: Option<Instant>) -> bool {
        let mut g = self.gen.lock().unwrap();
        while *g == gen {
            match deadline {
                None => g = self.cv.wait(g).unwrap(),
                Some(d) => {
                    let now = Instant::now();
                    if now >= d {
                        return false;
                    }
                    g = self.cv.wait_timeout(g, d - now).unwrap().0;
                }
            }
        }
        true
    }
}

struct Msg {
    data: Vec<u8>,
    handles: Vec<Arc<Endpoint>>,
}

#[derive(Default)]
struct EpState {
    queue: VecDeque<Msg>,
    peer_closed: bool,
}

pub struct Endpoint {
    st: Mutex<EpState>,
    peer: OnceLock<Weak<Endpoint>>,
    owner: Mutex<Weak<Bell>>,
}

impl Endpoint {
    fn ring(&self) {
        let b = self.owner.lock().unwrap().upgrade();
        if let Some(b) = b {
            b.ring()
        }
    }
    fn ready(&self) -> bool {
        let s = self.st.lock().unwrap();
        !s.queue.is_empty() || s.peer_closed
    }
    fn peer(&self) -> Option<Arc<Endpoint>> {
        self.peer.get().and_then(Weak::upgrade)
    }
}

impl Drop for Endpoint {
    fn drop(&mut self) {
        if let Some(p) = self.peer() {
            p.st.lock().unwrap().peer_closed = true;
            p.ring();
        }
    }
}

fn new_pair() -> (Arc<Endpoint>, Arc<Endpoint>) {
    let mk = || Arc::new(Endpoint { st: Mutex::new(EpState::default()), peer: OnceLock::new(), owner: Mutex::new(Weak::new()) });
    let (a, b) = (mk(), mk());
    let _ = a.peer.set(Arc::downgrade(&b));
    let _ = b.peer.set(Arc::downgrade(&a));
    (a, b)
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    Closed,
    Timeout,
    BadHandle,
    TooBig,
}

impl Error {
    fn code(self) -> i32 {
        match self {
            Error::Closed => -1,
            Error::Timeout => -2,
            Error::BadHandle => -3,
            Error::TooBig => -4,
        }
    }
}
const E_SPAWN: i32 = -5;
const E_FAULT: i32 = -6;

fn deadline(timeout_ms: i32) -> Option<Instant> {
    (timeout_ms >= 0).then(|| Instant::now() + Duration::from_millis(timeout_ms as u64))
}

// --- the shared runtime -----------------------------------------------------------------

/// What every process of a run gets: directories, environment, network.
#[derive(Clone, Default)]
pub struct RunOptions {
    /// (host, guest) directories, read-write.
    pub dirs: Vec<(String, String)>,
    pub env: Vec<(String, String)>,
    pub network: bool,
    pub cache: bool,
}

#[derive(Clone)]
enum Program {
    Core(Module),
    Command(Component),
}

pub struct Runtime {
    engine: Engine,
    opts: RunOptions,
    programs: Mutex<HashMap<PathBuf, Program>>,
    next_pid: AtomicU32,
}

/// The engine settings every latha-gui engine uses: what wasi-sdk 27 emits for
/// webby (setjmp/longjmp as wasm exceptions with exnref, musttail as
/// return_call, SIMD).
pub fn base_config() -> Config {
    let mut c = Config::new();
    c.parallel_compilation(true);
    c.wasm_tail_call(true);
    c.wasm_simd(true);
    c.wasm_function_references(true);
    c.wasm_gc(true);
    c.wasm_exceptions(true);
    c
}

impl Runtime {
    pub fn new(opts: RunOptions) -> Result<Arc<Runtime>> {
        let mut config = base_config();
        config.wasm_component_model(true);
        // wasmtime-wasi turns on `async`, which needs async_stack_size >= max_wasm_stack
        // (we never run async; the size only matters for fibers).
        config.async_stack_size(WASM_STACK + (1 << 20));
        config.max_wasm_stack(WASM_STACK);
        config.epoch_interruption(true);
        let engine = Engine::new(&config)?;
        // The clock `kill` is checked on: stores trap at the next epoch once
        // their kill flag is set (a cheap check at loop heads and calls).
        let weak = engine.weak();
        std::thread::Builder::new()
            .name("latha-epoch".into())
            .spawn(move || loop {
                std::thread::sleep(Duration::from_millis(10));
                match weak.upgrade() {
                    Some(e) => e.increment_epoch(),
                    None => break,
                }
            })?;
        Ok(Arc::new(Runtime { engine, opts, programs: Mutex::new(HashMap::new()), next_pid: AtomicU32::new(1) }))
    }

    fn program(&self, path: &Path) -> Result<Program> {
        let key = path.canonicalize().unwrap_or_else(|_| path.to_path_buf());
        if let Some(p) = self.programs.lock().unwrap().get(&key) {
            return Ok(p.clone());
        }
        let bytes = std::fs::read(path).with_context(|| format!("reading {}", path.display()))?;
        let p = match wasm_kind(&bytes) {
            Some(WasmKind::Core) => Program::Core(crate::load_cached(
                &self.engine,
                &bytes,
                self.opts.cache,
                |e, f| unsafe { Module::deserialize_file(e, f) },
                |e, b| Module::new(e, b),
                |m| m.serialize(),
            )?),
            Some(WasmKind::Component) => Program::Command(crate::load_cached(
                &self.engine,
                &bytes,
                self.opts.cache,
                |e, f| unsafe { Component::deserialize_file(e, f) },
                |e, b| Component::new(e, b),
                |c| c.serialize(),
            )?),
            None => return Err(format_err!("{}: not a wasm file", path.display())),
        };
        self.programs.lock().unwrap().insert(key, p.clone());
        Ok(p)
    }

    fn wasi(&self, args: &[String]) -> Result<WasiCtxBuilder> {
        let mut b = WasiCtxBuilder::new();
        b.inherit_stdio().args(args);
        for (k, v) in &self.opts.env {
            b.env(k, v);
        }
        for (host, guest) in &self.opts.dirs {
            b.preopened_dir(host, guest, FsPerms::ReadWrite).with_context(|| format!("--dir {host}"))?;
        }
        if self.opts.network {
            b.inherit_network().allow_ip_name_lookup(true).allow_tcp(true).allow_udp(true);
        }
        Ok(b)
    }

    /// Run `path` as the first process, on its own big-stack thread; returns
    /// its exit code.
    pub fn run_main(self: &Arc<Self>, path: &Path, args: Vec<String>) -> Result<i32> {
        let prog = self.program(path)?;
        let state = ProcState::new(self.clone(), path.to_path_buf(), None, Arc::new(AtomicBool::new(false)));
        let rt = self.clone();
        let t = std::thread::Builder::new()
            .name("latha-main".into())
            .stack_size(THREAD_STACK)
            .spawn(move || rt.run(prog, args, state))?;
        t.join().map_err(|_| format_err!("the program's thread panicked"))
    }

    fn run(&self, prog: Program, args: Vec<String>, state: ProcState) -> i32 {
        let name = args.first().cloned().unwrap_or_default();
        let r = match prog {
            Program::Core(m) => self.run_core(&m, &args, state),
            Program::Command(c) => self.run_command(&c, &args, state),
        };
        match r {
            Ok(code) => code,
            Err(e) => {
                if let Some(x) = e.downcast_ref::<I32Exit>() {
                    return x.0;
                }
                if e.downcast_ref::<Killed>().is_some() {
                    eprintln!("latha: {name}: killed");
                    return TRAP_EXIT;
                }
                eprintln!("latha: {name}: {e:?}");
                TRAP_EXIT
            }
        }
    }

    fn arm_kill<T>(store: &mut Store<T>, kill: Arc<AtomicBool>) {
        store.set_epoch_deadline(1);
        store.epoch_deadline_callback(move |_| {
            if kill.load(Ordering::Relaxed) {
                Err(wasmtime::Error::new(Killed))
            } else {
                Ok(UpdateDeadline::Continue(1))
            }
        });
    }

    fn run_core(&self, module: &Module, args: &[String], state: ProcState) -> Result<i32> {
        let mut linker: Linker<CoreHost> = Linker::new(&self.engine);
        wasmtime_wasi::p1::add_to_linker_sync(&mut linker, |h: &mut CoreHost| &mut h.wasi)?;
        add_core_imports(&mut linker, |h: &mut CoreHost| &mut h.proc)?;
        let kill = state.kill.clone();
        let mut store = Store::new(&self.engine, CoreHost { wasi: self.wasi(args)?.build_p1(), proc: state });
        Self::arm_kill(&mut store, kill);
        let inst = linker.instantiate(&mut store, module)?;
        let start = inst
            .get_typed_func::<(), ()>(&mut store, "_start")
            .context("the module has no _start (not a WASI command)")?;
        start.call(&mut store, ())?;
        Ok(0)
    }

    fn run_command(&self, component: &Component, args: &[String], state: ProcState) -> Result<i32> {
        let mut linker: wasmtime::component::Linker<CompHost> = wasmtime::component::Linker::new(&self.engine);
        wasmtime_wasi::p2::add_to_linker_sync(&mut linker)?;
        add_component_imports(&mut linker, |h: &mut CompHost| &mut h.proc)?;
        let kill = state.kill.clone();
        let mut store =
            Store::new(&self.engine, CompHost { wasi: self.wasi(args)?.build(), table: ResourceTable::new(), proc: state });
        Self::arm_kill(&mut store, kill);
        let cmd = wasmtime_wasi::p2::bindings::sync::Command::instantiate(&mut store, component, &linker)?;
        Ok(match cmd.wasi_cli_run().call_run(&mut store)? {
            Ok(()) => 0,
            Err(()) => 1,
        })
    }
}

/// The trap of a killed process.
#[derive(Debug)]
struct Killed;
impl std::fmt::Display for Killed {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("killed")
    }
}
impl std::error::Error for Killed {}

pub enum WasmKind {
    Core,
    Component,
}

pub fn wasm_kind(bytes: &[u8]) -> Option<WasmKind> {
    if bytes.len() < 8 || bytes[0..4] != *b"\0asm" {
        return None;
    }
    match bytes[4..8] {
        [1, 0, 0, 0] => Some(WasmKind::Core),
        [_, _, 1, 0] => Some(WasmKind::Component),
        _ => None,
    }
}

struct CoreHost {
    wasi: WasiP1Ctx,
    proc: ProcState,
}

struct CompHost {
    wasi: WasiCtx,
    table: ResourceTable,
    proc: ProcState,
}

impl WasiView for CompHost {
    fn ctx(&mut self) -> WasiCtxView<'_> {
        WasiCtxView { ctx: &mut self.wasi, table: &mut self.table }
    }
}

// --- one process's view -----------------------------------------------------------------

struct Child {
    exit: Mutex<Option<i32>>,
    cv: Condvar,
    kill: Arc<AtomicBool>,
    bell: Arc<Bell>,
}

/// latha:proc state of one instance (a process, or latha-gui's app).
pub struct ProcState {
    rt: Arc<Runtime>,
    self_path: PathBuf,
    bell: Arc<Bell>,
    handles: HashMap<u32, Arc<Endpoint>>,
    next: u32,
    parent: Option<Arc<Endpoint>>,
    children: HashMap<u32, Arc<Child>>,
    kill: Arc<AtomicBool>,
}

impl ProcState {
    fn new(rt: Arc<Runtime>, self_path: PathBuf, parent: Option<Arc<Endpoint>>, kill: Arc<AtomicBool>) -> ProcState {
        let mut s = ProcState {
            rt,
            self_path,
            bell: Bell::new(),
            handles: HashMap::new(),
            next: 1,
            parent: None,
            children: HashMap::new(),
            kill,
        };
        if let Some(p) = &parent {
            *p.owner.lock().unwrap() = Arc::downgrade(&s.bell);
        }
        s.parent = parent;
        s
    }

    /// State for latha-gui's own app (the window's component): `wake` runs,
    /// on any thread, whenever a message arrives for it.
    pub fn for_app(rt: Arc<Runtime>, self_path: PathBuf, wake: Box<dyn Fn() + Send + Sync>) -> ProcState {
        let s = ProcState::new(rt, self_path, None, Arc::new(AtomicBool::new(false)));
        *s.bell.hook.lock().unwrap() = Some(wake);
        s
    }

    fn insert(&mut self, ep: Arc<Endpoint>) -> u32 {
        *ep.owner.lock().unwrap() = Arc::downgrade(&self.bell);
        let notify = ep.ready();
        let h = self.next;
        self.next = self.next.wrapping_add(1).max(1);
        self.handles.insert(h, ep);
        if notify {
            self.bell.ring();
        }
        h
    }

    fn get(&self, h: u32) -> Result<Arc<Endpoint>, Error> {
        self.handles.get(&h).cloned().ok_or(Error::BadHandle)
    }

    pub fn pair(&mut self) -> (u32, u32) {
        let (a, b) = new_pair();
        (self.insert(a), self.insert(b))
    }

    pub fn send(&mut self, h: u32, data: Vec<u8>, hs: &[u32]) -> Result<(), Error> {
        if data.len() > MAX_BYTES || hs.len() > MAX_HANDLES {
            return Err(Error::TooBig);
        }
        let ep = self.get(h)?;
        let peer = ep.peer();
        for (i, x) in hs.iter().enumerate() {
            let e = self.get(*x)?;
            // Sending an endpoint along itself, or the peer through its own
            // queue, would keep the channel alive for ever.
            if *x == h || hs[..i].contains(x) || peer.as_ref().is_some_and(|p| Arc::ptr_eq(p, &e)) {
                return Err(Error::BadHandle);
            }
        }
        let moved: Vec<Arc<Endpoint>> = hs.iter().filter_map(|x| self.handles.remove(x)).collect();
        for m in &moved {
            *m.owner.lock().unwrap() = Weak::new();
        }
        let Some(peer) = peer else { return Err(Error::Closed) };
        peer.st.lock().unwrap().queue.push_back(Msg { data, handles: moved });
        peer.ring();
        Ok(())
    }

    /// The next message on `h` if `fits(len, nhandles)`; else TooBig with the
    /// sizes, the message stays queued.
    fn recv_with(
        &mut self,
        h: u32,
        timeout_ms: i32,
        fits: impl Fn(usize, usize) -> bool,
    ) -> Result<(Vec<u8>, Vec<u32>), (Error, usize, usize)> {
        let ep = self.get(h).map_err(|e| (e, 0, 0))?;
        let dl = deadline(timeout_ms);
        loop {
            let gen = self.bell.generation();
            {
                let mut st = ep.st.lock().unwrap();
                if let Some(m) = st.queue.front() {
                    if !fits(m.data.len(), m.handles.len()) {
                        return Err((Error::TooBig, m.data.len(), m.handles.len()));
                    }
                    let m = st.queue.pop_front().unwrap();
                    drop(st);
                    let hs = m.handles.into_iter().map(|e| self.insert(e)).collect();
                    return Ok((m.data, hs));
                }
                if st.peer_closed {
                    return Err((Error::Closed, 0, 0));
                }
            }
            if self.kill.load(Ordering::Relaxed) || !self.bell.sleep(gen, dl) {
                return Err((Error::Timeout, 0, 0));
            }
        }
    }

    pub fn wait(&mut self, hs: &[u32], timeout_ms: i32) -> Vec<u32> {
        let eps: Vec<Option<Arc<Endpoint>>> = hs.iter().map(|h| self.handles.get(h).cloned()).collect();
        let dl = deadline(timeout_ms);
        loop {
            let gen = self.bell.generation();
            // A bad handle counts as ready: reading it says why.
            let ready: Vec<u32> = eps
                .iter()
                .enumerate()
                .filter(|(_, e)| e.as_ref().map_or(true, |e| e.ready()))
                .map(|(i, _)| i as u32)
                .collect();
            if !ready.is_empty() || self.kill.load(Ordering::Relaxed) || !self.bell.sleep(gen, dl) {
                return ready;
            }
        }
    }

    pub fn close(&mut self, h: u32) {
        self.handles.remove(&h);
    }

    pub fn take_parent(&mut self) -> Option<u32> {
        let p = self.parent.take()?;
        Some(self.insert(p))
    }

    pub fn spawn(&mut self, path: &str, args: Vec<String>) -> Result<(u32, u32), String> {
        let path = if path.is_empty() {
            self.self_path.clone()
        } else {
            let p = PathBuf::from(path);
            if p.is_absolute() {
                p
            } else {
                self.self_path.parent().unwrap_or(Path::new(".")).join(p)
            }
        };
        let prog = self.rt.program(&path).map_err(|e| format!("{e:?}"))?;
        let (ours, theirs) = new_pair();
        let kill = Arc::new(AtomicBool::new(false));
        let state = ProcState::new(self.rt.clone(), path.clone(), Some(theirs), kill.clone());
        let child = Arc::new(Child { exit: Mutex::new(None), cv: Condvar::new(), kill, bell: state.bell.clone() });
        let pid = self.rt.next_pid.fetch_add(1, Ordering::Relaxed);
        let args = if args.is_empty() { vec![path.display().to_string()] } else { args };
        let rt = self.rt.clone();
        let c = child.clone();
        std::thread::Builder::new()
            .name(format!("latha-{pid}"))
            .stack_size(THREAD_STACK)
            .spawn(move || {
                let code = rt.run(prog, args, state);
                *c.exit.lock().unwrap() = Some(code);
                c.cv.notify_all();
            })
            .map_err(|e| format!("starting a thread: {e}"))?;
        self.children.insert(pid, child);
        Ok((pid, self.insert(ours)))
    }

    pub fn wait_exit(&mut self, pid: u32, timeout_ms: i32) -> Option<i32> {
        let c = self.children.get(&pid)?.clone();
        let dl = deadline(timeout_ms);
        let mut g = c.exit.lock().unwrap();
        loop {
            if let Some(code) = *g {
                return Some(code);
            }
            match dl {
                None => g = c.cv.wait(g).unwrap(),
                Some(d) => {
                    let now = Instant::now();
                    if now >= d {
                        return None;
                    }
                    g = c.cv.wait_timeout(g, d - now).unwrap().0;
                }
            }
        }
    }

    pub fn kill(&mut self, pid: u32) {
        if let Some(c) = self.children.get(&pid) {
            c.kill.store(true, Ordering::Relaxed);
            c.bell.ring();
        }
    }
}

// --- the component interface ----------------------------------------------------------------

fn wit_err(e: Error) -> wch::Error {
    match e {
        Error::Closed => wch::Error::Closed,
        Error::Timeout => wch::Error::Timeout,
        Error::BadHandle => wch::Error::BadHandle,
        Error::TooBig => wch::Error::TooBig,
    }
}

impl wch::Host for ProcState {
    fn pair(&mut self) -> (u32, u32) {
        ProcState::pair(self)
    }
    fn send(&mut self, h: u32, data: Vec<u8>, handles: Vec<u32>) -> Result<(), wch::Error> {
        ProcState::send(self, h, data, &handles).map_err(wit_err)
    }
    fn recv(&mut self, h: u32, timeout_ms: i32) -> Result<wch::Message, wch::Error> {
        self.recv_with(h, timeout_ms, |_, _| true)
            .map(|(data, handles)| wch::Message { data, handles })
            .map_err(|(e, _, _)| wit_err(e))
    }
    fn wait(&mut self, hs: Vec<u32>, timeout_ms: i32) -> Vec<u32> {
        ProcState::wait(self, &hs, timeout_ms)
    }
    fn close(&mut self, h: u32) {
        ProcState::close(self, h)
    }
    fn parent(&mut self) -> Option<u32> {
        self.take_parent()
    }
}

impl wit::latha::proc::process::Host for ProcState {
    fn spawn(&mut self, path: String, args: Vec<String>) -> Result<(u32, u32), String> {
        ProcState::spawn(self, &path, args)
    }
    fn wait_exit(&mut self, pid: u32, timeout_ms: i32) -> Option<i32> {
        ProcState::wait_exit(self, pid, timeout_ms)
    }
    fn kill(&mut self, pid: u32) {
        ProcState::kill(self, pid)
    }
}

pub fn add_component_imports<T: Send + 'static>(
    linker: &mut wasmtime::component::Linker<T>,
    f: fn(&mut T) -> &mut ProcState,
) -> Result<()> {
    wit::Proc::add_to_linker::<T, HasSelf<ProcState>>(linker, f)
}

// --- the core module interface ("latha:proc", latha_proc.h) -----------------------------------

fn memory<T>(c: &mut Caller<'_, T>) -> Option<wasmtime::Memory> {
    match c.get_export("memory") {
        Some(Extern::Memory(m)) => Some(m),
        _ => None,
    }
}

fn read_bytes<T>(c: &mut Caller<'_, T>, ptr: u32, len: u32) -> Option<Vec<u8>> {
    let m = memory(c)?;
    let d = m.data(&c);
    let (s, e) = (ptr as usize, ptr as usize + len as usize);
    d.get(s..e).map(<[u8]>::to_vec)
}

fn read_u32s<T>(c: &mut Caller<'_, T>, ptr: u32, n: u32) -> Option<Vec<u32>> {
    let b = read_bytes(c, ptr, n.checked_mul(4)?)?;
    Some(b.chunks_exact(4).map(|x| u32::from_le_bytes([x[0], x[1], x[2], x[3]])).collect())
}

fn write_bytes<T>(c: &mut Caller<'_, T>, ptr: u32, data: &[u8]) -> bool {
    let Some(m) = memory(c) else { return false };
    m.write(c, ptr as usize, data).is_ok()
}

fn write_u32s<T>(c: &mut Caller<'_, T>, ptr: u32, v: &[u32]) -> bool {
    let b: Vec<u8> = v.iter().flat_map(|x| x.to_le_bytes()).collect();
    write_bytes(c, ptr, &b)
}

pub fn add_core_imports<T: 'static>(linker: &mut Linker<T>, f: fn(&mut T) -> &mut ProcState) -> Result<()> {
    const M: &str = "latha:proc";
    linker.func_wrap(M, "pair", move |mut c: Caller<'_, T>, out: u32| -> i32 {
        let (a, b) = f(c.data_mut()).pair();
        if write_u32s(&mut c, out, &[a, b]) {
            0
        } else {
            E_FAULT
        }
    })?;
    linker.func_wrap(
        M,
        "send",
        move |mut c: Caller<'_, T>, h: u32, data: u32, len: u32, hs: u32, nh: u32| -> i32 {
            if len as usize > MAX_BYTES || nh as usize > MAX_HANDLES {
                return Error::TooBig.code();
            }
            let (Some(d), Some(hv)) = (read_bytes(&mut c, data, len), read_u32s(&mut c, hs, nh)) else {
                return E_FAULT;
            };
            match f(c.data_mut()).send(h, d, &hv) {
                Ok(()) => 0,
                Err(e) => e.code(),
            }
        },
    )?;
    linker.func_wrap(
        M,
        "recv",
        move |mut c: Caller<'_, T>,
              h: u32,
              buf: u32,
              cap: u32,
              hbuf: u32,
              hcap: u32,
              out_len: u32,
              out_nh: u32,
              timeout_ms: i32|
              -> i32 {
            let r = f(c.data_mut()).recv_with(h, timeout_ms, |l, n| l <= cap as usize && n <= hcap as usize);
            match r {
                Ok((data, hs)) => {
                    let ok = write_bytes(&mut c, buf, &data)
                        && write_u32s(&mut c, hbuf, &hs)
                        && write_u32s(&mut c, out_len, &[data.len() as u32])
                        && write_u32s(&mut c, out_nh, &[hs.len() as u32]);
                    // A fault after the message left the queue loses it; the
                    // program is broken anyway.
                    if ok {
                        0
                    } else {
                        E_FAULT
                    }
                }
                Err((e, l, n)) => {
                    if e == Error::TooBig
                        && !(write_u32s(&mut c, out_len, &[l as u32]) && write_u32s(&mut c, out_nh, &[n as u32]))
                    {
                        return E_FAULT;
                    }
                    e.code()
                }
            }
        },
    )?;
    linker.func_wrap(M, "wait", move |mut c: Caller<'_, T>, hs: u32, n: u32, ready: u32, timeout_ms: i32| -> i32 {
        let Some(hv) = read_u32s(&mut c, hs, n) else { return E_FAULT };
        let r = f(c.data_mut()).wait(&hv, timeout_ms);
        let mut flags = vec![0u8; n as usize];
        for i in &r {
            flags[*i as usize] = 1;
        }
        if write_bytes(&mut c, ready, &flags) {
            r.len() as i32
        } else {
            E_FAULT
        }
    })?;
    linker.func_wrap(M, "close", move |mut c: Caller<'_, T>, h: u32| -> i32 {
        let s = f(c.data_mut());
        if s.handles.contains_key(&h) {
            s.close(h);
            0
        } else {
            Error::BadHandle.code()
        }
    })?;
    linker.func_wrap(M, "parent", move |mut c: Caller<'_, T>, out: u32| -> i32 {
        match f(c.data_mut()).take_parent() {
            Some(h) => {
                if write_u32s(&mut c, out, &[h]) {
                    0
                } else {
                    E_FAULT
                }
            }
            None => Error::BadHandle.code(),
        }
    })?;
    linker.func_wrap(
        M,
        "spawn",
        move |mut c: Caller<'_, T>, path: u32, plen: u32, args: u32, alen: u32, out_pid: u32, out_h: u32| -> i32 {
            let (Some(p), Some(a)) = (read_bytes(&mut c, path, plen), read_bytes(&mut c, args, alen)) else {
                return E_FAULT;
            };
            let p = String::from_utf8_lossy(&p).into_owned();
            let mut argv: Vec<String> =
                a.split(|b| *b == 0).map(|s| String::from_utf8_lossy(s).into_owned()).collect();
            if a.last() == Some(&0) {
                argv.pop(); // the split after the last NUL
            }
            if a.is_empty() {
                argv.clear();
            }
            match f(c.data_mut()).spawn(&p, argv) {
                Ok((pid, h)) => {
                    if write_u32s(&mut c, out_pid, &[pid]) && write_u32s(&mut c, out_h, &[h]) {
                        0
                    } else {
                        E_FAULT
                    }
                }
                Err(e) => {
                    eprintln!("latha: spawn {p}: {e}");
                    E_SPAWN
                }
            }
        },
    )?;
    linker.func_wrap(M, "wait_exit", move |mut c: Caller<'_, T>, pid: u32, timeout_ms: i32, out: u32| -> i32 {
        match f(c.data_mut()).wait_exit(pid, timeout_ms) {
            Some(code) => {
                if write_u32s(&mut c, out, &[code as u32]) {
                    0
                } else {
                    E_FAULT
                }
            }
            None => Error::Timeout.code(),
        }
    })?;
    linker.func_wrap(M, "kill", move |mut c: Caller<'_, T>, pid: u32| -> i32 {
        f(c.data_mut()).kill(pid);
        0
    })?;
    Ok(())
}
