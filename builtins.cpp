#include "builtins.hpp"
#include "helper.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <glob.h>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
std::unordered_map<std::string, std::string> aliases, vars, readonly_vars,
    command_hash;
std::unordered_map<std::string, std::string> functions;
std::unordered_map<int, std::string> traps;
std::vector<std::string> dirs, positionals;
std::string shell_name = "dax";
std::vector<Job> jobs;
int status_value = 0, exit_value = 0;
bool exiting = false, errexit = false, nounset = false, allexport = false;
int errexit_ignored_context = 0;
bool errexit_immune_status = false;
bool returning = false;
int loop_depth = 0;
bool loop_break = false, loop_continue = false;
int substitution_status = 0;
bool substitution_ran = false;
volatile sig_atomic_t pending_signal = 0;
bool expansion_error = false;
size_t expansion_error_index = 0;
size_t getopts_index = 1, getopts_char = 1;
struct SavedVar {
  bool had_var;
  std::string var;
  bool had_env;
  std::string env;
};
std::vector<std::unordered_map<std::string, SavedVar>> local_scopes;

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
  if (c == "cd") {
    if (a.size() > 2) {
      std::cerr << "cd: too many arguments\n";
      return 1;
    }
    const char *h = getenv("HOME");
    const char *old = getenv("OLDPWD");
    std::string p = a.size() > 1 ? a[1] : (h ? h : "");
    if (p == "-")
      p = old ? old : "";
    if (p.empty()) {
      std::cerr << "cd: HOME not set\n";
      return 1;
    }
    if (p[0] == '~' && (p.size() == 1 || p[1] == '/') && h)
      p = std::string(h) + p.substr(1);
    char before[4096], after[4096];
    getcwd(before, sizeof(before));
    if (chdir(p.c_str())) {
      std::cerr << "cd: " << strerror(errno) << '\n';
      return 1;
    }
    if (getcwd(after, sizeof(after))) {
      setenv("OLDPWD", before, 1);
      setenv("PWD", after, 1);
      vars["OLDPWD"] = before;
      vars["PWD"] = after;
    }
    if (a.size() > 1 && a[1] == "-")
      std::cout << getenv("PWD") << '\n';
    return 0;
  }
  if (c == "pwd") {
    char b[4096];
    if (!getcwd(b, sizeof(b))) {
      perror("pwd");
      return 1;
    }
    std::cout << b << '\n';
    return 0;
  }
  if (c == "echo") {
    size_t i = 1;
    bool nl = true;
    if (i < a.size() && a[i] == "-n") {
      ++i;
      nl = false;
    }
    for (size_t j = i; j < a.size(); ++j) {
      if (j > i)
        std::cout << ' ';
      std::cout << a[j];
    }
    if (nl)
      std::cout << '\n';
    return 0;
  }
  if (c == "export" || c == "typeset" || c == "local" || c == "readonly") {
    size_t i = 1;
    if (i == a.size()) {
      for (auto &[k, v] : vars)
        std::cout << (c == "export" ? "export " : "") << k << '=' << v << '\n';
      return 0;
    }
    if (a[i] == "-p" || a[i] == "-x" || a[i] == "-r")
      ++i;
    for (; i < a.size(); ++i) {
      auto p = a[i].find('=');
      std::string key = a[i].substr(0, p);
      if (c == "local" && local_scopes.empty()) {
        std::cerr << "local: can only be used in a function\n";
        return 1;
      }
      if ((c == "local" || c == "typeset") && !local_scopes.empty() &&
          name_ok(key)) {
        if (!local_scopes.back().count(key)) {
          const char *old = getenv(key.c_str());
          auto vi = vars.find(key);
          local_scopes.back()[key] = {vi != vars.end(),
                                      vi != vars.end() ? vi->second : "",
                                      old != nullptr, old ? old : ""};
        }
      }
      if (p != std::string::npos) {
        if (!setvar(a[i], c == "export"))
          return 1;
        if (c == "readonly")
          readonly_vars[key] = 1;
      } else if (c == "readonly")
        readonly_vars[key] = 1;
      else if (c == "export") {
        auto vi = vars.find(key);
        if (vi != vars.end() && setenv(key.c_str(), vi->second.c_str(), 1))
          return 1;
      } else if (c == "local" || c == "typeset") {
        const char *v = getenv(a[i].c_str());
        vars[a[i]] = v ? v : "";
      }
    }
    return 0;
  }
  if (c == "unset") {
    int rc = 0;
    for (size_t i = 1; i < a.size(); ++i) {
      if (a[i] == "-f" && i + 1 < a.size()) {
        functions.erase(a[++i]);
        continue;
      }
      if (readonly_vars.count(a[i])) {
        std::cerr << "unset: " << a[i] << ": readonly variable\n";
        rc = 1;
      } else {
        vars.erase(a[i]);
        unsetenv(a[i].c_str());
      }
    }
    return rc;
  }
  if (c == "alias") {
    if (a.size() == 1) {
      for (auto &[k, v] : aliases)
        std::cout << "alias " << k << "='" << v << "'\n";
      return 0;
    }
    for (size_t i = 1; i < a.size(); ++i) {
      auto p = a[i].find('=');
      if (p == std::string::npos) {
        auto it = aliases.find(a[i]);
        if (it == aliases.end())
          return 1;
        std::cout << "alias " << it->first << "='" << it->second << "'\n";
      } else
        aliases[a[i].substr(0, p)] = a[i].substr(p + 1);
    }
    return 0;
  }
  if (c == "unalias") {
    if (a.size() < 2)
      return 2;
    int rc = 0;
    for (size_t i = 1; i < a.size(); ++i)
      if (!aliases.erase(a[i]))
        rc = 1;
    return rc;
  }
  if (c == "history") {
    if (a.size() == 1)
      History::print();
    else if (a[1] == "-c")
      History::clear();
    else if (a[1] == "-d" && a.size() > 2) {
      try {
        History::remove(std::stoi(a[2]) - 1);
      } catch (...) {
        std::cerr << "history: numeric argument required\n";
        return 2;
      }
    } else {
      try {
        History::print(std::stoi(a[1]));
      } catch (...) {
        std::cerr << "history: numeric argument required\n";
        return 2;
      }
    }
    return 0;
  }
  if (c == "command" || c == "builtin" || c == "type" || c == "whence" ||
      c == "which") {
    if (a.size() < 2)
      return 2;
    size_t i = (a[1] == "-v" || a[1] == "-V" || a[1] == "-p") ? 2 : 1;
    if (c == "command" && i == 1)
      return external(std::vector<std::string>(a.begin() + 1, a.end()));
    if (c == "builtin" && i == 1) {
      std::vector<std::string> nested(a.begin() + 1, a.end());
      auto result = run(nested);
      return result ? *result : 1;
    }
    if (c == "command" && a[1] == "-v") {
      int rc = 0;
      for (size_t n = 2; n < a.size(); ++n) {
        if (is_builtin(a[n]) || functions.count(a[n]))
          std::cout << a[n] << '\n';
        else if (auto path = find_command(a[n]))
          std::cout << *path << '\n';
        else
          rc = 1;
      }
      return rc;
    }
    int rc = 0;
    for (; i < a.size(); ++i) {
      if (is_builtin(a[i]))
        std::cout << a[i] << " is a shell builtin\n";
      else if (functions.count(a[i]))
        std::cout << a[i] << " is a shell function\n";
      else if (aliases.count(a[i]))
        std::cout << a[i] << " is an alias\n";
      else if (auto p = find_command(a[i]))
        std::cout << *p << '\n';
      else {
        std::cerr << a[i] << ": not found\n";
        rc = 1;
      }
    }
    return rc;
  }
  if (c == "pushd" || c == "popd" || c == "dirs") {
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd)))
      return 1;
    if (c == "dirs") {
      std::cout << cwd;
      for (auto &d : dirs)
        std::cout << ' ' << d;
      std::cout << '\n';
      return 0;
    }
    if (c == "pushd" && a.size() > 1) {
      if (chdir(a[1].c_str()))
        return 1;
      dirs.insert(dirs.begin(), cwd);
    } else if (c == "pushd") {
      if (dirs.empty())
        return 1;
      std::string target = dirs.front();
      dirs.erase(dirs.begin());
      if (chdir(target.c_str()))
        return 1;
      dirs.insert(dirs.begin(), cwd);
    } else {
      if (dirs.empty())
        return 1;
      std::string target = dirs.front();
      if (chdir(target.c_str()))
        return 1;
      dirs.erase(dirs.begin());
    }
    getcwd(cwd, sizeof(cwd));
    setenv("PWD", cwd, 1);
    std::cout << cwd << '\n';
    return 0;
  }
  if (c == "jobs") {
    const int current_job = jobs.empty() ? -1 : jobs.back().id;
    const int previous_job = jobs.size() < 2 ? -1 : jobs[jobs.size() - 2].id;
    auto marker_for = [&](int id) {
      return id == current_job ? '+' : id == previous_job ? '-' : ' ';
    };
    for (auto it = jobs.begin(); it != jobs.end();) {
      int st;
      pid_t r = waitpid(it->pid, &st, WNOHANG | WUNTRACED);
      if (r == it->pid && (WIFEXITED(st) || WIFSIGNALED(st))) {
        std::cout << '[' << it->id << ']' << marker_for(it->id) << "  Done "
                  << it->command << '\n';
        it = jobs.erase(it);
      } else {
        if (r == it->pid && WIFSTOPPED(st))
          it->stopped = true;
        std::cout << '[' << it->id << ']' << marker_for(it->id) << "  "
                  << (it->stopped ? "Stopped" : "Running") << ' ' << it->command
                  << " &\n";
        ++it;
      }
    }
    return 0;
  }
  if (c == "fg" || c == "bg" || c == "wait" || c == "disown") {
    if (c == "wait" && a.size() == 1) {
      for (auto it = jobs.begin(); it != jobs.end();) {
        int status = 0;
        while (waitpid(it->pid, &status, 0) < 0 && errno == EINTR) {}
        it = jobs.erase(it);
      }
      return 0;
    }
    if (jobs.empty()) {
      const std::string job = a.size() > 1 ? a[1] : "%+";
      std::cerr << c << ": " << job << ": no such job\n";
      return c == "wait" ? 127 : 1;
    }
    int id = jobs.back().id;
    if (a.size() > 1) {
      std::string x = a[1];
      if (x[0] == '%')
        x.erase(0, 1);
      id = atoi(x.c_str());
    }
    auto it = std::find_if(jobs.begin(), jobs.end(),
                           [&](auto &j) { return j.id == id; });
    if (it == jobs.end()) {
      const std::string job = a.size() > 1 ? a[1] : "%+";
      std::cerr << c << ": " << job << ": no such job\n";
      return c == "wait" ? 127 : 1;
    }
    if (c == "disown") {
      jobs.erase(it);
      return 0;
    }
    if (c == "bg") {
      kill(-it->pid, SIGCONT);
      it->stopped = false;
      return 0;
    }
    if (c == "fg") {
      if (isatty(STDIN_FILENO))
        tcsetpgrp(STDIN_FILENO, it->pid);
      kill(-it->pid, SIGCONT);
    }
    int st = 0;
    waitpid(it->pid, &st, c == "fg" ? WUNTRACED : 0);
    if (c == "fg" && isatty(STDIN_FILENO))
      tcsetpgrp(STDIN_FILENO, getpgrp());
    if (WIFSTOPPED(st))
      it->stopped = true;
    else
      jobs.erase(it);
    return WIFEXITED(st)     ? WEXITSTATUS(st)
           : WIFSIGNALED(st) ? 128 + WTERMSIG(st)
                             : 1;
  }
  if (c == "kill") {
    if (a.size() < 2)
      return 2;
    int sig = SIGTERM;
    size_t i = 1;
    if (a[i][0] == '-') {
      sig = signal_number(a[i++]);
      if (!sig)
        return 2;
    }
    int rc = 0;
    for (; i < a.size(); ++i) {
      pid_t p = atoi(a[i].c_str());
      if (a[i][0] == '%') {
        int id = atoi(a[i].c_str() + 1);
        auto it = std::find_if(jobs.begin(), jobs.end(),
                               [&](auto &j) { return j.id == id; });
        if (it == jobs.end()) {
          rc = 1;
          continue;
        }
        p = -it->pid;
      }
      if (kill(p, sig))
        rc = 1;
    }
    return rc;
  }
  if (c == "suspend") {
    kill(getpid(), SIGSTOP);
    return 0;
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
  if (c == "printf") {
    if (a.size() < 2)
      return 0;
    size_t n = 2;
    for (size_t i = 0; i < a[1].size(); ++i) {
      if (a[1][i] == '%' && i + 1 < a[1].size()) {
        char f = a[1][++i];
        if (f == '%')
          std::cout << '%';
        else if (f == 's')
          std::cout << (n < a.size() ? a[n++] : "");
        else if (f == 'd' || f == 'i')
          std::cout << atoi(n < a.size() ? a[n++].c_str() : "0");
        else
          return 1;
      } else if (a[1][i] == '\\' && i + 1 < a[1].size()) {
        char x = a[1][++i];
        std::cout << (x == 'n' ? '\n' : x == 't' ? '\t' : x);
      } else
        std::cout << a[1][i];
    }
    return 0;
  }
  if (c == "test" || c == "[") {
    size_t end = a.size();
    if (c == "[") {
      if (end < 2 || a.back() != "]") {
        std::cerr << "[: missing ]\n";
        return 2;
      }
      --end;
    }
    auto eval = [&](auto &&self, size_t b, size_t e) -> int {
      if (b == e)
        return 1;
      if (a[b] == "!")
        return self(self, b + 1, e) == 0 ? 0 : 1;
      if (e - b == 1)
        return a[b].empty() ? 1 : 0;
      if (e - b == 2) {
        struct stat st;
        const auto &op = a[b];
        const auto &v = a[b + 1];
        bool ok = false;
        if (op == "-n")
          ok = !v.empty();
        else if (op == "-z")
          ok = v.empty();
        else if (op == "-e" || op == "-a")
          ok = access(v.c_str(), F_OK) == 0;
        else if (op == "-f")
          ok = stat(v.c_str(), &st) == 0 && S_ISREG(st.st_mode);
        else if (op == "-d")
          ok = stat(v.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        else if (op == "-r")
          ok = access(v.c_str(), R_OK) == 0;
        else if (op == "-w")
          ok = access(v.c_str(), W_OK) == 0;
        else if (op == "-x")
          ok = access(v.c_str(), X_OK) == 0;
        else if (op == "-s")
          ok = stat(v.c_str(), &st) == 0 && st.st_size > 0;
        else
          return 2;
        return ok ? 0 : 1;
      }
      for (size_t i = b + 1; i + 1 < e; ++i)
        if (a[i] == "-a" || a[i] == "-o") {
          int l = self(self, b, i), r = self(self, i + 1, e);
          return a[i] == "-a" ? (l == 0 && r == 0 ? 0 : 1)
                              : (l == 0 || r == 0 ? 0 : 1);
        }
      if (e - b != 3)
        return 2;
      const auto &l = a[b];
      const auto &op = a[b + 1];
      const auto &r = a[b + 2];
      if (op == "=" || op == "==")
        return l == r ? 0 : 1;
      if (op == "!=")
        return l != r ? 0 : 1;
      char *le = nullptr;
      char *re = nullptr;
      long lv = strtol(l.c_str(), &le, 10), rv = strtol(r.c_str(), &re, 10);
      if (!le || *le || !re || *re)
        return 2;
      if (op == "-eq")
        return lv == rv ? 0 : 1;
      if (op == "-ne")
        return lv != rv ? 0 : 1;
      if (op == "-gt")
        return lv > rv ? 0 : 1;
      if (op == "-ge")
        return lv >= rv ? 0 : 1;
      if (op == "-lt")
        return lv < rv ? 0 : 1;
      if (op == "-le")
        return lv <= rv ? 0 : 1;
      return 2;
    };
    return eval(eval, 1, end);
  }
  if (c == "set" || c == "setopt" || c == "unsetopt") {
    if ((c == "setopt" || c == "unsetopt") && a.size() == 1) {
      for (auto name : {"errexit", "nounset", "allexport"})
        std::cout << name << '\t'
                  << ((std::string(name) == "errexit" && errexit) ||
                              (std::string(name) == "nounset" && nounset) ||
                              (std::string(name) == "allexport" && allexport)
                          ? "on"
                          : "off")
                  << '\n';
      return 0;
    }
    if (c == "set" && a.size() == 2 && a[1] == "-o") {
      for (auto name : {"errexit", "nounset", "allexport"})
        std::cout << name << '\t'
                  << ((std::string(name) == "errexit" && errexit) ||
                              (std::string(name) == "nounset" && nounset) ||
                              (std::string(name) == "allexport" && allexport)
                          ? "on"
                          : "off")
                  << '\n';
      return 0;
    }
    for (size_t i = 1; i < a.size(); ++i) {
      auto x = a[i];
      bool on = c != "unsetopt";
      if (x == "-e" || x == "errexit" || x == "ERR_EXIT")
        errexit = on;
      else if (x == "+e")
        errexit = false;
      else if (x == "-u" || x == "nounset" || x == "NO_UNSET")
        nounset = on;
      else if (x == "+u")
        nounset = false;
      else if (x == "-a" || x == "allexport" || x == "ALL_EXPORT")
        allexport = on;
      else if (x == "+a")
        allexport = false;
      else if (c == "set" && x == "--") {
        positionals.assign(a.begin() + i + 1, a.end());
        break;
      } else if (c == "set" && x.find('=') != std::string::npos) {
        if (!setvar(x))
          return 1;
      } else if (c == "set" && i == 1) {
        for (auto &[k, v] : vars)
          std::cout << k << '=' << v << '\n';
      } else if (x != "-o") {
        std::cerr << c << ": unsupported option: " << x << '\n';
        return 2;
      }
    }
    return 0;
  }
  if (c == "hash") {
    if (a.size() == 1) {
      for (auto &[n, p] : command_hash)
        std::cout << n << " = " << p << '\n';
      return 0;
    }
    if (a[1] == "-r") {
      command_hash.clear();
      return 0;
    }
    if (a[1] == "-d") {
      if (a.size() < 3)
        return 2;
      return command_hash.erase(a[2]) ? 0 : 1;
    }
    for (size_t i = 1; i < a.size(); ++i)
      if (!find_command(a[i])) {
        std::cerr << "hash: " << a[i] << ": not found\n";
        return 1;
      }
    return 0;
  }
  if (c == "source" || c == ".") {
    if (a.size() < 2)
      return 2;
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
    if (a.size() > 1 && atoi(a[1].c_str()) != 1) {
      std::cerr << c << ": only a single loop level is supported\n";
      return 2;
    }
    if (loop_depth == 0) {
      std::cerr << c << ": can only be used inside a loop\n";
      return 1;
    }
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
        std::cout << s << ' ' << v << '\n';
      return 0;
    }
    if (a.size() < 3)
      return 2;
    const bool exit_trap = a[2] == "EXIT" || a[2] == "0";
    int sig = exit_trap ? 0 : signal_number(a[2]);
    if (!exit_trap && sig == 0) {
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
std::optional<int> handle_builtin(const std::vector<std::string> &a) {
  return run(a);
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
      "getopts", "trap",     "autoload", "break", "continue"};
  return std::find(names.begin(), names.end(), n) != names.end();
}
void remember_job(int pid, const std::string &cmd) {
  int id = jobs.empty() ? 1 : jobs.back().id + 1;
  jobs.push_back({id, pid, cmd, false});
  std::cout << '[' << id << "] " << pid << '\n';
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
    for (size_t i = 0; i < w.size(); ++i) {
      if (w[i] == '\x01') { literal = !literal; continue; }
      if (w[i] == '\x02') { double_quoted = !double_quoted; continue; }
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
        for (size_t j = 0; j < positionals.size(); ++j) {
          if (j && !double_quoted) append_text(" ", true);
          append_text(positionals[j], !double_quoted);
        }
        ++i; continue;
      }
      size_t b = i + 1, e = b;
      std::string parameter_operator, parameter_word;
      std::string parameter_name;
      bool length_expansion = false;
      if (b < w.size() && w[b] == '{') {
        b++; e = w.find('}', b);
        if (e == std::string::npos) { append_text("$", false); continue; }
        std::string expression = w.substr(b, e - b);
        if (!expression.empty() && expression[0] == '#') {
          length_expansion = true;
          expression.erase(expression.begin());
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
      const bool is_set = it != vars.end() || v != nullptr;
      std::string value = it != vars.end() ? it->second : (v ? v : "");
      if (length_expansion) {
        append_text(std::to_string(value.size()), !double_quoted);
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
bool take_loop_break() { bool result = loop_break; loop_break = false; return result; }
bool take_loop_continue() { bool result = loop_continue; loop_continue = false; return result; }
bool loop_control_pending() { return loop_break || loop_continue; }
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
  else
    return false;
  return true;
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
