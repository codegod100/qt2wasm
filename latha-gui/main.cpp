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
#include <QEventLoop>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <atomic>
#include <unistd.h>
#include <thread>
#include <QDesktopServices>
#include <QProcess>
#include <QUrl>
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
    if (const auto dark = Accent::instance()->dark())
        return *dark;
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
// host.open-url: $LATHA_GUI_OPEN_URL (a command; the URL is its last
// argument) if set, else the desktop's way (QDesktopServices: the XDG portal
// in a sandbox, else xdg-open / the desktop's opener).
int cbOpenUrl(void *, LathaStr u)
{
    const QUrl url(qstr(u), QUrl::StrictMode);
    if (!url.isValid() || url.scheme().isEmpty())
        return 0;
    const QString cmd = qEnvironmentVariable("LATHA_GUI_OPEN_URL");
    if (!cmd.isEmpty()) {
        QStringList args = QProcess::splitCommand(cmd);
        if (args.isEmpty())
            return 0;
        const QString program = args.takeFirst();
        return QProcess::startDetached(program, args << url.toString(QUrl::FullyEncoded)) ? 1 : 0;
    }
    return QDesktopServices::openUrl(url) ? 1 : 0;
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

// From any thread (latha:proc): hop to the GUI thread.
void cbWake(void *)
{
    if (!qApp)
        return;
    QMetaObject::invokeMethod(qApp, [] {
        if (g_window)
            g_window->procWake();
    }, Qt::QueuedConnection);
}

// --- first run: a window while the app is fetched precompiled or compiled ---------------

struct PrepareState {
    std::atomic<int> stage{0};
    std::atomic<uint64_t> done{0}, total{0};
};

void cbPrepareProgress(void *user, int stage, uint64_t done, uint64_t total)
{
    auto *s = static_cast<PrepareState *>(user);
    s->stage = stage;
    s->total = total;
    s->done = done;
}

// latha_prepare on a worker thread behind a small window, so the first run of a big app (webby: a 200 MB
// component, half a minute of Cranelift) shows something at once instead of nothing. Closing the window quits.
bool prepareWithWindow(const QByteArray &wasmPath, const QString &name, const QIcon &icon, bool debug)
{
    QElapsedTimer t;
    t.start();
    QWidget w;
    w.setWindowTitle(name);
    auto *row = new QHBoxLayout(&w);
    row->setContentsMargins(20, 18, 20, 18);
    row->setSpacing(16);
    auto *iconLabel = new QLabel;
    if (!icon.isNull())
        iconLabel->setPixmap(icon.pixmap(48, 48));
    iconLabel->setAlignment(Qt::AlignTop);
    row->addWidget(iconLabel);
    auto *col = new QVBoxLayout;
    col->setSpacing(8);
    row->addLayout(col, 1);
    auto *title = new QLabel(QStringLiteral("Getting %1 ready").arg(name));
    QFont f = title->font();
    f.setPointSizeF(f.pointSizeF() * 1.25);
    f.setBold(true);
    title->setFont(f);
    auto *bar = new QProgressBar;
    bar->setRange(0, 0);
    bar->setTextVisible(false);
    auto *detail = new QLabel(QStringLiteral("Only the first time: later starts take a second."));
    detail->setWordWrap(true);
    col->addWidget(title);
    col->addWidget(bar);
    col->addWidget(detail);
    w.setMinimumWidth(440);
    w.show();

    const bool quitOnClose = qApp->quitOnLastWindowClosed();
    qApp->setQuitOnLastWindowClosed(false);
    // The worker cannot be stopped half way: closing the window ends the process.
    const auto closed = QObject::connect(qApp, &QGuiApplication::lastWindowClosed, [] { _exit(0); });

    PrepareState st;
    int rc = 0;
    QByteArray err;
    QEventLoop loop;
    std::thread worker([&] {
        char *e = nullptr;
        rc = latha_prepare(wasmPath.constData(), 0, cbPrepareProgress, &st, &e);
        if (e) {
            err = e;
            latha_string_free(e);
        }
        QMetaObject::invokeMethod(&loop, [&loop] { loop.quit(); }, Qt::QueuedConnection);
    });
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&] {
        const uint64_t done = st.done, total = st.total;
        switch (st.stage.load()) {
        case 1: // downloading (bytes)
            title->setText(QStringLiteral("Downloading %1").arg(name));
            detail->setText(total ? QStringLiteral("%1 of %2 MB, compiled ahead of time. Only the first time.")
                                        .arg(done >> 20).arg(total >> 20)
                                  : QStringLiteral("%1 MB, compiled ahead of time. Only the first time.")
                                        .arg(done >> 20));
            if (total) {
                bar->setRange(0, 1000);
                bar->setValue(int(done * 1000 / total));
            }
            break;
        case 2: // compiling (functions)
            title->setText(QStringLiteral("Compiling %1 for this computer").arg(name));
            detail->setText(QStringLiteral("Only the first time: later starts take a second."));
            if (total) {
                bar->setRange(0, 1000);
                bar->setValue(int(std::min<uint64_t>(done * 1000 / total, 990)));
            }
            break;
        }
    });
    timer.start(100);
    loop.exec();
    worker.join();
    timer.stop();
    QObject::disconnect(closed);
    w.hide();
    qApp->setQuitOnLastWindowClosed(quitOnClose);
    if (debug)
        fprintf(stderr, "latha-gui: %s prepared in %lld ms\n", wasmPath.constData(), (long long)t.elapsed());
    if (rc != 0) {
        fprintf(stderr, "latha-gui: %s\n", err.isEmpty() ? "cannot compile the app" : err.constData());
        return false;
    }
    return true;
}

void usage(FILE *f)
{
    fprintf(f,
            "usage: latha-gui [options] APP.wasm [args...]\n"
            "  runs a latha:gui component (gui/wit/gui.wit) in a window, or a WASI command\n"
            "  (core module or wasi:cli/run component) in the terminal, without one\n"
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
            "  --app-info            print the app's id, name, version and icons (latha:app, latha:icon) and exit\n"
            "  --precompile DIR      compile APP.wasm for a prebuilt store into DIR/<key>.cwasm.zst and exit\n"
            "environment: LATHA_GUI_THEME=dark|light, LATHA_GUI_CACHE=0, LATHA_GUI_CACHE_DIR,\n"
            "  LATHA_GUI_PREBUILT=0 (never fetch a precompiled app), LATHA_GUI_PREBUILT_URL (its stores),\n"
            "  LATHA_GUI_DESKTOP_ENTRY=0 (do not write ~/.local/share/applications/<app id>.desktop),\n"
            "  LATHA_GUI_UPDATE=0 (no app updates), LATHA_GUI_UPDATE_DIR, LATHA_GUI_UPDATE_INTERVAL (s, default 21600)\n");
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
    QByteArray precompileDir;
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
        } else if (!strcmp(a, "--precompile")) {
            precompileDir = next();
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
    QByteArray wasmPath = argv[i];
    if (size.isEmpty()) {
        const QList<QByteArray> p = qgetenv("LATHA_GUI_WINDOW_SIZE").split('x');
        if (p.size() == 2)
            size = QSize(p[0].toInt(), p[1].toInt());
    }

    if (!precompileDir.isEmpty()) {
        char *err = nullptr;
        char *out = latha_precompile(wasmPath.constData(), precompileDir.constData(), &err);
        if (!out) {
            fprintf(stderr, "latha-gui: %s\n", err ? err : "cannot compile");
            latha_string_free(err);
            return 2;
        }
        printf("%s\n", out);
        latha_string_free(out);
        return 0;
    }

    // The newest copy of the app: a downloaded update of it (host/src/update.rs) when that is newer than the file
    // given, which stays the app's name for the desktop entry and the next update check.
    const QByteArray givenPath = wasmPath;
    {
        char *r = latha_update_resolve(givenPath.constData(), nullptr);
        if (r) {
            wasmPath = r;
            latha_string_free(r);
        }
    }

    // A WASI command (js.wasm, a test runner, ...): run it here, in the
    // terminal, without Qt or a window; the current directory is "." too.
    const int kind = latha_wasm_kind(wasmPath.constData());
    if ((kind == 1 || kind == 2) && !appInfo) {
        if (!noRoot && !guestDirs.contains(".")) {
            dirs << ".";
            guestDirs << ".";
        }
        QList<QByteArray> cargs{QFileInfo(QString::fromLocal8Bit(wasmPath)).fileName().toUtf8()};
        for (int j = i + 1; j < argc; ++j)
            cargs << QByteArray(argv[j]);
        std::vector<LathaStr> ca, hd, gd;
        for (const auto &a : cargs)
            ca.push_back(LathaStr{a.constData(), size_t(a.size())});
        for (int j = 0; j < dirs.size(); ++j) {
            hd.push_back(LathaStr{dirs[j].constData(), size_t(dirs[j].size())});
            gd.push_back(LathaStr{guestDirs[j].constData(), size_t(guestDirs[j].size())});
        }
        LathaOptions clo{hd.data(), gd.data(), hd.size(), noCache ? 1 : 0};
        char *cerr = nullptr;
        const int code = latha_run(wasmPath.constData(), &clo, ca.data(), ca.size(), &cerr);
        if (code < 0) {
            fprintf(stderr, "latha-gui: %s\n", cerr ? cerr : "cannot run");
            latha_string_free(cerr);
            return 2;
        }
        return code;
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
        printf("id: %s\nname: %s\nversion: %s\nupdate: %s\n", qPrintable(meta.id), qPrintable(meta.name),
               qPrintable(meta.version), qPrintable(meta.update));
        if (wasmPath != givenPath)
            printf("runs: %s (a downloaded update)\n", wasmPath.constData());
        printf("icons: %lld\n", (long long)meta.icons.size());
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
        && qgetenv("LATHA_GUI_DESKTOP_ENTRY") != "0" && installDesktopEntry(meta, QString::fromLocal8Bit(givenPath))
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
    cb.open_url = cbOpenUrl;
    cb.quit = cbQuit;
    cb.pty_spawn = cbPtySpawn;
    cb.pty_write = cbPtyWrite;
    cb.pty_read = cbPtyRead;
    cb.pty_resize = cbPtyResize;
    cb.pty_exit_code = cbPtyExitCode;
    cb.pty_close = cbPtyClose;
    cb.wake = cbWake;
    LathaOptions lo{hostDirs.data(), gDirs.data(), hostDirs.size(), noCache ? 1 : 0};

    // First run of a big app: fetch or compile it behind a window that says so, then load it from the cache.
    if (latha_needs_prepare(wasmPath.constData(), noCache ? 1 : 0)
        && !prepareWithWindow(wasmPath, meta.name.isEmpty() ? appName : meta.name, QApplication::windowIcon(),
                              opts.debug))
        return 2;

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
    // Look for a newer build of the app in the background (at most every few hours; update.rs); it runs from the
    // next start. Not for test runs.
    if (opts.screenshot.isEmpty() && opts.quitAfterMs < 0 && !meta.update.isEmpty())
        std::thread([givenPath, debug = opts.debug] {
            char *version = nullptr, *newPath = nullptr, *uerr = nullptr;
            const int r = latha_update_check(givenPath.constData(), nullptr, 0, &version, &newPath, &uerr);
            if (r > 0)
                fprintf(stderr, "latha-gui: downloaded version %s of the app (%s); it runs from the next start\n",
                        version, newPath);
            else if (r < 0)
                fprintf(stderr, "latha-gui: update check: %s\n", uerr ? uerr : "failed");
            else if (debug)
                fprintf(stderr, "latha-gui: no update to download\n");
            latha_string_free(version);
            latha_string_free(newPath);
            latha_string_free(uerr);
        }).detach();
    const int code = qapp.exec();
    window.finish(code);
    g_window = nullptr;
    return code;
}
