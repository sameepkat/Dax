#include "builtins_internal.hpp"
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
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
    bool long_format = false, pid_only = false, running_only = false, stopped_only = false;
    bool changed_only = false;
    bool execute_after_substitution = false;
    size_t first_spec = 1;
    for (; first_spec < a.size() && a[first_spec].size() > 1 && a[first_spec][0] == '-'; ++first_spec) {
      if (a[first_spec] == "--") { ++first_spec; break; }
      for (size_t j = 1; j < a[first_spec].size(); ++j) {
        switch (a[first_spec][j]) {
        case 'l': long_format = true; break;
        case 'p': pid_only = true; break;
        case 'r': running_only = true; break;
        case 's': stopped_only = true; break;
        case 'n': changed_only = true; break;
        case 'x': execute_after_substitution = true; break;
        default:
          std::cerr << "jobs: invalid option -- '" << a[first_spec][j] << "'\n";
          return 2;
        }
      }
    }
    std::vector<int> selected_ids;
    int status = 0;
    bool ambiguous_job_spec = false;
    auto resolve_job = [&](const std::string &job_spec) {
      std::string spec = job_spec;
      if (!spec.empty() && spec[0] == '%') spec.erase(0, 1);
      if (spec.empty() || spec == "+" || spec == "%") {
        if (!state.jobs.empty()) return std::prev(state.jobs.end());
      } else if (spec == "-") {
        if (state.jobs.size() > 1) return std::prev(state.jobs.end(), 2);
      } else {
        char *end = nullptr;
        const long id = std::strtol(spec.c_str(), &end, 10);
        if (end != spec.c_str() && *end == '\0')
          return std::find_if(state.jobs.begin(), state.jobs.end(),
                              [&](const Job &job) { return job.id == id; });
      }
      if (!spec.empty()) {
        const bool substring_match = spec.front() == '?';
        const std::string needle = substring_match ? spec.substr(1) : spec;
        if (!needle.empty()) {
          auto match = state.jobs.end();
          for (auto candidate = state.jobs.begin(); candidate != state.jobs.end(); ++candidate) {
            const bool matches = substring_match
                ? candidate->command.find(needle) != std::string::npos
                : candidate->command.rfind(needle, 0) == 0;
            if (!matches) continue;
            if (match != state.jobs.end()) {
              ambiguous_job_spec = true;
              return state.jobs.end();
            }
            match = candidate;
          }
          if (match != state.jobs.end()) return match;
        }
      }
      return state.jobs.end();
    };
    if (execute_after_substitution) {
      if (first_spec >= a.size()) {
        std::cerr << "jobs: -x: command required\n";
        return 2;
      }
      std::vector<std::string> command(a.begin() + first_spec, a.end());
      for (size_t i = 0; i < command.size(); ++i) {
        if (command[i].empty() || command[i].front() != '%') continue;
        ambiguous_job_spec = false;
        const auto job = resolve_job(command[i]);
        if (job == state.jobs.end()) {
          std::cerr << "jobs: " << command[i]
                    << (ambiguous_job_spec ? ": ambiguous job spec\n" : ": no such job\n");
          return 1;
        }
        command[i] = std::to_string(job->pid);
      }
      if (auto builtin = handle_builtin(command)) return *builtin;
      return shell_external_command(command, true);
    }
    if (first_spec == a.size()) {
      for (const auto &job : state.jobs) selected_ids.push_back(job.id);
    } else {
      for (size_t i = first_spec; i < a.size(); ++i) {
        const auto found = resolve_job(a[i]);
        if (found == state.jobs.end()) {
          std::cerr << "jobs: " << a[i]
                    << (ambiguous_job_spec ? ": ambiguous job spec\n" : ": no such job\n");
          status = 1;
        } else if (std::find(selected_ids.begin(), selected_ids.end(), found->id) == selected_ids.end()) {
          selected_ids.push_back(found->id);
        }
      }
    }
    const int current_job = state.jobs.empty() ? -1 : state.jobs.back().id;
    const int previous_job = state.jobs.size() < 2 ? -1 : state.jobs[state.jobs.size() - 2].id;
    auto marker_for = [&](int id) {
      return id == current_job ? '+' : id == previous_job ? '-' : ' ';
    };
    for (auto it = state.jobs.begin(); it != state.jobs.end();) {
      if (std::find(selected_ids.begin(), selected_ids.end(), it->id) == selected_ids.end()) {
        ++it;
        continue;
      }
      int st = 0;
      int wait_options = WNOHANG | WUNTRACED;
#ifdef WCONTINUED
      wait_options |= WCONTINUED;
#endif
      const pid_t r = it->completed ? 0 : waitpid(it->pid, &st, wait_options);
      if (r == it->pid) {
        it->status = st;
        if (WIFEXITED(st) || WIFSIGNALED(st)) {
          it->completed = true;
          it->stopped = false;
          it->status_change_pending = true;
        } else if (WIFSTOPPED(st)) {
          it->stopped = true;
          it->status_change_pending = true;
#ifdef WIFCONTINUED
        } else if (WIFCONTINUED(st)) {
          it->stopped = false;
          it->status_change_pending = true;
#endif
        }
      }
      const bool state_matches =
          (!running_only || (!it->stopped && !it->completed)) &&
          (!stopped_only || it->stopped);
      const bool should_print = state_matches &&
                                (!changed_only || it->status_change_pending);
      if (!should_print) {
        ++it;
        continue;
      }
      if (it->completed) {
        if (pid_only) std::cout << it->pid << '\n';
        else {
          std::cout << '[' << it->id << ']' << marker_for(it->id) << "  Done ";
          if (long_format) std::cout << it->pid << ' ';
          std::cout << it->command << '\n';
        }
        it = state.jobs.erase(it);
        continue;
      }
      if (pid_only) std::cout << it->pid << '\n';
      else {
        std::cout << '[' << it->id << ']' << marker_for(it->id) << "  ";
        if (long_format) std::cout << it->pid << ' ';
        std::cout << (it->stopped ? "Stopped " : "Running ") << it->command;
        if (!it->stopped) std::cout << " &";
        std::cout << '\n';
      }
      it->status_change_pending = false;
      ++it;
    }
    return status;
  }
  if (c == "disown") {
    bool all = false, running_only = false, mark_hup = false;
    size_t i = 1;
    for (; i < a.size() && a[i].size() > 1 && a[i][0] == '-'; ++i) {
      if (a[i] == "--") { ++i; break; }
      for (size_t j = 1; j < a[i].size(); ++j) {
        if (a[i][j] == 'a') all = true;
        else if (a[i][j] == 'r') running_only = true;
        else if (a[i][j] == 'h') mark_hup = true;
        else {
          std::cerr << "disown: invalid option -- '" << a[i][j] << "'\n";
          return 2;
        }
      }
    }
    std::vector<int> selected;
    int status = 0;
    const bool no_specs = i == a.size();
    auto add_job = [&](auto it) {
      if (it != state.jobs.end() &&
          (!running_only || (!it->stopped && !it->completed)))
        selected.push_back(it->id);
    };
    if (all) {
      for (const auto &job : state.jobs)
        if (!running_only || (!job.stopped && !job.completed)) selected.push_back(job.id);
    } else if (i == a.size()) {
      if (!state.jobs.empty()) add_job(std::prev(state.jobs.end()));
      else status = 1;
    } else {
      for (; i < a.size(); ++i) {
        std::string spec = a[i];
        if (spec.empty() || spec[0] != '%') {
          std::cerr << "disown: " << spec << ": not a job spec\n";
          status = 1;
          continue;
        }
        spec.erase(0, 1);
        auto found = state.jobs.end();
        if (spec.empty() || spec == "+" || spec == "%") {
          if (!state.jobs.empty()) found = std::prev(state.jobs.end());
        } else if (spec == "-") {
          if (state.jobs.size() > 1) found = std::prev(state.jobs.end(), 2);
        } else {
          char *end = nullptr;
          const long id = std::strtol(spec.c_str(), &end, 10);
          if (end != spec.c_str() && *end == '\0')
            found = std::find_if(state.jobs.begin(), state.jobs.end(),
                [&](const Job &job) { return job.id == id; });
        }
        if (found == state.jobs.end() || (running_only && (found->stopped || found->completed))) {
          std::cerr << "disown: " << a[i] << ": no such job\n";
          status = 1;
        } else add_job(found);
      }
    }
    if (selected.empty() && !all) {
      if (no_specs) std::cerr << "disown: current: no such job\n";
      return status ? status : 1;
    }
    if (mark_hup) {
      for (auto &job : state.jobs)
        if (std::find(selected.begin(), selected.end(), job.id) != selected.end()) job.ignore_hup = true;
    } else {
      state.jobs.erase(std::remove_if(state.jobs.begin(), state.jobs.end(), [&](const Job &job) {
        return std::find(selected.begin(), selected.end(), job.id) != selected.end();
      }), state.jobs.end());
    }
    return status;
  }
  if (c == "fg" || c == "bg" || c == "wait") {
    if (c == "wait") {
      reap_completed_process_substitutions();
      // Bash's wait -n waits for whichever eligible child finishes first.
      bool wait_next = false;
      bool wait_force = false;
      bool has_explicit_specs = false;
      bool result_var_requested = false;
      std::string result_var;
      std::vector<std::string> specs;
      std::vector<pid_t> eligible;
      for (size_t i = 1; i < a.size(); ++i) {
        if (a[i] == "-n") {
          wait_next = true;
        } else if (a[i] == "-f") {
          wait_force = true;
        } else if (a[i] == "-p") {
          if (i + 1 >= a.size()) {
            std::cerr << "wait: -p: option requires an argument\n";
            return 2;
          }
          result_var_requested = true;
          result_var = a[++i];
        } else if (a[i].rfind("-p", 0) == 0 && a[i].size() > 2) {
          result_var_requested = true;
          result_var = a[i].substr(2);
        } else if (a[i] == "--") {
          for (++i; i < a.size(); ++i) specs.push_back(a[i]);
          has_explicit_specs = !specs.empty();
          break;
        } else {
          if (a[i].size() > 1 && a[i][0] == '-') {
            std::cerr << "wait: invalid option -- '" << a[i][1] << "'\n";
            return 2;
          }
          has_explicit_specs = true;
          specs.push_back(a[i]);
        }
      }
      if (result_var_requested) {
        if (!shell_valid_name(result_var)) {
          std::cerr << "wait: " << result_var << ": not a valid identifier\n";
          return 2;
        }
        if (state.readonly_vars.count(result_var)) {
          std::cerr << "wait: " << result_var << ": readonly variable\n";
          return 2;
        }
        if (run_environment_builtin({"unset", result_var}, state).value_or(1) != 0)
          return 2;
      }
      if (wait_next) {
        for (const auto &spec : specs) {
          if (!spec.empty() && spec.front() == '%') {
            const std::string id_text = spec.substr(1);
            auto job = state.jobs.end();
            if (id_text.empty() || id_text == "+")
              job = state.jobs.empty() ? state.jobs.end() : std::prev(state.jobs.end());
            else if (id_text == "-")
              job = state.jobs.size() < 2 ? state.jobs.end()
                                          : std::prev(state.jobs.end(), 2);
            else {
              char *end = nullptr;
              const long id = std::strtol(id_text.c_str(), &end, 10);
              if (end != id_text.c_str() && *end == '\0')
                job = std::find_if(state.jobs.begin(), state.jobs.end(),
                                   [&](const Job &entry) { return entry.id == id; });
            }
            if (job != state.jobs.end()) eligible.push_back(job->pid);
          } else {
            char *end = nullptr;
            const long value = std::strtol(spec.c_str(), &end, 10);
            if (end != spec.c_str() && *end == '\0' && value > 0)
              eligible.push_back(static_cast<pid_t>(value));
          }
        }
        if (eligible.empty() && has_explicit_specs) return 127;
        if (eligible.empty())
          for (const auto &job : state.jobs) eligible.push_back(job.pid);
        if (!has_explicit_specs) {
          for (const auto &[pid, child_status] : state.completed_child_statuses) {
            (void)child_status;
            if (std::find(eligible.begin(), eligible.end(), pid) == eligible.end())
              eligible.push_back(pid);
          }
          for (const int pid : state.process_substitution_pids)
            if (std::find(eligible.begin(), eligible.end(), pid) == eligible.end())
              eligible.push_back(pid);
        }
        if (eligible.empty()) return 127;

        int status = 0;
        pid_t completed = -1;
        for (;;) {
          const auto completed_child = std::find_if(
              eligible.begin(), eligible.end(), [&](pid_t candidate) {
                return state.completed_child_statuses.count(candidate) != 0;
              });
          if (completed_child != eligible.end()) {
            completed = *completed_child;
            status = state.completed_child_statuses.at(completed);
            break;
          }
          auto ready = std::find_if(state.jobs.begin(), state.jobs.end(),
                                    [&](const Job &entry) {
                                      return (entry.completed ||
                                              (state.monitor && !wait_force &&
                                               entry.status_change_pending)) &&
                                          std::find(eligible.begin(), eligible.end(), entry.pid) != eligible.end();
                                    });
          if (ready != state.jobs.end()) {
            completed = ready->pid;
            status = ready->status;
            ready->status_change_pending = false;
            break;
          }
          bool any_child = false;
          for (const pid_t candidate : eligible) {
            auto child = std::find_if(state.jobs.begin(), state.jobs.end(),
                                      [&](const Job &entry) { return entry.pid == candidate; });
            if (child == state.jobs.end() || child->completed) continue;
            any_child = true;
            pid_t result;
            do {
              result = waitpid(candidate, &status,
                               WNOHANG | (state.monitor && !wait_force ? WUNTRACED : 0));
            } while (result < 0 && errno == EINTR);
            if (result == candidate) {
              child->stopped = WIFSTOPPED(status);
              child->completed = !child->stopped;
              child->status = status;
              child->status_change_pending = false;
              completed = candidate;
              break;
            }
            if (result < 0 && errno == ECHILD) {
              child->completed = true;
              child->status = 127 << 8;
            }
          }
          if (completed < 0) {
            for (const pid_t candidate : eligible) {
              if (!state.process_substitution_pids.count(static_cast<int>(candidate)))
                continue;
              any_child = true;
              int child_status = 0;
              pid_t result;
              do {
                result = waitpid(candidate, &child_status, WNOHANG);
              } while (result < 0 && errno == EINTR);
              if (result == candidate) {
                state.process_substitution_pids.erase(static_cast<int>(candidate));
                state.completed_child_statuses[static_cast<int>(candidate)] = child_status;
                completed = candidate;
                status = child_status;
                break;
              }
              if (result < 0 && errno == ECHILD) {
                state.process_substitution_pids.erase(static_cast<int>(candidate));
                state.completed_child_statuses[static_cast<int>(candidate)] = 127 << 8;
              }
            }
          }
          if (completed > 0) break;
          if (!any_child) return 127;
          usleep(1000);
        }
        auto job = std::find_if(state.jobs.begin(), state.jobs.end(),
                                [&](const Job &entry) { return entry.pid == completed; });
        if (result_var_requested &&
            !set_shell_variable(result_var, std::to_string(completed)))
          return 1;
        const int return_status = WIFEXITED(status) ? WEXITSTATUS(status)
                                 : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                 : WIFSTOPPED(status) ? 128 + WSTOPSIG(status) : 127;
        if (state.completed_child_statuses.erase(completed) == 0 &&
            !WIFSTOPPED(status))
          state.jobs.erase(job);
        return return_status;
      }
      if (specs.empty()) {
        if (!state.jobs.empty()) {
          for (auto it = state.jobs.begin(); it != state.jobs.end();) {
            int status = 0;
            if (it->completed) status = it->status;
            else {
              pid_t result;
              do {
                result = waitpid(it->pid, &status, 0);
              } while (result < 0 && errno == EINTR);
              if (result < 0) status = 127 << 8;
            }
            it = state.jobs.erase(it);
          }
        }
        const int last_process_substitution = state.last_background_pid;
        if (state.process_substitution_pids.count(last_process_substitution)) {
          int status = 0;
          pid_t result;
          do {
            result = waitpid(last_process_substitution, &status, 0);
          } while (result < 0 && errno == EINTR);
          state.process_substitution_pids.erase(last_process_substitution);
        }
        state.completed_child_statuses.clear();
        return 0;
      }
      if (has_explicit_specs) {
        int final_status = 0;
        for (const auto &spec : specs) {
          auto job = state.jobs.end();
          if (!spec.empty() && spec.front() == '%') {
            std::string id_text = spec.substr(1);
            if (id_text.empty() || id_text == "+")
              job = state.jobs.empty() ? state.jobs.end() : std::prev(state.jobs.end());
            else if (id_text == "-")
              job = state.jobs.size() < 2 ? state.jobs.end()
                                          : std::prev(state.jobs.end(), 2);
            else {
              char *end = nullptr;
              const long id = std::strtol(id_text.c_str(), &end, 10);
              if (end != id_text.c_str() && *end == '\0')
                job = std::find_if(state.jobs.begin(), state.jobs.end(),
                                   [&](const Job &entry) { return entry.id == id; });
            }
          } else {
            char *end = nullptr;
            const long pid = std::strtol(spec.c_str(), &end, 10);
            if (end != spec.c_str() && *end == '\0' && pid > 0)
              job = std::find_if(state.jobs.begin(), state.jobs.end(),
                                 [&](const Job &entry) { return entry.pid == pid; });
          }
          if (job == state.jobs.end()) {
            if (spec.empty() || spec.front() != '%') {
              char *end = nullptr;
              const long pid = std::strtol(spec.c_str(), &end, 10);
              const auto completed = end != spec.c_str() && *end == '\0' && pid > 0
                  ? state.completed_child_statuses.find(static_cast<int>(pid))
                  : state.completed_child_statuses.end();
              if (completed != state.completed_child_statuses.end()) {
                const int status = completed->second;
                if (result_var_requested &&
                    !set_shell_variable(result_var, std::to_string(pid)))
                  return 1;
                final_status = WIFEXITED(status) ? WEXITSTATUS(status)
                              : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 127;
                state.completed_child_statuses.erase(completed);
                continue;
              }
              const auto process_substitution = end != spec.c_str() && *end == '\0' && pid > 0
                  ? state.process_substitution_pids.find(static_cast<int>(pid))
                  : state.process_substitution_pids.end();
              if (process_substitution != state.process_substitution_pids.end()) {
                int status = 0;
                pid_t result;
                do {
                  result = waitpid(static_cast<pid_t>(pid), &status, 0);
                } while (result < 0 && errno == EINTR);
                state.process_substitution_pids.erase(process_substitution);
                if (result < 0) status = 127 << 8;
                if (result_var_requested &&
                    !set_shell_variable(result_var, std::to_string(pid)))
                  return 1;
                final_status = WIFEXITED(status) ? WEXITSTATUS(status)
                              : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 127;
                continue;
              }
            }
            std::cerr << "wait: " << spec << ": no such job\n";
            final_status = 127;
            continue;
          }
          const pid_t completed = job->pid;
          int status = job->status;
          if (!job->completed && state.monitor && !wait_force && job->stopped &&
              job->status_change_pending) {
            job->status_change_pending = false;
          } else if (!job->completed) {
            pid_t result;
            do {
              result = waitpid(job->pid, &status,
                               state.monitor && !wait_force ? WUNTRACED : 0);
            } while (result < 0 && errno == EINTR);
            if (result < 0) {
              std::cerr << "wait: " << spec << ": no such child\n";
              final_status = 127;
              state.jobs.erase(job);
              continue;
            }
          }
          final_status = WIFEXITED(status) ? WEXITSTATUS(status)
                        : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                        : WIFSTOPPED(status) ? 128 + WSTOPSIG(status) : 127;
          if (result_var_requested &&
              !set_shell_variable(result_var, std::to_string(completed)))
            return 1;
          if (state.monitor && !wait_force && WIFSTOPPED(status)) {
            job->stopped = true;
            job->status = status;
            job->status_change_pending = false;
          } else state.jobs.erase(job);
        }
        return final_status;
      }
    }
    if (c == "bg") {
      if (!state.monitor) {
        std::cerr << "bg: job control not enabled\n";
        return 1;
      }
      int status = 0;
      std::vector<std::string> specs;
      if (a.size() == 1) specs.push_back("%+");
      else specs.assign(a.begin() + 1, a.end());
      auto resolve_background_job = [&](const std::string &input) {
        std::string spec = input;
        if (!spec.empty() && spec.front() == '%') spec.erase(spec.begin());
        if (spec.empty() || spec == "+" || spec == "%")
          return state.jobs.empty() ? state.jobs.end() : std::prev(state.jobs.end());
        if (spec == "-")
          return state.jobs.size() < 2 ? state.jobs.end()
                                       : std::prev(state.jobs.end(), 2);
        char *end = nullptr;
        const long id = std::strtol(spec.c_str(), &end, 10);
        if (end != spec.c_str() && *end == '\0')
          return std::find_if(state.jobs.begin(), state.jobs.end(),
                              [&](const Job &job) { return job.id == id; });
        const bool substring = !spec.empty() && spec.front() == '?';
        const std::string needle = substring ? spec.substr(1) : spec;
        if (needle.empty()) return state.jobs.end();
        auto match = state.jobs.end();
        for (auto candidate = state.jobs.begin(); candidate != state.jobs.end(); ++candidate) {
          const bool matches = substring
              ? candidate->command.find(needle) != std::string::npos
              : candidate->command.rfind(needle, 0) == 0;
          if (!matches) continue;
          if (match != state.jobs.end()) return state.jobs.end();
          match = candidate;
        }
        return match;
      };
      for (const auto &spec : specs) {
        auto job = resolve_background_job(spec);
        if (job == state.jobs.end() || job->completed) {
          std::cerr << "bg: " << spec << ": no such job\n";
          status = 1;
          continue;
        }
        if (kill(-job->pid, SIGCONT) != 0) {
          std::cerr << "bg: " << spec << ": " << std::strerror(errno) << '\n';
          status = 1;
          continue;
        }
        job->stopped = false;
        job->status_change_pending = false;
        const auto marker = job == std::prev(state.jobs.end()) ? '+'
            : state.jobs.size() > 1 && job == std::prev(state.jobs.end(), 2) ? '-' : ' ';
        std::cout << '[' << job->id << ']' << marker << " " << job->command << " &\n";
      }
      return status;
    }
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
    auto it = state.jobs.end();
    if (c == "wait" && a.size() > 1 && (a[1].empty() || a[1][0] != '%')) {
      const pid_t pid = static_cast<pid_t>(std::strtol(a[1].c_str(), nullptr, 10));
      it = std::find_if(state.jobs.begin(), state.jobs.end(),
                        [&](const Job &job) { return job.pid == pid; });
    } else {
      int id = state.jobs.back().id;
      if (a.size() > 1) {
        std::string x = a[1];
        if (!x.empty() && x[0] == '%') x.erase(0, 1);
        id = atoi(x.c_str());
      }
      it = std::find_if(state.jobs.begin(), state.jobs.end(),
                        [&](const Job &job) { return job.id == id; });
    }
    if (it == state.jobs.end()) {
      const std::string job = a.size() > 1 ? a[1] : "%+";
      std::cerr << c << ": " << job << ": no such job\n";
      return c == "wait" ? 127 : 1;
    }
    if (c == "fg") {
      if (!state.monitor) {
        std::cerr << "fg: job control not enabled\n";
        return 1;
      }
      if (a.size() > 2) {
        std::cerr << "fg: too many arguments\n";
        return 2;
      }
      if (state.monitor && isatty(STDIN_FILENO))
        tcsetpgrp(STDIN_FILENO, it->pid);
      kill(-it->pid, SIGCONT);
      it->stopped = false;
      it->status_change_pending = false;
    }
    int st = 0;
    if (c == "wait" && it->completed) {
      st = it->status;
      state.jobs.erase(it);
      return WIFEXITED(st) ? WEXITSTATUS(st)
           : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 127;
    }
    waitpid(it->pid, &st, c == "fg" ? WUNTRACED : 0);
    if (c == "fg" && state.monitor && isatty(STDIN_FILENO))
      tcsetpgrp(STDIN_FILENO, getpgrp());
    if (WIFSTOPPED(st)) {
      it->stopped = true;
      it->status = st;
      it->status_change_pending = false;
    } else
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
