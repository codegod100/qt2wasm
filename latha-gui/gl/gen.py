#!/usr/bin/env python3
"""Generates latha:gui's `gl` interface from gl-functions.json.

    gen.py OUT_DIR

writes
    gl.wit          the interface (package latha:gui), for qt2wasm/gui/wit and for guests
    gl_gen.rs       the host side: GL entry points loaded by name, the bindgen Host trait implemented on GlState
    latha_gles.c    the guest side: the GLES 2/3 C API (plus the few ANGLE/EXT/OES entry points WebGL uses) on top
                    of wit-bindgen's C bindings for `gl`, so a GLES program compiles for wasm32-wasi unchanged

gl-functions.json is Ladybird's Libraries/LibCompositing/WebGL/GLFunctions.json (the GL calls its WebGL command
replayer makes, with the size of each pointer argument) plus the calls its Compositor's OpenGLContext makes directly.
Each argument is one of: a value; `string` (a NUL terminated name); a `payload` it reads (an expression for its size in
bytes, `nullable` if it may be null); `out` values it writes (a `payload` of them, or one value); `offset` (a pointer
that is a byte offset into a bound buffer). Functions in CUSTOM are written by hand in gl.rs and latha_gles_custom.c.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
VERSION = "0.2.3"

# type -> (wit, rust ffi, guest C)
SCALARS = {
    "GLenum": ("u32", "u32"),
    "GLuint": ("u32", "u32"),
    "GLbitfield": ("u32", "u32"),
    "GLint": ("s32", "i32"),
    "GLsizei": ("s32", "i32"),
    "GLfloat": ("f32", "f32"),
    "GLboolean": ("u8", "u8"),
    "GLintptr": ("s64", "isize"),
    "GLsizeiptr": ("s64", "isize"),
    "GLuint64": ("u64", "u64"),
    "GLint64": ("s64", "i64"),
    "GLsync": ("u64", "*const c_void"),
}
ELEM_SIZE = {"GLfloat": 4, "GLint": 4, "GLuint": 4, "GLenum": 4, "GLsizei": 4, "GLboolean": 1, "GLchar": 1, "GLint64": 8,
             "void": 1, "GLubyte": 1}

# Hand-written on both sides (gl.rs, latha_gles_custom.c): pointers whose size the JSON cannot say, results the
# host keeps (syncs, mappings), and context management.
CUSTOM = {
    "glGetString", "glGetUniformIndices", "glGetVertexAttribPointervRobustANGLE", "glMapBufferRange",
    "glReadPixelsRobustANGLE", "glShaderSource", "glTransformFeedbackVaryings", "glUnmapBuffer", "glReadPixels",
    "glGetIntegerv", "glGetFloatv", "glGetBooleanv", "glGetError", "glFenceSync", "glClientWaitSync", "glWaitSync",
    "glDeleteSync", "glGetSynciv", "glIsSync", "glTexImage2D",
}
# Uploads a host without ANGLE runs as the plain call, which reads all the pixels the unpack state says: the host
# pads what the app sent to that size (gl.rs pad_unpack).
TEX_UPLOADS = {"glTexImage2DRobustANGLE": "1", "glTexSubImage2DRobustANGLE": "1",
               "glTexImage3DRobustANGLE": "depth", "glTexSubImage3DRobustANGLE": "depth"}
# Vector getters whose value count depends on pname (host: gl.rs value_count).
PNAME_GETTERS = {
    "glGetBooleanvRobustANGLE", "glGetFloatvRobustANGLE", "glGetIntegervRobustANGLE", "glGetInteger64vRobustANGLE",
}
# ANGLE's robust entry points take a bufSize and write a length: a host without ANGLE runs the plain call.
ROBUST_FALLBACK = re.compile(r"^(gl\w+?)RobustANGLE$")
# An extension entry point a host without that extension runs as the core one.
ALIASES = {
    "glBindVertexArrayOES": "glBindVertexArray", "glDeleteVertexArraysOES": "glDeleteVertexArrays",
    "glGenVertexArraysOES": "glGenVertexArrays", "glIsVertexArrayOES": "glIsVertexArray",
    "glDrawArraysInstancedANGLE": "glDrawArraysInstanced", "glDrawElementsInstancedANGLE": "glDrawElementsInstanced",
    "glVertexAttribDivisorANGLE": "glVertexAttribDivisor", "glDrawBuffersEXT": "glDrawBuffers",
}
# Host checks: what a payload expression's GL constants are.
CONSTANTS = {"GL_COLOR": 0x1800}

WIT_KEYWORDS = {"type", "func", "use", "record", "enum", "flags", "variant", "resource", "interface", "world",
                "import", "export", "package", "include", "with", "static", "constructor", "string", "list",
                "option", "result", "bool", "char", "own", "borrow", "async", "future", "stream", "as", "from",
                "u8", "u16", "u32", "u64", "s8", "s16", "s32", "s64", "f32", "f64", "error-context"}
RUST_KEYWORDS = {"type", "ref", "match", "fn", "mod", "use", "in", "loop", "box", "move", "self", "super", "crate",
                 "where", "impl", "trait", "struct", "enum", "const", "static", "mut", "pub", "let", "if", "else",
                 "for", "while", "return", "break", "continue", "as", "dyn", "true", "false", "extern", "unsafe"}


def kebab(name):
    """glTexImage2DRobustANGLE -> tex-image2d-robust-angle; primitiveMode -> primitive-mode."""
    if name.startswith("gl") and name[2:3].isupper():
        name = name[2:]
    name = re.sub(r"(\d)D(?=[A-Z]|$)", r"\1d", name)
    words = re.findall(r"[A-Z]+(?=[A-Z][a-z]|\d|$)\d*[a-z]*\d*[a-z]*|[A-Z]?[a-z]+\d*[a-z]*\d*[a-z]*|[A-Z]\d+[a-z]*|\d+[a-z]*", name)
    assert "".join(words) == name, (name, words)
    out = []
    for w in words:
        w = w.lower()
        if out and w[0].isdigit():
            out[-1] += w
        else:
            out.append(w)
    return "-".join(out)


def snake(name):
    return kebab(name).replace("-", "_")


def wit_ident(name):
    k = kebab(name) if not name.islower() else name
    return f"%{k}" if k in WIT_KEYWORDS else k


def rust_ident(name):
    s = kebab(name).replace("-", "_") if not name.islower() else name
    return f"r#{s}" if s in RUST_KEYWORDS else s


def rust_field(name):
    s = kebab(name).replace("-", "_") if not name.islower() else name
    return s + "_" if s in RUST_KEYWORDS else s


def c_param(t, n):
    return f"{t} {n}" if not t.endswith("*") else f"{t}{n}"


def base_type(t):
    return t.replace("const", "").replace("*", "").strip()


def rust_expr(expr, names):
    """A payload size expression in C (count * 4 * sizeof(GLfloat), (buffer == GL_COLOR ? 4 : 1) * ...) as Rust i64."""
    e = expr
    for t, s in ELEM_SIZE.items():
        e = e.replace(f"sizeof({t})", str(s))
    for c, v in CONSTANTS.items():
        e = re.sub(rf"\b{c}\b", str(v), e)
    e = re.sub(r"\((\w+) == (\w+) \? (\w+) : (\w+)\)", r"(if \1 == \2 { \3 } else { \4 })", e)
    def arg(m):
        w = m.group(0)
        return f"({rust_ident(w)} as i64)" if w in names else w
    e = re.sub(r"\b[A-Za-z_]\w*\b", arg, e)
    e = e.replace("(if (", "(if (")
    return e


class Arg:
    def __init__(self, a):
        self.type = a["type"]
        self.name = a["name"]
        self.payload = a.get("payload")
        self.nullable = a.get("nullable", False)
        self.out = a.get("out", False)
        self.offset = a.get("offset", False)
        self.string = a.get("string", False)
        self.is_ptr = "*" in self.type
        if self.string:
            self.kind = "string"
        elif self.offset:
            self.kind = "offset"
        elif self.out:
            self.kind = "out-payload" if self.payload else "out-value"
        elif self.is_ptr and self.payload:
            self.kind = "in-payload"
        elif self.is_ptr:
            self.kind = "gen-out"  # glGen*: n names written
        else:
            self.kind = "value"


def load():
    with open(os.path.join(HERE, "gl-functions.json")) as f:
        spec = json.load(f)
    funcs = []
    for f in spec:
        if not f["name"].startswith("gl") or f.get("category") == "builtin-command":
            continue
        funcs.append(f)
    return funcs


def wit_value_type(t):
    return SCALARS[t][0]


def gen_wit(funcs):
    lines = [
        f"package latha:gui@{VERSION};",
        "",
        "// Generated by latha-gui/gl/gen.py from gl-functions.json: edit those, not this file.",
        "",
        "/// latha:gui's GL: an app's OpenGL ES 3 calls, run by the host on its own GPU (EGL; ANGLE when the host has it).",
        "///",
        "/// The host makes GL contexts with no window (`create-context`) and runs the calls on the one `make-current`",
        "/// last made current. Names (buffers, textures, programs, ...) are the host's own. Pointers become lists:",
        "/// what a call reads goes in, what it writes comes back. A sync is a handle the host keeps.",
        "///",
        f"/// A host sets the WASI environment variable LATHA_GUI_GL={VERSION} when it has this interface (a host",
        "/// without it traps if called).",
        "interface gl {",
        "    /// A context of client version 2 or 3 (EGL_CONTEXT_CLIENT_VERSION); `attribs` are the app's EGL context",
        "    /// attributes (key, value pairs), which a host without ANGLE leaves out where it cannot honour them.",
        "    /// 0 when the host has no GL.",
        "    create-context: func(client-version: u32, attribs: list<s32>) -> u32;",
        "    /// Makes the context current (0: none). False if there is no such context.",
        "    make-current: func(context: u32) -> bool;",
        "    destroy-context: func(context: u32);",
        "    /// What runs the calls, for logs: \"<GL_RENDERER> (<GL_VERSION>)\", with \"ANGLE\" when it is ANGLE.",
        "    renderer: func() -> string;",
        "",
        "    get-error: func() -> u32;",
        "    get-string: func(name: u32) -> option<string>;",
        "    /// glGet*v: every value pname has (a host knows how many).",
        "    get-integerv: func(pname: u32) -> list<s32>;",
        "    get-floatv: func(pname: u32) -> list<f32>;",
        "    get-booleanv: func(pname: u32) -> list<u8>;",
        "    /// glReadPixels into a list of `buf-size` bytes, or into the bound pixel pack buffer at `offset`.",
        "    record read-pixels-result { length: s32, columns: s32, rows: s32, pixels: list<u8> }",
        "    read-pixels: func(x: s32, y: s32, width: s32, height: s32, format: u32, %type: u32, buf-size: s32, offset: u64) -> read-pixels-result;",
        "    shader-source: func(shader: u32, sources: list<string>);",
        "    transform-feedback-varyings: func(program: u32, varyings: list<string>, buffer-mode: u32);",
        "    get-uniform-indices: func(program: u32, names: list<string>) -> list<u32>;",
        "    get-vertex-attrib-pointerv: func(index: u32, pname: u32) -> u64;",
        "    /// glMapBufferRange for reading: a copy of the range (none on error); the buffer stays unmapped.",
        "    read-buffer-range: func(target: u32, offset: s64, length: s64) -> option<list<u8>>;",
        "    fence-sync: func(condition: u32, %flags: u32) -> u64;",
        "    client-wait-sync: func(sync: u64, %flags: u32, timeout: u64) -> u32;",
        "    wait-sync: func(sync: u64, %flags: u32, timeout: u64);",
        "    delete-sync: func(sync: u64);",
        "    is-sync: func(sync: u64) -> u8;",
        "    get-synciv: func(sync: u64, pname: u32, count: s32) -> list<s32>;",
        "",
    ]
    for f in funcs:
        if f["name"] in CUSTOM:
            continue
        args = [Arg(a) for a in f.get("args", [])]
        params = []
        results = []
        for a in args:
            n = wit_ident(a.name)
            if a.kind == "value":
                params.append(f"{n}: {wit_value_type(a.type)}")
            elif a.kind == "string":
                params.append(f"{n}: string")
            elif a.kind == "offset":
                params.append(f"{n}: u64")
            elif a.kind == "in-payload":
                params.append(f"{n}: {'option<list<u8>>' if a.nullable else 'list<u8>'}")
            elif a.kind == "out-payload":
                results.append((n, "list<u8>"))
            elif a.kind == "out-value":
                results.append((n, wit_value_type(base_type(a.type))))
            elif a.kind == "gen-out":
                results.append((n, "list<u32>"))
        ret = f.get("return", "void")
        if ret != "void":
            results.insert(0, ("%result" if False else "ret", wit_value_type(ret)))
        name = wit_ident(f["name"])
        if len(results) == 0:
            lines.append(f"    {name}: func({', '.join(params)});")
        elif len(results) == 1:
            lines.append(f"    {name}: func({', '.join(params)}) -> {results[0][1]};")
        else:
            rec = f"{kebab(f['name'])}-result"
            fields = ", ".join(f"{n}: {t}" for n, t in results)
            lines.append(f"    record {rec} {{ {fields} }}")
            lines.append(f"    {name}: func({', '.join(params)}) -> {rec};")
    lines.append("}")
    lines.append("")
    lines.append("/// Just the GL calls: for a library an app links in (next to its own world, as two sets of bindings).")
    lines.append("world gl-guest {")
    lines.append("    import gl;")
    lines.append("}")
    return "\n".join(lines) + "\n"


# --- host ----------------------------------------------------------------------------------------------------------

def rust_ffi_type(t):
    if t.endswith("*"):
        return "*const c_void" if "const" in t else "*mut c_void"
    return SCALARS[t][1]


def gen_host(funcs):
    out = [
        "// Generated by latha-gui/gl/gen.py from gl-functions.json: edit those, not this file.",
        "#![allow(non_snake_case, clippy::too_many_arguments, unused_variables, unused_mut)]",
        "use super::gl::{GlState, GL_INVALID_VALUE};",
        "use super::gl_bindings::latha::gui::gl as w;",
        "use std::ffi::{c_void, CString};",
        "",
    ]
    # Function pointer table: every generated function, its robust fallback and aliases, plus the hand-written ones'.
    ptrs = {}
    for f in funcs:
        args = f.get("args", [])
        ptrs[f["name"]] = (f.get("return", "void"), [a["type"] for a in args])
    extra = json.load(open(os.path.join(HERE, "gl-host-extra.json")))
    for f in extra:
        ptrs[f["name"]] = (f.get("return", "void"), [a["type"] for a in f["args"]])
    out.append("pub struct GlFns {")
    for n, (ret, types) in sorted(ptrs.items()):
        r = "" if ret == "void" else f" -> {rust_ffi_type(ret) if ret != 'GLubyte const*' else '*const u8'}"
        out.append(f"    pub {n}: Option<unsafe extern \"C\" fn({', '.join(rust_ffi_type(t) for t in types)}){r}>,")
    out.append("}")
    out.append("")
    out.append("impl GlFns {")
    out.append("    pub fn load(get: &dyn Fn(&str) -> *const c_void) -> GlFns {")
    out.append("        unsafe {")
    out.append("            GlFns {")
    for n in sorted(ptrs):
        out.append(f"                {n}: std::mem::transmute::<*const c_void, _>(get(\"{n}\")),")
    out.append("            }")
    out.append("        }")
    out.append("    }")
    out.append("}")
    out.append("")
    out.append("impl w::Host for GlState {")
    out.append("    fn create_context(&mut self, client_version: u32, attribs: Vec<i32>) -> u32 { self.create_context_impl(client_version, attribs) }")
    out.append("    fn make_current(&mut self, context: u32) -> bool { self.make_current_impl(context) }")
    out.append("    fn destroy_context(&mut self, context: u32) { self.destroy_context_impl(context) }")
    out.append("    fn renderer(&mut self) -> String { self.renderer_impl() }")
    out.append("    fn get_error(&mut self) -> u32 { self.get_error_impl() }")
    out.append("    fn get_string(&mut self, name: u32) -> Option<String> { self.get_string_impl(name) }")
    out.append("    fn get_integerv(&mut self, pname: u32) -> Vec<i32> { self.get_integerv_impl(pname) }")
    out.append("    fn get_floatv(&mut self, pname: u32) -> Vec<f32> { self.get_floatv_impl(pname) }")
    out.append("    fn get_booleanv(&mut self, pname: u32) -> Vec<u8> { self.get_booleanv_impl(pname) }")
    out.append("    fn read_pixels(&mut self, x: i32, y: i32, width: i32, height: i32, format: u32, type_: u32, buf_size: i32, offset: u64) -> w::ReadPixelsResult { self.read_pixels_impl(x, y, width, height, format, type_, buf_size, offset) }")
    out.append("    fn shader_source(&mut self, shader: u32, sources: Vec<String>) { self.shader_source_impl(shader, sources) }")
    out.append("    fn transform_feedback_varyings(&mut self, program: u32, varyings: Vec<String>, buffer_mode: u32) { self.transform_feedback_varyings_impl(program, varyings, buffer_mode) }")
    out.append("    fn get_uniform_indices(&mut self, program: u32, names: Vec<String>) -> Vec<u32> { self.get_uniform_indices_impl(program, names) }")
    out.append("    fn get_vertex_attrib_pointerv(&mut self, index: u32, pname: u32) -> u64 { self.get_vertex_attrib_pointerv_impl(index, pname) }")
    out.append("    fn read_buffer_range(&mut self, target: u32, offset: i64, length: i64) -> Option<Vec<u8>> { self.read_buffer_range_impl(target, offset, length) }")
    out.append("    fn fence_sync(&mut self, condition: u32, flags: u32) -> u64 { self.fence_sync_impl(condition, flags) }")
    out.append("    fn client_wait_sync(&mut self, sync: u64, flags: u32, timeout: u64) -> u32 { self.client_wait_sync_impl(sync, flags, timeout) }")
    out.append("    fn wait_sync(&mut self, sync: u64, flags: u32, timeout: u64) { self.wait_sync_impl(sync, flags, timeout) }")
    out.append("    fn delete_sync(&mut self, sync: u64) { self.delete_sync_impl(sync) }")
    out.append("    fn is_sync(&mut self, sync: u64) -> u8 { self.is_sync_impl(sync) }")
    out.append("    fn get_synciv(&mut self, sync: u64, pname: u32, count: i32) -> Vec<i32> { self.get_synciv_impl(sync, pname, count) }")
    out.append("")
    for f in funcs:
        if f["name"] in CUSTOM:
            continue
        out.extend(gen_host_fn(f))
    out.append("}")
    return "\n".join(out) + "\n"


def rust_wit_type(t):
    return {"u32": "u32", "s32": "i32", "f32": "f32", "u8": "u8", "s64": "i64", "u64": "u64"}[t]


def gen_host_fn(f):
    name = f["name"]
    args = [Arg(a) for a in f.get("args", [])]
    names = {a.name for a in args}
    params = []
    results = []
    for a in args:
        n = rust_ident(a.name)
        if a.kind == "value":
            params.append(f"{n}: {rust_wit_type(wit_value_type(a.type))}")
        elif a.kind == "string":
            params.append(f"{n}: String")
        elif a.kind == "offset":
            params.append(f"{n}: u64")
        elif a.kind == "in-payload":
            params.append(f"{n}: {'Option<Vec<u8>>' if a.nullable else 'Vec<u8>'}")
        elif a.kind == "out-payload":
            results.append((a, "Vec<u8>"))
        elif a.kind == "out-value":
            results.append((a, rust_wit_type(wit_value_type(base_type(a.type)))))
        elif a.kind == "gen-out":
            results.append((a, "Vec<u32>"))
    ret = f.get("return", "void")
    rtype = None
    if ret != "void":
        rtype = rust_wit_type(wit_value_type(ret))
    if len(results) + (1 if rtype else 0) == 0:
        sig_ret = ""
    elif len(results) + (1 if rtype else 0) == 1:
        sig_ret = f" -> {rtype or results[0][1]}"
    else:
        rec = "".join(p.capitalize() for p in kebab(name).split("-")) + "Result"
        sig_ret = f" -> w::{rec}"
    body = []
    robust = ROBUST_FALLBACK.match(name)
    # Inputs: size checks, pointers.
    call_args = []
    if name in TEX_UPLOADS:
        d = TEX_UPLOADS[name]
        d = d if d == "1" else rust_ident(d)
        body.append(f"        let pixels = if self.is_angle {{ pixels }} else {{ self.pad_unpack(pixels, width, height, {d}, format, r#type) }};")
    if name.startswith("glCompressed"):
        body.append("        if image_size < 0 || image_size > buf_size || (data.len() as i64) < image_size as i64 { self.set_error(GL_INVALID_VALUE); return Default::default(); }")
    for a in args:
        n = rust_ident(a.name)
        if a.kind == "value":
            if a.type == "GLintptr" or a.type == "GLsizeiptr":
                call_args.append(f"{n} as isize")
            else:
                call_args.append(n)
        elif a.kind == "string":
            body.append(f"        let {n}_c = match CString::new({n}) {{ Ok(s) => s, Err(_) => {{ self.set_error(GL_INVALID_VALUE); return Default::default(); }} }};")
            call_args.append(f"{n}_c.as_ptr() as *const c_void")
        elif a.kind == "offset":
            call_args.append(f"{n} as usize as *const c_void")
        elif a.kind == "in-payload":
            need = rust_expr(a.payload, names)
            if a.nullable:
                body.append(f"        let {n}_p: *const c_void = match &{n} {{")
                body.append(f"            Some(d) => {{ let need = {need}; if need < 0 || (d.len() as i64) < need {{ self.set_error(GL_INVALID_VALUE); return Default::default(); }} d.as_ptr() as *const c_void }}")
                body.append(f"            None => std::ptr::null(),")
                body.append(f"        }};")
            else:
                body.append(f"        let need = {need}; if need < 0 || ({n}.len() as i64) < need {{ self.set_error(GL_INVALID_VALUE); return Default::default(); }}")
                body.append(f"        let {n}_p = {n}.as_ptr() as *const c_void;")
            call_args.append(f"{n}_p")
        elif a.kind == "out-payload":
            need = rust_expr(a.payload, names)
            body.append(f"        let need = {need}; if need < 0 {{ self.set_error(GL_INVALID_VALUE); return Default::default(); }}")
            # Room past the end: a host without ANGLE writes what pname has, which bufSize may not cover.
            body.append(f"        let mut {n}_buf = vec![0u8; need as usize + 65536];")
            call_args.append(f"{n}_buf.as_mut_ptr() as *mut c_void")
        elif a.kind == "out-value":
            body.append(f"        let mut {n}_v: {rust_wit_type(wit_value_type(base_type(a.type)))} = Default::default();")
            call_args.append(f"&mut {n}_v as *mut _ as *mut c_void")
        elif a.kind == "gen-out":
            count = [x for x in args if x.kind == "value"][0]
            cn = rust_ident(count.name)
            body.append(f"        if {cn} < 0 || {cn} > 1 << 16 {{ self.set_error(GL_INVALID_VALUE); return Default::default(); }}")
            body.append(f"        let mut {n}_v = vec![0u32; {cn} as usize];")
            call_args.append(f"{n}_v.as_mut_ptr() as *mut c_void")
    body.insert(0, "        if !self.is_current() { return Default::default(); }")
    callee = name
    lines = [f"    fn {snake(name)}(&mut self, {', '.join(params)}){sig_ret} {{"]
    lines.extend(body)
    retbind = "let r = " if rtype else ""
    if robust:
        plain = robust.group(1)
        plain_args = _plain_args(name, args, call_args)
        lines.append(f"        {retbind}unsafe {{")
        lines.append(f"            if let (true, Some(fp)) = (self.is_angle, self.fns.{name}) {{ fp({', '.join(call_args)}) }}")
        if plain_args is None:
            lines.append(f"            else {{ self.unsupported(\"{name}\"); Default::default() }}")
        else:
            lines.append(f"            else if let Some(fp) = self.fns.{plain} {{ fp({', '.join(plain_args)}) }}")
            lines.append(f"            else {{ self.unsupported(\"{name}\"); Default::default() }}")
        lines.append("        };")
        # length: what the plain call wrote
        for a, _ in results:
            if a.kind == "out-value" and a.name == "length":
                if name in PNAME_GETTERS:
                    pn = [x for x in args if x.name == "pname"][0]
                    bs = [x for x in args if x.name == "bufSize"][0]
                    lines.append(f"        if !self.is_angle {{ length_v = (self.value_count(pname) as i32).min({rust_ident(bs.name)}); }}")
                elif name.startswith("glGet"):
                    bs = [x for x in args if x.name == "bufSize"][0]
                    lines.append(f"        if !self.is_angle {{ length_v = self.param_count(\"{name}\", {'pname' if any(x.name == 'pname' for x in args) else '0'}).min({rust_ident(bs.name)}); }}")
    else:
        alias = ALIASES.get(name)
        lines.append(f"        {retbind}unsafe {{")
        if alias:
            lines.append(f"            if let Some(fp) = self.fns.{name}.or(self.fns.{alias}) {{ fp({', '.join(call_args)}) }}")
        else:
            lines.append(f"            if let Some(fp) = self.fns.{name} {{ fp({', '.join(call_args)}) }}")
        lines.append(f"            else {{ self.unsupported(\"{name}\"); Default::default() }}")
        lines.append("        };")
    # Results.
    vals = []
    for a, t in results:
        n = rust_ident(a.name)
        if a.kind == "out-payload":
            need = rust_expr(a.payload, names)
            lines.append(f"        {n}_buf.truncate(({need}) as usize);")
            vals.append((a, f"{n}_buf"))
        elif a.kind == "out-value":
            vals.append((a, f"{n}_v"))
        elif a.kind == "gen-out":
            vals.append((a, f"{n}_v"))
    if rtype:
        r = "r as u64" if ret == "GLsync" else "r"
        vals.insert(0, (None, r))
    if len(vals) == 1:
        lines.append(f"        {vals[0][1]}")
    elif len(vals) > 1:
        rec = "".join(p.capitalize() for p in kebab(name).split("-")) + "Result"
        fields = ", ".join(("ret" if a is None else rust_field(a.name)) + ": " + v for a, v in vals)
        lines.append(f"        w::{rec} {{ {fields} }}")
    lines.append("    }")
    lines.append("")
    return lines


def _plain_args(name, args, call_args):
    """The plain call's arguments for a RobustANGLE call: drop bufSize, length, columns, rows."""
    drop = {"bufSize", "length", "columns", "rows"}
    if name.startswith("glCompressed") or name == "glGetInternalformativRobustANGLE":
        # glCompressedTex*Image*D(..., imageSize, data) keeps imageSize; glGetInternalformativ takes a count.
        drop = {"bufSize"} if name.startswith("glCompressed") else {"length"}
    out = []
    for a, c in zip(args, call_args):
        if a.name in drop:
            continue
        out.append(c)
    return out


# --- guest ---------------------------------------------------------------------------------------------------------

def c_wit_scalar(t):
    return {"u32": "uint32_t", "s32": "int32_t", "f32": "float", "u8": "uint8_t", "s64": "int64_t", "u64": "uint64_t"}[t]


def gen_guest(funcs):
    out = [
        "// Generated by latha-gui/gl/gen.py from gl-functions.json: edit those, not this file.",
        "// The GLES C API over latha:gui's gl interface (wit-bindgen's C bindings for the gl-guest world, gl_guest.h).",
        "#include <stdint.h>",
        "#include <stdlib.h>",
        "#include <string.h>",
        "#include \"gl_guest.h\"",
        "#include \"latha_gles.h\"",
        "",
    ]
    for f in funcs:
        if f["name"] in CUSTOM:
            continue
        out.extend(gen_guest_fn(f))
    return "\n".join(out) + "\n"


def gen_guest_fn(f):
    name = f["name"]
    args = [Arg(a) for a in f.get("args", [])]
    ret = f.get("return", "void")
    cret = ret
    params = ", ".join(c_param(a.type, a.name) for a in args) or "void"
    lines = [f"GL_APICALL {cret} GL_APIENTRY {name}({params})", "{"]
    call = []
    pre = []
    results = []
    for a in args:
        n = a.name
        if a.kind == "value":
            if a.type in ("GLintptr", "GLsizeiptr"):
                call.append(f"(int64_t){n}")
            else:
                call.append(n)
        elif a.kind == "string":
            pre.append(f"    gl_guest_string_t {n}_s = {{ (uint8_t*){n}, {n} ? strlen((char const*){n}) : 0 }};")
            call.append(f"&{n}_s")
        elif a.kind == "offset":
            call.append(f"(uint64_t)(uintptr_t){n}")
        elif a.kind == "in-payload":
            if a.nullable:
                # option<list<u8>>: wit-bindgen's C takes a pointer to the list, NULL for none.
                pre.append(f"    gl_guest_list_u8_t {n}_l = {{ (uint8_t*){n}, {n} ? (size_t)({a.payload}) : 0 }};")
                call.append(f"{n} ? &{n}_l : NULL")
            else:
                pre.append(f"    gl_guest_list_u8_t {n}_l = {{ (uint8_t*){n}, (size_t)({a.payload}) }};")
                call.append(f"&{n}_l")
        else:
            results.append(a)
    if not LATHA_READY:
        pass
    fn = "latha_gui_gl_" + snake(name)
    nres = len(results) + (1 if ret != "void" else 0)
    lines.extend(pre)
    lines.append("    if (!latha_gles_ready()) " + ("return 0;" if ret != "void" else "return;"))
    if nres == 0:
        lines.append(f"    {fn}({', '.join(call)});")
    elif nres == 1 and ret != "void":
        lines.append(f"    return ({cret}){fn}({', '.join(call)});")
    elif nres == 1:
        a = results[0]
        lines.extend(guest_result(fn, call, a, f"r"))
    else:
        rec = f"latha_gui_gl_{snake(name)}_result_t"
        lines.append(f"    {rec} r;")
        lines.append(f"    {fn}({', '.join(call + ['&r'])});")
        for a in results:
            lines.extend(guest_store(a, f"r.{guest_field(a.name)}"))
        if ret != "void":
            lines.append(f"    return ({cret})r.ret;")
    lines.append("}")
    lines.append("")
    return lines


def guest_field(n):
    s = kebab(n).replace("-", "_") if not n.islower() else n
    return s


def guest_result(fn, call, a, var):
    if a.kind == "out-value":
        t = c_wit_scalar(wit_value_type(base_type(a.type)))
        return [f"    {t} {var} = {fn}({', '.join(call)});", f"    if ({a.name}) *{a.name} = {var};"]
    if a.kind == "out-payload":
        return [f"    gl_guest_list_u8_t {var};", f"    {fn}({', '.join(call + ['&' + var])});"] + guest_store(a, var)
    if a.kind == "gen-out":
        return [f"    gl_guest_list_u32_t {var};", f"    {fn}({', '.join(call + ['&' + var])});"] + guest_store(a, var)
    raise ValueError(a.kind)


def guest_store(a, v):
    n = a.name
    if a.kind == "out-value":
        return [f"    if ({n}) *{n} = {v};"]
    if a.kind == "out-payload":
        return [f"    if ({n}) memcpy({n}, {v}.ptr, {v}.len < (size_t)({a.payload}) ? {v}.len : (size_t)({a.payload}));",
                f"    gl_guest_list_u8_free(&{v});"]
    if a.kind == "gen-out":
        return [f"    if ({n}) memcpy({n}, {v}.ptr, {v}.len * sizeof(GLuint));", f"    gl_guest_list_u32_free(&{v});"]
    raise ValueError(a.kind)


LATHA_READY = True


def main():
    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)
    funcs = load()
    with open(os.path.join(out_dir, "gl.wit"), "w") as f:
        f.write(gen_wit(funcs))
    with open(os.path.join(out_dir, "gl_gen.rs"), "w") as f:
        f.write(gen_host(funcs))
    with open(os.path.join(out_dir, "latha_gles.c"), "w") as f:
        f.write(gen_guest(funcs))


if __name__ == "__main__":
    main()
