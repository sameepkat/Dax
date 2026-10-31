#include "parser.hpp"
#include "arithmetic.hpp"
#include "builtins.hpp"
#include "builtins_internal.hpp"
#include "file_access.hpp"
#include "file_times.hpp"
#include "helper.hpp"
#include "janet_commands.hpp"
#include "shell_state.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cctype>
#include <cinttypes>
#include <climits>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <fcntl.h>
#include <fnmatch.h>
#include <map>
#include <optional>
#include <regex.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
class ProcessSubstitutionScope {
public:
  ProcessSubstitutionScope() : mark_(process_substitution_mark()) {}
  ~ProcessSubstitutionScope() { finish_process_substitutions(mark_); }
private:
  size_t mark_;
};

class ExtraDescriptorRestore {
public:
  explicit ExtraDescriptorRestore(const std::vector<Redirection> &redirections) {
    for (const auto &redirection : redirections) {
      if (redirection.descriptor >= 3) reserved_.insert(redirection.descriptor);
      if (redirection.type == ">&" || redirection.type == "<&") {
        char *end = nullptr;
        const long source = std::strtol(redirection.filename.c_str(), &end, 10);
        if (end != redirection.filename.c_str() && *end == '\0' &&
            source >= 3 && source <= INT_MAX)
          reserved_.insert(static_cast<int>(source));
      }
    }
  }
  ~ExtraDescriptorRestore() { restore(); }
  bool save(int descriptor) {
    if (descriptor < 3 || saved_.count(descriptor)) return true;
    int backup = fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
    if (backup < 0) {
      if (errno == EBADF) {
        saved_[descriptor] = -1;
        return true;
      }
      return false;
    }
    while (reserved_.count(backup)) {
      const int next = fcntl(backup, F_DUPFD_CLOEXEC, backup + 1);
      close(backup);
      if (next < 0) return false;
      backup = next;
    }
    saved_[descriptor] = backup;
    return true;
  }
  void release() {
    for (const auto &[descriptor, backup] : saved_) {
      (void)descriptor;
      if (backup >= 0) close(backup);
    }
    saved_.clear();
    active_ = false;
  }
  void restore() {
    if (!active_) return;
    for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) {
      if (it->second >= 0) {
        dup2(it->second, it->first);
        close(it->second);
      } else {
        close(it->first);
      }
    }
    saved_.clear();
    active_ = false;
  }
private:
  std::set<int> reserved_;
  std::map<int, int> saved_;
  bool active_ = true;
};

class DynamicDescriptorCleanup {
public:
  explicit DynamicDescriptorCleanup(bool enabled) : enabled_(enabled) {}
  ~DynamicDescriptorCleanup() {
    if (enabled_)
      for (const int descriptor : descriptors_) close(descriptor);
  }
  void add(int descriptor) { descriptors_.push_back(descriptor); }
  void forget(int descriptor) {
    descriptors_.erase(std::remove(descriptors_.begin(), descriptors_.end(), descriptor),
                       descriptors_.end());
  }
  void release() { enabled_ = false; }
private:
  bool enabled_;
  std::vector<int> descriptors_;
};

bool assign_associative_entries(const std::string &name,
                                const std::vector<std::string> &entries,
                                bool reset) {
  auto &state = shell_state();
  auto array = state.associative_arrays.find(name);
  if (array == state.associative_arrays.end()) return false;
  if (reset) array->second.clear();
  ProcessSubstitutionScope process_substitution_scope;
  if (entries.empty()) return true;
  const bool assignment_form = !entries.front().empty() && entries.front().front() == '[' &&
                               entries.front().find("]=") != std::string::npos;
  std::optional<std::string> pending_key;
  for (const auto &entry : entries) {
    std::vector<std::string> expanded = {std::string("\x09") + entry + "\x09"};
    begin_word_expansion();
    expand_words(expanded);
    if (!word_expansion_succeeded() || expanded.size() != 1) {
      std::cerr << "dax: associative array assignment expansion failed\n";
      return false;
    }
    const std::string &expanded_entry = expanded.front();
    if (assignment_form) {
      const size_t delimiter = expanded_entry.find("]=");
      if (expanded_entry.empty() || expanded_entry.front() != '[' || delimiter == std::string::npos) {
        std::cerr << "dax: invalid associative array assignment: " << expanded_entry << '\n';
        return false;
      }
      const std::string key = expanded_entry.substr(1, delimiter - 1);
      const std::string value = expanded_entry.substr(delimiter + 2);
      if (key.empty() || !set_shell_variable(name + "[" + key + "]", value)) {
        std::cerr << "dax: invalid associative array key\n";
        return false;
      }
    } else if (!pending_key) {
      if (expanded_entry.empty()) {
        std::cerr << "dax: associative array keys cannot be empty\n";
        return false;
      }
      pending_key = expanded_entry;
    } else {
      if (!set_shell_variable(name + "[" + *pending_key + "]", expanded_entry)) {
        std::cerr << "dax: invalid associative array key\n";
        return false;
      }
      pending_key.reset();
    }
  }
  if (pending_key && !set_shell_variable(name + "[" + *pending_key + "]", "")) {
    std::cerr << "dax: invalid associative array key\n";
    return false;
  }
  return true;
}

void set_pipeline_statuses(const std::vector<int> &statuses) {
  auto &pipeline_status = shell_state().arrays["PIPESTATUS"];
  pipeline_status.clear();
  shell_array_clear_set_indices("PIPESTATUS");
  for (int status : statuses) pipeline_status.push_back(std::to_string(status));
  shell_array_mark_all_set("PIPESTATUS");
}

void set_single_pipeline_status(int status) {
  set_pipeline_statuses({status});
}

class SinglePipelineStatusOnExit {
public:
  ~SinglePipelineStatusOnExit() {
    set_single_pipeline_status(last_status());
  }
};
}

void CommandNode::print(int indent) const {
  std::string padding(indent, ' ');
  std::cout << padding << "Command: ";
  for (const auto &arg : args) {
    std::cout << arg << " ";
  }
  for (const auto &red : redirections) {
    std::cout << "[" << red.type << " " << red.filename << "] ";
  }
  std::cout << "\n";
}

void ArrayAssignmentNode::print(int indent) const {
  std::cout << std::string(indent, ' ') << "Array assignment: " << name << '\n';
}

int ArrayAssignmentNode::execute() {
  SinglePipelineStatusOnExit pipeline_status_on_exit;
  ProcessSubstitutionScope process_substitution_scope;
  if (shell_variable_readonly(name)) {
    std::cerr << "dax: " << name << ": readonly variable\n";
    set_last_status(1);
    run_err_trap(1);
    return 1;
  }
  if (append && indexed) {
    std::cerr << "dax: array append cannot use an index\n";
    set_last_status(2);
    run_err_trap(2);
    return 2;
  }
  if (shell_state().associative_arrays.count(name)) {
    std::vector<std::string> entries = values;
    bool reset = !append;
    if (indexed) {
      if (values.size() != 1) {
        std::cerr << "dax: indexed associative assignment requires one value\n";
        set_last_status(1);
        run_err_trap(1);
        return 1;
      }
      entries = {"[" + index_expression + "]=" + values.front()};
      reset = false;
    }
    if (!assign_associative_entries(name, entries, reset)) {
      set_last_status(1);
      run_err_trap(1);
      return 1;
    }
    shell_state().unset_vars.erase(name);
    set_last_status(0);
    return 0;
  }
  long long resolved_index = 0;
  if (indexed) {
    std::string arithmetic_error;
    const auto evaluated_index = evaluate_arithmetic(index_expression, &arithmetic_error);
    if (!evaluated_index) {
      std::cerr << "dax: invalid array index '" << index_expression << "': "
                << arithmetic_error << '\n';
      set_last_status(1);
      run_err_trap(1);
      return 1;
    }
    resolved_index = *evaluated_index;
    if (resolved_index > 1000000 || resolved_index < -1000000) {
      std::cerr << "dax: array index out of range: " << resolved_index << '\n';
      set_last_status(1);
      run_err_trap(1);
      return 1;
    }
  }
  const auto raw_values = values;
  if (!indexed && name != "DIRSTACK") {
    std::vector<std::pair<std::string, std::string>> expanded_items;
    for (const auto &raw : raw_values) {
      std::vector<std::string> expanded = {std::string("\x02") + raw + "\x02"};
      begin_word_expansion();
      expand_words(expanded);
      if (!word_expansion_succeeded()) {
        set_last_status(1);
        run_err_trap(1);
        return 1;
      }
      for (auto &item : expanded)
        expanded_items.emplace_back(raw, std::move(item));
    }
    auto &state = shell_state();
    auto &array = state.arrays[name];
    if (!append) {
      array.clear();
      shell_array_clear_set_indices(name);
    }
    state.unset_vars.erase(name);
    size_t next_index = shell_array_index_extent(name);
    for (const auto &[raw, expanded_item] : expanded_items) {
      const bool has_subscript = !raw.empty() && raw.front() == '[' &&
                                 raw.find("]=") != std::string::npos;
      long long index = static_cast<long long>(next_index);
      std::string element = expanded_item;
      if (has_subscript) {
        const size_t delimiter = element.find("]=");
        if (element.empty() || element.front() != '[' || delimiter == std::string::npos) {
          std::cerr << "dax: invalid indexed array assignment: " << element << '\n';
          set_last_status(1);
          run_err_trap(1);
          return 1;
        }
        const std::string index_expression = element.substr(1, delimiter - 1);
        std::string arithmetic_error;
        const auto evaluated = evaluate_arithmetic(index_expression, &arithmetic_error);
        if (!evaluated) {
          std::cerr << "dax: invalid array index '" << index_expression << "': "
                    << arithmetic_error << '\n';
          set_last_status(1);
          run_err_trap(1);
          return 1;
        }
        index = *evaluated;
        if (index < 0) index += static_cast<long long>(shell_array_index_extent(name));
        element.erase(0, delimiter + 2);
      }
      if (index < 0 || index > 1000000 ||
          !set_shell_variable(name + "[" + std::to_string(index) + "]", element)) {
        if (index > 1000000)
          std::cerr << "dax: array index out of range: " << index << '\n';
        set_last_status(1);
        run_err_trap(1);
        return 1;
      }
      next_index = static_cast<size_t>(index) + 1;
    }
    set_last_status(0);
    return 0;
  }
  for (auto &value : values) value = std::string("\x02") + value + "\x02";
  begin_word_expansion();
  expand_words(values);
  if (!word_expansion_succeeded()) {
    set_last_status(1);
    run_err_trap(1);
    return 1;
  }
  auto &array = shell_state().arrays[name];
  shell_state().unset_vars.erase(name);
  if (indexed && resolved_index < 0) {
    const size_t extent = shell_array_index_extent(name);
    if (static_cast<unsigned long long>(0) - static_cast<unsigned long long>(resolved_index) > extent) {
      std::cerr << "dax: array index out of range: " << resolved_index << '\n';
      set_last_status(1);
      return 1;
    }
    resolved_index += static_cast<long long>(extent);
  }
  auto &state = shell_state();
  if (name == "DIRSTACK" &&
      !state.disabled_special_parameters.count("DIRSTACK")) {
    // Bash lets DIRSTACK assignments replace existing entries, but they do
    // not change the current directory or grow/shrink the directory stack.
    if (!append) {
      const size_t first = indexed ? static_cast<size_t>(resolved_index) : 0;
      for (size_t offset = 0; offset < values.size(); ++offset) {
        const size_t index = first + offset;
        if (index == 0) continue;
        const size_t directory_index = index - 1;
        if (directory_index >= state.dirs.size()) continue;
        state.dirs[directory_index] = values[offset];
        state.arrays["DIRSTACK"][index] = values[offset];
        shell_array_mark_index_set("DIRSTACK", index);
      }
    }
    state.unset_vars.erase(name);
    set_last_status(0);
    return 0;
  }
  if (append) {
    const size_t first = array.size();
    if (!shell_state().array_set_indices.count(name)) shell_array_mark_all_set(name);
    array.insert(array.end(), values.begin(), values.end());
    if (!values.empty()) {
      for (size_t index = first; index < array.size(); ++index) shell_array_mark_index_set(name, index);
    }
  } else if (indexed) {
    if (!shell_state().array_set_indices.count(name)) shell_array_mark_all_set(name);
    if (array.size() <= static_cast<size_t>(resolved_index)) array.resize(static_cast<size_t>(resolved_index) + 1);
    array[static_cast<size_t>(resolved_index)] = values.empty() ? std::string() : values.front();
    shell_array_mark_index_set(name, static_cast<size_t>(resolved_index));
  } else {
    array = values;
    shell_array_mark_all_set(name);
  }
  set_last_status(0);
  return 0;
}

void ArrayDeclarationNode::print(int indent) const {
  std::cout << std::string(indent, ' ')
            << (associative ? "Associative declaration: " : "Indexed declaration: ")
            << name << '\n';
}

int ArrayDeclarationNode::execute() {
  SinglePipelineStatusOnExit pipeline_status_on_exit;
  const uint64_t err_generation = shell_state().err_trap_generation;
  GlobalDeclarationView global_view(shell_state(), name,
                                    flags.find('g') != std::string::npos);
  auto &state = shell_state();
  if (shell_variable_readonly(name)) {
    std::cerr << command << ": " << name << ": readonly variable\n";
    set_last_status(1);
    if (err_generation == shell_state().err_trap_generation) run_err_trap(1);
    return 1;
  }
  const bool make_readonly = command == "readonly" || flags.find('r') != std::string::npos;
  const bool make_integer = flags.find('i') != std::string::npos;
  std::string declaration_flags = flags;
  declaration_flags.erase(
      std::remove_if(declaration_flags.begin(), declaration_flags.end(), [](char flag) {
        return flag == 'r' || flag == 'i' || flag == 'l' || flag == 'u';
      }), declaration_flags.end());
  if (declaration_flags == "-") declaration_flags.clear();
  const std::string declaration_command = command == "readonly" ? "declare" : command;
  std::vector<std::string> declaration = {declaration_command};
  if (!declaration_flags.empty()) declaration.push_back(declaration_flags);
  declaration.push_back(name);
  auto status = handle_builtin(declaration);
  if (!status || *status != 0) {
    const int result = status.value_or(1);
    set_last_status(result);
    if (err_generation == shell_state().err_trap_generation) run_err_trap(result);
    return result;
  }
  if (associative) {
    if (!assign_associative_entries(name, entries, !append)) {
      set_last_status(1);
      if (err_generation == shell_state().err_trap_generation) run_err_trap(1);
      return 1;
    }
  } else {
    ArrayAssignmentNode assignment;
    assignment.name = name;
    assignment.values = entries;
    assignment.append = append;
    const int result = assignment.execute();
    if (result != 0) return result;
  }
  if (make_integer) state.integer_vars.insert(name);
  for (size_t index = 1; index < flags.size(); ++index) {
    if (flags[index] == 'l') {
      state.lowercase_vars.insert(name);
      state.uppercase_vars.erase(name);
    } else if (flags[index] == 'u') {
      state.uppercase_vars.insert(name);
      state.lowercase_vars.erase(name);
    }
  }
  if (make_readonly) {
    state.readonly_vars[name] = "1";
  }
  set_last_status(0);
  return 0;
}

void ConditionalNode::print(int indent) const {
  std::cout << std::string(indent, ' ') << "Conditional: [[ ... ]]\n";
}

int ConditionalNode::execute() {
  ProcessSubstitutionScope process_substitution_scope;
  run_debug_trap(raw_command);
  std::vector<std::string> words = expression;
  bool expansion_failed = false;
  auto expand_operand = [&](size_t index, bool regex_pattern = false,
                            bool glob_pattern = false) -> std::optional<std::string> {
    if (index >= expression.size()) return std::string();
    std::string raw = expression[index];
    if (regex_pattern || glob_pattern) {
      // Keep quote boundaries through expansion so metacharacters produced by
      // a quoted parameter or command substitution remain literal too.
      std::string marked_pattern;
      bool single = false, quoted = false;
      for (size_t i = 0; i < raw.size(); ++i) {
        const char ch = raw[i];
        if (ch == '\x01') {
          single = !single;
          if (!single) marked_pattern += '\x10';
          marked_pattern += ch;
          if (single) marked_pattern += '\x0f';
          continue;
        }
        if (ch == '\x02') {
          quoted = !quoted;
          if (!quoted) marked_pattern += '\x10';
          marked_pattern += ch;
          if (quoted) marked_pattern += '\x0f';
          continue;
        }
        if (ch == '\x0b' && i + 1 < raw.size()) {
          const char escaped = raw[++i];
          if (!single && !quoted) marked_pattern += '\x0f';
          marked_pattern += '\x0b';
          marked_pattern += escaped;
          if (!single && !quoted) marked_pattern += '\x10';
          continue;
        }
        marked_pattern += ch;
      }
      raw = std::move(marked_pattern);
    }
    std::vector<std::string> operand = {std::string("\x09") + raw + "\x09"};
    begin_word_expansion();
    expand_words(operand);
    if (!word_expansion_succeeded()) {
      expansion_failed = true;
      return {};
    }
    if (operand.empty()) return std::string();
    std::string joined;
    bool literal_pattern = false;
    for (const auto &part : operand) {
      if (!joined.empty()) joined += ' ';
      for (size_t i = 0; i < part.size(); ++i) {
        const char ch = part[i];
        if (ch == '\x0f') { literal_pattern = true; continue; }
        if (ch == '\x10') { literal_pattern = false; continue; }
        if (literal_pattern && regex_pattern &&
            std::string(".^$|()[]{}*+?\\").find(ch) != std::string::npos)
          joined += '\\';
        if (literal_pattern && glob_pattern &&
            (std::string("*?[]\\-!^").find(ch) != std::string::npos ||
            (glob_pattern && std::string("@?*+!").find(ch) != std::string::npos &&
              i + 1 < part.size() && part[i + 1] == '(')))
          joined += '\x0e';
        joined += ch;
      }
    }
    return joined;
  };

  size_t pos = 0;
  auto syntax_error = []() {
    std::cerr << "dax: syntax error in [[ ]] conditional expression\n";
    return 2;
  };
  std::function<int(bool)> parse_or;
  std::function<int(bool)> parse_and;
  std::function<int(bool)> parse_unary;
  std::function<int(bool)> parse_primary;
  parse_primary = [&](bool evaluate) -> int {
    if (pos >= words.size()) return syntax_error();
    if (words[pos] == "(") {
      ++pos;
      const int value = parse_or(evaluate);
      if (pos >= words.size() || words[pos] != ")") return syntax_error();
      ++pos;
      return value;
    }
    const size_t first_index = pos;
    const std::string first_word = words[pos++];
    static const std::vector<std::string> unary_ops = {
        "-n", "-z", "-e", "-a", "-f", "-d", "-r", "-w", "-x",
        "-s", "-b", "-c", "-p", "-S", "-L", "-h", "-t", "-v",
        "-R", "-o", "-O", "-G", "-g", "-k", "-u", "-N"};
    if (std::find(unary_ops.begin(), unary_ops.end(), first_word) != unary_ops.end()) {
      if (pos >= words.size()) return syntax_error();
      const size_t value_index = pos++;
      if (!evaluate) return 1;
      auto expanded = expand_operand(value_index);
      if (!expanded) return 2;
      const std::string &value = *expanded;
      const std::string variable_value = first_word == "-v"
          ? shell_resolve_nameref(value) : value;
      struct stat info {};
      const bool exists = stat(value.c_str(), &info) == 0;
      bool result = false;
      if (first_word == "-R") {
        const size_t bracket = value.find('[');
        result = bracket == std::string::npos &&
                 shell_state().namerefs.count(value) != 0;
      }
      else if (first_word == "-o") {
        const std::string enabled_options = shell_enabled_options();
        size_t begin = 0;
        while (begin <= enabled_options.size()) {
          const size_t end = enabled_options.find(':', begin);
          const size_t length = end == std::string::npos
                                    ? enabled_options.size() - begin
                                    : end - begin;
          if (enabled_options.compare(begin, length, value) == 0 &&
              length == value.size()) {
            result = true;
            break;
          }
          if (end == std::string::npos) break;
          begin = end + 1;
        }
      }
      else if (first_word == "-n") result = !value.empty();
      else if (first_word == "-z") result = value.empty();
      else if (first_word == "-e" || first_word == "-a") result = access(value.c_str(), F_OK) == 0;
      else if (first_word == "-f") result = exists && S_ISREG(info.st_mode);
      else if (first_word == "-d") result = exists && S_ISDIR(info.st_mode);
      else if (first_word == "-r") result = dax_effective_access(value.c_str(), R_OK);
      else if (first_word == "-w") result = dax_effective_access(value.c_str(), W_OK);
      else if (first_word == "-x") result = dax_effective_access(value.c_str(), X_OK);
      else if (first_word == "-s") result = exists && info.st_size > 0;
      else if (first_word == "-O") result = exists && info.st_uid == geteuid();
      else if (first_word == "-G") result = exists && info.st_gid == getegid();
      else if (first_word == "-g") result = exists && (info.st_mode & S_ISGID) != 0;
      else if (first_word == "-k") result = exists && (info.st_mode & S_ISVTX) != 0;
      else if (first_word == "-u") result = exists && (info.st_mode & S_ISUID) != 0;
      else if (first_word == "-N") result = exists && dax_file_modified_since_read(info);
      else if (first_word == "-b") result = exists && S_ISBLK(info.st_mode);
      else if (first_word == "-c") result = exists && S_ISCHR(info.st_mode);
      else if (first_word == "-p") result = exists && S_ISFIFO(info.st_mode);
      else if (first_word == "-S") result = exists && S_ISSOCK(info.st_mode);
      else if (first_word == "-L" || first_word == "-h") result = lstat(value.c_str(), &info) == 0 && S_ISLNK(info.st_mode);
      else if (first_word == "-t") {
        char *end = nullptr;
        long fd = std::strtol(value.c_str(), &end, 10);
        result = end != value.c_str() && *end == '\0' && fd >= 0 && fd <= INT_MAX && isatty(static_cast<int>(fd));
      } else {
        auto &state = shell_state();
        const size_t bracket = variable_value.find('[');
        if (variable_value.empty()) {
          result = false;
        } else if (bracket != std::string::npos && variable_value.back() == ']') {
          const std::string array_name = variable_value.substr(0, bracket);
          const std::string index_text = variable_value.substr(bracket + 1, variable_value.size() - bracket - 2);
          auto associative = state.associative_arrays.find(array_name);
          if (associative != state.associative_arrays.end()) {
            result = associative->second.count(index_text) != 0;
          } else {
            auto indexed = state.arrays.find(array_name);
            if (indexed != state.arrays.end() &&
                (index_text == "@" || index_text == "*")) {
              const auto indices = state.array_set_indices.find(array_name);
              result = indices == state.array_set_indices.end()
                           ? !indexed->second.empty()
                           : !indices->second.empty();
            } else {
              std::string arithmetic_error;
              auto index = evaluate_arithmetic(index_text, &arithmetic_error);
              if (indexed != state.arrays.end() && index) {
                long long resolved = *index;
                if (resolved < 0) resolved += static_cast<long long>(shell_array_index_extent(array_name));
                result = resolved >= 0 && static_cast<size_t>(resolved) < indexed->second.size() &&
                         shell_array_index_is_set(array_name, static_cast<size_t>(resolved));
              }
            }
          }
        } else {
          if (!state.unset_vars.count(variable_value)) {
            const auto indexed = state.arrays.find(variable_value);
            const auto associative = state.associative_arrays.find(variable_value);
            if (indexed != state.arrays.end()) {
              const auto indices = state.array_set_indices.find(variable_value);
              result = indices != state.array_set_indices.end()
                           ? !indices->second.empty()
                           : !indexed->second.empty();
            }
            else if (associative != state.associative_arrays.end())
              result = !associative->second.empty();
            else
              result = state.vars.count(variable_value) ||
                       std::getenv(variable_value.c_str()) != nullptr;
          }
        }
      }
      return result ? 0 : 1;
    }
    if (pos >= words.size()) {
      if (!evaluate) return 1;
      auto first = expand_operand(first_index);
      if (!first) return 2;
      return first->empty() ? 1 : 0;
    }
    const std::string op = words[pos];
    static const std::vector<std::string> binary_ops = {
        "=", "==", "!=", "<", ">", "=~", "-eq", "-ne", "-lt", "-le", "-gt", "-ge",
        "-nt", "-ot", "-ef"};
    if (std::find(binary_ops.begin(), binary_ops.end(), op) == binary_ops.end()) {
      if (!evaluate) return 1;
      auto first = expand_operand(first_index);
      if (!first) return 2;
      return first->empty() ? 1 : 0;
    }
    ++pos;
    if (pos >= words.size()) return syntax_error();
    const size_t right_index = pos;
    ++pos;
    if (!evaluate) return 1;
    auto expanded_first = expand_operand(first_index);
    const bool glob_comparison = op == "=" || op == "==" || op == "!=";
    auto expanded_right = expand_operand(right_index, op == "=~", glob_comparison);
    if (!expanded_first || !expanded_right) return 2;
    const std::string &first = *expanded_first;
    std::string right = *expanded_right;
    std::string glob_right;
    bool has_active_pattern = false;
    for (size_t i = 0; i < right.size(); ++i) {
      const char ch = right[i];
      if (ch == '\x0e' && i + 1 < right.size()) {
        glob_right += '\\';
        glob_right += right[++i];
        continue;
      }
      if (ch == '*' || ch == '?' || ch == '[') has_active_pattern = true;
      if (std::string("@?*+!").find(ch) != std::string::npos &&
          i + 1 < right.size() && right[i + 1] == '(')
        has_active_pattern = true;
      glob_right += ch;
    }
    right.erase(std::remove(right.begin(), right.end(), '\x0e'), right.end());
    bool result = false;
    if (op == "-nt" || op == "-ot" || op == "-ef") {
      struct stat left_info {}, right_info {};
      const bool left_exists = stat(first.c_str(), &left_info) == 0;
      const bool right_exists = stat(right.c_str(), &right_info) == 0;
      if (op == "-ef") {
        result = left_exists && right_exists && left_info.st_dev == right_info.st_dev &&
                 left_info.st_ino == right_info.st_ino;
      } else if (op == "-nt") {
        result = left_exists &&
                 (!right_exists || dax_compare_file_mtime(left_info, right_info) > 0);
      } else {
        result = right_exists &&
                 (!left_exists || dax_compare_file_mtime(left_info, right_info) < 0);
      }
    } else if (op == "=" || op == "==") {
      result = (has_active_pattern || shell_state().nocasematch)
                   ? fnmatch(glob_right.c_str(), first.c_str(),
                             FNM_EXTMATCH |
                             (shell_state().nocasematch ? FNM_CASEFOLD : 0)) == 0
                   : first == right;
    } else if (op == "!=") {
      result = (has_active_pattern || shell_state().nocasematch)
                   ? fnmatch(glob_right.c_str(), first.c_str(),
                             FNM_EXTMATCH |
                             (shell_state().nocasematch ? FNM_CASEFOLD : 0)) != 0
                   : first != right;
    } else if (op == "<") result = std::strcoll(first.c_str(), right.c_str()) < 0;
    else if (op == ">") result = std::strcoll(first.c_str(), right.c_str()) > 0;
    else if (op == "=~") {
      std::string regex_source = right;
      auto &shell = shell_state();
      regex_t regex{};
      const int regex_flags = REG_EXTENDED | (shell.nocasematch ? REG_ICASE : 0);
      shell.arrays.erase("BASH_REMATCH");
      shell.array_set_indices.erase("BASH_REMATCH");
      shell.vars.erase("BASH_REMATCH");
      shell.unset_vars.erase("BASH_REMATCH");
      shell.associative_arrays.erase("BASH_REMATCH");
      shell.arrays["BASH_REMATCH"].clear();
      shell_array_mark_all_set("BASH_REMATCH");
      if (regcomp(&regex, regex_source.c_str(), regex_flags) != 0) {
        std::cerr << "dax: invalid regular expression in [[ =~ ]]: " << regex_source << '\n';
        return 2;
      }
      std::vector<regmatch_t> matches(regex.re_nsub + 1);
      const int match_status = regexec(&regex, first.c_str(), matches.size(), matches.data(), 0);
      regfree(&regex);
      if (match_status == 0) {
        result = true;
        auto &captures = shell.arrays["BASH_REMATCH"];
        for (size_t index = 0; index < matches.size(); ++index) {
          const auto &capture = matches[index];
          if (captures.size() <= index) captures.resize(index + 1);
          if (capture.rm_so < 0 || capture.rm_eo < capture.rm_so) {
            captures[index].clear();
          } else {
            captures[index] = first.substr(static_cast<size_t>(capture.rm_so),
                                           static_cast<size_t>(capture.rm_eo - capture.rm_so));
          }
          shell_array_mark_index_set("BASH_REMATCH", index);
        }
      } else if (match_status != REG_NOMATCH) {
        std::cerr << "dax: failed to evaluate regular expression in [[ =~ ]]\n";
        return 2;
      }
    } else {
      std::string arithmetic_error;
      const auto evaluate_conditional_integer = [&](const std::string &operand) {
        return operand.empty()
                   ? std::optional<long long>(0)
                   : evaluate_arithmetic(operand, &arithmetic_error);
      };
      auto left_value = evaluate_conditional_integer(first);
      auto right_value = evaluate_conditional_integer(right);
      if (!left_value || !right_value) return syntax_error();
      const long long left_number = *left_value, right_number = *right_value;
      if (op == "-eq") result = left_number == right_number;
      else if (op == "-ne") result = left_number != right_number;
      else if (op == "-lt") result = left_number < right_number;
      else if (op == "-le") result = left_number <= right_number;
      else if (op == "-gt") result = left_number > right_number;
      else result = left_number >= right_number;
    }
    return result ? 0 : 1;
  };
  parse_unary = [&](bool evaluate) -> int {
    if (pos < words.size() && words[pos] == "!") {
      ++pos;
      const int result = parse_unary(evaluate);
      if (!evaluate && result <= 1) return 1;
      return result <= 1 ? 1 - result : result;
    }
    return parse_primary(evaluate);
  };
  parse_and = [&](bool evaluate) -> int {
    int result = parse_unary(evaluate);
    while (pos < words.size() && words[pos] == "&&") {
      ++pos;
      const int right = parse_unary(evaluate && result == 0);
      if (result > 1 || right > 1) return result > 1 ? result : right;
      if (!evaluate) { result = 1; continue; }
      if (right > 1) result = right;
      else result = result == 0 && right == 0 ? 0 : 1;
    }
    return result;
  };
  parse_or = [&](bool evaluate) -> int {
    int result = parse_and(evaluate);
    while (pos < words.size() && words[pos] == "||") {
      ++pos;
      const int right = parse_and(evaluate && result != 0);
      if (result > 1 || right > 1) return result > 1 ? result : right;
      if (!evaluate) { result = 1; continue; }
      if (right > 1) result = right;
      else result = result == 0 || right == 0 ? 0 : 1;
    }
    return result;
  };

  const uint64_t err_generation = shell_state().err_trap_generation;
  int status = parse_or(true);
  if (expansion_failed) status = 1;
  if (pos != words.size()) status = syntax_error();
  set_single_pipeline_status(status);
  set_last_status(status);
  if (err_generation == shell_state().err_trap_generation) run_err_trap(status);
  return status;
}

void ArithmeticCommandNode::print(int indent) const {
  std::cout << std::string(indent, ' ') << "Arithmetic command: (( " << expression << " ))\n";
}

int ArithmeticCommandNode::execute() {
  run_debug_trap(raw_command);
  std::string error;
  auto value = evaluate_arithmetic(expression, &error);
  if (!value) {
    std::cerr << "dax: arithmetic expression: " << error << '\n';
    set_single_pipeline_status(1);
    set_last_status(1);
    run_err_trap(1);
    return 1;
  }
  const int status = *value == 0 ? 1 : 0;
  const uint64_t err_generation = shell_state().err_trap_generation;
  set_single_pipeline_status(status);
  set_last_status(status);
  if (err_generation == shell_state().err_trap_generation) run_err_trap(status);
  return status;
}

void TimeNode::print(int indent) const {
  std::cout << std::string(indent, ' ') << "Time:\n";
  if (command) command->print(indent + 2);
}

int TimeNode::execute() {
  const auto wall_start = std::chrono::steady_clock::now();
  rusage self_before{}, self_after{}, children_before{}, children_after{};
  getrusage(RUSAGE_SELF, &self_before);
  getrusage(RUSAGE_CHILDREN, &children_before);
  const int status = command ? command->execute() : 0;
  getrusage(RUSAGE_CHILDREN, &children_after);
  getrusage(RUSAGE_SELF, &self_after);
  const auto wall_end = std::chrono::steady_clock::now();
  const double real = std::chrono::duration<double>(wall_end - wall_start).count();
  auto timeval_seconds = [](const timeval &value) {
    return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1000000.0;
  };
  const double user = std::max(0.0,
      timeval_seconds(self_after.ru_utime) - timeval_seconds(self_before.ru_utime) +
      timeval_seconds(children_after.ru_utime) - timeval_seconds(children_before.ru_utime));
  const double system = std::max(0.0,
      timeval_seconds(self_after.ru_stime) - timeval_seconds(self_before.ru_stime) +
      timeval_seconds(children_after.ru_stime) - timeval_seconds(children_before.ru_stime));
  const auto format_time_value = [](double value, int precision) {
    std::ostringstream formatted;
    formatted << std::fixed << std::setprecision(precision) << value;
    std::string result = formatted.str();
    const lconv *numeric_locale = std::localeconv();
    if (numeric_locale && numeric_locale->decimal_point &&
        std::string(numeric_locale->decimal_point) != ".") {
      const size_t point = result.find('.');
      if (point != std::string::npos)
        result.replace(point, 1, numeric_locale->decimal_point);
    }
    return result;
  };
  if (portable) {
    std::cerr << "real " << format_time_value(real, 2) << '\n'
              << "user " << format_time_value(user, 2) << '\n'
              << "sys " << format_time_value(system, 2) << '\n';
    set_last_status(status);
    return status;
  }

  auto &state = shell_state();
  auto format_it = state.vars.find("TIMEFORMAT");
  const char *environment_format = std::getenv("TIMEFORMAT");
  const std::string format = format_it != state.vars.end() ? format_it->second
      : environment_format ? environment_format
      : "\nreal\t%3lR\nuser\t%3lU\nsys\t%3lS\n";
  if (!format.empty()) {
    std::ostringstream output;
    bool valid_format = true;
    for (size_t i = 0; i < format.size(); ++i) {
      if (format[i] != '%' || i + 1 >= format.size()) {
        output << format[i];
        continue;
      }
      if (format[i + 1] == '%') { output << '%'; ++i; continue; }
      size_t cursor = i + 1;
      if (format[cursor] == 'P') {
        const double percentage = real > 0 ? 100.0 * (user + system) / real : 0.0;
        output << format_time_value(percentage, 2);
        i = cursor;
        continue;
      }
      int precision = 3;
      if (std::isdigit(static_cast<unsigned char>(format[cursor])))
        precision = std::min(6, format[cursor++] - '0');
      bool long_format = false;
      if (cursor < format.size() && format[cursor] == 'l') { long_format = true; ++cursor; }
      if (cursor >= format.size() || std::string("RUES").find(format[cursor]) == std::string::npos) {
        const char invalid_conversion = cursor < format.size() ? format[cursor] : '\0';
        if (invalid_conversion == '\0')
          std::cerr << "dax: TIMEFORMAT: incomplete time format conversion\n";
        else
          std::cerr << "dax: TIMEFORMAT: '" << invalid_conversion
                    << "': invalid format character\n";
        valid_format = false;
        break;
      }
      const char conversion = format[cursor];
      const double value = conversion == 'R' || conversion == 'E' ? real : conversion == 'U' ? user
                           : conversion == 'S' ? system
                           : real > 0 ? 100.0 * (user + system) / real : 0.0;
      if (long_format) {
        const long long minutes = static_cast<long long>(value / 60.0);
        output << minutes << 'm'
               << format_time_value(std::max(0.0, value - minutes * 60.0), precision)
               << 's';
      } else {
        output << format_time_value(value, precision);
      }
      i = cursor;
    }
    if (valid_format) {
      std::string rendered = output.str();
      if (rendered.empty() || rendered.back() != '\n') rendered += '\n';
      std::cerr << rendered;
    }
  }
  set_last_status(status);
  return status;
}

void CoprocNode::print(int indent) const {
  std::cout << std::string(indent, ' ') << "Coproc " << name << ":\n";
  if (command) command->print(indent + 2);
}

int CoprocNode::execute() {
  if (!command) return 2;
  auto &state = shell_state();
  auto previous = state.arrays.find(name);
  if (previous != state.arrays.end()) {
    for (const auto &fd_text : previous->second) {
      char *end = nullptr;
      const long fd = std::strtol(fd_text.c_str(), &end, 10);
      if (end != fd_text.c_str() && *end == '\0' && fd > STDERR_FILENO)
        close(static_cast<int>(fd));
    }
  }
  int parent_to_child[2] = {-1, -1}, child_to_parent[2] = {-1, -1};
  if (pipe(parent_to_child) < 0 || pipe(child_to_parent) < 0) {
    perror("coproc pipe");
    if (parent_to_child[0] >= 0) close(parent_to_child[0]);
    if (parent_to_child[1] >= 0) close(parent_to_child[1]);
    return 1;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    perror("coproc fork");
    close(parent_to_child[0]); close(parent_to_child[1]);
    close(child_to_parent[0]); close(child_to_parent[1]);
    return 1;
  }
  if (pid == 0) {
    state.err_trap_suppressed = state.err_trap_suppressed || !state.errtrace;
    enter_subshell_environment();
    setpgid(0, 0);
    signal(SIGINT, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    if (dup2(parent_to_child[0], STDIN_FILENO) < 0 ||
        dup2(child_to_parent[1], STDOUT_FILENO) < 0)
      _exit(126);
    close(parent_to_child[0]); close(parent_to_child[1]);
    close(child_to_parent[0]); close(child_to_parent[1]);
    const int result = command->execute();
    run_exit_trap();
    std::cout.flush();
    std::cerr.flush();
    _exit(result & 255);
  }
  setpgid(pid, pid);
  close(parent_to_child[0]);
  close(child_to_parent[1]);
  fcntl(parent_to_child[1], F_SETFD, FD_CLOEXEC);
  fcntl(child_to_parent[0], F_SETFD, FD_CLOEXEC);
  state.arrays[name] = {std::to_string(child_to_parent[0]),
                        std::to_string(parent_to_child[1])};
  shell_array_mark_all_set(name);
  const std::string pid_name = name + "_PID";
  state.vars[pid_name] = std::to_string(pid);
  const int id = state.jobs.empty() ? 1 : state.jobs.back().id + 1;
  state.jobs.push_back({id, static_cast<int>(pid), "coproc " + name, false});
  state.last_background_pid = static_cast<int>(pid);
  set_last_status(0);
  return 0;
}

void PipeNode::print(int indent) const {
  std::string padding(indent, ' ');
  std::cout << padding << (stderr_to_pipe ? "Pipe with stderr:\n" : "Pipe:\n");
  if (left)
    left->print(indent + 2);
  if (right)
    right->print(indent + 2);
}

int CommandNode::execute() {
  ProcessSubstitutionScope process_substitution_scope;
  const uint64_t err_generation = shell_state().err_trap_generation;
  const int status = execute_command();
  set_single_pipeline_status(status);
  set_last_status(status);
  if (err_generation == shell_state().err_trap_generation) run_err_trap(status);
  return status;
}

int CommandNode::execute_command() {
  if (!arg_locations.empty()) shell_state().current_lineno = arg_locations.front().first;
  else if (!redirections.empty()) shell_state().current_lineno = redirections.front().line;
  if (!shell_state().running_err_trap && !shell_state().running_debug_trap &&
      !raw_command.empty())
    shell_state().bash_command = raw_command;
  run_debug_trap(raw_command);
  if (args.empty() && redirections.empty())
    return 0;
  std::vector<std::string> expanded = args;
  begin_word_expansion();
  expand_words(expanded);
  if (!word_expansion_succeeded()) {
    shell_state().exit_warning_pending = false;
    const size_t index = word_expansion_error_index();
    if (index < arg_locations.size())
      std::cerr << "dax: line " << arg_locations[index].first << ", column "
                << arg_locations[index].second << ": word expansion failed\n";
    return 1;
  }
  if (!expanded.empty()) shell_state().last_argument = expanded.back();
  trace_shell_command(expanded);
  auto assignment_equal = [](const std::string &word) {
    const size_t equal = word.find('=');
    if (equal == std::string::npos || equal == 0 ||
        !(std::isalpha(static_cast<unsigned char>(word[0])) || word[0] == '_'))
      return std::string::npos;
    for (size_t i = 1; i < equal; ++i)
      if (!(std::isalnum(static_cast<unsigned char>(word[i])) || word[i] == '_'))
        return std::string::npos;
    return equal;
  };
  size_t assignment_count = 0;
  while (assignment_count < expanded.size() &&
         assignment_equal(expanded[assignment_count]) != std::string::npos)
    ++assignment_count;
  const bool assignment_only = !expanded.empty() && assignment_count == expanded.size();
  int no_command_status = 0;
  if (assignment_only && redirections.empty()) {
    shell_state().exit_warning_pending = false;
    for (const auto &assignment : expanded) {
      const auto p = assignment_equal(assignment);
      if (!set_shell_variable(assignment.substr(0, p),
                              assignment.substr(p + 1)))
        return 1;
    }
    return word_expansion_ran_substitution() ? word_expansion_substitution_status() : 0;
  }
  if (assignment_only) {
    shell_state().exit_warning_pending = false;
    for (const auto &assignment : expanded) {
      const auto p = assignment_equal(assignment);
      if (!set_shell_variable(assignment.substr(0, p),
                              assignment.substr(p + 1)))
        return 1;
    }
    no_command_status = word_expansion_ran_substitution()
                            ? word_expansion_substitution_status() : 0;
    expanded.clear();
    assignment_count = 0;
  }
  std::vector<std::pair<std::string, std::optional<std::string>>>
      saved_environment;
  std::vector<size_t> assignment_indices;
  for (size_t i = 0; i < expanded.size(); ++i) {
    if (i < assignment_count ||
        (shell_state().keyword_assignments && assignment_equal(expanded[i]) != std::string::npos))
      assignment_indices.push_back(i);
  }
  std::unordered_set<std::string> saved_names;
  std::unordered_set<std::string> persistent_environment_names;
  std::vector<bool> is_assignment(expanded.size(), false);
  auto restore_environment = [&]() {
    for (const auto &[name, value] : saved_environment) {
      if (persistent_environment_names.count(name)) continue;
      if (value)
        setenv(name.c_str(), value->c_str(), 1);
      else
        unsetenv(name.c_str());
    }
  };
  const bool posix_special_builtin = shell_state().posix_mode &&
      assignment_count < expanded.size() &&
      shell_is_posix_special_builtin(expanded[assignment_count]);
  auto mark_posix_special_builtin_failure = [&](int status) {
    if (posix_special_builtin && !shell_state().interactive_shell) {
      shell_state().exit_value = status;
      shell_state().exiting = true;
    }
  };
  for (const size_t index : assignment_indices) {
    is_assignment[index] = true;
    const auto p = assignment_equal(expanded[index]);
    std::string name = expanded[index].substr(0, p);
    const std::string value = expanded[index].substr(p + 1);
    if (shell_variable_readonly(name)) {
      std::cerr << name << ": readonly variable\n";
      restore_environment();
      mark_posix_special_builtin_failure(1);
      return 1;
    }
    if (saved_names.insert(name).second) {
      const char *old = getenv(name.c_str());
      saved_environment.push_back(
          {name, old ? std::optional<std::string>(old) : std::nullopt});
    }
    if (setenv(name.c_str(), value.c_str(), 1) != 0) {
      restore_environment();
      mark_posix_special_builtin_failure(1);
      return 1;
    }
    if (posix_special_builtin && index < assignment_count) {
      if (!set_shell_variable(name, value, true)) {
        restore_environment();
        mark_posix_special_builtin_failure(1);
        return 1;
      }
      persistent_environment_names.insert(name);
    }
  }
  if (!assignment_indices.empty()) {
    std::vector<std::string> command_words;
    command_words.reserve(expanded.size() - assignment_indices.size());
    for (size_t i = 0; i < expanded.size(); ++i)
      if (!is_assignment[i]) command_words.push_back(std::move(expanded[i]));
    expanded = std::move(command_words);
  }
  if (expanded.size() == 2 && expanded[0] == "exec" &&
      !shell_function_precedes_builtin("exec") && redirections.size() == 1 &&
      redirections[0].type == ">&" && redirections[0].filename == "-" &&
      expanded[1].size() > 2 && expanded[1].front() == '{' && expanded[1].back() == '}') {
    const std::string descriptor_name = expanded[1].substr(1, expanded[1].size() - 2);
    const size_t bracket = descriptor_name.find('[');
    if (bracket != std::string::npos && descriptor_name.back() == ']') {
      const std::string array_name = descriptor_name.substr(0, bracket);
      const std::string index_text = descriptor_name.substr(bracket + 1,
                                                             descriptor_name.size() - bracket - 2);
      char *end = nullptr;
      const long index = std::strtol(index_text.c_str(), &end, 10);
      auto array = shell_state().arrays.find(array_name);
      if (array != shell_state().arrays.end() && end != index_text.c_str() && *end == '\0' &&
          index >= 0 && static_cast<size_t>(index) < array->second.size()) {
        char *fd_end = nullptr;
        const long fd = std::strtol(array->second[static_cast<size_t>(index)].c_str(), &fd_end, 10);
        if (fd_end != array->second[static_cast<size_t>(index)].c_str() && *fd_end == '\0' &&
            fd > STDERR_FILENO && fd <= INT_MAX && close(static_cast<int>(fd)) == 0) {
          restore_environment();
          set_last_status(0);
          return 0;
        }
      }
      std::cerr << "dax: exec: " << descriptor_name << ": invalid file descriptor\n";
      restore_environment();
      mark_posix_special_builtin_failure(1);
      return 1;
    }
  }
  // 1. Handle Redirections
  int saved_stdin = dup(STDIN_FILENO);
  int saved_stdout = dup(STDOUT_FILENO);
  int saved_stderr = dup(STDERR_FILENO);
  ExtraDescriptorRestore extra_descriptors(redirections);
  DynamicDescriptorCleanup dynamic_descriptor_cleanup(shell_state().varredir_close);
  for (const auto &redirection : redirections) {
    if (redirection.descriptor >= 3 && !extra_descriptors.save(redirection.descriptor)) {
      std::cerr << "dax: cannot preserve file descriptor " << redirection.descriptor
                << ": " << std::strerror(errno) << '\n';
      close(saved_stdin); close(saved_stdout); close(saved_stderr);
      restore_environment();
      mark_posix_special_builtin_failure(1);
      return 1;
    }
  }
  auto bind_dynamic_descriptor = [&](const Redirection &redirection, int &fd) {
    if (redirection.descriptor_variable.empty()) return true;
    if (fd < 10) {
      const int moved = fcntl(fd, F_DUPFD, 10);
      if (moved < 0) return false;
      close(fd);
      fd = moved;
    }
    if (shell_variable_readonly(redirection.descriptor_variable) ||
        !set_shell_variable(redirection.descriptor_variable, std::to_string(fd))) {
      errno = EINVAL;
      return false;
    }
    dynamic_descriptor_cleanup.add(fd);
    return true;
  };

  for (const auto &red : redirections) {
    if (red.here_document) {
      std::string content = red.filename;
      if (red.expand_document) {
        std::string expansion_source;
        for (size_t i = 0; i < content.size(); ++i) {
          if (content[i] == '\\' && i + 1 < content.size()) {
            const char next = content[i + 1];
            if (next == '\n') { ++i; continue; }
            if (next == '$' || next == '`' || next == '\\') {
              expansion_source += '\x0b';
              expansion_source += next;
              ++i;
              continue;
            }
            // Here-document expansion has a narrower escape set than
            // double-quoted word expansion. Protect the backslash from the
            // double-quote wrapper used by expand_words (e.g. preserve `\"`).
            expansion_source += '\x0b';
            expansion_source += '\\';
            continue;
          }
          expansion_source += content[i];
        }
        std::string encoded_source;
        for (size_t i = 0; i < expansion_source.size(); ++i) {
          if (expansion_source[i] == '\x0b' && i + 1 < expansion_source.size()) {
            encoded_source += expansion_source[i];
            encoded_source += expansion_source[++i];
            continue;
          }
          if (expansion_source[i] == '`') {
            size_t end = i + 1;
            while (end < expansion_source.size()) {
              if (expansion_source[end] == '\x0b' && end + 1 < expansion_source.size()) {
                end += 2;
                continue;
              }
              if (expansion_source[end] == '`') break;
              ++end;
            }
            if (end < expansion_source.size()) {
              encoded_source += '\x03';
              encoded_source.append(expansion_source, i + 1, end - i - 1);
              encoded_source += '\x03';
              i = end;
              continue;
            }
          }
          encoded_source += expansion_source[i];
        }
        std::vector<std::string> expanded_content = {
            std::string("\x02") + encoded_source + "\x02"};
        expand_words(expanded_content);
        if (!word_expansion_succeeded()) {
          dup2(saved_stdin, STDIN_FILENO); dup2(saved_stdout, STDOUT_FILENO); dup2(saved_stderr, STDERR_FILENO);
          close(saved_stdin); close(saved_stdout); close(saved_stderr); restore_environment();
          mark_posix_special_builtin_failure(1);
          return 1;
        }
        content = expanded_content.empty() ? std::string() : expanded_content.front();
      }
      if (red.here_string) content += '\n';
      FILE *temporary = tmpfile();
      if (!temporary || fwrite(content.data(), 1, content.size(), temporary) != content.size() ||
          fseek(temporary, 0, SEEK_SET) != 0) {
        perror("here-document");
        if (temporary) fclose(temporary);
        dup2(saved_stdin, STDIN_FILENO); dup2(saved_stdout, STDOUT_FILENO); dup2(saved_stderr, STDERR_FILENO);
        close(saved_stdin); close(saved_stdout); close(saved_stderr); restore_environment();
        mark_posix_special_builtin_failure(1);
        return 1;
      }
      const int temporary_fd = fileno(temporary);
      int source_fd = temporary_fd;
      int destination = red.descriptor >= 0 ? red.descriptor : STDIN_FILENO;
      bool temporary_closed = false;
      if (!red.descriptor_variable.empty()) {
        int dynamic_fd = fcntl(source_fd, F_DUPFD, 10);
        fclose(temporary);
        temporary_closed = true;
        if (dynamic_fd < 0 || !bind_dynamic_descriptor(red, dynamic_fd)) {
          if (dynamic_fd >= 0) close(dynamic_fd);
          std::cerr << "dax: cannot allocate dynamic file descriptor: " << std::strerror(errno) << '\n';
          dup2(saved_stdin, STDIN_FILENO); dup2(saved_stdout, STDOUT_FILENO); dup2(saved_stderr, STDERR_FILENO);
          close(saved_stdin); close(saved_stdout); close(saved_stderr); restore_environment();
          mark_posix_special_builtin_failure(1);
          return 1;
        }
        destination = dynamic_fd;
        source_fd = -1;
      }
      if (source_fd >= 0 && source_fd == destination) {
        const int moved = fcntl(source_fd, F_DUPFD, std::max(3, destination + 1));
        if (moved < 0) {
          perror("here-document");
          fclose(temporary);
          dup2(saved_stdin, STDIN_FILENO); dup2(saved_stdout, STDOUT_FILENO); dup2(saved_stderr, STDERR_FILENO);
          close(saved_stdin); close(saved_stdout); close(saved_stderr); restore_environment();
          mark_posix_special_builtin_failure(1);
          return 1;
        }
        source_fd = moved;
      }
      if (source_fd >= 0 && dup2(source_fd, destination) < 0) {
        perror("here-document");
        if (source_fd != temporary_fd) close(source_fd);
        if (!temporary_closed) fclose(temporary);
        dup2(saved_stdin, STDIN_FILENO); dup2(saved_stdout, STDOUT_FILENO); dup2(saved_stderr, STDERR_FILENO);
        close(saved_stdin); close(saved_stdout); close(saved_stderr); restore_environment();
        mark_posix_special_builtin_failure(1);
        return 1;
      }
      if (!temporary_closed) fclose(temporary);
      if (source_fd >= 0 && source_fd != temporary_fd && source_fd != destination)
        close(source_fd);
      continue;
    }
    int fd;
    std::vector<std::string> target = {red.filename};
    const auto redirection_expansion_mode = shell_state().posix_mode
        ? shell_state().interactive_shell
              ? WordExpansionMode::InteractiveRedirection
              : WordExpansionMode::Redirection
        : WordExpansionMode::Normal;
    expand_words(target, redirection_expansion_mode);
    if (!word_expansion_succeeded() || target.size() != 1) {
      if (!word_expansion_succeeded())
        std::cerr << "dax: line " << red.line << ", column " << red.column
                  << ": redirection expansion failed\n";
      dup2(saved_stdin, STDIN_FILENO);
      dup2(saved_stdout, STDOUT_FILENO);
      dup2(saved_stderr, STDERR_FILENO);
      close(saved_stdin);
      close(saved_stdout);
      close(saved_stderr);
      restore_environment();
      mark_posix_special_builtin_failure(1);
      return 1;
    }
    const std::string &filename = target.front();
    if (red.type == "2>&" || red.type == ">&" || red.type == "<&") {
      if (!red.descriptor_variable.empty() && filename == "-") {
        const auto &state = shell_state();
        auto variable = state.vars.find(red.descriptor_variable);
        const char *environment = std::getenv(red.descriptor_variable.c_str());
        const std::string value = variable != state.vars.end()
            ? variable->second : environment ? environment : std::string();
        char *end = nullptr;
        const long existing_fd = std::strtol(value.c_str(), &end, 10);
        if (value.empty() || end == value.c_str() || *end || existing_fd < 10 ||
            existing_fd > INT_MAX || close(static_cast<int>(existing_fd)) < 0) {
          std::cerr << "dax: " << red.descriptor_variable << ": invalid file descriptor\n";
          dup2(saved_stdin, STDIN_FILENO); dup2(saved_stdout, STDOUT_FILENO); dup2(saved_stderr, STDERR_FILENO);
          close(saved_stdin); close(saved_stdout); close(saved_stderr); restore_environment();
          mark_posix_special_builtin_failure(1);
          return 1;
        }
        dynamic_descriptor_cleanup.forget(static_cast<int>(existing_fd));
        continue;
      }
      const int destination = red.descriptor >= 0 ? red.descriptor :
                              red.type == "<&" ? STDIN_FILENO : STDOUT_FILENO;
      if (filename == "-") {
        close(destination);
      } else {
        char *end = nullptr;
        long source = std::strtol(filename.c_str(), &end, 10);
        if (end == filename.c_str() || *end || source < 0 || source > INT_MAX) {
          std::cerr << "dax: " << red.type << ": bad file descriptor: " << filename << '\n';
          dup2(saved_stdin, STDIN_FILENO); dup2(saved_stdout, STDOUT_FILENO); dup2(saved_stderr, STDERR_FILENO);
          close(saved_stdin); close(saved_stdout); close(saved_stderr); restore_environment();
          mark_posix_special_builtin_failure(1);
          return 1;
        }
        if (!red.descriptor_variable.empty()) {
          int dynamic_fd = fcntl(static_cast<int>(source), F_DUPFD, 10);
          if (dynamic_fd < 0 || !bind_dynamic_descriptor(red, dynamic_fd)) {
            if (dynamic_fd >= 0) close(dynamic_fd);
            std::cerr << "dax: " << red.type << ": cannot allocate dynamic file descriptor\n";
            dup2(saved_stdin, STDIN_FILENO); dup2(saved_stdout, STDOUT_FILENO); dup2(saved_stderr, STDERR_FILENO);
            close(saved_stdin); close(saved_stdout); close(saved_stderr); restore_environment();
            mark_posix_special_builtin_failure(1);
            return 1;
          }
        } else if (dup2(static_cast<int>(source), destination) < 0) {
          std::cerr << "dax: " << red.type << ": bad file descriptor: " << filename << '\n';
          dup2(saved_stdin, STDIN_FILENO); dup2(saved_stdout, STDOUT_FILENO); dup2(saved_stderr, STDERR_FILENO);
          close(saved_stdin); close(saved_stdout); close(saved_stderr); restore_environment();
          mark_posix_special_builtin_failure(1);
          return 1;
        }
      }
    } else if (red.type == "<" || red.type == "<>") {
      const int open_flags = red.type == "<>" ? O_RDWR | O_CREAT : O_RDONLY;
      fd = open(filename.c_str(), open_flags, 0666);
      if (fd < 0) {
        perror(red.type == "<>" ? "open read/write" : "open input");
        dup2(saved_stdin, STDIN_FILENO);
        dup2(saved_stdout, STDOUT_FILENO);
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stdin);
        close(saved_stdout);
        close(saved_stderr);
        restore_environment();
        mark_posix_special_builtin_failure(1);
        return 1;
      }
      int destination = red.descriptor >= 0 ? red.descriptor : STDIN_FILENO;
      if (!red.descriptor_variable.empty()) {
        if (!bind_dynamic_descriptor(red, fd)) {
          std::cerr << "dax: cannot allocate dynamic file descriptor: " << std::strerror(errno) << '\n';
          close(fd);
          dup2(saved_stdin, STDIN_FILENO); dup2(saved_stdout, STDOUT_FILENO); dup2(saved_stderr, STDERR_FILENO);
          close(saved_stdin); close(saved_stdout); close(saved_stderr); restore_environment();
          mark_posix_special_builtin_failure(1);
          return 1;
        }
        destination = fd;
      }
      if (dup2(fd, destination) < 0) {
        perror("dup2 input");
        close(fd);
        dup2(saved_stdin, STDIN_FILENO); dup2(saved_stdout, STDOUT_FILENO); dup2(saved_stderr, STDERR_FILENO);
        close(saved_stdin); close(saved_stdout); close(saved_stderr); restore_environment();
        mark_posix_special_builtin_failure(1);
        return 1;
      }
      if (fd != destination) close(fd);
    } else if (red.type == ">" || red.type == ">|" || red.type == "&>") {
      const int create_flags = O_WRONLY | O_CREAT | O_TRUNC |
                               (red.type != ">|" && shell_noclobber_enabled()
                                    ? O_EXCL : 0);
      fd = open(filename.c_str(), create_flags, 0666);
      if (fd < 0) {
        if (errno == EEXIST && shell_noclobber_enabled())
          std::cerr << "dax: cannot overwrite existing file: " << filename << '\n';
        else
          perror("open output");
        dup2(saved_stdin, STDIN_FILENO);
        dup2(saved_stdout, STDOUT_FILENO);
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stdin);
        close(saved_stdout);
        close(saved_stderr);
        restore_environment();
        mark_posix_special_builtin_failure(1);
        return 1;
      }
      int destination = red.descriptor >= 0 ? red.descriptor : STDOUT_FILENO;
      if (!red.descriptor_variable.empty()) {
        if (!bind_dynamic_descriptor(red, fd)) {
          std::cerr << "dax: cannot allocate dynamic file descriptor: " << std::strerror(errno) << '\n';
          close(fd);
          dup2(saved_stdin, STDIN_FILENO); dup2(saved_stdout, STDOUT_FILENO); dup2(saved_stderr, STDERR_FILENO);
          close(saved_stdin); close(saved_stdout); close(saved_stderr); restore_environment();
          mark_posix_special_builtin_failure(1);
          return 1;
        }
        destination = fd;
      }
      if (dup2(fd, destination) < 0 ||
          (red.type == "&>" && dup2(fd, STDERR_FILENO) < 0)) {
        perror("dup2 output");
        if (fd != destination) close(fd);
        dup2(saved_stdin, STDIN_FILENO); dup2(saved_stdout, STDOUT_FILENO); dup2(saved_stderr, STDERR_FILENO);
        close(saved_stdin); close(saved_stdout); close(saved_stderr); restore_environment();
        mark_posix_special_builtin_failure(1);
        return 1;
      }
      if (fd != destination) close(fd);
    } else if (red.type == ">>" || red.type == "&>>") {
      fd = open(filename.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0666);
      if (fd < 0) {
        perror("open append");
        dup2(saved_stdin, STDIN_FILENO);
        dup2(saved_stdout, STDOUT_FILENO);
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stdin);
        close(saved_stdout);
        close(saved_stderr);
        restore_environment();
        mark_posix_special_builtin_failure(1);
        return 1;
      }
      int destination = red.descriptor >= 0 ? red.descriptor : STDOUT_FILENO;
      if (!red.descriptor_variable.empty()) {
        if (!bind_dynamic_descriptor(red, fd)) {
          std::cerr << "dax: cannot allocate dynamic file descriptor: " << std::strerror(errno) << '\n';
          close(fd);
          dup2(saved_stdin, STDIN_FILENO); dup2(saved_stdout, STDOUT_FILENO); dup2(saved_stderr, STDERR_FILENO);
          close(saved_stdin); close(saved_stdout); close(saved_stderr); restore_environment();
          mark_posix_special_builtin_failure(1);
          return 1;
        }
        destination = fd;
      }
      if (dup2(fd, destination) < 0 ||
          (red.type == "&>>" && dup2(fd, STDERR_FILENO) < 0)) {
        perror("dup2 append");
        if (fd != destination) close(fd);
        dup2(saved_stdin, STDIN_FILENO); dup2(saved_stdout, STDOUT_FILENO); dup2(saved_stderr, STDERR_FILENO);
        close(saved_stdin); close(saved_stdout); close(saved_stderr); restore_environment();
        mark_posix_special_builtin_failure(1);
        return 1;
      }
      if (fd != destination) close(fd);
    }
  }

  // Bash performs the implicit stderr-to-pipe duplication for `|&` after
  // applying the command's explicit redirections.
  if (shell_state().pipeline_stderr_to_stdout &&
      dup2(STDOUT_FILENO, STDERR_FILENO) < 0) {
    perror("|&");
    dup2(saved_stdin, STDIN_FILENO);
    dup2(saved_stdout, STDOUT_FILENO);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stdin);
    close(saved_stdout);
    close(saved_stderr);
    restore_environment();
    mark_posix_special_builtin_failure(1);
    return 1;
  }

  if (deferred_execution) {
    const int status = deferred_execution();
    std::cout.flush();
    std::cerr.flush();
    dup2(saved_stdin, STDIN_FILENO);
    dup2(saved_stdout, STDOUT_FILENO);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stdin);
    close(saved_stdout);
    close(saved_stderr);
    restore_environment();
    mark_posix_special_builtin_failure(status);
    return status;
  }

  if (!expanded.empty() && shell_function_precedes_builtin(expanded[0])) {
    const int status = shell_execute_function(expanded);
    std::cout.flush();
    std::cerr.flush();
    dup2(saved_stdin, STDIN_FILENO);
    dup2(saved_stdout, STDOUT_FILENO);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stdin);
    close(saved_stdout);
    close(saved_stderr);
    restore_environment();
    return status;
  }

  if (!expanded.empty() && expanded[0] == "exec") {
    if (expanded.size() == 1 && redirections.empty()) {
      dup2(saved_stdin, STDIN_FILENO); dup2(saved_stdout, STDOUT_FILENO); dup2(saved_stderr, STDERR_FILENO);
      close(saved_stdin); close(saved_stdout); close(saved_stderr); restore_environment();
      return 0;
    }
    if (expanded.size() == 1) {
      extra_descriptors.release();
      dynamic_descriptor_cleanup.release();
      close(saved_stdin); close(saved_stdout); close(saved_stderr);
      restore_environment();
      return 0;
    }
    std::vector<char *> exec_args;
    for (size_t i = 1; i < expanded.size(); ++i) exec_args.push_back(const_cast<char *>(expanded[i].c_str()));
    exec_args.push_back(nullptr);
    const auto resolved_exec_path = shell_find_command(expanded[1], true);
    const std::string exec_path = resolved_exec_path.value_or(expanded[1]);
    const char *old_underscore = std::getenv("_");
    const std::optional<std::string> saved_underscore = old_underscore
        ? std::optional<std::string>(old_underscore) : std::nullopt;
    setenv("_", exec_path.c_str(), 1);
    fcntl(saved_stdin, F_SETFD, FD_CLOEXEC); fcntl(saved_stdout, F_SETFD, FD_CLOEXEC); fcntl(saved_stderr, F_SETFD, FD_CLOEXEC);
    if (resolved_exec_path) {
      execv(exec_path.c_str(), exec_args.data());
      if (errno == ENOEXEC) execvp(exec_path.c_str(), exec_args.data());
    } else {
      execvp(exec_args[0], exec_args.data());
    }
    const int exec_error = errno;
    report_command_launch_error(expanded[1], exec_error);
    if (saved_underscore) setenv("_", saved_underscore->c_str(), 1);
    else unsetenv("_");
    dup2(saved_stdin, STDIN_FILENO); dup2(saved_stdout, STDOUT_FILENO); dup2(saved_stderr, STDERR_FILENO);
    close(saved_stdin); close(saved_stdout); close(saved_stderr);
    restore_environment();
    const int exec_status = exec_error == ENOENT ? 127 : 126;
    if (!shell_state().interactive_shell && !shell_state().execfail) {
      shell_state().exit_value = exec_status;
      shell_state().exiting = true;
    }
    return exec_status;
  }

  // 2. Handle Builtins
  if (!expanded.empty())
    if (auto status = handle_builtin(expanded)) {
      // Restore FDs
      std::cout.flush();
      dup2(saved_stdin, STDIN_FILENO);
      dup2(saved_stdout, STDOUT_FILENO);
      dup2(saved_stderr, STDERR_FILENO);
      close(saved_stdin);
      close(saved_stdout);
      close(saved_stderr);
      restore_environment();
      mark_posix_special_builtin_failure(*status);
      return *status;
    }

  if (!expanded.empty()) {
    if (auto status = janet_dispatch_command(expanded)) {
      std::cout.flush();
      std::cerr.flush();
      dup2(saved_stdin, STDIN_FILENO);
      dup2(saved_stdout, STDOUT_FILENO);
      dup2(saved_stderr, STDERR_FILENO);
      close(saved_stdin);
      close(saved_stdout);
      close(saved_stderr);
      restore_environment();
      return *status;
    }
  }

  if (!expanded.empty() && shell_state().interactive_shell &&
      shell_state().autocd && !shell_find_command(expanded.front(), false, false)) {
    std::error_code directory_error;
    if (std::filesystem::is_directory(expanded.front(), directory_error)) {
      std::vector<std::string> cd_command = expanded;
      cd_command.front() = "cd";
      const int cd_status = handle_builtin(cd_command).value_or(1);
      std::cout.flush();
      std::cerr.flush();
      dup2(saved_stdin, STDIN_FILENO);
      dup2(saved_stdout, STDOUT_FILENO);
      dup2(saved_stderr, STDERR_FILENO);
      close(saved_stdin);
      close(saved_stdout);
      close(saved_stderr);
      restore_environment();
      return cd_status;
    }
  }

  if (!expanded.empty() &&
      !shell_find_command(expanded.front(), false, false) &&
      shell_state().functions.count("command_not_found_handle")) {
    bool path_entry_exists = false;
    const bool searched_path = expanded.front().find('/') == std::string::npos;
    auto check_path_entry = [&](const std::string &candidate) {
      struct stat info {};
      if (stat(candidate.c_str(), &info) == 0 &&
          (!searched_path || !shell_command_path_is_ignored(candidate)))
        path_entry_exists = true;
    };
    if (expanded.front().find('/') != std::string::npos) {
      check_path_entry(expanded.front());
    } else if (const auto path = shell_path_value()) {
      size_t begin = 0;
      while (begin <= path->size() && !path_entry_exists) {
        const size_t end = path->find(':', begin);
        const std::string directory = path->substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        check_path_entry((std::filesystem::path(directory.empty() ? "." : directory) /
                          expanded.front()).string());
        if (end == std::string::npos) break;
        begin = end + 1;
      }
    }
    if (!path_entry_exists) {
      std::vector<std::string> handler = {"command_not_found_handle"};
      handler.insert(handler.end(), expanded.begin(), expanded.end());
      if (auto status = handle_builtin(handler)) {
        std::cout.flush();
        std::cerr.flush();
        dup2(saved_stdin, STDIN_FILENO);
        dup2(saved_stdout, STDOUT_FILENO);
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stdin);
        close(saved_stdout);
        close(saved_stderr);
        restore_environment();
        return *status;
      }
    }
  }

  if (expanded.empty()) {
    std::cout.flush();
    dup2(saved_stdin, STDIN_FILENO);
    dup2(saved_stdout, STDOUT_FILENO);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stdin);
    close(saved_stdout);
    close(saved_stderr);
    restore_environment();
    return no_command_status;
  }

  const bool is_exit_attempt = expanded[0] == "exit" || expanded[0] == "logout" ||
      (expanded[0] == "command" && expanded.size() > 1 &&
       (expanded[1] == "exit" || expanded[1] == "logout"));
  if (shell_state().exit_warning_pending && !is_exit_attempt)
    shell_state().exit_warning_pending = false;

  // 3. Prepare arguments
  std::vector<char *> c_args;
  for (const auto &arg : expanded) {
    c_args.push_back(const_cast<char *>(arg.c_str()));
  }
  c_args.push_back(nullptr);
  const auto resolved_executable = shell_find_command(expanded[0], true);
  const std::string executable_path = resolved_executable.value_or(expanded[0]);

  // 3. Fork and Exec
  pid_t pid = fork();
  if (pid == 0) {
    // Child process
    // Reset signals to default
    signal(SIGINT, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    setenv("_", executable_path.c_str(), 1);

    if (resolved_executable) {
      execv(executable_path.c_str(), c_args.data());
      if (errno == ENOEXEC) execvp(executable_path.c_str(), c_args.data());
    } else {
      execvp(c_args[0], c_args.data());
    }
    int exec_error = errno;
    report_command_launch_error(c_args[0], exec_error);
    _exit(exec_error == ENOENT ? 127 : 126);
  } else if (pid < 0) {
    perror("fork");
    dup2(saved_stdin, STDIN_FILENO);
    dup2(saved_stdout, STDOUT_FILENO);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stdin);
    close(saved_stdout);
    close(saved_stderr);
    restore_environment();
    return 1;
  } else {
    // Parent process
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }

    refresh_shell_terminal_dimensions(saved_stderr);

    // Restore FDs
    dup2(saved_stdin, STDIN_FILENO);
    dup2(saved_stdout, STDOUT_FILENO);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stdin);
    close(saved_stdout);
    close(saved_stderr);

    if (WIFEXITED(status)) {
      restore_environment();
      return WEXITSTATUS(status);
    }
    restore_environment();
    return 1;
  }
}

void RedirectionNode::print(int indent) const {
  std::cout << std::string(indent, ' ') << "Redirections:\n";
  if (child) child->print(indent + 2);
  for (const auto &redirection : redirections)
    std::cout << std::string(indent + 2, ' ') << redirection.type << ' '
              << redirection.filename << '\n';
}

int RedirectionNode::execute() {
  CommandNode wrapper;
  wrapper.redirections = redirections;
  wrapper.raw_command = raw_command;
  wrapper.deferred_execution = [&]() { return child ? child->execute() : 0; };
  return wrapper.execute();
}

int PipeNode::execute() {
  std::vector<ASTNode *> stages;
  std::vector<bool> stderr_edges;
  auto flatten = [&](auto &&self, ASTNode *node) -> void {
    if (auto *pipeline = dynamic_cast<PipeNode *>(node)) {
      self(self, pipeline->left.get());
      if (!stderr_edges.empty()) stderr_edges.back() = pipeline->stderr_to_pipe;
      self(self, pipeline->right.get());
    } else if (node) {
      stages.push_back(node);
      stderr_edges.push_back(false);
    }
  };
  flatten(flatten, this);
  if (stages.empty()) return 0;
  const uint64_t err_generation = shell_state().err_trap_generation;

  std::vector<std::array<int, 2>> pipes(stages.size() - 1);
  for (auto &fd : pipes) fd = {-1, -1};
  for (auto &fd : pipes) {
    if (pipe(fd.data()) < 0) {
      perror("pipe");
      for (const auto &opened : pipes) {
        if (opened[0] >= 0) close(opened[0]);
        if (opened[1] >= 0) close(opened[1]);
      }
      return 1;
    }
  }

  std::vector<pid_t> children;
  const bool previous_trap_suppression = shell_state().err_trap_suppressed;
  bool fork_failed = false;
  const bool run_last_in_parent = stages.size() > 1 && shell_state().lastpipe &&
                                  !shell_state().monitor;
  const size_t fork_count = run_last_in_parent ? stages.size() - 1 : stages.size();
  for (size_t i = 0; i < fork_count; ++i) {
    const pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
      fork_failed = true;
      break;
    }
    if (pid == 0) {
      // Bash never runs ERR for a failing non-final pipeline component. The
      // pipeline's own status is checked after all stages finish, where
      // pipefail determines whether that failure is visible to the parent.
      // errtrace still allows ERR in the final component's subshell.
      shell_state().err_trap_suppressed = previous_trap_suppression ||
          !shell_state().errtrace || i + 1 < stages.size();
      enter_subshell_environment();
      signal(SIGINT, SIG_DFL);
      signal(SIGCHLD, SIG_DFL);
      if (i > 0) dup2(pipes[i - 1][0], STDIN_FILENO);
      if (i + 1 < stages.size()) dup2(pipes[i][1], STDOUT_FILENO);
      shell_state().pipeline_stderr_to_stdout = i + 1 < stages.size() && stderr_edges[i];
      // Simple commands defer this duplication until after their explicit
      // redirections. Other AST stages have no command-level redirection
      // pass, so establish the pipeline stream before executing them.
      if (shell_state().pipeline_stderr_to_stdout &&
          !dynamic_cast<CommandNode *>(stages[i]) &&
          dup2(STDOUT_FILENO, STDERR_FILENO) < 0)
        _exit(1);
      for (const auto &fd : pipes) {
        close(fd[0]);
        close(fd[1]);
      }
      const int status = stages[i]->execute() & 255;
      run_exit_trap();
      std::exit(status);
    }
    children.push_back(pid);
  }
  std::vector<int> stage_statuses(stages.size(), 1);
  if (run_last_in_parent && !fork_failed) {
    const int saved_stdin = dup(STDIN_FILENO);
    const int pipeline_input = pipes.back()[0];
    for (auto &fd : pipes) {
      if (fd[1] >= 0) { close(fd[1]); fd[1] = -1; }
      if (fd[0] >= 0 && fd[0] != pipeline_input) { close(fd[0]); fd[0] = -1; }
    }
    if (saved_stdin < 0 || dup2(pipeline_input, STDIN_FILENO) < 0) {
      perror("lastpipe");
      if (saved_stdin >= 0) close(saved_stdin);
      if (pipeline_input >= 0) close(pipeline_input);
      fork_failed = true;
    } else {
      close(pipeline_input);
      begin_errexit_only_ignored_context();
      const int last_status = stages.back()->execute();
      end_errexit_only_ignored_context();
      stage_statuses.back() = last_status & 255;
      std::cout.flush();
      if (dup2(saved_stdin, STDIN_FILENO) < 0) perror("lastpipe: restore stdin");
      close(saved_stdin);
    }
  } else {
    for (auto &fd : pipes) {
      if (fd[0] >= 0) { close(fd[0]); fd[0] = -1; }
      if (fd[1] >= 0) { close(fd[1]); fd[1] = -1; }
    }
  }
  if (fork_failed) {
    for (pid_t pid : children) kill(pid, SIGTERM);
  }

  for (size_t i = 0; i < children.size(); ++i) {
    int wait_status = 0;
    pid_t waited;
    do { waited = waitpid(children[i], &wait_status, 0); }
    while (waited < 0 && errno == EINTR);
    if (waited >= 0 && WIFEXITED(wait_status)) stage_statuses[i] = WEXITSTATUS(wait_status);
    else if (waited >= 0 && WIFSIGNALED(wait_status)) stage_statuses[i] = 128 + WTERMSIG(wait_status);
    else stage_statuses[i] = 255;
  }
  refresh_shell_terminal_dimensions(STDERR_FILENO);
  const int right_status = stage_statuses.back();
  int result = right_status;
  if (shell_option_enabled("pipefail")) {
    for (auto it = stage_statuses.rbegin(); it != stage_statuses.rend(); ++it) {
      if (*it != 0) { result = *it; break; }
    }
  }
  set_pipeline_statuses(stage_statuses);
  set_last_status(result);
  if (err_generation == shell_state().err_trap_generation)
    run_err_trap(result);
  return result;
}

int ListNode::execute() {
  int status = 0;
  if (left)
    status = left->execute();
  set_last_status(status);
  run_pending_signal_trap();
  set_last_status(status);
  apply_errexit(status);
  if (right && !shell_noexec_enabled() && !shell_should_exit() &&
      !shell_should_return() && !loop_control_pending()) {
    clear_errexit_immune();
    status = right->execute();
  }
  set_last_status(status);
  apply_errexit(status);
  return status;
}

void AndOrNode::print(int indent) const {
  std::cout << std::string(indent, ' ') << (and_mode ? "And:\n" : "Or:\n");
  if (left) left->print(indent + 2);
  if (right) right->print(indent + 2);
}

int AndOrNode::execute() {
  begin_errexit_ignored_context();
  int status = left ? left->execute() : 0;
  end_errexit_ignored_context();
  set_last_status(status);
  if (!shell_noexec_enabled() &&
      ((and_mode && status == 0) || (!and_mode && status != 0))) {
    clear_errexit_immune();
    status = right ? right->execute() : 2;
  } else if (status != 0) {
    mark_errexit_immune();
  }
  set_last_status(status);
  return status;
}

void NotNode::print(int indent) const {
  std::cout << std::string(indent, ' ') << "Not:\n";
  if (child) child->print(indent + 2);
}

int NotNode::execute() {
  begin_errexit_ignored_context();
  int status = child ? child->execute() : 0;
  end_errexit_ignored_context();
  status = status == 0 ? 1 : 0;
  if (status != 0) mark_errexit_immune();
  set_last_status(status);
  return status;
}

void GroupNode::print(int indent) const {
  std::cout << std::string(indent, ' ') << (subshell ? "Subshell:\n" : "Group:\n");
  if (child) child->print(indent + 2);
}

int GroupNode::execute() {
  if (!subshell) {
    int status = child ? child->execute() : 0;
    set_last_status(status);
    return status;
  }
  const bool previous_trap_suppression = shell_state().err_trap_suppressed;
  shell_state().err_trap_suppressed = previous_trap_suppression ||
                                      !shell_state().errtrace;
  pid_t pid = fork();
  if (pid != 0) shell_state().err_trap_suppressed = previous_trap_suppression;
  if (pid < 0) {
    perror("fork");
    set_single_pipeline_status(1);
    set_last_status(1);
    return 1;
  }
  if (pid == 0) {
    enter_subshell_environment();
    int status = child ? child->execute() : 0;
    run_exit_trap();
    std::cout.flush();
    std::cerr.flush();
    _exit(status & 255);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  int result = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  set_single_pipeline_status(result);
  set_last_status(result);
  run_err_trap(result);
  return result;
}

void IfNode::print(int indent) const {
  std::cout << std::string(indent, ' ') << "If:\n";
  for (const auto &branch : branches) {
    std::cout << std::string(indent + 2, ' ') << "Condition:\n";
    if (branch.first) branch.first->print(indent + 4);
    if (branch.second) branch.second->print(indent + 4);
  }
  if (otherwise) { std::cout << std::string(indent + 2, ' ') << "Else:\n"; otherwise->print(indent + 4); }
}

int IfNode::execute() {
  for (auto &branch : branches) {
    begin_errexit_ignored_context();
    const int condition_status = branch.first ? branch.first->execute() : 0;
    end_errexit_ignored_context();
    // Immunity applies to the condition's status, not commands in the branch.
    clear_errexit_immune();
    set_last_status(condition_status);
    if (shell_noexec_enabled()) return condition_status;
    if (condition_status == 0) {
      int status = branch.second ? branch.second->execute() : 0;
      set_last_status(status);
      return status;
    }
  }
  int status = otherwise ? otherwise->execute() : 0;
  set_last_status(status);
  return status;
}

void LoopNode::print(int indent) const {
  std::cout << std::string(indent, ' ') << (until ? "Until:\n" : "While:\n");
  if (condition) condition->print(indent + 2);
  if (body) body->print(indent + 2);
}

int LoopNode::execute() {
  int status = 0;
  enter_shell_loop();
  while (!shell_should_exit() && !shell_should_return()) {
    begin_errexit_ignored_context();
    int condition_status = condition ? condition->execute() : 0;
    end_errexit_ignored_context();
    // A failed condition (including a negated or &&/|| list) must not make
    // the loop body immune to errexit or ERR handling.
    clear_errexit_immune();
    set_last_status(condition_status);
    if (shell_noexec_enabled()) { status = condition_status; break; }
    const int condition_control = take_loop_control();
    if (condition_control == 1 || condition_control == 3) break;
    if (condition_control == 2) continue;
    if (until ? condition_status == 0 : condition_status != 0) break;
    status = body ? body->execute() : 0;
    set_last_status(status);
    if (shell_noexec_enabled()) break;
    const int body_control = take_loop_control();
    if (body_control == 1 || body_control == 3) break;
    if (body_control == 2) continue;
  }
  leave_shell_loop();
  return status;
}

void ForNode::print(int indent) const {
  std::cout << std::string(indent, ' ') << "For " << variable << ":\n";
  for (const auto &value : values) std::cout << std::string(indent + 2, ' ') << value << '\n';
  if (body) body->print(indent + 2);
}

int ForNode::execute() {
  ProcessSubstitutionScope process_substitution_scope;
  run_debug_trap(raw_command);
  begin_word_expansion();
  expand_words(values);
  if (!word_expansion_succeeded()) return 1;
  int status = 0;
  enter_shell_loop();
  for (const auto &value : values) {
    if (shell_should_exit() || shell_should_return()) break;
    if (!set_shell_variable(variable, value)) {
      leave_shell_loop();
      set_last_status(1);
      return 1;
    }
    status = body ? body->execute() : 0;
    set_last_status(status);
    if (shell_noexec_enabled()) break;
    const int control = take_loop_control();
    if (control == 1 || control == 3) break;
    if (control == 2) continue;
  }
  leave_shell_loop();
  return status;
}

void SelectNode::print(int indent) const {
  std::cout << std::string(indent, ' ') << "Select " << variable << ":\n";
  for (const auto &value : values) std::cout << std::string(indent + 2, ' ') << value << '\n';
  if (body) body->print(indent + 2);
}

int SelectNode::execute() {
  ProcessSubstitutionScope process_substitution_scope;
  run_debug_trap(raw_command);
  begin_word_expansion();
  expand_words(values);
  if (!word_expansion_succeeded()) return 1;

  auto &state = shell_state();
  auto prompt = state.vars.find("PS3");
  const char *environment_prompt = std::getenv("PS3");
  const std::string ps3 = prompt != state.vars.end() ? prompt->second :
                          environment_prompt ? environment_prompt : "#? ";
  const size_t width = std::to_string(values.size()).size();
  const auto display_width = [](const std::string &text) {
    std::mbstate_t conversion_state{};
    size_t cells = 0;
    for (size_t offset = 0; offset < text.size();) {
      wchar_t character = 0;
      const size_t consumed = std::mbrtowc(&character, text.data() + offset,
                                            text.size() - offset, &conversion_state);
      if (consumed == static_cast<size_t>(-1) || consumed == static_cast<size_t>(-2)) {
        conversion_state = std::mbstate_t{};
        ++cells;
        ++offset;
      } else {
        const size_t bytes = consumed == 0 ? 1 : consumed;
        cells += static_cast<size_t>(std::max(::wcwidth(character), 0));
        offset += bytes;
      }
    }
    return cells;
  };
  int status = 0;
  bool print_menu = true;
  enter_shell_loop();
  while (!shell_should_exit() && !shell_should_return()) {
    if (print_menu) {
      size_t item_width = width + 2;
      for (const auto &value : values)
        item_width = std::max(item_width, width + 2 + display_width(value));
      size_t terminal_width = 0;
      const auto configured_columns = state.vars.find("COLUMNS");
      const char *columns_value = configured_columns != state.vars.end()
          ? configured_columns->second.c_str() : std::getenv("COLUMNS");
      if (columns_value && *columns_value) {
        char *end = nullptr;
        errno = 0;
        const long parsed = std::strtol(columns_value, &end, 10);
        if (!errno && end != columns_value && parsed > 0)
          terminal_width = static_cast<size_t>(parsed);
      }
      if (terminal_width == 0) {
        struct winsize terminal_size {};
        if (ioctl(STDERR_FILENO, TIOCGWINSZ, &terminal_size) == 0 &&
            terminal_size.ws_col > 0)
          terminal_width = terminal_size.ws_col;
      }
      if (terminal_width == 0) terminal_width = 80;
      const size_t columns = isatty(STDIN_FILENO)
          ? std::max<size_t>(1, terminal_width / (item_width + 2)) : 1;
      const size_t rows = std::max<size_t>(1, (values.size() + columns - 1) / columns);
      const size_t actual_columns = values.empty()
          ? 1 : (values.size() + rows - 1) / rows;
      for (size_t row = 0; row < rows && !values.empty(); ++row) {
        for (size_t column = 0; column < actual_columns; ++column) {
          const size_t index = row + column * rows;
          if (index >= values.size()) continue;
          const std::string number = std::to_string(index + 1);
          const std::string item = std::string(width > number.size()
                  ? width - number.size() : 0, ' ') + number + ") " + values[index];
          std::cerr << item;
          if (column + 1 < actual_columns) {
            const size_t item_cells = width + 2 + display_width(values[index]);
            const size_t padding = item_width + 2 > item_cells
                ? item_width + 2 - item_cells : 2;
            std::cerr << std::string(padding, ' ');
          }
        }
        std::cerr << '\n';
      }
      if (values.empty()) std::cerr << '\n';
      print_menu = false;
    }
    std::cerr << ps3;
    std::cerr.flush();

    const bool read_was_disabled = state.disabled_builtins.erase("read") != 0;
    const auto read_result = handle_builtin({"read"});
    if (read_was_disabled) state.disabled_builtins.insert("read");
    if (!read_result || *read_result != 0 || shell_should_exit()) {
      if (!read_result || *read_result != 0) status = 1;
      std::cout << '\n';
      break;
    }
    const auto reply_value = state.vars.find("REPLY");
    const std::string reply = reply_value == state.vars.end() ? std::string() : reply_value->second;

    if (reply.empty()) {
      if (!set_shell_variable(variable, "")) { status = 1; break; }
      print_menu = true;
      continue;
    }
    char *end = nullptr;
    errno = 0;
    const intmax_t selected = std::strtoimax(reply.c_str(), &end, 10);
    while (end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    const bool valid = !errno && end != reply.c_str() && end && *end == '\0' &&
                       selected > 0 && static_cast<uintmax_t>(selected) <= values.size();
    const std::string selection = valid ? values[selected - 1] : std::string();
    if (!set_shell_variable(variable, selection)) { status = 1; break; }
    status = body ? body->execute() : 0;
    set_last_status(status);
    if (shell_noexec_enabled()) break;
    const int control = take_loop_control();
    if (control == 1 || control == 3) break;
    if (control == 2) continue;
  }
  leave_shell_loop();
  return status;
}

void ArithmeticForNode::print(int indent) const {
  std::cout << std::string(indent, ' ') << "Arithmetic for: (( " << initialization << "; "
            << condition << "; " << increment << " ))\n";
  if (body) body->print(indent + 2);
}

int ArithmeticForNode::execute() {
  run_debug_trap(raw_command);
  auto evaluate = [](const std::string &expression, long long &value) {
    if (expression.find_first_not_of(" \t\r\n") == std::string::npos) {
      value = 1;
      return true;
    }
    std::string error;
    auto result = evaluate_arithmetic(expression, &error);
    if (!result) {
      std::cerr << "dax: arithmetic for: " << error << '\n';
      return false;
    }
    value = *result;
    return true;
  };
  long long ignored = 0;
  if (!initialization.empty() && !evaluate(initialization, ignored)) {
    set_last_status(1);
    run_err_trap(1);
    return 1;
  }
  int status = 0;
  enter_shell_loop();
  while (!shell_should_exit() && !shell_should_return()) {
    long long condition_value = 1;
    if (!condition.empty() && !evaluate(condition, condition_value)) {
      status = 1;
      run_err_trap(status);
      break;
    }
    if (condition_value == 0) break;
    status = body ? body->execute() : 0;
    set_last_status(status);
    if (shell_noexec_enabled()) break;
    const int control = take_loop_control();
    if (control == 1 || control == 3) break;
    if (!increment.empty() && !evaluate(increment, ignored)) {
      status = 1;
      run_err_trap(status);
      break;
    }
    if (control == 2) continue;
  }
  leave_shell_loop();
  return status;
}

void CaseNode::print(int indent) const {
  std::cout << std::string(indent, ' ') << "Case " << value << ":\n";
  for (const auto &clause : clauses) {
    for (const auto &pattern : clause.patterns) std::cout << std::string(indent + 2, ' ') << pattern << '\n';
    if (clause.body) clause.body->print(indent + 4);
  }
}

int CaseNode::execute() {
  ProcessSubstitutionScope process_substitution_scope;
  run_debug_trap(raw_command);
  std::vector<std::string> subject{value};
  begin_word_expansion();
  expand_words(subject, WordExpansionMode::SingleWord);
  if (!word_expansion_succeeded()) return 1;
  const std::string matched_value = subject.empty() ? std::string() : subject.front();
  bool matched = false;
  Terminator previous_terminator = Terminator::Stop;
  int status = 0;
  for (auto &clause : clauses) {
    if (matched && previous_terminator == Terminator::Stop) break;
    bool clause_matches = false;
    if (matched && previous_terminator == Terminator::FallThrough)
      clause_matches = true;
    else {
      for (const auto &raw_pattern : clause.patterns) {
        std::vector<std::string> expanded_patterns{raw_pattern};
        begin_word_expansion();
        expand_words(expanded_patterns, WordExpansionMode::Pattern);
        if (!word_expansion_succeeded()) return 1;
        for (const auto &pattern : expanded_patterns) {
          if (fnmatch(pattern.c_str(), matched_value.c_str(),
                      (shell_state().extglob ? FNM_EXTMATCH : 0) |
                      (shell_state().nocasematch ? FNM_CASEFOLD : 0)) == 0) {
            clause_matches = true;
            break;
          }
        }
        if (clause_matches) break;
      }
    }
    if (!clause_matches) continue;
    matched = true;
    status = clause.body ? clause.body->execute() : 0;
    set_last_status(status);
    previous_terminator = clause.terminator;
    if (clause.terminator == Terminator::Stop) return status;
  }
  if (!matched) status = 0;
  set_last_status(status);
  return status;
}

void BackgroundNode::print(int indent) const {
  std::cout << std::string(indent, ' ') << "Background:\n";
  if (child)
    child->print(indent + 2);
}

int BackgroundNode::execute() {
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return 1;
  }
  if (pid == 0) {
    auto &state = shell_state();
    state.err_trap_suppressed = state.err_trap_suppressed || !state.errtrace;
    enter_subshell_environment();
    setpgid(0, 0);
    signal(SIGINT, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
      int status = child ? child->execute() : 0;
      run_exit_trap();
      std::cout.flush();
      _exit(status);
  }
  setpgid(pid, pid);
  remember_job(pid, command);
  return 0;
}

void ListNode::print(int indent) const {
  std::string padding(indent, ' ');
  std::cout << padding << "List:\n";
  if (left)
    left->print(indent + 2);
  if (right)
    right->print(indent + 2);
}

