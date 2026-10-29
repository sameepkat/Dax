#include "builtins_internal.hpp"
#include <iostream>
#include <string>
#include <vector>

std::optional<int> run_lookup_builtin(const std::vector<std::string> &a,
                                      ShellState &state) {
  if (a.empty()) return 0;
  const auto &c = a[0];
  if (c == "command" || c == "builtin" || c == "type" || c == "whence" ||
      c == "which") {
    if (a.size() < 2)
      return 2;
    size_t i = (a[1] == "-v" || a[1] == "-V" || a[1] == "-p") ? 2 : 1;
    if (c == "command" && i == 1)
      return shell_external_command(std::vector<std::string>(a.begin() + 1, a.end()));
    if (c == "builtin" && i == 1) {
      std::vector<std::string> nested(a.begin() + 1, a.end());
      auto result = handle_builtin(nested);
      return result ? *result : 1;
    }
    if (c == "command" && a[1] == "-v") {
      int rc = 0;
      for (size_t n = 2; n < a.size(); ++n) {
        if (is_builtin(a[n]) || state.functions.count(a[n]))
          std::cout << a[n] << '\n';
        else if (auto path = shell_find_command(a[n]))
          std::cout << *path << '\n';
        else
          rc = 1;
      }
      return rc;
    }
    int rc = 0;
    for (; i < a.size(); ++i) {
      if (is_builtin(a[i]))
        std::cout << a[i] << " is a shell builtin\n";
      else if (state.functions.count(a[i]))
        std::cout << a[i] << " is a shell function\n";
      else if (state.aliases.count(a[i]))
        std::cout << a[i] << " is an alias\n";
      else if (auto p = shell_find_command(a[i]))
        std::cout << *p << '\n';
      else {
        std::cerr << a[i] << ": not found\n";
        rc = 1;
      }
    }
    return rc;
  }
  if (c == "hash") {
    if (a.size() == 1) {
      for (auto &[n, p] : state.command_hash)
        std::cout << n << " = " << p << '\n';
      return 0;
    }
    if (a[1] == "-r") {
      state.command_hash.clear();
      return 0;
    }
    if (a[1] == "-d") {
      if (a.size() < 3)
        return 2;
      return state.command_hash.erase(a[2]) ? 0 : 1;
    }
    for (size_t i = 1; i < a.size(); ++i)
      if (!shell_find_command(a[i])) {
        std::cerr << "hash: " << a[i] << ": not found\n";
        return 1;
      }
    return 0;
  }
  return {};
}
