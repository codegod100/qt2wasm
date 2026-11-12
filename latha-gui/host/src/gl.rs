//! latha:gui's `gl` interface (qt2wasm/gui/wit/gl.wit): an app's OpenGL ES 3 calls, run on the host's GPU.
//!
//! The GL library is loaded when the app makes its first context: `LATHA_GUI_GL_EGL` (and `LATHA_GUI_GL_GLES`)
//! name an EGL (and GLES) library to use, such as ANGLE's libEGL.so/libGLESv2.so; otherwise the system's
//! libEGL.so.1 and libGLESv2.so.2 (glvnd, Mesa, NVIDIA). `LATHA_GUI_GL=0` turns GL off: every context fails.
//! Contexts have no window: surfaceless where EGL can do that (EGL_KHR_surfaceless_context), else a 1x1 pbuffer.
//! Everything runs on the thread that calls the app (the GUI thread).
//!
//! ANGLE has the robust entry points WebGL engines call (glTexImage2DRobustANGLE, ...) and WebGL's validation
//! (EGL_CONTEXT_WEBGL_COMPATIBILITY_ANGLE). Another driver gets the plain call: the host pads uploads to what the
//! unpack state reads, sizes readbacks by the pack state, and counts the values a getter writes. Generated part:
//! gl_gen.rs (latha-gui/gl/gen.py).

use std::collections::{HashMap, HashSet};
use std::ffi::{c_char, c_void, CStr, CString};

use super::gl_bindings::latha::gui::gl as w;
use super::gl_gen::GlFns;

pub const GL_INVALID_VALUE: u32 = 0x0501;
pub const GL_INVALID_OPERATION: u32 = 0x0502;

/// The WASI environment variable that tells an app this host has `gl` (and which version of it).
pub const ENV: (&str, &str) = ("LATHA_GUI_GL", "0.2.3");

/// False when the user turned GL off (LATHA_GUI_GL=0): then the app is not told it has GL.
pub fn enabled() -> bool {
    std::env::var("LATHA_GUI_GL").as_deref() != Ok("0")
}

type EGLDisplay = *mut c_void;
type EGLConfig = *mut c_void;
type EGLContext = *mut c_void;
type EGLSurface = *mut c_void;
type EGLint = i32;
type EGLBoolean = u32;

const EGL_NONE: EGLint = 0x3038;
const EGL_EXTENSIONS: EGLint = 0x3055;
const EGL_OPENGL_ES_API: u32 = 0x30A0;
const EGL_SURFACE_TYPE: EGLint = 0x3033;
const EGL_PBUFFER_BIT: EGLint = 0x0001;
const EGL_RENDERABLE_TYPE: EGLint = 0x3040;
const EGL_OPENGL_ES2_BIT: EGLint = 0x0004;
const EGL_OPENGL_ES3_BIT: EGLint = 0x0040;
const EGL_WIDTH: EGLint = 0x3057;
const EGL_HEIGHT: EGLint = 0x3056;
const EGL_CONTEXT_CLIENT_VERSION: EGLint = 0x3098;
const EGL_CONTEXT_MINOR_VERSION: EGLint = 0x30FB;
const EGL_PLATFORM_SURFACELESS_MESA: u32 = 0x31DD;
const EGL_PLATFORM_ANGLE_ANGLE: u32 = 0x3202;
const EGL_PLATFORM_ANGLE_TYPE_ANGLE: isize = 0x3203;
const EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE: isize = 0x320D;
const EGL_PLATFORM_ANGLE_NATIVE_PLATFORM_TYPE_ANGLE: isize = 0x348F;

const GL_VENDOR: u32 = 0x1F00;
const GL_RENDERER: u32 = 0x1F01;
const GL_VERSION: u32 = 0x1F02;
const GL_REQUESTABLE_EXTENSIONS_ANGLE: u32 = 0x93A8;

struct Egl {
    _lib: *mut c_void,
    gles: *mut c_void,
    get_proc_address: unsafe extern "C" fn(*const c_char) -> *const c_void,
    get_display: unsafe extern "C" fn(*mut c_void) -> EGLDisplay,
    get_platform_display: Option<unsafe extern "C" fn(u32, *mut c_void, *const isize) -> EGLDisplay>,
    initialize: unsafe extern "C" fn(EGLDisplay, *mut EGLint, *mut EGLint) -> EGLBoolean,
    query_string: unsafe extern "C" fn(EGLDisplay, EGLint) -> *const c_char,
    bind_api: unsafe extern "C" fn(u32) -> EGLBoolean,
    choose_config: unsafe extern "C" fn(EGLDisplay, *const EGLint, *mut EGLConfig, EGLint, *mut EGLint) -> EGLBoolean,
    create_context: unsafe extern "C" fn(EGLDisplay, EGLConfig, EGLContext, *const EGLint) -> EGLContext,
    destroy_context: unsafe extern "C" fn(EGLDisplay, EGLContext) -> EGLBoolean,
    create_pbuffer_surface: unsafe extern "C" fn(EGLDisplay, EGLConfig, *const EGLint) -> EGLSurface,
    destroy_surface: unsafe extern "C" fn(EGLDisplay, EGLSurface) -> EGLBoolean,
    make_current: unsafe extern "C" fn(EGLDisplay, EGLSurface, EGLSurface, EGLContext) -> EGLBoolean,
    get_current_context: unsafe extern "C" fn() -> EGLContext,
    get_error: unsafe extern "C" fn() -> EGLint,
}

unsafe fn sym(lib: *mut c_void, name: &str) -> *const c_void {
    let c = CString::new(name).unwrap();
    libc::dlsym(lib, c.as_ptr()) as *const c_void
}

unsafe fn open(names: &[String]) -> Option<*mut c_void> {
    for n in names {
        let c = CString::new(n.as_str()).ok()?;
        let h = libc::dlopen(c.as_ptr(), libc::RTLD_NOW | libc::RTLD_LOCAL);
        if !h.is_null() {
            return Some(h);
        }
    }
    None
}

impl Egl {
    unsafe fn load() -> Result<Egl, String> {
        let egl_names: Vec<String> = match std::env::var("LATHA_GUI_GL_EGL") {
            Ok(p) => vec![p],
            Err(_) => vec!["libEGL.so.1".into(), "libEGL.so".into()],
        };
        let gles_names: Vec<String> = match (std::env::var("LATHA_GUI_GL_GLES"), std::env::var("LATHA_GUI_GL_EGL")) {
            (Ok(p), _) => vec![p],
            (Err(_), Ok(egl)) => {
                // ANGLE ships libEGL and libGLESv2 side by side.
                let dir = std::path::Path::new(&egl).parent().map(|d| d.to_path_buf()).unwrap_or_default();
                let file = std::path::Path::new(&egl).file_name().and_then(|f| f.to_str()).unwrap_or("").to_string();
                vec![dir.join(file.replace("EGL", "GLESv2")).to_string_lossy().into_owned()]
            }
            _ => vec!["libGLESv2.so.2".into(), "libGLESv2.so".into()],
        };
        let lib = open(&egl_names).ok_or_else(|| format!("cannot load {}", egl_names.join(" or ")))?;
        let gles = open(&gles_names).unwrap_or(std::ptr::null_mut());
        macro_rules! need {
            ($n:literal) => {{
                let p = sym(lib, $n);
                if p.is_null() {
                    return Err(format!("{} has no {}", egl_names[0], $n));
                }
                std::mem::transmute::<*const c_void, _>(p)
            }};
        }
        let get_proc_address: unsafe extern "C" fn(*const c_char) -> *const c_void = need!("eglGetProcAddress");
        let gpd = {
            let p = sym(lib, "eglGetPlatformDisplay");
            let p = if p.is_null() { get_proc_address(c"eglGetPlatformDisplayEXT".as_ptr()) } else { p };
            if p.is_null() { None } else { Some(std::mem::transmute::<*const c_void, _>(p)) }
        };
        Ok(Egl {
            _lib: lib,
            gles,
            get_proc_address,
            get_display: need!("eglGetDisplay"),
            get_platform_display: gpd,
            initialize: need!("eglInitialize"),
            query_string: need!("eglQueryString"),
            bind_api: need!("eglBindAPI"),
            choose_config: need!("eglChooseConfig"),
            create_context: need!("eglCreateContext"),
            destroy_context: need!("eglDestroyContext"),
            create_pbuffer_surface: need!("eglCreatePbufferSurface"),
            destroy_surface: need!("eglDestroySurface"),
            make_current: need!("eglMakeCurrent"),
            get_current_context: need!("eglGetCurrentContext"),
            get_error: need!("eglGetError"),
        })
    }

    unsafe fn proc(&self, name: &str) -> *const c_void {
        if !self.gles.is_null() {
            let p = sym(self.gles, name);
            if !p.is_null() {
                return p;
            }
        }
        let c = CString::new(name).unwrap();
        (self.get_proc_address)(c.as_ptr())
    }
}

fn has_ext(list: *const c_char, ext: &str) -> bool {
    if list.is_null() {
        return false;
    }
    let s = unsafe { CStr::from_ptr(list) }.to_string_lossy();
    s.split(' ').any(|e| e == ext)
}

struct Context {
    context: EGLContext,
    surface: EGLSurface,
}

/// The `gl` state of one app (one store).
pub struct GlState {
    loaded: Option<Result<Box<Loaded>, String>>,
    pub(crate) fns: GlFns,
    pub(crate) is_angle: bool,
    contexts: HashMap<u32, Context>,
    next_context: u32,
    current: u32,
    pending_error: Option<u32>,
    syncs: HashMap<u64, *const c_void>,
    next_sync: u64,
    warned: HashSet<&'static str>,
}

struct Loaded {
    egl: Egl,
    display: EGLDisplay,
    config: EGLConfig,
    surfaceless: bool,
}

// The pointers are only used on the thread that runs the app's calls.
unsafe impl Send for GlState {}

impl Default for GlState {
    fn default() -> Self {
        GlState {
            loaded: None,
            fns: GlFns::load(&|_| std::ptr::null()),
            is_angle: false,
            contexts: HashMap::new(),
            next_context: 1,
            current: 0,
            pending_error: None,
            syncs: HashMap::new(),
            next_sync: 1,
            warned: HashSet::new(),
        }
    }
}

impl Drop for GlState {
    fn drop(&mut self) {
        let ids: Vec<u32> = self.contexts.keys().copied().collect();
        for id in ids {
            self.destroy_context_impl(id);
        }
    }
}

impl GlState {
    fn loaded(&mut self) -> Option<&Loaded> {
        if self.loaded.is_none() {
            let r = if std::env::var("LATHA_GUI_GL").as_deref() == Ok("0") {
                Err("LATHA_GUI_GL=0".to_string())
            } else {
                unsafe { Self::load() }
            };
            match &r {
                Ok(l) => {
                    self.fns = GlFns::load(&|n| unsafe { l.egl.proc(n) });
                    let ext = unsafe { (l.egl.query_string)(l.display, EGL_EXTENSIONS) };
                    self.is_angle = has_ext(ext, "EGL_ANGLE_create_context_webgl_compatibility")
                        || unsafe { CStr::from_ptr((l.egl.query_string)(l.display, 0x3053 /* EGL_VENDOR */)) }
                            .to_string_lossy()
                            .contains("Google");
                }
                Err(e) => eprintln!("latha-gui: no GL for the app: {e}"),
            }
            self.loaded = Some(r);
        }
        match &self.loaded {
            Some(Ok(l)) => Some(l),
            _ => None,
        }
    }

    unsafe fn load() -> Result<Box<Loaded>, String> {
        let egl = Egl::load()?;
        let client_ext = (egl.query_string)(std::ptr::null_mut(), EGL_EXTENSIONS);
        let mut display = std::ptr::null_mut();
        if let Some(gpd) = egl.get_platform_display {
            if has_ext(client_ext, "EGL_ANGLE_platform_angle") {
                let attribs = [
                    EGL_PLATFORM_ANGLE_TYPE_ANGLE,
                    EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE,
                    EGL_PLATFORM_ANGLE_NATIVE_PLATFORM_TYPE_ANGLE,
                    EGL_PLATFORM_SURFACELESS_MESA as isize,
                    EGL_NONE as isize,
                ];
                display = gpd(EGL_PLATFORM_ANGLE_ANGLE, std::ptr::null_mut(), attribs.as_ptr());
            } else if has_ext(client_ext, "EGL_MESA_platform_surfaceless") {
                display = gpd(EGL_PLATFORM_SURFACELESS_MESA, std::ptr::null_mut(), std::ptr::null());
            }
        }
        if display.is_null() {
            display = (egl.get_display)(std::ptr::null_mut());
        }
        if display.is_null() {
            return Err("no EGL display".into());
        }
        let (mut major, mut minor) = (0, 0);
        if (egl.initialize)(display, &mut major, &mut minor) == 0 {
            return Err(format!("eglInitialize failed ({:#x})", (egl.get_error)()));
        }
        if (egl.bind_api)(EGL_OPENGL_ES_API) == 0 {
            return Err("EGL has no OpenGL ES".into());
        }
        let ext = (egl.query_string)(display, EGL_EXTENSIONS);
        let surfaceless = has_ext(ext, "EGL_KHR_surfaceless_context");
        let mut config: EGLConfig = std::ptr::null_mut();
        let mut n = 0;
        for renderable in [EGL_OPENGL_ES3_BIT, EGL_OPENGL_ES2_BIT] {
            // EGL_SURFACE_TYPE defaults to EGL_WINDOW_BIT, which a surfaceless display has none of: ask for any
            // (0 matches every config) when there will be no surface, else a pbuffer.
            let surface_type = if surfaceless { 0 } else { EGL_PBUFFER_BIT };
            let attribs = [EGL_RENDERABLE_TYPE, renderable, EGL_SURFACE_TYPE, surface_type, EGL_NONE];
            if (egl.choose_config)(display, attribs.as_ptr(), &mut config, 1, &mut n) != 0 && n > 0 {
                break;
            }
        }
        if n == 0 {
            return Err("no EGL config for OpenGL ES".into());
        }
        Ok(Box::new(Loaded { egl, display, config, surfaceless }))
    }

    pub(crate) fn create_context_impl(&mut self, client_version: u32, attribs: Vec<i32>) -> u32 {
        let is_angle = self.is_angle;
        let Some(l) = self.loaded() else { return 0 };
        let (display, config, surfaceless) = (l.display, l.config, l.surfaceless);
        let egl = &l.egl as *const Egl;
        let egl = unsafe { &*egl };
        // ANGLE takes the app's attributes (WebGL compatibility, robust resource initialisation); a plain driver
        // gets an ES 3.0 context (it runs WebGL 1's GLSL ES 1.00 shaders too), or 2.0 if that is all there is.
        let mut tries: Vec<Vec<EGLint>> = Vec::new();
        if is_angle {
            let mut a: Vec<EGLint> = attribs.chunks_exact(2).filter(|p| p[0] != EGL_NONE).flat_map(|p| [p[0], p[1]]).collect();
            if !a.chunks_exact(2).any(|p| p[0] == EGL_CONTEXT_CLIENT_VERSION) {
                a.extend([EGL_CONTEXT_CLIENT_VERSION, client_version as EGLint]);
            }
            a.push(EGL_NONE);
            tries.push(a);
        }
        tries.push(vec![EGL_CONTEXT_CLIENT_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE]);
        if client_version <= 2 {
            tries.push(vec![EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE]);
        }
        let mut context = std::ptr::null_mut();
        for a in &tries {
            context = unsafe { (egl.create_context)(display, config, std::ptr::null_mut(), a.as_ptr()) };
            if !context.is_null() {
                break;
            }
        }
        if context.is_null() {
            eprintln!("latha-gui: eglCreateContext failed ({:#x})", unsafe { (egl.get_error)() });
            return 0;
        }
        let surface = if surfaceless {
            std::ptr::null_mut()
        } else {
            let a = [EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE];
            unsafe { (egl.create_pbuffer_surface)(display, config, a.as_ptr()) }
        };
        let id = self.next_context;
        self.next_context += 1;
        self.contexts.insert(id, Context { context, surface });
        id
    }

    pub(crate) fn make_current_impl(&mut self, id: u32) -> bool {
        if id == 0 {
            self.current = 0;
            if let Some(l) = self.loaded() {
                unsafe { (l.egl.make_current)(l.display, std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut()) };
            }
            return true;
        }
        if !self.contexts.contains_key(&id) {
            return false;
        }
        self.current = id;
        self.is_current()
    }

    pub(crate) fn destroy_context_impl(&mut self, id: u32) {
        let Some(c) = self.contexts.remove(&id) else { return };
        if self.current == id {
            self.current = 0;
        }
        if let Some(Ok(l)) = &self.loaded {
            unsafe {
                if (l.egl.get_current_context)() == c.context {
                    (l.egl.make_current)(l.display, std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut());
                }
                if !c.surface.is_null() {
                    (l.egl.destroy_surface)(l.display, c.surface);
                }
                (l.egl.destroy_context)(l.display, c.context);
            }
        }
    }

    /// Whether the app's current context is current on this thread (making it so if something else, such as
    /// another app's GL, changed it). GL calls without one do nothing.
    pub(crate) fn is_current(&mut self) -> bool {
        if self.current == 0 {
            return false;
        }
        let Some(Ok(l)) = &self.loaded else { return false };
        let Some(c) = self.contexts.get(&self.current) else { return false };
        unsafe {
            if (l.egl.get_current_context)() == c.context {
                return true;
            }
            (l.egl.make_current)(l.display, c.surface, c.surface, c.context) != 0
        }
    }

    pub(crate) fn set_error(&mut self, e: u32) {
        if self.pending_error.is_none() {
            self.pending_error = Some(e);
        }
    }

    pub(crate) fn unsupported(&mut self, name: &'static str) {
        if self.warned.insert(name) {
            eprintln!("latha-gui: the app called {name}, which this GL does not have");
        }
    }

    pub(crate) fn renderer_impl(&mut self) -> String {
        if !self.is_current() {
            return String::new();
        }
        let mut s = |n| self.get_string_impl(n).unwrap_or_default();
        let (vendor, renderer, version) = (s(GL_VENDOR), s(GL_RENDERER), s(GL_VERSION));
        format!("{renderer} ({version}; {vendor}){}", if self.is_angle { " ANGLE" } else { "" })
    }

    pub(crate) fn get_error_impl(&mut self) -> u32 {
        if let Some(e) = self.pending_error.take() {
            return e;
        }
        if !self.is_current() {
            return 0;
        }
        match self.fns.glGetError {
            Some(f) => unsafe { f() },
            None => 0,
        }
    }

    pub(crate) fn get_string_impl(&mut self, name: u32) -> Option<String> {
        if !self.is_current() || (name == GL_REQUESTABLE_EXTENSIONS_ANGLE && !self.is_angle) {
            return None;
        }
        let f = self.fns.glGetString?;
        let p = unsafe { f(name) };
        if p.is_null() {
            return None;
        }
        Some(unsafe { CStr::from_ptr(p as *const c_char) }.to_string_lossy().into_owned())
    }

    fn get_i(&mut self, pname: u32) -> i32 {
        let mut v = [0i32; 4];
        if let Some(f) = self.fns.glGetIntegerv {
            unsafe { f(pname, v.as_mut_ptr() as *mut c_void) };
        }
        v[0]
    }

    /// How many values glGet*v writes for pname.
    pub(crate) fn value_count(&mut self, pname: u32) -> usize {
        match pname {
            0x0BA2 /* VIEWPORT */ | 0x0C10 /* SCISSOR_BOX */ | 0x0C22 /* COLOR_CLEAR_VALUE */
            | 0x0C23 /* COLOR_WRITEMASK */ | 0x8005 /* BLEND_COLOR */ => 4,
            0x0B70 /* DEPTH_RANGE */ | 0x846D /* ALIASED_POINT_SIZE_RANGE */ | 0x846E /* ALIASED_LINE_WIDTH_RANGE */
            | 0x0D3A /* MAX_VIEWPORT_DIMS */ => 2,
            0x86A3 /* COMPRESSED_TEXTURE_FORMATS */ => self.get_i(0x86A2).max(0) as usize,
            0x8DF8 /* SHADER_BINARY_FORMATS */ => self.get_i(0x8DF9).max(0) as usize,
            0x87FF /* PROGRAM_BINARY_FORMATS */ => self.get_i(0x87FE).max(0) as usize,
            _ => 1,
        }
    }

    /// How many values another glGet*v writes (an upper bound where it depends on more than pname).
    pub(crate) fn param_count(&mut self, func: &str, pname: u32) -> i32 {
        match (func, pname) {
            ("glGetVertexAttribfvRobustANGLE" | "glGetVertexAttribivRobustANGLE", 0x8626 /* CURRENT_VERTEX_ATTRIB */) => 4,
            ("glGetInternalformativRobustANGLE", 0x80A9 /* SAMPLES */) => i32::MAX,
            ("glGetActiveUniformBlockivRobustANGLE", 0x8A43 /* UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES */) => i32::MAX,
            _ => 1,
        }
    }

    fn getv<T: Copy + Default>(&mut self, pname: u32, f: Option<unsafe extern "C" fn(u32, *mut c_void)>) -> Vec<T> {
        if !self.is_current() {
            return Vec::new();
        }
        let Some(f) = f else { return Vec::new() };
        let n = self.value_count(pname);
        let mut v = vec![T::default(); n.max(1) + 16];
        unsafe { f(pname, v.as_mut_ptr() as *mut c_void) };
        v.truncate(n);
        v
    }

    pub(crate) fn get_integerv_impl(&mut self, pname: u32) -> Vec<i32> {
        let f = self.fns.glGetIntegerv;
        self.getv(pname, f)
    }

    pub(crate) fn get_floatv_impl(&mut self, pname: u32) -> Vec<f32> {
        let f = self.fns.glGetFloatv;
        self.getv(pname, f)
    }

    pub(crate) fn get_booleanv_impl(&mut self, pname: u32) -> Vec<u8> {
        let f = self.fns.glGetBooleanv;
        self.getv(pname, f)
    }

    /// Bytes per pixel of format/type (0: unknown).
    fn pixel_size(format: u32, ty: u32) -> i64 {
        let packed = match ty {
            0x8363 | 0x8033 | 0x8034 => 2, // UNSIGNED_SHORT_5_6_5, _4_4_4_4, _5_5_5_1
            0x8368 | 0x8C3B | 0x8C3E | 0x84FA => 4, // UNSIGNED_INT_2_10_10_10_REV, 10F_11F_11F_REV, 5_9_9_9_REV, 24_8
            0x8DAD => 8,                   // FLOAT_32_UNSIGNED_INT_24_8_REV
            _ => 0,
        };
        if packed != 0 {
            return packed;
        }
        let size = match ty {
            0x1400 | 0x1401 => 1,                   // BYTE, UNSIGNED_BYTE
            0x1402 | 0x1403 | 0x140B | 0x8D61 => 2, // SHORT, UNSIGNED_SHORT, HALF_FLOAT, HALF_FLOAT_OES
            0x1404 | 0x1405 | 0x1406 => 4,          // INT, UNSIGNED_INT, FLOAT
            _ => return 0,
        };
        let components = match format {
            0x1903 | 0x8D94 | 0x1906 | 0x1909 | 0x1902 | 0x84F9 => 1, // RED, RED_INTEGER, ALPHA, LUMINANCE, DEPTH_COMPONENT, DEPTH_STENCIL
            0x8227 | 0x8228 | 0x190A => 2,                            // RG, RG_INTEGER, LUMINANCE_ALPHA
            0x1907 | 0x8D98 => 3,                                     // RGB, RGB_INTEGER
            0x1908 | 0x8D99 | 0x80E1 => 4,                            // RGBA, RGBA_INTEGER, BGRA_EXT
            _ => return 0,
        };
        size * components
    }

    /// Bytes a glTex*Image* of this size reads with the current unpack state (pack: glReadPixels writes).
    fn image_bytes(&mut self, pack: bool, width: i32, height: i32, depth: i32, format: u32, ty: u32) -> Option<i64> {
        let bpp = Self::pixel_size(format, ty);
        if bpp == 0 || width < 0 || height < 0 || depth < 0 {
            return None;
        }
        if width == 0 || height == 0 || depth == 0 {
            return Some(0);
        }
        let (alignment, row_length, skip_pixels, skip_rows) = if pack {
            (self.get_i(0x0D05), self.get_i(0x0D02), self.get_i(0x0D04), self.get_i(0x0D03))
        } else {
            (self.get_i(0x0CF5), self.get_i(0x0CF2), self.get_i(0x0CF4), self.get_i(0x0CF3))
        };
        let (image_height, skip_images) = if pack || depth == 1 { (0, 0) } else { (self.get_i(0x806E), self.get_i(0x806D)) };
        let alignment = alignment.max(1) as i64;
        let row = if row_length > 0 { row_length as i64 } else { width as i64 } * bpp;
        let stride = (row + alignment - 1) / alignment * alignment;
        let rows_per_image = if image_height > 0 { image_height as i64 } else { height as i64 };
        let total = (skip_images.max(0) as i64 + depth as i64 - 1) * stride * rows_per_image
            + (skip_rows.max(0) as i64 + height as i64 - 1) * stride
            + (skip_pixels.max(0) as i64 + width as i64) * bpp;
        Some(total)
    }

    /// The pixels of an upload, zero padded to what the plain call reads (None: from the pixel unpack buffer).
    pub(crate) fn pad_unpack(&mut self, pixels: Option<Vec<u8>>, width: i32, height: i32, depth: i32, format: u32, ty: u32) -> Option<Vec<u8>> {
        let mut p = pixels?;
        if self.get_i(0x88EF /* PIXEL_UNPACK_BUFFER_BINDING */) != 0 {
            return None;
        }
        let need = self.image_bytes(false, width, height, depth, format, ty).unwrap_or(0);
        if (p.len() as i64) < need {
            p.resize(need as usize, 0);
        }
        Some(p)
    }

    pub(crate) fn read_pixels_impl(&mut self, x: i32, y: i32, width: i32, height: i32, format: u32, ty: u32, buf_size: i32, offset: u64) -> w::ReadPixelsResult {
        let mut r = w::ReadPixelsResult { length: 0, columns: 0, rows: 0, pixels: Vec::new() };
        if !self.is_current() || buf_size < 0 {
            return r;
        }
        let to_buffer = self.get_i(0x88ED /* PIXEL_PACK_BUFFER_BINDING */) != 0;
        if self.is_angle {
            if let Some(f) = self.fns.glReadPixelsRobustANGLE {
                if !to_buffer {
                    r.pixels = vec![0u8; buf_size as usize];
                }
                let ptr = if to_buffer { offset as usize as *mut c_void } else { r.pixels.as_mut_ptr() as *mut c_void };
                unsafe {
                    f(x, y, width, height, format, ty, buf_size,
                        &mut r.length as *mut i32 as *mut c_void, &mut r.columns as *mut i32 as *mut c_void,
                        &mut r.rows as *mut i32 as *mut c_void, ptr)
                };
                return r;
            }
        }
        let Some(f) = self.fns.glReadPixels else {
            self.unsupported("glReadPixels");
            return r;
        };
        if to_buffer {
            unsafe { f(x, y, width, height, format, ty, offset as usize as *mut c_void) };
            r.columns = width;
            r.rows = height;
            return r;
        }
        let Some(need) = self.image_bytes(true, width, height, 1, format, ty) else {
            self.set_error(0x0500 /* INVALID_ENUM */);
            return r;
        };
        if need > buf_size as i64 {
            self.set_error(GL_INVALID_OPERATION);
            return r;
        }
        r.pixels = vec![0u8; buf_size as usize];
        unsafe { f(x, y, width, height, format, ty, r.pixels.as_mut_ptr() as *mut c_void) };
        r.length = need as i32;
        r.columns = width;
        r.rows = height;
        r
    }

    fn with_cstrings<R>(&mut self, strings: &[String], f: impl FnOnce(&mut Self, &[*const c_char]) -> R) -> Option<R> {
        let cs: Vec<CString> = strings.iter().map(|s| CString::new(s.as_str())).collect::<Result<_, _>>().ok()?;
        let ptrs: Vec<*const c_char> = cs.iter().map(|c| c.as_ptr()).collect();
        Some(f(self, &ptrs))
    }

    pub(crate) fn shader_source_impl(&mut self, shader: u32, sources: Vec<String>) {
        if !self.is_current() {
            return;
        }
        let Some(fp) = self.fns.glShaderSource else { return self.unsupported("glShaderSource") };
        if self
            .with_cstrings(&sources, |_, p| unsafe { fp(shader, p.len() as i32, p.as_ptr() as *const c_void, std::ptr::null()) })
            .is_none()
        {
            self.set_error(GL_INVALID_VALUE);
        }
    }

    pub(crate) fn transform_feedback_varyings_impl(&mut self, program: u32, varyings: Vec<String>, buffer_mode: u32) {
        if !self.is_current() {
            return;
        }
        let Some(fp) = self.fns.glTransformFeedbackVaryings else { return self.unsupported("glTransformFeedbackVaryings") };
        if self
            .with_cstrings(&varyings, |_, p| unsafe { fp(program, p.len() as i32, p.as_ptr() as *const c_void, buffer_mode) })
            .is_none()
        {
            self.set_error(GL_INVALID_VALUE);
        }
    }

    pub(crate) fn get_uniform_indices_impl(&mut self, program: u32, names: Vec<String>) -> Vec<u32> {
        if !self.is_current() {
            return Vec::new();
        }
        let Some(fp) = self.fns.glGetUniformIndices else {
            self.unsupported("glGetUniformIndices");
            return Vec::new();
        };
        let mut out = vec![u32::MAX; names.len()];
        let o = out.as_mut_ptr();
        if self
            .with_cstrings(&names, |_, p| unsafe { fp(program, p.len() as i32, p.as_ptr() as *const c_void, o as *mut c_void) })
            .is_none()
        {
            self.set_error(GL_INVALID_VALUE);
        }
        out
    }

    pub(crate) fn get_vertex_attrib_pointerv_impl(&mut self, index: u32, pname: u32) -> u64 {
        if !self.is_current() {
            return 0;
        }
        let mut p: *mut c_void = std::ptr::null_mut();
        if let Some(f) = self.fns.glGetVertexAttribPointerv {
            unsafe { f(index, pname, &mut p as *mut _ as *mut c_void) };
        }
        p as usize as u64
    }

    pub(crate) fn read_buffer_range_impl(&mut self, target: u32, offset: i64, length: i64) -> Option<Vec<u8>> {
        if !self.is_current() || offset < 0 || length < 0 {
            return None;
        }
        let (map, unmap) = (self.fns.glMapBufferRange?, self.fns.glUnmapBuffer?);
        if length == 0 {
            return Some(Vec::new());
        }
        let p = unsafe { map(target, offset as isize, length as isize, 0x0001 /* MAP_READ_BIT */) };
        if p.is_null() {
            return None;
        }
        let v = unsafe { std::slice::from_raw_parts(p as *const u8, length as usize) }.to_vec();
        unsafe { unmap(target) };
        Some(v)
    }

    fn sync(&self, id: u64) -> Option<*const c_void> {
        self.syncs.get(&id).copied()
    }

    pub(crate) fn fence_sync_impl(&mut self, condition: u32, flags: u32) -> u64 {
        if !self.is_current() {
            return 0;
        }
        let Some(f) = self.fns.glFenceSync else { return 0 };
        let s = unsafe { f(condition, flags) };
        if s.is_null() {
            return 0;
        }
        let id = self.next_sync;
        self.next_sync += 1;
        self.syncs.insert(id, s);
        id
    }

    pub(crate) fn client_wait_sync_impl(&mut self, sync: u64, flags: u32, timeout: u64) -> u32 {
        if !self.is_current() {
            return 0x911D; // WAIT_FAILED
        }
        match (self.sync(sync), self.fns.glClientWaitSync) {
            (Some(s), Some(f)) => unsafe { f(s, flags, timeout) },
            _ => {
                self.set_error(GL_INVALID_VALUE);
                0x911D
            }
        }
    }

    pub(crate) fn wait_sync_impl(&mut self, sync: u64, flags: u32, timeout: u64) {
        if !self.is_current() {
            return;
        }
        match (self.sync(sync), self.fns.glWaitSync) {
            (Some(s), Some(f)) => unsafe { f(s, flags, timeout) },
            _ => self.set_error(GL_INVALID_VALUE),
        }
    }

    pub(crate) fn delete_sync_impl(&mut self, sync: u64) {
        if !self.is_current() {
            return;
        }
        if let (Some(s), Some(f)) = (self.syncs.remove(&sync), self.fns.glDeleteSync) {
            unsafe { f(s) };
        }
    }

    pub(crate) fn is_sync_impl(&mut self, sync: u64) -> u8 {
        self.syncs.contains_key(&sync) as u8
    }

    pub(crate) fn get_synciv_impl(&mut self, sync: u64, pname: u32, count: i32) -> Vec<i32> {
        if !self.is_current() || count < 0 {
            return Vec::new();
        }
        let (Some(s), Some(f)) = (self.sync(sync), self.fns.glGetSynciv) else {
            self.set_error(GL_INVALID_VALUE);
            return Vec::new();
        };
        let mut v = vec![0i32; count as usize + 4];
        let mut len = 0i32;
        unsafe { f(s, pname, count, &mut len as *mut i32 as *mut c_void, v.as_mut_ptr() as *mut c_void) };
        v.truncate(len.clamp(0, count) as usize);
        v
    }
}
