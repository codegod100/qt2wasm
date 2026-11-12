// SPDX-FileCopyrightText: 2026 Kwi WebAssembly port contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only
//
// latha-run: run a WASI command (core module or command component) the way
// latha-gui runs one, without Qt: big wasm stack, wasm exceptions, tail calls,
// SIMD, wasi:sockets, latha:proc processes and channels.
//
//   latha-run [--dir HOST[::GUEST]]... [--no-root] [--no-cache] [--no-network] APP.wasm [ARGS...]
//   latha-run --precompile DIR APP.wasm     compile a window app for a prebuilt store (prints the file)
//   latha-run --prebuilt-name APP.wasm      the file name a prebuilt store keeps APP.wasm under
//
// The whole file system is open as itself (/) and the current directory as
// ".", unless --no-root.
use std::process::exit;

fn main() {
    let mut argv = std::env::args().skip(1).peekable();
    let mut dirs: Vec<(String, String)> = Vec::new();
    let (mut root, mut cache, mut network) = (true, true, true);
    while let Some(a) = argv.peek().cloned() {
        if !a.starts_with('-') {
            break;
        }
        argv.next();
        match a.as_str() {
            "--dir" => {
                let Some(d) = argv.next() else { usage() };
                let (h, g) = match d.split_once("::") {
                    Some((h, g)) => (h.to_string(), g.to_string()),
                    None => (d.clone(), d.clone()),
                };
                dirs.push((h, g));
            }
            "--precompile" => {
                let (Some(dir), Some(app)) = (argv.next(), argv.next()) else { usage() };
                match latha_host::prebuilt::precompile(app.as_ref(), dir.as_ref()) {
                    Ok(p) => {
                        println!("{}", p.display());
                        exit(0)
                    }
                    Err(e) => {
                        eprintln!("latha-run: {e:#}");
                        exit(2)
                    }
                }
            }
            "--prebuilt-name" => {
                let Some(app) = argv.next() else { usage() };
                match std::fs::read(&app).map_err(|e| e.into()).and_then(|b| latha_host::prebuilt::prebuilt_name(&b)) {
                    Ok(n) => {
                        println!("{n}");
                        exit(0)
                    }
                    Err(e) => {
                        eprintln!("latha-run: {app}: {e:#}");
                        exit(2)
                    }
                }
            }
            "--no-root" => root = false,
            "--no-cache" => cache = false,
            "--no-network" => network = false,
            "--" => break,
            "-h" | "--help" => usage(),
            _ => {
                eprintln!("latha-run: unknown option {a}");
                usage()
            }
        }
    }
    let Some(app) = argv.next() else { usage() };
    if root {
        if !dirs.iter().any(|(_, g)| g == "/") {
            dirs.insert(0, ("/".into(), "/".into()));
        }
        if !dirs.iter().any(|(_, g)| g == ".") {
            dirs.push((".".into(), ".".into()));
        }
    }
    let name = std::path::Path::new(&app).file_name().map_or(app.clone(), |n| n.to_string_lossy().into_owned());
    let args: Vec<String> = std::iter::once(name).chain(argv).collect();
    let ro = latha_host::procs::RunOptions {
        dirs,
        env: std::env::vars().collect(),
        network,
        cache: cache && std::env::var("LATHA_GUI_CACHE").as_deref() != Ok("0"),
    };
    match latha_host::run_with(std::path::Path::new(&app), ro, args) {
        Ok(code) => exit(code),
        Err(e) => {
            eprintln!("latha-run: {app}: {e:?}");
            exit(1)
        }
    }
}

fn usage() -> ! {
    eprintln!("usage: latha-run [--dir HOST[::GUEST]]... [--no-root] [--no-cache] [--no-network] APP.wasm [ARGS...]");
    eprintln!("       latha-run --precompile DIR APP.wasm | --prebuilt-name APP.wasm");
    exit(2)
}
