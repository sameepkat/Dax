#include "builtins.hpp"
#include "helper.hpp"
#include "interactive_shell.hpp"
#include "janet_runtime.hpp"
#include "shell.hpp"
#include <version.hpp>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <unistd.h>
#include <vector>



enum class CliMode { Interactive, Command, Script, Stdin, Janet, RestrictedJanet };

struct CliInvocation {
  CliMode mode = CliMode::Interactive;
  bool show_help = false;
  bool show_version = false;
  bool shell_option_seen = false;
  bool login = false;
  bool force_interactive = false;
  bool has_operands = false;
  bool monitor_option_seen = false;
  std::string command;
  std::string shell_name = "dax";
  std::string script_path;
  std::string workspace;
  std::vector<std::string> arguments;
};

static void print_usage(std::ostream &out) {
  out << "Usage: dax [options] [script [args...]]\n"
         "       dax [options] -c COMMAND [NAME [ARG...]]\n"
         "       dax [options] -s [ARG...]\n"
         "       dax --script FILE [ARG...]\n"
         "       dax --janet FILE [ARG...]\n"
         "       dax --janet-restricted WORKSPACE FILE [ARG...]\n\n"
         "Options:\n"
         "  --help, --version       Show help or version and exit\n"
         "  --                      Stop processing startup options\n"
         "  -l                      Start as a login shell (Termux extension)\n"
         "  -i                      Force interactive mode\n"
         "  -s                      Read shell commands from standard input\n"
         "  -c COMMAND             Run one Dax shell command\n"
         "  -o OPTION              Enable a shell option; -o no_OPTION disables it\n"
         "  --OPTION, --no-OPTION  Enable or disable a shell option\n"
         "  +o OPTION              Disable a shell option; +o no_OPTION enables it\n"
         "  +-OPTION, +-no-OPTION  Disable or enable a shell option\n"
         "  -a/-b/-C/-e/-f/-m/-n/-u/-v/-x and + forms\n"
         "                         POSIX shell option flags\n"
         "Named options: allexport, notify, noclobber, errexit, pipefail, noglob,\n"
         "               monitor, noexec, nounset, verbose, xtrace\n\n"
         "Dax supports POSIX-style invocation and these options; this does not\n"
         "claim full POSIX shell-language conformance.\n";
}

static bool apply_named_cli_option(const std::string &name, bool enabled,
                                   bool &option_seen, std::string &error) {
  if (!set_shell_option(name, enabled)) {
    error = "unknown shell option: " + name;
    return false;
  }
  option_seen = true;
  return true;
}

static bool parse_cli(int argc, char **argv, CliInvocation &invocation,
                      std::string &error) {
  size_t i = 1;
  bool stop_options = false;
  auto set_flag = [&](char flag, bool enabled) {
    const std::string name = flag == 'a' ? "allexport"
                             : flag == 'b' ? "notify"
                             : flag == 'C' ? "noclobber"
                             : flag == 'e' ? "errexit"
                             : flag == 'f' ? "noglob"
                             : flag == 'm' ? "monitor"
                             : flag == 'n' ? "noexec"
                             : flag == 'u' ? "nounset"
                             : flag == 'v' ? "verbose"
                             : flag == 'x' ? "xtrace" : "";
    if (name.empty()) return false;
    if (flag == 'm') invocation.monitor_option_seen = true;
    return apply_named_cli_option(name, enabled,
                                  invocation.shell_option_seen, error);
  };
  auto set_named_flag = [&](std::string name, bool default_enabled) {
    bool enabled = default_enabled;
    if (name.rfind("no_", 0) == 0 || name.rfind("no-", 0) == 0) {
      name.erase(0, 3);
      enabled = !enabled;
    }
    if (name == "monitor") invocation.monitor_option_seen = true;
    return apply_named_cli_option(name, enabled,
                                  invocation.shell_option_seen, error);
  };
  while (i < static_cast<size_t>(argc)) {
    const std::string arg = argv[i];
    if (!stop_options && arg == "--help") {
      invocation.show_help = true;
      return true;
    }
    if (!stop_options && arg == "--version") {
      invocation.show_version = true;
      return true;
    }
    if (!stop_options && arg == "--") {
      stop_options = true;
      ++i;
      continue;
    }
    if (!stop_options &&
        (arg == "--script" || arg == "--janet" ||
         arg == "--janet-restricted")) {
      if (arg == "--janet-restricted") {
        if (i + 2 >= static_cast<size_t>(argc)) {
          error = "--janet-restricted requires a workspace and script path";
          return false;
        }
        invocation.mode = CliMode::RestrictedJanet;
        invocation.workspace = argv[++i];
        invocation.script_path = argv[++i];
      } else {
        if (i + 1 >= static_cast<size_t>(argc)) {
          error = arg + " requires a script path";
          return false;
        }
        invocation.mode = arg == "--janet" ? CliMode::Janet : CliMode::Script;
        invocation.script_path = argv[++i];
      }
      ++i;
      for (; i < static_cast<size_t>(argc); ++i)
        invocation.arguments.emplace_back(argv[i]);
      invocation.has_operands = !invocation.arguments.empty();
      return true;
    }
    if (!stop_options && (arg == "-o" || arg == "+o")) {
      const bool default_enabled = arg == "-o";
      if (++i >= static_cast<size_t>(argc)) {
        error = arg + " requires an option name";
        return false;
      }
      if (!set_named_flag(argv[i], default_enabled)) return false;
      ++i;
      continue;
    }
    if (!stop_options && arg.rfind("+-", 0) == 0) {
      if (!set_named_flag(arg.substr(2), false))
        return false;
      ++i;
      continue;
    }
    if (!stop_options && arg.rfind("--no-", 0) == 0) {
      if (!set_named_flag(arg.substr(5), false))
        return false;
      ++i;
      continue;
    }
    if (!stop_options && arg.rfind("--", 0) == 0) {
      if (!set_named_flag(arg.substr(2), true))
        return false;
      ++i;
      continue;
    }
    if (!stop_options && arg.size() > 1 &&
        (arg[0] == '-' || arg[0] == '+')) {
      const bool enabled = arg[0] == '-';
      for (size_t j = 1; j < arg.size(); ++j) {
        const char flag = arg[j];
        if (flag == 'o') {
          std::string option_name;
          if (j + 1 < arg.size()) {
            option_name = arg.substr(j + 1);
            j = arg.size();
          } else if (i + 1 < static_cast<size_t>(argc)) {
            option_name = argv[++i];
          } else {
            error = std::string(1, arg[0]) + "o requires an option name";
            return false;
          }
          if (!set_named_flag(option_name, enabled)) return false;
          break;
        }
        if (flag == 'c' && enabled) {
          std::string command;
          if (j + 1 < arg.size()) command = arg.substr(j + 1);
          else if (i + 1 < static_cast<size_t>(argc)) command = argv[++i];
          else {
            error = "-c requires a command string";
            return false;
          }
          invocation.mode = CliMode::Command;
          invocation.command = std::move(command);
          invocation.has_operands = true;
          ++i;
          if (i < static_cast<size_t>(argc)) {
            invocation.shell_name = argv[i++];
            for (; i < static_cast<size_t>(argc); ++i)
              invocation.arguments.emplace_back(argv[i]);
            invocation.has_operands = true;
          }
          return true;
        }
        if (flag == 'i' && enabled) {
          invocation.force_interactive = true;
          continue;
        }
        if (flag == 'l' && enabled) {
          invocation.login = true;
          continue;
        }
        if (flag == 's' && enabled) {
          invocation.mode = CliMode::Stdin;
          ++i;
          for (; i < static_cast<size_t>(argc); ++i)
            invocation.arguments.emplace_back(argv[i]);
          invocation.has_operands = !invocation.arguments.empty();
          return true;
        }
        if (!set_flag(flag, enabled)) {
          error = "invalid option: " + arg;
          return false;
        }
      }
      ++i;
      continue;
    }
    if (!stop_options && !arg.empty() && arg[0] == '-') {
      error = "invalid option: " + arg;
      return false;
    }
    if (!stop_options && arg.size() > 1 && arg[0] == '+') {
      error = "invalid option: " + arg;
      return false;
    }

    invocation.mode = CliMode::Script;
    invocation.script_path = arg;
    invocation.shell_name = arg;
    invocation.has_operands = true;
    ++i;
    for (; i < static_cast<size_t>(argc); ++i)
      invocation.arguments.emplace_back(argv[i]);
    return true;
  }
  return true;
}

static bool start_interactive_runtime() {
  if (!janet_runtime_start()) return false;
  const char *config_root = std::getenv("XDG_CONFIG_HOME");
  std::string init_path;
  if (config_root && *config_root)
    init_path = std::string(config_root) + "/dax/init.janet";
  else if (const char *home = std::getenv("HOME"))
    init_path = std::string(home) + "/.config/dax/init.janet";
  if (!init_path.empty()) {
    std::ifstream init(init_path);
    if (init) {
      std::vector<std::string> no_args;
      janet_run_file(init_path, no_args);
      janet_run_startup_hook();
    }
  }
  return true;
}


static void run_stdin_commands(std::istream &input) {
  std::string source_line;
  std::string command_buffer;
  while (std::getline(input, source_line)) {
    if (!command_buffer.empty()) command_buffer += '\n';
    command_buffer += source_line;
    if (shell_input_incomplete(command_buffer)) continue;
    execute_line(command_buffer);
    print_job_notifications();
    command_buffer.clear();
    if (shell_should_exit()) break;
  }
  if (!command_buffer.empty() && !shell_should_exit())
    execute_line(command_buffer);
}

int main(int argc, char **argv) {
  CliInvocation invocation;
  std::string cli_error;
  if (!parse_cli(argc, argv, invocation, cli_error)) {
    std::cerr << "dax: " << cli_error << "\nTry 'dax --help' for usage.\n";
    return 2;
  }
  if (invocation.show_help) {
    print_usage(std::cout);
    return 0;
  }
  if (invocation.show_version) {
    std::cout << "dax " << DAX_VERSION << "\n";
    return 0;
  }

  const bool dax_invocation = invocation.mode != CliMode::Janet &&
                              invocation.mode != CliMode::RestrictedJanet;
  const bool stdin_requested = invocation.mode == CliMode::Stdin;
  const bool terminal_default =
      (invocation.mode == CliMode::Interactive ||
       invocation.mode == CliMode::Stdin) && !invocation.has_operands &&
      isatty(STDIN_FILENO) && isatty(STDERR_FILENO);
  const bool interactive = dax_invocation &&
      (invocation.force_interactive || terminal_default);
  if ((invocation.mode == CliMode::Interactive ||
       invocation.mode == CliMode::Stdin) && !interactive)
    invocation.mode = CliMode::Stdin;
  else if (invocation.mode == CliMode::Stdin && interactive)
    invocation.mode = CliMode::Interactive;
  set_shell_modes(invocation.login, interactive,
                  invocation.monitor_option_seen);

  if (invocation.mode == CliMode::Command)
    set_shell_arguments(invocation.shell_name, invocation.arguments);
  else if (invocation.mode == CliMode::Script)
    set_shell_arguments(invocation.script_path, invocation.arguments);
  else if (invocation.mode == CliMode::Stdin)
    set_shell_arguments(argc > 0 ? argv[0] : "dax", invocation.arguments);
  else if (invocation.mode == CliMode::Interactive)
    set_shell_arguments(argc > 0 ? argv[0] : "dax",
                        stdin_requested ? invocation.arguments
                                        : std::vector<std::string>{});

  if (interactive) {
    if (!start_interactive_runtime()) return 1;
  }

  if (invocation.mode == CliMode::Command) {
    execute_line(invocation.command);
    print_job_notifications();
    run_exit_trap();
    janet_runtime_stop();
    return shell_should_exit() ? requested_exit_status() : last_status();
  }
  if (invocation.mode == CliMode::Script) {
    std::ifstream script(invocation.script_path);
    if (!script) {
      std::cerr << "dax: cannot open script: " << invocation.script_path << "\n";
      janet_runtime_stop();
      return 127;
    }
    std::ostringstream source;
    source << script.rdbuf();
    execute_line(source.str());
    print_job_notifications();
    run_exit_trap();
    janet_runtime_stop();
    return shell_should_exit() ? requested_exit_status() : last_status();
  }
  if (invocation.mode == CliMode::Stdin) {
    run_stdin_commands(std::cin);
    print_job_notifications();
    run_exit_trap();
    janet_runtime_stop();
    return shell_should_exit() ? requested_exit_status() : last_status();
  }
  if (invocation.mode == CliMode::Janet) {
    if (!janet_runtime_start()) return 1;
    const int result = janet_run_file(invocation.script_path, invocation.arguments);
    janet_runtime_stop();
    return result;
  }
  if (invocation.mode == CliMode::RestrictedJanet)
    return janet_run_restricted_file(invocation.script_path, invocation.workspace,
                                     invocation.arguments);

  return run_interactive_shell();
}
