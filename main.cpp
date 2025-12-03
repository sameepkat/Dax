#include <iostream>
#include <sstream>
#include <string>
#include <csignal>
#include <vector>
#include "signal_handler.hpp"
#include "builtins.hpp"
#include "tokenizer.hpp"
#include "helper.hpp"
#include "parser.hpp"

int main() {
  std::string line;
  std::vector<std::string> tokens;

  setup_signals();

  while (true) {
    print_prompt();
    if (!std::getline(std::cin, line)) break;
    
    if(line.empty()) continue;
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
  return 0;
}
