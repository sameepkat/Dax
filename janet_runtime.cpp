#include "janet_runtime.hpp"
#include "janet.h"
#include "builtins.hpp"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <csignal>
#include <thread>
#include <sys/select.h>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#ifdef __linux__
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <stddef.h>
#endif

namespace {
JanetTable *environment = nullptr;
std::string prompt_override;
struct AgentSession { std::string workspace; std::string temporary; int timeout_ms; bool network; size_t output_limit; };
std::unordered_map<int32_t, AgentSession> sessions;
int32_t next_session_id = 1;
bool restricted_mode = false;
std::string restricted_root, restricted_temp;
int approval_tty = -1;

Janet option_get(Janet options, const char *key) {
  Janet symbol = janet_ckeywordv(key);
  if (janet_checktype(options, JANET_TABLE))
    return janet_table_get(janet_unwrap_table(options), symbol);
  if (janet_checktype(options, JANET_STRUCT))
    return janet_struct_get(janet_unwrap_struct(options), symbol);
  return janet_wrap_nil();
}

bool path_is_within(const std::string &path, const std::string &root) {
  auto relative = std::filesystem::path(path).lexically_relative(root);
  return !relative.is_absolute() && (relative.empty() || *relative.begin() != "..");
}

bool ask_for_approval(const std::string &action, const std::vector<std::string> &paths,
                      const std::string &reason) {
  int fd = approval_tty;
  bool close_fd = false;
  if (fd < 0) {
    fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    close_fd = true;
  }
  dprintf(fd, "\nDax approval request\nAction: %s\n", action.c_str());
  for (const auto &path : paths) dprintf(fd, "Path: %s\n", path.c_str());
  dprintf(fd, "Reason: %s\nType 'approve' to allow: ", reason.c_str());
  std::string response;
  char ch;
  while (response.size() < 32) {
    ssize_t n = read(fd, &ch, 1);
    if (n <= 0 || ch == '\n' || ch == '\r') break;
    response += ch;
  }
  if (close_fd) close(fd);
  return response == "approve";
}

bool validate_approval_paths(const std::vector<std::string> &input,
                             std::vector<std::string> &canonical) {
  std::error_code ec;
  std::string root = restricted_mode ? restricted_root : std::filesystem::current_path(ec).string();
  if (ec || root.empty()) return false;
  root = std::filesystem::canonical(root, ec).string();
  if (ec) return false;
  for (const auto &item : input) {
    if (item.find_first_of("\r\n\0") != std::string::npos) return false;
    std::string resolved = std::filesystem::canonical(item, ec).string();
    if (ec || !path_is_within(resolved, root) || resolved == root) return false;
    canonical.push_back(std::move(resolved));
  }
  return !canonical.empty();
}

#ifdef __linux__
bool apply_landlock(const std::string &workspace, const std::string &temporary) {
  uint64_t handled = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE |
      LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
      LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
      LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
      LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
      LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
      LANDLOCK_ACCESS_FS_MAKE_SYM;
  int abi = syscall(__NR_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
  if (abi < 1) return false;
  if (abi >= 2) handled |= LANDLOCK_ACCESS_FS_REFER;
  if (abi >= 3) handled |= LANDLOCK_ACCESS_FS_TRUNCATE;
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
  if (abi >= 5) handled |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
#endif
  landlock_ruleset_attr ruleset_attr{};
  ruleset_attr.handled_access_fs = handled;
  int ruleset = syscall(__NR_landlock_create_ruleset, &ruleset_attr, sizeof(ruleset_attr), 0);
  if (ruleset < 0) return false;
  auto allow = [&](const std::string &path, uint64_t access) {
    int fd = open(path.c_str(), O_PATH | O_CLOEXEC);
    if (fd < 0) return false;
    landlock_path_beneath_attr rule{};
    rule.allowed_access = access;
    rule.parent_fd = fd;
    bool ok = syscall(__NR_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &rule, 0) == 0;
    close(fd);
    return ok;
  };
  bool ok = allow(workspace, handled);
  std::error_code ec;
  if (!temporary.empty()) ok &= allow(temporary, handled);
  const uint64_t read_exec = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
  for (const char *path : {"/usr/bin", "/usr/lib", "/bin", "/sbin", "/lib", "/lib64"}) {
    if (access(path, F_OK) == 0) ok &= allow(path, read_exec);
  }
  if (access("/etc/ld.so.cache", F_OK) == 0)
    ok &= allow("/etc/ld.so.cache", LANDLOCK_ACCESS_FS_READ_FILE);
  if (!ok || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
      syscall(__NR_landlock_restrict_self, ruleset, 0) != 0) {
    close(ruleset);
    return false;
  }
  close(ruleset);
  return true;
}

bool apply_network_filter() {
  struct sock_filter filter[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
#ifdef __NR_socket
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
#endif
#ifdef __NR_socketpair
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_socketpair, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
#endif
#ifdef __NR_connect
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_connect, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
#endif
#ifdef __NR_bind
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_bind, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
#endif
#ifdef __NR_listen
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_listen, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
#endif
#ifdef __NR_accept
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_accept, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
#endif
#ifdef __NR_accept4
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_accept4, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
#endif
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  struct sock_fprog program{static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])), filter};
  return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
         prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
}

bool apply_kernel_hardening_filter(bool block_signals) {
  std::vector<sock_filter> filter;
  filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)));
  auto deny = [&](int syscall_number) {
    filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K,
                              static_cast<unsigned int>(syscall_number), 0, 1));
    filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)));
  };
#ifdef __NR_ptrace
  deny(__NR_ptrace);
#endif
#ifdef __NR_process_vm_readv
  deny(__NR_process_vm_readv);
#endif
#ifdef __NR_process_vm_writev
  deny(__NR_process_vm_writev);
#endif
#ifdef __NR_mount
  deny(__NR_mount);
#endif
#ifdef __NR_umount2
  deny(__NR_umount2);
#endif
#ifdef __NR_pivot_root
  deny(__NR_pivot_root);
#endif
#ifdef __NR_chroot
  deny(__NR_chroot);
#endif
#ifdef __NR_unshare
  deny(__NR_unshare);
#endif
#ifdef __NR_setns
  deny(__NR_setns);
#endif
#ifdef __NR_bpf
  deny(__NR_bpf);
#endif
#ifdef __NR_perf_event_open
  deny(__NR_perf_event_open);
#endif
#ifdef __NR_keyctl
  deny(__NR_keyctl);
#endif
#ifdef __NR_add_key
  deny(__NR_add_key);
#endif
#ifdef __NR_request_key
  deny(__NR_request_key);
#endif
#ifdef __NR_open_by_handle_at
  deny(__NR_open_by_handle_at);
#endif
#ifdef __NR_name_to_handle_at
  deny(__NR_name_to_handle_at);
#endif
#ifdef __NR_userfaultfd
  deny(__NR_userfaultfd);
#endif
#ifdef __NR_kexec_load
  deny(__NR_kexec_load);
#endif
#ifdef __NR_init_module
  deny(__NR_init_module);
#endif
#ifdef __NR_finit_module
  deny(__NR_finit_module);
#endif
#ifdef __NR_delete_module
  deny(__NR_delete_module);
#endif
#ifdef __NR_io_uring_setup
  deny(__NR_io_uring_setup);
#endif
#ifdef __NR_io_uring_enter
  deny(__NR_io_uring_enter);
#endif
#ifdef __NR_io_uring_register
  deny(__NR_io_uring_register);
#endif
  if (block_signals) {
#ifdef __NR_kill
    deny(__NR_kill);
#endif
#ifdef __NR_tkill
    deny(__NR_tkill);
#endif
#ifdef __NR_tgkill
    deny(__NR_tgkill);
#endif
#ifdef __NR_pidfd_send_signal
    deny(__NR_pidfd_send_signal);
#endif
  }
  filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
  struct sock_fprog program{static_cast<unsigned short>(filter.size()), filter.data()};
  return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
         prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
}
#else
bool apply_landlock(const std::string &, const std::string &) { return false; }
bool apply_network_filter() { return false; }
bool apply_kernel_hardening_filter(bool) { return false; }
#endif

Janet c_alias(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  const char *name = janet_getcstring(argv, 0);
  const char *value = janet_getcstring(argv, 1);
  if (!name || !value) janet_panic("alias expects name and value strings");
  auto result = handle_builtin({"alias", std::string(name) + "=" + value});
  return janet_wrap_boolean(result.value_or(1) == 0);
}

Janet c_set_prompt(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  const char *value = janet_getcstring(argv, 0);
  if (!value) janet_panic("set-prompt expects a string");
  prompt_override = value;
  return janet_cstringv(prompt_override.c_str());
}

Janet c_on(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  if (!janet_checktype(argv[0], JANET_KEYWORD) ||
      janet_cstrcmp(janet_unwrap_keyword(argv[0]), "startup") != 0 ||
      !janet_checktype(argv[1], JANET_FUNCTION))
    janet_panic("dax/on supports :startup and a function");
  Janet hook_key = janet_ckeywordv("dax-startup-hooks");
  Janet hooks = janet_table_get(environment, hook_key);
  JanetArray *array = janet_checktype(hooks, JANET_ARRAY)
      ? janet_unwrap_array(hooks) : janet_array(2);
  janet_array_push(array, argv[1]);
  janet_table_put(environment, hook_key, janet_wrap_array(array));
  return argv[1];
}

Janet c_cwd(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 0);
  (void)argv;
  char path[4096];
  if (!getcwd(path, sizeof(path))) janet_panic("could not read current directory");
  return janet_cstringv(path);
}

Janet c_getenv(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  const char *name = janet_getcstring(argv, 0);
  const char *value = name ? getenv(name) : nullptr;
  return value ? janet_cstringv(value) : janet_wrap_nil();
}

Janet c_setenv(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  const char *name = janet_getcstring(argv, 0);
  const char *value = janet_getcstring(argv, 1);
  if (!name || !value || setenv(name, value, 1) != 0)
    janet_panic("could not set environment variable");
  return janet_wrap_true();
}

Janet c_cd(int32_t argc, Janet *argv) {
  janet_arity(argc, 0, 1);
  const char *path = argc ? janet_getcstring(argv, 0) : getenv("HOME");
  if (!path || chdir(path) != 0) janet_panicf("cd: %s", strerror(errno));
  return c_cwd(0, nullptr);
}

Janet c_execute(int32_t argc, Janet *argv) {
  janet_arity(argc, 1, 2);
  const bool is_array = janet_checktype(argv[0], JANET_ARRAY);
  const bool is_tuple = janet_checktype(argv[0], JANET_TUPLE);
  if (!is_array && !is_tuple) janet_panic("expected argv array or tuple");
  JanetArray *items = is_array ? janet_unwrap_array(argv[0]) : nullptr;
  const int32_t count = is_array ? items->count : janet_tuple_length(janet_unwrap_tuple(argv[0]));
  const Janet *values = is_array ? items->data : janet_unwrap_tuple(argv[0]);
  if (!count) janet_panic("argv must not be empty");
  Janet options = janet_wrap_nil();
  int timeout_ms = 0;
  size_t output_limit = 1024 * 1024;
  bool capture_output = true;
  bool restrict_filesystem = false;
  bool network = true;
  std::string workspace;
  std::string temporary;
  std::string cwd;
  std::vector<std::pair<std::string, std::string>> env;
  if (argc == 2) {
    if (!janet_checktype(argv[1], JANET_TABLE) && !janet_checktype(argv[1], JANET_STRUCT)) janet_panic("options must be a table or struct");
    options = argv[1];
    Janet value = option_get(options, "cwd");
    if (!janet_checktype(value, JANET_NIL)) {
      if (!janet_checktype(value, JANET_STRING)) janet_panic(":cwd must be a string");
      cwd = reinterpret_cast<const char *>(janet_unwrap_string(value));
    }
    value = option_get(options, "timeout-ms");
    if (!janet_checktype(value, JANET_NIL)) {
      if (!janet_checkint(value)) janet_panic(":timeout-ms must be an integer");
      timeout_ms = static_cast<int>(janet_unwrap_integer(value));
      if (timeout_ms < 0 || timeout_ms > 24 * 60 * 60 * 1000) janet_panic(":timeout-ms is out of range");
    }
    value = option_get(options, "max-output-bytes");
    if (!janet_checktype(value, JANET_NIL)) {
      if (!janet_checkint(value)) janet_panic(":max-output-bytes must be an integer");
      int64_t limit = janet_unwrap_integer(value);
      if (limit < 0 || limit > 8 * 1024 * 1024) janet_panic(":max-output-bytes must be between 0 and 8388608");
      output_limit = static_cast<size_t>(limit);
    }
    value = option_get(options, "capture-output");
    if (!janet_checktype(value, JANET_NIL)) {
      if (!janet_checktype(value, JANET_BOOLEAN)) janet_panic(":capture-output must be boolean");
      capture_output = janet_unwrap_boolean(value);
    }
    value = option_get(options, "workspace");
    if (!janet_checktype(value, JANET_NIL)) {
      if (!janet_checktype(value, JANET_STRING)) janet_panic(":workspace must be a string");
      std::error_code ec;
      workspace = std::filesystem::canonical(reinterpret_cast<const char *>(janet_unwrap_string(value)), ec).string();
      if (ec || workspace.empty() || !std::filesystem::is_directory(workspace)) janet_panic(":workspace must name an existing directory");
      restrict_filesystem = true;
    }
    value = option_get(options, "temp");
    if (!janet_checktype(value, JANET_NIL)) {
      if (!janet_checktype(value, JANET_STRING)) janet_panic(":temp must be a string");
      temporary = reinterpret_cast<const char *>(janet_unwrap_string(value));
    }
    value = option_get(options, "network");
    if (!janet_checktype(value, JANET_NIL)) {
      if (!janet_checktype(value, JANET_BOOLEAN)) janet_panic(":network must be boolean");
      network = janet_unwrap_boolean(value);
    }
    value = option_get(options, "env");
    if (!janet_checktype(value, JANET_NIL)) {
      if (!janet_checktype(value, JANET_TABLE) && !janet_checktype(value, JANET_STRUCT)) janet_panic(":env must be a table or struct");
      const JanetKV *data = nullptr; int32_t len = 0, capacity = 0;
      janet_dictionary_view(value, &data, &len, &capacity);
      (void)len;
      for (int32_t i = 0; i < capacity; ++i) {
        Janet key = data[i].key, val = data[i].value;
        if (janet_checktype(key, JANET_NIL)) continue;
        if (!janet_checktype(key, JANET_STRING) || !janet_checktype(val, JANET_STRING))
          janet_panic(":env keys and values must be strings");
        env.emplace_back(reinterpret_cast<const char *>(janet_unwrap_string(key)),
                         reinterpret_cast<const char *>(janet_unwrap_string(val)));
      }
    }
  }
  if (restrict_filesystem && cwd.empty()) cwd = workspace;
  if (restrict_filesystem && temporary.empty()) janet_panic(":temp is required with :workspace");
  std::vector<std::string> storage;
  std::vector<char *> args;
  storage.reserve(count);
  args.reserve(count + 1);
  for (int32_t i = 0; i < count; ++i) {
    Janet item = values[i];
    if (!janet_checktype(item, JANET_STRING)) janet_panic("argv values must be strings");
    storage.emplace_back(reinterpret_cast<const char *>(janet_unwrap_string(item)));
  }
  for (auto &item : storage) args.push_back(item.data());
  args.push_back(nullptr);

  int out_pipe[2], err_pipe[2];
  if (capture_output && (pipe(out_pipe) || pipe(err_pipe))) janet_panic("could not create output pipes");
  pid_t pid = fork();
  if (pid < 0) janet_panic("could not start process");
  if (pid == 0) {
    setpgid(0, 0);
    if (capture_output) {
      dup2(out_pipe[1], STDOUT_FILENO);
      dup2(err_pipe[1], STDERR_FILENO);
      close(out_pipe[0]); close(out_pipe[1]); close(err_pipe[0]); close(err_pipe[1]);
    }
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
      dprintf(STDERR_FILENO, "dax/execute: cannot change directory to %s: %s\n", cwd.c_str(), strerror(errno));
      _exit(126);
    }
    for (const auto &[name, value] : env) setenv(name.c_str(), value.c_str(), 1);
#ifdef __linux__
    if (restrict_filesystem && !apply_landlock(workspace, temporary)) {
      dprintf(STDERR_FILENO, "dax/execute: could not establish filesystem confinement\n");
      _exit(125);
    }
    if (!network && !apply_network_filter()) {
      dprintf(STDERR_FILENO, "dax/execute: could not establish network restriction\n");
      _exit(125);
    }
    if ((restrict_filesystem || !network) && !apply_kernel_hardening_filter(true)) {
      dprintf(STDERR_FILENO, "dax/execute: could not establish syscall restrictions\n");
      _exit(125);
    }
#else
    if (restrict_filesystem || !network) {
      dprintf(STDERR_FILENO, "dax/execute: requested sandbox is unsupported on this platform\n");
      _exit(125);
    }
#endif
    execvp(args[0], args.data());
    dprintf(STDERR_FILENO, "%s: %s\n", args[0], strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);
  }
  setpgid(pid, pid);
  if (capture_output) { close(out_pipe[1]); close(err_pipe[1]); }
  std::string out, err;
  bool timed_out = false, truncated = false;
  auto deadline = timeout_ms ? std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms)
                             : std::chrono::steady_clock::time_point::max();
  char buffer[4096];
  // Drain both streams without letting either pipe fill and block the child.
  fd_set readers;
  int maxfd = capture_output ? (out_pipe[0] > err_pipe[0] ? out_pipe[0] : err_pipe[0]) : -1;
  bool out_open = capture_output, err_open = capture_output;
  while (out_open || err_open) {
    FD_ZERO(&readers);
    if (out_open) FD_SET(out_pipe[0], &readers);
    if (err_open) FD_SET(err_pipe[0], &readers);
    timeval timeout_value{};
    timeval *timeout_ptr = nullptr;
    if (timeout_ms) {
      auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now()).count();
      if (remaining <= 0 && !timed_out) {
        timed_out = true;
        kill(-pid, SIGTERM);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        kill(-pid, SIGKILL);
        remaining = 0;
      }
      timeout_value.tv_sec = remaining / 1000000;
      timeout_value.tv_usec = remaining % 1000000;
      timeout_ptr = &timeout_value;
    }
    int ready = select(maxfd + 1, &readers, nullptr, nullptr, timeout_ptr);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ready == 0 && !timed_out) {
      timed_out = true;
      kill(-pid, SIGTERM);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      kill(-pid, SIGKILL);
      continue;
    }
    if (out_open && FD_ISSET(out_pipe[0], &readers)) {
      ssize_t n = read(out_pipe[0], buffer, sizeof(buffer));
      if (n > 0) {
        size_t room = out.size() < output_limit ? output_limit - out.size() : 0;
        out.append(buffer, std::min(room, static_cast<size_t>(n)));
        truncated |= static_cast<size_t>(n) > room;
      } else { close(out_pipe[0]); out_open = false; }
    }
    if (err_open && FD_ISSET(err_pipe[0], &readers)) {
      ssize_t n = read(err_pipe[0], buffer, sizeof(buffer));
      if (n > 0) {
        size_t room = err.size() < output_limit ? output_limit - err.size() : 0;
        err.append(buffer, std::min(room, static_cast<size_t>(n)));
        truncated |= static_cast<size_t>(n) > room;
      } else { close(err_pipe[0]); err_open = false; }
    }
  }
  int status = 1;
  pid_t waited = 0;
  while (waited == 0) {
    waited = waitpid(pid, &status, WNOHANG);
    if (waited < 0 && errno == EINTR) { waited = 0; continue; }
    if (waited < 0) break;
    if (waited == 0) {
      if (timeout_ms && std::chrono::steady_clock::now() >= deadline && !timed_out) {
        timed_out = true;
        kill(-pid, SIGTERM);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        kill(-pid, SIGKILL);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  JanetTable *result = janet_table(5);
  janet_table_put(result, janet_ckeywordv("stdout"), janet_cstringv(out.c_str()));
  janet_table_put(result, janet_ckeywordv("stderr"), janet_cstringv(err.c_str()));
  janet_table_put(result, janet_ckeywordv("exit-code"), janet_wrap_integer(code));
  janet_table_put(result, janet_ckeywordv("timed-out"), janet_wrap_boolean(timed_out));
  janet_table_put(result, janet_ckeywordv("truncated"), janet_wrap_boolean(truncated));
  return janet_wrap_table(result);
}

Janet c_session_create(int32_t argc, Janet *argv) {
  janet_arity(argc, 0, 1);
  Janet opts = argc ? argv[0] : janet_wrap_nil();
  if (argc && !janet_checktype(opts, JANET_TABLE) && !janet_checktype(opts, JANET_STRUCT))
    janet_panic("session options must be a table or struct");
  std::error_code ec;
  Janet workspace_value = argc ? option_get(opts, "workspace") : janet_wrap_nil();
  std::string workspace = janet_checktype(workspace_value, JANET_STRING)
      ? reinterpret_cast<const char *>(janet_unwrap_string(workspace_value))
      : std::filesystem::current_path(ec).string();
  if (ec) janet_panic("could not resolve current directory");
  workspace = std::filesystem::canonical(workspace, ec).string();
  if (ec || workspace.empty() || !std::filesystem::is_directory(workspace))
    janet_panic(":workspace must name an existing directory");
  if (restricted_mode) {
    auto relative = std::filesystem::path(workspace).lexically_relative(restricted_root);
    if (relative.is_absolute() || (!relative.empty() && *relative.begin() == ".."))
      janet_panic("session workspace must stay inside the restricted workspace");
  }
  int timeout = 30000;
  Janet timeout_value = argc ? option_get(opts, "timeout-ms") : janet_wrap_nil();
  if (!janet_checktype(timeout_value, JANET_NIL)) {
    if (!janet_checkint(timeout_value)) janet_panic(":timeout-ms must be an integer");
    timeout = janet_unwrap_integer(timeout_value);
  }
  if (timeout < 1 || timeout > 24 * 60 * 60 * 1000) janet_panic(":timeout-ms is out of range");
  size_t output_limit = 1024 * 1024;
  Janet output_value = argc ? option_get(opts, "max-output-bytes") : janet_wrap_nil();
  if (!janet_checktype(output_value, JANET_NIL)) {
    if (!janet_checkint(output_value)) janet_panic(":max-output-bytes must be an integer");
    int limit = janet_unwrap_integer(output_value);
    if (limit < 0 || limit > 8 * 1024 * 1024) janet_panic(":max-output-bytes is out of range");
    output_limit = static_cast<size_t>(limit);
  }
  bool network = false;
  Janet network_value = argc ? option_get(opts, "network") : janet_wrap_nil();
  if (!janet_checktype(network_value, JANET_NIL)) {
    if (!janet_checktype(network_value, JANET_BOOLEAN)) janet_panic(":network must be boolean");
    network = janet_unwrap_boolean(network_value);
  }
  if (restricted_mode && network) janet_panic("restricted sessions cannot enable network access");
  if (restricted_mode && timeout > 120000) janet_panic("restricted session timeout cannot exceed 120000 ms");
  if (restricted_mode && output_limit > 1024 * 1024) janet_panic("restricted session output cannot exceed 1048576 bytes");
  std::string temporary;
  if (restricted_mode) {
    temporary = restricted_temp;
  } else {
    char temp_template[] = "/tmp/dax-session-XXXXXX";
    char *created = mkdtemp(temp_template);
    if (!created) janet_panic("could not create session temporary directory");
    temporary = created;
  }
  int32_t id = next_session_id++;
  sessions.emplace(id, AgentSession{workspace, temporary, timeout, network, output_limit});
  return janet_wrap_integer(id);
}

Janet c_session_execute(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  if (!janet_checkint(argv[0])) janet_panic("expected session id");
  int32_t id = janet_unwrap_integer(argv[0]);
  auto it = sessions.find(id);
  if (it == sessions.end()) janet_panic("session is closed or unknown");
  JanetTable *opts = janet_table(5);
  janet_table_put(opts, janet_ckeywordv("workspace"), janet_cstringv(it->second.workspace.c_str()));
  janet_table_put(opts, janet_ckeywordv("temp"), janet_cstringv(it->second.temporary.c_str()));
  janet_table_put(opts, janet_ckeywordv("timeout-ms"), janet_wrap_integer(it->second.timeout_ms));
  janet_table_put(opts, janet_ckeywordv("max-output-bytes"), janet_wrap_integer(it->second.output_limit));
  janet_table_put(opts, janet_ckeywordv("network"), janet_wrap_boolean(it->second.network));
  Janet call_args[] = {argv[1], janet_wrap_table(opts)};
  return c_execute(2, call_args);
}

Janet c_session_close(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 1);
  if (!janet_checkint(argv[0])) janet_panic("expected session id");
  auto it = sessions.find(janet_unwrap_integer(argv[0]));
  if (it == sessions.end()) return janet_wrap_false();
  if (!restricted_mode) {
    std::error_code ec;
    std::filesystem::remove_all(it->second.temporary, ec);
  }
  sessions.erase(it);
  return janet_wrap_true();
}

Janet c_request_approval(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 3);
  const char *action = janet_getcstring(argv, 0);
  const char *reason = janet_getcstring(argv, 2);
  if (!action || !reason || std::strchr(action, '\n') || std::strchr(reason, '\n'))
    janet_panic("approval action and reason must be single-line strings");
  std::vector<std::string> input_paths;
  Janet sequence = argv[1];
  int32_t count = 0;
  const Janet *values = nullptr;
  if (janet_checktype(sequence, JANET_ARRAY)) {
    JanetArray *array = janet_unwrap_array(sequence);
    count = array->count; values = array->data;
  } else if (janet_checktype(sequence, JANET_TUPLE)) {
    count = janet_tuple_length(janet_unwrap_tuple(sequence));
    values = janet_unwrap_tuple(sequence);
  } else {
    janet_panic("approval paths must be an array or tuple");
  }
  for (int32_t i = 0; i < count; ++i) {
    if (!janet_checktype(values[i], JANET_STRING)) janet_panic("approval paths must be strings");
    input_paths.emplace_back(reinterpret_cast<const char *>(janet_unwrap_string(values[i])));
  }
  std::vector<std::string> paths;
  if (!validate_approval_paths(input_paths, paths)) return janet_ckeywordv("denied");
  return ask_for_approval(action, paths, reason) ? janet_ckeywordv("approved") : janet_ckeywordv("denied");
}

Janet c_remove_tree(int32_t argc, Janet *argv) {
  janet_fixarity(argc, 2);
  const char *path = janet_getcstring(argv, 0);
  const char *reason = janet_getcstring(argv, 1);
  if (!path || !reason || std::strchr(reason, '\n')) janet_panic("remove-tree expects a path and single-line reason");
  std::vector<std::string> paths;
  if (!validate_approval_paths({path}, paths)) return janet_wrap_false();
  if (!ask_for_approval("delete-tree", paths, reason)) return janet_wrap_false();
  std::error_code ec;
  std::filesystem::remove_all(paths.front(), ec);
  return janet_wrap_boolean(!ec);
}

const JanetReg dax_functions[] = {
    {"alias", c_alias, "Define a Dax shell alias."},
    {"set-prompt", c_set_prompt, "Set the interactive Dax prompt."},
    {"on", c_on, "Register a Dax lifecycle callback."},
    {"cwd", c_cwd, "Return Dax's current working directory."},
    {"getenv", c_getenv, "Read an environment variable."},
    {"setenv", c_setenv, "Set an environment variable."},
    {"cd", c_cd, "Change Dax's current working directory."},
    {"execute", c_execute, "Execute an argv array without shell parsing."},
    {"session-create", c_session_create, "Create a bounded, workspace-confined execution session."},
    {"session-execute", c_session_execute, "Run an argv command in a session."},
    {"session-close", c_session_close, "Close an execution session."},
    {"request-approval", c_request_approval, "Ask the user to approve an action on validated workspace paths."},
    {"remove-tree", c_remove_tree, "Request approval and remove a validated workspace subtree."},
    {nullptr, nullptr, nullptr}};

int eval(const std::string &source, const std::string &name) {
  Janet result;
  const int rc = janet_dostring(environment, source.c_str(), name.c_str(), &result);
  if (rc) {
    JanetString message = janet_to_string(result);
    std::fprintf(stderr, "dax: Janet error: %s\n", message);
    return 1;
  }
  return janet_checkint(result) ? (janet_unwrap_integer(result) & 255) : 0;
}
} // namespace

bool janet_runtime_start() {
  if (environment) return true;
  janet_init();
  environment = janet_core_env(nullptr);
  janet_cfuns_prefix(environment, "dax", dax_functions);
  prompt_override.clear();
  return environment != nullptr;
}

int janet_run_file(const std::string &path, const std::vector<std::string> &args) {
  std::ifstream file(path);
  if (!file) { std::fprintf(stderr, "dax: cannot open Janet script: %s\n", path.c_str()); return 127; }
  std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  JanetArray *array = janet_array(args.size());
  for (const auto &arg : args) janet_array_push(array, janet_cstringv(arg.c_str()));
  janet_def(environment, "args", janet_wrap_array(array), "Command-line arguments passed to this Janet script.");
  return eval(source, path);
}

int janet_run_restricted_file(const std::string &path, const std::string &workspace,
                              const std::vector<std::string> &args) {
#ifndef __linux__
  (void)path; (void)workspace; (void)args;
  std::fprintf(stderr, "dax: restricted Janet mode requires Linux Landlock and seccomp\n");
  return 125;
#else
  std::ifstream file(path);
  if (!file) { std::fprintf(stderr, "dax: cannot open Janet script: %s\n", path.c_str()); return 127; }
  std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (!janet_runtime_start()) return 125;
  std::error_code ec;
  restricted_root = std::filesystem::canonical(workspace, ec).string();
  if (ec || restricted_root.empty() || !std::filesystem::is_directory(restricted_root)) {
    std::fprintf(stderr, "dax: restricted workspace must be an existing directory\n");
    return 125;
  }
  char temp_template[] = "/tmp/dax-agent-XXXXXX";
  char *temp = mkdtemp(temp_template);
  if (!temp) { std::perror("dax: cannot create restricted temporary directory"); return 125; }
  restricted_temp = temp;
  restricted_mode = true;
  approval_tty = open("/dev/tty", O_RDWR | O_CLOEXEC);
  JanetArray *array = janet_array(args.size());
  for (const auto &arg : args) janet_array_push(array, janet_cstringv(arg.c_str()));
  janet_def(environment, "args", janet_wrap_array(array), "Arguments passed to this restricted Janet script.");

  std::vector<Janet> remove_keys;
  const JanetKV *entries = nullptr; int32_t len = 0, capacity = 0;
  janet_dictionary_view(janet_wrap_table(environment), &entries, &len, &capacity);
  (void)len;
  for (int32_t i = 0; i < capacity; ++i) {
    Janet key = entries[i].key;
    if (!janet_checktype(key, JANET_SYMBOL)) continue;
    const char *name = reinterpret_cast<const char *>(janet_unwrap_symbol(key));
    if (!std::strncmp(name, "os/", 3) || !std::strncmp(name, "ffi/", 4))
      remove_keys.push_back(key);
  }
  for (Janet key : remove_keys) janet_table_remove(environment, key);
  for (const char *name : {"dax/alias", "dax/set-prompt", "dax/on", "dax/cd",
                           "dax/getenv", "dax/setenv", "dax/execute"})
    janet_table_remove(environment, janet_csymbolv(name));

  if (!apply_landlock(restricted_root, restricted_temp) || !apply_network_filter() ||
      !apply_kernel_hardening_filter(false) ||
      chdir(restricted_root.c_str()) != 0) {
    std::fprintf(stderr, "dax: could not establish restricted Janet sandbox; refusing to run script\n");
    janet_runtime_stop();
    return 125;
  }
  return eval(source, path);
#endif
}

int janet_run_string(const std::string &source, const std::string &name) { return eval(source, name); }

int janet_run_startup_hook() {
  if (!environment) return 0;
  Janet hooks = janet_table_get(environment, janet_ckeywordv("dax-startup-hooks"));
  if (!janet_checktype(hooks, JANET_ARRAY)) return 0;
  int result = 0;
  JanetArray *array = janet_unwrap_array(hooks);
  for (int32_t i = 0; i < array->count; ++i) {
    Janet callback = array->data[i], out;
    JanetSignal signal = janet_pcall(janet_unwrap_function(callback), 0, nullptr, &out, nullptr);
    if (signal != JANET_SIGNAL_OK) {
      JanetString message = janet_to_string(out);
      std::fprintf(stderr, "dax: Janet startup hook error: %s\n", message);
      result = 1;
    }
  }
  return result;
}

const std::string &janet_prompt() { return prompt_override; }

void janet_runtime_stop() {
  if (!environment) return;
  if (!restricted_mode) {
    for (const auto &[id, session] : sessions) {
      (void)id;
      std::error_code ec;
      std::filesystem::remove_all(session.temporary, ec);
    }
  }
  sessions.clear();
  environment = nullptr;
  janet_deinit();
  if (!restricted_temp.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(restricted_temp, ec);
    restricted_temp.clear();
  }
  if (approval_tty >= 0) { close(approval_tty); approval_tty = -1; }
  restricted_root.clear();
  restricted_mode = false;
}
