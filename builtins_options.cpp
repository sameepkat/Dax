#include "builtins_internal.hpp"
#include <iostream>
#include <string>
#include <vector>

std::optional<int> run_options_builtin(const std::vector<std::string> &a,
                                       ShellState &state) {
  if (a.empty()) return 0;
  const auto &c = a[0];
  if (c == "set" || c == "setopt" || c == "unsetopt") {
    static const std::vector<std::string> shell_options = {
        "allexport", "notify", "noclobber", "errexit", "pipefail", "noglob",
        "monitor", "noexec", "nounset", "verbose", "xtrace"};
    if ((c == "setopt" || c == "unsetopt") && a.size() == 1) {
      for (const auto &name : shell_options)
        std::cout << name << '\t'
                  << (shell_option_enabled(name) ? "on" : "off")
                  << '\n';
      return 0;
    }
    if (c == "set" && (a.size() == 2 && (a[1] == "-o" || a[1] == "+o"))) {
      for (const auto &name : shell_options)
        std::cout << name << '\t'
                  << (shell_option_enabled(name) ? "on" : "off")
                  << '\n';
      return 0;
    }
    for (size_t i = 1; i < a.size(); ++i) {
      auto x = a[i];
      bool on = c != "unsetopt";
      if (c == "set" && x == "--") {
        state.positionals.assign(a.begin() + i + 1, a.end());
        break;
      } else if (c == "set" && x.find('=') != std::string::npos) {
        if (!shell_set_assignment(x))
          return 1;
      } else if (x == "-o" || x == "+o") {
        if (i + 1 >= a.size()) {
          for (const auto &name : shell_options)
            std::cout << name << '\t'
                      << (shell_option_enabled(name) ? "on" : "off") << '\n';
          break;
        }
        if (!set_shell_option(a[++i], x == "-o")) {
          std::cerr << c << ": unsupported option: " << a[i] << '\n';
          return 2;
        }
      } else if ((x.size() > 1 && (x[0] == '-' || x[0] == '+')) && c == "set") {
        on = x[0] == '-';
        for (size_t j = 1; j < x.size(); ++j) {
          const char flag = x[j];
          const std::string name = flag == 'a' ? "allexport"
                                   : flag == 'b' ? "notify"
                                   : flag == 'C' ? "noclobber"
                                   : flag == 'e' ? "errexit"
                                   : flag == 'o' ? "pipefail"
                                   : flag == 'f' ? "noglob"
                                   : flag == 'm' ? "monitor"
                                   : flag == 'n' ? "noexec"
                                   : flag == 'u' ? "nounset"
                                   : flag == 'v' ? "verbose"
                                   : flag == 'x' ? "xtrace" : "";
          if (name.empty()) {
            std::cerr << c << ": unsupported option: " << flag << '\n';
            return 2;
          }
          set_shell_option(name, on);
        }
      } else if (!set_shell_option(x, on)) {
        std::cerr << c << ": unsupported option: " << x << '\n';
        return 2;
      }
    }
    return 0;
  }
  return {};
}
