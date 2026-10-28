#ifndef HELPER_HPP
#define HELPER_HPP
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>

std::vector<std::string> extract_key_value(const std::string &token);

void print_vector(const std::vector<std::string> &vector);

void print_table(const std::unordered_map<std::string, std::string> &);

void print_prompt();
void report_command_launch_error(const std::string &command, int error);


namespace History{
  void add(const std::string &line);
  void print();
  void print(int num);
  void clear();
  void remove(int index);
  const std::vector<std::string>& get_all();
  void load_from_file();
  void save_to_file();
}
#endif
