#include "ve/core/crash.h"
#include "ve/core/log.h"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <execinfo.h>

namespace ve::core {
namespace {

constexpr int kSignals[] = {SIGSEGV, SIGILL, SIGFPE, SIGBUS, SIGABRT};

const char* signal_name(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGBUS: return "SIGBUS";
    case SIGABRT: return "SIGABRT";
    default: return "SIG?";
    }
}

void on_fatal_signal(int sig) {
    crash_dump(sig);
    // Restore default action and re-raise so the kernel reports the death
    // faithfully (core dumps, exit status).
    struct sigaction d {};
    d.sa_handler = SIG_DFL;
    sigaction(sig, &d, nullptr);
    std::raise(sig);
}

} // namespace

void crash_dump(int signal_number) {
    VE_LOG_FATAL("crash: %s — stack dump follows", signal_name(signal_number));
    void* frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, 2); // fd 2 = stderr (async-signal-safe-ish)
    std::fprintf(stderr, "-- end stack dump --\n");
}

void install_crash_handler() {
    for (int sig : kSignals) {
        struct sigaction sa {};
        sa.sa_handler = on_fatal_signal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_NODEFER; // our handler may re-raise this signal
        sigaction(sig, &sa, nullptr);
    }
}

} // namespace ve::core
