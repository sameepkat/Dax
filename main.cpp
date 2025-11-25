#include <iostream>
#include <sstream>
#include <string>
#include <csignal>
#include <vector>
#include "signal_handler.hpp"
#include "builtins.hpp"
#include "tokenizer.hpp"

int main() {
  std::string line;
  std::vector<std::string> tokens;

  std::signal(SIGINT, SIG_IGN);
  std::signal(SIGCHLD, SIG_IGN);

  while (true) {
    std::cout << "> ";
    std::cout.flush();
    if (!std::getline(std::cin, line)) break;
    
    tokens = tokenize(line);
    if(tokens.empty()) continue;

    if(handle_builtin(tokens)) continue;


    //std::cout << "Not a built-in command: " << tokens[0] << "\n";

     std::cout << "Tokens: \n";
     std::cout << "Tokens: [";
     for (const std::string& token: tokens) {
         std::cout << token << "," ;
     }
     std::cout << "]";
     std::cout << "\n";
   }
  std::cout << "Token size: " << tokens.size() << "\n";
  return 0;
}
