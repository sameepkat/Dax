#include "builtins_internal.hpp"
#include "arithmetic.hpp"
#include "file_access.hpp"
#include "file_times.hpp"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
const std::time_t printf_shell_start_time = std::time(nullptr);
}

std::optional<int> run_output_builtin(const std::vector<std::string> &a) {
  if (a.empty()) return 0;
  const auto &c = a[0];
  if (c == "echo") {
    size_t i = 1;
    bool nl = true;
    bool interpret_escapes = shell_state().xpg_echo;
    bool stop_output = false;
    if (!shell_state().xpg_echo) {
      for (; i < a.size(); ++i) {
        const std::string &option = a[i];
        if (option.size() < 2 || option[0] != '-') break;
        bool valid = true;
        for (size_t j = 1; j < option.size(); ++j)
          if (option[j] != 'n' && option[j] != 'e' && option[j] != 'E') valid = false;
        if (!valid) break;
        for (size_t j = 1; j < option.size(); ++j) {
          if (option[j] == 'n') nl = false;
          else interpret_escapes = option[j] == 'e';
        }
      }
    }
    auto echo_argument = [&](const std::string &argument) {
      for (size_t j = 0; j < argument.size(); ++j) {
        if (!interpret_escapes || argument[j] != '\\' || j + 1 == argument.size()) {
          std::cout << argument[j];
          continue;
        }
        const char escaped = argument[++j];
        switch (escaped) {
        case 'a': std::cout << '\a'; break;
        case 'b': std::cout << '\b'; break;
        case 'c': stop_output = true; return;
        case 'e': case 'E': std::cout << '\033'; break;
        case 'f': std::cout << '\f'; break;
        case 'n': std::cout << '\n'; break;
        case 'r': std::cout << '\r'; break;
        case 't': std::cout << '\t'; break;
        case 'v': std::cout << '\v'; break;
        case '\\': std::cout << '\\'; break;
        case '0': {
          unsigned value = 0;
          size_t digits = 0;
          while (digits < 3 && j + 1 < argument.size() &&
                 argument[j + 1] >= '0' && argument[j + 1] <= '7') {
            value = value * 8 + static_cast<unsigned>(argument[++j] - '0');
            ++digits;
          }
          std::cout << static_cast<char>(value & 0xff);
          break;
        }
        case 'x': {
          unsigned value = 0;
          size_t digits = 0;
          while (digits < 2 && j + 1 < argument.size() &&
                 std::isxdigit(static_cast<unsigned char>(argument[j + 1]))) {
            const unsigned char digit = static_cast<unsigned char>(argument[++j]);
            value = value * 16 + (digit <= '9' ? digit - '0' :
                std::tolower(digit) - 'a' + 10);
            ++digits;
          }
          if (digits == 0) std::cout << "\\x";
          else std::cout << static_cast<char>(value & 0xff);
          break;
        }
        default: std::cout << '\\' << escaped; break;
        }
      }
    };
    for (size_t j = i; j < a.size(); ++j) {
      if (stop_output) break;
      if (j > i) std::cout << ' ';
      echo_argument(a[j]);
    }
    if (nl && !stop_output)
      std::cout << '\n';
    return 0;
  }
  if (c == "printf") {
    size_t arg = 1;
    std::string variable;
    auto valid_assignment_target = [](const std::string &name) {
      if (shell_valid_name(name)) return true;
      const size_t bracket = name.find('[');
      return bracket != std::string::npos && !name.empty() && name.back() == ']' &&
             bracket > 0 && shell_valid_name(name.substr(0, bracket)) &&
             bracket + 1 < name.size() - 1;
    };
    if (arg < a.size() && a[arg] == "-v") {
      if (arg + 2 >= a.size()) return 2;
      const std::string assignment_name = a[arg + 1];
      if (!valid_assignment_target(assignment_name)) return 2;
      variable = assignment_name;
      arg += 2;
    } else if (arg < a.size() && a[arg] == "--") {
      ++arg;
    }
    if (arg >= a.size()) return 0;
    const std::string &format = a[arg++];
    std::ostringstream output;
    bool failed = false, stop_output = false, first_pass = true;
    auto unescape = [](const std::string &input, bool &stop) {
      std::string result;
      for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '\\' || i + 1 == input.size()) { result += input[i]; continue; }
        char ch = input[++i];
        switch (ch) {
        case 'a': result += '\a'; break; case 'b': result += '\b'; break;
        case 'e': case 'E': result += '\x1b'; break; case 'f': result += '\f'; break;
        case 'n': result += '\n'; break; case 'r': result += '\r'; break;
        case 't': result += '\t'; break; case 'v': result += '\v'; break;
        case '\\': result += '\\'; break;
        case 'c': stop = true; return result;
        case 'x': case 'u': case 'U': {
          int digits = ch == 'x' ? 2 : ch == 'u' ? 4 : 8;
          unsigned long value = 0; int count = 0;
          while (count < digits && i + 1 < input.size() &&
                 std::isxdigit(static_cast<unsigned char>(input[i + 1]))) {
            const char digit = input[++i];
            value = value * 16 + (digit <= '9' ? digit - '0' : std::tolower(static_cast<unsigned char>(digit)) - 'a' + 10);
            ++count;
          }
          if (count == 0) {
            result += '\\';
            result += ch;
            break;
          }
          if (ch == 'x' || value <= 0x7f) result += static_cast<char>(value & 0xff);
          else if (value <= 0x7ff) {
            result += static_cast<char>(0xc0 | (value >> 6));
            result += static_cast<char>(0x80 | (value & 0x3f));
          } else if (value <= 0xffff) {
            result += static_cast<char>(0xe0 | (value >> 12));
            result += static_cast<char>(0x80 | ((value >> 6) & 0x3f));
            result += static_cast<char>(0x80 | (value & 0x3f));
          } else if (value <= 0x10ffff) {
            result += static_cast<char>(0xf0 | (value >> 18));
            result += static_cast<char>(0x80 | ((value >> 12) & 0x3f));
            result += static_cast<char>(0x80 | ((value >> 6) & 0x3f));
            result += static_cast<char>(0x80 | (value & 0x3f));
          }
          break;
        }
        case '0': {
          unsigned value = 0; int count = 0;
          while (count < 3 && i + 1 < input.size() && input[i + 1] >= '0' && input[i + 1] <= '7') {
            value = value * 8 + static_cast<unsigned>(input[++i] - '0'); ++count;
          }
          result += static_cast<char>(value & 0xff); break;
        }
        default: result += '\\'; result += ch; break;
        }
      }
      return result;
    };
    auto shell_quote = [](const std::string &value, bool force_single_quotes) {
      if (force_single_quotes) {
        std::string quoted = "'";
        for (char ch : value) {
          if (ch == '\'') quoted += "'\\''";
          else quoted += ch;
        }
        quoted += '\'';
        return quoted;
      }
      if (value.empty()) return std::string("''");
      bool needs_ansi_c = false;
      std::mbstate_t character_state{};
      for (size_t offset = 0; offset < value.size();) {
        wchar_t character = 0;
        const size_t length = std::mbrtowc(
            &character, value.data() + offset, value.size() - offset,
            &character_state);
        if (length == static_cast<size_t>(-1) ||
            length == static_cast<size_t>(-2) || length == 0 ||
            !std::iswprint(character)) {
          needs_ansi_c = true;
          break;
        }
        offset += length;
      }
      if (needs_ansi_c) {
        static constexpr char hex[] = "0123456789abcdef";
        std::string quoted = "$'";
        for (unsigned char ch : value) {
          switch (ch) {
          case '\\': quoted += "\\\\"; break;
          case '\'': quoted += "\\'"; break;
          case '\a': quoted += "\\a"; break;
          case '\b': quoted += "\\b"; break;
          case '\x1b': quoted += "\\e"; break;
          case '\f': quoted += "\\f"; break;
          case '\n': quoted += "\\n"; break;
          case '\r': quoted += "\\r"; break;
          case '\t': quoted += "\\t"; break;
          case '\v': quoted += "\\v"; break;
          default:
            if (std::isprint(ch)) quoted += static_cast<char>(ch);
            else {
              quoted += "\\x";
              quoted += hex[ch >> 4];
              quoted += hex[ch & 0x0f];
            }
            break;
          }
        }
        quoted += '\'';
        return quoted;
      }
      auto safe = [](unsigned char ch) {
        return ch >= 0x80 || std::isalnum(ch) ||
               std::string("_@%+=:,./-").find(static_cast<char>(ch)) != std::string::npos;
      };
      std::string quoted;
      for (unsigned char ch : value) {
        if (!safe(ch)) quoted += '\\';
        quoted += static_cast<char>(ch);
      }
      return quoted;
    };
    auto argument = [&](bool numeric_default = false) -> std::string {
      if (arg < a.size()) return a[arg++];
      return numeric_default ? "0" : "";
    };
    while (first_pass || arg < a.size()) {
      first_pass = false;
      const size_t args_before = arg;
      for (size_t i = 0; i < format.size() && !stop_output; ++i) {
        if (format[i] == '\\') {
          bool stop = false;
          size_t escape_end = i + 1;
          if (escape_end < format.size() && format[escape_end] == '0') {
            ++escape_end;
            for (int count = 0; count < 3 && escape_end < format.size() &&
                 format[escape_end] >= '0' && format[escape_end] <= '7'; ++count) ++escape_end;
          } else if (escape_end < format.size() &&
                     (format[escape_end] == 'x' || format[escape_end] == 'u' || format[escape_end] == 'U')) {
            const char kind = format[escape_end++];
            const int limit = kind == 'x' ? 2 : kind == 'u' ? 4 : 8;
            for (int count = 0; count < limit && escape_end < format.size() &&
                 std::isxdigit(static_cast<unsigned char>(format[escape_end])); ++count) ++escape_end;
          } else if (escape_end < format.size()) ++escape_end;
          output << unescape(format.substr(i, escape_end - i), stop);
          stop_output = stop;
          i = escape_end ? escape_end - 1 : i;
          continue;
        }
        if (format[i] != '%') { output << format[i]; continue; }
        if (i + 1 < format.size() && format[i + 1] == '%') { output << '%'; ++i; continue; }
        const size_t directive_start = i;
        ++i;
        std::string flags;
        while (i < format.size() && std::string("#0- +'").find(format[i]) != std::string::npos) flags += format[i++];
        std::string width;
        if (i < format.size() && format[i] == '*') {
          const std::string width_arg = argument(true);
          char *end = nullptr; long parsed = std::strtol(width_arg.c_str(), &end, 10);
          if (!end || *end) parsed = 0;
          if (parsed < 0) { flags += '-'; parsed = -parsed; }
          width = std::to_string(std::min(parsed, 100000L)); ++i;
        } else {
          while (i < format.size() && std::isdigit(static_cast<unsigned char>(format[i]))) width += format[i++];
        }
        bool precision_set = false;
        std::string precision;
        if (i < format.size() && format[i] == '.') {
          precision_set = true; ++i;
          if (i < format.size() && format[i] == '*') {
            const std::string precision_arg = argument(true);
            char *end = nullptr; long parsed = std::strtol(precision_arg.c_str(), &end, 10);
            if (!end || *end) parsed = 0;
            if (parsed >= 0) precision = std::to_string(std::min(parsed, 100000L));
            else precision_set = false;
            ++i;
          } else {
            while (i < format.size() && std::isdigit(static_cast<unsigned char>(format[i]))) precision += format[i++];
          }
        }
        std::string length_modifier;
        while (i < format.size() && std::string("hlLjzt").find(format[i]) != std::string::npos)
          length_modifier += format[i++];
        if (i >= format.size()) { output << format.substr(directive_start); break; }
        const char conversion = format[i];
        std::string spec = "%" + flags + width;
        if (precision_set) spec += "." + precision;
        std::string value;
        if (conversion == '(') {
          const size_t end = format.find(")T", i + 1);
          if (end == std::string::npos) {
            std::cerr << "printf: missing closing ')' in format\n";
            failed = true;
            break;
          }
          const std::string date_format = format.substr(i + 1, end - i - 1);
          long long timestamp = -1;
          if (arg < a.size()) {
            const std::string raw = argument();
            char *end_number = nullptr;
            errno = 0;
            timestamp = std::strtoll(raw.c_str(), &end_number, 0);
            if (raw.empty() || end_number == raw.c_str() || !end_number || *end_number || errno == ERANGE) {
              std::cerr << "printf: " << raw << ": invalid number\n";
              failed = true;
              timestamp = 0;
            }
          }
          const std::time_t when = timestamp == -1 ? std::time(nullptr) :
                                   timestamp == -2 ? printf_shell_start_time : static_cast<std::time_t>(timestamp);
          std::string date_value;
          std::tm *local = std::localtime(&when);
          if (local) {
            std::vector<char> buffer(65536);
            const size_t count = std::strftime(buffer.data(), buffer.size(), date_format.c_str(), local);
            date_value.assign(buffer.data(), count);
          }
          if (precision_set) {
            const size_t maximum = precision.empty()
                ? 0 : static_cast<size_t>(std::strtoull(precision.c_str(), nullptr, 10));
            if (date_value.size() > maximum) date_value.resize(maximum);
          }
          const size_t field_width = width.empty()
              ? 0 : static_cast<size_t>(std::min<unsigned long long>(
                    std::strtoull(width.c_str(), nullptr, 10), 100000ULL));
          const size_t padding = field_width > date_value.size()
              ? field_width - date_value.size() : 0;
          if (flags.find('-') == std::string::npos) output << std::string(padding, ' ');
          output << date_value;
          if (flags.find('-') != std::string::npos) output << std::string(padding, ' ');
          i = end + 1;
          continue;
        } else if (conversion == 's' || conversion == 'S') {
          value = argument();
          if (conversion == 's' && length_modifier != "l") {
            spec += 's';
            std::vector<char> buffer(100000 + value.size() + 2);
            const int count = std::snprintf(buffer.data(), buffer.size(), spec.c_str(), value.c_str());
            if (count < 0) { failed = true; continue; }
            output.write(buffer.data(), std::min<size_t>(static_cast<size_t>(count), buffer.size() - 1));
          } else {
            std::mbstate_t state{};
            const char *source = value.c_str();
            const size_t required = std::mbsrtowcs(nullptr, &source, 0, &state);
            if (required == static_cast<size_t>(-1)) {
              std::cerr << "printf: invalid multibyte string\n";
              failed = true;
              continue;
            }
            std::vector<wchar_t> wide(required + 1);
            state = {};
            source = value.c_str();
            if (std::mbsrtowcs(wide.data(), &source, wide.size(), &state) == static_cast<size_t>(-1)) {
              std::cerr << "printf: invalid multibyte string\n";
              failed = true;
              continue;
            }
            size_t character_count = required;
            if (precision_set) {
              const size_t maximum = precision.empty()
                  ? 0 : static_cast<size_t>(std::strtoull(precision.c_str(), nullptr, 10));
              character_count = std::min(character_count, maximum);
              wide[character_count] = L'\0';
            }
            std::mbstate_t output_state{};
            const wchar_t *wide_source = wide.data();
            const size_t byte_count = std::wcsrtombs(nullptr, &wide_source, 0, &output_state);
            if (byte_count == static_cast<size_t>(-1)) {
              std::cerr << "printf: invalid multibyte string\n";
              failed = true;
              continue;
            }
            std::vector<char> buffer(byte_count + 1);
            output_state = {};
            wide_source = wide.data();
            if (std::wcsrtombs(buffer.data(), &wide_source, buffer.size(), &output_state) ==
                static_cast<size_t>(-1)) {
              std::cerr << "printf: invalid multibyte string\n";
              failed = true;
              continue;
            }
            const size_t field_width = width.empty()
                ? 0 : static_cast<size_t>(std::min<unsigned long long>(
                      std::strtoull(width.c_str(), nullptr, 10), 100000ULL));
            const size_t padding = field_width > character_count
                ? field_width - character_count : 0;
            if (flags.find('-') == std::string::npos) output << std::string(padding, ' ');
            output.write(buffer.data(), static_cast<std::streamsize>(byte_count));
            if (flags.find('-') != std::string::npos) output << std::string(padding, ' ');
          }
        } else if (conversion == 'c' || conversion == 'C') {
          value = argument();
          if (conversion == 'C' || length_modifier == "l") {
            std::mbstate_t state{};
            const char *source = value.c_str();
            wchar_t character = 0;
            if (!value.empty()) {
              const size_t converted = std::mbrtowc(&character, source, value.size(), &state);
              if (converted == static_cast<size_t>(-1) || converted == static_cast<size_t>(-2)) {
                std::cerr << "printf: invalid multibyte character\n";
                failed = true;
                continue;
              }
            }
            spec += "lc";
            const int count = std::snprintf(nullptr, 0, spec.c_str(), static_cast<wint_t>(character));
            if (count < 0) { failed = true; continue; }
            std::vector<char> buffer(static_cast<size_t>(count) + 1);
            std::snprintf(buffer.data(), buffer.size(), spec.c_str(), static_cast<wint_t>(character));
            output.write(buffer.data(), static_cast<size_t>(count));
          } else {
            spec += 'c';
            const int count = std::snprintf(nullptr, 0, spec.c_str(), value.empty() ? 0 : static_cast<unsigned char>(value[0]));
            if (count < 0) { failed = true; continue; }
            std::vector<char> buffer(static_cast<size_t>(count) + 1);
            std::snprintf(buffer.data(), buffer.size(), spec.c_str(), value.empty() ? 0 : static_cast<unsigned char>(value[0]));
            output.write(buffer.data(), static_cast<size_t>(count));
          }
        } else if (conversion == 'b') {
          bool stop = false;
          value = unescape(argument(), stop);
          if (precision_set) {
            const size_t maximum = precision.empty()
                ? 0 : static_cast<size_t>(std::strtoull(precision.c_str(), nullptr, 10));
            if (value.size() > maximum) value.resize(maximum);
          }
          size_t field_width = 0;
          if (!width.empty())
            field_width = static_cast<size_t>(std::min<unsigned long long>(
                std::strtoull(width.c_str(), nullptr, 10), 100000ULL));
          const size_t padding = field_width > value.size() ? field_width - value.size() : 0;
          if (flags.find('-') == std::string::npos)
            output << std::string(padding, ' ');
          output.write(value.data(), static_cast<std::streamsize>(value.size()));
          if (flags.find('-') != std::string::npos)
            output << std::string(padding, ' ');
          stop_output = stop;
        } else if (conversion == 'q' || conversion == 'Q') {
          value = argument();
          if (conversion == 'Q' && precision_set) {
            const size_t precision_value = precision.empty() ? 0 : static_cast<size_t>(std::strtoull(precision.c_str(), nullptr, 10));
            if (value.size() > precision_value) value.resize(precision_value);
          }
          const bool force_single_quotes = flags.find('#') != std::string::npos;
          const std::string quoted = shell_quote(value, force_single_quotes);
          std::string quote_flags = flags;
          quote_flags.erase(std::remove(quote_flags.begin(), quote_flags.end(), '#'),
                            quote_flags.end());
          std::string quote_spec = "%" + quote_flags + width;
          if (conversion == 'q' && precision_set)
            quote_spec += "." + precision;
          quote_spec += 's';
          std::vector<char> buffer(100000 + quoted.size() + 2);
          const int count = std::snprintf(buffer.data(), buffer.size(), quote_spec.c_str(), quoted.c_str());
          if (count < 0) { failed = true; continue; }
          output.write(buffer.data(), std::min<size_t>(static_cast<size_t>(count), buffer.size() - 1));
        } else if (conversion == 'n') {
          const std::string name = argument();
          if (!valid_assignment_target(name) ||
              !set_shell_variable(name, std::to_string(output.tellp())))
            failed = true;
        } else if (std::string("diuoxX").find(conversion) != std::string::npos) {
          value = argument(true);
          long long parsed_number = 0;
          bool valid_number = false;
          if (value.size() > 1 && (value.front() == '\'' || value.front() == '"')) {
            std::mbstate_t character_state{};
            wchar_t character = 0;
            const size_t character_size = std::mbrtowc(
                &character, value.data() + 1, value.size() - 1, &character_state);
            if (character_size != static_cast<size_t>(-1) &&
                character_size != static_cast<size_t>(-2) && character_size != 0) {
              parsed_number = static_cast<long long>(character);
              valid_number = true;
            }
          } else {
            char *number_end = nullptr;
            errno = 0;
            parsed_number = std::strtoll(value.c_str(), &number_end, 0);
            valid_number = !value.empty() && number_end != value.c_str() && number_end &&
                           !*number_end && errno != ERANGE;
          }
          if (!valid_number) {
            const bool octal_error = value.size() > 1 && value[0] == '0' &&
                value.find_first_of("89") != std::string::npos;
            std::cerr << "printf: " << value << (octal_error ? ": invalid octal number\n" : ": invalid number\n");
            failed = true; parsed_number = 0;
          }
          if (conversion == 'C') spec += "lc";
          else { spec += "ll"; spec += conversion; }
          const unsigned long long unsigned_value = static_cast<unsigned long long>(parsed_number);
          const int count = conversion == 'C'
              ? std::snprintf(nullptr, 0, spec.c_str(), static_cast<wint_t>(parsed_number))
              : (conversion == 'u' || conversion == 'o' || conversion == 'x' || conversion == 'X')
              ? std::snprintf(nullptr, 0, spec.c_str(), unsigned_value)
              : std::snprintf(nullptr, 0, spec.c_str(), parsed_number);
          if (count < 0) { failed = true; continue; }
          std::vector<char> buffer(static_cast<size_t>(count) + 1);
          if (conversion == 'C')
            std::snprintf(buffer.data(), buffer.size(), spec.c_str(), static_cast<wint_t>(parsed_number));
          else if (conversion == 'u' || conversion == 'o' || conversion == 'x' || conversion == 'X')
            std::snprintf(buffer.data(), buffer.size(), spec.c_str(), unsigned_value);
          else std::snprintf(buffer.data(), buffer.size(), spec.c_str(), parsed_number);
          output.write(buffer.data(), static_cast<size_t>(count));
        } else if (std::string("fFeEgGaA").find(conversion) != std::string::npos) {
          value = argument(true); char *end = nullptr; const double number = std::strtod(value.c_str(), &end);
          if (!end || end == value.c_str() || *end) failed = true;
          spec += conversion;
          const int count = std::snprintf(nullptr, 0, spec.c_str(), number);
          if (count < 0) { failed = true; continue; }
          std::vector<char> buffer(static_cast<size_t>(count) + 1);
          std::snprintf(buffer.data(), buffer.size(), spec.c_str(), number);
          output.write(buffer.data(), static_cast<size_t>(count));
        } else {
          std::cerr << "printf: `" << conversion << "': invalid format character\n";
          failed = true;
        }
      }
      if (arg == args_before || arg >= a.size() || stop_output) break;
    }
    const std::string result = output.str();
    if (!variable.empty()) {
      if (!set_shell_variable(variable, result)) return 1;
    } else std::cout << result;
    return failed ? 1 : 0;
  }
  if (c == "test" || c == "[") {
    size_t end = a.size();
    if (c == "[") {
      if (end < 2 || a.back() != "]") {
        std::cerr << "[: missing ]\n";
        return 2;
      }
      --end;
    }
    auto eval = [&](auto &&self, size_t b, size_t e) -> int {
      const size_t count = e - b;
      if (count == 0)
        return 1;
      // Bash dispatches the one-argument form before interpreting any token
      // as an operator: `test !` is a non-empty string and therefore true.
      if (count == 1)
        return a[b].empty() ? 1 : 0;

      // Bash's test/[ builtins use argument-count-specific rules for the
      // ambiguous 2-4 argument forms. Keep these ahead of the general
      // precedence parser below.
      if (count == 2) {
        if (a[b] == "!") {
          const int result = self(self, b + 1, e);
          return result > 1 ? result : (result == 0 ? 1 : 0);
        }
        // Unary primaries are evaluated by the same implementation used by
        // the precedence parser. With two operands, Bash reports an invalid
        // unary primary instead of treating it as a false expression.
      } else if (count == 3) {
        const auto &op = a[b + 1];
        const bool binary = op == "=" || op == "==" || op == "!=" ||
                            op == "<" || op == ">" || op == "-eq" ||
                            op == "-ne" || op == "-gt" || op == "-ge" ||
                            op == "-lt" || op == "-le" || op == "-nt" ||
                            op == "-ot" || op == "-ef" || op == "-a" ||
                            op == "-o";
        if (binary) {
          if (op == "-a")
            return !a[b].empty() && !a[b + 2].empty() ? 0 : 1;
          if (op == "-o")
            return !a[b].empty() || !a[b + 2].empty() ? 0 : 1;
          // Continue to the binary-primary evaluator below.
        } else if (a[b] == "!") {
          const int result = self(self, b + 1, e);
          return result > 1 ? result : (result == 0 ? 1 : 0);
        } else if (a[b] == "(" && a[e - 1] == ")") {
          return a[b + 1].empty() ? 1 : 0;
        } else {
          return 1;
        }
      } else if (count == 4) {
        if (a[b] == "!") {
          const int result = self(self, b + 1, e);
          return result > 1 ? result : (result == 0 ? 1 : 0);
        }
        if (a[b] == "(" && a[e - 1] == ")")
          return self(self, b + 1, e - 1);
      }

      if (count >= 5 && a[b] == "(" && a[e - 1] == ")")
        return self(self, b + 1, e - 1);
      if (count >= 4 && a[b] == "!") {
        const int result = self(self, b + 1, e);
        return result > 1 ? result : (result == 0 ? 1 : 0);
      }
      if (count == 2) {
        struct stat st {};
        const auto &op = a[b];
        const auto &v = a[b + 1];
        bool ok = false;
        if (op == "-n")
          ok = !v.empty();
        else if (op == "-z")
          ok = v.empty();
        else if (op == "-e" || op == "-a")
          ok = access(v.c_str(), F_OK) == 0;
        else if (op == "-f")
          ok = stat(v.c_str(), &st) == 0 && S_ISREG(st.st_mode);
        else if (op == "-d")
          ok = stat(v.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        else if (op == "-r")
          ok = dax_effective_access(v.c_str(), R_OK);
        else if (op == "-w")
          ok = dax_effective_access(v.c_str(), W_OK);
        else if (op == "-x")
          ok = dax_effective_access(v.c_str(), X_OK);
        else if (op == "-s")
          ok = stat(v.c_str(), &st) == 0 && st.st_size > 0;
        else if (op == "-b")
          ok = stat(v.c_str(), &st) == 0 && S_ISBLK(st.st_mode);
        else if (op == "-c")
          ok = stat(v.c_str(), &st) == 0 && S_ISCHR(st.st_mode);
        else if (op == "-g")
          ok = stat(v.c_str(), &st) == 0 && (st.st_mode & S_ISGID) != 0;
        else if (op == "-k")
          ok = stat(v.c_str(), &st) == 0 && (st.st_mode & S_ISVTX) != 0;
        else if (op == "-p")
          ok = stat(v.c_str(), &st) == 0 && S_ISFIFO(st.st_mode);
        else if (op == "-S")
          ok = stat(v.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
        else if (op == "-L" || op == "-h")
          ok = lstat(v.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
        else if (op == "-u")
          ok = stat(v.c_str(), &st) == 0 && (st.st_mode & S_ISUID) != 0;
        else if (op == "-O")
          ok = stat(v.c_str(), &st) == 0 && st.st_uid == geteuid();
        else if (op == "-G")
          ok = stat(v.c_str(), &st) == 0 && st.st_gid == getegid();
        else if (op == "-t") {
          char *fd_end = nullptr;
          errno = 0;
          const long fd = std::strtol(v.c_str(), &fd_end, 10);
          ok = !errno && fd_end != v.c_str() && *fd_end == '\0' &&
               fd >= 0 && fd <= INT_MAX && isatty(static_cast<int>(fd));
        } else if (op == "-o") {
          ok = shell_option_enabled(v);
        } else if (op == "-R") {
          ok = v.find('[') == std::string::npos &&
               shell_state().namerefs.count(v) != 0;
        } else if (op == "-v") {
          const std::string variable = shell_resolve_nameref(v);
          const size_t bracket = variable.find('[');
          auto &state = shell_state();
          if (variable.empty()) {
            ok = false;
          } else if (bracket != std::string::npos && variable.back() == ']') {
            const std::string array_name = variable.substr(0, bracket);
            const std::string index = variable.substr(bracket + 1,
                                                       variable.size() - bracket - 2);
            auto associative = state.associative_arrays.find(array_name);
            if (associative != state.associative_arrays.end()) {
              ok = associative->second.count(index) != 0;
            } else {
              auto indexed = state.arrays.find(array_name);
              if (indexed != state.arrays.end() && (index == "@" || index == "*")) {
                const auto set_indices = state.array_set_indices.find(array_name);
                ok = set_indices == state.array_set_indices.end()
                         ? !indexed->second.empty()
                         : !set_indices->second.empty();
              } else if (indexed != state.arrays.end()) {
                std::string arithmetic_error;
                const auto parsed = evaluate_arithmetic(index, &arithmetic_error);
                if (parsed) {
                  long long resolved = *parsed;
                  if (resolved < 0)
                    resolved += static_cast<long long>(shell_array_index_extent(array_name));
                  ok = resolved >= 0 && static_cast<size_t>(resolved) < indexed->second.size() &&
                       shell_array_index_is_set(array_name, static_cast<size_t>(resolved));
                }
              }
            }
          } else {
            if (!state.unset_vars.count(variable)) {
              const auto indexed = state.arrays.find(variable);
              const auto associative = state.associative_arrays.find(variable);
              if (indexed != state.arrays.end()) {
                const auto set_indices = state.array_set_indices.find(variable);
                ok = set_indices == state.array_set_indices.end()
                         ? !indexed->second.empty()
                         : !set_indices->second.empty();
              }
              else if (associative != state.associative_arrays.end())
                ok = !associative->second.empty();
              else
                ok = state.vars.count(variable) ||
                     std::getenv(variable.c_str()) != nullptr;
            }
          }
        }
        else if (op == "-N")
          ok = stat(v.c_str(), &st) == 0 && dax_file_modified_since_read(st);
        else {
          std::cerr << "test: " << op << ": unary operator expected\n";
          return 2;
        }
        return ok ? 0 : 1;
      }
      for (const std::string op : {"-o", "-a"})
        for (size_t i = b + 1; i + 1 < e; ++i)
          if (a[i] == op) {
            const int left = self(self, b, i);
            const int right = self(self, i + 1, e);
            if (left > 1) return left;
            if (right > 1) return right;
            return op == "-a" ? (left == 0 && right == 0 ? 0 : 1)
                               : (left == 0 || right == 0 ? 0 : 1);
          }
      if (count != 3)
        return 2;
      const auto &l = a[b];
      const auto &op = a[b + 1];
      const auto &r = a[b + 2];
      if (op == "-nt" || op == "-ot" || op == "-ef") {
        struct stat left_info {}, right_info {};
        const bool left_exists = stat(l.c_str(), &left_info) == 0;
        const bool right_exists = stat(r.c_str(), &right_info) == 0;
        if (op == "-ef")
          return left_exists && right_exists && left_info.st_dev == right_info.st_dev &&
                         left_info.st_ino == right_info.st_ino ? 0 : 1;
        if (op == "-nt")
          return left_exists &&
                         (!right_exists || dax_compare_file_mtime(left_info, right_info) > 0)
                     ? 0 : 1;
        return right_exists &&
                       (!left_exists || dax_compare_file_mtime(left_info, right_info) < 0)
                   ? 0 : 1;
      }
      if (op == "=" || op == "==")
        return l == r ? 0 : 1;
      if (op == "!=")
        return l != r ? 0 : 1;
      if (op == "<" || op == ">") {
        const int comparison = shell_option_enabled("posix")
                                   ? std::strcoll(l.c_str(), r.c_str())
                                   : std::strcmp(l.c_str(), r.c_str());
        return (op == "<" ? comparison < 0 : comparison > 0) ? 0 : 1;
      }
      char *le = nullptr;
      char *re = nullptr;
      errno = 0;
      const long long lv = std::strtoll(l.c_str(), &le, 10);
      const bool left_invalid = errno == ERANGE || le == l.c_str() || !le || *le;
      errno = 0;
      const long long rv = std::strtoll(r.c_str(), &re, 10);
      const bool right_invalid = errno == ERANGE || re == r.c_str() || !re || *re;
      if (left_invalid || right_invalid) {
        std::cerr << "test: " << (left_invalid ? l : r)
                  << ": integer expression expected\n";
        return 2;
      }
      if (op == "-eq")
        return lv == rv ? 0 : 1;
      if (op == "-ne")
        return lv != rv ? 0 : 1;
      if (op == "-gt")
        return lv > rv ? 0 : 1;
      if (op == "-ge")
        return lv >= rv ? 0 : 1;
      if (op == "-lt")
        return lv < rv ? 0 : 1;
      if (op == "-le")
        return lv <= rv ? 0 : 1;
      return 2;
    };
    return eval(eval, 1, end);
  }
  return {};
}
