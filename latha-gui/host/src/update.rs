// SPDX-FileCopyrightText: 2026 Kwi WebAssembly port contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only
//
// App updates: an app that says where its newer builds are gets them, for any app and any loader (latha-gui,
// latha-android), with nothing in its WIT.
//
// The app's `latha:app` custom section (see ../appmeta.h) carries, next to id= and name=:
//
//   version=  its version: dot/dash separated parts, numbers compared as numbers (2026.11.12.1730, 1.4.0-2)
//   update=   https URL of its update manifest
//
// The manifest is UTF-8 `key=value` lines:
//
//   version=  the newest build's version
//   url=      the build (absolute, or relative to the manifest); a name ending .zst is zstd and unpacked
//   sha256=   SHA-256 (hex) of the unpacked .wasm
//
// check() (at most once per LATHA_GUI_UPDATE_INTERVAL seconds, default 6 h, per app) reads the manifest of the
// newest copy it has of the app; when its version is newer it downloads the build into the update store
// (<store>/<id>/<version>.wasm), checks the SHA-256, that it is wasm, and that its own latha:app has the same id
// and the manifest's version, and records it in <store>/<id>/current. resolve() is what a loader runs: the store's
// copy when it is newer than the file it was given (a package's read-only file stays as installed; a newer package
// wins over an older download), else that file. So an update is used from the next start on.
//
// Trust: the update is the same app from the place the app itself names, over https (http only to localhost, for
// tests), and must match the SHA-256 the manifest gives; it runs sandboxed like the first one did, with whatever
// the loader gives apps. Precompiled code (prebuilt.rs) never comes this way. LATHA_GUI_UPDATE=0 turns both
// resolve and check off; LATHA_GUI_UPDATE_DIR moves the store (default $XDG_DATA_HOME/latha-gui/updates).

use sha2::{Digest, Sha256};
use std::cmp::Ordering;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};
use wasmtime::{bail, format_err, Result};

/// What an app's latha:app section says (the fields updates use).
#[derive(Debug, Default, Clone, PartialEq)]
pub struct AppInfo {
    pub id: String,
    pub name: String,
    pub version: String,
    pub update: String,
}

fn leb<R: Read>(r: &mut R) -> Option<u64> {
    let mut v = 0u64;
    for shift in (0..64).step_by(7) {
        let mut b = [0u8];
        r.read_exact(&mut b).ok()?;
        v |= u64::from(b[0] & 0x7f) << shift;
        if b[0] & 0x80 == 0 {
            return Some(v);
        }
    }
    None
}

/// The top-level `latha:app` section of a .wasm (module or component), read by seeking past the other sections.
/// None when the file is not wasm; empty fields when it has no section.
pub fn read_app_info(path: &Path) -> Option<AppInfo> {
    let f = std::fs::File::open(path).ok()?;
    let size = f.metadata().ok()?.len();
    let mut r = std::io::BufReader::with_capacity(1 << 12, f);
    let mut head = [0u8; 8];
    r.read_exact(&mut head).ok()?;
    if &head[..4] != b"\0asm" {
        return None;
    }
    let mut info = AppInfo::default();
    let mut pos = 8u64;
    while pos < size {
        let mut id = [0u8];
        if r.read_exact(&mut id).is_err() {
            break;
        }
        let Some(len) = leb(&mut r) else { break };
        let body = r.stream_position().ok()?;
        if len > size - body {
            break;
        }
        pos = body + len;
        if id[0] == 0 && len < (1 << 20) {
            let mut b = vec![0u8; len as usize];
            if r.read_exact(&mut b).is_err() {
                break;
            }
            let mut c = std::io::Cursor::new(&b[..]);
            let Some(nlen) = leb(&mut c) else { continue };
            let start = c.position() as usize;
            let Some(name) = b.get(start..start + nlen as usize) else { continue };
            if name == b"latha:app" {
                parse_lines(&b[start + nlen as usize..], |k, v| match k {
                    "id" => info.id = v.into(),
                    "name" => info.name = v.into(),
                    "version" => info.version = v.into(),
                    "update" => info.update = v.into(),
                    _ => {}
                });
            }
        } else if r.seek(SeekFrom::Start(pos)).is_err() {
            break;
        }
    }
    // Same rule as appmeta.cpp: an id names files and desktop entries.
    if !info.id.bytes().all(|c| c.is_ascii_alphanumeric() || matches!(c, b'.' | b'-' | b'_')) || info.id.starts_with('.') {
        info.id.clear();
    }
    Some(info)
}

fn parse_lines(text: &[u8], mut f: impl FnMut(&str, &str)) {
    for line in String::from_utf8_lossy(text).lines() {
        if let Some((k, v)) = line.split_once('=') {
            if !k.trim().is_empty() {
                f(k.trim(), v.trim());
            }
        }
    }
}

/// Compare versions: parts split at anything not a letter or digit, digit runs compared as numbers,
/// letters as text; a longer version with the same start is newer.
pub fn compare_versions(a: &str, b: &str) -> Ordering {
    fn parts(s: &str) -> Vec<&str> {
        let mut v = Vec::new();
        let mut start = None;
        let mut digit = false;
        for (i, c) in s.char_indices() {
            if c.is_ascii_alphanumeric() {
                let d = c.is_ascii_digit();
                match start {
                    Some(st) if d != digit => {
                        v.push(&s[st..i]);
                        start = Some(i);
                    }
                    None => start = Some(i),
                    _ => {}
                }
                digit = d;
            } else if let Some(st) = start.take() {
                v.push(&s[st..i]);
            }
        }
        if let Some(st) = start {
            v.push(&s[st..]);
        }
        v
    }
    let (pa, pb) = (parts(a), parts(b));
    for (x, y) in pa.iter().zip(pb.iter()) {
        let o = match (x.parse::<u128>(), y.parse::<u128>()) {
            (Ok(m), Ok(n)) => m.cmp(&n),
            (Ok(_), Err(_)) => Ordering::Greater,
            (Err(_), Ok(_)) => Ordering::Less,
            _ => x.cmp(y),
        };
        if o != Ordering::Equal {
            return o;
        }
    }
    pa.len().cmp(&pb.len())
}

fn enabled() -> bool {
    std::env::var("LATHA_GUI_UPDATE").as_deref() != Ok("0")
}

fn debug() -> bool {
    std::env::var_os("LATHA_GUI_DEBUG").is_some_and(|v| v != "0")
}

/// The default update store: $LATHA_GUI_UPDATE_DIR, else $XDG_DATA_HOME/latha-gui/updates
/// (~/.local/share/latha-gui/updates).
pub fn default_store() -> Option<PathBuf> {
    if let Some(d) = std::env::var_os("LATHA_GUI_UPDATE_DIR") {
        return Some(PathBuf::from(d));
    }
    if let Some(d) = std::env::var_os("XDG_DATA_HOME") {
        if !d.is_empty() {
            return Some(PathBuf::from(d).join("latha-gui").join("updates"));
        }
    }
    std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".local/share/latha-gui/updates"))
}

/// <store>/<id>/current: "version\nfile name\n".
fn current(store: &Path, id: &str) -> Option<(String, PathBuf)> {
    let text = std::fs::read_to_string(store.join(id).join("current")).ok()?;
    let mut l = text.lines();
    let (v, f) = (l.next()?.trim(), l.next()?.trim());
    if v.is_empty() || f.is_empty() || f.contains('/') {
        return None;
    }
    let p = store.join(id).join(f);
    p.is_file().then(|| (v.to_string(), p))
}

/// The copy of the app at `path` a loader should run: the update store's when it has a newer version of the same
/// app, else `path`.
pub fn resolve(path: &Path, store: &Path) -> PathBuf {
    if !enabled() {
        return path.to_path_buf();
    }
    let Some(info) = read_app_info(path) else { return path.to_path_buf() };
    if info.id.is_empty() {
        return path.to_path_buf();
    }
    match current(store, &info.id) {
        Some((v, p)) if compare_versions(&v, &info.version) == Ordering::Greater => {
            if debug() {
                eprintln!("latha-gui: running {} {v} (update) instead of {} {}", info.id, path.display(), info.version);
            }
            p
        }
        _ => path.to_path_buf(),
    }
}

/// What check() did.
#[derive(Debug, PartialEq)]
pub enum Checked {
    /// The app has no id, version or update URL, or updates are off.
    NoUpdates,
    /// Checked less than the interval ago.
    NotDue,
    /// The manifest's version is not newer.
    UpToDate(String),
    /// Downloaded this version and its path; resolve() runs it from now on.
    Updated(String, PathBuf),
}

fn interval() -> Duration {
    let s = std::env::var("LATHA_GUI_UPDATE_INTERVAL").ok().and_then(|v| v.parse::<u64>().ok()).unwrap_or(6 * 3600);
    Duration::from_secs(s)
}

fn check_url(url: &str) -> Result<()> {
    if url.starts_with("https://") {
        return Ok(());
    }
    if let Some(rest) = url.strip_prefix("http://") {
        let host = rest.split(['/', '?', '#']).next().unwrap_or("");
        let host = host.rsplit_once('@').map_or(host, |(_, h)| h);
        let host = if host.starts_with('[') { host.split(']').next().unwrap_or("").trim_start_matches('[') } else { host.split(':').next().unwrap_or("") };
        if matches!(host, "localhost" | "127.0.0.1" | "::1") {
            return Ok(());
        }
    }
    bail!("{url}: updates come over https only")
}

/// `rel` against the manifest URL `base`.
fn join_url(base: &str, rel: &str) -> String {
    if rel.contains("://") {
        return rel.to_string();
    }
    let scheme_end = base.find("://").map_or(0, |i| i + 3);
    if let Some(r) = rel.strip_prefix('/') {
        let host_end = base[scheme_end..].find('/').map_or(base.len(), |i| scheme_end + i);
        return format!("{}/{r}", &base[..host_end]);
    }
    let base = base.split(['?', '#']).next().unwrap_or(base);
    match base[scheme_end..].rfind('/') {
        Some(i) => format!("{}/{rel}", &base[..scheme_end + i]),
        None => format!("{base}/{rel}"),
    }
}

/// One manifest.
#[derive(Debug, Default)]
struct Manifest {
    version: String,
    url: String,
    sha256: String,
}

/// Check for a newer build of the app at `path` (or of the newer copy resolve() gives) and download it into
/// `store`. `force` checks even when the last check was less than the interval ago. Blocking (network): call it on
/// a worker thread.
pub fn check(path: &Path, store: &Path, force: bool) -> Result<Checked> {
    if !enabled() {
        return Ok(Checked::NoUpdates);
    }
    let run = resolve(path, store);
    let info = read_app_info(&run).ok_or_else(|| format_err!("{}: not a wasm file", run.display()))?;
    if info.id.is_empty() || info.version.is_empty() || info.update.is_empty() {
        return Ok(Checked::NoUpdates);
    }
    check_url(&info.update)?;
    let dir = store.join(&info.id);
    std::fs::create_dir_all(&dir).map_err(|e| format_err!("{}: {e}", dir.display()))?;
    let stamp = dir.join("checked");
    if !force {
        if let Ok(t) = std::fs::metadata(&stamp).and_then(|m| m.modified()) {
            if SystemTime::now().duration_since(t).is_ok_and(|d| d < interval()) {
                return Ok(Checked::NotDue);
            }
        }
    }
    // One check at a time per app (two windows of the same app start together); a lock older than an hour is
    // left over from a killed run.
    let lock = dir.join("lock");
    if let Ok(t) = std::fs::metadata(&lock).and_then(|m| m.modified()) {
        if SystemTime::now().duration_since(t).is_ok_and(|d| d > Duration::from_secs(3600)) {
            let _ = std::fs::remove_file(&lock);
        }
    }
    let Ok(_) = std::fs::OpenOptions::new().write(true).create_new(true).open(&lock) else {
        return Ok(Checked::NotDue);
    };
    struct Unlock(PathBuf);
    impl Drop for Unlock {
        fn drop(&mut self) {
            let _ = std::fs::remove_file(&self.0);
        }
    }
    let _unlock = Unlock(lock);

    let mut text = Vec::new();
    http::get(&info.update)?.take(64 << 10).read_to_end(&mut text)?;
    let mut m = Manifest::default();
    parse_lines(&text, |k, v| match k {
        "version" => m.version = v.into(),
        "url" => m.url = v.into(),
        "sha256" => m.sha256 = v.to_ascii_lowercase(),
        _ => {}
    });
    if m.version.is_empty() || m.url.is_empty() || m.sha256.len() != 64 || !m.sha256.bytes().all(|c| c.is_ascii_hexdigit()) {
        bail!("{}: not an update manifest (needs version=, url=, sha256=)", info.update);
    }
    let _ = std::fs::write(&stamp, b"");
    if compare_versions(&m.version, &info.version) != Ordering::Greater {
        return Ok(Checked::UpToDate(m.version));
    }
    let url = join_url(&info.update, &m.url);
    check_url(&url)?;
    let file: String = m.version.chars().map(|c| if c.is_ascii_alphanumeric() || matches!(c, '.' | '-' | '_') { c } else { '_' }).collect();
    let file = format!("{}.wasm", file.trim_start_matches('.'));
    let dest = dir.join(&file);
    let part = dir.join(format!("{file}.part"));
    let t = std::time::Instant::now();
    let res = (|| -> Result<()> {
        let body = http::get(&url)?;
        let mut body: Box<dyn Read> = if url.split(['?', '#']).next().unwrap_or("").ends_with(".zst") {
            let mut z = zstd::stream::read::Decoder::new(body)?;
            let _ = z.window_log_max(31);
            Box::new(z)
        } else {
            body
        };
        let mut f = std::io::BufWriter::new(std::fs::File::create(&part)?);
        let mut h = Sha256::new();
        let mut buf = vec![0u8; 1 << 20];
        loop {
            let n = body.read(&mut buf)?;
            if n == 0 {
                break;
            }
            h.update(&buf[..n]);
            f.write_all(&buf[..n])?;
        }
        f.flush()?;
        drop(f);
        let got: String = h.finalize().iter().map(|b| format!("{b:02x}")).collect();
        if got != m.sha256 {
            bail!("{url}: SHA-256 {got}, the manifest says {}", m.sha256);
        }
        let new = read_app_info(&part).ok_or_else(|| format_err!("{url}: not a wasm file"))?;
        if new.id != info.id {
            bail!("{url}: is the app {:?}, not {:?}", new.id, info.id);
        }
        if new.version != m.version {
            bail!("{url}: says it is version {:?}, the manifest says {:?}", new.version, m.version);
        }
        std::fs::rename(&part, &dest)?;
        let cur = dir.join("current.tmp");
        std::fs::write(&cur, format!("{}\n{file}\n", m.version))?;
        std::fs::rename(&cur, dir.join("current"))?;
        Ok(())
    })();
    if let Err(e) = res {
        let _ = std::fs::remove_file(&part);
        return Err(e);
    }
    if debug() {
        eprintln!("latha-gui: downloaded {} {} in {:.1} s", info.id, m.version, t.elapsed().as_secs_f64());
    }
    // Older downloads: a running instance keeps its open file (unlinked files stay readable).
    if let Ok(rd) = std::fs::read_dir(&dir) {
        for e in rd.flatten() {
            let n = e.file_name();
            let n = n.to_string_lossy();
            if n != file && (n.ends_with(".wasm") || n.ends_with(".part")) {
                let _ = std::fs::remove_file(e.path());
            }
        }
    }
    Ok(Checked::Updated(m.version, dest))
}

/// HTTP GET as a reader. Desktop: curl, as prebuilt.rs (the system's proxies and certificates); Android has no
/// curl: ureq with rustls and the Mozilla roots.
mod http {
    use std::io::Read;
    use wasmtime::{format_err, Result};

    #[cfg(not(target_os = "android"))]
    pub fn get(url: &str) -> Result<Box<dyn Read>> {
        struct Curl {
            child: std::process::Child,
            out: std::process::ChildStdout,
            url: String,
        }
        impl Read for Curl {
            fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
                let n = self.out.read(buf)?;
                if n == 0 && !buf.is_empty() {
                    let s = self.child.wait()?;
                    if !s.success() {
                        return Err(std::io::Error::other(format!("curl {}: {s}", self.url)));
                    }
                }
                Ok(n)
            }
        }
        impl Drop for Curl {
            fn drop(&mut self) {
                let _ = self.child.kill();
                let _ = self.child.wait();
            }
        }
        let mut child = std::process::Command::new("curl")
            .args(["-fsSL", "--proto", "=https,http", "--connect-timeout", "10", "--speed-time", "60", "--speed-limit", "1024", url])
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::null())
            .spawn()
            .map_err(|e| format_err!("curl: {e}"))?;
        let out = child.stdout.take().ok_or_else(|| format_err!("curl: no stdout"))?;
        Ok(Box::new(Curl { child, out, url: url.to_string() }))
    }

    #[cfg(target_os = "android")]
    pub fn get(url: &str) -> Result<Box<dyn Read>> {
        let r = ureq::get(url).call().map_err(|e| format_err!("{url}: {e}"))?;
        Ok(Box::new(r.into_body().into_reader()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn versions() {
        assert_eq!(compare_versions("2026.11.12.1730", "2026.11.12.930"), Ordering::Greater);
        assert_eq!(compare_versions("1.4", "1.4.0"), Ordering::Less);
        assert_eq!(compare_versions("1.10", "1.9"), Ordering::Greater);
        assert_eq!(compare_versions("1.0-rc1", "1.0-rc2"), Ordering::Less);
        assert_eq!(compare_versions("2.0", "2.0"), Ordering::Equal);
    }

    #[test]
    fn urls() {
        assert_eq!(join_url("https://h/a/b/m.txt", "x.wasm"), "https://h/a/b/x.wasm");
        assert_eq!(join_url("https://h/a/b/m.txt?q=1", "x.wasm"), "https://h/a/b/x.wasm");
        assert_eq!(join_url("https://h/a/b/m.txt", "/x.wasm"), "https://h/x.wasm");
        assert_eq!(join_url("https://h/a/m", "https://o/x"), "https://o/x");
        assert!(check_url("http://127.0.0.1:8000/m").is_ok());
        assert!(check_url("http://example.com/m").is_err());
        assert!(check_url("http://localhost.evil.com/m").is_err());
    }
}
