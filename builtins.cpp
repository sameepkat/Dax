#include "builtins.hpp"
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <stdlib.h>
#include "helper.hpp"

// Internal environment map
static std::unordered_map<std::string, std::string> shell_vars;
static std::unordered_map<std::string, std::string> alias_map;

// Dispatcher
BuiltinFunc handle_builtin(const std::vector<std::string> &input) {
    static std::unordered_map<std::string, BuiltinFunc> builtins ={
      {"exit", builtin_exit},
      {"cd", builtin_cd},
      {"echo", builtin_echo},
      {"pwd", builtin_pwd},
      {"export", builtin_export},
        };

  if (input.empty())
    return nullptr;

  auto it = builtins.find(input[0]);
  if (it != builtins.end()) {
    it->second(input);
    return it->second;
  }
  return nullptr;
}

bool builtin_exit(const std::vector<std::string> &input) {
    if(input.empty() || input[0] != "exit") return false;
    exit(0);
}

bool builtin_cd(const std::vector<std::string> &input) {
    if(input.empty() || input[0] != "cd") return false;

    if (input.size() < 2) {
      std::cerr << "cd: missing destination\n";
      return true;
    }

    if (chdir(input[1].c_str()) != 0) {
        std::cerr << "cd: " << strerror(errno) << "\n";
    }
    return true;
}

bool builtin_pwd(const std::vector<std::string> &input){
    char curr_dir[1024];
    if (getcwd(curr_dir, sizeof(curr_dir)) != nullptr) {
        std::cout << curr_dir << "\n";
        return true;
    } else {
        return false;
    }
}

bool builtin_echo(const std::vector<std::string> &input) {
    if(input.size() < 1) return false;
  for (int i = 1; i < input.size(); i++) {
      std::cout << input[i] << " ";
  }
    std::cout << "\n";
  return true;
}

bool builtin_export(const std::vector<std::string> &input){
    if (input.size() < 2) {
        return false;
    }

    std::vector<std::string> key_val = extract_key_value(input[1]);
    if (key_val.size() != 2) {
        std::cerr << "can't find key value pair.\n";
        return false;
    }
    std::string key = key_val[0];
    std::string value = key_val[1];
    shell_vars[key] = value;

    print_table(shell_vars);

    if (setenv(key.c_str(), value.c_str(), 1) != 0) {
        std::cerr << "setenv failed.\n ";
        return false;
    }

    return true;
}

bool builtin_alias(const std::vector<std::string> &) {
    return false;
}
bool builtin_unalias(const std::vector<std::string> &) {
    return false;
}

