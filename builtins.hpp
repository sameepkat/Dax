#ifndef DAX_BUILTINS_HPP
#define DAX_BUILTINS_HPP

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "tokenizer.hpp"

struct Job {
  int id;
  int pid;
  std::string command;
  bool stopped = false;
};

std::optional<int> handle_builtin(const std::vector<std::string> &args);
bool is_builtin(const std::string &name);
void remember_job(int pid, const std::string &command);
void set_last_status(int status);
int last_status();
bool shell_should_exit();
int requested_exit_status();
void clear_exit_request();
void execute_line(const std::string &line);
std::vector<ShellToken> tokenize(const std::string &line);
void expand_words(std::vector<std::string> &words);
void expand_aliases(std::vector<ShellToken> &words);
void define_function(const std::string &name, const std::string &body);
bool function_exists(const std::string &name);
bool shell_should_return();
void enter_shell_loop();
void leave_shell_loop();
bool take_loop_break();
bool take_loop_continue();
bool loop_control_pending();
void run_exit_trap();
void run_pending_signal_trap();
bool set_shell_variable(const std::string &name, const std::string &value,
                        bool export_value = false);
bool shell_variable_readonly(const std::string &name);
void set_shell_arguments(const std::string &shell_name,
                         const std::vector<std::string> &arguments);
bool set_shell_option(const std::string &name, bool enabled);
bool shell_option_enabled(const std::string &name);
void set_shell_modes(bool login, bool interactive, bool monitor_was_explicit);
bool shell_noexec_enabled();
bool shell_verbose_enabled();
bool shell_xtrace_enabled();
bool shell_noglob_enabled();
bool shell_noclobber_enabled();
void trace_shell_command(const std::vector<std::string> &args);
std::vector<std::string> take_completed_job_notifications();
void begin_word_expansion();
bool word_expansion_succeeded();
bool word_expansion_ran_substitution();
int word_expansion_substitution_status();
size_t word_expansion_error_index();
void apply_errexit(int status);
void begin_errexit_ignored_context();
void end_errexit_ignored_context();
void mark_errexit_immune();
void clear_errexit_immune();

#endif
