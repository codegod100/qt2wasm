// qt2wasm/convert.h: Qt values <-> wit-bindgen C values.
//
// wit-bindgen's C generator gives every world its own struct names
// (`counter_world_string_t`, `exports_x_list_f64_t`, ...) but the shapes are
// fixed: strings and lists are {ptr, len}, records have one field per WIT
// field, enums are small integers. So the converters here are templates over
// the C type and only name fields.
//
//   Conv<T>::in(C *c)            consume a C value the canonical ABI handed us
//                                (export parameters are owned by the callee)
//   Conv<T>::out(const T&, C *c) write a C value, heap parts malloc'ed
//                                (freed by wit-bindgen's post-return, or by drop)
//   Conv<T>::drop(C *c)          free what out() allocated, for values we pass
//                                to imports (the caller keeps ownership there)
//
// Generated glue uses these; hand-written WIT-first code can too.
#pragma once

#include <QByteArray>
#include <QFlags>
#include <QList>
#include <QMetaEnum>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QStringList>

#if defined(QT_GUI_LIB) || defined(QT2WASM_GUI)
#include <QColor>
#include <QImage>
#define QT2WASM_HAVE_GUI 1
#endif

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <type_traits>
#include <vector>

namespace qt2wasm {

inline void *alloc(size_t n) {
    if (n == 0)
        return nullptr;
    void *p = std::malloc(n);
    if (!p)
        std::abort();
    return p;
}

template <class T, class Enable = void>
struct Conv;

// Primitives (bool, ints, floats) pass straight through.
template <class T>
struct Conv<T, std::enable_if_t<std::is_arithmetic_v<T>>> {
    template <class C> static T in(C *c) { return static_cast<T>(*c); }
    template <class C> static void out(const T &v, C *c) { *c = static_cast<C>(v); }
    template <class C> static void drop(C *) {}
};

template <>
struct Conv<QChar> {
    template <class C> static QChar in(C *c) { return QChar(char16_t(*c)); }  // BMP only
    template <class C> static void out(const QChar &v, C *c) { *c = v.unicode(); }
    template <class C> static void drop(C *) {}
};

template <>
struct Conv<QString> {
    template <class C> static QString in(C *c) {
        QString s = QString::fromUtf8(reinterpret_cast<const char *>(c->ptr), qsizetype(c->len));
        std::free(c->ptr);
        c->ptr = nullptr;
        c->len = 0;
        return s;
    }
    template <class C> static void out(const QString &v, C *c) {
        const QByteArray b = v.toUtf8();
        c->len = size_t(b.size());
        c->ptr = static_cast<uint8_t *>(alloc(c->len));
        if (c->len)
            std::memcpy(c->ptr, b.constData(), c->len);
    }
    template <class C> static void drop(C *c) { std::free(c->ptr); }
};

template <>
struct Conv<QByteArray> {
    template <class C> static QByteArray in(C *c) {
        QByteArray b(reinterpret_cast<const char *>(c->ptr), qsizetype(c->len));
        std::free(c->ptr);
        return b;
    }
    template <class C> static void out(const QByteArray &v, C *c) {
        c->len = size_t(v.size());
        c->ptr = static_cast<uint8_t *>(alloc(c->len));
        if (c->len)
            std::memcpy(c->ptr, v.constData(), c->len);
    }
    template <class C> static void drop(C *c) { std::free(c->ptr); }
};

namespace detail {
template <class L, class T>
struct ListConv {
    template <class C> static L in(C *c) {
        L out;
        out.reserve(qsizetype(c->len));
        for (size_t i = 0; i < c->len; ++i)
            out.push_back(Conv<T>::in(&c->ptr[i]));
        std::free(c->ptr);
        return out;
    }
    template <class C> static void out(const L &v, C *c) {
        using Elem = std::remove_pointer_t<decltype(c->ptr)>;
        c->len = size_t(v.size());
        c->ptr = static_cast<Elem *>(alloc(sizeof(Elem) * c->len));
        size_t i = 0;
        for (const auto &e : v)
            Conv<T>::out(e, &c->ptr[i++]);
    }
    template <class C> static void drop(C *c) {
        for (size_t i = 0; i < c->len; ++i)
            Conv<T>::drop(&c->ptr[i]);
        std::free(c->ptr);
    }
};
}  // namespace detail

template <class T>
struct Conv<QList<T>> : detail::ListConv<QList<T>, T> {};
template <>
struct Conv<QStringList> : detail::ListConv<QStringList, QString> {};
template <class T>
struct Conv<std::vector<T>> {
    template <class C> static std::vector<T> in(C *c) {
        std::vector<T> out;
        out.reserve(c->len);
        for (size_t i = 0; i < c->len; ++i)
            out.push_back(Conv<T>::in(&c->ptr[i]));
        std::free(c->ptr);
        return out;
    }
    template <class C> static void out(const std::vector<T> &v, C *c) {
        detail::ListConv<QList<T>, T>::out(QList<T>(v.begin(), v.end()), c);
    }
    template <class C> static void drop(C *c) { detail::ListConv<QList<T>, T>::drop(c); }
};

// option<T> as a parameter or record field: {bool is_some; T val;}.
// (An option *result* is a `bool f(..., T *ret)` signature; glue handles it.)
template <class T>
struct Conv<std::optional<T>> {
    template <class C> static std::optional<T> in(C *c) {
        if (!c->is_some)
            return std::nullopt;
        return Conv<T>::in(&c->val);
    }
    template <class C> static void out(const std::optional<T> &v, C *c) {
        c->is_some = v.has_value();
        if (v)
            Conv<T>::out(*v, &c->val);
    }
    template <class C> static void drop(C *c) {
        if (c->is_some)
            Conv<T>::drop(&c->val);
    }
};

// Q_ENUM: WIT enum case i is the i-th declared enumerator, whatever its value.
template <class E>
struct Conv<E, std::enable_if_t<std::is_enum_v<E>>> {
    template <class C> static E in(C *c) {
        return static_cast<E>(QMetaEnum::fromType<E>().value(int(*c)));
    }
    template <class C> static void out(const E &v, C *c) {
        const QMetaEnum me = QMetaEnum::fromType<E>();
        for (int i = 0; i < me.keyCount(); ++i)
            if (me.value(i) == int(v)) {
                *c = C(i);
                return;
            }
        *c = C(0);
    }
    template <class C> static void drop(C *) {}
};

// Q_FLAG: WIT flag bit i is the i-th declared enumerator.
template <class E>
struct Conv<QFlags<E>> {
    template <class C> static QFlags<E> in(C *c) {
        const QMetaEnum me = QMetaEnum::fromType<QFlags<E>>();
        QFlags<E> f;
        for (int i = 0; i < me.keyCount(); ++i)
            if (uint64_t(*c) & (uint64_t(1) << i))
                f |= static_cast<E>(me.value(i));
        return f;
    }
    template <class C> static void out(const QFlags<E> &v, C *c) {
        const QMetaEnum me = QMetaEnum::fromType<QFlags<E>>();
        uint64_t bits = 0;
        for (int i = 0; i < me.keyCount(); ++i)
            if (me.value(i) != 0 && v.testFlag(static_cast<E>(me.value(i))))
                bits |= uint64_t(1) << i;
        *c = C(bits);
    }
    template <class C> static void drop(C *) {}
};

// Qt value records (the `types` interface qt2wasm generates).
#define QT2WASM_TRIVIAL_DROP \
    template <class C> static void drop(C *) {}

template <> struct Conv<QPoint> {
    template <class C> static QPoint in(C *c) { return {int(c->x), int(c->y)}; }
    template <class C> static void out(const QPoint &v, C *c) { c->x = v.x(); c->y = v.y(); }
    QT2WASM_TRIVIAL_DROP
};
template <> struct Conv<QPointF> {
    template <class C> static QPointF in(C *c) { return {c->x, c->y}; }
    template <class C> static void out(const QPointF &v, C *c) { c->x = v.x(); c->y = v.y(); }
    QT2WASM_TRIVIAL_DROP
};
template <> struct Conv<QSize> {
    template <class C> static QSize in(C *c) { return {int(c->width), int(c->height)}; }
    template <class C> static void out(const QSize &v, C *c) { c->width = v.width(); c->height = v.height(); }
    QT2WASM_TRIVIAL_DROP
};
template <> struct Conv<QSizeF> {
    template <class C> static QSizeF in(C *c) { return {c->width, c->height}; }
    template <class C> static void out(const QSizeF &v, C *c) { c->width = v.width(); c->height = v.height(); }
    QT2WASM_TRIVIAL_DROP
};
template <> struct Conv<QRect> {
    template <class C> static QRect in(C *c) { return {int(c->x), int(c->y), int(c->width), int(c->height)}; }
    template <class C> static void out(const QRect &v, C *c) {
        c->x = v.x(); c->y = v.y(); c->width = v.width(); c->height = v.height();
    }
    QT2WASM_TRIVIAL_DROP
};
template <> struct Conv<QRectF> {
    template <class C> static QRectF in(C *c) { return {c->x, c->y, c->width, c->height}; }
    template <class C> static void out(const QRectF &v, C *c) {
        c->x = v.x(); c->y = v.y(); c->width = v.width(); c->height = v.height();
    }
    QT2WASM_TRIVIAL_DROP
};
#ifdef QT2WASM_HAVE_GUI
template <> struct Conv<QColor> {
    template <class C> static QColor in(C *c) { return QColor(c->r, c->g, c->b, c->a); }
    template <class C> static void out(const QColor &v, C *c) {
        const QColor s = v.toRgb();
        c->r = uint8_t(s.red()); c->g = uint8_t(s.green()); c->b = uint8_t(s.blue()); c->a = uint8_t(s.alpha());
    }
    QT2WASM_TRIVIAL_DROP
};

// QImage <-> record image { width: u32, height: u32, pixels: list<u8> },
// RGBA8888 with tightly packed rows. A short pixel list gives a null image.
template <> struct Conv<QImage> {
    template <class C> static QImage in(C *c) {
        const size_t w = c->width, h = c->height;
        QImage img;
        if (w && h && c->pixels.len >= w * h * 4) {
            img = QImage(int(w), int(h), QImage::Format_RGBA8888);
            for (size_t y = 0; y < h; ++y)
                std::memcpy(img.scanLine(int(y)), c->pixels.ptr + y * w * 4, w * 4);
        }
        std::free(c->pixels.ptr);
        return img;
    }
    template <class C> static void out(const QImage &v, C *c) {
        const QImage img = v.convertToFormat(QImage::Format_RGBA8888);
        const size_t w = size_t(img.width()), h = size_t(img.height());
        c->width = uint32_t(w);
        c->height = uint32_t(h);
        c->pixels.len = w * h * 4;
        c->pixels.ptr = static_cast<uint8_t *>(alloc(c->pixels.len));
        for (size_t y = 0; y < h; ++y)
            std::memcpy(c->pixels.ptr + y * w * 4, img.constScanLine(int(y)), w * 4);
    }
    template <class C> static void drop(C *c) { std::free(c->pixels.ptr); }
};

#endif  // QT2WASM_HAVE_GUI

#undef QT2WASM_TRIVIAL_DROP

}  // namespace qt2wasm
