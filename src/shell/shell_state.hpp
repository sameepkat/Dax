#ifndef DAX_SHELL_STATE_HPP
#define DAX_SHELL_STATE_HPP

#include "job.hpp"
#include <chrono>
#include <csignal>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct SavedVar {
  bool had_var = false;
  std::string var;
  bool had_env = false;
  std::string env;
  bool had_indexed_array = false;
  std::vector<std::string> indexed_array;
  bool had_indexed_array_indices = false;
  std::unordered_set<size_t> indexed_array_indices;
  bool had_associative_array = false;
  std::unordered_map<std::string, std::string> associative_array;
  bool had_integer_attribute = false;
  bool had_lowercase_attribute = false;
  bool had_uppercase_attribute = false;
  bool had_readonly_attribute = false;
  bool had_unset_state = false;
  bool had_ignoreeof_state = false;
  bool ignoreeof_state = false;
  bool had_nameref = false;
  std::string nameref_target;
};

struct ShellOptionSnapshot {
  bool errexit, errtrace, functrace, nounset, allexport, pipefail;
  bool notify_jobs, noclobber, nolog, noglob, monitor, keyword_assignments, physical_directory, one_command;
  bool noexec, verbose, xtrace, hashall, braceexpand, histexpand, history, ignoreeof;
  bool privileged;
  bool extglob, nullglob, failglob, dotglob, globstar, globskipdots, globasciiranges;
  bool nocaseglob, nocasematch, sourcepath, expand_aliases;
  bool huponexit, lastpipe, inherit_errexit, shift_verbose, patsub_replacement, varredir_close, xpg_echo, checkwinsize, checkhash, checkjobs, promptvars, cdable_vars, cdspell, autocd, execfail, localvar_inherit, localvar_unset, interactive_comments, progcomp, no_empty_cmd_completion, cmdhist, lithist, histappend, posix_mode;
};

struct ShellState {
  std::unordered_map<std::string, std::string> aliases, vars, readonly_vars, command_hash;
  std::unordered_map<std::string, std::vector<std::string>> completion_specs;
  std::string active_completion_key;
  std::vector<std::string> active_completion_options;
  bool completion_active = false;
  bool last_completion_nospace = false;
  std::unordered_map<std::string, size_t> command_hash_hits;
  std::unordered_map<std::string, std::string> namerefs;
  std::unordered_set<std::string> unset_vars;
  std::unordered_set<std::string> disabled_special_parameters;
  std::unordered_set<std::string> integer_vars;
  std::unordered_set<std::string> lowercase_vars, uppercase_vars;
  std::unordered_set<std::string> disabled_builtins;
  std::unordered_map<std::string, std::vector<std::string>> arrays;
  std::unordered_map<std::string, std::unordered_set<size_t>> array_set_indices;
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>> associative_arrays;
  std::unordered_map<std::string, std::string> functions;
  std::unordered_set<std::string> readonly_functions;
  std::unordered_set<std::string> exported_functions;
  std::unordered_set<std::string> traced_functions;
  std::unordered_map<int, std::string> traps;
  std::unordered_set<int> initially_ignored_signals;
  std::vector<std::string> dirs, positionals;
  std::vector<std::string> function_stack, function_source_stack, source_stack;
  std::vector<std::string> function_call_sources;
  std::vector<bool> function_frame_flags;
  std::vector<size_t> function_call_lines;
  std::unordered_map<std::string, std::string> function_sources;
  size_t current_lineno = 1;
  uint64_t command_number = 0;
  int last_background_pid = 0;
  int shell_pid = 0;
  int trap_signal = 0;
  unsigned subshell_level = 0;
  std::chrono::steady_clock::time_point seconds_epoch =
      std::chrono::steady_clock::now();
  long long seconds_offset = 0;
  uint32_t random_seed = 1;
  uint32_t secure_random_seed = 1073741823U;
  uint32_t last_secure_random = 0;
  std::string bash_command;
  std::string execution_string;
  std::string last_argument;
  std::string shell_name = "dax";
  std::string script_source;
  std::vector<Job> jobs;
  std::unordered_map<int, int> completed_child_statuses;
  std::unordered_set<int> process_substitution_pids;
  int status_value = 0, exit_value = 0;
  bool exiting = false, errexit = false, errtrace = false, functrace = false, nounset = false, allexport = false;
  bool pipefail = false;
  bool notify_jobs = false, noclobber = false, noglob = false, monitor = false;
  bool nolog = false;
  bool keyword_assignments = false;
  bool physical_directory = false;
  bool one_command = false;
  bool noexec = false, verbose = false, xtrace = false;
  bool privileged = false;
  bool ignore_inherited_environment = false;
  bool hashall = true, braceexpand = true;
  bool emacs_mode = true, vi_mode = false;
  bool histexpand = false;
  bool history = true;
  bool posix_mode = false;
  bool ignoreeof = false;
  bool extglob = false, nullglob = false, failglob = false, dotglob = false;
  bool sourcepath = true;
  bool expand_aliases = false;
  bool globstar = false;
  bool globskipdots = true;
  bool globasciiranges = true;
  bool nocaseglob = false, nocasematch = false;
  bool login_shell = false, interactive_shell = false;
  bool command_string_mode = false, stdin_mode = false;
  bool force_shell_source = false;
  bool huponexit = false;
  bool lastpipe = false;
  bool inherit_errexit = false;
  bool shift_verbose = false;
  bool patsub_replacement = true;
  bool varredir_close = false;
  bool xpg_echo = false;
  bool checkwinsize = true;
  bool checkhash = false;
  bool checkjobs = false;
  bool promptvars = true;
  bool exit_warning_pending = false;
  bool cdable_vars = false;
  bool cdspell = false;
  bool autocd = false;
  bool execfail = false;
  bool localvar_inherit = false;
  bool localvar_unset = false;
  bool interactive_comments = true;
  bool progcomp = true;
  bool no_empty_cmd_completion = false;
  bool cmdhist = true;
  bool lithist = false;
  bool histappend = true;
  bool pipeline_stderr_to_stdout = false;
  int errexit_ignored_context = 0;
  int err_trap_ignored_context = 0;
  bool errexit_immune_status = false;
  bool returning = false;
  bool running_err_trap = false;
  bool running_debug_trap = false, running_return_trap = false;
  size_t debug_trap_function_depth = 0, return_trap_function_depth = 0;
  bool exit_trap_active = false;
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
  std::vector<std::optional<ShellOptionSnapshot>> local_option_scopes;
};

ShellState &shell_state();
bool prepare_shell_startup_privilege(bool requested);
ShellOptionSnapshot capture_shell_options(const ShellState &state);
void restore_shell_options(ShellState &state, const ShellOptionSnapshot &snapshot);
void refresh_shell_terminal_dimensions(int descriptor, bool force = false);

#endif
