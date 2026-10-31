#include "arithmetic.hpp"
#include "builtins.hpp"
#include "shell_state.hpp"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace {
struct Token {
  std::string text;
  enum Kind { End, Number, Name, String, Operator } kind = End;
  std::string suffix;
};

class ArithmeticParser {
 public:
  explicit ArithmeticParser(const std::string &source, unsigned depth = 0)
      : source_(source), depth_(depth) { next(); }

  std::optional<long long> parse(std::string *error, bool evaluate = true) {
    error_ = error;
    if (depth_ > 1024) return fail("arithmetic recursion limit exceeded");
    if (source_.find_first_not_of(" \t\r\n") == std::string::npos)
      return 0LL;
    auto result = expression(1, evaluate);
    if (!result || current_.kind != Token::End)
      return fail_if_needed("invalid arithmetic expression");
    if (evaluate && result->unset && shell_state().nounset) return fail("unbound variable");
    return result->value;
  }

 private:
  struct Value { long long value = 0; std::string lvalue; bool unset = false; };
  const std::string &source_;
  size_t cursor_ = 0;
  unsigned depth_;
  Token current_;
  std::string *error_ = nullptr;

  static long long wrap(std::uint64_t value) {
    static_assert(std::numeric_limits<long long>::digits == 63,
                  "Dax arithmetic currently requires 64-bit long long");
    if (value <= static_cast<std::uint64_t>(std::numeric_limits<long long>::max()))
      return static_cast<long long>(value);
    // Convert the two's-complement bit pattern without an out-of-range
    // unsigned-to-signed cast, whose result is implementation-defined.
    return -1 - static_cast<long long>(~value);
  }

  static long long arithmetic_right_shift(long long value, unsigned count) {
    if (count == 0) return value;
    constexpr unsigned width = std::numeric_limits<std::uint64_t>::digits;
    const auto bits = static_cast<std::uint64_t>(value);
    std::uint64_t shifted = bits >> count;
    if (value < 0) shifted |= (~std::uint64_t{0}) << (width - count);
    return wrap(shifted);
  }

  std::optional<long long> fail(const std::string &message) {
    if (error_ && error_->empty()) *error_ = message;
    return {};
  }
  std::optional<long long> fail_if_needed(const std::string &message) {
    return fail(message);
  }
  void next() {
    while (cursor_ < source_.size() && std::isspace(static_cast<unsigned char>(source_[cursor_]))) ++cursor_;
    if (cursor_ == source_.size()) { current_ = {}; return; }
    const char c = source_[cursor_];
    if (c == '\'' || c == '"') {
      const char quote = c;
      ++cursor_;
      std::string value;
      bool closed = false;
      while (cursor_ < source_.size()) {
        const char ch = source_[cursor_++];
        if (ch == quote) { closed = true; break; }
        if (ch == '\\' && quote == '"' && cursor_ < source_.size()) {
          const char escaped = source_[cursor_++];
          if (escaped == '\n') continue;
          if (escaped == '"' || escaped == '\\' || escaped == '$' || escaped == '`') {
            if (escaped == '$') value += '\x0b';
            value += escaped;
          } else {
            value += ch;
            value += escaped;
          }
        } else {
          value += ch;
        }
      }
      current_ = {std::move(value), closed ? Token::String : Token::Operator,
                  quote == '"' ? "double" : "single"};
      if (!closed) current_.text = "unterminated quoted arithmetic subscript";
      return;
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {
      const size_t begin = cursor_++;
      while (cursor_ < source_.size() &&
             (std::isalnum(static_cast<unsigned char>(source_[cursor_])) ||
              source_[cursor_] == '#' || source_[cursor_] == '@' ||
              source_[cursor_] == '_')) ++cursor_;
      current_ = {source_.substr(begin, cursor_ - begin), Token::Number, {}};
      return;
    }
    if (c == '$') {
      const size_t begin = cursor_;
      std::string suffix;
      ++cursor_;
      if (cursor_ < source_.size() && source_[cursor_] == '{') {
        ++cursor_;
        while (cursor_ < source_.size() && source_[cursor_] != '}') ++cursor_;
        if (cursor_ < source_.size()) ++cursor_;
        const size_t parameter_end = cursor_;
        const std::string braced_name = source_.substr(
            begin + 2, parameter_end > begin + 3 ? parameter_end - begin - 3 : 0);
        if (!braced_name.empty() &&
            std::all_of(braced_name.begin(), braced_name.end(),
                        [](unsigned char ch) { return std::isdigit(ch); })) {
          const size_t suffix_begin = cursor_;
          while (cursor_ < source_.size() &&
                 std::isdigit(static_cast<unsigned char>(source_[cursor_]))) ++cursor_;
          suffix = source_.substr(suffix_begin, cursor_ - suffix_begin);
        }
      } else if (cursor_ < source_.size() &&
                 std::isdigit(static_cast<unsigned char>(source_[cursor_]))) {
        // Outside braces, a positional parameter is exactly one digit.
        ++cursor_;
        const size_t suffix_begin = cursor_;
        while (cursor_ < source_.size() &&
               std::isdigit(static_cast<unsigned char>(source_[cursor_]))) ++cursor_;
        suffix = source_.substr(suffix_begin, cursor_ - suffix_begin);
      } else if (cursor_ < source_.size() &&
                 std::string("$#?!@*-").find(source_[cursor_]) != std::string::npos) {
        ++cursor_;
      } else {
        while (cursor_ < source_.size() &&
               (std::isalnum(static_cast<unsigned char>(source_[cursor_])) || source_[cursor_] == '_')) ++cursor_;
      }
      current_ = {source_.substr(begin, cursor_ - begin - suffix.size()),
                  Token::Name, std::move(suffix)};
      return;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      const size_t begin = cursor_++;
      while (cursor_ < source_.size() &&
             (std::isalnum(static_cast<unsigned char>(source_[cursor_])) || source_[cursor_] == '_')) ++cursor_;
      current_ = {source_.substr(begin, cursor_ - begin), Token::Name, {}};
      return;
    }
    static const std::vector<std::string> operators = {
        "<<=", ">>=", "++", "--", "**", "<=", ">=", "==", "!=",
        "&&", "||", "<<", ">>", "+=", "-=", "*=", "/=", "%=", "&=", "^=", "|="};
    for (const auto &op : operators) {
      if (source_.compare(cursor_, op.size(), op) == 0) {
        cursor_ += op.size();
        current_ = {op, Token::Operator, {}};
        return;
      }
    }
    current_ = {std::string(1, c), Token::Operator, {}};
    ++cursor_;
  }
  bool take(const std::string &token) {
    if (current_.text != token) return false;
    next();
    return true;
  }
  static int precedence(const std::string &op) {
    if (op == ",") return 1;
    if (op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=" || op == "%=" ||
        op == "<<=" || op == ">>=" || op == "&=" || op == "^=" || op == "|=") return 2;
    if (op == "?") return 3;
    if (op == "||") return 4;
    if (op == "&&") return 5;
    if (op == "|") return 6;
    if (op == "^") return 7;
    if (op == "&") return 8;
    if (op == "==" || op == "!=") return 9;
    if (op == "<" || op == "<=" || op == ">" || op == ">=") return 10;
    if (op == "<<" || op == ">>") return 11;
    if (op == "+" || op == "-") return 12;
    if (op == "*" || op == "/" || op == "%") return 13;
    if (op == "**") return 15;
    return 0;
  }
  static bool assignment(const std::string &op) { return precedence(op) == 2; }
  std::optional<long long> number(const std::string &text) {
    if (text.empty()) return fail("invalid integer constant");
    const auto parse_digits = [&](int base, size_t begin, bool extended)
        -> std::optional<long long> {
      if (begin >= text.size()) return fail("invalid integer constant");
      std::uint64_t value = 0;
      for (size_t i = begin; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        int digit = std::isdigit(c) ? c - '0' :
                    std::islower(c) ? c - 'a' + 10 :
                    std::isupper(c) ? c - 'A' + (base <= 36 ? 10 : 36) :
                    extended && c == '@' ? 62 : extended && c == '_' ? 63 : -1;
        if (digit < 0 || digit >= base)
          return fail(extended ? "invalid digit in base integer"
                               : "invalid integer constant");
        value = value * static_cast<std::uint64_t>(base) +
                static_cast<std::uint64_t>(digit);
      }
      return wrap(value);
    };

    const size_t hash = text.find('#');
    if (hash != std::string::npos) {
      const std::string base_text = text.substr(0, hash);
      char *base_end = nullptr;
      errno = 0;
      const long base = std::strtol(base_text.c_str(), &base_end, 10);
      if (errno || base_end == base_text.c_str() || *base_end || base < 2 || base > 64)
        return fail("arithmetic base out of range");
      return parse_digits(static_cast<int>(base), hash + 1, true);
    }

    int base = 10;
    size_t begin = 0;
    if (text.size() > 2 && text[0] == '0' &&
        (text[1] == 'x' || text[1] == 'X')) {
      base = 16;
      begin = 2;
    } else if (text.size() > 1 && text[0] == '0') {
      base = 8;
      begin = 1;
    }
    if (begin == text.size()) return 0LL;
    return parse_digits(base, begin, false);
  }
  std::optional<long long> variable_value(const std::string &name, bool *unset = nullptr) {
    auto &state = shell_state();
    auto unset_value = [&](const std::string &variable) -> std::optional<long long> {
      if (state.nounset) {
        if (unset) *unset = true;
        return 0LL;
      }
      (void)variable;
      return 0LL;
    };
    const std::string resolved_name = shell_resolve_nameref(name);
    if (resolved_name.empty()) return unset_value(name);
    if (resolved_name == "$$") return static_cast<long long>(state.shell_pid);
    if (const auto special = shell_arithmetic_special_value(resolved_name))
      return *special;
    const size_t bracket = resolved_name.find('[');
    if (bracket != std::string::npos && resolved_name.back() == ']') {
      const std::string array_name = resolved_name.substr(0, bracket);
      auto associative = state.associative_arrays.find(array_name);
      if (associative != state.associative_arrays.end()) {
        const std::string key = resolved_name.substr(bracket + 1, resolved_name.size() - bracket - 2);
        auto element = associative->second.find(key);
        if (element == associative->second.end()) return unset_value(resolved_name);
        if (element->second.empty()) return 0LL;
        ArithmeticParser nested(element->second, depth_ + 1);
        std::string nested_error;
        auto result = nested.parse(&nested_error);
        if (!result) return fail("invalid associative array value in arithmetic expression: " + nested_error);
        return result;
      }
      char *index_end = nullptr;
      const std::string index_text = resolved_name.substr(bracket + 1, resolved_name.size() - bracket - 2);
      long long index = std::strtoll(index_text.c_str(), &index_end, 10);
      auto array = state.arrays.find(array_name);
      if (index_end == nullptr || *index_end || array == state.arrays.end())
        return unset_value(resolved_name);
      if (index < 0) index += static_cast<long long>(shell_array_index_extent(array_name));
      if (index < 0 || static_cast<size_t>(index) >= array->second.size() ||
          !shell_array_index_is_set(array_name, static_cast<size_t>(index)))
        return unset_value(resolved_name);
      const std::string &element = array->second[static_cast<size_t>(index)];
      if (element.empty()) return 0LL;
      ArithmeticParser nested(element, depth_ + 1);
      std::string nested_error;
      auto result = nested.parse(&nested_error);
      if (!result) return fail("invalid array value in arithmetic expression: " + nested_error);
      return result;
    }
    auto var = state.vars.find(resolved_name);
    auto array = state.arrays.find(resolved_name);
    std::string text;
    if (var != state.vars.end()) text = var->second;
    else if (array != state.arrays.end()) {
      if (!shell_array_index_is_set(resolved_name, 0)) return unset_value(resolved_name);
      text = array->second.empty() ? std::string() : array->second.front();
    } else if (!state.unset_vars.count(resolved_name)) {
      if (const char *environment = std::getenv(resolved_name.c_str())) text = environment;
      else return unset_value(resolved_name);
    } else return unset_value(resolved_name);
    if (text.empty()) return 0LL;
    ArithmeticParser nested(text, depth_ + 1);
    std::string nested_error;
    auto result = nested.parse(&nested_error);
    if (!result) return fail("invalid value for arithmetic variable '" + name + "': " + nested_error);
    return result;
  }
  bool store(const std::string &name, long long value) {
    if (name.empty()) return false;
    auto &state = shell_state();
    const std::string resolved = shell_resolve_nameref(name);
    const std::string &target = resolved.empty() ? name : resolved;
    const size_t bracket = target.find('[');
    if (bracket == std::string::npos && !state.arrays.count(target))
      return set_shell_variable(target, std::to_string(value));
    const std::string array_name = bracket == std::string::npos ? target : target.substr(0, bracket);
    if (bracket != std::string::npos && state.associative_arrays.count(array_name))
      return set_shell_variable(target, std::to_string(value));
    long long index = 0;
    if (bracket != std::string::npos) {
      if (target.back() != ']') return false;
      const std::string index_text = target.substr(bracket + 1, target.size() - bracket - 2);
      char *end = nullptr;
      index = std::strtoll(index_text.c_str(), &end, 10);
      if (end == index_text.c_str() || *end) return false;
    }
    if (shell_variable_readonly(array_name)) return false;
    state.arrays.try_emplace(array_name);
    if (index < 0) index += static_cast<long long>(shell_array_index_extent(array_name));
    if (index < 0 || index > 1000000) return false;
    return set_shell_variable(array_name + "[" + std::to_string(index) + "]",
                              std::to_string(value));
  }
  std::optional<Value> prefix(bool evaluate) {
    if (current_.text == "+" || current_.text == "-" || current_.text == "!" || current_.text == "~" ||
        current_.text == "++" || current_.text == "--") {
      const std::string op = current_.text;
      next();
      // Bash applies a leading sign before exponentiation: -2**2 is
      // evaluated as (-2)**2. Prefix operators also remain valid on exponents.
      auto operand = prefix(evaluate);
      if (!operand) return {};
      if ((op == "++" || op == "--") && operand->lvalue.empty()) {
        fail("increment requires a variable");
        return {};
      }
      if (!evaluate) return Value{};
      if (operand->unset) { fail("unbound variable"); return {}; }
      if (op == "+") operand->value = +operand->value;
      else if (op == "-") operand->value = wrap(std::uint64_t(0) - static_cast<std::uint64_t>(operand->value));
      else if (op == "!") operand->value = !operand->value;
      else if (op == "~") operand->value = ~operand->value;
      else {
        operand->value = wrap(static_cast<std::uint64_t>(operand->value) +
                              (op == "++" ? 1ULL : std::uint64_t(-1)));
        if (!store(operand->lvalue, operand->value)) return {};
      }
      operand->lvalue.clear();
      return operand;
    }
    Value result;
    if (take("(")) {
      auto inner = expression(1, evaluate);
      if (!inner || !take(")")) { fail("missing ) in arithmetic expression"); return {}; }
      result = *inner;
      result.lvalue.clear();
    } else if (current_.kind == Token::Number) {
      if (evaluate) {
        auto value = number(current_.text);
        if (!value) return {};
        result.value = *value;
      }
      next();
    } else if (current_.kind == Token::Name) {
      std::string name = current_.text;
      const std::string parameter_suffix = current_.suffix;
      next();
      const bool shell_pid_parameter = name == "$$";
      const bool dollar_parameter = !name.empty() && name.front() == '$';
      std::string parameter_name = name;
      if (parameter_name.size() >= 3 && parameter_name[0] == '$' &&
          parameter_name[1] == '{' && parameter_name.back() == '}')
        parameter_name = parameter_name.substr(2, parameter_name.size() - 3);
      else if (dollar_parameter && !shell_pid_parameter)
        parameter_name.erase(parameter_name.begin());
      if (!dollar_parameter && name.size() >= 2 && name.front() == '{' && name.back() == '}')
        name = name.substr(1, name.size() - 2);
      else if (dollar_parameter && !shell_pid_parameter)
        name = parameter_name;
      if (name.empty()) { fail("invalid arithmetic variable"); return {}; }
      if (take("[")) {
        if (shell_state().associative_arrays.count(name)) {
          if (current_.kind != Token::Name && current_.kind != Token::Number &&
              current_.kind != Token::String && current_.text != "@" &&
              current_.text != "*") {
            fail("invalid associative array subscript in arithmetic expression");
            return {};
          }
          std::string key = current_.text;
          const bool quoted_key = current_.kind == Token::String;
          const bool double_quoted_key = quoted_key && current_.suffix == "double";
          if (evaluate && double_quoted_key) {
            std::string expanded_key;
            for (size_t i = 0; i < key.size(); ++i) {
              if (key[i] == '\x0b' && i + 1 < key.size()) {
                expanded_key += key[++i];
                continue;
              }
              if (key[i] != '$' || i + 1 >= key.size()) {
                expanded_key += key[i];
                continue;
              }
              std::string parameter;
              size_t end = i + 1;
              if (key[end] == '{') {
                const size_t close = key.find('}', end + 1);
                if (close == std::string::npos) {
                  if (error_ && error_->empty()) *error_ = "invalid parameter in associative subscript";
                  return {};
                }
                parameter = key.substr(end + 1, close - end - 1);
                end = close;
              } else if (std::string("?#@*!$-").find(key[end]) != std::string::npos) {
                parameter = key.substr(end, 1);
              } else if (std::isalpha(static_cast<unsigned char>(key[end])) || key[end] == '_') {
                size_t finish = end + 1;
                while (finish < key.size() &&
                       (std::isalnum(static_cast<unsigned char>(key[finish])) || key[finish] == '_')) ++finish;
                parameter = key.substr(end, finish - end);
                end = finish - 1;
              } else if (std::isdigit(static_cast<unsigned char>(key[end]))) {
                parameter = key.substr(end, 1);
              } else {
                expanded_key += '$';
                continue;
              }
              auto &state = shell_state();
              std::string value;
              if (parameter == "?") value = std::to_string(state.status_value);
              else if (parameter == "#") value = std::to_string(state.positionals.size());
              else if (parameter == "$") value = std::to_string(state.shell_pid);
              else if (parameter == "!") {
                if (state.last_background_pid > 0) value = std::to_string(state.last_background_pid);
              } else if (parameter == "0") value = state.shell_name;
              else if (parameter == "@" || parameter == "*") {
                auto ifs = state.vars.find("IFS");
                const std::string separator = ifs == state.vars.end() ? " " : ifs->second.substr(0, 1);
                for (size_t n = 0; n < state.positionals.size(); ++n) {
                  if (n) value += separator;
                  value += state.positionals[n];
                }
              } else {
                const std::string resolved = shell_resolve_nameref(parameter);
                const auto variable = state.vars.find(resolved.empty() ? parameter : resolved);
                if (variable != state.vars.end()) value = variable->second;
                else if (const char *environment = std::getenv(parameter.c_str())) value = environment;
                else if (state.nounset) {
                  if (error_ && error_->empty()) *error_ = "unbound variable in associative subscript: " + parameter;
                  return {};
                }
              }
              expanded_key += value;
              i = end;
            }
            key = std::move(expanded_key);
          }
          if (!quoted_key) {
            if (key.size() >= 2 && key.front() == '$') key.erase(key.begin());
            if (key.size() >= 2 && key.front() == '{' && key.back() == '}')
              key = key.substr(1, key.size() - 2);
          }
          if (evaluate && !quoted_key) {
            key = shell_resolve_nameref(key);
            auto value = shell_state().vars.find(key);
            if (value != shell_state().vars.end()) key = value->second;
          }
          next();
          if (!take("]")) { fail("invalid associative array subscript in arithmetic expression"); return {}; }
          name += "[" + key + "]";
        } else {
          std::optional<Value> index;
          if (current_.kind == Token::String) {
            const std::string index_source = current_.text;
            next();
            ArithmeticParser nested(index_source, depth_ + 1);
            std::string nested_error;
            auto index_value = nested.parse(&nested_error, evaluate);
            if (!index_value) {
              fail("invalid array subscript in arithmetic expression: " + nested_error);
              return {};
            }
            index = Value{*index_value, {}, false};
          } else {
            index = expression(1, evaluate);
          }
          if (!index || !take("]")) { fail("invalid array subscript in arithmetic expression"); return {}; }
          name += "[" + std::to_string(index->value) + "]";
        }
      }
      if (evaluate) {
        auto &state = shell_state();
        if (dollar_parameter && parameter_name.size() > 1 &&
            parameter_name.front() == '#') {
          std::string target = parameter_name.substr(1);
          const size_t bracket = target.find('[');
          std::string value;
          bool is_array_length = false;
          if (bracket != std::string::npos && target.back() == ']') {
            const std::string raw_base = target.substr(0, bracket);
            const std::string resolved_base = shell_resolve_nameref(raw_base);
            const std::string base = resolved_base.empty() ? raw_base : resolved_base;
            const std::string index = target.substr(bracket + 1,
                                                    target.size() - bracket - 2);
            const auto associative = state.associative_arrays.find(base);
            const auto indexed = state.arrays.find(base);
            if (index == "@" || index == "*") {
              if (associative != state.associative_arrays.end()) {
                result.value = static_cast<long long>(associative->second.size());
                is_array_length = true;
              } else if (indexed != state.arrays.end()) {
                const auto set_indices = state.array_set_indices.find(base);
                result.value = set_indices == state.array_set_indices.end()
                                   ? static_cast<long long>(indexed->second.size())
                                   : static_cast<long long>(set_indices->second.size());
                is_array_length = true;
              }
            } else if (associative != state.associative_arrays.end()) {
              std::string key = index;
              const bool single_quoted = key.size() >= 2 && key.front() == '\'' &&
                                         key.back() == '\'';
              const bool double_quoted = key.size() >= 2 && key.front() == '"' &&
                                         key.back() == '"';
              if (single_quoted || double_quoted) {
                key = key.substr(1, key.size() - 2);
              }
              bool expand_key = !single_quoted;
              if (key.size() >= 3 && key.front() == '$' && key[1] == '{' &&
                  key.back() == '}') {
                key = key.substr(2, key.size() - 3);
              } else if (key.size() > 1 && key.front() == '$') {
                key.erase(key.begin());
              } else if (double_quoted) {
                expand_key = false;
              }
              if (expand_key) {
                const std::string resolved_key = shell_resolve_nameref(key);
                const auto key_variable = state.vars.find(
                    resolved_key.empty() ? key : resolved_key);
                if (key_variable != state.vars.end()) key = key_variable->second;
                else if (const char *environment = std::getenv(key.c_str())) key = environment;
              }
              const auto element = associative->second.find(key);
              if (element != associative->second.end()) value = element->second;
              else if (state.nounset) result.unset = true;
            } else if (indexed != state.arrays.end()) {
              ArithmeticParser subscript(index, depth_ + 1);
              std::string subscript_error;
              const auto parsed_index = subscript.parse(&subscript_error);
              if (!parsed_index) {
                fail("invalid array subscript in parameter length expansion: " +
                     subscript_error);
                return {};
              }
              long long element_index = *parsed_index;
              if (element_index < 0)
                element_index += static_cast<long long>(shell_array_index_extent(base));
              if (element_index >= 0 &&
                  static_cast<size_t>(element_index) < indexed->second.size() &&
                  shell_array_index_is_set(base, static_cast<size_t>(element_index))) {
                value = indexed->second[static_cast<size_t>(element_index)];
              } else if (state.nounset) {
                result.unset = true;
              }
            } else if (state.nounset) {
              result.unset = true;
            }
          } else {
            if (target == "@" || target == "*") {
              result.value = static_cast<long long>(state.positionals.size());
              is_array_length = true;
            } else if (target == "#") {
              value = std::to_string(state.positionals.size());
            } else if (target == "?") {
              value = std::to_string(state.status_value);
            } else if (target == "$") {
              value = std::to_string(state.shell_pid);
            } else if (target == "!") {
              if (state.last_background_pid > 0)
                value = std::to_string(state.last_background_pid);
              else if (state.nounset)
                result.unset = true;
            } else if (!target.empty() &&
                       std::all_of(target.begin(), target.end(),
                                   [](unsigned char ch) { return std::isdigit(ch); })) {
              char *end = nullptr;
              errno = 0;
              const auto position = std::strtoull(target.c_str(), &end, 10);
              if (errno || end == target.c_str() || *end) {
                result.unset = true;
              } else if (position == 0) {
                value = state.shell_name;
              } else if (position <= state.positionals.size()) {
                value = state.positionals[static_cast<size_t>(position - 1)];
              } else if (state.nounset) {
                result.unset = true;
              }
            } else {
              const auto special = shell_arithmetic_special_string(target);
              if (special) {
                value = *special;
              } else {
                const std::string resolved = shell_resolve_nameref(target);
                const auto variable = state.vars.find(resolved.empty() ? target : resolved);
                if (variable != state.vars.end()) value = variable->second;
                else if (const char *environment = std::getenv(target.c_str())) value = environment;
                else if (state.nounset) result.unset = true;
              }
            }
          }
          if (!is_array_length) {
            std::mbstate_t conversion_state{};
            size_t offset = 0;
            long long characters = 0;
            while (offset < value.size()) {
              wchar_t character = 0;
              const size_t consumed = std::mbrtowc(&character, value.data() + offset,
                                                    value.size() - offset,
                                                    &conversion_state);
              if (consumed == static_cast<size_t>(-1) ||
                  consumed == static_cast<size_t>(-2)) {
                ++offset;
                ++characters;
                conversion_state = {};
              } else if (consumed == 0) {
                ++offset;
                ++characters;
              } else {
                offset += consumed;
                ++characters;
              }
            }
            result.value = characters;
          }
        } else if (dollar_parameter && parameter_name == "#") {
          result.value = static_cast<long long>(state.positionals.size());
        } else if (dollar_parameter && parameter_name == "?") {
          result.value = state.status_value;
        } else if (dollar_parameter && parameter_name == "!") {
          if (state.last_background_pid > 0)
            result.value = state.last_background_pid;
          else
            result.unset = true;
        } else if (dollar_parameter &&
                   (parameter_name == "@" || parameter_name == "*")) {
          if (state.positionals.size() > 1) {
            fail("invalid arithmetic expression");
            return {};
          }
          if (state.positionals.empty() || state.positionals.front().empty()) {
            result.value = 0;
          } else {
            ArithmeticParser nested(state.positionals.front(), depth_ + 1);
            std::string nested_error;
            auto parsed = nested.parse(&nested_error);
            if (!parsed) {
              fail("invalid arithmetic positional parameters: " + nested_error);
              return {};
            }
            result.value = *parsed;
          }
        } else if (dollar_parameter &&
                   !parameter_name.empty() &&
                   std::all_of(parameter_name.begin(), parameter_name.end(),
                               [](unsigned char ch) { return std::isdigit(ch); })) {
          char *index_end = nullptr;
          errno = 0;
          const unsigned long long index = std::strtoull(parameter_name.c_str(), &index_end, 10);
          std::string text;
          if (errno || index_end == parameter_name.c_str() || *index_end) {
            result.unset = true;
          } else if (index == 0) {
            text = state.shell_name;
          } else if (index <= state.positionals.size()) {
            text = state.positionals[static_cast<size_t>(index - 1)];
          } else {
            result.unset = true;
          }
          text += parameter_suffix;
          if (text.empty()) {
            result.value = 0;
          } else {
            ArithmeticParser nested(text, depth_ + 1);
            std::string nested_error;
            auto parsed = nested.parse(&nested_error);
            if (!parsed) {
              fail("invalid value for arithmetic positional parameter: " + nested_error);
              return {};
            }
            result.value = *parsed;
          }
        } else {
          auto value = variable_value(name, &result.unset);
          if (!value) return {};
          result.value = *value;
        }
      }
      result.lvalue = (shell_pid_parameter || dollar_parameter) ? std::string() : name;
    } else {
      fail("expected value in arithmetic expression");
      return {};
    }
    while (current_.text == "++" || current_.text == "--") {
      const std::string op = current_.text;
      next();
      if (result.lvalue.empty()) { fail("increment requires a variable"); return {}; }
      if (evaluate) {
        if (result.unset) { fail("unbound variable"); return {}; }
        const long long old = result.value;
        result.value = wrap(static_cast<std::uint64_t>(result.value) +
                             (op == "++" ? 1ULL : std::uint64_t(-1)));
        if (!store(result.lvalue, result.value)) return {};
        result.value = old;
      }
      result.lvalue.clear();
    }
    return result;
  }
  std::optional<Value> expression(int min_precedence, bool evaluate) {
    auto left = prefix(evaluate);
    if (!left) return {};
    while (true) {
      const std::string op = current_.text;
      const int level = precedence(op);
      if (level < min_precedence || level == 0) break;
      next();
      if (op == "?") {
        if (evaluate && left->unset) { fail("unbound variable"); return {}; }
        const bool choose_true = evaluate && left->value != 0;
        auto yes = expression(1, choose_true);
        if (!yes || !take(":")) { fail("missing : in arithmetic conditional"); return {}; }
        // The false arm accepts an assignment expression, but the lower-
        // precedence comma operator remains outside the conditional.
        auto no = expression(level - 1, evaluate && !choose_true);
        if (!no) return {};
        left->value = !evaluate ? 0 : choose_true ? yes->value : no->value;
        left->unset = evaluate && (choose_true ? yes->unset : no->unset);
        left->lvalue.clear();
        continue;
      }
      const int next_level = (assignment(op) || op == "**") ? level : level + 1;
      bool evaluate_right = evaluate;
      if (op == "&&" && left->value == 0) evaluate_right = false;
      if (op == "||" && left->value != 0) evaluate_right = false;
      auto right = expression(next_level, evaluate_right);
      if (!right) return {};
      if (!evaluate) { left = Value{}; continue; }
      if (assignment(op)) {
        if (left->lvalue.empty()) { fail("assignment requires a variable"); return {}; }
        if (right->unset || (op != "=" && left->unset)) {
          fail("unbound variable");
          return {};
        }
        long long value = right->value;
        if (op != "=") {
          const std::string base = op.substr(0, op.size() - 1);
          if (base == "+") value = wrap(static_cast<std::uint64_t>(left->value) + static_cast<std::uint64_t>(right->value));
          else if (base == "-") value = wrap(static_cast<std::uint64_t>(left->value) - static_cast<std::uint64_t>(right->value));
          else if (base == "*") value = wrap(static_cast<std::uint64_t>(left->value) * static_cast<std::uint64_t>(right->value));
          else if (base == "/" || base == "%") {
            if (!right->value) { fail("division by zero"); return {}; }
            if (left->value == std::numeric_limits<long long>::min() && right->value == -1)
              value = base == "/" ? left->value : 0;
            else value = base == "/" ? left->value / right->value : left->value % right->value;
          } else if (base == "<<" || base == ">>") {
            if (right->value < 0 || right->value >= 64) { fail("invalid shift count"); return {}; }
            value = base == "<<" ? wrap(static_cast<std::uint64_t>(left->value) << right->value)
                                  : arithmetic_right_shift(left->value, static_cast<unsigned>(right->value));
          }
          else if (base == "&") value = left->value & right->value;
          else if (base == "^") value = left->value ^ right->value;
          else value = left->value | right->value;
        }
        if (!store(left->lvalue, value)) return {};
        left->value = value;
        left->unset = false;
        left->lvalue.clear();
        continue;
      }
      if (left->unset || right->unset) { fail("unbound variable"); return {}; }
      const long long a = left->value, b = right->value;
      if (op == ",") left->value = b;
      else if (op == "||") left->value = a || b;
      else if (op == "&&") left->value = a && b;
      else if (op == "|") left->value = a | b;
      else if (op == "^") left->value = a ^ b;
      else if (op == "&") left->value = a & b;
      else if (op == "==") left->value = a == b;
      else if (op == "!=") left->value = a != b;
      else if (op == "<") left->value = a < b;
      else if (op == "<=") left->value = a <= b;
      else if (op == ">") left->value = a > b;
      else if (op == ">=") left->value = a >= b;
      else if (op == "<<" || op == ">>") {
        if (b < 0 || b >= 64) { fail("invalid shift count"); return {}; }
        left->value = op == "<<" ? wrap(static_cast<std::uint64_t>(a) << b)
                                  : arithmetic_right_shift(a, static_cast<unsigned>(b));
      } else if (op == "+") left->value = wrap(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b));
      else if (op == "-") left->value = wrap(static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b));
      else if (op == "*") left->value = wrap(static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b));
      else if (op == "/" || op == "%") {
        if (!b) { fail("division by zero"); return {}; }
        if (a == std::numeric_limits<long long>::min() && b == -1)
          left->value = op == "/" ? a : 0;
        else left->value = op == "/" ? a / b : a % b;
      } else if (op == "**") {
        if (b < 0) { fail("negative exponent"); return {}; }
        std::uint64_t value = 1, base = static_cast<std::uint64_t>(a);
        for (long long exponent = b; exponent; exponent >>= 1) {
          if (exponent & 1) value *= base;
          if (exponent > 1) base *= base;
        }
        left->value = wrap(value);
      }
      left->lvalue.clear();
    }
    return left;
  }
};
} // namespace

std::optional<long long> evaluate_arithmetic(const std::string &expression,
                                             std::string *error) {
  if (error) error->clear();
  ArithmeticParser parser(expression);
  return parser.parse(error);
}
