// SPDX-FileCopyrightText: 2026 Kwi WebAssembly port contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only
//
// Precompiled apps: compiling a big component (webby, ~200 MB of wasm) takes Cranelift 30-50 s of every core the
// first time it runs. `latha-gui --precompile APP.wasm DIR` compiles it once, for a portable CPU baseline
// (x86-64-v3 / plain aarch64), into DIR/<key>.cwasm.zst; a release puts that file in a prebuilt store, and the
// first latha_load (or latha_prepare) of the same app fetches it into the cache instead of compiling: about a
// second for the download and none for the compile.
//
// The key is (this host's version, the portable engine's compatibility hash, the app's SHA-256): a precompiled
// file only ever matches the exact app bytes and a latha-gui with the same wasmtime and settings. A CPU without
// one of the baseline's features fails wasmtime's check at load, and the app is compiled here as before.
//
// Trust: a .cwasm is native code, not sandboxed wasm, so it only ever comes from the stores in
// LATHA_GUI_PREBUILT_URL (default: the release stores latha-gui itself is published next to), never from a URL
// or file the app names. LATHA_GUI_PREBUILT=0 turns fetching off.

use sha2::{Digest, Sha256};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;
use wasmtime::{format_err, Engine, Result};

/// Stage, done, total. Stages: 1 downloading (bytes), 2 compiling (functions, total an estimate).
pub type Progress<'a> = &'a (dyn Fn(u32, u64, u64) + Sync);

pub const STAGE_DOWNLOAD: u32 = 1;
pub const STAGE_COMPILE: u32 = 2;

/// Functions Cranelift started on (FunctionCache::get counts them).
pub static FUNCTIONS_DONE: AtomicUsize = AtomicUsize::new(0);

const DEFAULT_STORES: &str = "https://fileapi.fastapicloud.dev/api/forge/repos/nandi.uk/qt2wasm/releases/prebuilt/assets/ \
     https://fileapi.fastapicloud.dev/api/forge/repos/nandi.uk/webby/releases/wasm-engine/assets/";

/// SHA-256 of an app, remembered for the last few byte slices hashed (latha_needs_prepare, latha_prepare and
/// latha_load each hash the same 200 MB otherwise). Keyed by length and a sample, then checked in full only
/// by hashing: a hit needs the same length and the same first/last 64 KiB.
pub fn digest(bytes: &[u8]) -> [u8; 32] {
    static MEMO: Mutex<Vec<(u64, [u8; 32], [u8; 32])>> = Mutex::new(Vec::new());
    let sample = {
        let mut h = Sha256::new();
        let n = bytes.len().min(1 << 16);
        h.update(&bytes[..n]);
        h.update(&bytes[bytes.len() - n..]);
        h.update((bytes.len() as u64).to_le_bytes());
        <[u8; 32]>::from(h.finalize())
    };
    let len = bytes.len() as u64;
    if bytes.len() > (1 << 20) {
        if let Some(d) = MEMO.lock().unwrap().iter().find(|e| e.0 == len && e.1 == sample).map(|e| e.2) {
            return d;
        }
    }
    let d: [u8; 32] = Sha256::digest(bytes).into();
    if bytes.len() > (1 << 20) {
        let mut m = MEMO.lock().unwrap();
        if m.len() >= 4 {
            m.remove(0);
        }
        m.push((len, sample, d));
    }
    d
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

/// (our version, the engine's settings, the app's digest) -> hex key.
pub fn engine_key(engine: &Engine, digest: &[u8; 32]) -> String {
    use std::hash::{Hash, Hasher};
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
    h.update(digest);
    hex(&h.finalize())
}

/// The window-app engine for a portable CPU baseline, the same settings as latha_load's otherwise.
pub fn portable_engine() -> Result<Engine> {
    let mut c = crate::app_config(false);
    c.target(&target_lexicon::Triple::host().to_string())?;
    if cfg!(target_arch = "x86_64") {
        // x86-64-v3 (Haswell, 2013, and every AMD since Excavator): AVX2, BMI, FMA, LZCNT, POPCNT.
        for f in [
            "has_sse3", "has_ssse3", "has_sse41", "has_sse42", "has_popcnt", "has_avx", "has_avx2", "has_fma",
            "has_bmi1", "has_bmi2", "has_lzcnt", "has_cmpxchg16b",
        ] {
            unsafe {
                c.cranelift_flag_enable(f);
            }
        }
    }
    Engine::new(&c)
}

/// The prebuilt store's file name for an app.
pub fn prebuilt_name(bytes: &[u8]) -> Result<String> {
    Ok(format!("{}.cwasm.zst", engine_key(&portable_engine()?, &digest(bytes))))
}

/// Compile `wasm` for the portable baseline into DIR/<key>.cwasm.zst; returns that path.
pub fn precompile(wasm: &Path, dir: &Path) -> Result<PathBuf> {
    let bytes = std::fs::read(wasm).map_err(|e| format_err!("{}: {e}", wasm.display()))?;
    let engine = portable_engine()?;
    let out = dir.join(format!("{}.cwasm.zst", engine_key(&engine, &digest(&bytes))));
    let t = std::time::Instant::now();
    let data = match crate::procs::wasm_kind(&bytes) {
        Some(crate::procs::WasmKind::Core) => engine.precompile_module(&bytes)?,
        _ => engine.precompile_component(&bytes)?,
    };
    eprintln!("latha-gui: compiled {} in {:.1} s ({} MB)", wasm.display(), t.elapsed().as_secs_f64(), data.len() >> 20);
    std::fs::create_dir_all(dir)?;
    let tmp = out.with_extension("tmp");
    {
        let f = std::io::BufWriter::new(std::fs::File::create(&tmp)?);
        let mut z = zstd::stream::write::Encoder::new(f, 19)?;
        let threads = std::thread::available_parallelism().map_or(1, |n| n.get()) as u32;
        let _ = z.multithread(threads);
        let _ = z.long_distance_matching(true);
        let _ = z.window_log(27);
        z.write_all(&data)?;
        z.finish()?.flush()?;
    }
    std::fs::rename(&tmp, &out)?;
    Ok(out)
}

fn stores() -> Vec<String> {
    let v = std::env::var("LATHA_GUI_PREBUILT_URL").unwrap_or_else(|_| DEFAULT_STORES.to_string());
    v.split_whitespace()
        .map(|s| if s.ends_with('/') { s.to_string() } else { format!("{s}/") })
        .collect()
}

fn debug() -> bool {
    std::env::var_os("LATHA_GUI_DEBUG").is_some_and(|v| v != "0")
}

/// Bytes the asset at `url` has (HEAD), or None when the store does not have it (or curl is missing).
fn head(url: &str) -> Option<u64> {
    let out = std::process::Command::new("curl")
        .args(["-fsSIL", "--connect-timeout", "5", "--max-time", "15", url])
        .stderr(std::process::Stdio::null())
        .output()
        .ok()?;
    if !out.status.success() {
        return None;
    }
    // With redirects there is one header block per hop: the last length wins.
    let text = String::from_utf8_lossy(&out.stdout);
    let mut len = None;
    for line in text.lines() {
        if let Some((k, v)) = line.split_once(':') {
            if k.trim().eq_ignore_ascii_case("content-length") {
                len = v.trim().parse::<u64>().ok();
            }
        }
    }
    Some(len.unwrap_or(0))
}

/// Download `url` (zstd) into `dest`, decompressed, reporting progress.
fn download(url: &str, total: u64, dest: &Path, progress: Progress<'_>) -> Result<()> {
    struct Counting<'a, R> {
        inner: R,
        done: u64,
        total: u64,
        last: u64,
        progress: Progress<'a>,
    }
    impl<R: Read> Read for Counting<'_, R> {
        fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
            let n = self.inner.read(buf)?;
            self.done += n as u64;
            if self.done - self.last >= (1 << 20) || n == 0 {
                self.last = self.done;
                (self.progress)(STAGE_DOWNLOAD, self.done, self.total);
            }
            Ok(n)
        }
    }
    let mut child = std::process::Command::new("curl")
        .args(["-fsSL", "--connect-timeout", "10", "--speed-time", "30", "--speed-limit", "1024", url])
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::null())
        .spawn()?;
    let stdout = child.stdout.take().ok_or_else(|| format_err!("curl: no stdout"))?;
    let reader = Counting {
        inner: stdout,
        done: 0,
        total,
        last: 0,
        progress,
    };
    let tmp = dest.with_extension(format!("dl{}", std::process::id()));
    let res = (|| -> Result<()> {
        if let Some(d) = dest.parent() {
            std::fs::create_dir_all(d)?;
        }
        let mut z = zstd::stream::read::Decoder::new(reader)?;
        let _ = z.window_log_max(31);
        let mut f = std::io::BufWriter::new(std::fs::File::create(&tmp)?);
        std::io::copy(&mut z, &mut f)?;
        f.flush()?;
        Ok(())
    })();
    let status = child.wait()?;
    if let Err(e) = res {
        let _ = std::fs::remove_file(&tmp);
        return Err(e);
    }
    if !status.success() {
        let _ = std::fs::remove_file(&tmp);
        return Err(format_err!("curl {url}: {status}"));
    }
    std::fs::rename(&tmp, dest)?;
    Ok(())
}

/// Put the precompiled `bytes` at `dest` (the native cache path) from a prebuilt store; false if no store has it.
pub fn fetch(bytes: &[u8], dest: &Path, progress: Progress<'_>) -> bool {
    if std::env::var("LATHA_GUI_PREBUILT").as_deref() == Ok("0") {
        return false;
    }
    // Only for apps where it pays off: compiling a small one is quicker than asking a server.
    if bytes.len() < (16 << 20) {
        return false;
    }
    let Ok(name) = prebuilt_name(bytes) else { return false };
    for store in stores() {
        let url = format!("{store}{name}");
        let Some(total) = head(&url) else {
            if debug() {
                eprintln!("latha-gui: no precompiled app at {url}");
            }
            continue;
        };
        let t = std::time::Instant::now();
        match download(&url, total, dest, progress) {
            Ok(()) => {
                if debug() {
                    eprintln!("latha-gui: fetched {url} in {:.1} s", t.elapsed().as_secs_f64());
                }
                return true;
            }
            Err(e) => eprintln!("latha-gui: {url}: {e:#}; compiling the app here"),
        }
    }
    false
}

/// Number of functions a component's (or module's) code sections hold: the compile progress bar's total.
pub fn count_functions(bytes: &[u8]) -> u64 {
    let mut n = 0u64;
    for payload in wasmparser::Parser::new(0).parse_all(bytes) {
        match payload {
            Ok(wasmparser::Payload::CodeSectionStart { count, .. }) => n += count as u64,
            Ok(_) => {}
            Err(_) => break,
        }
    }
    n
}

/// Run `compile`, reporting how many functions Cranelift has started on every 100 ms.
pub fn compile_with_progress<T>(bytes: &[u8], progress: Progress<'_>, compile: impl FnOnce() -> T) -> T {
    let total = count_functions(bytes);
    let start = FUNCTIONS_DONE.load(Ordering::Relaxed);
    let stop = std::sync::atomic::AtomicBool::new(false);
    std::thread::scope(|s| {
        s.spawn(|| {
            progress(STAGE_COMPILE, 0, total);
            while !stop.load(Ordering::Relaxed) {
                std::thread::sleep(std::time::Duration::from_millis(100));
                let done = (FUNCTIONS_DONE.load(Ordering::Relaxed) - start) as u64;
                progress(STAGE_COMPILE, done.min(total), total);
            }
        });
        let r = compile();
        stop.store(true, Ordering::Relaxed);
        r
    })
}
