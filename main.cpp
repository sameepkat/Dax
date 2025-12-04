#include <iostream>
#include <sstream>
#include <string>
#include <csignal>
#include <vector>
#include <readline/readline.h>
#include <readline/history.h>
#include "signal_handler.hpp"
#include "builtins.hpp"
#include "tokenizer.hpp"
#include "helper.hpp"
#include "parser.hpp"

int main() {
  std::string line;
  std::vector<std::string> tokens;

  setup_signals();

  // Load history from file on startup
  History::load_from_file();
  
  // Set history size limit to 1000 commands
  stifle_history(1000);

  // Load existing history into readline
  const auto& hist = History::get_all();
  for (const auto& cmd : hist) {
    add_history(cmd.c_str());
  }

  while (true) {
    // Get prompt string
    char cwd[1024];
    std::string prompt_str;
    if (getcwd(cwd, sizeof(cwd)) != nullptr) {
        std::string path(cwd);
        const char* home = getenv("HOME");
        if (home) {
            std::string home_str(home);
            if (path.find(home_str) == 0) {
                path.replace(0, home_str.length(), "~");
            }
        }
        prompt_str = path + " > ";
    } else {
        prompt_str = "> ";
    }

    // Use readline for input with arrow key support
    char* input = readline(prompt_str.c_str());
    
    // Handle Ctrl+D (EOF)
    if (!input) {
      std::cout << "\n";
      break;
    }
    
    line = input;
    free(input);
    
    if(line.empty()) continue;
    
    // Add to both readline history and our History namespace
    add_history(line.c_str());
    History::add(line);

    tokens = tokenize(line);
    if(tokens.empty()) continue;

    // Execute the AST
    try {
        auto ast = parse(tokens);
        if (ast) {
            ast->execute();
        }
    } catch (const std::exception& e) {
        std::cerr << "Parse/Exec Error: " << e.what() << "\n";
    }


    //std::cout << "Not a built-in command: " << tokens[0] << "\n";

    //  std::cout << "Tokens: [";
    //  for (size_t i = 0; i < tokens.size(); ++i) {
    //      std::cout << tokens[i];
    //      if (i < tokens.size() - 1) std::cout << ",";
    //  }
    //  std::cout << "]\n";
   }
  
  // Save history to file before exiting
  History::save_to_file();
  
  return 0;
}
