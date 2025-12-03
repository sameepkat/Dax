#ifndef HELPER_HPP
#define HELPER_HPP
#include <iostream>
#include <vector>
#include <unordered_map>

std::vector<std::string> extract_key_value(const std::string &token);

void print_vector(const std::vector<std::string> &vector);

void print_table(const std::unordered_map<std::string, std::string> &);


namespace History{
  void add(const std::string &line);
  void print();
  void print(int num);
  void clear();
  void remove(int index);
}
#endif