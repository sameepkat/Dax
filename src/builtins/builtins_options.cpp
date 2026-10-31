#include "builtins_internal.hpp"
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

std::optional<int> run_options_builtin(const std::vector<std::string> &a,
                                       ShellState &state) {
  if (a.empty()) return 0;
  const auto &c = a[0];
  if (c == "enable") {
    bool disable = false, print_all = false, print = false;
    size_t i = 1;
    for (; i < a.size() && a[i].size() > 1 && a[i][0] == '-'; ++i) {
      if (a[i] == "--") { ++i; break; }
      for (size_t j = 1; j < a[i].size(); ++j) {
        switch (a[i][j]) {
        case 'n': disable = true; break;
        case 'a': print_all = true; break;
        case 'p': print = true; break;
        case 'f': case 'd':
          std::cerr << "enable: dynamic builtin loading is not supported\n";
          return 2;
        default:
          std::cerr << "enable: invalid option -- '" << a[i][j] << "'\n";
          return 2;
        }
      }
    }
    if (i == a.size() || print || print_all) {
      const bool list_disabled_only = disable && i == a.size() && !print_all;
      for (const auto &name : shell_builtin_names()) {
        const bool is_disabled = state.disabled_builtins.count(name) != 0;
        if ((list_disabled_only && !is_disabled) || (!print_all && !list_disabled_only && is_disabled)) continue;
        std::cout << "enable" << (is_disabled ? " -n " : " ") << name << '\n';
      }
      return 0;
    }
    int status = 0;
    for (; i < a.size(); ++i) {
      if (std::find(shell_builtin_names().begin(), shell_builtin_names().end(), a[i]) ==
          shell_builtin_names().end() || (disable && a[i] == "enable")) {
        std::cerr << "enable: " << a[i] << ": not a shell builtin\n";
        status = 1;
      } else if (disable) {
        state.disabled_builtins.insert(a[i]);
      } else {
        state.disabled_builtins.erase(a[i]);
      }
    }
    return status;
  }
  if (c == "shopt") {
    bool enable = true, query = false, print = false, shell_options_mode = false;
    bool set_flag_seen = false, unset_flag_seen = false;
    size_t first_name = 1;
    for (; first_name < a.size() && a[first_name].size() > 1 && a[first_name][0] == '-'; ++first_name) {
      for (size_t j = 1; j < a[first_name].size(); ++j) {
        switch (a[first_name][j]) {
        case 's': enable = true; set_flag_seen = true; unset_flag_seen = false; break;
        case 'u': enable = false; unset_flag_seen = true; set_flag_seen = false; break;
        case 'q': query = true; break;
        case 'p': print = true; break;
        case 'o': shell_options_mode = true; break;
        default:
          std::cerr << "shopt: " << a[first_name][j] << ": invalid option\n";
          return 2;
        }
      }
    }
    static const std::vector<std::string> shopt_options = {
        "autocd", "cmdhist", "dotglob", "execfail", "extglob", "failglob", "globasciiranges", "globskipdots", "globstar", "histappend", "huponexit", "inherit_errexit", "interactive_comments", "lastpipe", "lithist", "localvar_inherit", "localvar_unset", "nocaseglob", "nocasematch", "no_empty_cmd_completion", "nullglob", "patsub_replacement", "progcomp", "sourcepath", "expand_aliases", "shift_verbose", "varredir_close", "xpg_echo", "checkwinsize", "checkhash", "checkjobs", "promptvars", "cdable_vars", "cdspell"};
    static const std::vector<std::string> set_options = {
        "allexport", "notify", "noclobber", "errexit", "errtrace", "functrace", "pipefail", "noglob",
        "monitor", "noexec", "nounset", "verbose", "xtrace", "hashall", "history", "keyword", "nolog", "physical", "onecmd",
        "braceexpand", "histexpand", "ignoreeof", "emacs", "vi", "posix", "privileged"};
    auto options = shell_options_mode ? set_options : shopt_options;
    std::sort(options.begin(), options.end());
    auto option_enabled = [&](const std::string &name) {
      if (shell_options_mode) return shell_option_enabled(name);
      if (name == "huponexit") return state.huponexit;
      if (name == "inherit_errexit") return state.inherit_errexit;
      if (name == "lastpipe") return state.lastpipe;
      if (name == "shift_verbose") return state.shift_verbose;
      if (name == "varredir_close") return state.varredir_close;
      if (name == "checkwinsize") return state.checkwinsize;
      if (name == "checkhash") return state.checkhash;
      if (name == "checkjobs") return state.checkjobs;
      if (name == "promptvars") return state.promptvars;
      if (name == "cdable_vars") return state.cdable_vars;
      if (name == "cdspell") return state.cdspell;
      if (name == "autocd") return state.autocd;
      if (name == "execfail") return state.execfail;
      if (name == "localvar_inherit") return state.localvar_inherit;
      if (name == "localvar_unset") return state.localvar_unset;
      if (name == "interactive_comments") return state.interactive_comments;
      if (name == "progcomp") return state.progcomp;
      if (name == "no_empty_cmd_completion") return state.no_empty_cmd_completion;
      if (name == "cmdhist") return state.cmdhist;
      if (name == "lithist") return state.lithist;
      if (name == "histappend") return state.histappend;
      return shell_option_enabled(name);
    };
    auto set_option = [&](const std::string &name, bool enabled) {
      if (shell_options_mode) return set_shell_option(name, enabled);
      if (name == "huponexit") { state.huponexit = enabled; refresh_option_parameter_values(); return true; }
      if (name == "inherit_errexit") { state.inherit_errexit = enabled; refresh_option_parameter_values(); return true; }
      if (name == "lastpipe") { state.lastpipe = enabled; refresh_option_parameter_values(); return true; }
      if (name == "shift_verbose") { state.shift_verbose = enabled; refresh_option_parameter_values(); return true; }
      if (name == "varredir_close") { state.varredir_close = enabled; refresh_option_parameter_values(); return true; }
      if (name == "xpg_echo") { state.xpg_echo = enabled; refresh_option_parameter_values(); return true; }
      if (name == "checkwinsize") { state.checkwinsize = enabled; refresh_option_parameter_values(); return true; }
      if (name == "checkhash") { state.checkhash = enabled; refresh_option_parameter_values(); return true; }
      if (name == "checkjobs") { state.checkjobs = enabled; refresh_option_parameter_values(); return true; }
      if (name == "promptvars") { state.promptvars = enabled; refresh_option_parameter_values(); return true; }
      if (name == "cdable_vars") { state.cdable_vars = enabled; refresh_option_parameter_values(); return true; }
      if (name == "cdspell") { state.cdspell = enabled; refresh_option_parameter_values(); return true; }
      if (name == "autocd") { state.autocd = enabled; refresh_option_parameter_values(); return true; }
      if (name == "execfail") { state.execfail = enabled; refresh_option_parameter_values(); return true; }
      if (name == "localvar_inherit") { state.localvar_inherit = enabled; refresh_option_parameter_values(); return true; }
      if (name == "localvar_unset") { state.localvar_unset = enabled; refresh_option_parameter_values(); return true; }
      if (name == "interactive_comments") { state.interactive_comments = enabled; refresh_option_parameter_values(); return true; }
      if (name == "progcomp") { state.progcomp = enabled; refresh_option_parameter_values(); return true; }
      if (name == "no_empty_cmd_completion") { state.no_empty_cmd_completion = enabled; refresh_option_parameter_values(); return true; }
      if (name == "cmdhist") { state.cmdhist = enabled; refresh_option_parameter_values(); return true; }
      if (name == "lithist") { state.lithist = enabled; refresh_option_parameter_values(); return true; }
      if (name == "histappend") { state.histappend = enabled; refresh_option_parameter_values(); return true; }
      return set_shell_option(name, enabled);
    };
    if (first_name == a.size()) {
      if (query) return 1;
      for (const auto &name : options) {
        const bool enabled = option_enabled(name);
        if (set_flag_seen && !enabled) continue;
        if (unset_flag_seen && enabled) continue;
        if (print) {
          if (shell_options_mode)
            std::cout << "set " << (enabled ? "-o " : "+o ") << name << '\n';
          else
            std::cout << "shopt " << (enabled ? "-s " : "-u ") << name << '\n';
        } else {
          std::cout << name << '\t' << (enabled ? "on" : "off") << '\n';
        }
      }
      return 0;
    }
    bool all_enabled = true;
    for (size_t i = first_name; i < a.size(); ++i) {
      if (std::find(options.begin(), options.end(), a[i]) == options.end()) {
        std::cerr << "shopt: " << a[i] << ": invalid shell option name\n";
        return 1;
      }
      if (query) {
        if (!option_enabled(a[i])) all_enabled = false;
      } else if (print) {
        const bool enabled = option_enabled(a[i]);
        if (shell_options_mode)
          std::cout << "set " << (enabled ? "-o " : "+o ") << a[i] << '\n';
        else
          std::cout << "shopt -" << (enabled ? 's' : 'u') << ' ' << a[i] << '\n';
      } else if (!set_option(a[i], enable)) {
        return 1;
      }
    }
    return query ? (all_enabled ? 0 : 1) : 0;
  }
  if (c == "set" || c == "setopt" || c == "unsetopt") {
    static const std::vector<std::string> shell_options = {
        "allexport", "notify", "noclobber", "errexit", "errtrace", "functrace", "pipefail", "noglob",
        "monitor", "noexec", "nounset", "verbose", "xtrace", "hashall", "history", "keyword", "nolog", "physical", "onecmd", "braceexpand", "histexpand", "ignoreeof", "emacs", "vi", "posix", "privileged"};
    auto print_shell_options = [&](bool reusable) {
      auto ordered_options = shell_options;
      std::sort(ordered_options.begin(), ordered_options.end());
      for (const auto &name : ordered_options) {
        const bool enabled = shell_option_enabled(name);
        if (reusable)
          std::cout << "set " << (enabled ? "-o " : "+o ") << name << '\n';
        else
          std::cout << name << '\t' << (enabled ? "on" : "off") << '\n';
      }
    };
    static const std::vector<std::string> shopt_options = {
        "autocd", "cdable_vars", "cdspell", "checkhash", "checkjobs", "checkwinsize",
        "cmdhist", "dotglob", "execfail", "expand_aliases", "extglob", "failglob",
        "globasciiranges", "globskipdots", "globstar", "histappend", "huponexit",
        "inherit_errexit", "interactive_comments", "lastpipe", "lithist",
        "localvar_inherit", "localvar_unset", "nocaseglob", "nocasematch",
        "no_empty_cmd_completion", "nullglob", "patsub_replacement", "progcomp",
        "promptvars", "shift_verbose", "sourcepath", "varredir_close", "xpg_echo"};
    auto print_shopt_options = [&](bool reusable) {
      const std::string enabled_options = ":" + shell_enabled_shopt_options() + ":";
      auto ordered_options = shopt_options;
      std::sort(ordered_options.begin(), ordered_options.end());
      for (const auto &name : ordered_options) {
        const bool enabled = enabled_options.find(":" + name + ":") != std::string::npos;
        if (reusable)
          std::cout << "set " << (enabled ? "-O " : "+O ") << name << '\n';
        else
          std::cout << name << '\t' << (enabled ? "on" : "off") << '\n';
      }
    };
    auto set_shopt_option = [&](const std::string &name, bool enabled) {
      if (std::find(shopt_options.begin(), shopt_options.end(), name) == shopt_options.end()) {
        std::cerr << "set: " << name << ": invalid shell option name\n";
        return 1;
      }
      const auto result = run_options_builtin(
          {"shopt", enabled ? "-s" : "-u", name}, state);
      return result.value_or(1);
    };
    if ((c == "setopt" || c == "unsetopt") && a.size() == 1) {
      for (const auto &name : shell_options)
        std::cout << name << '\t'
                  << (shell_option_enabled(name) ? "on" : "off")
                  << '\n';
      return 0;
    }
    if (c == "set" && a.size() == 1) {
      (void)run_environment_builtin({"declare", "-p"}, state);
      // Bash's POSIX mode omits function definitions from bare `set` output.
      // Keep the normal Bash-style listing outside POSIX mode.
      if (!state.posix_mode)
        (void)run_environment_builtin({"declare", "-f"}, state);
      return 0;
    }
    if (c == "set" && a.size() == 2 && (a[1] == "-o" || a[1] == "+o")) {
      print_shell_options(a[1] == "+o");
      return 0;
    }
    for (size_t i = 1; i < a.size(); ++i) {
      auto x = a[i];
      bool on = c != "unsetopt";
      if (c == "set" && x == "--") {
        state.positionals.assign(a.begin() + i + 1, a.end());
        break;
      } else if (c == "set" && x == "-") {
        set_shell_option("xtrace", false);
        set_shell_option("verbose", false);
        if (i + 1 < a.size())
          state.positionals.assign(a.begin() + i + 1, a.end());
        break;
      } else if (x == "-o" || x == "+o") {
        if (i + 1 >= a.size()) {
          print_shell_options(x == "+o");
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
          if (flag == 'o') {
            if (i + 1 >= a.size()) {
              print_shell_options(!on);
              break;
            }
            const std::string option_name = a[++i];
            if (!set_shell_option(option_name, on)) {
              std::cerr << c << ": unsupported option: " << option_name << '\n';
              return 2;
            }
            continue;
          }
          if (flag == 'O') {
            std::string option_name;
            if (j + 1 < x.size()) {
              option_name = x.substr(j + 1);
              j = x.size();
            } else if (i + 1 < a.size() &&
                       (a[i + 1].empty() ||
                        (a[i + 1][0] != '-' && a[i + 1][0] != '+'))) {
              option_name = a[++i];
            } else {
              print_shopt_options(!on);
              break;
            }
            if (set_shopt_option(option_name, on) != 0) return 1;
            break;
          }
          const std::string name = flag == 'a' ? "allexport"
                                   : flag == 'b' ? "notify"
                                   : flag == 'C' ? "noclobber"
                                   : flag == 'e' ? "errexit"
                                   : flag == 'E' ? "errtrace"
                                   : flag == 'T' ? "functrace"
                                   : flag == 'h' ? "hashall"
                                   : flag == 'k' ? "keyword"
                                   : flag == 'P' ? "physical"
                                   : flag == 'p' ? "privileged"
                                   : flag == 't' ? "onecmd"
                                   : flag == 'B' ? "braceexpand"
                                   : flag == 'H' ? "histexpand"
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
          if (!set_shell_option(name, on)) {
            std::cerr << "set: could not change effective user or group ID\n";
            return 1;
          }
        }
      } else if (c == "set" && (x.empty() || (x[0] != '-' && x[0] != '+'))) {
        state.positionals.assign(a.begin() + i, a.end());
        break;
      } else if (!set_shell_option(x, on)) {
        std::cerr << c << ": unsupported option: " << x << '\n';
        return 2;
      }
    }
    return 0;
  }
  return {};
}
