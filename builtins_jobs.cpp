#include "builtins_internal.hpp"
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

std::optional<int> run_jobs_builtin(const std::vector<std::string> &a,
                                    ShellState &state) {
  if (a.empty()) return 0;
  const auto &c = a[0];
  if (c == "jobs") {
    const int current_job = state.jobs.empty() ? -1 : state.jobs.back().id;
    const int previous_job = state.jobs.size() < 2 ? -1 : state.jobs[state.jobs.size() - 2].id;
    auto marker_for = [&](int id) {
      return id == current_job ? '+' : id == previous_job ? '-' : ' ';
    };
    for (auto it = state.jobs.begin(); it != state.jobs.end();) {
      int st;
      pid_t r = waitpid(it->pid, &st, WNOHANG | WUNTRACED);
      if (r == it->pid && (WIFEXITED(st) || WIFSIGNALED(st))) {
        std::cout << '[' << it->id << ']' << marker_for(it->id) << "  Done "
                  << it->command << '\n';
        it = state.jobs.erase(it);
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
      for (auto it = state.jobs.begin(); it != state.jobs.end();) {
        int status = 0;
        while (waitpid(it->pid, &status, 0) < 0 && errno == EINTR) {}
        it = state.jobs.erase(it);
      }
      return 0;
    }
    if (state.jobs.empty()) {
      const std::string job = a.size() > 1 ? a[1] : "%+";
      std::cerr << c << ": " << job << ": no such job\n";
      return c == "wait" ? 127 : 1;
    }
    int id = state.jobs.back().id;
    if (a.size() > 1) {
      std::string x = a[1];
      if (x[0] == '%')
        x.erase(0, 1);
      id = atoi(x.c_str());
    }
    auto it = std::find_if(state.jobs.begin(), state.jobs.end(),
                           [&](auto &j) { return j.id == id; });
    if (it == state.jobs.end()) {
      const std::string job = a.size() > 1 ? a[1] : "%+";
      std::cerr << c << ": " << job << ": no such job\n";
      return c == "wait" ? 127 : 1;
    }
    if (c == "disown") {
      state.jobs.erase(it);
      return 0;
    }
    if (c == "bg") {
      kill(-it->pid, SIGCONT);
      it->stopped = false;
      return 0;
    }
    if (c == "fg") {
      if (state.monitor && isatty(STDIN_FILENO))
        tcsetpgrp(STDIN_FILENO, it->pid);
      kill(-it->pid, SIGCONT);
    }
    int st = 0;
    waitpid(it->pid, &st, c == "fg" ? WUNTRACED : 0);
    if (c == "fg" && state.monitor && isatty(STDIN_FILENO))
      tcsetpgrp(STDIN_FILENO, getpgrp());
    if (WIFSTOPPED(st))
      it->stopped = true;
    else
      state.jobs.erase(it);
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
      sig = shell_signal_number(a[i++]);
      if (!sig)
        return 2;
    }
    int rc = 0;
    for (; i < a.size(); ++i) {
      pid_t p = atoi(a[i].c_str());
      if (a[i][0] == '%') {
        int id = atoi(a[i].c_str() + 1);
        auto it = std::find_if(state.jobs.begin(), state.jobs.end(),
                               [&](auto &j) { return j.id == id; });
        if (it == state.jobs.end()) {
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
  return {};
}
