#include "builtins_internal.hpp"
#include "helper.hpp"
#include "shell.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <readline/history.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
std::string join_arguments(const std::vector<std::string> &args, size_t first) {
  std::string result;
  for (size_t i = first; i < args.size(); ++i) {
    if (!result.empty()) result += ' ';
    result += args[i];
  }
  return result;
}

void print_history(size_t count = 0) {
  const auto &items = History::get_all();
  size_t first = count && count < items.size() ? items.size() - count : 0;
  for (size_t i = first; i < items.size(); ++i)
    std::cout << std::setw(5) << (history_base + static_cast<int>(i)) << "  "
              << items[i] << '\n';
}

bool parse_history_index(const std::string &text, long long &value) {
  char *end = nullptr;
  errno = 0;
  value = std::strtoll(text.c_str(), &end, 10);
  return !errno && end != text.c_str() && *end == '\0';
}

std::optional<size_t> find_history_entry(const std::string &selector,
                                         const std::deque<std::string> &items) {
  long long number = 0;
  if (parse_history_index(selector, number)) {
    const long long index = number < 0
        ? static_cast<long long>(items.size()) + number
        : number - static_cast<long long>(history_base);
    if (index < 0 || static_cast<unsigned long long>(index) >= items.size())
      return std::nullopt;
    return static_cast<size_t>(index);
  }
  for (size_t i = items.size(); i > 0; --i)
    if (items[i - 1].rfind(selector, 0) == 0) return i - 1;
  return std::nullopt;
}

std::string shell_quote(const std::string &value) {
  std::string result = "'";
  for (char ch : value) result += ch == '\'' ? "'\\''" : std::string(1, ch);
  return result + "'";
}

int edit_and_execute_history(const std::deque<std::string> &items,
                             size_t first, size_t last, bool reverse,
                             const std::string &editor) {
  std::vector<size_t> selected;
  for (size_t i = first; i <= last; ++i) selected.push_back(i);
  if (reverse) std::reverse(selected.begin(), selected.end());

  std::error_code error;
  const auto directory = std::filesystem::temp_directory_path(error);
  if (error) return 1;
  std::string pattern = (directory / "dax-fc-XXXXXX").string();
  std::vector<char> path_buffer(pattern.begin(), pattern.end());
  path_buffer.push_back('\0');
  const int fd = mkstemp(path_buffer.data());
  if (fd < 0) {
    std::cerr << "fc: cannot create temporary file: " << std::strerror(errno) << '\n';
    return 1;
  }
  close(fd);
  const std::string path(path_buffer.data());
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
      std::filesystem::remove(path, error);
      std::cerr << "fc: cannot write temporary file\n";
      return 1;
    }
    for (size_t index : selected) output << items[index] << '\n';
    if (!output) {
      std::filesystem::remove(path, error);
      std::cerr << "fc: cannot write temporary file\n";
      return 1;
    }
  }

  int editor_status = 0;
  if (editor != "-") {
    execute_line(editor + " " + shell_quote(path));
    editor_status = last_status();
  }
  if (editor_status != 0) {
    std::filesystem::remove(path, error);
    return editor_status;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    std::filesystem::remove(path, error);
    std::cerr << "fc: cannot read temporary file\n";
    return 1;
  }
  std::string command((std::istreambuf_iterator<char>(input)),
                      std::istreambuf_iterator<char>());
  std::filesystem::remove(path, error);
  while (!command.empty() && (command.back() == '\n' || command.back() == '\r'))
    command.pop_back();
  if (command.empty()) return 0;
  std::cout << command << '\n';
  History::add(command);
  execute_line(command);
  return last_status();
}
} // namespace

std::optional<int> run_history_builtin(const std::vector<std::string> &a) {
  if (a.empty()) return 0;
  const auto &c = a[0];
  if (c == "history") {
    if (a.size() == 1) {
      print_history();
      return 0;
    }
    const std::string &option = a[1];
    if (option == "-c" && a.size() == 2) {
      History::clear();
      return 0;
    }
    if (option == "-d" && a.size() == 3) {
      const std::string &range = a[2];
      const size_t dash = range.find('-', !range.empty() && range[0] == '-' ? 1 : 0);
      long long first = 0, last = 0;
      if (dash == std::string::npos) {
        if (!parse_history_index(range, first)) {
          std::cerr << "history: " << range << ": numeric argument required\n";
          return 2;
        }
        last = first;
      } else if (!parse_history_index(range.substr(0, dash), first) ||
                 !parse_history_index(range.substr(dash + 1), last)) {
        std::cerr << "history: " << range << ": numeric argument required\n";
        return 2;
      }
      const auto &items = History::get_all();
      const long long count = static_cast<long long>(items.size());
      auto to_index = [&](long long number) {
        return number < 0 ? count + number
                          : number - static_cast<long long>(history_base);
      };
      const long long begin = to_index(first), end = to_index(last);
      if (begin < 0 || end < begin || end >= count) {
        std::cerr << "history: " << range << ": history position out of range\n";
        return 1;
      }
      for (long long index = end; index >= begin; --index)
        History::remove(static_cast<int>(index));
      return 0;
    }
    if ((option == "-w" || option == "-r") && a.size() <= 3) {
      const std::string filename = a.size() == 3 ? a[2] : std::string();
      const bool ok = option == "-w" ? History::write_to_file(filename)
                                     : History::read_from_file(filename);
      if (!ok) {
        std::cerr << "history: " << (filename.empty() ? "history file" : filename)
                  << ": cannot " << (option == "-w" ? "write" : "read")
                  << " history file\n";
        return 1;
      }
      return 0;
    }
    if ((option == "-a" || option == "-n") && a.size() <= 3) {
      const std::string filename = a.size() == 3 ? a[2] : std::string();
      const bool ok = option == "-a" ? History::append_new_to_file(filename)
                                     : History::read_new_from_file(filename);
      if (!ok) {
        std::cerr << "history: " << (filename.empty() ? "history file" : filename)
                  << ": cannot " << (option == "-a" ? "append to" : "read")
                  << " history file\n";
        return 1;
      }
      return 0;
    }
    if (option == "-p" && a.size() >= 3) {
      for (size_t i = 2; i < a.size(); ++i) {
        char *expanded = nullptr;
        const int status = history_expand(a[i].c_str(), &expanded);
        if (status < 0) {
          std::cerr << (expanded ? expanded : "history: expansion failed") << '\n';
          std::free(expanded);
          return 1;
        }
        std::cout << (expanded ? expanded : a[i].c_str()) << '\n';
        std::free(expanded);
      }
      return 0;
    }
    if (option == "-s" && a.size() >= 3) {
      const auto &items = History::get_all();
      if (!items.empty()) History::remove(static_cast<int>(items.size() - 1));
      return History::add(join_arguments(a, 2)) ? 0 : 1;
    }
    if (!option.empty() && option[0] == '-') {
      std::cerr << "history: " << option << ": invalid option\n";
      return 2;
    }
    long long count = 0;
    if (!parse_history_index(option, count) || count < 0) {
      std::cerr << "history: " << option << ": numeric argument required\n";
      return 2;
    }
    if (a.size() != 2) {
      std::cerr << "history: too many arguments\n";
      return 2;
    }
    print_history(static_cast<size_t>(count));
    return 0;
  }
  if (c == "fc") {
    if (a.size() > 1 && a[1] == "-s") {
      const auto &items = History::get_all();
      if (items.empty()) return 1;
      size_t selected = items.size() - 1;
      if (shell_state().interactive_shell && selected) --selected;
      std::string command = items[selected];
      size_t argument = 2;
      std::string old_text, new_text;
      if (argument < a.size()) {
        const size_t eq = a[argument].find('=');
        if (eq != std::string::npos) {
          old_text = a[argument].substr(0, eq);
          new_text = a[argument].substr(eq + 1);
          if (old_text.empty()) {
            std::cerr << "fc: substitution pattern cannot be empty\n";
            return 2;
          }
          ++argument;
        }
      }
      if (argument < a.size()) {
        const auto index = find_history_entry(a[argument], items);
        if (!index) {
          std::cerr << "fc: " << a[argument] << ": history specification out of range\n";
          return 1;
        }
        selected = *index;
        command = items[selected];
        ++argument;
      }
      if (argument < a.size()) {
        std::cerr << "fc: too many arguments\n";
        return 2;
      }
      if (!old_text.empty()) {
        size_t at = 0;
        while ((at = command.find(old_text, at)) != std::string::npos) {
          command.replace(at, old_text.size(), new_text);
          at += new_text.size();
        }
      }
      History::add(command);
      execute_line(command);
      return last_status();
    }

    bool list = false, no_numbers = false, reverse = false;
    std::string editor;
    size_t index = 1;
    for (; index < a.size() && a[index].size() > 1 && a[index][0] == '-'; ++index) {
      long long numeric_selector = 0;
      if (parse_history_index(a[index], numeric_selector)) break;
      if (a[index] == "-e") {
        if (++index >= a.size()) {
          std::cerr << "fc: -e requires an editor\n";
          return 2;
        }
        editor = a[index];
        continue;
      }
      if (a[index] == "--") { ++index; break; }
      for (size_t flag = 1; flag < a[index].size(); ++flag) {
        if (a[index][flag] == 'l') list = true;
        else if (a[index][flag] == 'n') no_numbers = true;
        else if (a[index][flag] == 'r') reverse = true;
        else {
          std::cerr << "fc: invalid option -- '" << a[index][flag] << "'\n";
          return 2;
        }
      }
    }

    const auto &items = History::get_all();
    if (items.empty()) {
      std::cerr << "fc: no command history\n";
      return 1;
    }
    const bool editing = !list;
    long long first_selector = editing ? -2 : -16;
    long long last_selector = editing ? -2 : -1;
    std::string first_text, last_text;
    if (index < a.size()) first_text = a[index++];
    if (index < a.size()) last_text = a[index++];
    if (index < a.size()) {
      std::cerr << "fc: too many arguments\n";
      return 2;
    }

    auto resolve_selector = [&](const std::string &text, long long fallback)
        -> std::optional<size_t> {
      if (text.empty()) {
        const long long resolved = fallback < 0
            ? static_cast<long long>(items.size()) + fallback
            : fallback - history_base;
        if (resolved < 0 || static_cast<unsigned long long>(resolved) >= items.size())
          return std::nullopt;
        return static_cast<size_t>(resolved);
      }
      return find_history_entry(text, items);
    };
    const auto first = resolve_selector(first_text, first_selector);
    const auto last = resolve_selector(last_text, first_text.empty() ? last_selector :
                                      static_cast<long long>(first.value_or(0)) + history_base);
    if (!first || !last || *first > *last) {
      std::cerr << "fc: history specification out of range\n";
      return 1;
    }
    if (list) {
      for (size_t i = 0; i <= *last - *first; ++i) {
        const size_t selected = reverse ? *last - i : *first + i;
        if (!no_numbers)
          std::cout << std::setw(5) << (history_base + static_cast<int>(selected)) << "  ";
        std::cout << items[selected] << '\n';
      }
      return 0;
    }
    if (editor.empty()) {
      auto get_variable = [](const std::string &name) {
        const auto value = shell_state().vars.find(name);
        if (value != shell_state().vars.end() && !value->second.empty()) return value->second;
        const char *environment = std::getenv(name.c_str());
        return environment ? std::string(environment) : std::string();
      };
      editor = get_variable("FCEDIT");
      if (editor.empty()) editor = get_variable("EDITOR");
      if (editor.empty()) editor = "vi";
    }
    return edit_and_execute_history(items, *first, *last, reverse, editor);
  }
  return {};
}
