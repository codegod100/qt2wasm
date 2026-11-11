// qt2wasmgui: Qt's platform layer for a GUI component (`qt2wasm build --gui`).
//
// The component exports latha:gui/app (gui/wit/gui.wit) and imports
// latha:gui/host. This file implements both sides of that for Qt:
//
//   - a QPA platform plugin ("qt2wasmgui"): one screen the size of the host's
//     window, raster backing stores, a compositor that stacks the visible
//     top-level windows (the first normal one fills the screen; popups and
//     tool tips float at their positions) into one frame, the clipboard,
//     cursors and the color scheme through host calls;
//   - the app exports: start() runs the program's qt2wasm_gui_main(), the
//     input exports become QWindowSystemInterface events, paint() composes
//     the damaged parts of the frame and frame-pixels() hands them out.
//
// Geometry: Qt sees logical pixels (device pixels / scale), as on Wayland;
// backing stores and the frame are device pixels, so text stays sharp at
// any scale. Window system events are delivered synchronously: the
// component's event dispatcher never blocks and has no window system queue.
#include "qt2wasmgui.h"

#include <qt2wasm/gui.h>
#include <qt2wasm/runtime.h>
#include "wasieventdispatcher.h"

#include QT2WASM_GUI_BINDINGS_H

#if QT_CONFIG(clipboard)
#include <QClipboard>
#endif
#include <QCursor>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QInputMethodEvent>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QWindow>
#include <QtPlugin>
#include <qpa/qplatformbackingstore.h>
#include <qpa/qplatformclipboard.h>
#include <qpa/qplatformcursor.h>
#include <qpa/qplatformintegration.h>
#include <qpa/qplatformscreen.h>
#include <qpa/qplatformtheme.h>
#include <qpa/qplatformwindow.h>
#include <qpa/qwindowsysteminterface.h>
#include <QtGui/private/qfreetypefontdatabase_p.h>
#include <QtCore/private/qcoreapplication_p.h>
#include <QtGui/private/qguiapplication_p.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

Q_IMPORT_PLUGIN(Qt2WasmGuiIntegrationPlugin)

namespace {

// --- strings across the boundary ---------------------------------------------

QString fromWit(gui_string_t *s, bool free = true) {
    QString out = QString::fromUtf8(reinterpret_cast<const char *>(s->ptr), qsizetype(s->len));
    // Under wasmtime a zero-length argument is never allocated: free only real ones.
    if (free && s->len)
        gui_string_free(s);
    return out;
}

void toWit(gui_string_t *ret, const QByteArray &bytes) {
    ret->len = size_t(bytes.size());
    ret->ptr = nullptr;
    if (!bytes.isEmpty()) {
        ret->ptr = static_cast<uint8_t *>(std::malloc(bytes.size()));
        std::memcpy(ret->ptr, bytes.constData(), bytes.size());
    }
}

void toWit(gui_string_t *ret, const QString &s) { toWit(ret, s.toUtf8()); }

QString hostText(void (*get)(gui_string_t *)) {
    gui_string_t s{};
    get(&s);
    return fromWit(&s);
}

void hostSetText(void (*set)(gui_string_t *), const QString &text) {
    QByteArray b = text.toUtf8();
    gui_string_t s{reinterpret_cast<uint8_t *>(b.data()), size_t(b.size())};
    set(&s);
}

ulong now() {
    static QElapsedTimer clock;
    if (!clock.isValid())
        clock.start();
    return ulong(clock.elapsed());
}

class GuiScreen;
class GuiWindow;

// --- shared state -----------------------------------------------------------

struct State {
    QSize device{800, 600};  // the host surface, device pixels
    qreal scale = 1.0;
    bool dark = false;
    bool resized = false;    // resize() was called at least once
    GuiScreen *screen = nullptr;
    QList<GuiWindow *> stack;  // visible top-levels, bottom to top
    QImage frame;              // device pixels, ARGB32_Premultiplied
    QRegion damage;            // device pixels, waiting for paint()
    bool frameRequested = false;
    QPointer<QWindow> grab;    // the window a button press went to
    QPointer<QWindow> under;   // the window the pointer is over
    Qt::MouseButtons buttons;
    bool focused = true;
    bool quitRequested = false;
    QSize wish;                // the main window's own size, before it filled the screen
    bool hadWindow = false;    // a main window was shown
} S;

QSize logicalSize() {
    return QSize(qMax(1, int(std::ceil(S.device.width() / S.scale))),
                 qMax(1, int(std::ceil(S.device.height() / S.scale))));
}

QRect toDevice(const QRect &r) {
    const qreal s = S.scale;
    const int x0 = int(std::floor(r.left() * s)), y0 = int(std::floor(r.top() * s));
    const int x1 = int(std::ceil((r.right() + 1) * s)), y1 = int(std::ceil((r.bottom() + 1) * s));
    return QRect(x0, y0, x1 - x0, y1 - y0);
}

void requestFrame() {
    if (S.frameRequested)
        return;
    S.frameRequested = true;
    latha_gui_host_request_frame();
}

void damageDevice(const QRegion &r) {
    S.damage += r & QRect(QPoint(), S.device);
    requestFrame();
}

// --- screen -------------------------------------------------------------------

class GuiCursor : public QPlatformCursor {
public:
    void changeCursor(QCursor *c, QWindow *) override {
        latha_gui_types_cursor_t shape = LATHA_GUI_TYPES_CURSOR_DEFAULT;
        switch (c ? c->shape() : Qt::ArrowCursor) {
        case Qt::IBeamCursor: shape = LATHA_GUI_TYPES_CURSOR_TEXT; break;
        case Qt::PointingHandCursor: shape = LATHA_GUI_TYPES_CURSOR_POINTER; break;
        case Qt::CrossCursor: shape = LATHA_GUI_TYPES_CURSOR_CROSSHAIR; break;
        case Qt::WaitCursor: shape = LATHA_GUI_TYPES_CURSOR_WAIT; break;
        case Qt::BusyCursor: shape = LATHA_GUI_TYPES_CURSOR_PROGRESS; break;
        case Qt::WhatsThisCursor: shape = LATHA_GUI_TYPES_CURSOR_HELP; break;
        case Qt::SizeAllCursor: shape = LATHA_GUI_TYPES_CURSOR_MOVE; break;
        case Qt::ForbiddenCursor: shape = LATHA_GUI_TYPES_CURSOR_NOT_ALLOWED; break;
        case Qt::OpenHandCursor: shape = LATHA_GUI_TYPES_CURSOR_GRAB; break;
        case Qt::ClosedHandCursor: case Qt::DragMoveCursor: shape = LATHA_GUI_TYPES_CURSOR_GRABBING; break;
        case Qt::SizeHorCursor: shape = LATHA_GUI_TYPES_CURSOR_EW_RESIZE; break;
        case Qt::SizeVerCursor: shape = LATHA_GUI_TYPES_CURSOR_NS_RESIZE; break;
        case Qt::SizeBDiagCursor: shape = LATHA_GUI_TYPES_CURSOR_NESW_RESIZE; break;
        case Qt::SizeFDiagCursor: shape = LATHA_GUI_TYPES_CURSOR_NWSE_RESIZE; break;
        case Qt::SplitHCursor: shape = LATHA_GUI_TYPES_CURSOR_COL_RESIZE; break;
        case Qt::SplitVCursor: shape = LATHA_GUI_TYPES_CURSOR_ROW_RESIZE; break;
        case Qt::BlankCursor: shape = LATHA_GUI_TYPES_CURSOR_NONE; break;
        default: break;
        }
        latha_gui_host_set_cursor(shape);
    }
    QPoint pos() const override { return m_pos; }
    void setPos(const QPoint &p) override { m_pos = p; }
    QPoint m_pos;
};

class GuiScreen : public QPlatformScreen {
public:
    QRect geometry() const override { return QRect(QPoint(), logicalSize()); }
    int depth() const override { return 32; }
    QImage::Format format() const override { return QImage::Format_ARGB32_Premultiplied; }
    qreal devicePixelRatio() const override { return S.scale; }
    QDpi logicalDpi() const override { return QDpi(96, 96); }
    QDpi logicalBaseDpi() const override { return QDpi(96, 96); }
    QSizeF physicalSize() const override {
        const QSize s = logicalSize();
        return QSizeF(s.width() * 25.4 / 96, s.height() * 25.4 / 96);
    }
    qreal refreshRate() const override { return 60; }
    QString name() const override { return QStringLiteral("qt2wasmgui"); }
    QPlatformCursor *cursor() const override { return const_cast<GuiCursor *>(&m_cursor); }
    QWindow *topLevelAt(const QPoint &p) const override;
    GuiCursor m_cursor;
};

// --- windows ---------------------------------------------------------------------

class GuiBackingStore;

bool floats(QWindow *w) {
    const Qt::WindowType t = w->type();
    return t == Qt::Popup || t == Qt::ToolTip || t == Qt::Tool || t == Qt::SplashScreen ||
           t == Qt::Dialog || t == Qt::Sheet || t == Qt::Drawer;
}

class GuiWindow : public QPlatformWindow {
public:
    explicit GuiWindow(QWindow *w) : QPlatformWindow(w) {}
    ~GuiWindow() override { hide(); }

    GuiBackingStore *store = nullptr;
    bool shown = false;

    /// The first visible non-floating top-level fills the screen.
    bool fills() const {
        for (GuiWindow *g : S.stack)
            if (!floats(g->window()))
                return g == this;
        return false;
    }

    QRect geometry() const override { return QPlatformWindow::geometry(); }

    void setGeometry(const QRect &r) override {
        QRect g = r;
        if (shown && fills())
            g = QRect(QPoint(), logicalSize());
        const QRect old = QPlatformWindow::geometry();
        QPlatformWindow::setGeometry(g);
        if (shown)
            damageDevice(toDevice(old) | toDevice(g));
        QWindowSystemInterface::handleGeometryChange(window(), g);
        if (shown && g.size() != old.size())
            expose();
    }

    void expose() {
        QWindowSystemInterface::handleExposeEvent(window(), QRect(QPoint(), geometry().size()));
    }

    void show();
    void hide();

    void setVisible(bool visible) override { visible ? show() : hide(); }
    bool isExposed() const override { return shown; }
    bool isActive() const override { return S.focused && QGuiApplication::focusWindow() == window(); }
    qreal devicePixelRatio() const override { return S.scale; }
    WId winId() const override { return WId(this); }

    void setWindowTitle(const QString &title) override {
        if (fills())
            hostSetText(latha_gui_host_set_title, title);
    }

    void requestActivateWindow() override {
        QWindowSystemInterface::handleFocusWindowChanged(window(), Qt::ActiveWindowFocusReason);
    }

    void raise() override {
        if (S.stack.removeOne(this)) {
            S.stack.append(this);
            damageDevice(toDevice(geometry()));
        }
    }

    void lower() override {
        if (S.stack.removeOne(this)) {
            S.stack.prepend(this);
            damageDevice(toDevice(geometry()));
        }
    }

    // QWindow::requestUpdate() (QRasterWindow::update() too) is paced by the
    // host's frames: paint() delivers the update request first, then composes.
    bool updatePending = false;
    void requestUpdate() override {
        updatePending = true;
        requestFrame();
    }

    void propagateSizeHints() override {
        if (!fills())
            return;
        const QSize m = window()->minimumSize();
        latha_gui_types_size_t min{uint32_t(std::ceil(m.width() * S.scale)), uint32_t(std::ceil(m.height() * S.scale))};
        latha_gui_host_set_min_size(&min);
    }
};

class GuiBackingStore : public QPlatformBackingStore {
public:
    explicit GuiBackingStore(QWindow *w) : QPlatformBackingStore(w) {
        if (auto *g = static_cast<GuiWindow *>(w->handle()))
            g->store = this;
    }
    ~GuiBackingStore() override {
        if (auto *g = static_cast<GuiWindow *>(window()->handle()); g && g->store == this)
            g->store = nullptr;
    }

    QPaintDevice *paintDevice() override { return &image; }

    void resize(const QSize &size, const QRegion &) override {
        const qreal dpr = S.scale;
        const QSize px(qMax(1, int(std::ceil(size.width() * dpr))), qMax(1, int(std::ceil(size.height() * dpr))));
        if (image.size() == px && image.devicePixelRatio() == dpr)
            return;
        image = QImage(px, QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);
    }

    void beginPaint(const QRegion &region) override {
        if (!image.hasAlphaChannel())
            return;
        QPainter p(&image);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        for (const QRect &r : region)
            p.fillRect(r, Qt::transparent);
    }

    void flush(QWindow *w, const QRegion &region, const QPoint &offset) override {
        // Child windows have no handle of their own here; flush their top-level.
        QWindow *top = w;
        QPoint at = offset;
        while (top->parent()) {
            at += top->position();
            top = top->parent();
        }
        auto *g = static_cast<GuiWindow *>(top->handle());
        if (!g || !g->shown)
            return;
        const QPoint origin = g->geometry().topLeft();
        QRegion dev;
        for (const QRect &r : region)
            dev += toDevice(r.translated(at + origin));
        damageDevice(dev);
    }

    QImage toImage() const override { return image; }

    QImage image;
};

QWindow *GuiScreen::topLevelAt(const QPoint &p) const {
    for (auto it = S.stack.crbegin(); it != S.stack.crend(); ++it)
        if ((*it)->geometry().contains(p))
            return (*it)->window();
    return nullptr;
}

void GuiWindow::show() {
    if (shown)
        return;
    shown = true;
    S.stack.append(this);
    if (fills()) {
        if (!S.wish.isValid())
            S.wish = QPlatformWindow::geometry().size();
        S.hadWindow = true;
        QPlatformWindow::setGeometry(QRect(QPoint(), logicalSize()));
        QWindowSystemInterface::handleGeometryChange(window(), geometry());
        hostSetText(latha_gui_host_set_title, window()->title());
        propagateSizeHints();
    }
    damageDevice(toDevice(geometry()));
    expose();
    if (!floats(window()) || window()->type() == Qt::Dialog)
        QWindowSystemInterface::handleFocusWindowChanged(window(), Qt::ActiveWindowFocusReason);
}

void GuiWindow::hide() {
    if (!shown)
        return;
    shown = false;
    S.stack.removeOne(this);
    damageDevice(toDevice(geometry()));
    QWindowSystemInterface::handleExposeEvent(window(), QRegion());
    if (QGuiApplication::focusWindow() == window()) {
        QWindow *next = nullptr;
        for (auto it = S.stack.crbegin(); it != S.stack.crend() && !next; ++it)
            if ((*it)->window()->type() != Qt::ToolTip)
                next = (*it)->window();
        QWindowSystemInterface::handleFocusWindowChanged(next, Qt::ActiveWindowFocusReason);
    }
}

// --- clipboard, theme, integration -------------------------------------------

#if QT_CONFIG(clipboard)
class GuiClipboard : public QPlatformClipboard {
public:
    QMimeData *mimeData(QClipboard::Mode mode) override {
        if (!supportsMode(mode))
            return nullptr;
        QMimeData *&d = mode == QClipboard::Selection ? m_selection : m_clipboard;
        if (!d)
            d = new QMimeData;
        d->setText(hostText(mode == QClipboard::Selection ? latha_gui_host_primary_text : latha_gui_host_clipboard_text));
        return d;
    }
    void setMimeData(QMimeData *data, QClipboard::Mode mode) override {
        if (!supportsMode(mode))
            return;
        const QString text = data ? data->text() : QString();
        hostSetText(mode == QClipboard::Selection ? latha_gui_host_set_primary_text : latha_gui_host_set_clipboard_text, text);
        delete data;
        emitChanged(mode);
    }
    bool supportsMode(QClipboard::Mode mode) const override {
        return mode == QClipboard::Clipboard || mode == QClipboard::Selection;
    }
    QMimeData *m_clipboard = nullptr;
    QMimeData *m_selection = nullptr;
};
#endif

class GuiTheme : public QPlatformTheme {
public:
    Qt::ColorScheme colorScheme() const override { return S.dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light; }
};

class GuiIntegration : public QPlatformIntegration {
public:
    GuiIntegration() { QWindowSystemInterface::setSynchronousWindowSystemEvents(true); }
    ~GuiIntegration() override {
        if (S.screen)
            QWindowSystemInterface::handleScreenRemoved(S.screen);
        S.screen = nullptr;
    }

    void initialize() override {
        S.screen = new GuiScreen;
        QWindowSystemInterface::handleScreenAdded(S.screen, true);
    }

    bool hasCapability(Capability cap) const override {
        switch (cap) {
        case MultipleWindows: case NonFullScreenWindows: case WindowActivation: return true;
        case ThreadedPixmaps: case OpenGL: case ThreadedOpenGL: case RasterGLSurface: return false;
        default: return QPlatformIntegration::hasCapability(cap);
        }
    }

    QPlatformWindow *createPlatformWindow(QWindow *w) const override {
        auto *g = new GuiWindow(w);
        g->setGeometry(w->geometry());
        return g;
    }
    QPlatformBackingStore *createPlatformBackingStore(QWindow *w) const override { return new GuiBackingStore(w); }
    QAbstractEventDispatcher *createEventDispatcher() const override { return new qt2wasm::WasiEventDispatcher; }
    QPlatformFontDatabase *fontDatabase() const override { return &m_fonts; }
#if QT_CONFIG(clipboard)
    QPlatformClipboard *clipboard() const override { return &m_clipboard; }
#endif
    QStringList themeNames() const override { return {QStringLiteral("qt2wasmgui")}; }
    QPlatformTheme *createPlatformTheme(const QString &) const override { return new GuiTheme; }
    void beep() const override { latha_gui_host_bell(); }
    void quit() const override { S.quitRequested = true; }

    mutable QFreeTypeFontDatabase m_fonts;
#if QT_CONFIG(clipboard)
    mutable GuiClipboard m_clipboard;
#endif
};

// Quit events (QGuiApplication::quit(), the last window closing) end the app:
// there is no exec() to return from, so tell the host.
class QuitFilter : public QObject {
public:
    bool eventFilter(QObject *o, QEvent *e) override {
        if (o == qApp && e->type() == QEvent::Quit)
            S.quitRequested = true;
        return false;
    }
};

// --- after every export -------------------------------------------------------

/// Like qt2wasm::Call, plus what the host needs to hear about afterwards.
struct GuiCall {
    qt2wasm::Call call;
    ~GuiCall() {
        QWindowSystemInterface::sendWindowSystemEvents(QEventLoop::AllEvents);
        qt2wasm::pump();
        // Qt only quits on the last window closing inside exec(); do it here.
        if (S.hadWindow && QGuiApplication::quitOnLastWindowClosed()) {
            bool any = false;
            for (GuiWindow *g : std::as_const(S.stack))
                any = any || !floats(g->window());
            if (!any)
                S.quitRequested = true;
        }
        if (S.quitRequested) {
            S.quitRequested = false;
            latha_gui_host_quit(0);
            return;
        }
        auto *d = qobject_cast<qt2wasm::WasiEventDispatcher *>(QCoreApplication::eventDispatcher());
        const qint64 next = d ? d->msUntilNextTimer() : -1;
        if (next >= 0)
            latha_gui_host_request_tick(uint32_t(qMin<qint64>(next, 0x7fffffff)));
    }
};

// --- input --------------------------------------------------------------------

Qt::KeyboardModifiers qtMods(latha_gui_types_modifiers_t m) {
    Qt::KeyboardModifiers q;
    if (m & LATHA_GUI_TYPES_MODIFIERS_SHIFT) q |= Qt::ShiftModifier;
    if (m & LATHA_GUI_TYPES_MODIFIERS_CTRL) q |= Qt::ControlModifier;
    if (m & LATHA_GUI_TYPES_MODIFIERS_ALT) q |= Qt::AltModifier;
    if (m & LATHA_GUI_TYPES_MODIFIERS_META) q |= Qt::MetaModifier;
    if (m & LATHA_GUI_TYPES_MODIFIERS_KEYPAD) q |= Qt::KeypadModifier;
    return q;
}

Qt::MouseButtons qtButtons(latha_gui_types_buttons_t b) {
    Qt::MouseButtons q;
    if (b & LATHA_GUI_TYPES_BUTTONS_LEFT) q |= Qt::LeftButton;
    if (b & LATHA_GUI_TYPES_BUTTONS_RIGHT) q |= Qt::RightButton;
    if (b & LATHA_GUI_TYPES_BUTTONS_MIDDLE) q |= Qt::MiddleButton;
    if (b & LATHA_GUI_TYPES_BUTTONS_BACK) q |= Qt::BackButton;
    if (b & LATHA_GUI_TYPES_BUTTONS_FORWARD) q |= Qt::ForwardButton;
    return q;
}

Qt::MouseButton qtButton(latha_gui_types_button_t b) {
    switch (b) {
    case LATHA_GUI_TYPES_BUTTON_LEFT: return Qt::LeftButton;
    case LATHA_GUI_TYPES_BUTTON_RIGHT: return Qt::RightButton;
    case LATHA_GUI_TYPES_BUTTON_MIDDLE: return Qt::MiddleButton;
    case LATHA_GUI_TYPES_BUTTON_BACK: return Qt::BackButton;
    case LATHA_GUI_TYPES_BUTTON_FORWARD: return Qt::ForwardButton;
    default: return Qt::NoButton;
    }
}

// X11 keysym -> Qt::Key (the subset a keyboard sends; Latin-1 maps to itself).
int qtKey(uint32_t ks, bool *keypad) {
    *keypad = false;
    struct K { uint32_t ks; int key; bool kp; };
    static const K table[] = {
        {0xff08, Qt::Key_Backspace, false}, {0xff09, Qt::Key_Tab, false}, {0xfe20, Qt::Key_Backtab, false},
        {0xff0d, Qt::Key_Return, false}, {0xff13, Qt::Key_Pause, false}, {0xff14, Qt::Key_ScrollLock, false},
        {0xff15, Qt::Key_SysReq, false}, {0xff1b, Qt::Key_Escape, false}, {0xffff, Qt::Key_Delete, false},
        {0xff50, Qt::Key_Home, false}, {0xff51, Qt::Key_Left, false}, {0xff52, Qt::Key_Up, false},
        {0xff53, Qt::Key_Right, false}, {0xff54, Qt::Key_Down, false}, {0xff55, Qt::Key_PageUp, false},
        {0xff56, Qt::Key_PageDown, false}, {0xff57, Qt::Key_End, false}, {0xff58, Qt::Key_Home, false},
        {0xff60, Qt::Key_Select, false}, {0xff61, Qt::Key_Print, false}, {0xff62, Qt::Key_Execute, false},
        {0xff63, Qt::Key_Insert, false}, {0xff65, Qt::Key_Undo, false}, {0xff66, Qt::Key_Redo, false},
        {0xff67, Qt::Key_Menu, false}, {0xff68, Qt::Key_Find, false}, {0xff69, Qt::Key_Cancel, false},
        {0xff6a, Qt::Key_Help, false}, {0xff7f, Qt::Key_NumLock, false}, {0xffe5, Qt::Key_CapsLock, false},
        {0xffe1, Qt::Key_Shift, false}, {0xffe2, Qt::Key_Shift, false}, {0xffe3, Qt::Key_Control, false},
        {0xffe4, Qt::Key_Control, false}, {0xffe7, Qt::Key_Meta, false}, {0xffe8, Qt::Key_Meta, false},
        {0xffe9, Qt::Key_Alt, false}, {0xffea, Qt::Key_Alt, false}, {0xffeb, Qt::Key_Super_L, false},
        {0xffec, Qt::Key_Super_R, false}, {0xffed, Qt::Key_Hyper_L, false}, {0xffee, Qt::Key_Hyper_R, false},
        {0xfe03, Qt::Key_AltGr, false},
        // keypad
        {0xff8d, Qt::Key_Enter, true}, {0xff95, Qt::Key_Home, true}, {0xff96, Qt::Key_Left, true},
        {0xff97, Qt::Key_Up, true}, {0xff98, Qt::Key_Right, true}, {0xff99, Qt::Key_Down, true},
        {0xff9a, Qt::Key_PageUp, true}, {0xff9b, Qt::Key_PageDown, true}, {0xff9c, Qt::Key_End, true},
        {0xff9d, Qt::Key_Clear, true}, {0xff9e, Qt::Key_Insert, true}, {0xff9f, Qt::Key_Delete, true},
        {0xffbd, Qt::Key_Equal, true}, {0xffaa, Qt::Key_Asterisk, true}, {0xffab, Qt::Key_Plus, true},
        {0xffac, Qt::Key_Comma, true}, {0xffad, Qt::Key_Minus, true}, {0xffae, Qt::Key_Period, true},
        {0xffaf, Qt::Key_Slash, true}, {0xff80, Qt::Key_Space, true}, {0xff89, Qt::Key_Tab, true},
        // media / XF86
        {0x1008ff11, Qt::Key_VolumeDown, false}, {0x1008ff12, Qt::Key_VolumeMute, false},
        {0x1008ff13, Qt::Key_VolumeUp, false}, {0x1008ff14, Qt::Key_MediaPlay, false},
        {0x1008ff15, Qt::Key_MediaStop, false}, {0x1008ff16, Qt::Key_MediaPrevious, false},
        {0x1008ff17, Qt::Key_MediaNext, false}, {0x1008ff26, Qt::Key_Back, false},
        {0x1008ff27, Qt::Key_Forward, false}, {0x1008ff29, Qt::Key_Refresh, false},
        {0x1008ff57, Qt::Key_Copy, false}, {0x1008ff58, Qt::Key_Cut, false}, {0x1008ff6d, Qt::Key_Paste, false},
    };
    for (const K &k : table)
        if (k.ks == ks) {
            *keypad = k.kp;
            return k.key;
        }
    if (ks >= 0xffbe && ks <= 0xffe0)  // F1..F35
        return Qt::Key_F1 + int(ks - 0xffbe);
    if (ks >= 0xffb0 && ks <= 0xffb9) {  // KP_0..KP_9
        *keypad = true;
        return Qt::Key_0 + int(ks - 0xffb0);
    }
    if (ks >= 0x20 && ks <= 0xff) {  // Latin-1: Qt keys are the upper case code
        const QChar c = QChar(char16_t(ks)).toUpper();
        return c.unicode() <= 0xff ? c.unicode() : int(ks);
    }
    if (ks >= 0x01000100 && ks <= 0x0110ffff)  // Unicode keysyms
        return int(QChar::toUpper(char32_t(ks - 0x01000000)));
    return 0;
}

QWindow *keyTarget() {
    if (QWindow *f = QGuiApplication::focusWindow())
        return f;
    for (auto it = S.stack.crbegin(); it != S.stack.crend(); ++it)
        if ((*it)->window()->type() != Qt::ToolTip)
            return (*it)->window();
    return nullptr;
}

QWindow *pointerTarget(const QPointF &global) {
    if (S.grab && S.buttons)
        return S.grab;
    // Child windows (QWindow::setParent) are not separate surfaces: events go
    // to the top-level, as with a single native window.
    for (auto it = S.stack.crbegin(); it != S.stack.crend(); ++it)
        if (QRectF((*it)->geometry()).contains(global))
            return (*it)->window();
    return nullptr;
}

QPointF localOf(QWindow *w, const QPointF &global) {
    return global - QPointF(static_cast<GuiWindow *>(w->handle())->geometry().topLeft());
}

// --- paint ----------------------------------------------------------------------

void ensureFrame() {
    if (S.frame.size() == S.device)
        return;
    S.frame = QImage(S.device, QImage::Format_ARGB32_Premultiplied);
    S.frame.fill(Qt::black);
    S.damage = QRect(QPoint(), S.device);
}

void compose(const QRegion &region) {
    QPainter p(&S.frame);
    p.setClipRegion(region);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    // Behind the windows: the theme's window color.
    p.fillRect(S.frame.rect(), S.dark ? QColor(0x20, 0x20, 0x20) : QColor(0xef, 0xef, 0xef));
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    for (GuiWindow *g : std::as_const(S.stack)) {
        if (!g->store || g->store->image.isNull())
            continue;
        const QRect at = toDevice(g->geometry());
        QImage img = g->store->image;
        img.setDevicePixelRatio(1);
        p.drawImage(at.topLeft(), img, QRect(QPoint(), at.size()));
    }
}

}  // namespace

// --- qt2wasm/gui.h ------------------------------------------------------------

namespace qt2wasm::gui {
QString clipboardText() { return hostText(latha_gui_host_clipboard_text); }
void setClipboardText(const QString &text) { hostSetText(latha_gui_host_set_clipboard_text, text); }
QString primaryText() { return hostText(latha_gui_host_primary_text); }
void setPrimaryText(const QString &text) { hostSetText(latha_gui_host_set_primary_text, text); }
}  // namespace qt2wasm::gui

QPlatformIntegration *Qt2WasmGuiIntegrationPlugin::create(const QString &key, const QStringList &) {
    if (key.compare(QLatin1String("qt2wasmgui"), Qt::CaseInsensitive) == 0)
        return new GuiIntegration;
    return nullptr;
}

// --- exports: latha:gui/app -----------------------------------------------------

extern "C" {

// Every host function stays imported, used or not: hosts check a component's
// imports against the latha:gui world exactly, and --gc-sections would drop
// the ones this app never calls.
static void (*volatile g_keepImports[])() = {
    reinterpret_cast<void (*)()>(&latha_gui_host_request_frame),
    reinterpret_cast<void (*)()>(&latha_gui_host_request_tick),
    reinterpret_cast<void (*)()>(&latha_gui_host_set_title),
    reinterpret_cast<void (*)()>(&latha_gui_host_set_cursor),
    reinterpret_cast<void (*)()>(&latha_gui_host_set_min_size),
    reinterpret_cast<void (*)()>(&latha_gui_host_clipboard_text),
    reinterpret_cast<void (*)()>(&latha_gui_host_set_clipboard_text),
    reinterpret_cast<void (*)()>(&latha_gui_host_primary_text),
    reinterpret_cast<void (*)()>(&latha_gui_host_set_primary_text),
    reinterpret_cast<void (*)()>(&latha_gui_host_prefers_dark),
    reinterpret_cast<void (*)()>(&latha_gui_host_bell),
    reinterpret_cast<void (*)()>(&latha_gui_host_quit),
#ifdef QT2WASM_GUI_PTY
    reinterpret_cast<void (*)()>(&latha_gui_pty_spawn),
    reinterpret_cast<void (*)()>(&latha_gui_pty_write),
    reinterpret_cast<void (*)()>(&latha_gui_pty_read),
    reinterpret_cast<void (*)()>(&latha_gui_pty_resize),
    reinterpret_cast<void (*)()>(&latha_gui_pty_exit_code),
    reinterpret_cast<void (*)()>(&latha_gui_pty_close),
#endif
};

bool exports_latha_gui_app_start(gui_list_string_t *args, exports_latha_gui_app_size_t *ret, gui_string_t *err) {
    (void)g_keepImports[0];
    QList<QByteArray> argv8;
    for (size_t i = 0; i < args->len; ++i)
        argv8.append(fromWit(&args->ptr[i], false).toUtf8());
    gui_list_string_free(args);
    if (argv8.isEmpty())
        argv8.append("app");
    S.dark = latha_gui_host_prefers_dark();
    GuiCall call;
    static QuitFilter quitFilter;
    qApp->installEventFilter(&quitFilter);
    QGuiApplication::setApplicationName(QString::fromUtf8(argv8.first()));
    QGuiApplication::setQuitOnLastWindowClosed(true);

    std::vector<char *> argv;
    for (QByteArray &a : argv8)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    const int rc = qt2wasm_gui_main(int(argv8.size()), argv.data());
    // The host's loop is the app's exec(): without this, QCoreApplication::quit()
    // and quit-on-last-window-closed do nothing (they only act inside exec()).
    static_cast<QCoreApplicationPrivate *>(QObjectPrivate::get(qApp))->in_exec = true;
    if (rc != 0) {
        toWit(err, QStringLiteral("qt2wasm_gui_main returned %1").arg(rc));
        return false;
    }
    // The size the first window asked for, in device pixels at scale 1.
    *ret = {0, 0};
    if (S.wish.isValid())
        *ret = {uint32_t(qMax(0, S.wish.width())), uint32_t(qMax(0, S.wish.height()))};
    return true;
}

void exports_latha_gui_app_resize(uint32_t width, uint32_t height, double scale) {
    GuiCall call;
    const QSize device(qMax<uint32_t>(1, width), qMax<uint32_t>(1, height));
    const qreal sc = scale > 0 ? scale : 1.0;
    if (S.resized && device == S.device && sc == S.scale)
        return;
    const bool scaleChanged = sc != S.scale;
    S.device = device;
    S.scale = sc;
    S.resized = true;
    ensureFrame();
    damageDevice(QRect(QPoint(), S.device));
    if (S.screen && S.screen->screen()) {
        const QRect g = S.screen->geometry();
        QWindowSystemInterface::handleScreenGeometryChange(S.screen->screen(), g, g);
    }
    for (GuiWindow *g : QList<GuiWindow *>(S.stack)) {
        if (scaleChanged)
            QWindowSystemInterface::handleWindowDevicePixelRatioChanged(g->window());
        if (g->fills())
            g->setGeometry(QRect(QPoint(), logicalSize()));
        else if (scaleChanged)
            g->expose();
    }
}

void exports_latha_gui_app_key(exports_latha_gui_app_key_event_t *e) {
    GuiCall call;
    const QString text = fromWit(&e->text);
    QWindow *w = keyTarget();
    if (!w)
        return;
    bool keypad = false;
    int key = qtKey(e->keysym, &keypad);
    Qt::KeyboardModifiers mods = qtMods(e->modifiers);
    if (keypad)
        mods |= Qt::KeypadModifier;
    if (!key && text.isEmpty())
        return;
    QWindowSystemInterface::handleExtendedKeyEvent(w, now(), e->pressed ? QEvent::KeyPress : QEvent::KeyRelease,
                                                   key, mods, e->scancode, e->keysym, 0, text, e->repeat);
}

void exports_latha_gui_app_text(gui_string_t *t) {
    GuiCall call;
    const QString text = fromWit(t);
    if (text.isEmpty())
        return;
#if QT_CONFIG(im)
    QObject *fo = QGuiApplication::focusObject();
    if (fo) {
        QInputMethodQueryEvent q(Qt::ImEnabled);
        QCoreApplication::sendEvent(fo, &q);
        if (q.value(Qt::ImEnabled).toBool()) {
            QInputMethodEvent ev;
            ev.setCommitString(text);
            QCoreApplication::sendEvent(fo, &ev);
            return;
        }
    }
#endif
    if (QWindow *w = keyTarget()) {
        QWindowSystemInterface::handleKeyEvent(w, now(), QEvent::KeyPress, 0, Qt::NoModifier, text);
        QWindowSystemInterface::handleKeyEvent(w, now(), QEvent::KeyRelease, 0, Qt::NoModifier, text);
    }
}

void exports_latha_gui_app_pointer(exports_latha_gui_app_pointer_event_t *e) {
    GuiCall call;
    const QPointF global(e->x / S.scale, e->y / S.scale);
    static_cast<GuiCursor *>(S.screen->cursor())->m_pos = global.toPoint();
    const Qt::MouseButtons buttons = qtButtons(e->buttons);
    const Qt::KeyboardModifiers mods = qtMods(e->modifiers);
    if (e->kind == LATHA_GUI_TYPES_POINTER_KIND_LEAVE) {
        if (S.under)
            QWindowSystemInterface::handleLeaveEvent(S.under);
        S.under = nullptr;
        return;
    }
    QWindow *w = pointerTarget(global);
    if (e->kind == LATHA_GUI_TYPES_POINTER_KIND_PRESS && !S.buttons)
        S.grab = w;
    S.buttons = buttons;
    if (w != S.under && !(S.grab && buttons)) {
        if (S.under)
            QWindowSystemInterface::handleLeaveEvent(S.under);
        S.under = w;
        if (w)
            QWindowSystemInterface::handleEnterEvent(w, localOf(w, global), global);
    }
    if (e->kind == LATHA_GUI_TYPES_POINTER_KIND_ENTER)
        return;
    if (!w) {
        // A press outside every popup closes it, as a window manager would.
        if (e->kind == LATHA_GUI_TYPES_POINTER_KIND_PRESS)
            for (GuiWindow *g : QList<GuiWindow *>(S.stack))
                if (g->window()->type() == Qt::Popup)
                    g->window()->close();
        return;
    }
    QEvent::Type type = QEvent::MouseMove;
    if (e->kind == LATHA_GUI_TYPES_POINTER_KIND_PRESS)
        type = QEvent::MouseButtonPress;
    else if (e->kind == LATHA_GUI_TYPES_POINTER_KIND_RELEASE)
        type = QEvent::MouseButtonRelease;
    QWindowSystemInterface::handleMouseEvent(w, now(), localOf(w, global), global, buttons, qtButton(e->button), type, mods);
    if (!buttons)
        S.grab = nullptr;
}

void exports_latha_gui_app_wheel(exports_latha_gui_app_wheel_event_t *e) {
    GuiCall call;
    const QPointF global(e->x / S.scale, e->y / S.scale);
    QWindow *w = pointerTarget(global);
    if (!w)
        return;
    // Qt: positive angle = away from the user (up / left); ours: positive = down / right.
    const QPoint angle(int(std::lround(-e->steps_x * 120)), int(std::lround(-e->steps_y * 120)));
    const QPoint pixels(int(std::lround(-e->pixels_x / S.scale)), int(std::lround(-e->pixels_y / S.scale)));
    QWindowSystemInterface::handleWheelEvent(w, now(), localOf(w, global), global, pixels, angle, qtMods(e->modifiers));
}

void exports_latha_gui_app_focus(bool focused) {
    GuiCall call;
    S.focused = focused;
    QWindowSystemInterface::handleFocusWindowChanged(focused ? keyTarget() : nullptr, Qt::ActiveWindowFocusReason);
}

void exports_latha_gui_app_color_scheme(bool dark) {
    GuiCall call;
    if (dark == S.dark)
        return;
    S.dark = dark;
    QWindowSystemInterface::handleThemeChange();
    damageDevice(QRect(QPoint(), S.device));
}

bool exports_latha_gui_app_close(void) {
    GuiCall call;
    // Close the main window like a window manager would; the app may refuse
    // (a QCloseEvent it ignores). Closing the last window quits.
    for (GuiWindow *g : QList<GuiWindow *>(S.stack))
        if (!floats(g->window())) {
            if (!QWindowSystemInterface::handleCloseEvent(g->window()))
                return false;
            return true;
        }
    return true;
}

bool exports_latha_gui_app_tick(uint32_t *ret) {
    GuiCall call;
    // GuiCall's destructor runs the due work; report what is left after it.
    QWindowSystemInterface::sendWindowSystemEvents(QEventLoop::AllEvents);
    qt2wasm::pump();
    auto *d = qobject_cast<qt2wasm::WasiEventDispatcher *>(QCoreApplication::eventDispatcher());
    const qint64 next = d ? d->msUntilNextTimer() : -1;
    if (next < 0)
        return false;
    *ret = uint32_t(qMin<qint64>(next, 0x7fffffff));
    return true;
}

void exports_latha_gui_app_paint(exports_latha_gui_app_list_rect_t *ret) {
    GuiCall call;
    ret->ptr = nullptr;
    ret->len = 0;
    S.frameRequested = false;
    ensureFrame();
    for (GuiWindow *g : QList<GuiWindow *>(S.stack))
        if (g->updatePending) {
            g->updatePending = false;
            g->deliverUpdateRequest();
        }
    QWindowSystemInterface::sendWindowSystemEvents(QEventLoop::AllEvents);
    qt2wasm::pump();
    const QRegion damage = S.damage & QRect(QPoint(), S.device);
    S.damage = QRegion();
    if (damage.isEmpty())
        return;
    compose(damage);
    // Many small rects cost a call each on the host: past 16, send the bounds.
    QList<QRect> rects(damage.begin(), damage.end());
    if (rects.size() > 16)
        rects = {damage.boundingRect()};
    ret->len = size_t(rects.size());
    ret->ptr = static_cast<exports_latha_gui_app_rect_t *>(std::malloc(sizeof(exports_latha_gui_app_rect_t) * rects.size()));
    for (qsizetype i = 0; i < rects.size(); ++i) {
        const QRect &r = rects[i];
        ret->ptr[i] = {r.x(), r.y(), uint32_t(r.width()), uint32_t(r.height())};
    }
}

void exports_latha_gui_app_frame_size(exports_latha_gui_app_size_t *ret) {
    *ret = {uint32_t(S.device.width()), uint32_t(S.device.height())};
}

void exports_latha_gui_app_frame_pixels(exports_latha_gui_app_rect_t *r, gui_string_t *ret) {
    const QRect want = QRect(r->x, r->y, int(r->width), int(r->height)) & S.frame.rect();
    ret->ptr = nullptr;
    ret->len = 0;
    if (want.isEmpty())
        return;
    // Rows of the frame are contiguous only at full width: copy row by row.
    const int rowBytes = want.width() * 4;
    QByteArray raw(qsizetype(rowBytes) * want.height(), Qt::Uninitialized);
    for (int y = 0; y < want.height(); ++y)
        std::memcpy(raw.data() + qsizetype(y) * rowBytes,
                    S.frame.constScanLine(want.y() + y) + want.x() * 4, size_t(rowBytes));
    toWit(ret, raw.toBase64());
}

}  // extern "C"
