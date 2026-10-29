#include "builtins_internal.hpp"
#include "shell_state.hpp"
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

std::optional<int> run_environment_builtin(const std::vector<std::string> &a,
                                           ShellState &state) {
  if (a.empty()) return 0;
  const auto &c = a[0];
  if (c == "export" || c == "typeset" || c == "local" || c == "readonly") {
    size_t i = 1;
    if (i == a.size()) {
      for (auto &[k, v] : state.vars)
        std::cout << (c == "export" ? "export " : "") << k << '=' << v << '\n';
      return 0;
    }
    if (a[i] == "-p" || a[i] == "-x" || a[i] == "-r")
      ++i;
    for (; i < a.size(); ++i) {
      auto p = a[i].find('=');
      std::string key = a[i].substr(0, p);
      if (c == "local" && state.local_scopes.empty()) {
        std::cerr << "local: can only be used in a function\n";
        return 1;
      }
      if ((c == "local" || c == "typeset") && !state.local_scopes.empty() &&
          shell_valid_name(key)) {
        if (!state.local_scopes.back().count(key)) {
          const char *old = getenv(key.c_str());
          auto vi = state.vars.find(key);
          state.local_scopes.back()[key] = {vi != state.vars.end(),
                                      vi != state.vars.end() ? vi->second : "",
                                      old != nullptr, old ? old : ""};
        }
      }
      if (p != std::string::npos) {
        if (!shell_set_assignment(a[i], c == "export"))
          return 1;
        if (c == "readonly")
          state.readonly_vars[key] = 1;
      } else if (c == "readonly")
        state.readonly_vars[key] = 1;
      else if (c == "export") {
        auto vi = state.vars.find(key);
        if (vi != state.vars.end() && setenv(key.c_str(), vi->second.c_str(), 1))
          return 1;
      } else if (c == "local" || c == "typeset") {
        const char *v = getenv(a[i].c_str());
        state.vars[a[i]] = v ? v : "";
      }
    }
    return 0;
  }
  if (c == "unset") {
    int rc = 0;
    for (size_t i = 1; i < a.size(); ++i) {
      if (a[i] == "-f" && i + 1 < a.size()) {
        state.functions.erase(a[++i]);
        continue;
      }
      if (state.readonly_vars.count(a[i])) {
        std::cerr << "unset: " << a[i] << ": readonly variable\n";
        rc = 1;
      } else {
        state.vars.erase(a[i]);
        state.arrays.erase(a[i]);
        unsetenv(a[i].c_str());
      }
    }
    return rc;
  }
  if (c == "alias") {
    if (a.size() == 1) {
      for (auto &[k, v] : state.aliases)
        std::cout << "alias " << k << "='" << v << "'\n";
      return 0;
    }
    for (size_t i = 1; i < a.size(); ++i) {
      auto p = a[i].find('=');
      if (p == std::string::npos) {
        auto it = state.aliases.find(a[i]);
        if (it == state.aliases.end())
          return 1;
        std::cout << "alias " << it->first << "='" << it->second << "'\n";
      } else
        state.aliases[a[i].substr(0, p)] = a[i].substr(p + 1);
    }
    return 0;
  }
  if (c == "unalias") {
    if (a.size() < 2)
      return 2;
    int rc = 0;
    for (size_t i = 1; i < a.size(); ++i)
      if (!state.aliases.erase(a[i]))
        rc = 1;
    return rc;
  }
  return {};
}
