// qt2wasm/runtime.h: the Qt side of a component's life.
//
// A component is a reactor: the host calls an export, it returns, and nothing
// runs in between. So there is no QCoreApplication::exec(). Instead every
// generated export opens a `qt2wasm::Call`, which
//   * creates the Q(Gui|Core)Application on first use, with the non-blocking
//     qt2wasm event dispatcher, and registers bundled fonts (GUI builds);
//   * on return, delivers posted events, deferred deletes and due timers, so
//     queued connections and QTimer::singleShot still fire (on this or a
//     later call).
#pragma once

#include <QtGlobal>

namespace qt2wasm {

// Create the application object if it does not exist yet. Idempotent.
void ensureApplication();

// Deliver posted events and fire due timers. Never blocks.
void pump();

struct Call {
    Call() { ensureApplication(); }
    ~Call() { pump(); }
    Call(const Call &) = delete;
    Call &operator=(const Call &) = delete;
};

}  // namespace qt2wasm

// Mark a QObject class for export:  QT2WASM_EXPORT("ns:pkg/iface@0.1.0")
#define QT2WASM_EXPORT(ref) Q_CLASSINFO("qt2wasm-interface", ref)
