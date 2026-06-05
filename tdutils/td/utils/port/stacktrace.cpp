/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>

#include "td/utils/port/signals.h"
#include "td/utils/port/stacktrace.h"

#if TD_WINDOWS
#include <DbgHelp.h>
#else
#if TD_DARWIN || __GLIBC__
#include <dlfcn.h>
#include <execinfo.h>
#endif
#endif

#ifdef TD_HAVE_LIBBACKTRACE
#include <backtrace.h>
#include <cxxabi.h>
#include <dlfcn.h>
#endif

#if TD_LINUX || TD_FREEBSD
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if TD_LINUX
#include <sys/prctl.h>
#endif
#endif

#if TD_DARWIN
#include <mach-o/dyld.h>
#endif

namespace td {

namespace {

#ifdef TD_HAVE_LIBBACKTRACE
struct BacktraceState {
  int skip;
  int index;
};

// Hex number formatter (builds string backwards in embedded buffer)
class SafeHex {
 public:
  SafeHex(uintptr_t value) {
    char *ptr = buf_ + sizeof(buf_) - 1;
    *ptr = '\0';
    if (value == 0) {
      *--ptr = '0';
    } else {
      while (value != 0) {
        int digit = static_cast<int>(value & 0xF);
        *--ptr = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
        value >>= 4;
      }
    }
    slice_ = td::Slice(ptr, buf_ + sizeof(buf_) - 1);
  }

  operator td::Slice() const {
    return slice_;
  }

 private:
  char buf_[20];
  td::Slice slice_;
};

// Decimal number formatter (builds string backwards in embedded buffer)
class SafeDec {
 public:
  SafeDec(int value) {
    char *ptr = buf_ + sizeof(buf_) - 1;
    *ptr = '\0';
    if (value == 0) {
      *--ptr = '0';
    } else {
      while (value > 0) {
        *--ptr = '0' + (value % 10);
        value /= 10;
      }
    }
    slice_ = td::Slice(ptr, buf_ + sizeof(buf_) - 1);
  }

  operator td::Slice() const {
    return slice_;
  }

 private:
  char buf_[20];
  td::Slice slice_;
};

// Simple writer for building strings safely in signal handlers
class SafeWriter {
 public:
  SafeWriter(char *buf, size_t size) : begin_(buf), remaining_(buf, size) {
  }

  SafeWriter &operator<<(td::Slice str) {
    size_t to_copy = std::min(str.size(), remaining_.size());
    if (to_copy > 0) {
      std::memcpy(remaining_.data(), str.data(), to_copy);
      remaining_ = remaining_.substr(to_copy);
    }
    return *this;
  }

  td::Slice as_slice() const {
    size_t written = remaining_.begin() - begin_;
    return td::Slice(begin_, written);
  }

 private:
  char *begin_;
  td::MutableSlice remaining_;
};

void backtrace_error_callback(void *data, const char *msg, int errnum) {
  signal_safe_write("libbacktrace error: ", false);
  signal_safe_write(td::Slice(msg ? msg : "unknown error"), false);
  signal_safe_write("\n", false);
}

int backtrace_full_callback(void *data, uintptr_t pc, const char *filename, int lineno, const char *function) {
  BacktraceState *state = static_cast<BacktraceState *>(data);

  if (state->skip > 0) {
    state->skip--;
    return 0;
  }

  // Get module info for this PC
  Dl_info dlinfo;
  Slice module_cstr = "unknown";
  uintptr_t module_base = 0;
  if (dladdr(reinterpret_cast<void *>(pc), &dlinfo) && dlinfo.dli_fname) {
    module_cstr = td::CSlice(dlinfo.dli_fname);
    auto pos = module_cstr.rfind('/');
    if (pos != CSlice::npos) {
      module_cstr.remove_prefix(pos + 1);
    }
    module_base = reinterpret_cast<uintptr_t>(dlinfo.dli_fbase);
  }

  // Choose best function name: libbacktrace > dladdr > unknown
  const char *func_name = function ? function : (dlinfo.dli_sname ? dlinfo.dli_sname : nullptr);

  // Demangle C++ function names
  char *demangled = nullptr;
  if (func_name) {
    int status;
    demangled = abi::__cxa_demangle(func_name, nullptr, nullptr, &status);
    if (status == 0 && demangled) {
      func_name = demangled;
    }
  }

  // Extract file basename
  const char *file_cstr = nullptr;
  if (filename && filename[0]) {
    const char *slash = strrchr(filename, '/');
    file_cstr = slash ? slash + 1 : filename;
  }

  // Build output: "    #N  function at file:line (module+0xoffset) [0xaddress]\n"
  char buf[2048];
  SafeWriter w(buf, sizeof(buf));

  w << "    #" << SafeDec(state->index) << "  " << td::Slice(func_name ? func_name : "??").truncate(400);

  if (file_cstr) {
    w << " at " << td::Slice(file_cstr).truncate(80);
    if (lineno > 0) {
      w << ":" << SafeDec(lineno);
    }
  }

  uintptr_t offset = module_base ? (pc - module_base) : pc;
  w << " (" << module_cstr.truncate(100) << "+0x" << SafeHex(offset) << ") [0x" << SafeHex(pc) << "]\n";

  signal_safe_write(w.as_slice(), false);

  free(demangled);
  state->index++;
  return 0;
}

// Single atomic lock for backtrace operations
static std::atomic<bool> backtrace_lock{false};

// Deleter that releases the lock
struct BacktraceUnlock {
  void operator()(backtrace_state *) const {
    backtrace_lock.store(false, std::memory_order_release);
  }
};

using LockedBacktraceState = std::unique_ptr<backtrace_state, BacktraceUnlock>;

// Try to get backtrace state with lock. Returns nullptr if lock is taken.
static LockedBacktraceState get_locked_backtrace_state() {
  // Try to acquire lock
  bool expected = false;
  if (!backtrace_lock.compare_exchange_strong(expected, true, std::memory_order_acquire)) {
    return nullptr;  // Lock taken by another thread
  }

  // Get or initialize state (protected by lock, no atomics needed)
  static backtrace_state *state = nullptr;

  if (state == nullptr) {
    // Initialize on first use
    const char *exe_path = nullptr;
#if TD_LINUX || TD_FREEBSD
    static char path_buf[512];
    ssize_t res = readlink("/proc/self/exe", path_buf, sizeof(path_buf) - 1);
    if (res >= 0) {
      path_buf[res] = '\0';
      exe_path = path_buf;
    }
#elif TD_DARWIN
    static char path_buf[512];
    uint32_t size = sizeof(path_buf);
    if (_NSGetExecutablePath(path_buf, &size) == 0) {
      exe_path = path_buf;
    }
#endif

    state = backtrace_create_state(exe_path, 1, backtrace_error_callback, nullptr);
  }

  return LockedBacktraceState(state, BacktraceUnlock{});
}
#endif

void print_backtrace(void) {
#if TD_WINDOWS
  void *stack[100];
  HANDLE process = GetCurrentProcess();
  SymInitialize(process, nullptr, 1);
  unsigned frames = CaptureStackBackTrace(0, 100, stack, nullptr);
  signal_safe_write("------- Stack Backtrace -------\n", false);
  for (unsigned i = 0; i < frames; i++) {
    td::uint8 symbol_buf[sizeof(SYMBOL_INFO) + 256];
    auto symbol = (SYMBOL_INFO *)symbol_buf;
    memset(symbol_buf, 0, sizeof(symbol_buf));
    symbol->MaxNameLen = 255;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    SymFromAddr(process, (DWORD64)(stack[i]), nullptr, symbol);
    // Don't use sprintf here because it is not signal-safe
    char buf[256 + 32];
    char *buf_ptr = buf;
    if (frames - i - 1 < 10) {
      strcpy(buf_ptr, " ");
      buf_ptr += strlen(buf_ptr);
    }
    _itoa(frames - i - 1, buf_ptr, 10);
    buf_ptr += strlen(buf_ptr);
    strcpy(buf_ptr, ": [");
    buf_ptr += strlen(buf_ptr);
    _ui64toa(td::uint64(symbol->Address), buf_ptr, 16);
    buf_ptr += strlen(buf_ptr);
    strcpy(buf_ptr, "] ");
    buf_ptr += strlen(buf_ptr);
    strcpy(buf_ptr, symbol->Name);
    buf_ptr += strlen(buf_ptr);
    strcpy(buf_ptr, "\n");
    signal_safe_write(td::Slice{buf, strlen(buf)}, false);
  }
#else
#if TD_DARWIN || __GLIBC__
  void *buffer[128];
  int nptrs = backtrace(buffer, 128);
  signal_safe_write("------- Stack Backtrace -------\n", false);
  backtrace_symbols_fd(buffer, nptrs, 2);
  signal_safe_write("-------------------------------\n", false);
#endif
#endif
}

void print_backtrace_libbacktrace(void) {
#ifdef TD_HAVE_LIBBACKTRACE
  signal_safe_write("--- Enhanced Backtrace (libbacktrace) ---\n", false);

  auto locked_state = get_locked_backtrace_state();
  if (!locked_state) {
    signal_safe_write("(another thread is printing backtrace)\n", false);
    signal_safe_write("------------------------------------------\n", false);
    return;
  }

  BacktraceState state;
  state.skip = 0;
  state.index = 0;

  // Skip 1 frame: this function itself
  backtrace_full(locked_state.get(), 1, backtrace_full_callback, backtrace_error_callback, &state);

  signal_safe_write("------------------------------------------\n", false);
#endif
}

void print_backtrace_gdb(void) {
#if TD_LINUX || TD_FREEBSD
  char pid_buf[30];
  char *pid_buf_begin = pid_buf + sizeof(pid_buf);
  pid_t pid = getpid();
  *--pid_buf_begin = '\0';
  do {
    *--pid_buf_begin = static_cast<char>(pid % 10 + '0');
    pid /= 10;
  } while (pid > 0);

  char name_buf[512];
  ssize_t res = readlink("/proc/self/exe", name_buf, 511);  // TODO works only under Linux
  if (res >= 0) {
    name_buf[res] = 0;

#if TD_LINUX
#if defined(PR_SET_DUMPABLE)
    if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) < 0) {
      signal_safe_write("Can't set dumpable\n");
      return;
    }
#endif
#if defined(PR_SET_PTRACER)
    // We can't use event fd because we are in a signal handler
    int fds[2];
    bool need_set_ptracer = true;
    if (pipe(fds) < 0) {
      need_set_ptracer = false;
      signal_safe_write("Can't create a pipe\n");
    }
#endif
#endif

    int child_pid = fork();
    if (child_pid < 0) {
      signal_safe_write("Can't fork() to run gdb\n");
      return;
    }
    if (!child_pid) {
#if TD_LINUX && defined(PR_SET_PTRACER)
      if (need_set_ptracer) {
        char c;
        if (read(fds[0], &c, 1) < 0) {
          signal_safe_write("Failed to read from pipe\n");
        }
      }
#endif
      dup2(2, 1);  // redirect output to stderr
      execlp("gdb", "gdb", "--batch", "-n", "-ex", "thread", "-ex", "thread apply all bt full", name_buf, pid_buf_begin,
             nullptr);
      return;
    } else {
#if TD_LINUX && defined(PR_SET_PTRACER)
      if (need_set_ptracer) {
        if (prctl(PR_SET_PTRACER, child_pid, 0, 0, 0) < 0) {
          signal_safe_write("Can't set ptracer\n");
        }
        if (write(fds[1], "a", 1) != 1) {
          signal_safe_write("Can't write to pipe\n");
        }
      }
#endif
      waitpid(child_pid, nullptr, 0);
    }
  } else {
    signal_safe_write("Can't get name of executable file to pass to gdb\n");
  }
#endif
}

}  // namespace

void Stacktrace::print_to_stderr(const PrintOptions &options) {
  print_backtrace();
  if (options.use_libbacktrace) {
    print_backtrace_libbacktrace();
  }
  if (options.use_gdb) {
    print_backtrace_gdb();
  }
}

void Stacktrace::init() {
#ifdef TD_HAVE_LIBBACKTRACE
  // Initialize libbacktrace state early (no lock needed during init)
  get_locked_backtrace_state();
#endif
#if TD_DARWIN || __GLIBC__
  // backtrace needs to be called once to ensure that next calls are async-signal-safe
  void *buffer[1];
  backtrace(buffer, 1);
#endif
}

}  // namespace td
