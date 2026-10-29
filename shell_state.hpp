#ifndef DAX_SHELL_STATE_HPP
#define DAX_SHELL_STATE_HPP

#include "job.hpp"
#include <csignal>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct SavedVar {
  bool had_var = false;
  std::string var;
  bool had_env = false;
  std::string env;
};

struct ShellState {
  std::unordered_map<std::string, std::string> aliases, vars, readonly_vars, command_hash;
  std::unordered_map<std::string, std::vector<std::string>> arrays;
  std::unordered_map<std::string, std::string> functions;
  std::unordered_map<int, std::string> traps;
  std::vector<std::string> dirs, positionals;
  std::string shell_name = "dax";
  std::vector<Job> jobs;
  int status_value = 0, exit_value = 0;
  bool exiting = false, errexit = false, nounset = false, allexport = false;
  bool pipefail = false;
  bool notify_jobs = false, noclobber = false, noglob = false, monitor = false;
  bool noexec = false, verbose = false, xtrace = false;
  bool login_shell = false, interactive_shell = false;
  int errexit_ignored_context = 0;
  bool errexit_immune_status = false;
  bool returning = false;
  bool running_err_trap = false;
  bool err_trap_suppressed = false;
  uint64_t err_trap_generation = 0;
  int loop_depth = 0;
  bool loop_break = false, loop_continue = false;
  unsigned loop_control_levels = 1;
  int substitution_status = 0;
  bool substitution_ran = false;
  volatile sig_atomic_t pending_signal = 0;
  bool expansion_error = false;
  size_t expansion_error_index = 0;
  size_t getopts_index = 1, getopts_char = 1;
  std::vector<std::unordered_map<std::string, SavedVar>> local_scopes;
};

ShellState &shell_state();

#endif
