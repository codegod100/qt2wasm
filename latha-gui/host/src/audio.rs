// SPDX-FileCopyrightText: 2026 Kwi WebAssembly port contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only
//
// latha:gui's sound output (gui/wit/audio.wit). Each output is a short queue
// of samples the app writes and a thread of its own that hands them to the
// device with the platform's blocking write: PulseAudio's simple API on Linux
// (PipeWire serves it too), AAudio on Android. Both are opened with dlopen, so
// a host without them still runs and `open` fails.

use std::collections::{HashMap, VecDeque};
use std::ffi::{c_char, c_int, c_void, CString};
use std::sync::{Arc, Condvar, Mutex};
use std::time::Duration;

/// The longest queue in front of the device; older samples are dropped.
const MAX_QUEUE_MS: u64 = 250;
/// How much the thread hands the device at a time.
const PERIOD_MS: u64 = 10;

trait Device: Send {
    /// Blocks until the device has taken `samples` (whole frames).
    fn write(&mut self, samples: &[f32]) -> bool;
    /// Drops what the device holds and has not played.
    fn flush(&mut self) {}
}

struct Queue {
    samples: VecDeque<f32>,
    flush: bool,
    closed: bool,
}

struct Output {
    shared: Arc<(Mutex<Queue>, Condvar)>,
    channels: usize,
    max_samples: usize,
}

#[derive(Default)]
pub struct AudioState {
    outputs: HashMap<u32, Output>,
    next_id: u32,
}

impl Drop for AudioState {
    fn drop(&mut self) {
        for (_, o) in self.outputs.drain() {
            close_output(&o);
        }
    }
}

fn close_output(o: &Output) {
    let (m, c) = &*o.shared;
    m.lock().unwrap().closed = true;
    c.notify_all();
}

fn run(mut device: Box<dyn Device>, shared: Arc<(Mutex<Queue>, Condvar)>, period: usize) {
    let (m, c) = &*shared;
    let mut chunk = Vec::with_capacity(period);
    loop {
        let flush;
        {
            let mut q = m.lock().unwrap();
            // A period, or what there is once a period's time went by without more: the end of a sound.
            if q.samples.len() < period && !q.closed && !q.flush {
                let (g, _) = c.wait_timeout_while(q, Duration::from_millis(PERIOD_MS), |q| q.samples.len() < period && !q.closed && !q.flush).unwrap();
                q = g;
            }
            while q.samples.is_empty() && !q.closed && !q.flush {
                q = c.wait(q).unwrap();
            }
            if q.closed {
                return;
            }
            flush = std::mem::take(&mut q.flush);
            let n = q.samples.len().min(period);
            chunk.clear();
            chunk.extend(q.samples.drain(..n));
        }
        if flush {
            device.flush();
        }
        if !chunk.is_empty() && !device.write(&chunk) {
            eprintln!("latha-gui: the sound device stopped taking samples");
            m.lock().unwrap().closed = true;
            return;
        }
    }
}

impl crate::audio_bindings::latha::gui::audio::Host for AudioState {
    fn open(&mut self, sample_rate: u32, channels: u8, name: String) -> Result<u32, String> {
        if sample_rate == 0 || channels == 0 {
            return Err("no such format".into());
        }
        let device = open_device(sample_rate, channels, &name)?;
        let channels = channels as usize;
        let frames_per_ms = (sample_rate as usize).div_ceil(1000);
        let period = frames_per_ms * PERIOD_MS as usize * channels;
        let shared = Arc::new((Mutex::new(Queue { samples: VecDeque::new(), flush: false, closed: false }), Condvar::new()));
        let thread_shared = shared.clone();
        std::thread::Builder::new()
            .name("latha audio".into())
            .spawn(move || run(device, thread_shared, period))
            .map_err(|e| e.to_string())?;
        self.next_id += 1;
        let id = self.next_id;
        self.outputs.insert(id, Output { shared, channels, max_samples: frames_per_ms * MAX_QUEUE_MS as usize * channels });
        Ok(id)
    }

    fn write(&mut self, id: u32, samples: Vec<f32>) {
        let Some(o) = self.outputs.get(&id) else { return };
        let whole = samples.len() - samples.len() % o.channels;
        let (m, c) = &*o.shared;
        let mut q = m.lock().unwrap();
        q.samples.extend(&samples[..whole]);
        if q.samples.len() > o.max_samples {
            let over = q.samples.len() - o.max_samples;
            let over = over + (o.channels - over % o.channels) % o.channels;
            let over = over.min(q.samples.len());
            q.samples.drain(..over);
        }
        c.notify_all();
    }

    fn flush(&mut self, id: u32) {
        let Some(o) = self.outputs.get(&id) else { return };
        let (m, c) = &*o.shared;
        let mut q = m.lock().unwrap();
        q.samples.clear();
        q.flush = true;
        c.notify_all();
    }

    fn queued(&mut self, id: u32) -> u32 {
        let Some(o) = self.outputs.get(&id) else { return 0 };
        (o.shared.0.lock().unwrap().samples.len() / o.channels) as u32
    }

    fn close(&mut self, id: u32) {
        if let Some(o) = self.outputs.remove(&id) {
            close_output(&o);
        }
    }
}

/// Whether sound is turned off for this host (LATHA_GUI_AUDIO=0).
fn disabled() -> bool {
    std::env::var_os("LATHA_GUI_AUDIO").is_some_and(|v| v == "0")
}

unsafe fn sym<T: Copy>(lib: *mut c_void, name: &str) -> Result<T, String> {
    let c = CString::new(name).unwrap();
    let p = libc::dlsym(lib, c.as_ptr());
    if p.is_null() {
        return Err(format!("no {name}"));
    }
    Ok(std::mem::transmute_copy(&p))
}

unsafe fn dlopen(names: &[&str]) -> Result<*mut c_void, String> {
    for n in names {
        let c = CString::new(*n).unwrap();
        let lib = libc::dlopen(c.as_ptr(), libc::RTLD_NOW | libc::RTLD_LOCAL);
        if !lib.is_null() {
            return Ok(lib);
        }
    }
    Err(format!("no {}", names[0]))
}

#[cfg(not(target_os = "android"))]
fn open_device(rate: u32, channels: u8, name: &str) -> Result<Box<dyn Device>, String> {
    if disabled() {
        return Err("sound is off (LATHA_GUI_AUDIO=0)".into());
    }
    pulse::open(rate, channels, name)
}

#[cfg(target_os = "android")]
fn open_device(rate: u32, channels: u8, _name: &str) -> Result<Box<dyn Device>, String> {
    if disabled() {
        return Err("sound is off (LATHA_GUI_AUDIO=0)".into());
    }
    aaudio::open(rate, channels)
}

#[cfg(not(target_os = "android"))]
mod pulse {
    use super::*;

    #[repr(C)]
    struct SampleSpec {
        format: c_int,
        rate: u32,
        channels: u8,
    }
    #[repr(C)]
    struct BufferAttr {
        maxlength: u32,
        tlength: u32,
        prebuf: u32,
        minreq: u32,
        fragsize: u32,
    }
    const PA_SAMPLE_FLOAT32LE: c_int = 5;
    const PA_STREAM_PLAYBACK: c_int = 1;

    type New = unsafe extern "C" fn(*const c_char, *const c_char, c_int, *const c_char, *const c_char, *const SampleSpec, *const c_void, *const BufferAttr, *mut c_int) -> *mut c_void;
    type Write = unsafe extern "C" fn(*mut c_void, *const c_void, usize, *mut c_int) -> c_int;
    type Flush = unsafe extern "C" fn(*mut c_void, *mut c_int) -> c_int;
    type Free = unsafe extern "C" fn(*mut c_void);
    type Strerror = unsafe extern "C" fn(c_int) -> *const c_char;

    struct Pulse {
        s: *mut c_void,
        write: Write,
        flush: Flush,
        free: Free,
    }
    unsafe impl Send for Pulse {}

    impl Device for Pulse {
        fn write(&mut self, samples: &[f32]) -> bool {
            let mut err = 0;
            unsafe { (self.write)(self.s, samples.as_ptr().cast(), std::mem::size_of_val(samples), &mut err) == 0 }
        }
        fn flush(&mut self) {
            let mut err = 0;
            unsafe { (self.flush)(self.s, &mut err) };
        }
    }
    impl Drop for Pulse {
        fn drop(&mut self) {
            unsafe { (self.free)(self.s) }
        }
    }

    pub fn open(rate: u32, channels: u8, name: &str) -> Result<Box<dyn Device>, String> {
        unsafe {
            let lib = dlopen(&["libpulse-simple.so.0", "libpulse-simple.so"])?;
            let new: New = sym(lib, "pa_simple_new")?;
            let strerror: Strerror = sym(dlopen(&["libpulse.so.0", "libpulse.so"])?, "pa_strerror")?;
            let spec = SampleSpec { format: PA_SAMPLE_FLOAT32LE, rate, channels };
            // A short target (40 ms) in front of our own queue; the rest is the server's to choose.
            let bytes_per_ms = rate as u64 * channels as u64 * 4 / 1000;
            let attr = BufferAttr { maxlength: u32::MAX, tlength: (bytes_per_ms * 40) as u32, prebuf: u32::MAX, minreq: u32::MAX, fragsize: u32::MAX };
            let app = CString::new("latha").unwrap();
            let stream = CString::new(if name.is_empty() { "sound" } else { name }).unwrap_or_else(|_| CString::new("sound").unwrap());
            let mut err = 0;
            let s = new(std::ptr::null(), app.as_ptr(), PA_STREAM_PLAYBACK, std::ptr::null(), stream.as_ptr(), &spec, std::ptr::null(), &attr, &mut err);
            if s.is_null() {
                let msg = std::ffi::CStr::from_ptr(strerror(err)).to_string_lossy().into_owned();
                return Err(format!("PulseAudio: {msg}"));
            }
            Ok(Box::new(Pulse { s, write: sym(lib, "pa_simple_write")?, flush: sym(lib, "pa_simple_flush")?, free: sym(lib, "pa_simple_free")? }))
        }
    }
}

#[cfg(target_os = "android")]
mod aaudio {
    use super::*;

    const AAUDIO_FORMAT_PCM_FLOAT: i32 = 2;
    type CreateBuilder = unsafe extern "C" fn(*mut *mut c_void) -> i32;
    type SetI32 = unsafe extern "C" fn(*mut c_void, i32);
    type OpenStream = unsafe extern "C" fn(*mut c_void, *mut *mut c_void) -> i32;
    type BuilderDelete = unsafe extern "C" fn(*mut c_void) -> i32;
    type StreamOp = unsafe extern "C" fn(*mut c_void) -> i32;
    type StreamWrite = unsafe extern "C" fn(*mut c_void, *const c_void, i32, i64) -> i32;

    struct AAudio {
        s: *mut c_void,
        channels: usize,
        write: StreamWrite,
        flush: StreamOp,
        pause: StreamOp,
        start: StreamOp,
        close: StreamOp,
    }
    unsafe impl Send for AAudio {}

    impl Device for AAudio {
        fn write(&mut self, samples: &[f32]) -> bool {
            let mut rest = samples;
            while !rest.is_empty() {
                let frames = (rest.len() / self.channels) as i32;
                let n = unsafe { (self.write)(self.s, rest.as_ptr().cast(), frames, 1_000_000_000) };
                if n < 0 {
                    return false;
                }
                rest = &rest[n as usize * self.channels..];
            }
            true
        }
        fn flush(&mut self) {
            unsafe {
                (self.pause)(self.s);
                (self.flush)(self.s);
                (self.start)(self.s);
            }
        }
    }
    impl Drop for AAudio {
        fn drop(&mut self) {
            unsafe { (self.close)(self.s) };
        }
    }

    pub fn open(rate: u32, channels: u8) -> Result<Box<dyn Device>, String> {
        unsafe {
            let lib = dlopen(&["libaaudio.so"])?;
            let create: CreateBuilder = sym(lib, "AAudio_createStreamBuilder")?;
            let mut b = std::ptr::null_mut();
            if create(&mut b) != 0 {
                return Err("AAudio: no stream builder".into());
            }
            sym::<SetI32>(lib, "AAudioStreamBuilder_setSampleRate")?(b, rate as i32);
            sym::<SetI32>(lib, "AAudioStreamBuilder_setChannelCount")?(b, channels as i32);
            sym::<SetI32>(lib, "AAudioStreamBuilder_setFormat")?(b, AAUDIO_FORMAT_PCM_FLOAT);
            let mut s = std::ptr::null_mut();
            let r = sym::<OpenStream>(lib, "AAudioStreamBuilder_openStream")?(b, &mut s);
            sym::<BuilderDelete>(lib, "AAudioStreamBuilder_delete")?(b);
            if r != 0 || s.is_null() {
                return Err(format!("AAudio: no stream ({r})"));
            }
            let dev = AAudio {
                s,
                channels: channels as usize,
                write: sym(lib, "AAudioStream_write")?,
                flush: sym(lib, "AAudioStream_requestFlush")?,
                pause: sym(lib, "AAudioStream_requestPause")?,
                start: sym(lib, "AAudioStream_requestStart")?,
                close: sym(lib, "AAudioStream_close")?,
            };
            (dev.start)(s);
            Ok(Box::new(dev))
        }
    }
}
