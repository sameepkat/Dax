#ifndef DAX_BUILTINS_HPP
#define DAX_BUILTINS_HPP

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

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
std::vector<std::string> tokenize(const std::string &line);
void expand_words(std::vector<std::string> &words);
void expand_aliases(std::vector<std::string> &words);
void define_function(const std::string &name, const std::string &body);
bool function_exists(const std::string &name);
bool shell_should_return();
void run_exit_trap();
void run_pending_signal_trap();
bool set_shell_variable(const std::string &name, const std::string &value,
                        bool export_value = false);
bool shell_variable_readonly(const std::string &name);
void begin_word_expansion();
bool word_expansion_succeeded();
void apply_errexit(int status);

#endif
