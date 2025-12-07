#include "helper.hpp"
#include <unistd.h>
#include <cstdlib>
#include <readline/history.h>
#include <filesystem>
namespace fs = std::filesystem;

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
  for (const auto &it : vector) {
      std::cout << it << "\n";
  } 
}

void print_table(const std::unordered_map<std::string, std::string> &shellvar_map) {
    for (const auto& [key, value] : shellvar_map) {
        // Print each key-value pair
        std::cout << "Key: [" << key << "] Value: [" << value << "]\n";
    }
}

void print_prompt() {
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != nullptr) {
        fs::path path(cwd);
        std::string dir_name = path.filename().string();
        if (dir_name.empty()) dir_name = "/";
        
        std::cout << "\033[1;32m" << dir_name << " > \033[0m";
    } else {
        std::cout << "\033[1;32m> \033[0m";
    }
    std::cout.flush();
}


namespace History{
  static std::vector<std::string> history;
  void add(const std::string &line){
    history.push_back(line);
  }

  void print(){
    for (const auto &it : history) {
        std::cout << it << "\n";
    }
  }

  void print(int num){
    for (size_t i = history.size() - num; i < history.size(); --i) {
        std::cout << history[i] << "\n";
    }
  }

  void clear(){
    history.clear();
  }

  void remove(int index){
    history.erase(history.begin() + index);
  }

  const std::vector<std::string>& get_all(){
    return history;
  }

  void load_from_file(){
    const char* home = getenv("HOME");
    if (!home) return;
    
    std::string history_file = std::string(home) + "/.dax_history";
    read_history(history_file.c_str());
  }

  void save_to_file(){
    const char* home = getenv("HOME");
    if (!home) return;
    
    std::string history_file = std::string(home) + "/.dax_history";
    write_history(history_file.c_str());
  }
}