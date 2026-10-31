#ifndef DAX_BUILTINS_INTERNAL_HPP
#define DAX_BUILTINS_INTERNAL_HPP

#include "builtins.hpp"
#include "shell_state.hpp"

std::optional<int> run_output_builtin(const std::vector<std::string> &args);
std::optional<int> run_system_builtin(const std::vector<std::string> &args);
std::optional<int> run_environment_builtin(const std::vector<std::string> &args,
                                           ShellState &state);
bool shell_set_assignment(const std::string &value, bool export_it = false);
bool shell_valid_name(const std::string &name);
class GlobalDeclarationView {
public:
  GlobalDeclarationView(ShellState &state, const std::string &name, bool enabled);
  ~GlobalDeclarationView();
  GlobalDeclarationView(const GlobalDeclarationView &) = delete;
  GlobalDeclarationView &operator=(const GlobalDeclarationView &) = delete;

private:
  ShellState &state_;
  std::string name_;
  SavedVar local_;
  SavedVar *global_ = nullptr;
  bool active_ = false;
};
int shell_signal_number(const std::string &name);
std::optional<std::string> shell_find_command(const std::string &name,
                                              bool command_execution,
                                              bool use_hash_cache);
int shell_external_command(const std::vector<std::string> &args,
                           bool use_hash_cache);
std::optional<int> run_navigation_builtin(const std::vector<std::string> &args,
                                          ShellState &state);
std::optional<int> run_history_builtin(const std::vector<std::string> &args);
std::optional<int> run_options_builtin(const std::vector<std::string> &args,
                                       ShellState &state);
std::optional<int> run_jobs_builtin(const std::vector<std::string> &args,
                                    ShellState &state);
std::optional<int> run_lookup_builtin(const std::vector<std::string> &args,
                                      ShellState &state);

#endif
