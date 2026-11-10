// Copyright (C) 2026 ditherterm. Minimal platform defs for wasm32-wasi (wasi-sdk).
#ifndef QPLATFORMDEFS_H
#define QPLATFORMDEFS_H
#include "qglobal.h"
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include "../common/posix/qplatformdefs.h"
#undef QT_SOCKLEN_T
#define QT_SOCKLEN_T int
#endif
