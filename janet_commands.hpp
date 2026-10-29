#ifndef DAX_JANET_COMMANDS_HPP
#define DAX_JANET_COMMANDS_HPP

#include <optional>
#include <string>
#include <vector>

bool janet_command_exists(const std::string &name);
std::vector<std::string> janet_command_names();
std::optional<int> janet_dispatch_command(const std::vector<std::string> &args);

#endif
