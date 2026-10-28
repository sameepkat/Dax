#include "builtins.hpp"
#include "helper.hpp"
#include "janet_runtime.hpp"
#include "parser.hpp"
#include "signal_handler.hpp"
#include "tokenizer.hpp"
#include <version.hpp>
#include <csignal>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <readline/history.h>
#include <readline/readline.h>
#include <sstream>
#include <string>
#include <utility>
#include <unistd.h>
#include <vector>

void execute_line(const std::string &line) {
  if (shell_verbose_enabled()) {
    std::istringstream input(line);
    std::string source_line;
    while (std::getline(input, source_line)) std::cerr << source_line << '\n';
  }
  auto tokens = tokenize(line);
  if (tokens.empty())
    return;
  static std::string pending_name;
  static std::string pending_body_source;
  size_t function_end = 0;
  auto finish_pending_function = [&](size_t start) {
    for (size_t i = start; i < tokens.size(); ++i) {
      if (tokens[i] == "}") {
        if (!pending_body_source.empty()) pending_body_source += '\n';
        pending_body_source += line.substr(0, tokens[i].source_begin);
        if (!shell_noexec_enabled())
          define_function(pending_name, pending_body_source);
        pending_name.clear();
        pending_body_source.clear();
        function_end = i + 1;
        return true;
      }
    }
    if (!pending_body_source.empty()) pending_body_source += '\n';
    pending_body_source += line;
    return false;
  };
  if (!pending_name.empty()) {
    if (!finish_pending_function(0)) return;
    tokens.erase(tokens.begin(), tokens.begin() + function_end);
    while (!tokens.empty() && tokens.front() == ";") tokens.erase(tokens.begin());
  }
  for (size_t i = 0; i < tokens.size();) {
    size_t name_index = i, search_start = i;
    bool declaration = false;
    if (tokens[i] == "function" && i + 1 < tokens.size()) {
      name_index = i + 1;
      search_start = i + 2;
      declaration = true;
    } else if (i + 2 < tokens.size() && tokens[i + 1] == "(" && tokens[i + 2] == ")") {
      search_start = i + 3;
      declaration = true;
    }
    if (!declaration) { ++i; continue; }
    size_t open = search_start;
    while (open < tokens.size() && tokens[open] != "{") ++open;
    if (open == tokens.size()) { ++i; continue; }
    size_t close = open + 1;
    while (close < tokens.size() && tokens[close] != "}") ++close;
    if (close == tokens.size()) {
      pending_name = tokens[name_index];
      pending_body_source = std::string(tokens[open].line - 1, '\n') +
                            std::string(tokens[open].column, ' ') +
                            line.substr(tokens[open].source_end);
      tokens.erase(tokens.begin() + i, tokens.end());
      break;
    }
    std::string body;
    if (close > open + 1) {
      const auto &first_body_token = tokens[open + 1];
      const auto &last_body_token = tokens[close - 1];
      if (last_body_token.source_end >= first_body_token.source_begin &&
          last_body_token.source_end <= line.size()) {
        body = line.substr(first_body_token.source_begin,
                           last_body_token.source_end - first_body_token.source_begin);
        body.insert(0, std::string(first_body_token.line - 1, '\n') +
                           std::string(first_body_token.column - 1, ' '));
      }
    }
    if (body.empty()) {
      for (size_t j = open + 1; j < close; ++j) {
        if (!body.empty()) body += ' ';
        body += tokens[j];
      }
    }
    if (!shell_noexec_enabled())
      define_function(tokens[name_index], body);
    tokens.erase(tokens.begin() + i, tokens.begin() + close + 1);
    while (i < tokens.size() && tokens[i] == ";") tokens.erase(tokens.begin() + i);
  }
  if (tokens.empty()) return;
  expand_aliases(tokens);
  try {
    auto ast = parse(tokens, line);
    const int status = shell_noexec_enabled() ? 0 : (ast ? ast->execute() : 0);
    set_last_status(status);
    apply_errexit(status);
    clear_errexit_immune();
  } catch (const std::exception &e) {
    std::cerr << "Parse/Exec Error: " << e.what() << "\n";
    set_last_status(2);
  }
  run_pending_signal_trap();
}

static bool shell_input_incomplete(const std::string &source) {
  const auto tokens = tokenize(source);
  int if_depth = 0, loop_depth = 0, paren_depth = 0, brace_depth = 0;
  bool command_start = true;
  bool missing_heredoc = false;
  for (const auto &token : tokens) {
    if (token.size() == 1 && token[0] == '\x06') missing_heredoc = true;
    if (token == "if" && command_start) ++if_depth;
    else if (token == "fi" && if_depth > 0) --if_depth;
    if ((token == "for" || token == "while" || token == "until") && command_start) ++loop_depth;
    else if (token == "done" && loop_depth > 0) --loop_depth;
    if (token == "(") ++paren_depth;
    else if (token == ")" && paren_depth > 0) --paren_depth;
    if (token == "{") ++brace_depth;
    else if (token == "}" && brace_depth > 0) --brace_depth;
    command_start = token == ";" || token == "&" || token == "&&" || token == "||" ||
                    token == "|" || token == "then" || token == "do" || token == "else" || token == "elif";
  }
  std::string token_text;
  for (const auto &token : tokens) token_text += token;
  const auto single_quotes = std::count(token_text.begin(), token_text.end(), '\x01');
  const auto double_quotes = std::count(token_text.begin(), token_text.end(), '\x02');
  const auto command_substitutions = std::count(token_text.begin(), token_text.end(), '\x03');
  bool open_operator = !tokens.empty() &&
      (tokens.back() == "&&" || tokens.back() == "||" || tokens.back() == "|");
  return if_depth > 0 || loop_depth > 0 || paren_depth > 0 || brace_depth > 0 ||
         (single_quotes % 2) || (double_quotes % 2) || (command_substitutions % 2) ||
         missing_heredoc || open_operator;
}

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
         "Named options: allexport, notify, noclobber, errexit, noglob,\n"
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

static void print_job_notifications(bool redraw_line = false) {
  const auto notifications = take_completed_job_notifications();
  if (notifications.empty()) return;
  if (redraw_line) rl_crlf();
  for (const auto &notification : notifications)
    std::cout << notification << '\n';
  std::cout.flush();
  if (redraw_line) {
    rl_on_new_line();
    rl_redisplay();
  }
}

static int readline_job_event() {
  print_job_notifications(true);
  return 0;
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

  bool janet_started = false;
  if (interactive) {
    if (!start_interactive_runtime()) return 1;
    janet_started = true;
  }

  if (invocation.mode == CliMode::Command) {
    execute_line(invocation.command);
    print_job_notifications();
    run_exit_trap();
    if (janet_started) janet_runtime_stop();
    return shell_should_exit() ? requested_exit_status() : last_status();
  }
  if (invocation.mode == CliMode::Script) {
    std::ifstream script(invocation.script_path);
    if (!script) {
      std::cerr << "dax: cannot open script: " << invocation.script_path << "\n";
      if (janet_started) janet_runtime_stop();
      return 127;
    }
    std::ostringstream source;
    source << script.rdbuf();
    execute_line(source.str());
    print_job_notifications();
    run_exit_trap();
    if (janet_started) janet_runtime_stop();
    return shell_should_exit() ? requested_exit_status() : last_status();
  }
  if (invocation.mode == CliMode::Stdin) {
    run_stdin_commands(std::cin);
    print_job_notifications();
    run_exit_trap();
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

  std::string line;
  std::string command_buffer;

  // Load history from file on startup
  History::load_from_file();

  // Set history size limit to 1000 commands
  stifle_history(1000);

  // Load existing history into readline
  const auto &hist = History::get_all();
  for (const auto &cmd : hist) {
    add_history(cmd.c_str());
  }

  setup_signals();
  rl_event_hook = readline_job_event;

  while (true) {
    char cwd[1024];
    std::string prompt_str;
    if (getcwd(cwd, sizeof(cwd)) != nullptr) {
      std::string path(cwd);
      std::string dir_name;

      if (path == "/") {
        dir_name = "/";
      } else {
        size_t last_slash = path.find_last_of('/');
        if (last_slash != std::string::npos) {
          dir_name = path.substr(last_slash + 1);
        } else {
          dir_name = path;
        }
      }

      // Green color: \033[1;32m, Reset: \033[0m
      // \001 and \002 are used to tell readline these are non-printing
      // characters
      prompt_str = janet_prompt().empty() ? "\001\033[1;32m\002" + dir_name + " > \001\033[0m\002" : janet_prompt();
    } else {
      prompt_str = "\001\033[1;32m\002> \001\033[0m\002";
    }

    // Use readline for input with arrow key support
    const std::string input_prompt = command_buffer.empty() ? prompt_str : "... ";
    begin_readline_input(prompt_str.c_str());
    char *input = readline(input_prompt.c_str());
    const bool input_interrupted = finish_readline_input();

    // Handle Ctrl+D (EOF)
    if (!input) {
      std::cout << "\n";
      break;
    }

    line = input;
    free(input);

    if (input_interrupted)
      command_buffer.clear();

    if (line.empty() && command_buffer.empty())
      continue;

    // Add to both readline history and our History namespace
    add_history(line.c_str());
    History::add(line);

    if (!command_buffer.empty()) command_buffer += '\n';
    command_buffer += line;
    if (shell_input_incomplete(command_buffer)) continue;
    execute_line(command_buffer);
    print_job_notifications();
    command_buffer.clear();
    if (shell_should_exit())
      break;

    // std::cout << "Not a built-in command: " << tokens[0] << "\n";

    //  std::cout << "Tokens: [";
    //  for (size_t i = 0; i < tokens.size(); ++i) {
    //      std::cout << tokens[i];
    //      if (i < tokens.size() - 1) std::cout << ",";
    //  }
    //  std::cout << "]\n";
  }

  // Save history to file before exiting
  run_exit_trap();
  History::save_to_file();

  janet_runtime_stop();
  rl_event_hook = nullptr;

  return shell_should_exit() ? requested_exit_status() : last_status();
}
