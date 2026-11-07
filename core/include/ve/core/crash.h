#pragma once

namespace ve::core {

// Installs signal handlers for fatal signals (SEGV/ILL/FPE/BUS/ABRT) that dump
// a stack trace to the log + stderr, then re-raise so the exit status is
// honest. Call once at startup (tools/apps/tests). Safe alongside ASan: our
// sigaction calls happen after ASan's init, so ours win.
void install_crash_handler();

// The dump path, exposed for tests (writes to the active log sink without
// killing the process).
void crash_dump(int signal_number);

} // namespace ve::core
