#ifndef DAX_BUILTINS_HPP
#define DAX_BUILTINS_HPP

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "job.hpp"
#include "tokenizer.hpp"

std::optional<int> handle_builtin(const std::vector<std::string> &args);
bool is_builtin(const std::string &name);
const std::vector<std::string> &shell_builtin_names();
bool shell_command_resolves(const std::string &name);
std::vector<std::string> shell_completion_candidates(const std::string &prefix,
                                                     bool command_position,
                                                     bool variable_name,
                                                     const std::string &directory_prefix = {},
                                                     bool empty_line_completion = false);
void remember_job(int pid, const std::string &command);
void set_last_status(int status);
int last_status();
bool shell_should_exit();
int requested_exit_status();
void clear_exit_request();
void shell_finish_one_command();
std::vector<ShellToken> tokenize(const std::string &line);
enum class WordExpansionMode {
  Normal,
  SingleWord,
  Pattern,
  Redirection,
  InteractiveRedirection,
  ParameterReplacement
};
void expand_words(std::vector<std::string> &words,
                  WordExpansionMode mode = WordExpansionMode::Normal);
size_t process_substitution_mark();
void finish_process_substitutions(size_t mark);
void reap_completed_process_substitutions();
void expand_aliases(std::vector<ShellToken> &words);
bool define_function(const std::string &name, const std::string &body);
void import_exported_functions();
void export_functions_to_environment();
bool function_exists(const std::string &name);
bool shell_is_posix_special_builtin(const std::string &name);
bool shell_function_precedes_builtin(const std::string &name);
int shell_execute_function(const std::vector<std::string> &args);
bool shell_should_return();
void enter_shell_loop();
void leave_shell_loop();
int take_loop_control();
bool loop_control_pending();
void run_exit_trap();
void enter_subshell_environment();
void run_pending_signal_trap();
void run_err_trap(int status);
void run_debug_trap(const std::string &command = {});
void run_return_trap();
bool set_shell_variable(const std::string &name, const std::string &value,
                        bool export_value = false);
std::string shell_resolve_nameref(const std::string &name);
bool shell_set_nameref(const std::string &name, const std::string &target);
bool shell_array_index_is_set(const std::string &name, size_t index);
size_t shell_array_index_extent(const std::string &name);
void shell_array_mark_index_set(const std::string &name, size_t index);
void shell_array_mark_all_set(const std::string &name);
void shell_array_clear_set_indices(const std::string &name);
bool shell_array_unset_index(const std::string &name, size_t index);
bool shell_variable_readonly(const std::string &name);
std::optional<long long> shell_arithmetic_special_value(const std::string &name);
std::optional<std::string> shell_arithmetic_special_string(const std::string &name);
void set_shell_arguments(const std::string &shell_name,
                         const std::vector<std::string> &arguments);
bool set_shell_option(const std::string &name, bool enabled);
bool shell_option_enabled(const std::string &name);
std::string shell_enabled_options();
std::string shell_enabled_shopt_options();
void refresh_option_parameter_values();
std::optional<std::string> shell_find_command(const std::string &name,
                                              bool command_execution = false,
                                              bool use_hash_cache = true);
std::optional<std::string> shell_path_value();
bool shell_command_path_is_ignored(const std::string &path);
std::optional<std::vector<std::string>> shell_programmable_completions(
    const std::string &command, const std::string &word);
bool shell_last_completion_nospace();
void set_shell_modes(bool login, bool interactive, bool monitor_was_explicit,
                     bool histexpand_was_explicit = false,
                     bool command_string = false, bool stdin_input = false,
                     const std::string &execution_string = {},
                     bool history_was_explicit = false);
bool shell_histexpand_enabled();
bool shell_noexec_enabled();
bool shell_verbose_enabled();
bool shell_xtrace_enabled();
bool shell_noglob_enabled();
bool shell_noclobber_enabled();
void trace_shell_command(const std::vector<std::string> &args);
std::optional<std::string> shell_expand_prompt_variable(const std::string &name);
std::vector<std::string> take_completed_job_notifications();
void begin_word_expansion();
bool word_expansion_succeeded();
bool word_expansion_error_was_reported();
bool word_expansion_ran_substitution();
int word_expansion_substitution_status();
size_t word_expansion_error_index();
void apply_errexit(int status);
void begin_errexit_ignored_context();
void end_errexit_ignored_context();
void begin_errexit_only_ignored_context();
void end_errexit_only_ignored_context();
void mark_errexit_immune();
void clear_errexit_immune();

#endif
