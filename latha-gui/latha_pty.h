// SPDX-FileCopyrightText: 2026 Kwi WebAssembly port contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only
//
// Pseudo-terminals for latha:gui/pty: forkpty, output read on the Qt loop
// (QSocketNotifier) into a buffer that the app polls with pty.read.
#pragma once
#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <map>
#include <memory>

class QSocketNotifier;

class Pty : public QObject
{
    Q_OBJECT
public:
    ~Pty() override;
    /// argv empty: $SHELL. env: added to ours. cwd empty: $HOME.
    static std::unique_ptr<Pty> spawn(const QList<QByteArray> &argv, const QList<QPair<QByteArray, QByteArray>> &env,
                                      const QByteArray &cwd, quint16 cols, quint16 rows, QString *error);
    void write(const char *data, size_t len);
    QByteArray takeOutput();
    void resize(quint16 cols, quint16 rows);
    /// Ended, and all of its output was taken.
    bool finished() const { return m_ended && m_out.isEmpty(); }
    int exitCode() const { return m_exit; }
    void hangUp();

Q_SIGNALS:
    /// Output arrived, or the program ended.
    void activity();

private:
    Pty() = default;
    void onReadable();
    void reap(bool wait);

    int m_fd = -1;
    int m_pid = -1;
    bool m_ended = false;
    int m_exit = 0;
    QByteArray m_out;
    QByteArray m_pending; // written while the pty was full
    QSocketNotifier *m_read = nullptr;
    QSocketNotifier *m_write = nullptr;
};
