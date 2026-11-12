/*
 * The GL calls latha_gles.c leaves out: pointers whose size depends on GL state, strings and string arrays, the
 * getters that write as many values as a pname has, buffer mapping and syncs. Each matches one hand-written
 * function of latha:gui's gl interface (wit/gl.wit).
 */

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gl_guest.h"
#include "latha_gles.h"

int latha_gles_ready(void)
{
    static int ready = -1;
    if (ready < 0) {
        char const* v = getenv("LATHA_GUI_GL");
        ready = v && *v && strcmp(v, "0") != 0;
    }
    return ready;
}

GL_APICALL GLenum GL_APIENTRY glGetError(void)
{
    if (!latha_gles_ready())
        return GL_NO_ERROR;
    return latha_gui_gl_get_error();
}

/* glGetString's result has to stay valid: keep each distinct answer (they are few: the version strings and the
   extension lists, which grow as extensions are requested). */
struct kept_string {
    GLenum name;
    char* value;
    struct kept_string* next;
};
static struct kept_string* s_kept_strings;

GL_APICALL GLubyte const* GL_APIENTRY glGetString(GLenum name)
{
    if (!latha_gles_ready())
        return NULL;
    gl_guest_string_t s;
    if (!latha_gui_gl_get_string(name, &s))
        return NULL;
    for (struct kept_string* k = s_kept_strings; k; k = k->next) {
        if (k->name == name && strlen(k->value) == s.len && memcmp(k->value, s.ptr, s.len) == 0) {
            gl_guest_string_free(&s);
            return (GLubyte const*)k->value;
        }
    }
    struct kept_string* k = malloc(sizeof *k);
    char* value = malloc(s.len + 1);
    if (!k || !value) {
        free(k);
        free(value);
        gl_guest_string_free(&s);
        return NULL;
    }
    memcpy(value, s.ptr, s.len);
    value[s.len] = 0;
    gl_guest_string_free(&s);
    k->name = name;
    k->value = value;
    k->next = s_kept_strings;
    s_kept_strings = k;
    return (GLubyte const*)value;
}

GL_APICALL void GL_APIENTRY glGetIntegerv(GLenum pname, GLint* data)
{
    if (!latha_gles_ready() || !data)
        return;
    gl_guest_list_s32_t l;
    latha_gui_gl_get_integerv(pname, &l);
    if (l.len)
        memcpy(data, l.ptr, l.len * sizeof(GLint));
    gl_guest_list_s32_free(&l);
}

GL_APICALL void GL_APIENTRY glGetFloatv(GLenum pname, GLfloat* data)
{
    if (!latha_gles_ready() || !data)
        return;
    gl_guest_list_f32_t l;
    latha_gui_gl_get_floatv(pname, &l);
    if (l.len)
        memcpy(data, l.ptr, l.len * sizeof(GLfloat));
    gl_guest_list_f32_free(&l);
}

GL_APICALL void GL_APIENTRY glGetBooleanv(GLenum pname, GLboolean* data)
{
    if (!latha_gles_ready() || !data)
        return;
    gl_guest_list_u8_t l;
    latha_gui_gl_get_booleanv(pname, &l);
    if (l.len)
        memcpy(data, l.ptr, l.len);
    gl_guest_list_u8_free(&l);
}

static GLint get_integer(GLenum pname)
{
    GLint v = 0;
    glGetIntegerv(pname, &v);
    return v;
}

/* Bytes per pixel of the format/type pairs GLES reads and uploads; 0 for one we do not know. */
static int bytes_per_pixel(GLenum format, GLenum type)
{
    int components;
    switch (format) {
    case GL_RED: case GL_RED_INTEGER: case GL_ALPHA: case GL_LUMINANCE: case GL_DEPTH_COMPONENT: components = 1; break;
    case GL_RG: case GL_RG_INTEGER: case GL_LUMINANCE_ALPHA: case GL_DEPTH_STENCIL: components = 2; break;
    case GL_RGB: case GL_RGB_INTEGER: components = 3; break;
    case GL_RGBA: case GL_RGBA_INTEGER: case GL_BGRA_EXT: components = 4; break;
    default: return 0;
    }
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: return components;
    case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: case GL_HALF_FLOAT_OES: return components * 2;
    case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: return components * 4;
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_5_5_5_1: return 2;
    case GL_UNSIGNED_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_10F_11F_11F_REV: case GL_UNSIGNED_INT_5_9_9_9_REV:
    case GL_UNSIGNED_INT_24_8: return 4;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: return 8;
    default: return 0;
    }
}

/* What a pack (glReadPixels) or unpack (glTexImage2D) of width x height reads or writes, by the pixel store state. */
static long long image_size(int pack, GLsizei width, GLsizei height, GLenum format, GLenum type)
{
    int bpp = bytes_per_pixel(format, type);
    if (!bpp || width <= 0 || height <= 0)
        return 0;
    GLint alignment = get_integer(pack ? GL_PACK_ALIGNMENT : GL_UNPACK_ALIGNMENT);
    GLint row_length = get_integer(pack ? GL_PACK_ROW_LENGTH : GL_UNPACK_ROW_LENGTH);
    GLint skip_pixels = get_integer(pack ? GL_PACK_SKIP_PIXELS : GL_UNPACK_SKIP_PIXELS);
    GLint skip_rows = get_integer(pack ? GL_PACK_SKIP_ROWS : GL_UNPACK_SKIP_ROWS);
    if (alignment <= 0)
        alignment = 4;
    long long row_pixels = row_length > 0 ? row_length : width;
    long long stride = (row_pixels * bpp + alignment - 1) / alignment * alignment;
    return (long long)(skip_rows + height - 1) * stride + (long long)(skip_pixels + width) * bpp;
}

/* Reads into `pixels` (buf_size bytes), or into the bound pixel pack buffer at offset `pixels`. */
static void read_pixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLsizei buf_size,
    GLsizei* length, GLsizei* columns, GLsizei* rows, void* pixels)
{
    latha_gui_gl_read_pixels_result_t r;
    latha_gui_gl_read_pixels(x, y, width, height, format, type, buf_size, (uint64_t)(uintptr_t)pixels, &r);
    if (pixels && r.pixels.len)
        memcpy(pixels, r.pixels.ptr, r.pixels.len < (size_t)buf_size ? r.pixels.len : (size_t)buf_size);
    if (length)
        *length = r.length;
    if (columns)
        *columns = r.columns;
    if (rows)
        *rows = r.rows;
    latha_gui_gl_read_pixels_result_free(&r);
}

GL_APICALL void GL_APIENTRY glReadPixelsRobustANGLE(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
    GLenum type, GLsizei bufSize, GLsizei* length, GLsizei* columns, GLsizei* rows, void* pixels)
{
    if (!latha_gles_ready())
        return;
    read_pixels(x, y, width, height, format, type, bufSize, length, columns, rows, pixels);
}

GL_APICALL void GL_APIENTRY glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels)
{
    if (!latha_gles_ready())
        return;
    long long size = get_integer(GL_PIXEL_PACK_BUFFER_BINDING) ? 0 : image_size(1, width, height, format, type);
    if (size > INT_MAX)
        size = INT_MAX;
    read_pixels(x, y, width, height, format, type, (GLsizei)size, NULL, NULL, NULL, pixels);
}

GL_APICALL void GL_APIENTRY glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
    GLint border, GLenum format, GLenum type, void const* pixels)
{
    if (!latha_gles_ready())
        return;
    if (!pixels || get_integer(GL_PIXEL_UNPACK_BUFFER_BINDING)) {
        /* No data (or an offset into the bound unpack buffer, which only the robust entry point's plain form takes). */
        latha_gui_gl_tex_image2d_robust_angle(target, level, internalformat, width, height, border, format, type, 0, NULL);
        return;
    }
    long long size = image_size(0, width, height, format, type);
    if (size > INT_MAX)
        size = INT_MAX;
    gl_guest_list_u8_t l = { (uint8_t*)pixels, (size_t)size };
    latha_gui_gl_tex_image2d_robust_angle(target, level, internalformat, width, height, border, format, type, (int32_t)size, &l);
}

/* The strings of a GLchar const* const* argument, as WIT strings (no copies: they point at the caller's). */
static gl_guest_string_t* string_list(GLsizei count, GLchar const* const* strings, GLint const* lengths)
{
    if (count <= 0)
        return NULL;
    gl_guest_string_t* list = calloc((size_t)count, sizeof *list);
    if (!list)
        return NULL;
    for (GLsizei i = 0; i < count; ++i) {
        char const* s = strings && strings[i] ? strings[i] : "";
        list[i].ptr = (uint8_t*)s;
        list[i].len = lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(s);
    }
    return list;
}

GL_APICALL void GL_APIENTRY glShaderSource(GLuint shader, GLsizei count, GLchar const* const* string, GLint const* length)
{
    if (!latha_gles_ready())
        return;
    gl_guest_list_string_t l = { string_list(count, string, length), count > 0 ? (size_t)count : 0 };
    if (count > 0 && !l.ptr)
        return;
    latha_gui_gl_shader_source(shader, &l);
    free(l.ptr);
}

GL_APICALL void GL_APIENTRY glTransformFeedbackVaryings(GLuint program, GLsizei count, GLchar const* const* varyings, GLenum bufferMode)
{
    if (!latha_gles_ready())
        return;
    gl_guest_list_string_t l = { string_list(count, varyings, NULL), count > 0 ? (size_t)count : 0 };
    if (count > 0 && !l.ptr)
        return;
    latha_gui_gl_transform_feedback_varyings(program, &l, bufferMode);
    free(l.ptr);
}

GL_APICALL void GL_APIENTRY glGetUniformIndices(GLuint program, GLsizei uniformCount, GLchar const* const* uniformNames, GLuint* uniformIndices)
{
    if (!latha_gles_ready())
        return;
    gl_guest_list_string_t l = { string_list(uniformCount, uniformNames, NULL), uniformCount > 0 ? (size_t)uniformCount : 0 };
    if (uniformCount > 0 && !l.ptr)
        return;
    gl_guest_list_u32_t r;
    latha_gui_gl_get_uniform_indices(program, &l, &r);
    free(l.ptr);
    if (uniformIndices && r.len)
        memcpy(uniformIndices, r.ptr, (r.len < (size_t)uniformCount ? r.len : (size_t)uniformCount) * sizeof(GLuint));
    gl_guest_list_u32_free(&r);
}

GL_APICALL void GL_APIENTRY glGetVertexAttribPointervRobustANGLE(GLuint index, GLenum pname, GLsizei bufSize, GLsizei* length, void** pointer)
{
    if (!latha_gles_ready())
        return;
    uint64_t offset = latha_gui_gl_get_vertex_attrib_pointerv(index, pname);
    if (pointer && bufSize >= 1)
        *pointer = (void*)(uintptr_t)offset;
    if (length)
        *length = 1;
}

GL_APICALL void GL_APIENTRY glGetVertexAttribPointerv(GLuint index, GLenum pname, void** pointer)
{
    glGetVertexAttribPointervRobustANGLE(index, pname, 1, NULL, pointer);
}

/* Buffer mapping, for reading (WebGL's getBufferSubData): the host copies the range out and unmaps at once; the
   copy lives until glUnmapBuffer. Mapping for writing is not offered. */
struct mapping {
    GLenum target;
    void* data;
    struct mapping* next;
};
static struct mapping* s_mappings;

GL_APICALL void* GL_APIENTRY glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access)
{
    if (!latha_gles_ready() || !(access & GL_MAP_READ_BIT) || (access & GL_MAP_WRITE_BIT) || length < 0)
        return NULL;
    gl_guest_list_u8_t l;
    if (!latha_gui_gl_read_buffer_range(target, (int64_t)offset, (int64_t)length, &l))
        return NULL;
    struct mapping* m = malloc(sizeof *m);
    void* data = malloc(l.len ? l.len : 1);
    if (!m || !data) {
        free(m);
        free(data);
        gl_guest_list_u8_free(&l);
        return NULL;
    }
    memcpy(data, l.ptr, l.len);
    gl_guest_list_u8_free(&l);
    m->target = target;
    m->data = data;
    m->next = s_mappings;
    s_mappings = m;
    return data;
}

GL_APICALL GLboolean GL_APIENTRY glUnmapBuffer(GLenum target)
{
    for (struct mapping** p = &s_mappings; *p; p = &(*p)->next) {
        if ((*p)->target == target) {
            struct mapping* m = *p;
            *p = m->next;
            free(m->data);
            free(m);
            return GL_TRUE;
        }
    }
    return GL_FALSE;
}

/* Syncs: the host's handles, carried in the GLsync pointer. */
static uint64_t sync_handle(GLsync sync) { return (uint64_t)(uintptr_t)sync; }

GL_APICALL GLsync GL_APIENTRY glFenceSync(GLenum condition, GLbitfield flags)
{
    if (!latha_gles_ready())
        return NULL;
    return (GLsync)(uintptr_t)latha_gui_gl_fence_sync(condition, flags);
}

GL_APICALL GLenum GL_APIENTRY glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout)
{
    if (!latha_gles_ready())
        return GL_WAIT_FAILED;
    return latha_gui_gl_client_wait_sync(sync_handle(sync), flags, timeout);
}

GL_APICALL void GL_APIENTRY glWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout)
{
    if (!latha_gles_ready())
        return;
    latha_gui_gl_wait_sync(sync_handle(sync), flags, timeout);
}

GL_APICALL void GL_APIENTRY glDeleteSync(GLsync sync)
{
    if (!latha_gles_ready() || !sync)
        return;
    latha_gui_gl_delete_sync(sync_handle(sync));
}

GL_APICALL GLboolean GL_APIENTRY glIsSync(GLsync sync)
{
    if (!latha_gles_ready())
        return GL_FALSE;
    return latha_gui_gl_is_sync(sync_handle(sync));
}

GL_APICALL void GL_APIENTRY glGetSynciv(GLsync sync, GLenum pname, GLsizei count, GLsizei* length, GLint* values)
{
    if (!latha_gles_ready())
        return;
    gl_guest_list_s32_t l;
    latha_gui_gl_get_synciv(sync_handle(sync), pname, count, &l);
    size_t n = l.len < (size_t)(count > 0 ? count : 0) ? l.len : (size_t)(count > 0 ? count : 0);
    if (values && n)
        memcpy(values, l.ptr, n * sizeof(GLint));
    if (length)
        *length = (GLsizei)n;
    gl_guest_list_s32_free(&l);
}
