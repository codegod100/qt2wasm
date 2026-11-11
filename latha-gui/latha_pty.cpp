// SPDX-FileCopyrightText: 2026 Kwi WebAssembly port contributors
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only
#include "latha_pty.h"

#include <QSocketNotifier>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

std::unique_ptr<Pty> Pty::spawn(const QList<QByteArray> &argv, const QList<QPair<QByteArray, QByteArray>> &env,
                                const QByteArray &cwd, quint16 cols, quint16 rows, QString *error)
{
    QList<QByteArray> args = argv;
    if (args.isEmpty()) {
        const char *sh = getenv("SHELL");
        args << QByteArray(sh && *sh ? sh : "/bin/sh");
    }
    QByteArray dir = cwd;
    if (dir.isEmpty()) {
        const char *home = getenv("HOME");
        dir = home && *home ? home : "/";
    }
    // Everything the child needs, made before fork (no allocation after it).
    std::vector<char *> cargv;
    for (auto &a : args)
        cargv.push_back(const_cast<char *>(a.constData()));
    cargv.push_back(nullptr);
    bool hasTerm = false;
    for (const auto &kv : env)
        hasTerm = hasTerm || kv.first == "TERM";

    struct winsize ws = {};
    ws.ws_col = cols ? cols : 80;
    ws.ws_row = rows ? rows : 24;
    int fd = -1;
    pid_t pid = forkpty(&fd, nullptr, nullptr, &ws);
    if (pid < 0) {
        *error = QStringLiteral("forkpty: %1").arg(QString::fromLocal8Bit(strerror(errno)));
        return nullptr;
    }
    if (pid == 0) {
        // The child: default signals (we may have been started with some
        // ignored, e.g. SIGINT from `cmd &`), our env plus the app's.
        for (int s : {SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGPIPE, SIGCHLD, SIGTSTP, SIGTTIN, SIGTTOU})
            signal(s, SIG_DFL);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        if (!hasTerm)
            setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        for (const auto &kv : env)
            setenv(kv.first.constData(), kv.second.constData(), 1);
        if (chdir(dir.constData()) != 0) {
            const char *home = getenv("HOME");
            if (!home || chdir(home) != 0)
                (void)!chdir("/");
        }
        execvp(cargv[0], cargv.data());
        dprintf(2, "latha-gui: cannot run %s: %s\r\n", cargv[0], strerror(errno));
        _exit(127);
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    std::unique_ptr<Pty> p(new Pty);
    p->m_fd = fd;
    p->m_pid = pid;
    p->m_read = new QSocketNotifier(fd, QSocketNotifier::Read, p.get());
    connect(p->m_read, &QSocketNotifier::activated, p.get(), &Pty::onReadable);
    p->m_write = new QSocketNotifier(fd, QSocketNotifier::Write, p.get());
    p->m_write->setEnabled(false);
    connect(p->m_write, &QSocketNotifier::activated, p.get(), [q = p.get()] {
        q->m_write->setEnabled(false);
        QByteArray rest;
        rest.swap(q->m_pending);
        q->write(rest.constData(), rest.size());
    });
    return p;
}

Pty::~Pty()
{
    hangUp();
}

void Pty::onReadable()
{
    char buf[65536];
    for (int i = 0; i < 16; ++i) {
        ssize_t n = ::read(m_fd, buf, sizeof buf);
        if (n > 0) {
            m_out.append(buf, n);
            if (n < (ssize_t)sizeof buf)
                break;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            break;
        // EOF / EIO: the program (and everything on its pty) is gone.
        m_read->setEnabled(false);
        m_write->setEnabled(false);
        reap(true);
        break;
    }
    Q_EMIT activity();
}

void Pty::reap(bool wait)
{
    if (m_pid <= 0)
        return;
    int status = 0;
    pid_t r = waitpid(m_pid, &status, wait ? 0 : WNOHANG);
    if (r == m_pid) {
        m_exit = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        m_pid = -1;
    }
    m_ended = true;
}

void Pty::write(const char *data, size_t len)
{
    if (m_ended || m_fd < 0 || !len)
        return;
    if (!m_pending.isEmpty()) {
        m_pending.append(data, len);
        return;
    }
    while (len) {
        ssize_t n = ::write(m_fd, data, len);
        if (n > 0) {
            data += n;
            len -= n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN) {
            m_pending.append(data, len);
            m_write->setEnabled(true);
        }
        return;
    }
}

QByteArray Pty::takeOutput()
{
    QByteArray o;
    o.swap(m_out);
    return o;
}

void Pty::resize(quint16 cols, quint16 rows)
{
    if (m_ended || m_fd < 0)
        return;
    struct winsize ws = {};
    ws.ws_col = cols;
    ws.ws_row = rows;
    ioctl(m_fd, TIOCSWINSZ, &ws);
}

void Pty::hangUp()
{
    if (m_pid > 0) {
        kill(m_pid, SIGHUP);
        reap(false);
        if (m_pid > 0) {
            // Not gone yet: let it go in the background without a zombie.
            pid_t pid = m_pid;
            m_pid = -1;
            if (fork() == 0) {
                int st;
                for (int i = 0; i < 50 && waitpid(pid, &st, WNOHANG) == 0; ++i)
                    usleep(20000);
                _exit(0);
            }
        }
    }
    if (m_fd >= 0) {
        delete m_read;
        delete m_write;
        m_read = m_write = nullptr;
        ::close(m_fd);
        m_fd = -1;
    }
    m_ended = true;
}
