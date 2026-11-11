// qt2wasm/gui.h: a QtGui program as a GUI component (`qt2wasm build --gui`).
//
// The component exports latha:gui/app (qt2wasm gui/wit/gui.wit): a host such
// as perry-gui opens a native window, sends it input and shows the frames the
// app paints. Inside, the `qt2wasmgui` platform plugin turns that into
// ordinary Qt: QWindow / QRasterWindow / QPainter, key, mouse and wheel
// events, the clipboard, cursors, the dark / light color scheme, HiDPI.
//
// Write the start-up as a function instead of main():
//
//     #include <qt2wasm/gui.h>
//     int qt2wasm_gui_main(int argc, char **argv) {
//         auto *w = new ClockWindow;   // the QGuiApplication already exists
//         w->resize(400, 400);         // the first top-level fills the host window
//         w->show();
//         return 0;                    // never call exec(): the host runs the loop
//     }
//
// The clipboard: qt6-wasi is built without Qt's clipboard feature (no
// QClipboard), so use the functions below; they talk to the host directly.
// When a Qt build has it, QGuiApplication::clipboard() works as well.
//
// A non-zero return makes the host exit with an error. QGuiApplication::quit()
// (or exit(code)) closes the host window. Timers, queued connections and
// QWindow::requestUpdate() work: the host calls back when they are due.
#pragma once

#include <QByteArray>
#include <QList>
#include <QRect>
#include <QString>
#include <QStringList>
#include <functional>
#include <utility>

class QWindow;

extern "C" int qt2wasm_gui_main(int argc, char **argv);

namespace qt2wasm::gui {
QString clipboardText();
void setClipboardText(const QString &text);
/// The X11 primary selection (middle-click paste); "" where the host has none.
QString primaryText();
void setPrimaryText(const QString &text);
/// Ring the host's bell.
void bell();

/// Scroll: move the pixels inside `area` of `window`'s backing store by
/// (dx, dy), in device pixels (the backing store image's own), and tell the
/// host to move them too instead of copying them out again. Unlike
/// QBackingStore::scroll(), which takes logical pixels, this works at any
/// scale. Then paint and flush the strip it uncovered. False: `window` has
/// no backing store here (paint everything instead).
bool scrollWindow(QWindow *window, const QRect &area, int dx, int dy);

#ifdef QT2WASM_GUI_PTY
/// Pseudo-terminals (`qt2wasm build --gui gui-pty`): run `argv` (empty: the
/// user's shell) on a new pty of `cols` x `rows`, with `env` added to the
/// host's environment, in `cwd` (empty: home). `output` gets what the program
/// writes and `exited` its exit code, both called from the host's ticks.
/// Returns the pty's id, or -1 and sets *error.
int ptySpawn(const QStringList &argv, const QList<std::pair<QString, QString>> &env, const QString &cwd, int cols,
             int rows, std::function<void(const QByteArray &)> output, std::function<void(int)> exited,
             QString *error = nullptr);
void ptyWrite(int id, const QByteArray &data);
void ptyResize(int id, int cols, int rows);
/// Hang up (SIGHUP); `exited` is not called.
void ptyClose(int id);
#endif
}  // namespace qt2wasm::gui
