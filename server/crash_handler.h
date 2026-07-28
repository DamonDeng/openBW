// Best-effort SIGSEGV/SIGABRT/etc. handler for openbw_server.
//
// Purpose: when the sim segfaults in production (K8s pod exit code 139),
// we currently get nothing but the tail of stderr up to the crash --
// no stack, no faulting address. install_crash_handler() plugs a
// signal handler that prints a symbolic backtrace to stderr before
// letting the default action re-raise, so `kubectl logs` after the
// crash shows the frame.
//
// Design constraints for signal handlers:
//   * Only async-signal-safe libc calls. We use write(2), backtrace(3),
//     backtrace_symbols_fd(3). No printf, no malloc-heavy paths.
//   * Handler must terminate deterministically. After printing we
//     re-install SIG_DFL and re-raise the signal so the kernel writes
//     the "normal" exit code (139 for SIGSEGV) and any core dumps
//     configured on the host still fire.
//   * On macOS the backtrace is much less useful without dSYM; on
//     Linux with -rdynamic in the image, backtrace_symbols_fd gets us
//     mangled C++ names -- enough to nav to the frame in source.
//
// Not defensive against fatal errors DURING the handler (double
// fault -> we just die harder). Not thread-safe if multiple threads
// fault simultaneously; the first one printing is good enough.

#pragma once

#include <cstddef>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <initializer_list>
#include <unistd.h>

#include <execinfo.h>

namespace crash_handler {

namespace detail {

// A signal handler must not call printf(3); use write(2). This little
// helper writes a NUL-terminated string in one syscall.
inline void write_cstr(const char* s) {
	if (!s) return;
	size_t n = 0;
	while (s[n]) ++n;
	ssize_t r = ::write(STDERR_FILENO, s, n);
	(void)r;   // ignore short write; we're crashing anyway
}

// hex writer: no printf-family. Emits 0x-prefixed lowercase hex.
inline void write_hex(unsigned long v) {
	char buf[2 + 2 * sizeof(v) + 1];
	buf[0] = '0'; buf[1] = 'x';
	int i = 2 + 2 * (int)sizeof(v);
	buf[i] = '\0';
	--i;
	if (v == 0) { buf[i--] = '0'; }
	else while (v) {
		int nyb = (int)(v & 0xf);
		buf[i--] = (nyb < 10) ? char('0' + nyb) : char('a' + nyb - 10);
		v >>= 4;
	}
	ssize_t r = ::write(STDERR_FILENO, buf, sizeof(buf) - 1);
	(void)r;
}

inline const char* signal_name(int sig) {
	switch (sig) {
		case SIGSEGV: return "SIGSEGV";
		case SIGABRT: return "SIGABRT";
		case SIGBUS:  return "SIGBUS";
		case SIGILL:  return "SIGILL";
		case SIGFPE:  return "SIGFPE";
		default:      return "signal";
	}
}

inline void handler(int sig, siginfo_t* info, void* /*ucontext*/) {
	write_cstr("\n=== crash: ");
	write_cstr(signal_name(sig));
	write_cstr(" pid=");
	write_hex((unsigned long)::getpid());
	write_cstr(" fault_addr=");
	write_hex(info ? (unsigned long)info->si_addr : 0);
	write_cstr(" ===\n");

	// backtrace() writes into a fixed-size buffer; 128 frames is deep
	// enough for any realistic call depth we care about.
	void* frames[128];
	int n = ::backtrace(frames, 128);
	// backtrace_symbols_fd is signal-safe (it allocates no heap; it
	// writes each frame line to fd directly).
	::backtrace_symbols_fd(frames, n, STDERR_FILENO);

	write_cstr("=== end of backtrace ===\n");

	// Re-install default handler and re-raise so kernel writes the
	// expected exit code (128+sig) and any host-side core dump path
	// still fires. Without this we'd exit 0 from the handler frame.
	struct sigaction sa;
	std::memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_DFL;
	sigemptyset(&sa.sa_mask);
	::sigaction(sig, &sa, nullptr);
	::raise(sig);
}

}   // namespace detail

// Install for the fatal signals we care about. Call once from main()
// before anything else. Safe to call multiple times.
inline void install() {
	struct sigaction sa;
	std::memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = detail::handler;
	sigemptyset(&sa.sa_mask);
	// SA_SIGINFO gives us siginfo_t; SA_ONSTACK would let us use a
	// dedicated signal stack (recommended for SIGSEGV where the main
	// stack may be corrupted). We skip signalstack setup for now --
	// backtrace() works from the crashing stack in practice, and stack
	// overflows aren't the shape of bug we're hunting.
	sa.sa_flags = SA_SIGINFO;
	for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE}) {
		::sigaction(sig, &sa, nullptr);
	}
	// Best-effort acknowledgment to the log so someone reading the
	// output knows the handler was actually installed. Uses printf
	// here because we're not in signal context yet.
	std::fprintf(stderr, "[srv] crash_handler installed\n");
	std::fflush(stderr);
}

}   // namespace crash_handler
