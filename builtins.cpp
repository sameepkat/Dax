#include "builtins.hpp"
#include "helper.hpp"
#include "janet_runtime.hpp"
#include "janet_commands.hpp"
#include "shell_host.hpp"
#include "shell_state.hpp"
#include "shell.hpp"
#include "builtins_internal.hpp"
#include "builtins_internal.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <glob.h>
#include <fnmatch.h>
#include <iostream>
#include <pwd.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <ctime>
#include <sys/wait.h>
#include <unistd.h>

namespace {
ShellState &state = shell_state();
auto &aliases = state.aliases;
auto &vars = state.vars;
auto &readonly_vars = state.readonly_vars;
auto &command_hash = state.command_hash;
auto &functions = state.functions;
auto &traps = state.traps;
auto &dirs = state.dirs;
auto &positionals = state.positionals;
auto &shell_name = state.shell_name;
auto &jobs = state.jobs;
auto &status_value = state.status_value;
auto &exit_value = state.exit_value;
auto &exiting = state.exiting;
auto &errexit = state.errexit;
auto &nounset = state.nounset;
auto &allexport = state.allexport;
auto &notify_jobs = state.notify_jobs;
auto &noclobber = state.noclobber;
auto &noglob = state.noglob;
auto &monitor = state.monitor;
auto &noexec = state.noexec;
auto &verbose = state.verbose;
auto &xtrace = state.xtrace;
auto &login_shell = state.login_shell;
auto &interactive_shell = state.interactive_shell;
auto &errexit_ignored_context = state.errexit_ignored_context;
auto &errexit_immune_status = state.errexit_immune_status;
auto &returning = state.returning;
auto &loop_depth = state.loop_depth;
auto &loop_break = state.loop_break;
auto &loop_continue = state.loop_continue;
auto &loop_control_levels = state.loop_control_levels;
auto &substitution_status = state.substitution_status;
auto &substitution_ran = state.substitution_ran;
auto &pending_signal = state.pending_signal;
auto &expansion_error = state.expansion_error;
auto &expansion_error_index = state.expansion_error_index;
auto &getopts_index = state.getopts_index;
auto &getopts_char = state.getopts_char;
auto &local_scopes = state.local_scopes;
std::vector<std::pair<int, pid_t>> process_substitutions;

bool name_ok(const std::string &s) {
  return !s.empty() && (std::isalpha((unsigned char)s[0]) || s[0] == '_') &&
         std::all_of(s.begin() + 1, s.end(), [](unsigned char c) {
           return std::isalnum(c) || c == '_';
         });
}
std::string join(const std::vector<std::string> &a, size_t i = 1) {
  std::string s;
  for (; i < a.size(); ++i) {
    if (!s.empty())
      s += ' ';
    s += a[i];
  }
  return s;
}
int signal_number(std::string name) {
  if (!name.empty() && name[0] == '-')
    name.erase(name.begin());
  if (name.rfind("SIG", 0) == 0)
    name.erase(0, 3);
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  if (name.empty())
    return 0;
  if (std::isdigit(static_cast<unsigned char>(name[0])))
    return std::atoi(name.c_str());
  static const std::unordered_map<std::string, int> signals = {
      {"HUP", SIGHUP},   {"INT", SIGINT},   {"QUIT", SIGQUIT},
      {"ILL", SIGILL},   {"ABRT", SIGABRT}, {"FPE", SIGFPE},
      {"KILL", SIGKILL}, {"SEGV", SIGSEGV}, {"PIPE", SIGPIPE},
      {"ALRM", SIGALRM}, {"TERM", SIGTERM}, {"CHLD", SIGCHLD},
      {"CONT", SIGCONT}, {"STOP", SIGSTOP}, {"TSTP", SIGTSTP},
      {"TTIN", SIGTTIN}, {"TTOU", SIGTTOU}, {"USR1", SIGUSR1},
      {"USR2", SIGUSR2}};
  auto it = signals.find(name);
  return it == signals.end() ? 0 : it->second;
}
bool setvar(const std::string &x, bool export_it = false) {
  auto p = x.find('=');
  if (p == std::string::npos || !name_ok(x.substr(0, p)))
    return false;
  std::string k = x.substr(0, p), v = x.substr(p + 1);
  if (readonly_vars.count(k)) {
    std::cerr << k << ": readonly variable\n";
    return false;
  }
  vars[k] = v;
  if (export_it || allexport)
    return setenv(k.c_str(), v.c_str(), 1) == 0;
  return true;
}
std::optional<std::string> find_command(const std::string &n) {
  if (n.find('/') != std::string::npos)
    return access(n.c_str(), X_OK) == 0 ? std::optional<std::string>(n)
                                        : std::nullopt;
  auto cached = command_hash.find(n);
  if (cached != command_hash.end() && access(cached->second.c_str(), X_OK) == 0)
    return cached->second;
  const char *p = getenv("PATH");
  if (!p)
    return {};
  std::stringstream ss(p);
  std::string d;
  while (std::getline(ss, d, ':')) {
    auto c = (std::filesystem::path(d.empty() ? "." : d) / n).string();
    if (access(c.c_str(), X_OK) == 0) {
      command_hash[n] = c;
      return c;
    }
  }
  return {};
}
int external(const std::vector<std::string> &a, bool foreground = true) {
  if (a.empty())
    return 0;
  auto p = find_command(a[0]);
  if (!p) {
    report_command_launch_error(a[0], ENOENT);
    return 127;
  }
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return 126;
  }
  if (!pid) {
    signal(SIGINT, SIG_DFL);
    std::vector<char *> av;
    for (auto &s : a)
      av.push_back(const_cast<char *>(s.c_str()));
    av.push_back(nullptr);
    execv(p->c_str(), av.data());
    const int exec_error = errno;
    report_command_launch_error(a[0], exec_error);
    _exit(exec_error == ENOENT ? 127 : 126);
  }
  if (!foreground) {
    remember_job(pid, join(a, 0));
    return 0;
  }
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
  }
  return WIFEXITED(st)     ? WEXITSTATUS(st)
         : WIFSIGNALED(st) ? 128 + WTERMSIG(st)
                           : 1;
}
std::optional<int> run(const std::vector<std::string> &a) {
  if (a.empty())
    return 0;
  const auto &c = a[0];
  if (auto result = run_navigation_builtin(a, state)) return result;
  if (auto result = run_history_builtin(a)) return result;
  if (auto result = run_options_builtin(a, state)) return result;
  if (auto result = run_output_builtin(a)) return result;
  if (auto result = run_system_builtin(a)) return result;
  if (auto result = run_environment_builtin(a, state)) return result;
  if (auto result = run_jobs_builtin(a, state)) return result;
  if (auto result = run_lookup_builtin(a, state)) return result;
  if (c == ":")
    return 0;
  if (c == "true")
    return 0;
  if (c == "false")
    return 1;
  if (c == "exit") {
    if (a.size() > 2) {
      std::cerr << "exit: too many arguments\n";
      return 1;
    }
    if (a.size() > 1) {
      char *end = nullptr;
      errno = 0;
      long parsed = std::strtol(a[1].c_str(), &end, 10);
      if (errno || end == a[1].c_str() || *end) {
        std::cerr << "exit: numeric argument required\n";
        exit_value = 2;
        exiting = true;
        return exit_value;
      }
      exit_value = static_cast<int>(parsed & 255);
    } else {
      exit_value = status_value;
    }
    exiting = true;
    History::save_to_file();
    return exit_value;
  }
  if (c == "read") {
    std::string line;
    for (;;) {
      char ch;
      ssize_t count = ::read(STDIN_FILENO, &ch, 1);
      if (count < 0 && errno == EINTR)
        continue;
      if (count < 0)
        return 1;
      if (count == 0)
        return line.empty() ? 1 : 0;
      if (ch == '\n')
        break;
      if (ch != '\r')
        line += ch;
    }
    if (a.size() == 1) {
      set_shell_variable("REPLY", line);
      return 0;
    }
    if (a.size() == 2)
      return set_shell_variable(a[1], line) ? 0 : 1;
    std::istringstream ss(line);
    std::vector<std::string> f;
    std::string x;
    while (ss >> x)
      f.push_back(x);
    for (size_t i = 1; i < a.size(); ++i) {
      std::string v;
      if (i + 1 == a.size()) {
        for (size_t j = i - 1; j < f.size(); ++j) {
          if (j > i - 1)
            v += ' ';
          v += f[j];
        }
      } else if (i - 1 < f.size())
        v = f[i - 1];
      if (!set_shell_variable(a[i], v))
        return 1;
    }
    return 0;
  }
  if (c == "source" || c == ".") {
    if (a.size() < 2)
      return 2;
    if (std::filesystem::path(a[1]).extension() == ".janet") {
      if (!janet_runtime_start()) {
        std::cerr << "dax: could not initialize Janet runtime\n";
        return 1;
      }
      return janet_source_file(a[1], std::vector<std::string>(a.begin() + 2, a.end()));
    }
    std::ifstream f(a[1]);
    if (!f) {
      perror(a[1].c_str());
      return 1;
    }
    std::ostringstream source;
    source << f.rdbuf();
    execute_line(source.str());
    return last_status();
  }
  if (c == "eval") {
    execute_line(join(a));
    return last_status();
  }
  if (c == "return") {
    if (local_scopes.empty()) {
      std::cerr << "return: can only return from a function\n";
      return 1;
    }
    returning = true;
    return a.size() > 1 ? (atoi(a[1].c_str()) & 255) : status_value;
  }
  if (c == "shift") {
    int n = a.size() > 1 ? atoi(a[1].c_str()) : 1;
    if (n < 0 || (size_t)n > positionals.size())
      return 1;
    positionals.erase(positionals.begin(), positionals.begin() + n);
    return 0;
  }
  if (c == "break" || c == "continue") {
    if (loop_depth == 0) {
      std::cerr << c << ": can only be used inside a loop\n";
      return 1;
    }
    if (a.size() > 2) {
      std::cerr << c << ": too many arguments\n";
      return 2;
    }
    unsigned levels = 1;
    if (a.size() == 2) {
      char *end = nullptr;
      errno = 0;
      const unsigned long parsed = std::strtoul(a[1].c_str(), &end, 10);
      if (errno || end == a[1].c_str() || *end || parsed == 0 || parsed > static_cast<unsigned long>(loop_depth)) {
        std::cerr << c << ": loop count out of range\n";
        return 2;
      }
      levels = static_cast<unsigned>(parsed);
    }
    loop_control_levels = levels;
    if (c == "break") loop_break = true;
    else loop_continue = true;
    return 0;
  }
  if (c == "getopts") {
    if (a.size() < 3) {
      std::cerr << "getopts: usage: getopts optstring name\n";
      return 2;
    }
    const char *current = getenv("OPTIND");
    if (getopts_index == 1 && current) {
      size_t n = std::strtoul(current, nullptr, 10);
      if (n > 0)
        getopts_index = n;
    }
    if (getopts_index > positionals.size())
      return 1;
    const std::string &arg = positionals[getopts_index - 1];
    if (arg == "--" || arg.size() < 2 || arg[0] != '-') {
      setenv("OPTIND", std::to_string(getopts_index).c_str(), 1);
      return 1;
    }
    char opt = arg[getopts_char++];
    auto pos = a[1].find(opt);
    std::string result(1, (pos == std::string::npos || opt == ':') ? '?' : opt);
    if (pos != std::string::npos && opt != ':' && pos + 1 < a[1].size() &&
        a[1][pos + 1] == ':') {
      if (getopts_char < arg.size()) {
        setenv("OPTARG", arg.substr(getopts_char).c_str(), 1);
        getopts_char = arg.size();
      } else if (getopts_index < positionals.size()) {
        setenv("OPTARG", positionals[getopts_index].c_str(), 1);
        ++getopts_index;
      } else {
        result = a[1][0] == ':' ? ":" : "?";
        unsetenv("OPTARG");
      }
    }
    if (getopts_char >= arg.size()) {
      getopts_char = 1;
      ++getopts_index;
    }
    vars[a[2]] = result;
    setenv(a[2].c_str(), result.c_str(), 1);
    setenv("OPTIND", std::to_string(getopts_index).c_str(), 1);
    return 0;
  }
  if (c == "trap") {
    if (a.size() == 1) {
      for (auto &[s, v] : traps)
        std::cout << (s == -1 ? "ERR" : s == 0 ? "EXIT" : std::to_string(s)) << ' ' << v << '\n';
      return 0;
    }
    if (a.size() < 3)
      return 2;
    const bool exit_trap = a[2] == "EXIT" || a[2] == "0";
    const bool err_trap = a[2] == "ERR";
    int sig = exit_trap ? 0 : err_trap ? -1 : signal_number(a[2]);
    if (!exit_trap && !err_trap && sig == 0) {
      std::cerr << "trap: " << a[2] << ": unsupported or invalid signal\n";
      return 1;
    }
    if (a[1] == "-" || a[1].empty()) {
      traps.erase(sig);
      if (sig > 0)
        signal(sig, SIG_DFL);
    } else {
      traps[sig] = a[1];
      if (sig > 0)
        signal(sig, [](int s) { pending_signal = s; });
    }
    return 0;
  }
  if (c == "autoload") {
    std::cerr << "autoload: unsupported (no function autoload path)\n";
    return 2;
  }
  if (functions.count(c)) {
    auto saved = positionals;
    positionals.assign(a.begin() + 1, a.end());
    local_scopes.emplace_back();
    returning = false;
    execute_line(functions[c]);
    int result = status_value;
    for (auto &[k, v] : local_scopes.back()) {
      if (v.had_var)
        vars[k] = v.var;
      else
        vars.erase(k);
      if (v.had_env)
        setenv(k.c_str(), v.env.c_str(), 1);
      else
        unsetenv(k.c_str());
    }
    local_scopes.pop_back();
    positionals = std::move(saved);
    returning = false;
    return result;
  }
  return {};
}
} // namespace
bool shell_set_assignment(const std::string &value, bool export_it) {
  return setvar(value, export_it);
}
bool shell_valid_name(const std::string &name) { return name_ok(name); }
std::optional<std::string> shell_find_command(const std::string &name) { return find_command(name); }
int shell_external_command(const std::vector<std::string> &args) { return external(args); }
int shell_signal_number(const std::string &name) { return signal_number(name); }
std::optional<int> handle_builtin(const std::vector<std::string> &a) {
  return run(a);
}
bool shell_define_alias(const std::string &name, const std::string &value) {
  return handle_builtin({"alias", name + "=" + value}).value_or(1) == 0;
}
bool is_builtin(const std::string &n) {
  static const std::vector<std::string> names = {
      "exit",    "cd",       "pwd",     "echo",   "export",  "typeset",
      "local",   "readonly", "unset",   "alias",  "unalias", "history",
      "command", "builtin",  "type",    "whence", "which",   "pushd",
      "popd",    "dirs",     "jobs",    "fg",     "bg",      "wait",
      "disown",  "kill",     "suspend", "read",   "printf",  "test",
      "[",       "true",     "false",   "set",    "setopt",  "unsetopt",
      "hash",    "source",   ".",       "eval",   "return",  "shift",
      "getopts", "trap",     "autoload", "break", "continue", ":",
      "umask", "times", "ulimit", "exec", "fc"};
  return std::find(names.begin(), names.end(), n) != names.end();
}
bool shell_command_resolves(const std::string &name) {
  return is_builtin(name) || aliases.count(name) != 0 || functions.count(name) != 0 || janet_command_exists(name) ||
         find_command(name).has_value();
}
std::vector<std::string> shell_completion_candidates(const std::string &prefix,
                                                     bool command_position,
                                                     bool variable_name,
                                                     const std::string &directory_prefix) {
  std::vector<std::string> result;
  auto add = [&](const std::string &candidate) {
    if (candidate.rfind(prefix, 0) == 0 &&
        std::find(result.begin(), result.end(), candidate) == result.end())
      result.push_back(candidate);
  };
  if (variable_name) {
    for (const auto &[name, value] : vars) { (void)value; add(name); }
    extern char **environ;
    for (char **entry = environ; entry && *entry; ++entry) {
      const char *equals = std::strchr(*entry, '=');
      if (equals) add(std::string(*entry, static_cast<size_t>(equals - *entry)));
    }
    std::sort(result.begin(), result.end());
    return result;
  }
  if (command_position && prefix.find('/') == std::string::npos) {
    static const std::vector<std::string> builtins = {
        ":", "alias", "bg", "break", "builtin", "cd", "command", "continue",
        "dirs", "disown", "echo", "eval", "exec", "exit", "export", "false",
        "fc", "fg", "getopts", "hash", "history", "jobs", "kill", "local",
        "popd", "printf", "pushd", "pwd", "read", "readonly", "return", "set",
        "setopt", "shift", "source", "suspend", "test", "times", "trap", "true",
        "type", "typeset", "ulimit", "unalias", "unset", "unsetopt", "wait", "which",
        "whence"};
    for (const auto &name : builtins) add(name);
    for (const auto &[name, value] : aliases) { (void)value; add(name); }
    for (const auto &[name, body] : functions) { (void)body; add(name); }
    for (const auto &name : janet_command_names()) add(name);
    const char *path = std::getenv("PATH");
    std::stringstream directories(path ? path : "");
    std::string directory;
    while (std::getline(directories, directory, ':')) {
      if (directory.empty()) directory = ".";
      std::error_code error;
      for (std::filesystem::directory_iterator it(directory, error), end;
           !error && it != end; it.increment(error)) {
        const std::string name = it->path().filename().string();
        if (name.rfind(prefix, 0) == 0 && access(it->path().c_str(), X_OK) == 0) add(name);
      }
    }
    std::sort(result.begin(), result.end());
    return result;
  }
  if (!variable_name) {
    std::filesystem::path directory = directory_prefix.empty()
        ? std::filesystem::path(".") : std::filesystem::path(directory_prefix);
    if (!directory_prefix.empty() && directory_prefix[0] == '~') {
      if (const char *home = std::getenv("HOME"))
        directory = std::filesystem::path(std::string(home) + directory_prefix.substr(1));
    }
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory, error), end;
         !error && it != end; it.increment(error)) {
      const std::string name = it->path().filename().string();
      if (prefix.empty() && !name.empty() && name.front() == '.') continue;
      if (name.rfind(prefix, 0) != 0) continue;
      std::string candidate = name;
      std::error_code type_error;
      if (it->is_directory(type_error) && !type_error) candidate.push_back('/');
      add(candidate);
    }
    std::sort(result.begin(), result.end());
    return result;
  }
  return result;
}
void remember_job(int pid, const std::string &cmd) {
  int id = jobs.empty() ? 1 : jobs.back().id + 1;
  jobs.push_back({id, pid, cmd, false});
  std::cout << '[' << id << "] " << pid << '\n';
}
std::vector<std::string> take_completed_job_notifications() {
  std::vector<std::string> notifications;
  if (!notify_jobs) return notifications;
  for (auto it = jobs.begin(); it != jobs.end();) {
    int status = 0;
    const pid_t result = waitpid(it->pid, &status, WNOHANG);
    if (result == it->pid && (WIFEXITED(status) || WIFSIGNALED(status))) {
      notifications.push_back("[" + std::to_string(it->id) + "]  Done " +
                              it->command);
      it = jobs.erase(it);
    } else {
      ++it;
    }
  }
  return notifications;
}
void set_last_status(int s) { status_value = s; }
int last_status() { return status_value; }
bool shell_should_exit() { return exiting; }
int requested_exit_status() { return exit_value; }
void clear_exit_request() { exiting = false; }
static std::string run_command_substitution(const std::string &command) {
  int output_pipe[2];
  if (pipe(output_pipe) != 0) { substitution_status = 1; substitution_ran = true; return {}; }
  std::cout.flush();
  pid_t pid = fork();
  if (pid < 0) {
    close(output_pipe[0]); close(output_pipe[1]);
    substitution_status = 1; substitution_ran = true; return {};
  }
  if (pid == 0) {
    state.err_trap_suppressed = true;
    signal(SIGINT, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    dup2(output_pipe[1], STDOUT_FILENO);
    close(output_pipe[0]); close(output_pipe[1]);
    execute_line(command);
    std::cout.flush(); std::cerr.flush();
    _exit(last_status() & 255);
  }
  close(output_pipe[1]);
  std::string result;
  char buffer[4096];
  ssize_t n;
  constexpr size_t output_cap = 8 * 1024 * 1024;
  while ((n = read(output_pipe[0], buffer, sizeof(buffer))) > 0) {
    size_t room = result.size() < output_cap ? output_cap - result.size() : 0;
    result.append(buffer, std::min(room, static_cast<size_t>(n)));
  }
  close(output_pipe[0]);
  int status = 1;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  substitution_status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  substitution_ran = true;
  while (!result.empty() && result.back() == '\n') result.pop_back();
  return result;
}

static std::string run_process_substitution(const std::string &command,
                                            char direction) {
  int descriptors[2];
  if (pipe(descriptors) != 0) {
    std::perror("dax: process substitution pipe");
    expansion_error = true;
    return {};
  }
  const pid_t child = fork();
  if (child < 0) {
    std::perror("dax: process substitution fork");
    close(descriptors[0]); close(descriptors[1]);
    expansion_error = true;
    return {};
  }
  if (child == 0) {
    state.err_trap_suppressed = true;
    signal(SIGINT, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    for (const auto &[fd, pid] : process_substitutions) { (void)pid; close(fd); }
    const int source = direction == '<' ? descriptors[1] : descriptors[0];
    const int destination = direction == '<' ? STDOUT_FILENO : STDIN_FILENO;
    if (dup2(source, destination) < 0) _exit(125);
    close(descriptors[0]); close(descriptors[1]);
    execute_line(command);
    std::cout.flush(); std::cerr.flush();
    _exit(last_status() & 255);
  }
  if (direction == '<') {
    close(descriptors[1]);
    process_substitutions.emplace_back(descriptors[0], child);
    return "/dev/fd/" + std::to_string(descriptors[0]);
  }
  close(descriptors[0]);
  process_substitutions.emplace_back(descriptors[1], child);
  return "/dev/fd/" + std::to_string(descriptors[1]);
}

size_t process_substitution_mark() { return process_substitutions.size(); }

void finish_process_substitutions(size_t mark) {
  if (mark > process_substitutions.size()) mark = process_substitutions.size();
  for (size_t i = mark; i < process_substitutions.size(); ++i) close(process_substitutions[i].first);
  for (size_t i = mark; i < process_substitutions.size(); ++i) {
    const auto &[fd, pid] = process_substitutions[i];
    (void)fd;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  }
  process_substitutions.resize(mark);
}

static size_t command_substitution_end(const std::string &word, size_t dollar) {
  size_t cursor = dollar + 2;
  int depth = 1;
  char quote = 0;
  for (; cursor < word.size(); ++cursor) {
    const char ch = word[cursor];
    if (quote) {
      if (ch == '\\' && quote == '"' && cursor + 1 < word.size()) { ++cursor; continue; }
      if (ch == quote) quote = 0;
      continue;
    }
    if (ch == '\'' || ch == '"') { quote = ch; continue; }
    if (ch == '$' && cursor + 1 < word.size() && word[cursor + 1] == '(') {
      ++depth; ++cursor; continue;
    }
    if (ch == '(') { ++depth; continue; }
    if (ch == ')' && --depth == 0) return cursor;
  }
  return std::string::npos;
}

void expand_words(std::vector<std::string> &words) {
  std::vector<std::string> expanded_words;
  for (size_t word_index = 0; word_index < words.size(); ++word_index) {
    const auto &w = words[word_index];
    auto fail_expansion = [&]() {
      if (!expansion_error) expansion_error_index = word_index;
      expansion_error = true;
    };
    std::vector<std::string> fields(1), patterns(1);
    std::vector<bool> has_pattern(1, false);
    bool literal = false, double_quoted = false, pending_split = false;
    bool last_ifs_nonwhite = false, last_ifs_whitespace = false;
    auto append_text = [&](const std::string &text, bool split) {
      auto ifs_it = vars.find("IFS");
      const char *environment_ifs = std::getenv("IFS");
      const std::string ifs = ifs_it != vars.end() ? ifs_it->second
                              : environment_ifs ? environment_ifs
                                                : " \t\n";
      for (unsigned char ch : text) {
        const bool ifs_delimiter = split && ifs.find(static_cast<char>(ch)) != std::string::npos;
        if (ifs_delimiter) {
          const bool ifs_whitespace = ch == ' ' || ch == '\t' || ch == '\n';
          if (ifs_whitespace) {
            if (!fields.back().empty()) {
              fields.emplace_back();
              patterns.emplace_back();
              has_pattern.push_back(false);
            }
            last_ifs_whitespace = true;
          } else if (!(last_ifs_nonwhite && last_ifs_whitespace)) {
            fields.emplace_back();
            patterns.emplace_back();
            has_pattern.push_back(false);
            last_ifs_nonwhite = true;
            last_ifs_whitespace = false;
          }
          if (!ifs_whitespace) last_ifs_nonwhite = true;
          pending_split = true;
          continue;
        }
        pending_split = false;
        last_ifs_nonwhite = false;
        last_ifs_whitespace = false;
        fields.back() += static_cast<char>(ch);
        const bool meta = ch == '*' || ch == '?' || ch == '[';
        if (meta && !literal && !double_quoted) has_pattern.back() = true;
        if (meta && (literal || double_quoted)) patterns.back() += '\\';
        patterns.back() += static_cast<char>(ch);
      }
    };
    auto home_expand = [&](size_t start, size_t finish) -> std::optional<std::string> {
      std::string user = w.substr(start + 1, finish - start - 1);
      if (user.empty()) {
        auto home = vars.find("HOME");
        const char *env_home = std::getenv("HOME");
        if (home != vars.end()) return home->second;
        if (env_home) return std::string(env_home);
        return {};
      }
      struct passwd *entry = getpwnam(user.c_str());
      if (entry && entry->pw_dir) return std::string(entry->pw_dir);
      return {};
    };
    size_t assignment_equal = std::string::npos;
    for (size_t j = 0; j < w.size(); ++j) {
      if (w[j] == '=') { assignment_equal = j; break; }
      if (!(std::isalnum(static_cast<unsigned char>(w[j])) || w[j] == '_')) break;
    }
    for (size_t i = 0; i < w.size(); ++i) {
      if (w[i] == '\x01') { literal = !literal; continue; }
      if (w[i] == '\x02') { double_quoted = !double_quoted; continue; }
      if (!literal && !double_quoted && w[i] == '~' &&
          (i == 0 || (assignment_equal != std::string::npos &&
           (i == assignment_equal + 1 || (i > assignment_equal && w[i - 1] == ':'))))) {
        size_t end = i + 1;
        while (end < w.size() && w[end] != '/' && w[end] != ':') ++end;
        auto home = home_expand(i, end);
        if (home) { append_text(*home, false); i = end - 1; continue; }
      }
      if (w[i] == '\x08' && i + 2 < w.size() && w[i + 2] == '\x03') {
        const size_t end = w.find('\x03', i + 3);
        if (end == std::string::npos) { fail_expansion(); break; }
        const std::string path = run_process_substitution(w.substr(i + 3, end - i - 3), w[i + 1]);
        if (path.empty()) { fail_expansion(); break; }
        append_text(path, false);
        i = end;
        continue;
      }
      if (w[i] == '\x07' && i + 1 < w.size() && w[i + 1] == '\x03') {
        size_t end = w.find('\x03', i + 2);
        if (end == std::string::npos) { fail_expansion(); break; }
        std::string expr = w.substr(i + 2, end - i - 2);
        if (expr.size() >= 2 && expr.front() == '(' && expr.back() == ')') expr = expr.substr(1, expr.size() - 2);
        std::function<std::optional<long long>(std::string)> eval = [&](std::string e) -> std::optional<long long> {
          auto trim = [](std::string &s) { while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin()); while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back(); };
          trim(e);
          while (e.size() >= 2 && e.front() == '(' && e.back() == ')') { e = e.substr(1, e.size()-2); trim(e); }
          int depth = 0;
          for (const std::string ops : {"|", "^", "&", "<<", ">>", "+-", "*/%"}) {
            depth = 0;
            for (size_t p = e.size(); p-- > 0;) {
              if (e[p] == ')') ++depth; else if (e[p] == '(') --depth;
              if (depth || p == 0 || ops.find(e[p]) == std::string::npos) continue;
              if ((e[p] == '+' || e[p] == '-') && (p == 0 || std::string("+-*/%(<>").find(e[p-1]) != std::string::npos)) continue;
              size_t width = (p && ((e.substr(p-1,2)=="<<") || (e.substr(p-1,2)==">>"))) ? 2 : 1;
              if ((e.substr(p,2)=="<<") || (e.substr(p,2)==">>")) width = 2;
              auto l = eval(e.substr(0,p)); auto r = eval(e.substr(p+width));
              if (!l || !r) return {};
              switch (e[p]) { case '+': return *l+*r; case '-': return *l-*r; case '*': return *l**r; case '/': if (!*r) return {}; return *l / *r; case '%': if (!*r) return {}; return *l % *r; case '&': return *l & *r; case '^': return *l ^ *r; case '|': return *l | *r; case '<': return *l << *r; case '>': return *l >> *r; }
            }
          }
          if (!e.empty() && (e[0]=='+' || e[0]=='-' || e[0]=='!')) { auto v=eval(e.substr(1)); if(!v)return {}; return e[0]=='-'?-*v:e[0]=='!'?!*v:*v; }
          if (!e.empty() && (std::isalpha((unsigned char)e[0]) || e[0]=='_')) { auto it=vars.find(e); const char *env=getenv(e.c_str()); e=it!=vars.end()?it->second:(env?env:"0"); trim(e); }
          char *ep=nullptr; errno=0; long long value=std::strtoll(e.c_str(),&ep,0); if(errno || ep==e.c_str() || *ep)return {}; return value;
        };
        auto value = eval(expr);
        if (!value) { fail_expansion(); break; }
        append_text(std::to_string(*value), !double_quoted);
        i = end;
        continue;
      }
      if (w[i] == '\x03') {
        size_t end = w.find('\x03', i + 1);
        if (end == std::string::npos) { fail_expansion(); break; }
        append_text(run_command_substitution(w.substr(i + 1, end - i - 1)), !double_quoted);
        i = end;
        continue;
      }
      if (literal || w[i] != '$') {
        append_text(std::string(1, w[i]), false);
        continue;
      }
      if (i + 1 < w.size() && w[i + 1] == '(') {
        const size_t end = command_substitution_end(w, i);
        if (end == std::string::npos) { fail_expansion(); break; }
        append_text(run_command_substitution(w.substr(i + 2, end - i - 2)), !double_quoted);
        i = end;
        continue;
      }
      if (i + 1 < w.size() && w[i + 1] == '$') {
        append_text(std::to_string(getpid()), !double_quoted); ++i; continue;
      }
      if (i + 1 < w.size() && w[i + 1] == '?') {
        append_text(std::to_string(status_value), !double_quoted); ++i; continue;
      }
      if (i + 1 < w.size() && w[i + 1] == '#') {
        append_text(std::to_string(positionals.size()), !double_quoted); ++i; continue;
      }
      if (i + 1 < w.size() && (w[i + 1] == '@' || w[i + 1] == '*')) {
        const char special = w[i + 1];
        if (double_quoted && special == '@') {
          for (size_t j = 0; j < positionals.size(); ++j) {
            if (j) { fields.emplace_back(); patterns.emplace_back(); has_pattern.push_back(false); }
            append_text(positionals[j], false);
          }
          if (positionals.empty() && fields.size() == 1 && fields[0].empty() && i == 0 && i + 2 == w.size()) fields.clear();
        } else if (double_quoted) {
          auto ifs = vars.find("IFS");
          const char *env_ifs = std::getenv("IFS");
          const std::string sep = ifs != vars.end() ? ifs->second : env_ifs ? env_ifs : " \t\n";
          std::string value;
          for (size_t j = 0; j < positionals.size(); ++j) { if (j && !sep.empty()) value += sep[0]; value += positionals[j]; }
          append_text(value, false);
        } else {
          for (size_t j = 0; j < positionals.size(); ++j) {
            if (j) append_text(" ", true);
            append_text(positionals[j], true);
          }
        }
        ++i; continue;
      }
      size_t b = i + 1, e = b;
      std::string parameter_operator, parameter_word;
      std::string parameter_name;
      bool length_expansion = false;
      bool substring_expansion = false, replace_expansion = false, replace_all = false;
      long long substring_offset = 0, substring_length = 0;
      bool has_substring_length = false;
      std::string replace_pattern, replace_value;
      char array_mode = 0;
      size_t array_index = 0;
      bool indexed_array = false;
      if (b < w.size() && w[b] == '{') {
        b++; e = w.find('}', b);
        if (e == std::string::npos) { append_text("$", false); continue; }
        std::string expression = w.substr(b, e - b);
        if (!expression.empty() && expression[0] == '#') {
          length_expansion = true;
          expression.erase(expression.begin());
        }
        const size_t bracket = expression.find('[');
        if (bracket != std::string::npos && expression.back() == ']') {
          const std::string subscript = expression.substr(bracket + 1, expression.size() - bracket - 2);
          expression.resize(bracket);
          if (subscript == "@" || subscript == "*") array_mode = subscript[0];
          else {
            char *subscript_end = nullptr;
            const unsigned long parsed = std::strtoul(subscript.c_str(), &subscript_end, 10);
            if (subscript.empty() || subscript_end == subscript.c_str() || *subscript_end || parsed > 1000000) {
              fail_expansion();
              break;
            }
            indexed_array = true;
            array_index = static_cast<size_t>(parsed);
          }
        }
        {
          size_t removal_at = expression.find("##");
          if (removal_at != std::string::npos) {
            parameter_operator = "##";
            parameter_word = expression.substr(removal_at + 2);
            expression.resize(removal_at);
          } else if ((removal_at = expression.find("%%")) != std::string::npos) {
            parameter_operator = "%%";
            parameter_word = expression.substr(removal_at + 2);
            expression.resize(removal_at);
          } else if ((removal_at = expression.find('#')) != std::string::npos) {
            parameter_operator = "#";
            parameter_word = expression.substr(removal_at + 1);
            expression.resize(removal_at);
          } else if ((removal_at = expression.find('%')) != std::string::npos) {
            parameter_operator = "%";
            parameter_word = expression.substr(removal_at + 1);
            expression.resize(removal_at);
          }
          if (!parameter_operator.empty()) {
            // Pattern-removal operators are handled after reading the variable.
          } else {
          for (const std::string op : {":-", ":=", ":+", ":?"}) {
            size_t at = expression.find(op);
            if (at != std::string::npos) {
              parameter_operator = op;
              parameter_word = expression.substr(at + op.size());
              expression.resize(at);
              break;
            }
          }
          if (parameter_operator.empty()) {
            const size_t slash = expression.find('/');
            const size_t slash2 = slash == std::string::npos ? slash : expression.find('/', slash + 1);
            if (slash != std::string::npos && slash2 != std::string::npos) {
              replace_expansion = true;
              replace_all = slash + 1 < expression.size() && expression[slash + 1] == '/';
              const size_t pattern_begin = slash + (replace_all ? 2 : 1);
              const size_t separator = expression.find('/', pattern_begin);
              if (separator == std::string::npos) {
                replace_expansion = false;
              } else {
                replace_pattern = expression.substr(pattern_begin, separator - pattern_begin);
                replace_value = expression.substr(separator + 1);
                expression.resize(slash);
              }
            } else {
              const size_t colon = expression.find(':');
              if (colon != std::string::npos && colon + 1 < expression.size()) {
                std::string spec = expression.substr(colon + 1);
                while (!spec.empty() && std::isspace(static_cast<unsigned char>(spec.front()))) spec.erase(spec.begin());
                const size_t second_colon = spec.find(':');
                const std::string offset_text = spec.substr(0, second_colon);
                char *offset_end = nullptr;
                errno = 0;
                substring_offset = std::strtoll(offset_text.c_str(), &offset_end, 10);
                if (!errno && offset_end != offset_text.c_str() && !*offset_end) {
                  substring_expansion = true;
                  if (second_colon != std::string::npos) {
                    const std::string length_text = spec.substr(second_colon + 1);
                    char *length_end = nullptr;
                    errno = 0;
                    substring_length = std::strtoll(length_text.c_str(), &length_end, 10);
                    if (errno || length_end == length_text.c_str() || *length_end)
                      substring_expansion = false;
                    else has_substring_length = true;
                  }
                  if (substring_expansion) expression.resize(colon);
                }
              }
            }
          }
          }
          if (parameter_operator.empty()) {
            for (const std::string op : {"-", "=", "+", "?"}) {
              size_t at = expression.find(op);
              if (at != std::string::npos) {
                parameter_operator = op;
                parameter_word = expression.substr(at + op.size());
                expression.resize(at);
                break;
              }
            }
          }
        }
        parameter_name = expression;
        i = e;
      } else if (b < w.size() && std::isdigit((unsigned char)w[b])) {
        e = b + 1; i = e - 1;
      } else {
        while (e < w.size() && (std::isalnum((unsigned char)w[e]) || w[e] == '_')) ++e;
        if (e == b) { append_text("$", false); continue; }
        i = e - 1;
      }
      std::string k = parameter_name.empty() ? w.substr(b, e - b) : parameter_name;
      if (k == "0") {
        append_text(shell_name, !double_quoted);
        continue;
      }
      if (k.size() == 1 && std::isdigit((unsigned char)k[0])) {
        size_t n = k[0] - '1';
        if (n < positionals.size()) append_text(positionals[n], !double_quoted);
        continue;
      }
      auto it = vars.find(k);
      const char *v = getenv(k.c_str());
      auto array_it = shell_state().arrays.find(k);
      const bool is_set = it != vars.end() || v != nullptr || array_it != shell_state().arrays.end();
      std::string value = it != vars.end() ? it->second : (v ? v : "");
      if (array_it != shell_state().arrays.end() && !array_mode && !indexed_array)
        value = array_it->second.empty() ? std::string() : array_it->second.front();
      auto strip_quote_markers = [](std::string text) {
        text.erase(std::remove(text.begin(), text.end(), '\x01'), text.end());
        text.erase(std::remove(text.begin(), text.end(), '\x02'), text.end());
        return text;
      };
      if (replace_expansion && is_set && array_it == shell_state().arrays.end()) {
        const std::string pattern = strip_quote_markers(replace_pattern);
        const std::string replacement = strip_quote_markers(replace_value);
        size_t cursor = 0;
        while (cursor <= value.size()) {
          bool matched = false;
          for (size_t start = cursor; start <= value.size() && !matched; ++start) {
            for (size_t end = value.size(); end >= start; --end) {
              if (fnmatch(pattern.c_str(), value.substr(start, end - start).c_str(), 0) == 0) {
                value.replace(start, end - start, replacement);
                cursor = start + replacement.size();
                matched = true;
                break;
              }
              if (end == start) break;
            }
          }
          if (!matched || !replace_all) break;
          if (cursor == value.size() && pattern.empty()) break;
          if (cursor > value.size()) break;
        }
      }
      if (substring_expansion && is_set && array_it == shell_state().arrays.end()) {
        const long long size = static_cast<long long>(value.size());
        long long start = substring_offset < 0 ? size + substring_offset : substring_offset;
        start = std::clamp(start, 0LL, size);
        long long end = size;
        if (has_substring_length) {
          end = substring_length < 0 ? size + substring_length : start + substring_length;
          end = std::clamp(end, start, size);
        }
        value = value.substr(static_cast<size_t>(start), static_cast<size_t>(end - start));
      }
      if (array_mode || indexed_array) {
        if (array_it == shell_state().arrays.end()) {
          if (nounset) { std::cerr << "dax: " << k << ": unset variable\n"; fail_expansion(); }
        } else if (length_expansion) {
          const size_t length = array_mode ? array_it->second.size()
                                  : array_index < array_it->second.size()
                                      ? array_it->second[array_index].size() : 0;
          append_text(std::to_string(length), !double_quoted);
        } else if (indexed_array) {
          if (array_index < array_it->second.size()) append_text(array_it->second[array_index], !double_quoted);
        } else if (array_mode == '@' && double_quoted) {
          for (size_t j = 0; j < array_it->second.size(); ++j) {
            if (j) { fields.emplace_back(); patterns.emplace_back(); has_pattern.push_back(false); }
            append_text(array_it->second[j], false);
          }
          if (array_it->second.empty() && fields.size() == 1 && fields[0].empty()) fields.clear();
        } else if (array_mode == '*' && double_quoted) {
          auto ifs = vars.find("IFS");
          const char *env_ifs = std::getenv("IFS");
          const std::string sep = ifs != vars.end() ? ifs->second : env_ifs ? env_ifs : " \t\n";
          std::string joined;
          for (size_t j = 0; j < array_it->second.size(); ++j) {
            if (j && !sep.empty()) joined += sep[0];
            joined += array_it->second[j];
          }
          append_text(joined, false);
        } else {
          for (size_t j = 0; j < array_it->second.size(); ++j) {
            if (j) append_text(" ", true);
            append_text(array_it->second[j], true);
          }
        }
      } else if (length_expansion) {
        append_text(std::to_string(value.size()), !double_quoted);
      } else if (parameter_operator == "#" || parameter_operator == "##" ||
                 parameter_operator == "%" || parameter_operator == "%%") {
        const bool prefix = parameter_operator[0] == '#';
        const bool longest = parameter_operator.size() == 2;
        std::string pattern = parameter_word;
        pattern.erase(std::remove(pattern.begin(), pattern.end(), '\x01'), pattern.end());
        pattern.erase(std::remove(pattern.begin(), pattern.end(), '\x02'), pattern.end());
        if (prefix) {
          size_t match = std::string::npos;
          for (size_t n = 0; n <= value.size(); ++n)
            if (fnmatch(pattern.c_str(), value.substr(0, n).c_str(), 0) == 0) { match = n; if (!longest) break; }
          if (match != std::string::npos) value.erase(0, match);
        } else {
          size_t match = std::string::npos;
          for (size_t n = 0; n <= value.size(); ++n)
            if (fnmatch(pattern.c_str(), value.substr(n).c_str(), 0) == 0) { match = n; if (!longest) break; }
          if (match != std::string::npos) value.erase(match);
        }
        append_text(value, !double_quoted);
      } else if (!parameter_operator.empty()) {
        const bool colon_operator = parameter_operator[0] == ':';
        const bool condition = colon_operator ? (!is_set || value.empty()) : !is_set;
        const char op = parameter_operator.back();
        if (op == '-' && condition) append_text(parameter_word, !double_quoted);
        else if (op == '=' && condition) {
          if (set_shell_variable(k, parameter_word)) append_text(parameter_word, !double_quoted);
          else fail_expansion();
        } else if (op == '+' && !condition) append_text(parameter_word, !double_quoted);
        else if (op == '?' && condition) {
          std::cerr << "dax: " << k << ": " << (parameter_word.empty() ? "parameter null or not set" : parameter_word) << '\n';
          fail_expansion();
        } else if (is_set) append_text(value, !double_quoted);
      } else if (is_set) append_text(value, !double_quoted);
      else if (nounset) {
        std::cerr << "dax: " << k << ": unset variable\n";
        fail_expansion();
      }
    }
    if (pending_split && fields.size() > 1 && fields.back().empty()) {
      fields.pop_back(); patterns.pop_back(); has_pattern.pop_back();
    }
    for (size_t i = 0; i < fields.size(); ++i) {
      if (!has_pattern[i]) { expanded_words.push_back(fields[i]); continue; }
      if (noglob) {
        expanded_words.push_back(fields[i]);
        continue;
      }
      glob_t matches{};
      int rc = glob(patterns[i].c_str(), 0, nullptr, &matches);
      if (rc == 0 && matches.gl_pathc) {
        for (size_t j = 0; j < matches.gl_pathc; ++j) expanded_words.emplace_back(matches.gl_pathv[j]);
      } else {
        expanded_words.push_back(fields[i]);
      }
      globfree(&matches);
    }
  }
  words = std::move(expanded_words);
}
void expand_aliases(std::vector<ShellToken> &words) {
  for (int i = 0; i < 10 && !words.empty(); ++i) {
    auto it = aliases.find(words[0]);
    if (it == aliases.end())
      break;
    const size_t call_line = words[0].line;
    const size_t call_column = words[0].column;
    const size_t call_begin = words[0].source_begin;
    const size_t call_end = words[0].source_end;
    auto replacement = tokenize(it->second);
    for (auto &token : replacement) {
      token.line = call_line;
      token.column = call_column;
      token.source_begin = call_begin;
      token.source_end = call_end;
    }
    replacement.insert(replacement.end(), words.begin() + 1, words.end());
    words = std::move(replacement);
  }
}
void define_function(const std::string &name, const std::string &body) {
  if (name_ok(name))
    functions[name] = body;
}
bool function_exists(const std::string &name) {
  return functions.count(name) != 0;
}
bool shell_should_return() { return returning; }
void enter_shell_loop() { ++loop_depth; }
void leave_shell_loop() { if (loop_depth > 0) --loop_depth; }
int take_loop_control() {
  if (!loop_break && !loop_continue) return 0;
  if (loop_control_levels > 1) {
    --loop_control_levels;
    return 3;
  }
  const int result = loop_break ? 1 : 2;
  loop_break = loop_continue = false;
  loop_control_levels = 1;
  return result;
}
bool loop_control_pending() { return loop_break || loop_continue; }
void run_err_trap(int status) {
  if (status == 0 || state.running_err_trap || state.err_trap_suppressed ||
      errexit_ignored_context > 0 || errexit_immune_status)
    return;
  auto it = traps.find(-1);
  if (it == traps.end()) return;
  ++state.err_trap_generation;
  state.running_err_trap = true;
  const int saved_status = last_status();
  set_last_status(status);
  execute_line(it->second);
  set_last_status(saved_status);
  state.running_err_trap = false;
}
void run_exit_trap() {
  auto it = traps.find(0);
  if (it != traps.end()) {
    const int saved_status = last_status();
    execute_line(it->second);
    set_last_status(saved_status);
  }
}
void run_pending_signal_trap() {
  int sig = pending_signal;
  if (!sig)
    return;
  pending_signal = 0;
  auto it = traps.find(sig);
  if (it != traps.end())
    execute_line(it->second);
}
bool set_shell_variable(const std::string &name, const std::string &value,
                        bool export_value) {
  const size_t bracket = name.find('[');
  if (bracket != std::string::npos && name.back() == ']') {
    const std::string array_name = name.substr(0, bracket);
    const std::string index_text = name.substr(bracket + 1, name.size() - bracket - 2);
    if (!name_ok(array_name) || index_text.empty()) return false;
    char *end = nullptr;
    errno = 0;
    const unsigned long index = std::strtoul(index_text.c_str(), &end, 10);
    if (errno || end == index_text.c_str() || *end || index > 1000000) return false;
    auto &array = state.arrays[array_name];
    if (array.size() <= index) array.resize(index + 1);
    array[index] = value;
    return true;
  }
  if (!name_ok(name) || readonly_vars.count(name)) {
    if (readonly_vars.count(name))
      std::cerr << name << ": readonly variable\n";
    return false;
  }
  vars[name] = value;
  if (export_value || allexport || getenv(name.c_str()))
    return setenv(name.c_str(), value.c_str(), 1) == 0;
  return true;
}
void set_shell_arguments(const std::string &name,
                         const std::vector<std::string> &arguments) {
  shell_name = name;
  positionals = arguments;
  getopts_index = 1;
  getopts_char = 1;
}
bool set_shell_option(const std::string &name, bool enabled) {
  if (name == "errexit" || name == "ERR_EXIT")
    errexit = enabled;
  else if (name == "nounset" || name == "NO_UNSET")
    nounset = enabled;
  else if (name == "allexport" || name == "ALL_EXPORT")
    allexport = enabled;
  else if (name == "pipefail")
    state.pipefail = enabled;
  else if (name == "notify" || name == "notify_jobs")
    notify_jobs = enabled;
  else if (name == "noclobber")
    noclobber = enabled;
  else if (name == "noglob")
    noglob = enabled;
  else if (name == "monitor")
    monitor = enabled;
  else if (name == "noexec")
    noexec = enabled;
  else if (name == "verbose")
    verbose = enabled;
  else if (name == "xtrace")
    xtrace = enabled;
  else
    return false;
  return true;
}
bool shell_option_enabled(const std::string &name) {
  if (name == "errexit" || name == "ERR_EXIT") return errexit;
  if (name == "nounset" || name == "NO_UNSET") return nounset;
  if (name == "allexport" || name == "ALL_EXPORT") return allexport;
  if (name == "pipefail") return state.pipefail;
  if (name == "notify" || name == "notify_jobs") return notify_jobs;
  if (name == "noclobber") return noclobber;
  if (name == "noglob") return noglob;
  if (name == "monitor") return monitor;
  if (name == "noexec") return noexec;
  if (name == "verbose") return verbose;
  if (name == "xtrace") return xtrace;
  return false;
}
void set_shell_modes(bool login, bool interactive, bool monitor_was_explicit) {
  login_shell = login;
  interactive_shell = interactive;
  if (!monitor_was_explicit) monitor = interactive;
}
bool shell_noexec_enabled() { return noexec; }
bool shell_verbose_enabled() { return verbose; }
bool shell_xtrace_enabled() { return xtrace; }
bool shell_noglob_enabled() { return noglob; }
bool shell_noclobber_enabled() { return noclobber; }
void trace_shell_command(const std::vector<std::string> &args) {
  if (!xtrace) return;
  const char *prefix = getenv("PS4");
  std::cerr << (prefix ? prefix : "+ ");
  for (size_t i = 0; i < args.size(); ++i) {
    if (i) std::cerr << ' ';
    std::cerr << args[i];
  }
  std::cerr << '\n';
}
bool shell_variable_readonly(const std::string &name) {
  return readonly_vars.count(name) != 0;
}
void begin_word_expansion() { expansion_error = false; expansion_error_index = 0; substitution_status = 0; substitution_ran = false; }
bool word_expansion_succeeded() { return !expansion_error; }
bool word_expansion_ran_substitution() { return substitution_ran; }
int word_expansion_substitution_status() { return substitution_status; }
size_t word_expansion_error_index() { return expansion_error_index; }
void apply_errexit(int status) {
  if (status != 0 && errexit_immune_status) return;
  if (errexit_ignored_context > 0) return;
  if (errexit && status != 0) {
    exiting = true;
    exit_value = status & 255;
  }
}
void begin_errexit_ignored_context() { ++errexit_ignored_context; }
void end_errexit_ignored_context() {
  if (errexit_ignored_context > 0) --errexit_ignored_context;
}
void mark_errexit_immune() { errexit_immune_status = true; }
void clear_errexit_immune() { errexit_immune_status = false; }
