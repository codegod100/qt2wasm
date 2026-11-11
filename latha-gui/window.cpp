// SPDX-FileCopyrightText: 2026 Kwi WebAssembly port contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only
#include "accent.h"
#include "window.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDebug>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QStyleHints>
#include <QWheelEvent>
#include <cmath>
#include <cstring>

namespace {

// Wasm calls are limited in what one call may copy out; ~2.7 MB of base64.
constexpr qint64 MaxCallPixels = 512 * 1024;
// Paint at most this often (ms) while the app keeps asking (fast output).
constexpr int MinFrameMs = 6;

quint32 modifiersOf(Qt::KeyboardModifiers m)
{
    quint32 b = 0;
    if (m & Qt::ShiftModifier)
        b |= LATHA_SHIFT;
    if (m & Qt::ControlModifier)
        b |= LATHA_CTRL;
    if (m & Qt::AltModifier)
        b |= LATHA_ALT;
    if (m & Qt::MetaModifier)
        b |= LATHA_META;
    if (m & Qt::KeypadModifier)
        b |= LATHA_KEYPAD;
    return b;
}

quint32 buttonBit(Qt::MouseButton b)
{
    switch (b) {
    case Qt::LeftButton: return LATHA_BTN_LEFT;
    case Qt::RightButton: return LATHA_BTN_RIGHT;
    case Qt::MiddleButton: return LATHA_BTN_MIDDLE;
    case Qt::BackButton: return LATHA_BTN_BACK;
    case Qt::ForwardButton: return LATHA_BTN_FORWARD;
    default: return 0;
    }
}

quint32 buttonsOf(Qt::MouseButtons bs)
{
    quint32 b = 0;
    for (auto one : {Qt::LeftButton, Qt::RightButton, Qt::MiddleButton, Qt::BackButton, Qt::ForwardButton})
        if (bs & one)
            b |= buttonBit(one);
    return b;
}

quint32 buttonOf(Qt::MouseButton b)
{
    switch (b) {
    case Qt::LeftButton: return LATHA_BUTTON_LEFT;
    case Qt::RightButton: return LATHA_BUTTON_RIGHT;
    case Qt::MiddleButton: return LATHA_BUTTON_MIDDLE;
    case Qt::BackButton: return LATHA_BUTTON_BACK;
    case Qt::ForwardButton: return LATHA_BUTTON_FORWARD;
    default: return LATHA_BUTTON_NONE;
    }
}

// X11 keysyms for platforms that report none (offscreen, some tests); xcb
// and wayland give the real keysym as nativeVirtualKey.
quint32 keysymOf(const QKeyEvent *e)
{
    if (quint32 k = e->nativeVirtualKey())
        return k;
    const int key = e->key();
    switch (key) {
    case Qt::Key_Escape: return 0xff1b;
    case Qt::Key_Tab: return 0xff09;
    case Qt::Key_Backtab: return 0xfe20;
    case Qt::Key_Backspace: return 0xff08;
    case Qt::Key_Return: return 0xff0d;
    case Qt::Key_Enter: return 0xff8d;
    case Qt::Key_Insert: return 0xff63;
    case Qt::Key_Delete: return 0xffff;
    case Qt::Key_Pause: return 0xff13;
    case Qt::Key_Print: return 0xff61;
    case Qt::Key_Home: return 0xff50;
    case Qt::Key_End: return 0xff57;
    case Qt::Key_Left: return 0xff51;
    case Qt::Key_Up: return 0xff52;
    case Qt::Key_Right: return 0xff53;
    case Qt::Key_Down: return 0xff54;
    case Qt::Key_PageUp: return 0xff55;
    case Qt::Key_PageDown: return 0xff56;
    case Qt::Key_Shift: return 0xffe1;
    case Qt::Key_Control: return 0xffe3;
    case Qt::Key_Meta: return 0xffeb;
    case Qt::Key_Alt: return 0xffe9;
    case Qt::Key_CapsLock: return 0xffe5;
    case Qt::Key_NumLock: return 0xff7f;
    case Qt::Key_ScrollLock: return 0xff14;
    case Qt::Key_Menu: return 0xff67;
    default: break;
    }
    if (key >= Qt::Key_F1 && key <= Qt::Key_F35)
        return 0xffbe + (key - Qt::Key_F1);
    const QString t = e->text();
    if (t.size() == 1 && t[0].unicode() >= 0x20 && t[0].unicode() != 0x7f) {
        const char16_t c = t[0].unicode();
        return c < 0x100 ? c : 0x01000000 | c;
    }
    if (key >= Qt::Key_Space && key <= Qt::Key_AsciiTilde) {
        int c = key;
        if (c >= 'A' && c <= 'Z' && !(e->modifiers() & Qt::ShiftModifier))
            c += 'a' - 'A';
        return c;
    }
    return 0;
}

Qt::CursorShape cursorOf(quint32 shape)
{
    // types.cursor in WIT order
    static const Qt::CursorShape shapes[] = {
        Qt::ArrowCursor,        // default
        Qt::IBeamCursor,        // text
        Qt::PointingHandCursor, // pointer
        Qt::CrossCursor,        // crosshair
        Qt::WaitCursor,         // wait
        Qt::BusyCursor,         // progress
        Qt::WhatsThisCursor,    // help
        Qt::SizeAllCursor,      // move
        Qt::ForbiddenCursor,    // not-allowed
        Qt::OpenHandCursor,     // grab
        Qt::ClosedHandCursor,   // grabbing
        Qt::SizeHorCursor,      // ew-resize
        Qt::SizeVerCursor,      // ns-resize
        Qt::SizeBDiagCursor,    // nesw-resize
        Qt::SizeFDiagCursor,    // nwse-resize
        Qt::SplitHCursor,       // col-resize
        Qt::SplitVCursor,       // row-resize
        Qt::BlankCursor,        // none
    };
    return shape < sizeof shapes / sizeof shapes[0] ? shapes[shape] : Qt::ArrowCursor;
}

LathaStr strOf(const QByteArray &b)
{
    return LathaStr{b.constData(), size_t(b.size())};
}

} // namespace

AppWindow::AppWindow(LathaApp *app, const QString &appName, const RunOptions &opts)
    : m_app(app)
    , m_appName(appName)
    , m_opts(opts)
{
    m_clock.start();
    setWindowTitle(appName);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_InputMethodEnabled);
    setAttribute(Qt::WA_KeyCompression, false);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAutoFillBackground(false);

    const QByteArray theme = qgetenv("LATHA_GUI_THEME");
    if (theme == "dark" || theme == "light") {
        m_darkOverride = true;
        m_dark = theme == "dark";
    }
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (!m_darkOverride && m_app && check(latha_color_scheme(m_app, prefersDark())))
            schedulePump();
    });
    // color-scheme also tells the app to read host.accent-color again.
    connect(Accent::instance(), &Accent::changed, this, [this] {
        if (m_app && check(latha_color_scheme(m_app, prefersDark())))
            schedulePump();
    });

    m_pumpTimer.setSingleShot(true);
    m_pumpTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_pumpTimer, &QTimer::timeout, this, &AppWindow::pump);

    if (m_opts.quitAfterMs >= 0) {
        QTimer::singleShot(m_opts.quitAfterMs, this, [this] {
            if (!m_opts.screenshot.isEmpty()) {
                const bool ok = m_frame.save(m_opts.screenshot);
                fprintf(stderr, "latha-gui: %s %s (%dx%d)\n", ok ? "saved" : "could not save",
                        qPrintable(m_opts.screenshot), m_frame.width(), m_frame.height());
            }
            finish(0);
        });
    }
    if (!m_opts.typeText.isEmpty()) {
        QTimer::singleShot(m_opts.typeAfterMs, this, [this] {
            for (QChar c : m_opts.typeText) {
                const bool nl = c == u'\n';
                const QString t = nl ? QStringLiteral("\r") : QString(c);
                const quint32 sym = nl ? 0xff0d : c.unicode();
                const QByteArray b = t.toUtf8();
                if (!m_app || !check(latha_key(m_app, 1, 0, sym, 0, 0, strOf(b))))
                    return;
                check(latha_key(m_app, 0, 0, sym, 0, 0, LathaStr{"", 0}));
            }
            m_tickNow = true;
            schedulePump();
        });
    }
    schedulePump();
}

AppWindow::~AppWindow()
{
    m_ptys.clear();
}

bool AppWindow::prefersDark() const
{
    if (m_darkOverride)
        return m_dark;
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

double AppWindow::scale() const
{
    return devicePixelRatioF();
}

QRect AppWindow::toLogical(const QRect &d) const
{
    const double s = scale();
    const int x0 = int(std::floor(d.left() / s)), y0 = int(std::floor(d.top() / s));
    const int x1 = int(std::ceil((d.left() + d.width()) / s)), y1 = int(std::ceil((d.top() + d.height()) / s));
    return QRect(x0, y0, x1 - x0, y1 - y0);
}

bool AppWindow::check(int r)
{
    if (r >= 0)
        return true;
    if (m_app && !m_finished) {
        fprintf(stderr, "latha-gui: %s trapped: %s\n", qPrintable(m_appName), latha_error(m_app));
        finish(3);
    }
    return false;
}

void AppWindow::finish(int code)
{
    if (m_finished)
        return;
    m_finished = true;
    m_pumpTimer.stop();
    m_ptys.clear();
    // The component goes before the window: its imports call into us.
    if (m_app) {
        latha_free(m_app);
        m_app = nullptr;
    }
    QCoreApplication::exit(code);
}

// --- imports -------------------------------------------------------------------------

void AppWindow::requestFrame()
{
    m_frameWanted = true;
    schedulePump();
}

void AppWindow::requestTick(quint32 ms)
{
    m_tickAt = m_clock.elapsed() + ms;
    schedulePump(ms);
}

void AppWindow::setAppTitle(const QString &t)
{
    setWindowTitle(t.isEmpty() ? m_appName : t);
}

void AppWindow::setAppCursor(quint32 shape)
{
    setCursor(cursorOf(shape));
}

void AppWindow::setAppMinSize(quint32 w, quint32 h)
{
    const double s = scale();
    setMinimumSize(int(std::ceil(w / s)), int(std::ceil(h / s)));
}

void AppWindow::quitApp(int code)
{
    m_quitCode = code;
    schedulePump();
}

int64_t AppWindow::ptySpawn(const QList<QByteArray> &argv, const QList<QPair<QByteArray, QByteArray>> &env,
                            const QByteArray &cwd, quint16 cols, quint16 rows, QString *err)
{
    auto p = Pty::spawn(argv, env, cwd, cols, rows, err);
    if (!p)
        return -1;
    connect(p.get(), &Pty::activity, this, [this] {
        m_tickNow = true;
        schedulePump();
    });
    const quint32 id = m_nextPty++;
    m_ptys[id] = std::move(p);
    return id;
}

Pty *AppWindow::pty(quint32 id)
{
    auto it = m_ptys.find(id);
    return it == m_ptys.end() ? nullptr : it->second.get();
}

void AppWindow::ptyClose(quint32 id)
{
    auto it = m_ptys.find(id);
    if (it == m_ptys.end())
        return;
    // Deleting a QObject inside its own signal is not our case (we are in a
    // wasm call), but defer anyway: the notifier may be mid-dispatch.
    it->second.release()->deleteLater();
    m_ptys.erase(it);
}

// --- the loop ------------------------------------------------------------------------

void AppWindow::schedulePump(int ms)
{
    if (m_finished)
        return;
    if (m_pumpTimer.isActive() && m_pumpTimer.remainingTime() <= ms)
        return;
    m_pumpTimer.start(ms);
}

void AppWindow::applySize()
{
    if (!m_app)
        return;
    const double s = scale();
    const quint32 w = quint32(std::lround(width() * s)), h = quint32(std::lround(height() * s));
    if (!w || !h || (w == m_appliedW && h == m_appliedH && s == m_appliedScale))
        return;
    m_appliedW = w;
    m_appliedH = h;
    m_appliedScale = s;
    if (check(latha_resize(m_app, w, h, s))) {
        m_frameWanted = true;
        m_tickNow = true;
        schedulePump();
    }
}

void AppWindow::pump()
{
    if (!m_app || m_finished)
        return;
    const qint64 now = m_clock.elapsed();
    if (m_quitCode < 0 && (m_tickNow || (m_tickAt >= 0 && now >= m_tickAt))) {
        m_tickNow = false;
        const int64_t next = latha_tick(m_app);
        if (next == -2) {
            check(-1);
            return;
        }
        m_tickAt = next >= 0 ? m_clock.elapsed() + next : -1;
    }
    if (m_quitCode >= 0) {
        finish(m_quitCode);
        return;
    }
    if (m_frameWanted && m_appliedW && isVisible()) {
        const qint64 since = m_clock.elapsed() - m_lastPaint;
        if (since < MinFrameMs) {
            schedulePump(int(MinFrameMs - since));
        } else {
            m_frameWanted = false;
            m_lastPaint = m_clock.elapsed();
            paintApp();
        }
    }
    if (m_quitCode >= 0) {
        finish(m_quitCode);
        return;
    }
    if (m_tickNow)
        schedulePump();
    else if (m_tickAt >= 0)
        schedulePump(int(qMax<qint64>(0, m_tickAt - m_clock.elapsed())));
}

void AppWindow::applyMove(const LathaMove &m)
{
    // Shift the pixels inside `area` by (dx, dy), clipped to it.
    const QRect area = QRect(m.area.x, m.area.y, int(m.area.width), int(m.area.height)) & m_frame.rect();
    if (area.isEmpty() || (!m.dx && !m.dy))
        return;
    const QRect dst = area.translated(m.dx, m.dy) & area;
    if (dst.isEmpty())
        return;
    const QRect src = dst.translated(-m.dx, -m.dy);
    const qsizetype bpl = m_frame.bytesPerLine();
    uchar *bits = m_frame.bits();
    const size_t rowBytes = size_t(dst.width()) * 4;
    auto copyRow = [&](int i) {
        memmove(bits + (dst.top() + i) * bpl + dst.left() * 4, bits + (src.top() + i) * bpl + src.left() * 4, rowBytes);
    };
    if (m.dy > 0) {
        for (int i = dst.height() - 1; i >= 0; --i)
            copyRow(i);
    } else {
        for (int i = 0; i < dst.height(); ++i)
            copyRow(i);
    }
    update(toLogical(area));
}

void AppWindow::paintApp()
{
    QElapsedTimer t;
    if (m_opts.debug)
        t.start();
    if (!check(latha_paint(m_app)))
        return;
    const LathaMove *moves;
    const LathaRect *rects;
    size_t nmoves, nrects;
    latha_damage(m_app, &moves, &nmoves, &rects, &nrects);
    if (!nmoves && !nrects)
        return;
    // Our copy of the damage: the next app call may change it.
    const std::vector<LathaMove> mv(moves, moves + nmoves);
    const std::vector<LathaRect> rc(rects, rects + nrects);
    uint32_t fw = 0, fh = 0;
    if (!check(latha_frame_size(m_app, &fw, &fh)))
        return;
    if (m_frame.width() != int(fw) || m_frame.height() != int(fh)) {
        QImage f(int(fw), int(fh), QImage::Format_ARGB32_Premultiplied);
        f.fill(Qt::black);
        if (!m_frame.isNull()) {
            QPainter p(&f);
            p.drawImage(0, 0, m_frame);
        }
        m_frame = f;
    }
    m_frame.setDevicePixelRatio(1);
    for (const auto &m : mv)
        applyMove(m);
    qint64 bytes = 0;
    for (const auto &r0 : rc) {
        const QRect r = QRect(r0.x, r0.y, int(r0.width), int(r0.height)) & m_frame.rect();
        if (r.isEmpty())
            continue;
        const int step = int(qMax<qint64>(1, MaxCallPixels / r.width()));
        for (int y = r.top(); y <= r.bottom(); y += step) {
            const int h = qMin(step, r.bottom() + 1 - y);
            const LathaRect part{r.x(), y, uint32_t(r.width()), uint32_t(h)};
            const int res = latha_frame_pixels(m_app, part, m_frame.scanLine(y) + r.x() * 4, size_t(m_frame.bytesPerLine()));
            if (res == -2)
                fprintf(stderr, "latha-gui: frame-pixels %d,%d %dx%d: bad base64\n", r.x(), y, r.width(), h);
            else if (!check(res))
                return;
            bytes += qint64(r.width()) * h * 4;
        }
        update(toLogical(r));
    }
    if (m_opts.verify && nmoves)
        verifyFrame();
    if (m_opts.debug)
        fprintf(stderr, "latha-gui: paint %zu move(s), %zu rect(s), %lld KB in %.1f ms\n", nmoves, nrects,
                (long long)(bytes >> 10), t.nsecsElapsed() / 1e6);
}

void AppWindow::verifyFrame()
{
    QImage full(m_frame.size(), QImage::Format_ARGB32_Premultiplied);
    const int w = full.width();
    const int step = int(qMax<qint64>(1, MaxCallPixels / w));
    for (int y = 0; y < full.height(); y += step) {
        const int h = qMin(step, full.height() - y);
        if (!check(latha_frame_pixels(m_app, LathaRect{0, y, uint32_t(w), uint32_t(h)}, full.scanLine(y),
                                      size_t(full.bytesPerLine()))))
            return;
    }
    for (int y = 0; y < full.height(); ++y) {
        if (memcmp(full.constScanLine(y), m_frame.constScanLine(y), size_t(w) * 4) != 0) {
            fprintf(stderr, "latha-gui: VERIFY: moved frame differs from the app's at row %d\n", y);
            return;
        }
    }
    fprintf(stderr, "latha-gui: VERIFY: moved frame matches the app's\n");
}

// --- Qt events -----------------------------------------------------------------------

bool AppWindow::event(QEvent *e)
{
    switch (e->type()) {
    case QEvent::ShortcutOverride:
        // Every key is the app's: no Qt shortcut may eat it.
        e->accept();
        return true;
    case QEvent::KeyPress:
        // Tab / Backtab would move focus otherwise.
        keyPressEvent(static_cast<QKeyEvent *>(e));
        return true;
    case QEvent::DevicePixelRatioChange:
    case QEvent::ScreenChangeInternal:
        applySize();
        break;
    default:
        break;
    }
    return QWidget::event(e);
}

void AppWindow::paintEvent(QPaintEvent *e)
{
    QPainter p(this);
    if (m_frame.isNull()) {
        p.fillRect(e->rect(), Qt::black);
        return;
    }
    const double s = scale();
    m_frame.setDevicePixelRatio(s);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    for (const QRect &r : e->region()) {
        // Logical rect -> the frame's device pixels (whole pixels, outward).
        const QRect src(int(std::floor(r.x() * s)), int(std::floor(r.y() * s)), int(std::ceil(r.width() * s)) + 1,
                        int(std::ceil(r.height() * s)) + 1);
        const QRect srcClip = src & m_frame.rect();
        if (srcClip.isEmpty())
            continue;
        p.drawImage(QRectF(srcClip.x() / s, srcClip.y() / s, srcClip.width() / s, srcClip.height() / s), m_frame,
                    srcClip);
    }
    // Uncovered (window larger than the frame, e.g. mid-resize).
    const QRect frameLogical(0, 0, int(m_frame.width() / s), int(m_frame.height() / s));
    const QRegion rest = e->region().subtracted(frameLogical);
    for (const QRect &r : rest)
        p.fillRect(r, Qt::black);
    m_frame.setDevicePixelRatio(1);
}

void AppWindow::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    applySize();
}

void AppWindow::key(QKeyEvent *e, bool pressed)
{
    if (!m_app)
        return;
    const QByteArray text = pressed ? e->text().toUtf8() : QByteArray();
    if (check(latha_key(m_app, pressed, e->isAutoRepeat(), keysymOf(e), e->nativeScanCode(),
                        modifiersOf(e->modifiers()), strOf(text)))) {
        m_tickNow = true;
        schedulePump();
    }
}

void AppWindow::keyPressEvent(QKeyEvent *e)
{
    key(e, true);
    e->accept();
}

void AppWindow::keyReleaseEvent(QKeyEvent *e)
{
    key(e, false);
    e->accept();
}

void AppWindow::inputMethodEvent(QInputMethodEvent *e)
{
    if (m_app && !e->commitString().isEmpty()) {
        const QByteArray t = e->commitString().toUtf8();
        if (check(latha_text(m_app, strOf(t)))) {
            m_tickNow = true;
            schedulePump();
        }
    }
    e->accept();
}

QVariant AppWindow::inputMethodQuery(Qt::InputMethodQuery q) const
{
    switch (q) {
    case Qt::ImEnabled:
        return true;
    case Qt::ImCursorRectangle:
        return QRect(m_lastPressPos.toPoint(), QSize(1, 16));
    default:
        return QWidget::inputMethodQuery(q);
    }
}

void AppWindow::pointer(int kind, QMouseEvent *e, int clicks)
{
    if (!m_app)
        return;
    const double s = scale();
    LathaPointer ev{};
    ev.kind = uint32_t(kind);
    ev.button = (kind == LATHA_PRESS || kind == LATHA_RELEASE) ? buttonOf(e->button()) : LATHA_BUTTON_NONE;
    ev.buttons = buttonsOf(e->buttons());
    ev.modifiers = modifiersOf(e->modifiers());
    ev.x = e->position().x() * s;
    ev.y = e->position().y() * s;
    ev.clicks = uint32_t(clicks);
    if (check(latha_pointer(m_app, &ev))) {
        m_tickNow = true;
        schedulePump();
    }
}

void AppWindow::mousePressEvent(QMouseEvent *e)
{
    const qint64 now = m_clock.elapsed();
    const int dist = QApplication::startDragDistance();
    if (e->button() == m_lastButton && now - m_lastPress <= QGuiApplication::styleHints()->mouseDoubleClickInterval()
        && (e->position() - m_lastPressPos).manhattanLength() <= dist)
        ++m_clicks;
    else
        m_clicks = 1;
    m_lastPress = now;
    m_lastPressPos = e->position();
    m_lastButton = e->button();
    pointer(LATHA_PRESS, e, m_clicks);
}

void AppWindow::mouseDoubleClickEvent(QMouseEvent *e)
{
    // Qt sends this instead of the second press.
    mousePressEvent(e);
}

void AppWindow::mouseReleaseEvent(QMouseEvent *e)
{
    pointer(LATHA_RELEASE, e, 0);
}

void AppWindow::mouseMoveEvent(QMouseEvent *e)
{
    pointer(LATHA_MOVE, e, 0);
}

void AppWindow::enterEvent(QEnterEvent *e)
{
    if (!m_app)
        return;
    const double s = scale();
    LathaPointer ev{};
    ev.kind = LATHA_ENTER;
    ev.buttons = buttonsOf(QGuiApplication::mouseButtons());
    ev.modifiers = modifiersOf(QGuiApplication::keyboardModifiers());
    ev.x = e->position().x() * s;
    ev.y = e->position().y() * s;
    if (check(latha_pointer(m_app, &ev)))
        schedulePump();
}

void AppWindow::leaveEvent(QEvent *)
{
    if (!m_app)
        return;
    LathaPointer ev{};
    ev.kind = LATHA_LEAVE;
    ev.buttons = buttonsOf(QGuiApplication::mouseButtons());
    ev.modifiers = modifiersOf(QGuiApplication::keyboardModifiers());
    if (check(latha_pointer(m_app, &ev)))
        schedulePump();
}

void AppWindow::wheelEvent(QWheelEvent *e)
{
    if (!m_app)
        return;
    const double s = scale();
    LathaWheel w{};
    w.x = e->position().x() * s;
    w.y = e->position().y() * s;
    // latha: positive = down / right; Qt: positive = away from the user.
    const QPoint px = e->pixelDelta(), ang = e->angleDelta();
    if (!px.isNull()) {
        w.pixels_x = -px.x() * s;
        w.pixels_y = -px.y() * s;
        // Also notches, 1 per ~50 logical pixels, for apps that only read steps.
        w.steps_x = -px.x() / 50.0;
        w.steps_y = -px.y() / 50.0;
    } else {
        w.steps_x = -ang.x() / 120.0;
        w.steps_y = -ang.y() / 120.0;
    }
    w.modifiers = modifiersOf(e->modifiers());
    if (check(latha_wheel(m_app, &w))) {
        m_tickNow = true;
        schedulePump();
    }
    e->accept();
}

void AppWindow::focusInEvent(QFocusEvent *)
{
    if (m_app && check(latha_focus(m_app, 1)))
        schedulePump();
}

void AppWindow::focusOutEvent(QFocusEvent *)
{
    if (m_app && check(latha_focus(m_app, 0)))
        schedulePump();
}

void AppWindow::closeEvent(QCloseEvent *e)
{
    if (!m_app || m_finished) {
        e->accept();
        return;
    }
    const int r = latha_close(m_app);
    if (r == 0) {
        e->ignore();
        schedulePump();
        return;
    }
    e->accept();
    finish(r < 0 ? 3 : 0);
}
