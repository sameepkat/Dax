#include "builtins_internal.hpp"
#include "helper.hpp"
#include "shell.hpp"
#include <iostream>
#include <string>
#include <vector>

std::optional<int> run_history_builtin(const std::vector<std::string> &a) {
  if (a.empty()) return 0;
  const auto &c = a[0];
  if (c == "history") {
    if (a.size() == 1)
      History::print();
    else if (a[1] == "-c")
      History::clear();
    else if (a[1] == "-d" && a.size() > 2) {
      try {
        History::remove(std::stoi(a[2]) - 1);
      } catch (...) {
        std::cerr << "history: numeric argument required\n";
        return 2;
      }
    } else {
      try {
        History::print(std::stoi(a[1]));
      } catch (...) {
        std::cerr << "history: numeric argument required\n";
        return 2;
      }
    }
    return 0;
  }
  if (c == "fc") {
    bool list = a.size() == 1 || (a.size() > 1 && a[1] == "-l");
    if (list) {
      const auto &items = History::get_all();
      size_t first = items.size() > 16 ? items.size() - 16 : 0;
      for (size_t i = first; i < items.size(); ++i)
        std::cout << (i + 1) << "\t" << items[i] << '\n';
      return 0;
    }
    if (a[1] == "-s") {
      const auto &items = History::get_all();
      if (items.empty()) return 1;
      size_t selected = items.size() - 1;
      if (items[selected].rfind("fc -s", 0) == 0 && selected) --selected;
      std::string command = items[selected];
      if (a.size() >= 3) {
        const size_t eq = a[2].find('=');
        if (eq == std::string::npos) return 2;
        const size_t at = command.find(a[2].substr(0, eq));
        if (at != std::string::npos) command.replace(at, eq, a[2].substr(eq + 1));
      }
      execute_line(command);
      return last_status();
    }
    std::cerr << "fc: editor mode is not supported; use fc -l or fc -s\n";
    return 2;
  }
  return {};
}
