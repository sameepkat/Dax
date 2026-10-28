#ifndef JANET_RUNTIME_HPP
#define JANET_RUNTIME_HPP

#include <string>
#include <vector>
#include <optional>

bool janet_runtime_start();
int janet_run_file(const std::string &path, const std::vector<std::string> &args);
int janet_source_file(const std::string &path, const std::vector<std::string> &args);
int janet_run_restricted_file(const std::string &path, const std::string &workspace,
                             const std::vector<std::string> &args);
int janet_run_string(const std::string &source, const std::string &name = "<string>");
void janet_runtime_stop();
int janet_run_startup_hook();
const std::string &janet_prompt();
const std::string &janet_colorscheme();
bool janet_git_branch_enabled();
bool janet_command_exists(const std::string &name);
std::vector<std::string> janet_command_names();
std::optional<int> janet_dispatch_command(const std::vector<std::string> &args);

#endif
