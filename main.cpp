#include "builtins.hpp"
#include "helper.hpp"
#include "parser.hpp"
#include "signal_handler.hpp"
#include "tokenizer.hpp"
#include <csignal>
#include <iostream>
#include <readline/history.h>
#include <readline/readline.h>
#include <sstream>
#include <string>
#include <vector>

void execute_line(const std::string &line) {
  auto tokens = tokenize(line);
  if (tokens.empty())
    return;
  static std::string pending_name;
  static std::vector<std::string> pending_body;
  auto close_function = [&](size_t start) {
    for (size_t i = start; i < tokens.size(); ++i) {
      if (tokens[i] == "}") {
        std::string body;
        for (const auto &token : pending_body) {
          if (!body.empty())
            body += ' ';
          body += token;
        }
        define_function(pending_name, body);
        pending_name.clear();
        pending_body.clear();
        return true;
      }
      pending_body.push_back(tokens[i]);
    }
    return false;
  };
  if (!pending_name.empty()) {
    close_function(0);
    return;
  }
  size_t open = tokens.size(), name_index = 0, body_start = 0;
  if (tokens.size() >= 3 && tokens[0] == "function") {
    name_index = 1;
    for (size_t i = 2; i < tokens.size(); ++i)
      if (tokens[i] == "{") {
        open = i;
        break;
      }
  } else if (tokens.size() >= 4 && tokens[1] == "(" && tokens[2] == ")") {
    name_index = 0;
    for (size_t i = 3; i < tokens.size(); ++i)
      if (tokens[i] == "{") {
        open = i;
        break;
      }
  }
  if (open < tokens.size()) {
    pending_name = tokens[name_index];
    body_start = open + 1;
    close_function(body_start);
    return;
  }
  expand_aliases(tokens);
  try {
    auto ast = parse(tokens);
    const int status = ast ? ast->execute() : 0;
    set_last_status(status);
    apply_errexit(status);
  } catch (const std::exception &e) {
    std::cerr << "Parse/Exec Error: " << e.what() << "\n";
    set_last_status(2);
  }
  run_pending_signal_trap();
}

int main() {
  std::string line;
  std::vector<std::string> tokens;

  setup_signals();

  // Load history from file on startup
  History::load_from_file();

  // Set history size limit to 1000 commands
  stifle_history(1000);

  // Load existing history into readline
  const auto &hist = History::get_all();
  for (const auto &cmd : hist) {
    add_history(cmd.c_str());
  }

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
      prompt_str = "\001\033[1;32m\002" + dir_name + " > \001\033[0m\002";
    } else {
      prompt_str = "\001\033[1;32m\002> \001\033[0m\002";
    }

    // Use readline for input with arrow key support
    char *input = readline(prompt_str.c_str());

    // Handle Ctrl+D (EOF)
    if (!input) {
      std::cout << "\n";
      break;
    }

    line = input;
    free(input);

    if (line.empty())
      continue;

    // Add to both readline history and our History namespace
    add_history(line.c_str());
    History::add(line);

    execute_line(line);
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

  return shell_should_exit() ? requested_exit_status() : last_status();
}
