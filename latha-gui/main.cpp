// SPDX-FileCopyrightText: 2026 Kwi WebAssembly port contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only
//
// latha-gui: `wasmtime run`, but with a window. Point it at any latha:gui
// component (gui/wit/gui.wit) and it opens a native Qt window, sends the app
// its input and shows the frames it paints. The app runs on wasmtime
// (host/, Rust); apps that import latha:gui/pty get real pseudo-terminals.
//
//   latha-gui [options] APP.wasm [args...]
#include "host/latha_host.h"
#include "accent.h"
#include "appmeta.h"
#include "window.h"

#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QStyleHints>
#include <cstdio>
#include <cstring>

namespace {

AppWindow *g_window = nullptr;
// Imports that arrive during start(), before the window exists.
QString g_title;
quint32 g_cursor = 0, g_minW = 0, g_minH = 0;
int g_quit = -1;

AppWindow *win(void *)
{
    return g_window;
}

QString qstr(LathaStr s)
{
    return QString::fromUtf8(s.ptr, qsizetype(s.len));
}

QByteArray bytes(LathaStr s)
{
    return QByteArray(s.ptr, qsizetype(s.len));
}

void cbRequestFrame(void *u)
{
    if (auto *w = win(u))
        w->requestFrame();
}
void cbRequestTick(void *u, uint32_t ms)
{
    if (auto *w = win(u))
        w->requestTick(ms);
}
void cbSetTitle(void *u, LathaStr t)
{
    g_title = qstr(t);
    if (auto *w = win(u))
        w->setAppTitle(g_title);
}
void cbSetCursor(void *u, uint32_t shape)
{
    g_cursor = shape;
    if (auto *w = win(u))
        w->setAppCursor(shape);
}
void cbSetMinSize(void *u, uint32_t w0, uint32_t h0)
{
    g_minW = w0;
    g_minH = h0;
    if (auto *w = win(u))
        w->setAppMinSize(w0, h0);
}
void cbClipboardText(void *, int sel, LathaOut *out)
{
    auto *cb = QGuiApplication::clipboard();
    const QClipboard::Mode mode = sel ? QClipboard::Selection : QClipboard::Clipboard;
    if (sel && !cb->supportsSelection())
        return;
    const QByteArray t = cb->text(mode).toUtf8();
    latha_out_set(out, t.constData(), size_t(t.size()));
}
void cbSetClipboardText(void *, int sel, LathaStr t)
{
    auto *cb = QGuiApplication::clipboard();
    if (sel && !cb->supportsSelection())
        return;
    cb->setText(qstr(t), sel ? QClipboard::Selection : QClipboard::Clipboard);
}
int cbPrefersDark(void *u)
{
    if (auto *w = win(u))
        return w->prefersDark();
    const QByteArray theme = qgetenv("LATHA_GUI_THEME");
    if (theme == "dark" || theme == "light")
        return theme == "dark";
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}
int cbAccentColor(void *, uint32_t *rgb)
{
    const QColor c = Accent::instance()->color();
    if (!c.isValid())
        return 0;
    *rgb = c.rgb() & 0xffffff;
    return 1;
}
void cbBell(void *)
{
    QApplication::beep();
}
void cbQuit(void *u, int32_t code)
{
    g_quit = code;
    if (auto *w = win(u))
        w->quitApp(code);
}
int64_t cbPtySpawn(void *u, const LathaStr *argv, size_t argc, const LathaStr *env, size_t envc, LathaStr cwd,
                   uint16_t cols, uint16_t rows, LathaOut *err)
{
    auto *w = win(u);
    auto fail = [&](const QByteArray &m) {
        latha_out_set(err, m.constData(), size_t(m.size()));
        return int64_t(-1);
    };
    if (!w)
        return fail("latha-gui: no window yet (spawn after start)");
    QList<QByteArray> a;
    for (size_t i = 0; i < argc; ++i)
        a << bytes(argv[i]);
    QList<QPair<QByteArray, QByteArray>> e;
    for (size_t i = 0; i < envc; ++i)
        e << qMakePair(bytes(env[2 * i]), bytes(env[2 * i + 1]));
    QString msg;
    const int64_t id = w->ptySpawn(a, e, cwd.ptr ? bytes(cwd) : QByteArray(), cols, rows, &msg);
    return id < 0 ? fail(msg.toUtf8()) : id;
}
void cbPtyWrite(void *u, uint32_t id, const uint8_t *data, size_t len)
{
    if (auto *w = win(u))
        if (auto *p = w->pty(id))
            p->write(reinterpret_cast<const char *>(data), len);
}
void cbPtyRead(void *u, uint32_t id, LathaOut *out)
{
    if (auto *w = win(u))
        if (auto *p = w->pty(id)) {
            const QByteArray o = p->takeOutput();
            latha_out_set(out, o.constData(), size_t(o.size()));
        }
}
void cbPtyResize(void *u, uint32_t id, uint16_t cols, uint16_t rows)
{
    if (auto *w = win(u))
        if (auto *p = w->pty(id))
            p->resize(cols, rows);
}
int cbPtyExitCode(void *u, uint32_t id, int32_t *code)
{
    if (auto *w = win(u))
        if (auto *p = w->pty(id))
            if (p->finished()) {
                *code = p->exitCode();
                return 1;
            }
    return 0;
}
void cbPtyClose(void *u, uint32_t id)
{
    if (auto *w = win(u))
        w->ptyClose(id);
}

void usage(FILE *f)
{
    fprintf(f,
            "usage: latha-gui [options] APP.wasm [args...]\n"
            "  runs a latha:gui component (gui/wit/gui.wit) in a window\n"
            "options:\n"
            "  --dir HOST[::GUEST]   let the app open HOST (as GUEST, default the same path)\n"
            "                        (/ is always open as /, unless --no-root)\n"
            "  --no-root             do not give the app the whole file system (/)\n"
            "  --size WxH            window size in logical pixels (else what the app asks for)\n"
            "  --no-cache            compile the app again (no ~/.cache/latha-gui)\n"
            "  --screenshot FILE     save the app's frame to FILE and quit (with --quit-after)\n"
            "  --quit-after MS       quit after MS milliseconds\n"
            "  --type TEXT           type TEXT (\\n = Return) once the app runs (tests)\n"
            "  --type-after MS       when to type (default 500)\n"
            "  --debug               paint timings on stderr (LATHA_GUI_DEBUG=1)\n"
            "  --verify              check every scroll move against a full copy (LATHA_GUI_VERIFY=1)\n"
            "  --app-info            print the app's id, name and icons (latha:app, latha:icon) and exit\n"
            "environment: LATHA_GUI_THEME=dark|light, LATHA_GUI_CACHE=0, LATHA_GUI_CACHE_DIR,\n"
            "  LATHA_GUI_DESKTOP_ENTRY=0 (do not write ~/.local/share/applications/<app id>.desktop)\n");
}

} // namespace

int main(int argc, char **argv)
{
    // Our own arguments come before the wasm path; everything after it is the app's.
    RunOptions opts;
    opts.debug = qEnvironmentVariableIntValue("LATHA_GUI_DEBUG") != 0;
    opts.verify = qEnvironmentVariableIntValue("LATHA_GUI_VERIFY") != 0;
    QList<QByteArray> dirs, guestDirs;
    QSize size;
    bool noCache = false;
    bool noRoot = false;
    bool appInfo = false;
    int i = 1;
    for (; i < argc; ++i) {
        const char *a = argv[i];
        auto next = [&]() -> const char * {
            if (i + 1 >= argc) {
                fprintf(stderr, "latha-gui: %s needs a value\n", a);
                exit(2);
            }
            return argv[++i];
        };
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "--dir")) {
            const QByteArray d = next();
            const qsizetype sep = d.indexOf("::");
            const QByteArray host = sep < 0 ? d : d.left(sep);
            dirs << host;
            guestDirs << (sep < 0 ? QFileInfo(QString::fromLocal8Bit(host)).absoluteFilePath().toUtf8() : d.mid(sep + 2));
        } else if (!strcmp(a, "--no-root")) {
            noRoot = true;
        } else if (!strcmp(a, "--size")) {
            const QList<QByteArray> p = QByteArray(next()).split('x');
            if (p.size() == 2)
                size = QSize(p[0].toInt(), p[1].toInt());
        } else if (!strcmp(a, "--no-cache")) {
            noCache = true;
        } else if (!strcmp(a, "--screenshot")) {
            opts.screenshot = QString::fromLocal8Bit(next());
        } else if (!strcmp(a, "--quit-after")) {
            opts.quitAfterMs = atoi(next());
        } else if (!strcmp(a, "--type")) {
            opts.typeText = QString::fromUtf8(next()).replace(QStringLiteral("\\n"), QStringLiteral("\n"));
        } else if (!strcmp(a, "--type-after")) {
            opts.typeAfterMs = atoi(next());
        } else if (!strcmp(a, "--debug")) {
            opts.debug = true;
        } else if (!strcmp(a, "--verify")) {
            opts.verify = true;
        } else if (!strcmp(a, "--app-info")) {
            appInfo = true;
        } else if (!strcmp(a, "--")) {
            ++i;
            break;
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "latha-gui: unknown option %s\n", a);
            usage(stderr);
            return 2;
        } else {
            break;
        }
    }
    // The whole file system as itself, as for a native program (V's choice);
    // --no-root opts out. An explicit --dir / is not opened twice.
    if (!noRoot && !guestDirs.contains("/")) {
        dirs.prepend("/");
        guestDirs.prepend("/");
    }
    if (i >= argc) {
        usage(stderr);
        return 2;
    }
    if (!opts.screenshot.isEmpty() && opts.quitAfterMs < 0)
        opts.quitAfterMs = 2000;
    const QByteArray wasmPath = argv[i];
    if (size.isEmpty()) {
        const QList<QByteArray> p = qgetenv("LATHA_GUI_WINDOW_SIZE").split('x');
        if (p.size() == 2)
            size = QSize(p[0].toInt(), p[1].toInt());
    }

    // The app's own id, name and icon (custom sections, appmeta.h): its id is
    // the Wayland app id / WM_CLASS, so the desktop groups and labels it as
    // itself (and uses id.desktop when there is one); without one it is a
    // latha-gui window.
    const AppMeta meta = readAppMeta(QString::fromLocal8Bit(wasmPath));
    const QString appId = meta.id.isEmpty() ? QStringLiteral("latha-gui") : meta.id;
    QApplication::setApplicationName(appId);
    QApplication::setDesktopFileName(appId);
    if (appInfo) {
        if (!meta.valid) {
            fprintf(stderr, "latha-gui: %s: not a wasm file\n", wasmPath.constData());
            return 2;
        }
        QCoreApplication core(argc, argv); // for QImage format plugins (SVG)
        printf("id: %s\nname: %s\nicons: %lld\n", qPrintable(meta.id), qPrintable(meta.name),
               (long long)meta.icons.size());
        for (const QByteArray &b : meta.icons) {
            const QImage img = QImage::fromData(b);
            if (img.isNull())
                printf("  %lld bytes, not an image Qt reads\n", (long long)b.size());
            else
                printf("  %dx%d (%lld bytes)\n", img.width(), img.height(), (long long)b.size());
        }
        return 0;
    }
    QApplication qapp(argc, argv);
    // Before any window: it is part of the toplevel (xdg-toplevel-icon on
    // Wayland, _NET_WM_ICON on X11).
    const QIcon appIcon = meta.icon();
    QApplication::setWindowIcon(appIcon.isNull() ? lathaGuiIcon() : appIcon);
    // Plasma's task manager ignores the window icon when it finds a desktop
    // file for the window, and otherwise finds latha-gui.desktop by our
    // process: give the app its own entry (appmeta.h). Not for test runs.
    const QString platform = QGuiApplication::platformName();
    if ((platform == QLatin1String("wayland") || platform == QLatin1String("xcb")) && opts.screenshot.isEmpty()
        && qgetenv("LATHA_GUI_DESKTOP_ENTRY") != "0" && installDesktopEntry(meta, QString::fromLocal8Bit(wasmPath))
        && opts.debug)
        fprintf(stderr, "latha-gui: wrote the desktop entry %s.desktop\n", qPrintable(meta.id));

    // argv[0] stays the file name; latha:app's name is the first window title.
    QString appName = QFileInfo(QString::fromLocal8Bit(wasmPath)).fileName();
    if (appName.endsWith(QLatin1String(".wasm")))
        appName.chop(5);
    const QByteArray appNameUtf8 = appName.toUtf8();
    QList<QByteArray> appArgs{appNameUtf8};
    for (int j = i + 1; j < argc; ++j)
        appArgs << QByteArray(argv[j]);
    std::vector<LathaStr> args;
    for (const auto &a : appArgs)
        args.push_back(LathaStr{a.constData(), size_t(a.size())});
    std::vector<LathaStr> hostDirs, gDirs;
    for (int j = 0; j < dirs.size(); ++j) {
        hostDirs.push_back(LathaStr{dirs[j].constData(), size_t(dirs[j].size())});
        gDirs.push_back(LathaStr{guestDirs[j].constData(), size_t(guestDirs[j].size())});
    }

    LathaCallbacks cb{};
    cb.user = nullptr;
    cb.request_frame = cbRequestFrame;
    cb.request_tick = cbRequestTick;
    cb.set_title = cbSetTitle;
    cb.set_cursor = cbSetCursor;
    cb.set_min_size = cbSetMinSize;
    cb.clipboard_text = cbClipboardText;
    cb.set_clipboard_text = cbSetClipboardText;
    cb.prefers_dark = cbPrefersDark;
    cb.accent_color = cbAccentColor;
    cb.bell = cbBell;
    cb.quit = cbQuit;
    cb.pty_spawn = cbPtySpawn;
    cb.pty_write = cbPtyWrite;
    cb.pty_read = cbPtyRead;
    cb.pty_resize = cbPtyResize;
    cb.pty_exit_code = cbPtyExitCode;
    cb.pty_close = cbPtyClose;
    LathaOptions lo{hostDirs.data(), gDirs.data(), hostDirs.size(), noCache ? 1 : 0};

    QElapsedTimer t;
    t.start();
    char *err = nullptr;
    LathaApp *app = latha_load(wasmPath.constData(), &lo, &cb, args.data(), args.size(), &err);
    if (!app) {
        fprintf(stderr, "latha-gui: %s\n", err ? err : "cannot load");
        latha_string_free(err);
        return 2;
    }
    const qint64 loadMs = t.elapsed();
    // The window exists (hidden) before start: the app may spawn ptys or set
    // its title from start.
    AppWindow window(app, meta.name.isEmpty() ? appName : meta.name, opts);
    g_window = &window;
    uint32_t ww = 0, wh = 0;
    const int r = latha_start(app, args.data(), args.size(), &ww, &wh, &err);
    if (r != 0) {
        fprintf(stderr, "latha-gui: %s: %s\n", appNameUtf8.constData(), r > 0 && err ? err : latha_error(app));
        latha_string_free(err);
        window.finish(2);
        return 2;
    }
    if (opts.debug)
        fprintf(stderr, "latha-gui: %s loaded in %lld ms, started in %lld ms%s\n", wasmPath.constData(),
                (long long)loadMs, (long long)t.elapsed(), latha_has_pty(app) ? " (pty)" : "");
    if (g_quit >= 0) {
        window.finish(g_quit);
        return g_quit;
    }
    if (size.isEmpty()) {
        // The app's wish is in device pixels at scale 1: logical pixels.
        size = QSize(ww ? int(ww) : 800, wh ? int(wh) : 600);
    }
    window.resize(size);
    window.show();
    const int code = qapp.exec();
    window.finish(code);
    g_window = nullptr;
    return code;
}
