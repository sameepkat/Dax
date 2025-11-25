#include "helper.hpp"

std::vector<std::string> extract_key_value(const std::string &token) {
  auto pos = token.find('=');
  if (pos == std::string::npos) {
    std::cerr << "export: invalid format, expected VAR=value\n";
    return {};
  }

  std::string key = token.substr(0, pos);
  std::string value = token.substr(pos+1);

  return {key, value};
}

void print_vector(const std::vector<std::string> &vector) {
  for (const auto it : vector) {
      std::cout << it << "\n";
  } 
}

void print_table(std::unordered_map<std::string, std::string> &shellvar_map) {
    for (const auto& [key, value] : shellvar_map) {
        // Print each key-value pair
        std::cout << "Key: [" << key << "] Value: [" << value << "]\n";
    }
}
