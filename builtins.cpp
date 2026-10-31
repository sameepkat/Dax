#include "builtins.hpp"
#include "arithmetic.hpp"
#include "helper.hpp"
#include "janet_runtime.hpp"
#include "janet_commands.hpp"
#include "shell_host.hpp"
#include "shell_state.hpp"
#include "shell.hpp"
#include "signal_handler.hpp"
#include "builtins_internal.hpp"
#include "builtins_internal.hpp"
#include <version.hpp>
#include <algorithm>
#include <cctype>
#include <climits>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <iomanip>
#include <glob.h>
#include <fnmatch.h>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <pwd.h>
#include <poll.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <termios.h>
#include <ctime>
#include <sys/wait.h>
#include <unistd.h>

extern "C" char **environ;

namespace {
ShellState &state = shell_state();
auto &aliases = state.aliases;
auto &vars = state.vars;
auto &readonly_vars = state.readonly_vars;
auto &command_hash = state.command_hash;
auto &functions = state.functions;
auto &traps = state.traps;
auto &dirs = state.dirs;
auto &positionals = state.positionals;
auto &shell_name = state.shell_name;
auto &jobs = state.jobs;
auto &status_value = state.status_value;
auto &exit_value = state.exit_value;
auto &exiting = state.exiting;
auto &errexit = state.errexit;
auto &nounset = state.nounset;
auto &allexport = state.allexport;
auto &notify_jobs = state.notify_jobs;
auto &noclobber = state.noclobber;
auto &noglob = state.noglob;
auto &monitor = state.monitor;
auto &noexec = state.noexec;
auto &verbose = state.verbose;
auto &xtrace = state.xtrace;
auto &login_shell = state.login_shell;
auto &interactive_shell = state.interactive_shell;
auto &errexit_ignored_context = state.errexit_ignored_context;
auto &errexit_immune_status = state.errexit_immune_status;
auto &returning = state.returning;
auto &loop_depth = state.loop_depth;
auto &loop_break = state.loop_break;
auto &loop_continue = state.loop_continue;
auto &loop_control_levels = state.loop_control_levels;
auto &substitution_status = state.substitution_status;
auto &substitution_ran = state.substitution_ran;
auto &pending_signal = state.pending_signal;
auto &expansion_error = state.expansion_error;
auto &expansion_error_index = state.expansion_error_index;
auto &getopts_index = state.getopts_index;
auto &getopts_char = state.getopts_char;
auto &local_scopes = state.local_scopes;
std::vector<std::pair<int, pid_t>> process_substitutions;
bool ps4_expansion_active = false;
bool expansion_error_reported = false;
std::string readline_prefill;
struct ReadlineShellBinding {
  Keymap keymap = nullptr;
  std::string key_sequence;
  std::string display_key_sequence;
  std::string command;
};
std::vector<ReadlineShellBinding> readline_shell_bindings;

struct ReadField {
  std::string value;
  size_t begin;
};

std::vector<ReadField> split_read_fields(const std::string &line,
                                        const std::string &ifs) {
  if (ifs.empty()) return {{line, 0}};

  std::vector<std::string> delimiters;
  std::mbstate_t ifs_state{};
  for (size_t offset = 0; offset < ifs.size();) {
    wchar_t character = 0;
    const size_t length = std::mbrtowc(&character, ifs.data() + offset,
                                       ifs.size() - offset, &ifs_state);
    const size_t consumed = length == 0 ? 1 :
        length == static_cast<size_t>(-1) || length == static_cast<size_t>(-2)
            ? 1 : length;
    delimiters.push_back(ifs.substr(offset, consumed));
    offset += consumed;
    if (length == static_cast<size_t>(-1) || length == static_cast<size_t>(-2))
      ifs_state = {};
  }
  auto delimiter_at = [&](size_t offset) -> const std::string * {
    for (const auto &delimiter : delimiters)
      if (line.compare(offset, delimiter.size(), delimiter) == 0) return &delimiter;
    return nullptr;
  };
  auto is_ifs_whitespace = [](const std::string &delimiter) {
    return delimiter == " " || delimiter == "\t" || delimiter == "\n";
  };
  auto advance_character = [&](size_t &offset, std::mbstate_t &state) {
    wchar_t character = 0;
    const size_t length = std::mbrtowc(&character, line.data() + offset,
                                       line.size() - offset, &state);
    if (length == 0) ++offset;
    else if (length == static_cast<size_t>(-1) ||
             length == static_cast<size_t>(-2)) {
      ++offset;
      state = {};
    } else offset += length;
  };
  auto consume_delimiter = [&](size_t &offset, std::mbstate_t &state,
                               const std::string &delimiter) {
    wchar_t character = 0;
    const size_t length = std::mbrtowc(&character, line.data() + offset,
                                       line.size() - offset, &state);
    if (length == static_cast<size_t>(-1) || length == static_cast<size_t>(-2)) {
      offset += delimiter.size();
      state = {};
    } else offset += length == 0 ? 1 : length;
  };

  std::vector<ReadField> fields;
  size_t cursor = 0;
  std::mbstate_t state{};
  while (cursor < line.size()) {
    const std::string *delimiter = delimiter_at(cursor);
    if (!delimiter || !is_ifs_whitespace(*delimiter)) break;
    consume_delimiter(cursor, state, *delimiter);
  }
  size_t begin = cursor;
  bool trailing_nonwhite_delimiter = false;
  while (cursor < line.size()) {
    const std::string *delimiter = delimiter_at(cursor);
    if (!delimiter) {
      trailing_nonwhite_delimiter = false;
      advance_character(cursor, state);
      continue;
    }
    fields.push_back({line.substr(begin, cursor - begin), begin});
    if (is_ifs_whitespace(*delimiter)) {
      while (cursor < line.size()) {
        delimiter = delimiter_at(cursor);
        if (!delimiter || !is_ifs_whitespace(*delimiter)) break;
        consume_delimiter(cursor, state, *delimiter);
      }
      if (cursor < line.size()) {
        delimiter = delimiter_at(cursor);
        if (delimiter && !is_ifs_whitespace(*delimiter)) {
          consume_delimiter(cursor, state, *delimiter);
          while (cursor < line.size()) {
            delimiter = delimiter_at(cursor);
            if (!delimiter || !is_ifs_whitespace(*delimiter)) break;
            consume_delimiter(cursor, state, *delimiter);
          }
          trailing_nonwhite_delimiter = cursor == line.size();
        } else trailing_nonwhite_delimiter = false;
      } else trailing_nonwhite_delimiter = false;
    } else {
      consume_delimiter(cursor, state, *delimiter);
      while (cursor < line.size()) {
        delimiter = delimiter_at(cursor);
        if (!delimiter || !is_ifs_whitespace(*delimiter)) break;
        consume_delimiter(cursor, state, *delimiter);
      }
      trailing_nonwhite_delimiter = cursor == line.size();
    }
    begin = cursor;
  }
  if (begin < line.size()) fields.push_back({line.substr(begin), begin});
  else if (trailing_nonwhite_delimiter) fields.push_back({"", begin});
  return fields;
}

std::optional<std::string> translate_readline_key_sequence(const std::string &sequence) {
  std::vector<char> translated(sequence.size() * 2 + 2, '\0');
  int length = 0;
  if (rl_translate_keyseq(sequence.c_str(), translated.data(), &length) != 0 || length < 0)
    return std::nullopt;
  return std::string(translated.data(), static_cast<size_t>(length));
}

int execute_readline_shell_binding(int, int) {
  if (!rl_executing_keyseq) return 0;
  const std::string key_sequence(rl_executing_keyseq);
  const auto binding = std::find_if(readline_shell_bindings.rbegin(),
                                    readline_shell_bindings.rend(),
      [&](const ReadlineShellBinding &candidate) {
        return candidate.keymap == rl_get_keymap() &&
               candidate.key_sequence == key_sequence;
      });
  if (binding == readline_shell_bindings.rend()) return 0;
  const std::string command = binding->command;

  state.vars["READLINE_LINE"] = rl_line_buffer ? rl_line_buffer : "";
  state.vars["READLINE_POINT"] = std::to_string(rl_point);
  state.vars["READLINE_MARK"] = std::to_string(rl_mark);
  rl_deprep_terminal();
  execute_line(command);
  rl_prep_terminal(1);

  const auto line = state.vars.find("READLINE_LINE");
  if (line != state.vars.end()) rl_replace_line(line->second.c_str(), 0);
  auto parse_position = [&](const char *name, int fallback) {
    const auto value = state.vars.find(name);
    if (value == state.vars.end()) return fallback;
    char *end = nullptr;
    errno = 0;
    const long parsed = std::strtol(value->second.c_str(), &end, 10);
    if (errno || end == value->second.c_str() || *end) return fallback;
    return static_cast<int>(std::clamp(parsed, 0L, static_cast<long>(rl_end)));
  };
  rl_point = parse_position("READLINE_POINT", rl_point);
  rl_mark = parse_position("READLINE_MARK", rl_mark);
  rl_on_new_line();
  rl_redisplay();
  return 0;
}

int insert_readline_prefill() {
  if (!readline_prefill.empty()) rl_insert_text(readline_prefill.c_str());
  return 0;
}

void refresh_stack_variables() {
  const bool has_function = std::find(state.function_frame_flags.begin(),
                                      state.function_frame_flags.end(), true) !=
                             state.function_frame_flags.end();
  auto &names = state.arrays["FUNCNAME"];
  names.clear();
  if (has_function) {
    for (size_t i = state.function_stack.size(); i > 0; --i) {
      const size_t frame = i - 1;
      const bool is_function = frame < state.function_frame_flags.size() &&
                               state.function_frame_flags[frame];
      names.push_back(is_function ? state.function_stack[frame] : "source");
    }
  }
  shell_array_mark_all_set("FUNCNAME");
  auto &sources = state.arrays["BASH_SOURCE"];
  sources.clear();
  if (!state.function_stack.empty()) {
    sources.assign(state.function_source_stack.rbegin(), state.function_source_stack.rend());
  } else {
    sources.assign(state.source_stack.rbegin(), state.source_stack.rend());
    if (sources.empty() && !state.script_source.empty())
      sources.push_back(state.script_source);
  }
  shell_array_mark_all_set("BASH_SOURCE");
  auto &lines = state.arrays["BASH_LINENO"];
  lines.clear();
  for (size_t i = state.function_call_lines.size(); i > 0; --i)
    lines.push_back(std::to_string(state.function_call_lines[i - 1]));
  shell_array_mark_all_set("BASH_LINENO");
}

std::string active_source_name() {
  if (!state.source_stack.empty()) return state.source_stack.back();
  return state.script_source.empty() ? state.shell_name : state.script_source;
}

bool name_ok(const std::string &s) {
  return !s.empty() && (std::isalpha((unsigned char)s[0]) || s[0] == '_') &&
         std::all_of(s.begin() + 1, s.end(), [](unsigned char c) {
           return std::isalnum(c) || c == '_';
         });
}
std::string join(const std::vector<std::string> &a, size_t i = 1) {
  std::string s;
  for (; i < a.size(); ++i) {
    if (!s.empty())
      s += ' ';
    s += a[i];
  }
  return s;
}
std::string locale_case_transform(const std::string &input, bool uppercase) {
  std::string output;
  std::mbstate_t input_state{};
  size_t offset = 0;
  while (offset < input.size()) {
    wchar_t character = 0;
    const size_t size = std::mbrtowc(&character, input.data() + offset,
                                     input.size() - offset, &input_state);
    if (size == static_cast<size_t>(-1) || size == static_cast<size_t>(-2)) {
      output += input[offset++];
      input_state = {};
      continue;
    }
    const size_t character_size = size == 0 ? 1 : size;
    const std::string original = input.substr(offset, character_size);
    const wchar_t mapped = uppercase ? std::towupper(character) : std::towlower(character);
    char encoded[MB_LEN_MAX];
    std::mbstate_t output_state{};
    const size_t encoded_size = std::wcrtomb(encoded, mapped, &output_state);
    if (encoded_size == static_cast<size_t>(-1)) output += original;
    else output.append(encoded, encoded_size);
    offset += character_size;
  }
  return output;
}
std::vector<size_t> locale_character_boundaries(const std::string &input) {
  std::vector<size_t> boundaries{0};
  std::mbstate_t conversion_state{};
  size_t offset = 0;
  while (offset < input.size()) {
    wchar_t character = 0;
    const size_t consumed = std::mbrtowc(&character, input.data() + offset,
                                         input.size() - offset,
                                         &conversion_state);
    if (consumed == static_cast<size_t>(-1) ||
        consumed == static_cast<size_t>(-2)) {
      ++offset;
      conversion_state = {};
    } else {
      offset += consumed == 0 ? 1 : consumed;
    }
    boundaries.push_back(offset);
  }
  return boundaries;
}
size_t locale_character_length(const std::string &input) {
  return locale_character_boundaries(input).size() - 1;
}
std::string locale_character_substring(const std::string &input,
                                       size_t start, size_t end) {
  const auto boundaries = locale_character_boundaries(input);
  const size_t characters = boundaries.size() - 1;
  start = std::min(start, characters);
  end = std::clamp(end, start, characters);
  return input.substr(boundaries[start], boundaries[end] - boundaries[start]);
}
const std::vector<std::pair<int, std::string>> &shell_signal_names() {
  static const std::vector<std::pair<int, std::string>> names = [] {
    std::vector<std::pair<int, std::string>> values;
#define DAX_SIGNAL_NAME(signal) values.emplace_back(signal, #signal)
#ifdef SIGHUP
    DAX_SIGNAL_NAME(SIGHUP);
#endif
#ifdef SIGINT
    DAX_SIGNAL_NAME(SIGINT);
#endif
#ifdef SIGQUIT
    DAX_SIGNAL_NAME(SIGQUIT);
#endif
#ifdef SIGILL
    DAX_SIGNAL_NAME(SIGILL);
#endif
#ifdef SIGTRAP
    DAX_SIGNAL_NAME(SIGTRAP);
#endif
#ifdef SIGABRT
    DAX_SIGNAL_NAME(SIGABRT);
#endif
#ifdef SIGBUS
    DAX_SIGNAL_NAME(SIGBUS);
#endif
#ifdef SIGFPE
    DAX_SIGNAL_NAME(SIGFPE);
#endif
#ifdef SIGKILL
    DAX_SIGNAL_NAME(SIGKILL);
#endif
#ifdef SIGUSR1
    DAX_SIGNAL_NAME(SIGUSR1);
#endif
#ifdef SIGSEGV
    DAX_SIGNAL_NAME(SIGSEGV);
#endif
#ifdef SIGUSR2
    DAX_SIGNAL_NAME(SIGUSR2);
#endif
#ifdef SIGPIPE
    DAX_SIGNAL_NAME(SIGPIPE);
#endif
#ifdef SIGALRM
    DAX_SIGNAL_NAME(SIGALRM);
#endif
#ifdef SIGTERM
    DAX_SIGNAL_NAME(SIGTERM);
#endif
#ifdef SIGSTKFLT
    DAX_SIGNAL_NAME(SIGSTKFLT);
#endif
#ifdef SIGCHLD
    DAX_SIGNAL_NAME(SIGCHLD);
#endif
#ifdef SIGCONT
    DAX_SIGNAL_NAME(SIGCONT);
#endif
#ifdef SIGSTOP
    DAX_SIGNAL_NAME(SIGSTOP);
#endif
#ifdef SIGTSTP
    DAX_SIGNAL_NAME(SIGTSTP);
#endif
#ifdef SIGTTIN
    DAX_SIGNAL_NAME(SIGTTIN);
#endif
#ifdef SIGTTOU
    DAX_SIGNAL_NAME(SIGTTOU);
#endif
#ifdef SIGURG
    DAX_SIGNAL_NAME(SIGURG);
#endif
#ifdef SIGXCPU
    DAX_SIGNAL_NAME(SIGXCPU);
#endif
#ifdef SIGXFSZ
    DAX_SIGNAL_NAME(SIGXFSZ);
#endif
#ifdef SIGVTALRM
    DAX_SIGNAL_NAME(SIGVTALRM);
#endif
#ifdef SIGPROF
    DAX_SIGNAL_NAME(SIGPROF);
#endif
#ifdef SIGWINCH
    DAX_SIGNAL_NAME(SIGWINCH);
#endif
#ifdef SIGIO
    DAX_SIGNAL_NAME(SIGIO);
#endif
#ifdef SIGPWR
    DAX_SIGNAL_NAME(SIGPWR);
#endif
#ifdef SIGSYS
    DAX_SIGNAL_NAME(SIGSYS);
#endif
#ifdef SIGEMT
    DAX_SIGNAL_NAME(SIGEMT);
#endif
#ifdef SIGINFO
    DAX_SIGNAL_NAME(SIGINFO);
#endif
#ifdef SIGRTMIN
    for (int number = SIGRTMIN; number <= SIGRTMAX; ++number) {
      std::string name = number == SIGRTMIN ? "SIGRTMIN"
                         : number == SIGRTMAX ? "SIGRTMAX"
                                              : "SIGRTMIN+" + std::to_string(number - SIGRTMIN);
      values.emplace_back(number, std::move(name));
    }
#endif
#undef DAX_SIGNAL_NAME
    std::sort(values.begin(), values.end(), [](const auto &left, const auto &right) {
      return left.first < right.first;
    });
    return values;
  }();
  return names;
}
int signal_number(std::string name) {
  if (!name.empty() && name[0] == '-')
    name.erase(name.begin());
  if (name.rfind("SIG", 0) == 0)
    name.erase(0, 3);
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  if (name.empty())
    return 0;
  if (std::isdigit(static_cast<unsigned char>(name[0]))) {
    char *end = nullptr;
    errno = 0;
    const long value = std::strtol(name.c_str(), &end, 10);
    if (errno || end == name.c_str() || *end || value < 0 || value > INT_MAX)
      return 0;
#ifdef NSIG
    if (value > 0 && value >= NSIG) return 0;
#endif
    return static_cast<int>(value);
  }
  for (const auto &[number, full_name] : shell_signal_names()) {
    if (name == full_name.substr(3)) return number;
  }
#ifdef SIGABRT
  if (name == "IOT") return SIGABRT;
#endif
#ifdef SIGCHLD
  if (name == "CLD") return SIGCHLD;
#endif
#ifdef SIGIO
  if (name == "POLL") return SIGIO;
#endif
#if defined(SIGRTMIN) && defined(SIGRTMAX)
  auto parse_realtime_offset = [](const std::string &text, const char *prefix,
                                  int base, int sign) -> int {
    const std::string marker(prefix);
    if (text.rfind(marker, 0) != 0 || text.size() == marker.size()) return 0;
    char *end = nullptr;
    errno = 0;
    const long offset = std::strtol(text.c_str() + marker.size(), &end, 10);
    if (errno || end == text.c_str() + marker.size() || *end || offset < 0 ||
        offset > SIGRTMAX - SIGRTMIN) return 0;
    const long result = static_cast<long>(base) + sign * offset;
    return result >= SIGRTMIN && result <= SIGRTMAX ? static_cast<int>(result) : 0;
  };
  if (name.rfind("RTMIN+", 0) == 0)
    return parse_realtime_offset(name, "RTMIN+", SIGRTMIN, 1);
  if (name.rfind("RTMAX-", 0) == 0)
    return parse_realtime_offset(name, "RTMAX-", SIGRTMAX, -1);
#endif
  return 0;
}
std::string quote_shell_word(const std::string &value) {
  std::string result = "'";
  for (const char ch : value)
    result += ch == '\'' ? "'\\''" : std::string(1, ch);
  return result + "'";
}
uint32_t next_bash_random() {
  const uint32_t value = state.random_seed == 0 ? 123459876U : state.random_seed;
  const int64_t high = value / 127773U;
  const int64_t low = value % 127773U;
  int64_t next = 16807 * low - 2836 * high;
  if (next < 0) next += 0x7fffffff;
  state.random_seed = static_cast<uint32_t>(next);
  return ((state.random_seed >> 16) ^ (state.random_seed & 65535U)) & 32767U;
}
uint32_t next_bash_secure_random() {
  uint32_t value = 0;
  const int fd = open("/dev/urandom", O_RDONLY);
  if (fd >= 0) {
    size_t offset = 0;
    while (offset < sizeof(value)) {
      const ssize_t amount = ::read(fd, reinterpret_cast<char *>(&value) + offset,
                                   sizeof(value) - offset);
      if (amount < 0 && errno == EINTR) continue;
      if (amount <= 0) break;
      offset += static_cast<size_t>(amount);
    }
    close(fd);
    if (offset == sizeof(value)) {
      state.last_secure_random = value;
      return value;
    }
  }
  do {
    const uint32_t seed = state.secure_random_seed == 0
                              ? 123459876U : state.secure_random_seed;
    const int64_t high = seed / 127773U;
    const int64_t low = seed % 127773U;
    int64_t next = 16807 * low - 2836 * high;
    if (next < 0) next += 0x7fffffff;
    state.secure_random_seed = static_cast<uint32_t>(next);
    value = state.secure_random_seed & 0x7fffffffU;
  } while (value == state.last_secure_random);
  state.last_secure_random = value;
  return value;
}
bool setvar(const std::string &x, bool export_it = false) {
  auto p = x.find('=');
  if (p == std::string::npos || !name_ok(x.substr(0, p)))
    return false;
  std::string k = x.substr(0, p), v = x.substr(p + 1);
  if (readonly_vars.count(k)) {
    std::cerr << k << ": readonly variable\n";
    return false;
  }
  return set_shell_variable(k, v, export_it || allexport);
}
bool command_path_is_ignored(const std::string &pathname) {
  if (state.unset_vars.count("EXECIGNORE")) return false;
  const auto variable = state.vars.find("EXECIGNORE");
  const char *environment_value = std::getenv("EXECIGNORE");
  const std::string patterns = variable != state.vars.end()
      ? variable->second : environment_value ? environment_value : std::string();
  size_t begin = 0;
  const int flags = state.extglob ? FNM_EXTMATCH : 0;
  while (begin <= patterns.size()) {
    const size_t end = patterns.find(':', begin);
    const std::string pattern = patterns.substr(
        begin, end == std::string::npos ? std::string::npos : end - begin);
    if (!pattern.empty() && fnmatch(pattern.c_str(), pathname.c_str(), flags) == 0)
      return true;
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return false;
}

std::optional<std::string> find_command(const std::string &n,
                                        bool command_execution = false,
                                        bool use_hash_cache = true) {
  if (n.find('/') != std::string::npos)
    return access(n.c_str(), X_OK) == 0 ? std::optional<std::string>(n)
                                        : std::nullopt;
  if (use_hash_cache) {
    auto cached = command_hash.find(n);
    if (cached != command_hash.end()) {
      if (!state.checkhash || access(cached->second.c_str(), X_OK) == 0) {
        if (command_execution) ++state.command_hash_hits[n];
        return cached->second;
      }
      command_hash.erase(cached);
      state.command_hash_hits.erase(n);
    }
  }
  const auto path_value = shell_path_value();
  std::string default_search_path;
  const std::string *search_path_pointer = nullptr;
  if (path_value) {
    search_path_pointer = &*path_value;
  } else {
    const size_t required = confstr(_CS_PATH, nullptr, 0);
    if (required > 1) {
      std::vector<char> buffer(required);
      const size_t written = confstr(_CS_PATH, buffer.data(), buffer.size());
      if (written > 1 && written <= buffer.size())
        default_search_path.assign(buffer.data(), written - 1);
    }
    if (default_search_path.empty()) default_search_path = "/bin:/usr/bin";
    search_path_pointer = &default_search_path;
  }
  const std::string &search_path = *search_path_pointer;
  size_t component_start = 0;
  while (component_start <= search_path.size()) {
    const size_t separator = search_path.find(':', component_start);
    const std::string d = search_path.substr(
        component_start, separator == std::string::npos
                             ? std::string::npos
                             : separator - component_start);
    auto c = (std::filesystem::path(d.empty() ? "." : d) / n).string();
    struct stat candidate_info {};
    if (access(c.c_str(), X_OK) == 0 && stat(c.c_str(), &candidate_info) == 0 &&
        !S_ISDIR(candidate_info.st_mode) && !command_path_is_ignored(c)) {
      if (command_execution && state.hashall) {
        const auto previous = command_hash.find(n);
        if (previous == command_hash.end() || previous->second != c)
          state.command_hash_hits[n] = 0;
        command_hash[n] = c;
        if (command_execution) ++state.command_hash_hits[n];
      }
      return c;
    }
    if (separator == std::string::npos) break;
    component_start = separator + 1;
  }
  return {};
}
int external(const std::vector<std::string> &a, bool foreground = true,
             bool use_hash_cache = true) {
  if (a.empty())
    return 0;
  auto p = find_command(a[0], true, use_hash_cache);
  if (!p) {
    bool path_entry_exists = false;
    const bool searched_path = a[0].find('/') == std::string::npos;
    auto check_path_entry = [&](const std::string &candidate) {
      std::error_code error;
      if (std::filesystem::exists(candidate, error) &&
          (!searched_path || !command_path_is_ignored(candidate)))
        path_entry_exists = true;
    };
    if (a[0].find('/') != std::string::npos) {
      check_path_entry(a[0]);
    } else if (const auto path = shell_path_value()) {
      size_t begin = 0;
      while (begin <= path->size() && !path_entry_exists) {
        const size_t end = path->find(':', begin);
        const std::string directory = path->substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        check_path_entry((std::filesystem::path(directory.empty() ? "." : directory) /
                          a[0]).string());
        if (end == std::string::npos) break;
        begin = end + 1;
      }
    }
    if (!path_entry_exists && state.functions.count("command_not_found_handle")) {
      std::vector<std::string> handler = {"command_not_found_handle"};
      handler.insert(handler.end(), a.begin(), a.end());
      if (auto status = handle_builtin(handler)) return *status;
    }
    const int launch_error = path_entry_exists ? EACCES : ENOENT;
    report_command_launch_error(a[0], launch_error);
    return launch_error == ENOENT ? 127 : 126;
  }
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return 126;
  }
  if (!pid) {
    signal(SIGINT, SIG_DFL);
    setenv("_", p->c_str(), 1);
    std::vector<char *> av;
    for (auto &s : a)
      av.push_back(const_cast<char *>(s.c_str()));
    av.push_back(nullptr);
    execv(p->c_str(), av.data());
    const int exec_error = errno;
    report_command_launch_error(a[0], exec_error);
    _exit(exec_error == ENOENT ? 127 : 126);
  }
  if (!foreground) {
    remember_job(pid, join(a, 0));
    return 0;
  }
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
  }
  return WIFEXITED(st)     ? WEXITSTATUS(st)
         : WIFSIGNALED(st) ? 128 + WTERMSIG(st)
                           : 1;
}
int execute_function_call(const std::vector<std::string> &a) {
  const std::string &name = a[0];
  const auto nesting_limit = state.vars.find("FUNCNEST");
  const char *environment_limit = std::getenv("FUNCNEST");
  const std::string limit_text = nesting_limit != state.vars.end()
      ? nesting_limit->second : environment_limit ? environment_limit : std::string();
  if (!limit_text.empty()) {
    char *end = nullptr;
    errno = 0;
    const long limit = std::strtol(limit_text.c_str(), &end, 10);
    const size_t function_depth = static_cast<size_t>(std::count(
        state.function_frame_flags.begin(), state.function_frame_flags.end(), true));
    if (!errno && end != limit_text.c_str() && end && *end == '\0' && limit > 0 &&
        function_depth >= static_cast<unsigned long>(limit)) {
      std::cerr << name << ": maximum function nesting level exceeded (" << limit << ")\n";
      return 1;
    }
  }
  auto saved = positionals;
  const size_t saved_lineno = state.current_lineno;
  const std::string call_source = active_source_name();
  positionals.assign(a.begin() + 1, a.end());
  local_scopes.emplace_back();
  state.local_option_scopes.emplace_back(std::nullopt);
  state.function_stack.push_back(name);
  state.function_frame_flags.push_back(true);
  auto source = state.function_sources.find(name);
  state.function_source_stack.push_back(source == state.function_sources.end() ? active_source_name() : source->second);
  state.function_call_lines.push_back(saved_lineno);
  state.function_call_sources.push_back(call_source);
  refresh_stack_variables();
  returning = false;
  struct FunctionLoopContext {
    ShellState &state;
    int depth;
    bool breaking;
    bool continuing;
    unsigned levels;
    FunctionLoopContext()
        : state(shell_state()), depth(state.loop_depth), breaking(state.loop_break),
          continuing(state.loop_continue), levels(state.loop_control_levels) {
      state.loop_depth = 0;
      state.loop_break = false;
      state.loop_continue = false;
      state.loop_control_levels = 1;
    }
    ~FunctionLoopContext() {
      state.loop_depth = depth;
      state.loop_break = breaking;
      state.loop_continue = continuing;
      state.loop_control_levels = levels;
    }
  };
  {
    FunctionLoopContext function_loop_context;
    execute_line(functions[name]);
  }
  const int result = status_value;
  for (auto &[key, value] : local_scopes.back()) {
    if (value.had_var) vars[key] = value.var;
    else vars.erase(key);
    if (value.had_unset_state) state.unset_vars.insert(key);
    else state.unset_vars.erase(key);
    if (key == "IGNOREEOF" && value.had_ignoreeof_state) {
      state.ignoreeof = value.ignoreeof_state;
      refresh_option_parameter_values();
    }
    if (value.had_env) setenv(key.c_str(), value.env.c_str(), 1);
    else unsetenv(key.c_str());
    if (value.had_indexed_array) {
      state.arrays[key] = std::move(value.indexed_array);
      if (value.had_indexed_array_indices)
        state.array_set_indices[key] = std::move(value.indexed_array_indices);
      else shell_array_mark_all_set(key);
    } else {
      state.arrays.erase(key);
      state.array_set_indices.erase(key);
    }
    if (value.had_associative_array) state.associative_arrays[key] = std::move(value.associative_array);
    else state.associative_arrays.erase(key);
    if (value.had_integer_attribute) state.integer_vars.insert(key);
    else state.integer_vars.erase(key);
    if (value.had_lowercase_attribute) state.lowercase_vars.insert(key);
    else state.lowercase_vars.erase(key);
    if (value.had_uppercase_attribute) state.uppercase_vars.insert(key);
    else state.uppercase_vars.erase(key);
    if (value.had_readonly_attribute) state.readonly_vars[key] = "1";
    else state.readonly_vars.erase(key);
    if (value.had_nameref) state.namerefs[key] = value.nameref_target;
    else state.namerefs.erase(key);
  }
  local_scopes.pop_back();
  if (state.local_option_scopes.back())
    restore_shell_options(state, *state.local_option_scopes.back());
  state.local_option_scopes.pop_back();
  run_return_trap();
  positionals = std::move(saved);
  returning = false;
  state.function_stack.pop_back();
  state.function_frame_flags.pop_back();
  state.function_source_stack.pop_back();
  state.function_call_lines.pop_back();
  state.function_call_sources.pop_back();
  state.current_lineno = saved_lineno;
  refresh_stack_variables();
  return result;
}
std::optional<int> run(const std::vector<std::string> &a) {
  if (a.empty())
    return 0;
  const auto &c = a[0];
  if (state.disabled_builtins.count(c)) {
    if (functions.count(c)) return execute_function_call(a);
    return {};
  }
  if (auto result = run_navigation_builtin(a, state)) return result;
  if (auto result = run_history_builtin(a)) return result;
  if (auto result = run_options_builtin(a, state)) return result;
  if (auto result = run_output_builtin(a)) return result;
  if (auto result = run_system_builtin(a)) return result;
  if (auto result = run_environment_builtin(a, state)) return result;
  if (auto result = run_jobs_builtin(a, state)) return result;
  if (auto result = run_lookup_builtin(a, state)) return result;
  if (c == ":")
    return 0;
  if (c == "true")
    return 0;
  if (c == "false")
    return 1;
  if (c == "bind") {
    bool list_functions = false;
    bool dump_functions = false, dump_macros = false, dump_variables = false;
    bool dump_readable = true;
    bool all_keymaps = false;
    bool dump_shell_bindings = false;
    std::vector<std::string> directives;
    std::vector<std::pair<std::string, std::string>> shell_binding_specs;
    auto named_keymaps = [] {
      std::vector<Keymap> keymaps;
      for (const char *name : {"emacs", "emacs-standard", "emacs-meta", "emacs-ctlx",
                               "vi", "vi-insert", "vi-move", "vi-command"}) {
        if (Keymap keymap = rl_get_keymap_by_name(name)) {
          if (std::find(keymaps.begin(), keymaps.end(), keymap) == keymaps.end())
            keymaps.push_back(keymap);
        }
      }
      return keymaps;
    };
    auto parse_shell_binding = [&](const std::string &specification) {
      char quote = 0;
      bool escaped = false;
      size_t separator = std::string::npos;
      for (size_t j = 0; j < specification.size(); ++j) {
        const char ch = specification[j];
        if (escaped) { escaped = false; continue; }
        if (ch == '\\') { escaped = true; continue; }
        if (quote) {
          if (ch == quote) quote = 0;
        } else if (ch == '\'' || ch == '"') quote = ch;
        else if (ch == ':') { separator = j; break; }
      }
      if (separator == std::string::npos) return false;
      std::string key_sequence = specification.substr(0, separator);
      const size_t first = key_sequence.find_first_not_of(" \t");
      const size_t last = key_sequence.find_last_not_of(" \t");
      if (first == std::string::npos) return false;
      key_sequence = key_sequence.substr(first, last - first + 1);
      if (key_sequence.size() >= 2 &&
          ((key_sequence.front() == '"' && key_sequence.back() == '"') ||
           (key_sequence.front() == '\'' && key_sequence.back() == '\'')))
        key_sequence = key_sequence.substr(1, key_sequence.size() - 2);
      shell_binding_specs.emplace_back(
          std::move(key_sequence), specification.substr(separator + 1));
      return true;
    };
    auto option_value = [&](size_t &index, const char *option) -> const char * {
      if (index + 1 >= a.size()) {
        std::cerr << "bind: " << option << ": option requires an argument\n";
        return nullptr;
      }
      return a[++index].c_str();
    };
    for (size_t i = 1; i < a.size(); ++i) {
      if (a[i] == "--") {
        directives.insert(directives.end(), a.begin() + i + 1, a.end());
        break;
      }
      if (a[i] == "-f") {
        const char *path = option_value(i, "-f");
        if (!path) return 2;
        if (rl_read_init_file(path) != 0) {
          std::cerr << "bind: " << path << ": could not read Readline init file\n";
          return 1;
        }
      } else if (a[i] == "-m") {
        const char *name = option_value(i, "-m");
        if (!name) return 2;
        Keymap keymap = rl_get_keymap_by_name(name);
        if (!keymap) {
          std::cerr << "bind: " << name << ": unknown keymap name\n";
          return 1;
        }
        rl_set_keymap(keymap);
      } else if (a[i] == "-u") {
        const char *name = option_value(i, "-u");
        if (!name) return 2;
        if (!rl_named_function(name)) {
          std::cerr << "bind: " << name << ": could not unbind Readline function\n";
          return 1;
        }
        const auto targets = all_keymaps ? named_keymaps() : std::vector<Keymap>{rl_get_keymap()};
        for (Keymap keymap : targets) rl_unbind_command_in_map(name, keymap);
        if (std::string(name) == "dax-shell-command") {
          readline_shell_bindings.erase(std::remove_if(
              readline_shell_bindings.begin(), readline_shell_bindings.end(),
              [&](const ReadlineShellBinding &binding) {
                return std::find(targets.begin(), targets.end(), binding.keymap) != targets.end();
              }), readline_shell_bindings.end());
        }
      } else if (a[i] == "-r") {
        const char *key_sequence = option_value(i, "-r");
        if (!key_sequence) return 2;
        const auto targets = all_keymaps ? named_keymaps() : std::vector<Keymap>{rl_get_keymap()};
        for (Keymap keymap : targets) {
          if (rl_generic_bind(ISFUNC, key_sequence, nullptr, keymap) != 0) {
            std::cerr << "bind: " << key_sequence << ": invalid key sequence\n";
            return 1;
          }
        }
        const auto translated = translate_readline_key_sequence(key_sequence);
        if (translated) {
          readline_shell_bindings.erase(std::remove_if(
              readline_shell_bindings.begin(), readline_shell_bindings.end(),
              [&](const ReadlineShellBinding &binding) {
                return std::find(targets.begin(), targets.end(), binding.keymap) != targets.end() &&
                       binding.key_sequence == *translated;
          }), readline_shell_bindings.end());
        }
      } else if (a[i] == "-q") {
        const char *name = option_value(i, "-q");
        if (!name) return 2;
        rl_command_func_t *function = rl_named_function(name);
        if (!function) {
          std::cerr << "bind: " << name << ": unknown Readline function name\n";
          return 1;
        }
        char **key_sequences = rl_invoking_keyseqs_in_map(function, rl_get_keymap());
        bool found = key_sequences && key_sequences[0];
        if (found) {
          std::cout << name << " can be invoked via ";
          for (size_t key_index = 0; key_sequences[key_index]; ++key_index) {
            if (key_index) std::cout << ", ";
            std::cout << '"' << key_sequences[key_index] << '"';
            std::free(key_sequences[key_index]);
          }
          std::cout << ".\n";
        }
        std::free(key_sequences);
        if (!found) return 1;
      } else if (a[i] == "-x") {
        const char *specification = option_value(i, "-x");
        if (!specification) return 2;
        if (!parse_shell_binding(specification)) {
          std::cerr << "bind: -x: expected keyseq:shell-command\n";
          return 2;
        }
      } else if (a[i] == "-X") dump_shell_bindings = true;
      else if (a[i] == "-a") all_keymaps = true;
      else if (a[i] == "-l") list_functions = true;
      else if (a[i] == "-p" || a[i] == "-P") {
        dump_functions = true;
        dump_readable = a[i] == "-p";
      } else if (a[i] == "-s" || a[i] == "-S") {
        dump_macros = true;
        dump_readable = a[i] == "-s";
      } else if (a[i] == "-v" || a[i] == "-V") {
        dump_variables = true;
        dump_readable = a[i] == "-v";
      } else if (a[i].size() > 1 && a[i][0] == '-') {
        std::cerr << "bind: invalid option -- '" << a[i][1] << "'\n";
        return 2;
      } else directives.push_back(a[i]);
    }
    if (list_functions) rl_list_funmap_names();
    if (dump_functions) rl_function_dumper(dump_readable ? 1 : 0);
    if (dump_macros) rl_macro_dumper(dump_readable ? 1 : 0);
    if (dump_variables) rl_variable_dumper(dump_readable ? 1 : 0);
    if (dump_shell_bindings) {
      for (const auto &binding : readline_shell_bindings) {
        if (binding.keymap == rl_get_keymap())
          std::cout << binding.display_key_sequence << ": " << binding.command << '\n';
      }
    }
    if (!shell_binding_specs.empty()) {
      static const bool registered = rl_add_defun(
          "dax-shell-command", execute_readline_shell_binding, -1) == 0;
      if (!registered) {
        std::cerr << "bind: could not register shell-command Readline function\n";
        return 1;
      }
      const auto targets = all_keymaps ? named_keymaps() : std::vector<Keymap>{rl_get_keymap()};
      for (Keymap keymap : targets) {
        for (const auto &[key_sequence, command] : shell_binding_specs) {
          const auto translated = translate_readline_key_sequence(key_sequence);
          if (!translated || rl_generic_bind(
                  ISFUNC, key_sequence.c_str(),
                  reinterpret_cast<char *>(execute_readline_shell_binding), keymap) != 0) {
            std::cerr << "bind: " << key_sequence << ": invalid key sequence\n";
            return 1;
          }
          readline_shell_bindings.erase(std::remove_if(
              readline_shell_bindings.begin(), readline_shell_bindings.end(),
              [&](const ReadlineShellBinding &binding) {
            return binding.keymap == keymap && binding.key_sequence == *translated;
          }), readline_shell_bindings.end());
          readline_shell_bindings.push_back(
              {keymap, *translated, key_sequence, command});
        }
      }
    }
    if (all_keymaps && !directives.empty()) {
      Keymap selected = rl_get_keymap();
      const auto keymaps = named_keymaps();
      for (Keymap keymap : keymaps) {
        rl_set_keymap(keymap);
        for (auto directive : directives) {
          if (rl_parse_and_bind(directive.data()) != 0) {
            rl_set_keymap(selected);
            return 1;
          }
        }
      }
      rl_set_keymap(selected);
    } else {
      for (auto directive : directives) {
        if (rl_parse_and_bind(directive.data()) != 0) return 1;
      }
    }
    return 0;
  }
  if (c == "help") {
    bool descriptions_only = false, synopsis_only = false;
    std::vector<std::string> patterns;
    for (size_t i = 1; i < a.size(); ++i) {
      if (a[i] == "--") {
        patterns.insert(patterns.end(), a.begin() + i + 1, a.end());
        break;
      }
      if (a[i] == "-d") descriptions_only = true;
      else if (a[i] == "-s") synopsis_only = true;
      else if (a[i] == "-m") { /* use the same readable topic format */ }
      else if (a[i].size() > 1 && a[i][0] == '-') {
        std::cerr << "help: invalid option -- '" << a[i][1] << "'\n";
        return 2;
      } else patterns.push_back(a[i]);
    }
    if (descriptions_only && synopsis_only) {
      std::cerr << "help: cannot use -d and -s together\n";
      return 2;
    }
    static const std::unordered_map<std::string, std::string> descriptions = {
        {"alias", "Define or display command aliases."},
        {"bg", "Resume jobs in the background."},
        {"bind", "Display or change Readline key bindings."},
        {"break", "Exit from a loop."},
        {"builtin", "Run a shell builtin."},
        {"cd", "Change the current directory."},
        {"command", "Run a command, bypassing functions and aliases."},
        {"continue", "Resume the next iteration of a loop."},
        {"declare", "Declare variables and give them attributes."},
        {"echo", "Write arguments to standard output."},
        {"eval", "Read and execute arguments as shell input."},
        {"exec", "Replace the shell with a command, or change redirections."},
        {"exit", "Exit the shell."},
        {"export", "Mark variables for export to child processes."},
        {"false", "Return an unsuccessful status."},
        {"fc", "Display or edit commands from the history list."},
        {"fg", "Resume a job in the foreground."},
        {"getopts", "Parse positional parameters as options."},
        {"hash", "Remember or display command locations."},
        {"jobs", "Display active jobs."},
        {"kill", "Send a signal to a process or job."},
        {"let", "Evaluate arithmetic expressions."},
        {"local", "Declare variables local to a function."},
        {"mapfile", "Read lines from standard input into an indexed array."},
        {"popd", "Remove a directory from the directory stack."},
        {"printf", "Format and print arguments."},
        {"pushd", "Change directory and update the directory stack."},
        {"pwd", "Print the current working directory."},
        {"read", "Read a line from standard input into variables."},
        {"readonly", "Mark variables as readonly."},
        {"return", "Return from a shell function or sourced file."},
        {"set", "Set shell options and positional parameters."},
        {"shift", "Shift positional parameters."},
        {"shopt", "Set or display shell options."},
        {"source", "Execute a file in the current shell."},
        {"test", "Evaluate a conditional expression."},
        {"times", "Print the accumulated process times."},
        {"trap", "Set or display signal and shell-event handlers."},
        {"true", "Return a successful status."},
        {"type", "Describe how a command name is interpreted."},
        {"typeset", "Declare variables and give them attributes."},
        {"ulimit", "Display or change resource limits."},
        {"umask", "Display or set the file creation mask."},
        {"unalias", "Remove aliases."},
        {"unset", "Remove variables or functions."},
        {"wait", "Wait for jobs or processes to complete."},
    };
    const auto &builtins = shell_builtin_names();
    if (patterns.empty()) {
      std::vector<std::string> names = builtins;
      std::sort(names.begin(), names.end());
      for (size_t i = 0; i < names.size(); ++i)
        std::cout << names[i] << ((i + 1) % 5 == 0 || i + 1 == names.size() ? '\n' : '\t');
      return 0;
    }
    bool matched_any = false;
    for (const auto &pattern : patterns) {
      bool matched_pattern = false;
      for (const auto &name : builtins) {
        if (fnmatch(pattern.c_str(), name.c_str(), 0) != 0) continue;
        matched_any = matched_pattern = true;
        const auto description = descriptions.find(name);
        const std::string summary = description == descriptions.end()
                                        ? "Shell builtin."
                                        : description->second;
        if (descriptions_only) std::cout << name << " - " << summary << '\n';
        else if (synopsis_only) std::cout << name << ": " << name << " [arguments]\n";
        else std::cout << name << ": " << name << " [arguments]\n    " << summary << '\n';
      }
      if (!matched_pattern) {
        std::cerr << "help: no help topics match '" << pattern << "'\n";
      }
    }
    return matched_any ? 0 : 1;
  }
  if (c == "let") {
    int result = 0;
    for (size_t i = 1; i < a.size(); ++i) {
      std::string error;
      auto value = evaluate_arithmetic(a[i], &error);
      if (!value) {
        std::cerr << "let: " << a[i] << ": "
                  << (error.empty() ? "syntax error in expression" : error) << '\n';
        return 1;
      }
      result = *value == 0 ? 1 : 0;
    }
    return result;
  }
  if (c == "caller") {
    if (a.size() > 2) {
      std::cerr << "caller: too many arguments\n";
      return 1;
    }
    size_t level = 0;
    if (a.size() == 2) {
      char *end = nullptr;
      errno = 0;
      const unsigned long long parsed = std::strtoull(a[1].c_str(), &end, 10);
      if (errno || end == a[1].c_str() || *end || parsed > SIZE_MAX) {
        std::cerr << "caller: " << a[1] << ": numeric argument required\n";
        return 1;
      }
      level = static_cast<size_t>(parsed);
    }
    const size_t depth = state.function_stack.size();
    if (level >= depth || state.function_call_lines.size() != depth ||
        state.function_call_sources.size() != depth) return 1;
    const size_t frame = depth - 1 - level;
    std::cout << state.function_call_lines[frame] << ' ';
    if (a.size() == 2)
      std::cout << state.function_stack[frame] << ' ';
    std::cout << state.function_call_sources[frame] << '\n';
    return 0;
  }
  if (c == "logout" && !login_shell) {
    std::cerr << "logout: not login shell\n";
    return 1;
  }
  if (c == "exit" || c == "logout") {
    const std::string builtin_name = c;
    if (a.size() > 2) {
      std::cerr << builtin_name << ": too many arguments\n";
      return 1;
    }
    if (a.size() > 1) {
      char *end = nullptr;
      errno = 0;
      long parsed = std::strtol(a[1].c_str(), &end, 10);
      if (errno || end == a[1].c_str() || *end) {
        std::cerr << builtin_name << ": numeric argument required\n";
        exit_value = 2;
        exiting = true;
        return exit_value;
      }
      exit_value = static_cast<int>(parsed & 255);
    } else {
      exit_value = status_value;
    }
    if (state.interactive_shell) {
      for (auto &job : jobs) {
        if (job.completed) continue;
        int status = 0;
        int wait_options = WNOHANG | WUNTRACED;
#ifdef WCONTINUED
        wait_options |= WCONTINUED;
#endif
        const pid_t waited = waitpid(job.pid, &status, wait_options);
        if (waited != job.pid) continue;
        job.status = status;
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
          job.completed = true;
          job.stopped = false;
        } else if (WIFSTOPPED(status)) {
          job.stopped = true;
#ifdef WIFCONTINUED
        } else if (WIFCONTINUED(status)) {
          job.stopped = false;
#endif
        }
      }
      const bool has_stopped_job = std::any_of(
          jobs.begin(), jobs.end(), [](const Job &job) {
            return !job.completed && job.stopped;
          });
      const bool has_running_job = state.checkjobs && std::any_of(
          jobs.begin(), jobs.end(), [](const Job &job) {
            return !job.completed && !job.stopped;
          });
      if (has_stopped_job || has_running_job) {
        if (!state.exit_warning_pending) {
          if (state.checkjobs) {
            (void)handle_builtin({"jobs", "-r"});
            (void)handle_builtin({"jobs", "-s"});
          }
          state.exit_warning_pending = true;
          std::cerr << (has_stopped_job ? "There are stopped jobs.\n"
                                       : "There are running jobs.\n");
          return 1;
        }
        for (const auto &job : jobs) {
          if (job.completed || !job.stopped) continue;
          if (kill(-job.pid, SIGHUP) != 0 && errno == ESRCH)
            kill(job.pid, SIGHUP);
          if (kill(-job.pid, SIGCONT) != 0 && errno == ESRCH)
            kill(job.pid, SIGCONT);
        }
      }
    }
    state.exit_warning_pending = false;
    exiting = true;
    History::save_to_file();
    return exit_value;
  }
  if (c == "read") {
    int input_fd = STDIN_FILENO;
    bool raw = false;
    bool exact_count = false;
    bool array_mode = false;
    bool silent = false;
    bool character_limit_set = false;
    bool editline = false;
    std::string delimiter = "\n";
    size_t character_limit = 0;
    int64_t timeout_microseconds = -1;
    std::string prompt;
    std::string initial_text;
    std::string array_name;
    size_t first_name = 1;
    bool options = true;
    for (; first_name < a.size() && options; ++first_name) {
      const std::string &arg = a[first_name];
      if (arg == "--") { ++first_name; break; }
      if (arg.size() > 1 && arg.front() == '-') {
        for (size_t option_index = 1; option_index < arg.size(); ++option_index) {
          const char option = arg[option_index];
          if (option == 'r') { raw = true; continue; }
          if (option == 's') { silent = true; continue; }
          if (option == 'e' || option == 'E') { editline = true; continue; }
          if (std::string("nNdtuapi").find(option) == std::string::npos) {
            std::cerr << "read: " << arg << ": invalid option\n";
            return 2;
          }
          std::string value;
          if (option_index + 1 < arg.size()) {
            value = arg.substr(option_index + 1);
            option_index = arg.size();
          } else if (first_name + 1 < a.size()) {
            value = a[++first_name];
          } else {
            std::cerr << "read: option requires an argument -- " << option << '\n';
            return 2;
          }
          if (option == 'n' || option == 'N') {
            char *end = nullptr;
            errno = 0;
            const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
            const size_t first_nonspace = value.find_first_not_of(" \t\r\n");
            if (errno || end == value.c_str() || *end ||
                first_nonspace == std::string::npos || value[first_nonspace] == '-' ||
                parsed > std::numeric_limits<size_t>::max()) {
              std::cerr << "read: invalid character count: " << value << '\n';
              return 2;
            }
            character_limit = static_cast<size_t>(parsed);
            character_limit_set = true;
            exact_count = option == 'N';
          } else if (option == 'd') {
            if (value.empty()) {
              delimiter.assign(1, '\0');
            } else {
              std::mbstate_t delimiter_state{};
              wchar_t delimiter_character = 0;
              const size_t delimiter_length = std::mbrtowc(
                  &delimiter_character, value.data(), value.size(), &delimiter_state);
              const size_t first_character_length =
                  delimiter_length == 0 || delimiter_length == static_cast<size_t>(-1) ||
                          delimiter_length == static_cast<size_t>(-2)
                      ? 1 : delimiter_length;
              delimiter = value.substr(0, first_character_length);
            }
          } else if (option == 't') {
            char *end = nullptr;
            errno = 0;
            const double seconds = std::strtod(value.c_str(), &end);
            if (errno || end == value.c_str() || *end || !std::isfinite(seconds) ||
                seconds < 0 || seconds > static_cast<double>(INT_MAX) / 1000.0) {
              std::cerr << "read: invalid timeout: " << value << '\n';
              return 2;
            }
            timeout_microseconds = static_cast<int64_t>(std::ceil(seconds * 1000000.0));
          } else if (option == 'u') {
            char *end = nullptr;
            errno = 0;
            const long parsed = std::strtol(value.c_str(), &end, 10);
            if (errno || end == value.c_str() || *end || parsed < 0 || parsed > INT_MAX ||
                fcntl(static_cast<int>(parsed), F_GETFD) < 0) {
              std::cerr << "read: " << value << ": invalid file descriptor\n";
              return 2;
            }
            input_fd = static_cast<int>(parsed);
          } else if (option == 'a') {
            if (!shell_valid_name(value)) {
              std::cerr << "read: " << value << ": not a valid identifier\n";
              return 2;
            }
            array_mode = true;
            array_name = value;
          } else if (option == 'i') {
            editline = true;
            initial_text = value;
          } else {
            prompt = value;
          }
          break;
        }
        continue;
      }
      options = false;
      --first_name;
    }
    if (timeout_microseconds < 0) {
      const auto timeout_variable = state.vars.find("TMOUT");
      const char *environment_timeout = std::getenv("TMOUT");
      const std::string timeout_value = timeout_variable != state.vars.end()
          ? timeout_variable->second : environment_timeout ? environment_timeout : "";
      if (!timeout_value.empty()) {
        char *end = nullptr;
        errno = 0;
        const double seconds = std::strtod(timeout_value.c_str(), &end);
        if (!errno && end != timeout_value.c_str() && end && *end == '\0' &&
            std::isfinite(seconds) && seconds > 0 &&
            seconds <= static_cast<double>(INT_MAX) / 1000.0)
            timeout_microseconds = static_cast<int64_t>(std::ceil(seconds * 1000000.0));
      }
    }
    const bool use_readline = editline && input_fd == STDIN_FILENO && isatty(STDIN_FILENO) &&
                              !character_limit_set && delimiter == "\n" && !silent && timeout_microseconds < 0;
    if (!prompt.empty() && isatty(input_fd) && !use_readline) std::cerr << prompt << std::flush;
    if (timeout_microseconds == 0) {
      pollfd descriptor{input_fd, POLLIN, 0};
      int ready;
      for (;;) {
        ready = poll(&descriptor, 1, 0);
        if (ready < 0 && errno == EINTR) {
          if (consume_input_sigint() || pending_signal == SIGINT) return 130;
          continue;
        }
        break;
      }
      if (ready <= 0) return 1;
      return (descriptor.revents & (POLLIN | POLLHUP)) ? 0 : 1;
    }
    termios original_terminal_state{};
    bool terminal_state_changed = false;
    bool terminal_echo_disabled = false;
    if ((silent || character_limit_set) && isatty(input_fd)) {
      if (tcgetattr(input_fd, &original_terminal_state) == 0) {
        termios new_state = original_terminal_state;
        if (silent) new_state.c_lflag &= ~ECHO;
        if (character_limit_set) {
          new_state.c_lflag &= ~ICANON;
          new_state.c_cc[VMIN] = 1;
          new_state.c_cc[VTIME] = 0;
        }
        terminal_state_changed = tcsetattr(input_fd, TCSANOW, &new_state) == 0;
        terminal_echo_disabled = terminal_state_changed && silent;
      }
    }
    std::string line;
    bool escaped = false;
    bool input_complete = false;
    bool timed_out = false;
    bool input_interrupted = false;
    size_t consumed_characters = 0;
    size_t incomplete_character_bytes = 0;
    size_t delimiter_match_length = 0;
    bool delimiter_was_escaped = false;
    std::mbstate_t input_character_state{};
    auto track_multibyte_delimiter = [&](char ch, bool was_escaped) {
      if (delimiter.size() <= 1 || (exact_count && character_limit)) return false;
      if (delimiter_match_length == 0) {
        if (ch == delimiter.front()) {
          delimiter_match_length = 1;
          delimiter_was_escaped = was_escaped;
        }
      } else if (ch == delimiter[delimiter_match_length]) {
        ++delimiter_match_length;
      } else {
        delimiter_match_length = ch == delimiter.front() ? 1 : 0;
        delimiter_was_escaped = delimiter_match_length == 1 && was_escaped;
      }
      if (delimiter_match_length != delimiter.size()) return false;
      delimiter_match_length = 0;
      if (delimiter_was_escaped) {
        delimiter_was_escaped = false;
        return false;
      }
      line.resize(line.size() - delimiter.size());
      input_complete = true;
      return true;
    };
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::microseconds(std::max<int64_t>(timeout_microseconds, 0));
    if (use_readline) {
      readline_prefill = initial_text;
      rl_startup_hook = initial_text.empty() ? nullptr : insert_readline_prefill;
      begin_readline_input(nullptr);
      FILE *previous_readline_output = rl_outstream;
      rl_outstream = stderr;
      char *edited = readline(prompt.c_str());
      rl_outstream = previous_readline_output;
      input_interrupted = finish_readline_input() || consume_input_sigint();
      rl_startup_hook = nullptr;
      if (edited) {
        line = edited;
        std::free(edited);
        input_complete = true;
      }
      readline_prefill.clear();
    } else if (character_limit_set && character_limit == 0) {
      input_complete = true;
    }
    for (; !input_complete && !use_readline;) {
      if (timeout_microseconds >= 0) {
        pollfd descriptor{input_fd, POLLIN, 0};
        const auto remaining_microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        const int remaining = remaining_microseconds <= 0 ? 0 :
            static_cast<int>(std::min<int64_t>(INT_MAX, (remaining_microseconds + 999) / 1000));
        int ready;
        do {
          ready = poll(&descriptor, 1, remaining);
          if (ready < 0 && errno == EINTR &&
              (consume_input_sigint() || pending_signal == SIGINT)) {
            input_interrupted = true;
            break;
          }
        } while (ready < 0 && errno == EINTR);
        if (input_interrupted) break;
        if (ready == 0) { timed_out = true; break; }
        if (ready < 0) break;
      }
      char ch;
      ssize_t count = ::read(input_fd, &ch, 1);
      if (count < 0 && errno == EINTR &&
          (consume_input_sigint() || pending_signal == SIGINT)) {
        input_interrupted = true;
        break;
      }
      if (count < 0 && errno == EINTR)
        continue;
      if (count < 0) break;
      if (count == 0) {
        if (escaped) line += '\\';
        input_complete = false;
        break;
      }
      wchar_t decoded_character = 0;
      const size_t decoded = std::mbrtowc(&decoded_character, &ch, 1,
                                           &input_character_state);
      if (decoded == static_cast<size_t>(-2)) {
        ++incomplete_character_bytes;
      } else if (decoded == static_cast<size_t>(-1)) {
        consumed_characters += incomplete_character_bytes + 1;
        incomplete_character_bytes = 0;
        input_character_state = std::mbstate_t{};
      } else {
        ++consumed_characters;
        incomplete_character_bytes = 0;
        if (decoded == 0) input_character_state = std::mbstate_t{};
      }
      if (ch == '\0' && delimiter != std::string(1, '\0')) {
        delimiter_match_length = 0;
        continue;
      }
      if (delimiter.size() == 1 && ch == delimiter[0] &&
          !(exact_count && character_limit)) {
        if (escaped) {
          escaped = false;
          if (delimiter == "\n") {
            if (character_limit_set && consumed_characters >= character_limit) {
              input_complete = true;
              break;
            }
            continue;
          }
          line += ch;
          if (character_limit_set && consumed_characters >= character_limit) input_complete = true;
          if (input_complete) break;
          continue;
        }
        input_complete = true;
        break;
      }
      if (!raw && escaped) {
        line += ch;
        escaped = false;
        if (track_multibyte_delimiter(ch, true)) break;
        if (character_limit_set && consumed_characters >= character_limit) {
          input_complete = true;
          break;
        }
        continue;
      }
      if (!raw && ch == '\\') {
        delimiter_match_length = 0;
        escaped = true;
        if (character_limit_set && consumed_characters >= character_limit) {
          line += '\\';
          escaped = false;
          input_complete = true;
          break;
        }
        continue;
      }
      line += ch;
      if (track_multibyte_delimiter(ch, false)) break;
      if (character_limit_set && consumed_characters >= character_limit) {
        input_complete = true;
        break;
      }
    }
    if (terminal_state_changed)
      tcsetattr(input_fd, TCSANOW, &original_terminal_state);
    if (terminal_echo_disabled) {
      std::cerr << '\n';
    }
    const int read_status = input_interrupted ? 130 : input_complete ? 0 : timed_out ? 142 : 1;
    // Bash assigns the variables even when a timed read expires before
    // receiving its first byte; the failure status still reports the timeout.
    if (!input_complete && line.empty() && !timed_out) return read_status;
    if (array_mode) {
      if (state.namerefs.count(array_name)) {
        array_name = shell_resolve_nameref(array_name);
        if (array_name.empty() || !name_ok(array_name)) {
          std::cerr << "read: invalid nameref target\n";
          return 1;
        }
      }
      if (readonly_vars.count(array_name)) {
        std::cerr << "read: " << array_name << ": readonly variable\n";
        return 1;
      }
      if (state.associative_arrays.count(array_name)) {
        std::cerr << "read: " << array_name << ": not an indexed array\n";
        return 1;
      }
      if (!state.arrays.count(array_name) && state.vars.count(array_name)) {
        std::cerr << "read: " << array_name << ": not an indexed array\n";
        return 1;
      }
      state.arrays[array_name].clear();
      state.unset_vars.erase(array_name);
      shell_array_clear_set_indices(array_name);
      if (exact_count) {
        state.arrays[array_name].push_back(line);
        shell_array_mark_all_set(array_name);
        return read_status;
      }
      const char *environment_ifs = std::getenv("IFS");
      const std::string ifs = state.vars.count("IFS") ? state.vars["IFS"]
                              : environment_ifs ? environment_ifs : " \t\n";
      const auto fields = split_read_fields(line, ifs);
      for (const auto &field : fields) state.arrays[array_name].push_back(field.value);
      shell_array_mark_all_set(array_name);
      return read_status;
    }
    if (first_name >= a.size()) {
      if (!set_shell_variable("REPLY", line)) return 1;
      return read_status;
    }
    if (exact_count) {
      for (size_t i = first_name; i < a.size(); ++i) {
        const std::string value = i == first_name ? line : std::string();
        if (!set_shell_variable(a[i], value)) return 1;
      }
      return read_status;
    }
    const char *environment_ifs = std::getenv("IFS");
    const std::string ifs = state.vars.count("IFS") ? state.vars["IFS"]
                            : environment_ifs ? environment_ifs : " \t\n";
    const auto fields = split_read_fields(line, ifs);
    auto is_ifs_whitespace_at = [&](size_t offset) {
      return offset > 0 &&
             (line[offset - 1] == ' ' || line[offset - 1] == '\t' ||
              line[offset - 1] == '\n');
    };
    for (size_t i = first_name; i < a.size(); ++i) {
      std::string v;
      if (i + 1 == a.size()) {
        const size_t field_index = i - first_name;
        if (field_index < fields.size()) {
          size_t end = line.size();
          while (end > fields[field_index].begin && is_ifs_whitespace_at(end)) --end;
          v = line.substr(fields[field_index].begin, end - fields[field_index].begin);
        }
      } else if (i - first_name < fields.size()) v = fields[i - first_name].value;
      if (!set_shell_variable(a[i], v))
        return 1;
    }
    return read_status;
  }
  if (c == "mapfile" || c == "readarray") {
    bool strip_delimiter = false;
    bool origin_set = false;
    size_t line_limit = 0, skip_lines = 0, origin = 0, quantum = 0;
    int input_fd = STDIN_FILENO;
    std::string delimiter = "\n";
    std::string callback;
    std::string array_name = "MAPFILE";
    bool target_set = false;
    auto parse_unsigned = [&](const std::string &text, size_t &value,
                              size_t limit = std::numeric_limits<size_t>::max()) {
      if (text.empty()) return false;
      char *end = nullptr;
      errno = 0;
      unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
      const size_t first_nonspace = text.find_first_not_of(" \t\r\n");
      if (errno || end == text.c_str() || *end ||
          first_nonspace == std::string::npos || text[first_nonspace] == '-' ||
          parsed > std::numeric_limits<size_t>::max() || parsed > limit)
        return false;
      value = static_cast<size_t>(parsed);
      return true;
    };
    for (size_t i = 1; i < a.size(); ++i) {
      const std::string &arg = a[i];
      if (arg == "--") {
        if (++i < a.size()) { array_name = a[i++]; target_set = true; }
        if (i != a.size()) {
          std::cerr << c << ": too many arguments\n";
          return 2;
        }
        break;
      }
      if (arg.size() > 1 && arg[0] == '-') {
        for (size_t option_index = 1; option_index < arg.size(); ++option_index) {
          const char option = arg[option_index];
          if (option == 't') {
            strip_delimiter = true;
            continue;
          }
          if (std::string("nsOudCc").find(option) == std::string::npos) {
            std::cerr << c << ": invalid option -- '" << option << "'\n";
            return 2;
          }
          std::string value;
          if (option_index + 1 < arg.size()) {
            value = arg.substr(option_index + 1);
            option_index = arg.size();
          } else if (i + 1 < a.size()) {
            value = a[++i];
          } else {
            std::cerr << c << ": option requires an argument -- '" << option << "'\n";
            return 2;
          }
          if (option == 'n') {
            if (!parse_unsigned(value, line_limit)) { std::cerr << c << ": invalid line count\n"; return 2; }
          } else if (option == 's') {
            if (!parse_unsigned(value, skip_lines)) { std::cerr << c << ": invalid skip count\n"; return 2; }
          } else if (option == 'O') {
            if (!parse_unsigned(value, origin, 1000000)) { std::cerr << c << ": invalid origin\n"; return 2; }
            origin_set = true;
          } else if (option == 'u') {
            size_t fd = 0;
            if (!parse_unsigned(value, fd) || fd > static_cast<size_t>(INT_MAX) || fcntl(static_cast<int>(fd), F_GETFD) < 0) {
              std::cerr << c << ": invalid file descriptor: " << value << '\n';
              return 2;
            }
            input_fd = static_cast<int>(fd);
          } else if (option == 'd') {
            if (value.empty()) {
              delimiter.assign(1, '\0');
            } else {
              std::mbstate_t delimiter_state{};
              wchar_t delimiter_character = 0;
              const size_t delimiter_length = std::mbrtowc(
                  &delimiter_character, value.data(), value.size(), &delimiter_state);
              const size_t first_character_length =
                  delimiter_length == 0 || delimiter_length == static_cast<size_t>(-1) ||
                          delimiter_length == static_cast<size_t>(-2)
                      ? 1 : delimiter_length;
              delimiter = value.substr(0, first_character_length);
            }
          } else if (option == 'C') callback = value;
          else {
            if (!parse_unsigned(value, quantum) || quantum == 0) {
              std::cerr << c << ": invalid quantum\n";
              return 2;
            }
          }
          break;
        }
      } else if (!arg.empty() && arg[0] == '-') {
        std::cerr << c << ": invalid option -- '-'\n";
        return 2;
      } else if (!target_set) { array_name = arg; target_set = true; }
      else {
        std::cerr << c << ": too many arguments\n";
        return 2;
      }
    }
    if (!name_ok(array_name)) {
      std::cerr << c << ": " << array_name << ": not a valid identifier\n";
      return 2;
    }
    if (state.namerefs.count(array_name)) {
      array_name = shell_resolve_nameref(array_name);
      if (array_name.empty() || !name_ok(array_name)) {
        std::cerr << c << ": invalid nameref target\n";
        return 1;
      }
    }
    if (readonly_vars.count(array_name)) {
      std::cerr << c << ": " << array_name << ": readonly variable\n";
      return 1;
    }
    if (state.associative_arrays.count(array_name)) {
      std::cerr << c << ": " << array_name << ": not an indexed array\n";
      return 1;
    }
    if (!state.arrays.count(array_name) && state.vars.count(array_name)) {
      std::cerr << c << ": " << array_name << ": not an indexed array\n";
      return 1;
    }
    if (!origin_set) {
      state.arrays[array_name].clear();
      shell_array_clear_set_indices(array_name);
    }
    if (callback.empty() && quantum == 0) quantum = 5000;
    if (!callback.empty() && quantum == 0) quantum = 5000;
    size_t copied = 0;
    bool read_error = false;
    auto read_line = [&](std::string &line, bool &terminated) {
      line.clear();
      terminated = false;
      bool got_data = false;
      for (;;) {
        char ch = 0;
        const ssize_t amount = ::read(input_fd, &ch, 1);
        if (amount < 0 && errno == EINTR) continue;
        if (amount < 0) { read_error = true; return false; }
        if (amount == 0) return got_data;
        got_data = true;
        line += ch;
        if (line.size() >= delimiter.size() &&
            line.compare(line.size() - delimiter.size(), delimiter.size(), delimiter) == 0) {
          line.resize(line.size() - delimiter.size());
          terminated = true;
          return true;
        }
      }
    };
    std::string line;
    bool terminated = false;
    for (size_t skipped = 0; skipped < skip_lines; ++skipped)
      if (!read_line(line, terminated)) break;
    while (!read_error && (line_limit == 0 || copied < line_limit)) {
      if (!read_line(line, terminated)) break;
      if (terminated && !strip_delimiter) line += delimiter;
      if (!callback.empty() && quantum > 0 && (copied + 1) % quantum == 0) {
        auto shell_quote = [](const std::string &text) {
          std::string quoted = "'";
          for (char ch : text) {
            if (ch == '\'') quoted += "'\\''";
            else quoted += ch;
          }
          return quoted + "'";
        };
        execute_line(callback + " " + std::to_string(origin + copied) + " " + shell_quote(line));
      }
      if (!set_shell_variable(array_name + "[" + std::to_string(origin + copied) + "]", line)) {
        read_error = true;
        break;
      }
      ++copied;
    }
    const int result = read_error ? 1 : 0;
    set_last_status(result);
    return result;
  }
  if (c == "source" || c == ".") {
    size_t filename_index = 1;
    std::optional<std::string> explicit_source_path;
    if (filename_index < a.size() && a[filename_index] == "-p") {
      if (filename_index + 2 >= a.size()) {
        std::cerr << c << ": option requires an argument -- 'p'\n";
        return 2;
      }
      explicit_source_path = a[filename_index + 1];
      filename_index += 2;
    }
    if (filename_index >= a.size())
      return 2;
    const std::string &filename = a[filename_index];
    const bool has_slash = filename.find('/') != std::string::npos;
    std::filesystem::path source_file(filename);
    std::ifstream f;
    if (has_slash || (!explicit_source_path && !state.sourcepath)) {
      f.open(source_file);
    } else {
      std::string search_path;
      if (explicit_source_path) {
        search_path = *explicit_source_path;
      } else if (const auto path_value = shell_path_value()) {
        search_path = *path_value;
      }
      size_t begin = 0;
      while (begin <= search_path.size()) {
        const size_t end = search_path.find(':', begin);
        const std::string directory = search_path.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        const std::filesystem::path candidate =
            (directory.empty() ? std::filesystem::path(".") : std::filesystem::path(directory)) /
            filename;
        std::ifstream candidate_file(candidate);
        if (candidate_file) {
          source_file = candidate;
          f = std::move(candidate_file);
          break;
        }
        if (end == std::string::npos) break;
        begin = end + 1;
      }
      // Bash searches PATH first, then the current directory in its default
      // (non-POSIX) mode. An explicit `-p path` restricts the search to path.
      if (!f && !explicit_source_path && !state.posix_mode)
        f.open(filename);
    }
    if (!f) {
      std::cerr << c << ": " << filename << ": file not found\n";
      return 1;
    }
    const std::string resolved_source = source_file.string();
    if (source_file.extension() == ".janet" && !state.force_shell_source) {
      if (!janet_runtime_start()) {
        std::cerr << "dax: could not initialize Janet runtime\n";
        return 1;
      }
      return janet_source_file(
          resolved_source, std::vector<std::string>(a.begin() + filename_index + 1, a.end()));
    }
    std::ostringstream source;
    source << f.rdbuf();
    const size_t saved_lineno = state.current_lineno;
    const std::string call_source = active_source_name();
    const bool saved_returning = returning;
    const bool temporary_positionals = a.size() > filename_index + 1;
    std::vector<std::string> saved_positionals;
    if (temporary_positionals) {
      saved_positionals = positionals;
      positionals.assign(a.begin() + filename_index + 1, a.end());
    }
    state.source_stack.push_back(resolved_source);
    state.function_stack.push_back("source");
    state.function_frame_flags.push_back(false);
    state.function_source_stack.push_back(resolved_source);
    state.function_call_lines.push_back(saved_lineno);
    state.function_call_sources.push_back(call_source);
    refresh_stack_variables();
    returning = false;
    const bool inherit_debug_trap = state.functrace;
    std::optional<std::string> saved_debug_trap;
    const size_t saved_debug_trap_function_depth = state.debug_trap_function_depth;
    if (!inherit_debug_trap) {
      const auto debug_trap = traps.find(-2);
      if (debug_trap != traps.end()) saved_debug_trap = debug_trap->second;
      traps.erase(-2);
    }
    execute_line(source.str());
    if (!inherit_debug_trap) {
      if (saved_debug_trap) traps[-2] = *saved_debug_trap;
      else traps.erase(-2);
      state.debug_trap_function_depth = saved_debug_trap_function_depth;
    }
    run_return_trap();
    returning = saved_returning;
    state.function_call_lines.pop_back();
    state.function_call_sources.pop_back();
    state.function_source_stack.pop_back();
    state.function_stack.pop_back();
    state.function_frame_flags.pop_back();
    state.source_stack.pop_back();
    state.current_lineno = saved_lineno;
    if (temporary_positionals) positionals = std::move(saved_positionals);
    refresh_stack_variables();
    return last_status();
  }
  if (c == "eval") {
    execute_line(join(a));
    return last_status();
  }
  if (c == "return") {
    if (local_scopes.empty() && state.source_stack.empty()) {
      std::cerr << "return: can only return from a function\n";
      return 1;
    }
    returning = true;
    if (a.size() > 2) {
      returning = false;
      std::cerr << "return: too many arguments\n";
      return 1;
    }
    if (a.size() == 1) return status_value;
    char *end = nullptr;
    errno = 0;
    const long parsed = std::strtol(a[1].c_str(), &end, 10);
    if (errno == ERANGE || end == a[1].c_str() || !end || *end) {
      returning = false;
      std::cerr << "return: " << a[1] << ": numeric argument required\n";
      return 2;
    }
    return static_cast<int>(static_cast<unsigned long>(parsed) & 0xffUL);
  }
  if (c == "shift") {
    if (a.size() > 2) {
      std::cerr << "shift: too many arguments\n";
      return 2;
    }
    long long n = 1;
    if (a.size() == 2) {
      char *end = nullptr;
      errno = 0;
      n = std::strtoll(a[1].c_str(), &end, 10);
      if (errno || end == a[1].c_str() || *end) {
        std::cerr << "shift: " << a[1] << ": numeric argument required\n";
        return 2;
      }
    }
    if (n < 0) {
      std::cerr << "shift: " << n << ": shift count must be non-negative\n";
      return 1;
    }
    if (static_cast<unsigned long long>(n) > positionals.size()) {
      if (state.shift_verbose)
        std::cerr << "shift: " << n << ": shift count out of range\n";
      return 1;
    }
    positionals.erase(positionals.begin(), positionals.begin() + n);
    return 0;
  }
  if (c == "break" || c == "continue") {
    if (a.size() > 2) {
      std::cerr << c << ": too many arguments\n";
      return 2;
    }
    unsigned levels = 1;
    if (a.size() == 2) {
      char *end = nullptr;
      errno = 0;
      const long long parsed = std::strtoll(a[1].c_str(), &end, 10);
      if (errno || end == a[1].c_str() || *end) {
        std::cerr << c << ": loop count out of range\n";
        return 2;
      }
      if (parsed < 1) {
        std::cerr << c << ": loop count out of range\n";
        return 1;
      }
      levels = static_cast<unsigned>(std::min<unsigned long long>(
          static_cast<unsigned long long>(parsed), static_cast<unsigned long long>(loop_depth)));
    }
    if (loop_depth == 0) {
      std::cerr << c << ": only meaningful in a for, while, or until loop\n";
      return 0;
    }
    loop_control_levels = levels;
    if (c == "break") loop_break = true;
    else loop_continue = true;
    return 0;
  }
  if (c == "getopts") {
    if (a.size() < 3) {
      std::cerr << "getopts: usage: getopts optstring name\n";
      return 2;
    }
    std::vector<std::string> option_args;
    if (a.size() > 3) option_args.assign(a.begin() + 3, a.end());
    else option_args = positionals;
    if (const char *current = getenv("OPTIND")) {
      char *end = nullptr;
      const unsigned long parsed = std::strtoul(current, &end, 10);
      if (end != current && *end == '\0' && parsed > 0 && parsed != getopts_index) {
        getopts_index = parsed;
        getopts_char = 1;
      }
    }
    auto sync_optind = [&]() {
      const std::string value = std::to_string(getopts_index);
      const size_t character = getopts_char;
      const bool assigned = set_shell_variable("OPTIND", value);
      getopts_char = character;
      return assigned;
    };
    auto set_optarg = [&](const std::string &value) {
      return set_shell_variable("OPTARG", value);
    };
    auto clear_optarg = [&]() {
      return handle_builtin({"unset", "OPTARG"}).value_or(1) == 0;
    };
    auto set_result = [&](const std::string &value) {
      if (!name_ok(a[2])) {
        std::cerr << "getopts: " << a[2] << ": not a valid identifier\n";
        return false;
      }
      return set_shell_variable(a[2], value);
    };
    if (getopts_index > option_args.size()) {
      if (!clear_optarg() || !set_result("?") || !sync_optind()) return 1;
      return 1;
    }
    const std::string &arg = option_args[getopts_index - 1];
    if (arg == "--" || arg.size() < 2 || arg[0] != '-') {
      if (arg == "--") ++getopts_index;
      getopts_char = 1;
      if (!clear_optarg() || !set_result("?") || !sync_optind()) return 1;
      return 1;
    }
    char opt = arg[getopts_char++];
    auto pos = a[1].find(opt);
    const bool silent = !a[1].empty() && a[1][0] == ':';
    const bool invalid_option = pos == std::string::npos || opt == ':';
    std::string result(1, invalid_option ? '?' : opt);
    if (invalid_option) {
      if (silent) {
        if (!set_optarg(std::string(1, opt))) return 1;
      }
      else {
        if (!clear_optarg()) return 1;
        if (vars["OPTERR"] != "0")
          std::cerr << shell_name << ": illegal option -- " << opt << '\n';
      }
    } else if (pos + 1 < a[1].size() &&
        a[1][pos + 1] == ':') {
      if (getopts_char < arg.size()) {
        if (!set_optarg(arg.substr(getopts_char))) return 1;
        getopts_char = arg.size();
      } else if (getopts_index < option_args.size()) {
        if (!set_optarg(option_args[getopts_index])) return 1;
        ++getopts_index;
      } else {
        result = silent ? ":" : "?";
        if (silent) {
          if (!set_optarg(std::string(1, opt))) return 1;
        }
        else {
          if (!clear_optarg()) return 1;
          if (vars["OPTERR"] != "0")
            std::cerr << shell_name << ": option requires an argument -- " << opt << '\n';
        }
      }
    } else if (!clear_optarg()) return 1;
    if (getopts_char >= arg.size()) {
      getopts_char = 1;
      ++getopts_index;
    }
    if (!set_result(result) || !sync_optind()) return 1;
    return 0;
  }
  if (c == "trap") {
    size_t first = 1;
    if (first < a.size() && a[first] == "--") ++first;
    auto signal_label = [](int signal) {
      if (signal == -1) return std::string("ERR");
      if (signal == -2) return std::string("DEBUG");
      if (signal == -3) return std::string("RETURN");
      if (signal == 0) return std::string("EXIT");
      for (const auto &[number, full_name] : shell_signal_names())
        if (number == signal) return full_name.substr(3);
      return std::to_string(signal);
    };
    auto trap_action = [&](int signal) -> std::optional<std::string> {
      const auto trap = traps.find(signal);
      if (trap != traps.end()) return trap->second;
      if (state.initially_ignored_signals.count(signal)) return std::string();
      return std::nullopt;
    };
    if (first < a.size() && a[first] == "-l") {
      ++first;
      std::vector<std::pair<int, std::string>> selected;
      if (first == a.size()) {
        selected = shell_signal_names();
      } else {
        for (; first < a.size(); ++first) {
          if (a[first] == "0" || a[first] == "EXIT") {
            selected.emplace_back(0, "EXIT");
            continue;
          }
          const int number = signal_number(a[first]);
          const auto found = std::find_if(shell_signal_names().begin(),
                                          shell_signal_names().end(),
              [number](const auto &entry) { return entry.first == number; });
          if (found == shell_signal_names().end()) {
            std::cerr << "trap: " << a[first] << ": invalid signal specification\n";
            return 1;
          }
          selected.push_back(*found);
        }
      }
      for (size_t index = 0; index < selected.size(); ++index) {
        std::cout << std::setw(2) << selected[index].first << ") "
                  << selected[index].second;
        if ((index + 1) % 5 == 0 || index + 1 == selected.size())
          std::cout << '\n';
        else
          std::cout << "  ";
      }
      return 0;
    }
    if (first < a.size() && a[first] == "-P") {
      ++first;
      if (first < a.size() && a[first] == "--") ++first;
      if (first == a.size()) {
        std::cerr << "trap: -P requires at least one signal specification\n";
        return 2;
      }
      for (; first < a.size(); ++first) {
        const int signal = a[first] == "EXIT" || a[first] == "0" ? 0
                           : a[first] == "ERR" ? -1
                           : a[first] == "DEBUG" ? -2
                           : a[first] == "RETURN" ? -3
                                                : signal_number(a[first]);
        if (signal == 0 && a[first] != "EXIT" && a[first] != "0") {
          std::cerr << "trap: " << a[first] << ": invalid signal specification\n";
          return 1;
        }
        const auto trap = traps.find(signal);
        if (trap != traps.end()) std::cout << trap->second << '\n';
      }
      return 0;
    }
    if (first < a.size() && a[first] == "-p") {
      ++first;
      if (first < a.size() && a[first] == "--") ++first;
      std::vector<int> signals;
      if (first == a.size()) {
        signals.reserve(traps.size());
        for (const auto &[signal, command] : traps) {
          (void)command;
          signals.push_back(signal);
        }
        for (const int signal : state.initially_ignored_signals)
          if (!traps.count(signal)) signals.push_back(signal);
        std::sort(signals.begin(), signals.end());
      } else {
        for (; first < a.size(); ++first) {
          const int signal = a[first] == "EXIT" || a[first] == "0" ? 0
                             : a[first] == "ERR" ? -1
                             : a[first] == "DEBUG" ? -2
                             : a[first] == "RETURN" ? -3
                                                     : signal_number(a[first]);
          if (signal != 0 || a[first] == "EXIT" || a[first] == "0" ||
              a[first] == "DEBUG" || a[first] == "RETURN")
            signals.push_back(signal);
        }
      }
      for (const int signal : signals) {
        const auto action = trap_action(signal);
        if (!action) continue;
        std::cout << "trap -- " << quote_shell_word(*action) << ' '
                  << signal_label(signal) << '\n';
      }
      return 0;
    }
    if (first == a.size()) {
      std::vector<int> signals;
      signals.reserve(traps.size());
      for (const auto &[signal, command] : traps) {
        (void)command;
        signals.push_back(signal);
      }
      for (const int signal : state.initially_ignored_signals)
        if (!traps.count(signal)) signals.push_back(signal);
      std::sort(signals.begin(), signals.end());
      for (const int signal : signals) {
        const auto action = trap_action(signal);
        if (!action) continue;
        std::cout << "trap -- " << quote_shell_word(*action) << ' '
                  << signal_label(signal) << '\n';
      }
      return 0;
    }
    if (first + 1 >= a.size())
      return 2;
    const bool exit_trap = a[first + 1] == "EXIT" || a[first + 1] == "0";
    const bool err_trap = a[first + 1] == "ERR";
    const bool debug_trap = a[first + 1] == "DEBUG";
    const bool return_trap = a[first + 1] == "RETURN";
    int sig = exit_trap ? 0 : err_trap ? -1 : debug_trap ? -2
                             : return_trap ? -3 : signal_number(a[first + 1]);
    if (!exit_trap && !err_trap && !debug_trap && !return_trap && sig == 0) {
      std::cerr << "trap: " << a[first + 1] << ": unsupported or invalid signal\n";
      return 1;
    }
    if (sig > 0 && state.initially_ignored_signals.count(sig) &&
        !state.interactive_shell) {
      std::cerr << "trap: " << a[first + 1]
                << ": signal ignored on entry cannot be trapped or reset\n";
      return 1;
    }
    if (a[first] == "-" || a[first].empty()) {
      const auto disposition = state.initially_ignored_signals.count(sig)
                                   ? SIG_IGN : SIG_DFL;
      if (sig > 0 && signal(sig, disposition) == SIG_ERR) {
        std::cerr << "trap: " << a[first + 1] << ": cannot reset signal disposition\n";
        return 1;
      }
      traps.erase(sig);
      if (sig == 0) state.exit_trap_active = false;
      if (sig == -2) state.debug_trap_function_depth = 0;
      if (sig == -3) state.return_trap_function_depth = 0;
    } else {
      if (sig > 0 && signal(sig, [](int s) { pending_signal = s; }) == SIG_ERR) {
        std::cerr << "trap: " << a[first + 1] << ": cannot trap signal\n";
        return 1;
      }
      traps[sig] = a[first];
      if (sig == 0) state.exit_trap_active = true;
      const size_t function_depth = static_cast<size_t>(std::count(
          state.function_frame_flags.begin(), state.function_frame_flags.end(), true));
      if (sig == -2) state.debug_trap_function_depth = function_depth;
      if (sig == -3) state.return_trap_function_depth = function_depth;
    }
    return 0;
  }
  if (c == "autoload") {
    std::cerr << "autoload: unsupported (no function autoload path)\n";
    return 2;
  }
  if (functions.count(c)) return execute_function_call(a);
  return {};
}
} // namespace
std::optional<long long> shell_arithmetic_special_value(const std::string &name) {
  if (state.disabled_special_parameters.count(name)) return {};
  if (name == "BASHPID") return static_cast<long long>(getpid());
  if (name == "BASH_SUBSHELL") return static_cast<long long>(state.subshell_level);
  if (name == "LINENO") return static_cast<long long>(state.current_lineno);
  if (name == "SECONDS") {
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - state.seconds_epoch).count();
    return state.seconds_offset + elapsed;
  }
  if (name == "EPOCHSECONDS") {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
  }
  if (name == "RANDOM") {
    const auto value = next_bash_random();
    state.vars[name] = std::to_string(value);
    return static_cast<long long>(value);
  }
  if (name == "SRANDOM") {
    const auto value = next_bash_secure_random();
    state.vars[name] = std::to_string(value);
    return static_cast<long long>(value);
  }
  return {};
}
std::optional<std::string> shell_arithmetic_special_string(const std::string &name) {
  if (state.disabled_special_parameters.count(name)) return {};
  if (const auto numeric = shell_arithmetic_special_value(name))
    return std::to_string(*numeric);
  if (name == "EPOCHREALTIME") {
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream timestamp;
    timestamp << micros / 1000000 << '.' << std::setw(6) << std::setfill('0')
              << micros % 1000000;
    return timestamp.str();
  }
  if (name == "BASH_COMMAND") return state.bash_command;
  if (name == "BASH_EXECUTION_STRING" && state.command_string_mode)
    return state.execution_string;
  if (name == "BASH_TRAPSIG" && state.trap_signal != 0)
    return std::to_string(state.trap_signal);
  if (name == "HISTCMD" && state.interactive_shell)
    return std::to_string(History::get_all().size());
  if (name == "SHELLOPTS") return shell_enabled_options();
  if (name == "BASHOPTS") return shell_enabled_shopt_options();
  if (name == "_") return state.last_argument;
  return {};
}
bool shell_set_assignment(const std::string &value, bool export_it) {
  return setvar(value, export_it);
}
bool shell_function_precedes_builtin(const std::string &name) {
  if (!functions.count(name)) return false;
  return !state.posix_mode || !shell_is_posix_special_builtin(name);
}
bool shell_is_posix_special_builtin(const std::string &name) {
  static const std::unordered_set<std::string> posix_special_builtins = {
      ".", "source", ":", "break", "continue", "eval", "exec", "exit",
      "export", "readonly", "return", "set", "shift", "times", "trap", "unset"};
  return posix_special_builtins.count(name) != 0;
}
int shell_execute_function(const std::vector<std::string> &args) {
  return execute_function_call(args);
}
bool shell_valid_name(const std::string &name) { return name_ok(name); }
bool shell_command_path_is_ignored(const std::string &path) {
  return command_path_is_ignored(path);
}
std::optional<std::string> shell_find_command(const std::string &name,
                                              bool command_execution,
                                              bool use_hash_cache) {
  return find_command(name, command_execution, use_hash_cache);
}
std::optional<std::string> shell_path_value() {
  if (state.unset_vars.count("PATH")) return {};
  const auto shell_value = state.vars.find("PATH");
  if (shell_value != state.vars.end()) return shell_value->second;
  if (const char *environment_value = std::getenv("PATH"))
    return std::string(environment_value);
  return {};
}
std::optional<std::vector<std::string>> shell_programmable_completions(
    const std::string &command, const std::string &word) {
  if (!state.progcomp) return std::nullopt;
  const std::string special_prefix(1, '\x01');
  auto spec = state.completion_specs.find(command);
  if (spec == state.completion_specs.end() &&
      (command.empty() || command.front() != '\x01')) {
    if (command.find('/') != std::string::npos) {
      const size_t slash = command.find_last_of('/');
      spec = state.completion_specs.find(command.substr(slash + 1));
    }
    if (spec == state.completion_specs.end())
      spec = state.completion_specs.find(special_prefix + "default");
  }
  if (spec == state.completion_specs.end()) return {};
  state.completion_active = true;
  state.active_completion_key = spec->first;
  state.active_completion_options = spec->second;
  std::ostringstream captured;
  std::streambuf *previous_output = std::cout.rdbuf(captured.rdbuf());
  std::vector<std::string> args = {"compgen"};
  args.insert(args.end(), spec->second.begin(), spec->second.end());
  args.push_back(word);
  std::optional<int> status;
  try {
    status = handle_builtin(args);
  } catch (...) {
    std::cout.rdbuf(previous_output);
    state.completion_active = false;
    state.active_completion_key.clear();
    state.active_completion_options.clear();
    throw;
  }
  std::cout.flush();
  std::cout.rdbuf(previous_output);
  state.last_completion_nospace = false;
  bool allow_default_completion = false;
  for (size_t i = 0; i + 1 < state.active_completion_options.size(); ++i) {
    if (state.active_completion_options[i] != "-o") continue;
    const std::string &option = state.active_completion_options[i + 1];
    if (option == "nospace") state.last_completion_nospace = true;
    if (option == "default" || option == "bashdefault") allow_default_completion = true;
    ++i;
  }
  state.completion_active = false;
  state.active_completion_key.clear();
  state.active_completion_options.clear();
  if (!status || *status > 1) return std::vector<std::string>{};
  std::vector<std::string> completions;
  std::istringstream lines(captured.str());
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    completions.push_back(std::move(line));
  }
  if (completions.empty() && allow_default_completion) return {};
  return completions;
}
bool shell_last_completion_nospace() { return state.last_completion_nospace; }
int shell_external_command(const std::vector<std::string> &args,
                           bool use_hash_cache) {
  return external(args, true, use_hash_cache);
}
int shell_signal_number(const std::string &name) { return signal_number(name); }
std::optional<int> handle_builtin(const std::vector<std::string> &a) {
  return run(a);
}
bool shell_define_alias(const std::string &name, const std::string &value) {
  return handle_builtin({"alias", name + "=" + value}).value_or(1) == 0;
}
bool is_builtin(const std::string &n) {
  const auto &names = shell_builtin_names();
  return state.disabled_builtins.count(n) == 0 &&
         std::find(names.begin(), names.end(), n) != names.end();
}
const std::vector<std::string> &shell_builtin_names() {
  static const std::vector<std::string> names = {
      "exit",    "cd",       "pwd",     "echo",   "export",  "typeset", "declare",
      "local",   "readonly", "unset",   "alias",  "unalias", "history",
      "command", "builtin",  "type",    "whence", "which",   "compgen", "complete", "compopt", "bind", "pushd",
      "popd",    "dirs",     "jobs",    "fg",     "bg",      "wait",
      "disown",  "kill",     "suspend", "read",   "mapfile", "readarray", "printf",  "test",
      "[",       "true",     "false",   "set",    "setopt",  "unsetopt",
      "hash",    "shopt",    "source",  ".",      "eval",    "return",  "shift",
      "getopts", "trap",     "autoload", "break", "continue", ":",
      "umask", "times", "ulimit", "exec", "fc", "let", "caller", "enable", "logout", "help"};
  return names;
}
bool shell_command_resolves(const std::string &name) {
  return is_builtin(name) || aliases.count(name) != 0 || functions.count(name) != 0 || janet_command_exists(name) ||
         find_command(name).has_value();
}
std::vector<std::string> shell_completion_candidates(const std::string &prefix,
                                                     bool command_position,
                                                     bool variable_name,
                                                     const std::string &directory_prefix,
                                                     bool empty_line_completion) {
  std::vector<std::string> result;
  auto add = [&](const std::string &candidate) {
    if (candidate.rfind(prefix, 0) == 0 &&
        std::find(result.begin(), result.end(), candidate) == result.end())
      result.push_back(candidate);
  };
  if (variable_name) {
    for (const auto &[name, value] : vars) { (void)value; add(name); }
    extern char **environ;
    for (char **entry = environ; entry && *entry; ++entry) {
      const char *equals = std::strchr(*entry, '=');
      if (equals) add(std::string(*entry, static_cast<size_t>(equals - *entry)));
    }
    std::sort(result.begin(), result.end());
    return result;
  }
  if (command_position && prefix.find('/') == std::string::npos) {
    static const std::vector<std::string> builtins = {
        ":", "alias", "bg", "bind", "break", "builtin", "cd", "command", "continue",
        "dirs", "disown", "echo", "enable", "eval", "exec", "exit", "logout", "compgen", "complete", "compopt", "export", "false",
        "fc", "fg", "getopts", "hash", "help", "history", "jobs", "kill", "local",
        "mapfile", "readarray", "popd", "printf", "pushd", "pwd", "read", "readonly", "return", "set",
        "setopt", "shopt", "shift", "source", "suspend", "test", "times", "trap", "true",
        "type", "typeset", "declare", "caller", "let", "ulimit", "unalias", "unset", "unsetopt", "wait", "which",
        "whence"};
    for (const auto &name : builtins) if (is_builtin(name)) add(name);
    for (const auto &[name, value] : aliases) { (void)value; add(name); }
    for (const auto &[name, body] : functions) { (void)body; add(name); }
    for (const auto &name : janet_command_names()) add(name);
    const auto path = shell_state().no_empty_cmd_completion && empty_line_completion
        ? std::optional<std::string>() : shell_path_value();
    size_t path_start = 0;
    while (path && path_start <= path->size()) {
      const size_t separator = path->find(':', path_start);
      std::string directory = path->substr(
          path_start, separator == std::string::npos
                          ? std::string::npos : separator - path_start);
      if (directory.empty()) directory = ".";
      std::error_code error;
      for (std::filesystem::directory_iterator it(directory, error), end;
           !error && it != end; it.increment(error)) {
        const std::string name = it->path().filename().string();
        const std::string full_path = it->path().string();
        std::error_code type_error;
        if (name.rfind(prefix, 0) == 0 && access(full_path.c_str(), X_OK) == 0 &&
            !it->is_directory(type_error) && !type_error &&
            !command_path_is_ignored(full_path))
          add(name);
      }
      if (separator == std::string::npos) break;
      path_start = separator + 1;
    }
    std::sort(result.begin(), result.end());
    return result;
  }
  if (!variable_name) {
    std::string fignore;
    if (!state.unset_vars.count("FIGNORE")) {
      const auto variable = state.vars.find("FIGNORE");
      const char *environment_value = std::getenv("FIGNORE");
      if (variable != state.vars.end()) fignore = variable->second;
      else if (environment_value) fignore = environment_value;
    }
    auto ignored_completion_suffix = [&](const std::string &name) {
      size_t begin = 0;
      while (begin <= fignore.size()) {
        const size_t end = fignore.find(':', begin);
        const std::string suffix = fignore.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!suffix.empty() && name.size() >= suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
          return true;
        if (end == std::string::npos) break;
        begin = end + 1;
      }
      return false;
    };
    std::filesystem::path directory = directory_prefix.empty()
        ? std::filesystem::path(".") : std::filesystem::path(directory_prefix);
    if (!directory_prefix.empty() && directory_prefix[0] == '~') {
      if (const char *home = std::getenv("HOME"))
        directory = std::filesystem::path(std::string(home) + directory_prefix.substr(1));
    }
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory, error), end;
         !error && it != end; it.increment(error)) {
      const std::string name = it->path().filename().string();
      if (prefix.empty() && !name.empty() && name.front() == '.') continue;
      if (name.rfind(prefix, 0) != 0) continue;
      if (ignored_completion_suffix(name)) continue;
      std::string candidate = name;
      std::error_code type_error;
      if (it->is_directory(type_error) && !type_error) candidate.push_back('/');
      add(candidate);
    }
    std::sort(result.begin(), result.end());
    return result;
  }
  return result;
}
void remember_job(int pid, const std::string &cmd) {
  int id = jobs.empty() ? 1 : jobs.back().id + 1;
  state.last_background_pid = pid;
  jobs.push_back({id, pid, cmd, false});
  std::cout << '[' << id << "] " << pid << '\n';
}
std::vector<std::string> take_completed_job_notifications() {
  std::vector<std::string> notifications;
  reap_completed_process_substitutions();
  if (!notify_jobs) return notifications;
  for (auto it = jobs.begin(); it != jobs.end();) {
    int status = 0;
    const pid_t result = waitpid(it->pid, &status, WNOHANG);
    if (result == it->pid && (WIFEXITED(status) || WIFSIGNALED(status))) {
      notifications.push_back("[" + std::to_string(it->id) + "]  Done " +
                              it->command);
      it = jobs.erase(it);
    } else {
      ++it;
    }
  }
  return notifications;
}
void set_last_status(int s) { status_value = s; }
int last_status() { return status_value; }
bool shell_should_exit() { return exiting; }
int requested_exit_status() { return exit_value; }
void clear_exit_request() { exiting = false; }
void shell_finish_one_command() {
  if (!state.one_command || exiting) return;
  exit_value = status_value & 255;
  exiting = true;
}
static std::string run_command_substitution(const std::string &command) {
  // Bash's $(<file) shortcut reads a file without launching a `cat` process.
  // It is equivalent to a command consisting solely of an input redirection,
  // with normal shell expansion applied to the redirection target.
  const auto shortcut_tokens = tokenize(command);
  if (shortcut_tokens.size() == 2 && shortcut_tokens[0] == "<") {
    std::vector<std::string> target{
        static_cast<const std::string &>(shortcut_tokens[1])};
    const bool prior_expansion_error = expansion_error;
    const size_t prior_expansion_error_index = expansion_error_index;
    const size_t process_substitution_start = process_substitution_mark();
    begin_word_expansion();
    const auto redirection_expansion_mode = state.posix_mode
        ? state.interactive_shell
              ? WordExpansionMode::InteractiveRedirection
              : WordExpansionMode::Redirection
        : WordExpansionMode::Normal;
    expand_words(target, redirection_expansion_mode);
    const bool target_expanded = word_expansion_succeeded();
    finish_process_substitutions(process_substitution_start);
    expansion_error = prior_expansion_error;
    expansion_error_index = prior_expansion_error_index;
    if (!target_expanded || target.size() != 1) {
      std::cerr << "dax: ambiguous redirect\n";
      substitution_status = 1;
      substitution_ran = true;
      return {};
    }
    {
      errno = 0;
      std::ifstream file(target.front(), std::ios::binary);
      if (!file) {
        const int error = errno ? errno : EIO;
        std::cerr << "dax: " << target.front() << ": " << std::strerror(error) << '\n';
        substitution_status = 1;
        substitution_ran = true;
        return {};
      }
      std::string result((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
      while (!result.empty() && result.back() == '\n') result.pop_back();
      substitution_status = 0;
      substitution_ran = true;
      return result;
    }
  }
  int output_pipe[2];
  if (pipe(output_pipe) != 0) { substitution_status = 1; substitution_ran = true; return {}; }
  std::cout.flush();
  pid_t pid = fork();
  if (pid < 0) {
    close(output_pipe[0]); close(output_pipe[1]);
    substitution_status = 1; substitution_ran = true; return {};
  }
  if (pid == 0) {
    state.err_trap_suppressed = state.err_trap_suppressed || !state.errtrace;
    enter_subshell_environment();
    if (!state.inherit_errexit) state.errexit = false;
    signal(SIGINT, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    dup2(output_pipe[1], STDOUT_FILENO);
    close(output_pipe[0]); close(output_pipe[1]);
    execute_line(command);
    std::cout.flush(); std::cerr.flush();
    run_exit_trap();
    _exit(last_status() & 255);
  }
  close(output_pipe[1]);
  std::string result;
  char buffer[4096];
  ssize_t n;
  constexpr size_t output_cap = 8 * 1024 * 1024;
  while ((n = read(output_pipe[0], buffer, sizeof(buffer))) > 0) {
    size_t room = result.size() < output_cap ? output_cap - result.size() : 0;
    result.append(buffer, std::min(room, static_cast<size_t>(n)));
  }
  close(output_pipe[0]);
  int status = 1;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  substitution_status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  substitution_ran = true;
  while (!result.empty() && result.back() == '\n') result.pop_back();
  return result;
}

static std::string run_process_substitution(const std::string &command,
                                            char direction) {
  int descriptors[2];
  if (pipe(descriptors) != 0) {
    std::perror("dax: process substitution pipe");
    expansion_error = true;
    return {};
  }
  const pid_t child = fork();
  if (child < 0) {
    std::perror("dax: process substitution fork");
    close(descriptors[0]); close(descriptors[1]);
    expansion_error = true;
    return {};
  }
  if (child == 0) {
    state.err_trap_suppressed = state.err_trap_suppressed || !state.errtrace;
    enter_subshell_environment();
    signal(SIGINT, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    for (const auto &[fd, pid] : process_substitutions) { (void)pid; close(fd); }
    const int source = direction == '<' ? descriptors[1] : descriptors[0];
    const int destination = direction == '<' ? STDOUT_FILENO : STDIN_FILENO;
    if (dup2(source, destination) < 0) _exit(125);
    close(descriptors[0]); close(descriptors[1]);
    execute_line(command);
    std::cout.flush(); std::cerr.flush();
    run_exit_trap();
    _exit(last_status() & 255);
  }
  state.last_background_pid = static_cast<int>(child);
  if (direction == '<') {
    close(descriptors[1]);
    process_substitutions.emplace_back(descriptors[0], child);
    return "/dev/fd/" + std::to_string(descriptors[0]);
  }
  close(descriptors[0]);
  process_substitutions.emplace_back(descriptors[1], child);
  return "/dev/fd/" + std::to_string(descriptors[1]);
}

size_t process_substitution_mark() { return process_substitutions.size(); }

void finish_process_substitutions(size_t mark) {
  if (mark > process_substitutions.size()) mark = process_substitutions.size();
  for (size_t i = mark; i < process_substitutions.size(); ++i) close(process_substitutions[i].first);
  for (size_t i = mark; i < process_substitutions.size(); ++i) {
    const auto &[fd, pid] = process_substitutions[i];
    (void)fd;
    int status = 0;
    pid_t result;
    do {
      result = waitpid(pid, &status, WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result == pid) {
      state.completed_child_statuses[static_cast<int>(pid)] = status;
    } else if (result == 0) {
      state.process_substitution_pids.insert(static_cast<int>(pid));
    } else {
      state.completed_child_statuses[static_cast<int>(pid)] = 127 << 8;
    }
  }
  process_substitutions.resize(mark);
}

void reap_completed_process_substitutions() {
  for (auto it = state.process_substitution_pids.begin();
       it != state.process_substitution_pids.end();) {
    int status = 0;
    pid_t result;
    do {
      result = waitpid(static_cast<pid_t>(*it), &status, WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result == *it) {
      state.completed_child_statuses[*it] = status;
      it = state.process_substitution_pids.erase(it);
    } else if (result < 0 && errno == ECHILD) {
      state.completed_child_statuses[*it] = 127 << 8;
      it = state.process_substitution_pids.erase(it);
    } else {
      ++it;
    }
  }
}

static size_t command_substitution_end(const std::string &word, size_t dollar) {
  size_t cursor = dollar + 2;
  int depth = 1;
  char quote = 0;
  for (; cursor < word.size(); ++cursor) {
    const char ch = word[cursor];
    if (quote) {
      if (ch == '\\' && quote == '"' && cursor + 1 < word.size()) { ++cursor; continue; }
      if (ch == quote) quote = 0;
      continue;
    }
    if (ch == '\'' || ch == '"') { quote = ch; continue; }
    if (ch == '$' && cursor + 1 < word.size() && word[cursor + 1] == '(') {
      ++depth; ++cursor; continue;
    }
    if (ch == '(') { ++depth; continue; }
    if (ch == ')' && --depth == 0) return cursor;
  }
  return std::string::npos;
}

std::vector<std::string> brace_expand_word(const std::string &word, int depth = 0) {
  if (depth > 32) return {word};
  bool single = false, quoted = false, conditional = false;
  bool substitution = false;
  int parameter_depth = 0;
  for (size_t open = 0; open < word.size(); ++open) {
    const char ch = word[open];
    if (ch == '\x0b' && open + 1 < word.size()) { ++open; continue; }
    if (ch == '\x01') { single = !single; continue; }
    if (ch == '\x02') { quoted = !quoted; continue; }
    if (ch == '\x09') { conditional = !conditional; continue; }
    if (ch == '\x03') { substitution = !substitution; continue; }
    if (!single && !quoted && !substitution && ch == '$' && open + 1 < word.size() && word[open + 1] == '{') {
      ++parameter_depth;
      ++open;
      continue;
    }
    if (!single && !quoted && parameter_depth > 0 && ch == '}') { --parameter_depth; continue; }
    if (single || quoted || conditional || substitution || parameter_depth > 0 || ch != '{' ||
        (open > 0 && word[open - 1] == '$')) continue;

    bool inner_single = false, inner_quoted = false, inner_conditional = false;
    bool inner_substitution = false;
    int nesting = 0;
    size_t close = std::string::npos;
    std::vector<size_t> commas;
    for (size_t i = open + 1; i < word.size(); ++i) {
      const char current = word[i];
      if (current == '\x0b' && i + 1 < word.size()) { ++i; continue; }
      if (current == '\x01') { inner_single = !inner_single; continue; }
      if (current == '\x02') { inner_quoted = !inner_quoted; continue; }
      if (current == '\x09') { inner_conditional = !inner_conditional; continue; }
      if (current == '\x03') { inner_substitution = !inner_substitution; continue; }
      if (inner_single || inner_quoted || inner_conditional || inner_substitution) continue;
      if (current == '{') { ++nesting; continue; }
      if (current == '}') {
        if (nesting == 0) { close = i; break; }
        --nesting;
      } else if (current == ',' && nesting == 0) commas.push_back(i);
    }
    if (close == std::string::npos) continue;

    std::vector<std::string> alternatives;
    if (!commas.empty()) {
      size_t begin = open + 1;
      for (size_t comma : commas) {
        alternatives.push_back(word.substr(begin, comma - begin));
        begin = comma + 1;
      }
      alternatives.push_back(word.substr(begin, close - begin));
    } else {
      const std::string range = word.substr(open + 1, close - open - 1);
      const size_t dots = range.find("..");
      if (dots == std::string::npos) continue;
      const size_t second_dots = range.find("..", dots + 2);
      const std::string first = range.substr(0, dots);
      const std::string last = range.substr(dots + 2,
          second_dots == std::string::npos ? std::string::npos : second_dots - dots - 2);
      const std::string step_text = second_dots == std::string::npos ? "" : range.substr(second_dots + 2);
      long step = 1;
      if (!step_text.empty()) {
        char *end = nullptr;
        step = std::strtol(step_text.c_str(), &end, 10);
        if (!end || *end) continue;
        if (step == 0) step = 1;
      }
      if (first.size() == 1 && last.size() == 1 &&
          std::isalpha(static_cast<unsigned char>(first[0])) &&
          std::isalpha(static_cast<unsigned char>(last[0]))) {
        const int from = static_cast<unsigned char>(first[0]);
        const int to = static_cast<unsigned char>(last[0]);
        if (step == LONG_MIN) continue;
        step = from <= to ? std::labs(step) : -std::labs(step);
        for (long value = from; (step > 0 ? value <= to : value >= to); value += step) {
          alternatives.emplace_back(1, static_cast<char>(value));
          if (alternatives.size() > 10000) return {word};
        }
      } else {
        char *first_end = nullptr, *last_end = nullptr;
        const long from = std::strtol(first.c_str(), &first_end, 10);
        const long to = std::strtol(last.c_str(), &last_end, 10);
        if (first.empty() || last.empty() || !first_end || *first_end || !last_end || *last_end) continue;
        if (step == LONG_MIN) continue;
        step = from <= to ? std::labs(step) : -std::labs(step);
        const size_t width = std::max(first[0] == '-' ? first.size() - 1 : first.size(),
                                      last[0] == '-' ? last.size() - 1 : last.size());
        for (long value = from; (step > 0 ? value <= to : value >= to); value += step) {
          const bool negative = value < 0;
          std::string digits = std::to_string(negative ? -value : value);
          if (digits.size() < width) digits.insert(0, width - digits.size(), '0');
          alternatives.push_back((negative ? "-" : "") + digits);
          if (alternatives.size() > 10000 ||
              (step > 0 && value > LONG_MAX - step) || (step < 0 && value < LONG_MIN - step)) return {word};
        }
      }
      if (alternatives.empty()) continue;
    }

    std::vector<std::string> result;
    const std::string prefix = word.substr(0, open);
    const std::string suffix = word.substr(close + 1);
    for (const auto &alternative : alternatives) {
      const std::string candidate = prefix + alternative + suffix;
      if (candidate.empty()) continue;
      auto expanded = brace_expand_word(candidate, depth + 1);
      result.insert(result.end(), expanded.begin(), expanded.end());
      if (result.size() > 10000) return {word};
    }
    return result;
  }
  return {word};
}

static bool contains_extglob_operator(const std::string &pattern) {
  for (size_t i = 0; i + 1 < pattern.size(); ++i) {
    if (pattern[i] == '\\') { ++i; continue; }
    if (std::string("@?*+!").find(pattern[i]) != std::string::npos && pattern[i + 1] == '(') return true;
  }
  return false;
}

static std::string glob_ascii_ranges(const std::string &pattern) {
  if (!shell_state().globasciiranges) return pattern;
  std::string output;
  for (size_t open = 0; open < pattern.size();) {
    if (pattern[open] != '[') {
      output += pattern[open++];
      continue;
    }
    size_t close = std::string::npos;
    size_t cursor = open + 1;
    if (cursor < pattern.size() &&
        (pattern[cursor] == '!' || pattern[cursor] == '^')) ++cursor;
    if (cursor < pattern.size() && pattern[cursor] == ']') ++cursor;
    bool escaped = false;
    for (; cursor < pattern.size(); ++cursor) {
      const char ch = pattern[cursor];
      if (escaped) { escaped = false; continue; }
      if (ch == '\\') { escaped = true; continue; }
      if (ch == '[' && cursor + 1 < pattern.size() &&
          (pattern[cursor + 1] == ':' || pattern[cursor + 1] == '=' ||
           pattern[cursor + 1] == '.')) {
        const char marker = pattern[cursor + 1];
        const std::string terminator = std::string(1, marker) + "]";
        const size_t nested_end = pattern.find(terminator, cursor + 2);
        if (nested_end != std::string::npos) {
          cursor = nested_end + 1;
          continue;
        }
      }
      if (ch == ']') { close = cursor; break; }
    }
    if (close == std::string::npos) {
      output += pattern[open++];
      continue;
    }
    const std::string body = pattern.substr(open + 1, close - open - 1);
    output += '[';
    size_t index = 0;
    if (!body.empty() && (body[0] == '!' || body[0] == '^'))
      output += body[index++];
    if (index < body.size() && body[index] == ']') output += body[index++];
    for (; index < body.size();) {
      if (body[index] == '\\' && index + 1 < body.size()) {
        output += body[index++];
        output += body[index++];
        continue;
      }
      if (body[index] == '[' && index + 1 < body.size() &&
          (body[index + 1] == ':' || body[index + 1] == '=' ||
           body[index + 1] == '.')) {
        const char marker = body[index + 1];
        const std::string terminator = std::string(1, marker) + "]";
        const size_t nested_end = body.find(terminator, index + 2);
        if (nested_end != std::string::npos) {
          output.append(body, index, nested_end + 2 - index);
          index = nested_end + 2;
          continue;
        }
      }
      const auto first = static_cast<unsigned char>(body[index]);
      if (index + 2 < body.size() && body[index + 1] == '-' &&
          first < 0x80 &&
          static_cast<unsigned char>(body[index + 2]) < 0x80 &&
          first <= static_cast<unsigned char>(body[index + 2])) {
        const unsigned char last = static_cast<unsigned char>(body[index + 2]);
        for (unsigned value = first; value <= last; ++value) {
          const char member = static_cast<char>(value);
          if (member == ']' || member == '-' || member == '\\') output += '\\';
          output += member;
        }
        index += 3;
        continue;
      }
      output += body[index++];
    }
    output += ']';
    open = close + 1;
  }
  return output;
}

static std::string active_globignore() {
  const auto &state = shell_state();
  if (state.unset_vars.count("GLOBIGNORE")) return {};
  const auto variable = state.vars.find("GLOBIGNORE");
  if (variable != state.vars.end()) return variable->second;
  if (state.ignore_inherited_environment) return {};
  const char *environment_value = std::getenv("GLOBIGNORE");
  return environment_value ? environment_value : std::string();
}

static bool globignore_has_patterns(const std::string &ignore_patterns) {
  return ignore_patterns.find_first_not_of(':') != std::string::npos;
}

static bool globignore_matches(const std::string &path,
                               const std::string &ignore_patterns) {
  const int flags = FNM_PATHNAME |
                    (shell_state().extglob ? FNM_EXTMATCH : 0) |
                    (shell_state().nocaseglob ? FNM_CASEFOLD : 0);
  size_t begin = 0;
  while (begin <= ignore_patterns.size()) {
    const size_t end = ignore_patterns.find(':', begin);
    const std::string pattern = ignore_patterns.substr(
        begin, end == std::string::npos ? std::string::npos : end - begin);
    const std::string normalized_pattern = glob_ascii_ranges(pattern);
    if (!normalized_pattern.empty() && fnmatch(normalized_pattern.c_str(), path.c_str(), flags) == 0)
      return true;
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return false;
}

enum class GlobSortKey { Name, Numeric, Size, Mtime, Atime, Ctime, Blocks };
struct GlobSortConfiguration {
  GlobSortKey key = GlobSortKey::Name;
  bool reverse = false;
  bool enabled = true;
};

static GlobSortConfiguration glob_sort_configuration() {
  GlobSortConfiguration result;
  const auto &state = shell_state();
  std::optional<std::string> value;
  if (!state.unset_vars.count("GLOBSORT")) {
    const auto variable = state.vars.find("GLOBSORT");
    if (variable != state.vars.end()) value = variable->second;
    else if (const char *environment = std::getenv("GLOBSORT")) value = environment;
  }
  if (!value || value->empty()) return result;
  size_t begin = 0;
  if ((*value)[begin] == '-') { result.reverse = true; ++begin; }
  else if ((*value)[begin] == '+') ++begin;
  const std::string specifier = value->substr(begin);
  if (specifier == "nosort") { result.enabled = false; return result; }
  if (specifier == "name") result.key = GlobSortKey::Name;
  else if (specifier == "numeric") result.key = GlobSortKey::Numeric;
  else if (specifier == "size") result.key = GlobSortKey::Size;
  else if (specifier == "mtime") result.key = GlobSortKey::Mtime;
  else if (specifier == "atime") result.key = GlobSortKey::Atime;
  else if (specifier == "ctime") result.key = GlobSortKey::Ctime;
  else if (specifier == "blocks") result.key = GlobSortKey::Blocks;
  else result = GlobSortConfiguration{};
  return result;
}

static int compare_glob_names(const std::string &left,
                              const std::string &right, bool numeric) {
  if (!numeric) {
    const int comparison = std::strcoll(left.c_str(), right.c_str());
    return comparison < 0 ? -1 : comparison > 0 ? 1 : 0;
  }
  size_t li = 0, ri = 0;
  while (li < left.size() && ri < right.size()) {
    const bool left_digit = std::isdigit(static_cast<unsigned char>(left[li]));
    const bool right_digit = std::isdigit(static_cast<unsigned char>(right[ri]));
    if (left_digit && right_digit) {
      size_t lend = li, rend = ri;
      while (lend < left.size() && std::isdigit(static_cast<unsigned char>(left[lend]))) ++lend;
      while (rend < right.size() && std::isdigit(static_cast<unsigned char>(right[rend]))) ++rend;
      size_t lsignificant = li, rsignificant = ri;
      while (lsignificant < lend && left[lsignificant] == '0') ++lsignificant;
      while (rsignificant < rend && right[rsignificant] == '0') ++rsignificant;
      const size_t ldigits = lend - lsignificant, rdigits = rend - rsignificant;
      if (ldigits != rdigits) return ldigits < rdigits ? -1 : 1;
      const int digits = left.compare(lsignificant, ldigits, right, rsignificant, rdigits);
      if (digits != 0) return digits < 0 ? -1 : 1;
      li = lend;
      ri = rend;
      continue;
    }
    const unsigned char lc = static_cast<unsigned char>(left[li]);
    const unsigned char rc = static_cast<unsigned char>(right[ri]);
    if (lc != rc) return lc < rc ? -1 : 1;
    ++li;
    ++ri;
  }
  if (li == left.size() && ri == right.size()) return 0;
  return li == left.size() ? -1 : 1;
}

static int compare_glob_timespec(const timespec &left, const timespec &right) {
  if (left.tv_sec != right.tv_sec) return left.tv_sec < right.tv_sec ? -1 : 1;
  if (left.tv_nsec != right.tv_nsec) return left.tv_nsec < right.tv_nsec ? -1 : 1;
  return 0;
}

static void sort_glob_matches(std::vector<std::string> &matches) {
  const GlobSortConfiguration config = glob_sort_configuration();
  if (!config.enabled || matches.size() < 2) return;
  auto compare = [&](const std::string &left, const std::string &right) {
    int order = 0;
    struct stat left_info {}, right_info {};
    if (config.key != GlobSortKey::Name && config.key != GlobSortKey::Numeric) {
      const bool have_left = stat(left.c_str(), &left_info) == 0;
      const bool have_right = stat(right.c_str(), &right_info) == 0;
      if (have_left != have_right) order = have_left ? 1 : -1;
      else if (have_left) {
        if (config.key == GlobSortKey::Size && left_info.st_size != right_info.st_size)
          order = left_info.st_size < right_info.st_size ? -1 : 1;
        else if (config.key == GlobSortKey::Blocks && left_info.st_blocks != right_info.st_blocks)
          order = left_info.st_blocks < right_info.st_blocks ? -1 : 1;
        else if (config.key == GlobSortKey::Mtime)
          order = compare_glob_timespec(left_info.st_mtim, right_info.st_mtim);
        else if (config.key == GlobSortKey::Atime)
          order = compare_glob_timespec(left_info.st_atim, right_info.st_atim);
        else if (config.key == GlobSortKey::Ctime)
          order = compare_glob_timespec(left_info.st_ctim, right_info.st_ctim);
      }
    }
    if (order == 0)
      order = compare_glob_names(left, right, config.key == GlobSortKey::Numeric);
    if (config.reverse) order = -order;
    return order < 0;
  };
  std::stable_sort(matches.begin(), matches.end(), compare);
}

static std::vector<std::string> extglob_path_expand(const std::string &pattern) {
  const bool include_dotfiles = shell_state().dotglob ||
                                globignore_has_patterns(active_globignore());
  const bool absolute = !pattern.empty() && pattern.front() == '/';
  const bool trailing_slash = pattern.size() > 1 && pattern.back() == '/';
  std::vector<std::string> components;
  std::string component;
  int extglob_depth = 0;
  bool escaped = false, extglob_open_pending = false;
  for (size_t i = absolute ? 1 : 0; i < pattern.size(); ++i) {
    const char ch = pattern[i];
    if (escaped) { component += ch; escaped = false; continue; }
    if (ch == '\\') { component += ch; escaped = true; continue; }
    if (extglob_depth == 0 && std::string("@?*+!").find(ch) != std::string::npos &&
        i + 1 < pattern.size() && pattern[i + 1] == '(') {
      extglob_open_pending = true; component += ch; continue;
    }
    if (extglob_open_pending && ch == '(') { extglob_depth = 1; extglob_open_pending = false; component += ch; continue; }
    if (extglob_depth > 0 && ch == '(') { ++extglob_depth; component += ch; continue; }
    if (extglob_depth > 0 && ch == ')') { --extglob_depth; component += ch; continue; }
    if (ch == '/' && extglob_depth == 0) {
      components.push_back(component); component.clear();
    } else component += ch;
  }
  if (!component.empty() || components.empty()) components.push_back(component);
  if (trailing_slash && !components.empty() && components.back().empty()) components.pop_back();
  std::vector<std::string> paths{absolute ? "/" : ""};
  for (size_t index = 0; index < components.size(); ++index) {
    const std::string &part = components[index];
    const bool last = index + 1 == components.size();
    if (shell_state().globstar && part == "**") {
      std::vector<std::string> next;
      for (const auto &base : paths) {
        if (!last) next.push_back(base);
        std::function<void(const std::string &, unsigned)> walk = [&](const std::string &directory, unsigned depth) {
          if (depth > 64) return;
          DIR *dir = opendir(directory.empty() ? "." : directory.c_str());
          if (!dir) return;
          while (dirent *entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (name == "." || name == ".." || (!include_dotfiles && !name.empty() && name[0] == '.')) continue;
            const std::string candidate = directory.empty() ? name : directory == "/" ? directory + name : directory + "/" + name;
            struct stat info {};
            if (stat(candidate.c_str(), &info) != 0) continue;
            const bool is_directory = S_ISDIR(info.st_mode);
            if (last) {
              if (!trailing_slash || is_directory) next.push_back(candidate + (trailing_slash && candidate.back() != '/' ? "/" : ""));
            } else if (is_directory) next.push_back(candidate);
            struct stat link_info {};
            if (is_directory && lstat(candidate.c_str(), &link_info) == 0 && !S_ISLNK(link_info.st_mode))
              walk(candidate, depth + 1);
          }
          closedir(dir);
        };
        walk(base, 0);
      }
      paths = std::move(next);
      if (paths.empty()) break;
      continue;
    }
    const bool pattern_part = part.find_first_of("*?[") != std::string::npos ||
        (shell_state().extglob && contains_extglob_operator(part));
    std::vector<std::string> next;
    for (const auto &base : paths) {
    if (!pattern_part) {
        const std::string candidate = base.empty() ? part : base == "/" ? base + part : base + "/" + part;
        struct stat info {};
        if (stat(candidate.c_str(), &info) == 0 && (!(!last || trailing_slash) || S_ISDIR(info.st_mode)))
          next.push_back(candidate);
        continue;
      }
      const std::string directory = base.empty() ? "." : base;
      DIR *dir = opendir(directory.c_str());
      if (!dir) continue;
      const bool explicit_dot = include_dotfiles || (!part.empty() && part.front() == '.');
      const std::string match_part = glob_ascii_ranges(part);
      const int flags = (shell_state().extglob ? FNM_EXTMATCH : 0) |
                        (shell_state().nocaseglob ? FNM_CASEFOLD : 0) |
                        (include_dotfiles ? 0 : FNM_PERIOD);
      while (dirent *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if ((shell_state().globskipdots && (name == "." || name == "..")) ||
            (!explicit_dot && !name.empty() && name.front() == '.')) continue;
        if (fnmatch(match_part.c_str(), name.c_str(), flags) != 0) continue;
        const std::string candidate = base.empty() ? name : base == "/" ? base + name : base + "/" + name;
        if (!last || trailing_slash) {
          struct stat info {};
          if (stat(candidate.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) continue;
        }
        next.push_back(candidate);
      }
      closedir(dir);
    }
    paths = std::move(next);
    if (paths.empty()) break;
  }
  if (trailing_slash)
    for (auto &path : paths) if (path.empty() || path.back() != '/') path += '/';
  return paths;
}

void expand_words(std::vector<std::string> &words, WordExpansionMode mode) {
  auto find_parameter_close = [](const std::string &word, size_t content_start) {
    size_t depth = 1;
    bool single_quoted = false;
    bool double_quoted = false;
    size_t double_quote_depth = 0;
    for (size_t cursor = content_start; cursor < word.size(); ++cursor) {
      const char ch = word[cursor];
      if (ch == '\x0b' && cursor + 1 < word.size()) {
        ++cursor;
        continue;
      }
      if (ch == '\x03') {
        const size_t close = word.find('\x03', cursor + 1);
        if (close == std::string::npos) return std::string::npos;
        cursor = close;
        continue;
      }
      if (ch == '\x01' && !double_quoted) {
        single_quoted = !single_quoted;
        continue;
      }
      if (ch == '\x02' && !single_quoted) {
        double_quoted = !double_quoted;
        if (double_quoted) double_quote_depth = depth;
        continue;
      }
      if (single_quoted) continue;
      if (ch == '$' && cursor + 1 < word.size() && word[cursor + 1] == '{') {
        ++depth;
        ++cursor;
      } else if (ch == '}' && (!double_quoted || depth > double_quote_depth)) {
        if (--depth == 0) return cursor;
      }
    }
    return std::string::npos;
  };
  auto current_option_flags = []() {
    std::string flags;
    const std::string order = "abefhkimnpuvxtPCEBHTcs";
    for (char flag : order) {
      const bool enabled =
          (flag == 'a' && state.allexport) ||
          (flag == 'b' && state.notify_jobs) ||
          (flag == 'e' && state.errexit) ||
          (flag == 'E' && state.errtrace) ||
          (flag == 'T' && state.functrace) ||
          (flag == 'C' && state.noclobber) ||
          (flag == 'f' && state.noglob) ||
          (flag == 'h' && state.hashall) ||
          (flag == 'i' && state.interactive_shell) ||
          (flag == 'm' && state.monitor) ||
          (flag == 'k' && state.keyword_assignments) ||
          (flag == 'P' && state.physical_directory) ||
          (flag == 'p' && state.privileged) ||
          (flag == 't' && state.one_command) ||
          (flag == 'n' && state.noexec) ||
          (flag == 'u' && state.nounset) ||
          (flag == 'v' && state.verbose) ||
          (flag == 'x' && state.xtrace) ||
          (flag == 'B' && state.braceexpand) ||
          (flag == 'H' && state.histexpand) ||
          (flag == 'c' && state.command_string_mode) ||
          (flag == 's' && state.stdin_mode);
      if (enabled) flags += flag;
    }
    return flags;
  };
  std::vector<std::string> brace_expanded;
  for (const auto &word : words) {
    const bool brace_expansion = mode == WordExpansionMode::Normal ||
                                 mode == WordExpansionMode::Redirection ||
                                 mode == WordExpansionMode::InteractiveRedirection;
    auto expanded = shell_state().braceexpand && brace_expansion
        ? brace_expand_word(word) : std::vector<std::string>{word};
    brace_expanded.insert(brace_expanded.end(), expanded.begin(), expanded.end());
  }
  words = std::move(brace_expanded);
  std::vector<std::string> expanded_words;
  for (size_t word_index = 0; word_index < words.size(); ++word_index) {
    const auto &w = words[word_index];
    size_t assignment_equal = std::string::npos;
    for (size_t j = 0; j < w.size(); ++j) {
      if (w[j] == '=') { assignment_equal = j; break; }
      if (!(std::isalnum(static_cast<unsigned char>(w[j])) || w[j] == '_')) break;
    }
    const bool assignment_word = assignment_equal != std::string::npos;
    auto fail_expansion = [&]() {
      if (!expansion_error) expansion_error_index = word_index;
      expansion_error = true;
    };
    auto fail_unset_parameter = [&](const std::string &name) {
      std::cerr << "dax: " << name << ": unbound variable\n";
      expansion_error_reported = true;
      fail_expansion();
      if (!shell_state().interactive_shell) {
        shell_state().exit_value = 1;
        shell_state().exiting = true;
      }
    };
    std::vector<std::string> fields(1), patterns(1);
    std::vector<bool> has_pattern(1, false);
    const bool single_word_context = mode != WordExpansionMode::Normal;
    bool literal = false, double_quoted = false, conditional_context = single_word_context;
    bool pending_split = false;
    bool last_ifs_nonwhite = false, last_ifs_whitespace = false;
    auto append_text = [&](const std::string &text, bool split) {
      if (shell_state().extglob && !assignment_word && !literal && !double_quoted && !conditional_context) {
        for (size_t j = 0; j + 1 < text.size(); ++j)
          if (std::string("@?*+!").find(text[j]) != std::string::npos && text[j + 1] == '(')
            has_pattern.back() = true;
      }
      auto ifs_it = vars.find("IFS");
      const char *environment_ifs = std::getenv("IFS");
      const std::string ifs = ifs_it != vars.end() ? ifs_it->second
                              : environment_ifs ? environment_ifs
                                                : " \t\n";
      for (unsigned char ch : text) {
        if (mode == WordExpansionMode::ParameterReplacement && ch == '&') {
          // Preserve quote state until patsub_replacement handles the expanded
          // replacement text. Bash substitutes only unquoted ampersands.
          const char marker = literal || double_quoted ? '\x0d' : '\x0c';
          fields.back() += marker;
          patterns.back() += marker;
          pending_split = false;
          last_ifs_nonwhite = false;
          last_ifs_whitespace = false;
          continue;
        }
        if (mode == WordExpansionMode::ParameterReplacement &&
            (ch == '\x0b' || ch == '\x0c' || ch == '\x0d')) {
          fields.back() += '\x0b';
          fields.back() += static_cast<char>(ch);
          patterns.back() += '\x0b';
          patterns.back() += static_cast<char>(ch);
          pending_split = false;
          last_ifs_nonwhite = false;
          last_ifs_whitespace = false;
          continue;
        }
        const bool ifs_delimiter = split && !assignment_word && !conditional_context &&
                                   ifs.find(static_cast<char>(ch)) != std::string::npos;
        if (ifs_delimiter) {
          const bool ifs_whitespace = ch == ' ' || ch == '\t' || ch == '\n';
          if (ifs_whitespace) {
            if (!fields.back().empty()) {
              fields.emplace_back();
              patterns.emplace_back();
              has_pattern.push_back(false);
            }
            last_ifs_whitespace = true;
          } else if (!(last_ifs_nonwhite && last_ifs_whitespace)) {
            fields.emplace_back();
            patterns.emplace_back();
            has_pattern.push_back(false);
            last_ifs_nonwhite = true;
            last_ifs_whitespace = false;
          }
          if (!ifs_whitespace) last_ifs_nonwhite = true;
          pending_split = true;
          continue;
        }
        pending_split = false;
        last_ifs_nonwhite = false;
        last_ifs_whitespace = false;
        fields.back() += static_cast<char>(ch);
        const bool meta = ch == '*' || ch == '?' || ch == '[';
        if (meta && !assignment_word && !conditional_context && !literal && !double_quoted)
          has_pattern.back() = true;
        if (meta && (literal || double_quoted)) patterns.back() += '\\';
        patterns.back() += static_cast<char>(ch);
      }
    };
    auto home_expand = [&](size_t start, size_t finish) -> std::optional<std::string> {
      std::string user = w.substr(start + 1, finish - start - 1);
      if (user.empty()) {
        auto home = vars.find("HOME");
        const char *env_home = std::getenv("HOME");
        if (home != vars.end()) return home->second;
        if (env_home) return std::string(env_home);
        return {};
      }
      struct passwd *entry = getpwnam(user.c_str());
      if (entry && entry->pw_dir) return std::string(entry->pw_dir);
      return {};
    };
    for (size_t i = 0; i < w.size(); ++i) {
      const size_t expansion_start = i;
      auto is_standalone_parameter = [&](size_t end) {
        for (size_t j = 0; j < expansion_start; ++j)
          if (w[j] != '\x02') return false;
        for (size_t j = end + 1; j < w.size(); ++j)
          if (w[j] != '\x02') return false;
        return true;
      };
      if (w[i] == '\x0b' && i + 1 < w.size()) {
        const char ch = w[++i];
        const bool escaped_replacement_marker =
            mode == WordExpansionMode::ParameterReplacement &&
            (ch == '&' || ch == '\x0b' || ch == '\x0c' || ch == '\x0d');
        fields.back() += escaped_replacement_marker ? '\x0b' : ch;
        if (escaped_replacement_marker) fields.back() += ch;
        if (ch == '*' || ch == '?' || ch == '[') patterns.back() += '\\';
        patterns.back() += escaped_replacement_marker ? '\x0b' : ch;
        if (escaped_replacement_marker) patterns.back() += ch;
        pending_split = false;
        last_ifs_nonwhite = false;
        last_ifs_whitespace = false;
        continue;
      }
      if (w[i] == '\x09') { conditional_context = !conditional_context; continue; }
      if (w[i] == '\x01') { literal = !literal; continue; }
      if (w[i] == '\x02') { double_quoted = !double_quoted; continue; }
      if ((literal || double_quoted) && std::string("@+!").find(w[i]) != std::string::npos &&
          i + 1 < w.size() && w[i + 1] == '(') {
        fields.back() += w[i]; fields.back() += w[i + 1];
        patterns.back() += '\\'; patterns.back() += w[i]; patterns.back() += w[i + 1];
        ++i;
        pending_split = false;
        last_ifs_nonwhite = false;
        last_ifs_whitespace = false;
        continue;
      }
      if (!literal && !double_quoted && std::string("@?*+!").find(w[i]) != std::string::npos &&
          i + 1 < w.size() && w[i + 1] == '(') {
        if (!shell_state().extglob && !conditional_context) { fail_expansion(); break; }
        if (!conditional_context) has_pattern.back() = true;
        append_text(w.substr(i, 2), false);
        ++i;
        continue;
      }
      if (!literal && !double_quoted && w[i] == '~' &&
          (i == 0 || (assignment_equal != std::string::npos &&
           (i == assignment_equal + 1 || (i > assignment_equal && w[i - 1] == ':'))))) {
        size_t end = i + 1;
        while (end < w.size() && w[end] != '/' && w[end] != ':') ++end;
        auto home = home_expand(i, end);
        if (home) { append_text(*home, false); i = end - 1; continue; }
      }
      if (w[i] == '\x08' && i + 2 < w.size() && w[i + 2] == '\x03') {
        const size_t end = w.find('\x03', i + 3);
        if (end == std::string::npos) { fail_expansion(); break; }
        const std::string path = run_process_substitution(w.substr(i + 3, end - i - 3), w[i + 1]);
        if (path.empty()) { fail_expansion(); break; }
        append_text(path, false);
        i = end;
        continue;
      }
      if (w[i] == '\x07' && i + 1 < w.size() && w[i + 1] == '\x03') {
        size_t end = w.find('\x03', i + 2);
        if (end == std::string::npos) { fail_expansion(); break; }
        std::string expr = w.substr(i + 2, end - i - 2);
        if (expr.size() >= 2 && expr.front() == '(' && expr.back() == ')') expr = expr.substr(1, expr.size() - 2);
        std::string arithmetic_error;
        auto value = evaluate_arithmetic(expr, &arithmetic_error);
        if (!value) { fail_expansion(); break; }
        append_text(std::to_string(*value), !double_quoted);
        i = end;
        continue;
      }
      if (w[i] == '\x03') {
        size_t end = w.find('\x03', i + 1);
        if (end == std::string::npos) { fail_expansion(); break; }
        append_text(run_command_substitution(w.substr(i + 1, end - i - 1)), !double_quoted);
        i = end;
        continue;
      }
      if (literal || w[i] != '$') {
        append_text(std::string(1, w[i]), false);
        continue;
      }
      if (i + 1 < w.size() && w[i + 1] == '(') {
        const size_t end = command_substitution_end(w, i);
        if (end == std::string::npos) { fail_expansion(); break; }
        append_text(run_command_substitution(w.substr(i + 2, end - i - 2)), !double_quoted);
        i = end;
        continue;
      }
      if (i + 1 < w.size() && w[i + 1] == '$') {
        append_text(std::to_string(state.shell_pid), !double_quoted); ++i; continue;
      }
      if (i + 1 < w.size() && w[i + 1] == '?') {
        append_text(std::to_string(status_value), !double_quoted); ++i; continue;
      }
      if (i + 1 < w.size() && w[i + 1] == '!') {
        if (state.last_background_pid > 0)
          append_text(std::to_string(state.last_background_pid), !double_quoted);
        else if (nounset) fail_unset_parameter("!");
        ++i; continue;
      }
      if (i + 1 < w.size() && w[i + 1] == '#') {
        append_text(std::to_string(positionals.size()), !double_quoted); ++i; continue;
      }
      if (i + 1 < w.size() && w[i + 1] == '_') {
        append_text(state.last_argument, !double_quoted);
        ++i;
        continue;
      }
      if (i + 1 < w.size() && w[i + 1] == '-') {
        append_text(current_option_flags(), !double_quoted);
        ++i;
        continue;
      }
      if (i + 1 < w.size() && (w[i + 1] == '@' || w[i + 1] == '*')) {
        const char special = w[i + 1];
        if (double_quoted && special == '@') {
          for (size_t j = 0; j < positionals.size(); ++j) {
            if (j) { fields.emplace_back(); patterns.emplace_back(); has_pattern.push_back(false); }
            append_text(positionals[j], false);
          }
          if (positionals.empty() && fields.size() == 1 && fields[0].empty() &&
              is_standalone_parameter(i + 1)) fields.clear();
        } else if (double_quoted) {
          auto ifs = vars.find("IFS");
          const char *env_ifs = std::getenv("IFS");
          const std::string sep = ifs != vars.end() ? ifs->second : env_ifs ? env_ifs : " \t\n";
          std::string value;
          for (size_t j = 0; j < positionals.size(); ++j) { if (j && !sep.empty()) value += sep[0]; value += positionals[j]; }
          append_text(value, false);
        } else {
          for (size_t j = 0; j < positionals.size(); ++j) {
            if (j) { fields.emplace_back(); patterns.emplace_back(); has_pattern.push_back(false); }
            append_text(positionals[j], true);
          }
          if (positionals.empty() && fields.size() == 1 && fields[0].empty() &&
              is_standalone_parameter(i + 1))
            fields.clear();
        }
        ++i; continue;
      }
      size_t b = i + 1, e = b;
      std::string parameter_operator, parameter_word;
      std::string parameter_name;
      bool length_expansion = false;
      bool substring_expansion = false, replace_expansion = false, replace_all = false;
      bool replace_prefix = false, replace_suffix = false;
      long long substring_offset = 0, substring_length = 0;
      bool has_substring_length = false;
      std::string replace_pattern, replace_value;
      char case_transform = 0;
      bool transform_all = false, quote_transform = false;
      char parameter_transform = 0;
      std::string transform_pattern;
      char array_mode = 0;
      size_t array_index = 0;
      bool indexed_array = false;
      bool associative_index = false, associative_keys = false;
      bool indirect_expansion = false, name_expansion = false;
      char name_expansion_mode = 0;
      std::string associative_key;
      if (b < w.size() && w[b] == '{') {
        b++; e = find_parameter_close(w, b);
        if (e == std::string::npos) { append_text("$", false); continue; }
        std::string expression = w.substr(b, e - b);
        const bool hash_is_special_parameter = expression.size() == 1 ||
            (expression.size() > 1 && std::string(":-=+?").find(expression[1]) != std::string::npos);
        if (!expression.empty() && expression[0] == '#' && !hash_is_special_parameter) {
          length_expansion = true;
          expression.erase(expression.begin());
        }
        if (!expression.empty() && expression[0] == '!') {
          const bool special_parameter_operator = expression.size() == 1 ||
              (expression.size() > 1 && std::string(":-=+?").find(expression[1]) != std::string::npos);
          if (!special_parameter_operator) {
            expression.erase(expression.begin());
            const size_t open_bracket = expression.find('[');
            if (open_bracket == std::string::npos && !expression.empty() &&
                (expression.back() == '*' || expression.back() == '@')) {
              name_expansion = true;
              name_expansion_mode = expression.back();
              expression.pop_back();
            } else if (open_bracket == std::string::npos) {
              indirect_expansion = true;
            } else {
              associative_keys = true;
            }
          }
        }
        size_t parameter_name_end = 0;
        while (parameter_name_end < expression.size() &&
               (std::isalnum(static_cast<unsigned char>(expression[parameter_name_end])) ||
                expression[parameter_name_end] == '_'))
          ++parameter_name_end;
        const bool bracket_is_case_pattern = parameter_name_end < expression.size() &&
            (expression[parameter_name_end] == '^' || expression[parameter_name_end] == ',');
        const size_t bracket = expression.find('[');
        if (bracket != std::string::npos && !bracket_is_case_pattern) {
          size_t closing_bracket = std::string::npos;
          size_t nested_brackets = 0;
          std::vector<std::pair<char, size_t>> nested_expansions;
          bool single_quoted_subscript = false;
          bool double_quoted_subscript = false;
          for (size_t cursor = bracket + 1; cursor < expression.size(); ++cursor) {
            const char ch = expression[cursor];
            if (ch == '\x0b' && cursor + 1 < expression.size()) {
              ++cursor;
              continue;
            }
            if (ch == '\x01') {
              single_quoted_subscript = !single_quoted_subscript;
              continue;
            }
            if (ch == '\x02' && !single_quoted_subscript) {
              double_quoted_subscript = !double_quoted_subscript;
              continue;
            }
            if (single_quoted_subscript || double_quoted_subscript) continue;
            if (ch == '$' && cursor + 1 < expression.size()) {
              if (expression[cursor + 1] == '{') {
                nested_expansions.emplace_back('b', 0);
                ++cursor;
                continue;
              }
              if (expression[cursor + 1] == '(') {
                if (cursor + 2 < expression.size() && expression[cursor + 2] == '(') {
                  nested_expansions.emplace_back('a', 2);
                  cursor += 2;
                } else {
                  nested_expansions.emplace_back('c', 1);
                  ++cursor;
                }
                continue;
              }
            }
            if (!nested_expansions.empty() && nested_expansions.back().first == 'b' && ch == '}') {
              nested_expansions.pop_back();
              continue;
            }
            if (!nested_expansions.empty() &&
                (nested_expansions.back().first == 'a' || nested_expansions.back().first == 'c')) {
              auto &expansion = nested_expansions.back();
              if (ch == '(') ++expansion.second;
              else if (ch == ')' && --expansion.second == 0) {
                nested_expansions.pop_back();
                continue;
              }
            }
            const bool arithmetic_substitution = !nested_expansions.empty() &&
                nested_expansions.back().first == 'a';
            if (nested_expansions.empty() || arithmetic_substitution) {
              if (ch == '[') ++nested_brackets;
              else if (ch == ']') {
                if (nested_brackets == 0) {
                  closing_bracket = cursor;
                  break;
                }
                --nested_brackets;
              }
            }
          }
          if (closing_bracket == std::string::npos) { fail_expansion(); break; }
          const std::string subscript = expression.substr(bracket + 1, closing_bracket - bracket - 1);
          std::string array_name = expression.substr(0, bracket);
          const std::string resolved_array_name = shell_resolve_nameref(array_name);
          if (!resolved_array_name.empty() && resolved_array_name.find('[') == std::string::npos)
            array_name = resolved_array_name;
          const std::string suffix = expression.substr(closing_bracket + 1);
          expression = expression.substr(0, bracket) + suffix;
          if (subscript == "@" || subscript == "*") array_mode = subscript[0];
          else if (shell_state().associative_arrays.count(array_name)) {
            associative_index = true;
            std::vector<std::string> key_word = {std::string("\x09") + subscript + "\x09"};
            expand_words(key_word);
            if (!word_expansion_succeeded()) { fail_expansion(); break; }
            associative_key = key_word.empty() ? std::string() : key_word.front();
            associative_key.erase(std::remove(associative_key.begin(), associative_key.end(), '\x01'), associative_key.end());
            associative_key.erase(std::remove(associative_key.begin(), associative_key.end(), '\x02'), associative_key.end());
          }
          else {
            std::vector<std::string> subscript_word = {
                std::string("\x09") + subscript + "\x09"};
            expand_words(subscript_word);
            if (!word_expansion_succeeded()) {
              fail_expansion();
              break;
            }
            std::string arithmetic_subscript = subscript_word.empty()
                ? std::string() : subscript_word.front();
            arithmetic_subscript.erase(
                std::remove(arithmetic_subscript.begin(), arithmetic_subscript.end(), '\x01'),
                arithmetic_subscript.end());
            arithmetic_subscript.erase(
                std::remove(arithmetic_subscript.begin(), arithmetic_subscript.end(), '\x02'),
                arithmetic_subscript.end());
            std::string arithmetic_error;
            const auto evaluated_index = arithmetic_subscript.empty()
                ? std::optional<long long>{}
                : evaluate_arithmetic(arithmetic_subscript, &arithmetic_error);
            if (!evaluated_index) {
              fail_expansion();
              break;
            }
            long long parsed = *evaluated_index;
            if (parsed < 0) {
              const auto array = shell_state().arrays.find(array_name);
              const size_t extent = shell_array_index_extent(array_name);
              if (parsed < -1000000 || array == shell_state().arrays.end() ||
                  static_cast<unsigned long long>(-parsed) > extent) {
                fail_expansion();
                break;
              }
              parsed += static_cast<long long>(extent);
            }
            if (parsed > 1000000) { fail_expansion(); break; }
            indexed_array = true;
            array_index = static_cast<size_t>(parsed);
          }
        }
        size_t name_end = 0;
        while (name_end < expression.size() &&
               (std::isalnum(static_cast<unsigned char>(expression[name_end])) || expression[name_end] == '_'))
          ++name_end;
        if (name_end > 0 && name_end < expression.size()) {
          const char operation = expression[name_end];
          if (operation == '^' || operation == ',') {
            case_transform = operation;
            transform_all = name_end + 1 < expression.size() && expression[name_end + 1] == operation;
            const size_t pattern_begin = name_end + (transform_all ? 2 : 1);
            transform_pattern = expression.substr(pattern_begin);
            expression.resize(name_end);
          } else if (operation == '@') {
            const bool supported_transform = name_end + 2 == expression.size() &&
                std::string("QEUuLaAPkK").find(expression[name_end + 1]) != std::string::npos;
            if (!supported_transform) {
              std::cerr << "dax: bad substitution\n";
              expansion_error_reported = true;
              fail_expansion();
              break;
            }
            parameter_transform = expression[name_end + 1];
            quote_transform = parameter_transform == 'Q';
            expression.resize(name_end);
          }
        } else if (name_end == 0 && expression.size() > 1 &&
                   (expression[0] == '@' || expression[0] == '*') && expression[1] == '@') {
          const bool supported_transform = expression.size() == 3 &&
              std::string("QEUuLaAPkK").find(expression[2]) != std::string::npos;
          if (!supported_transform) {
            std::cerr << "dax: bad substitution\n";
            expansion_error_reported = true;
            fail_expansion();
            break;
          }
          parameter_transform = expression[2];
          quote_transform = parameter_transform == 'Q';
          expression.resize(1);
        }
        {
          size_t removal_at = expression.find("##");
          if (removal_at != std::string::npos && removal_at > 0) {
            parameter_operator = "##";
            parameter_word = expression.substr(removal_at + 2);
            expression.resize(removal_at);
          } else if ((removal_at = expression.find("%%")) != std::string::npos && removal_at > 0) {
            parameter_operator = "%%";
            parameter_word = expression.substr(removal_at + 2);
            expression.resize(removal_at);
          } else if ((removal_at = expression.find('#')) != std::string::npos && removal_at > 0) {
            parameter_operator = "#";
            parameter_word = expression.substr(removal_at + 1);
            expression.resize(removal_at);
          } else if ((removal_at = expression.find('%')) != std::string::npos && removal_at > 0) {
            parameter_operator = "%";
            parameter_word = expression.substr(removal_at + 1);
            expression.resize(removal_at);
          }
          if (!parameter_operator.empty()) {
            // Pattern-removal operators are handled after reading the variable.
          } else {
          const size_t colon = expression.find(':');
          if (colon != std::string::npos && colon + 1 < expression.size()) {
            std::string spec = expression.substr(colon + 1);
            const size_t second_colon = spec.find(':');
            const bool negative_offset_spelling = !spec.empty() && spec.front() == '-';
            if (second_colon != std::string::npos || !negative_offset_spelling ||
                std::isspace(static_cast<unsigned char>(spec.front()))) {
              while (!spec.empty() && std::isspace(static_cast<unsigned char>(spec.front()))) spec.erase(spec.begin());
              const size_t length_colon = spec.find(':');
              const std::string offset_text = spec.substr(0, length_colon);
              char *offset_end = nullptr;
              errno = 0;
              substring_offset = std::strtoll(offset_text.c_str(), &offset_end, 10);
              if (!errno && offset_end != offset_text.c_str() && !*offset_end) {
                substring_expansion = true;
                if (length_colon != std::string::npos) {
                  const std::string length_text = spec.substr(length_colon + 1);
                  char *length_end = nullptr;
                  errno = 0;
                  substring_length = std::strtoll(length_text.c_str(), &length_end, 10);
                  if (errno || length_end == length_text.c_str() || *length_end)
                    substring_expansion = false;
                  else has_substring_length = true;
                }
                if (substring_expansion) expression.resize(colon);
              }
            }
          }
          if (!substring_expansion) {
            for (const std::string op : {":-", ":=", ":+", ":?"}) {
              size_t at = expression.find(op);
              if (at != std::string::npos && at > 0) {
                parameter_operator = op;
                parameter_word = expression.substr(at + op.size());
                expression.resize(at);
                break;
              }
            }
          }
          if (parameter_operator.empty()) {
            const size_t slash = expression.find('/');
            if (slash != std::string::npos) {
              replace_expansion = true;
              replace_all = slash + 1 < expression.size() && expression[slash + 1] == '/';
              const size_t pattern_begin = slash + (replace_all ? 2 : 1);
              const size_t separator = expression.find('/', pattern_begin);
              replace_pattern = expression.substr(
                  pattern_begin, separator == std::string::npos
                                     ? std::string::npos : separator - pattern_begin);
              if (!replace_pattern.empty() && replace_pattern.front() == '#') {
                replace_prefix = true;
                replace_pattern.erase(replace_pattern.begin());
              } else if (!replace_pattern.empty() && replace_pattern.front() == '%') {
                replace_suffix = true;
                replace_pattern.erase(replace_pattern.begin());
              }
              if (separator != std::string::npos)
                replace_value = expression.substr(separator + 1);
              expression.resize(slash);
            } else {
              const size_t colon = expression.find(':');
              if (colon != std::string::npos && colon + 1 < expression.size()) {
                std::string spec = expression.substr(colon + 1);
                while (!spec.empty() && std::isspace(static_cast<unsigned char>(spec.front()))) spec.erase(spec.begin());
                const size_t second_colon = spec.find(':');
                const std::string offset_text = spec.substr(0, second_colon);
                char *offset_end = nullptr;
                errno = 0;
                substring_offset = std::strtoll(offset_text.c_str(), &offset_end, 10);
                if (!errno && offset_end != offset_text.c_str() && !*offset_end) {
                  substring_expansion = true;
                  if (second_colon != std::string::npos) {
                    const std::string length_text = spec.substr(second_colon + 1);
                    char *length_end = nullptr;
                    errno = 0;
                    substring_length = std::strtoll(length_text.c_str(), &length_end, 10);
                    if (errno || length_end == length_text.c_str() || *length_end)
                      substring_expansion = false;
                    else has_substring_length = true;
                  }
                  if (substring_expansion) expression.resize(colon);
                }
              }
            }
          }
          }
          if (parameter_operator.empty()) {
            for (const std::string op : {"-", "=", "+", "?"}) {
              size_t at = expression.find(op);
              if (at != std::string::npos && at > 0) {
                parameter_operator = op;
                parameter_word = expression.substr(at + op.size());
                expression.resize(at);
                break;
              }
            }
          }
        }
        parameter_name = expression;
        i = e;
      } else if (b < w.size() && std::isdigit((unsigned char)w[b])) {
        e = b + 1; i = e - 1;
      } else {
        while (e < w.size() && (std::isalnum((unsigned char)w[e]) || w[e] == '_')) ++e;
        if (e == b) { append_text("$", false); continue; }
        i = e - 1;
      }
      std::string k = parameter_name.empty() ? w.substr(b, e - b) : parameter_name;
      const std::string original_k = k;
      const bool unbound_nameref = !indirect_expansion && shell_state().namerefs.count(k) &&
                                   shell_resolve_nameref(k).empty();
      std::string nameref_target_name;
      if (!indirect_expansion && !unbound_nameref && shell_state().namerefs.count(k)) {
        nameref_target_name = shell_resolve_nameref(k);
        k = nameref_target_name;
        const size_t target_bracket = k.find('[');
        if (target_bracket != std::string::npos && !k.empty() && k.back() == ']') {
          const std::string target_base = k.substr(0, target_bracket);
          std::string target_index = k.substr(target_bracket + 1, k.size() - target_bracket - 2);
          target_index.erase(std::remove(target_index.begin(), target_index.end(), '\x01'), target_index.end());
          target_index.erase(std::remove(target_index.begin(), target_index.end(), '\x02'), target_index.end());
          if (shell_state().associative_arrays.count(target_base)) {
            associative_index = true;
            associative_key = target_index;
          } else {
            std::string arithmetic_error;
            const auto index = evaluate_arithmetic(target_index, &arithmetic_error);
            if (!index || *index < -1000000 || *index > 1000000) {
              fail_expansion();
              continue;
            }
            long long resolved_index = *index;
            if (resolved_index < 0)
              resolved_index += static_cast<long long>(shell_array_index_extent(target_base));
            if (resolved_index < 0 || resolved_index > 1000000) {
              fail_expansion();
              continue;
            }
            indexed_array = true;
            array_index = static_cast<size_t>(resolved_index);
          }
          k = target_base;
        }
      }
      if (substring_expansion && has_substring_length && substring_length < 0) {
        std::cerr << substring_length << ": substring expression < 0\n";
        expansion_error_reported = true;
        fail_expansion();
        continue;
      }
      std::optional<std::string> special_parameter_value;
      if (k == "SHELLOPTS" || k == "BASHOPTS") {
        const std::string value = k == "SHELLOPTS"
                                      ? shell_enabled_options()
                                      : shell_enabled_shopt_options();
        vars[k] = value;
        special_parameter_value = value;
      }
      if (k == "?") {
        special_parameter_value = std::to_string(status_value);
      } else if (k == "$") {
        special_parameter_value = std::to_string(state.shell_pid);
      } else if (k == "!" && state.last_background_pid > 0) {
        special_parameter_value = std::to_string(state.last_background_pid);
      } else if (k == "#") {
        special_parameter_value = std::to_string(positionals.size());
      } else if (k == "-") {
        special_parameter_value = current_option_flags();
      } else if (k == "_") {
        special_parameter_value = state.last_argument;
      } else if (k == "BASHPID" && !state.disabled_special_parameters.count(k)) {
        special_parameter_value = std::to_string(getpid());
      } else if (k == "BASH_SUBSHELL" &&
                 !state.disabled_special_parameters.count(k)) {
        special_parameter_value = std::to_string(state.subshell_level);
      } else if (k == "BASH_COMMAND" &&
                 !state.disabled_special_parameters.count(k)) {
        special_parameter_value = state.bash_command;
      } else if (k == "BASH_TRAPSIG" && state.trap_signal != 0) {
        special_parameter_value = std::to_string(state.trap_signal);
      } else if (k == "HISTCMD" && state.interactive_shell &&
                 !state.disabled_special_parameters.count(k)) {
        special_parameter_value = std::to_string(History::get_all().size());
      } else if (k == "BASH_EXECUTION_STRING" && state.command_string_mode) {
        special_parameter_value = state.execution_string;
      } else if ((k == "EPOCHSECONDS" || k == "EPOCHREALTIME") &&
                 !state.disabled_special_parameters.count(k)) {
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const long long seconds = micros / 1000000;
        if (k == "EPOCHSECONDS") {
          special_parameter_value = std::to_string(seconds);
        } else {
          const long long fractional = micros % 1000000;
          std::ostringstream value;
          value << seconds << '.' << std::setw(6) << std::setfill('0') << fractional;
          special_parameter_value = value.str();
        }
      } else if (k == "SECONDS" && !state.disabled_special_parameters.count(k)) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - state.seconds_epoch).count();
        special_parameter_value = std::to_string(state.seconds_offset + elapsed);
      } else if (k == "RANDOM" && !state.disabled_special_parameters.count(k)) {
        special_parameter_value = std::to_string(next_bash_random());
        vars[k] = *special_parameter_value;
      } else if (k == "SRANDOM" && !state.disabled_special_parameters.count(k)) {
        special_parameter_value = std::to_string(next_bash_secure_random());
        vars[k] = *special_parameter_value;
      } else if (k == "LINENO") {
        special_parameter_value = std::to_string(shell_state().current_lineno);
      }
      if (name_expansion) {
        std::vector<std::string> names;
        for (const auto &[name, value] : vars) {
          (void)value;
          if (name.rfind(k, 0) == 0) names.push_back(name);
        }
        extern char **environ;
        if (environ) {
          for (char **entry = environ; *entry; ++entry) {
            const std::string item(*entry);
            const size_t equal = item.find('=');
            if (equal == std::string::npos) continue;
            const std::string name = item.substr(0, equal);
            if (name.rfind(k, 0) == 0) names.push_back(name);
          }
        }
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        if (double_quoted && name_expansion_mode == '@') {
          for (size_t n = 0; n < names.size(); ++n) {
            if (n) { fields.emplace_back(); patterns.emplace_back(); has_pattern.push_back(false); }
            append_text(names[n], false);
          }
          if (names.empty() && fields.size() == 1 && fields[0].empty()) fields.clear();
        } else {
          auto ifs = vars.find("IFS");
          const char *env_ifs = std::getenv("IFS");
          const std::string separator = ifs != vars.end() ? ifs->second : env_ifs ? env_ifs : " \t\n";
          std::string joined;
          for (size_t n = 0; n < names.size(); ++n) {
            if (n && !separator.empty()) joined += separator[0];
            joined += names[n];
          }
          append_text(joined, !double_quoted);
        }
        continue;
      }
      if ((k == "@" || k == "*") && !parameter_transform) {
        if (length_expansion) {
          append_text(std::to_string(positionals.size()), !double_quoted);
          continue;
        }
        std::vector<std::string> items = positionals;
        if (substring_expansion) {
          if (substring_offset == 0) items.insert(items.begin(), shell_name);
          long long start = substring_offset == 0 ? 0
              : substring_offset < 0
                    ? static_cast<long long>(items.size()) + substring_offset
                    : substring_offset - 1;
          start = std::clamp(start, 0LL, static_cast<long long>(items.size()));
          size_t end = items.size();
          if (has_substring_length)
            end = std::min(items.size(), static_cast<size_t>(start) +
                                               static_cast<size_t>(substring_length));
          std::vector<std::string> sliced;
          for (size_t j = static_cast<size_t>(start); j < end; ++j) sliced.push_back(items[j]);
          items = std::move(sliced);
        }
        if (double_quoted && k == "@") {
          for (size_t j = 0; j < items.size(); ++j) {
            if (j) { fields.emplace_back(); patterns.emplace_back(); has_pattern.push_back(false); }
            append_text(items[j], false);
          }
          if (items.empty() && fields.size() == 1 && fields[0].empty() &&
              is_standalone_parameter(i))
            fields.clear();
        } else if (double_quoted || k == "*") {
          auto ifs = vars.find("IFS");
          const char *env_ifs = std::getenv("IFS");
          const std::string separator = ifs != vars.end() ? ifs->second : env_ifs ? env_ifs : " \t\n";
          std::string joined;
          for (size_t j = 0; j < items.size(); ++j) {
            if (j && !separator.empty()) joined += separator[0];
            joined += items[j];
          }
          append_text(joined, !double_quoted);
        } else {
          for (size_t j = 0; j < items.size(); ++j) {
            if (j) { fields.emplace_back(); patterns.emplace_back(); has_pattern.push_back(false); }
            append_text(items[j], true);
          }
        }
        if (!double_quoted && items.empty() && fields.size() == 1 && fields[0].empty() &&
            is_standalone_parameter(i)) fields.clear();
        continue;
      }
      auto it = vars.find(k);
      const char *v = getenv(k.c_str());
      const bool positional_parameter = !k.empty() &&
          std::all_of(k.begin(), k.end(), [](unsigned char ch) {
            return std::isdigit(ch);
          });
      size_t positional_number = 0;
      bool positional_number_valid = true;
      if (positional_parameter) {
        for (unsigned char ch : k) {
          const size_t digit = static_cast<size_t>(ch - '0');
          if (positional_number > (std::numeric_limits<size_t>::max() - digit) / 10) {
            positional_number_valid = false;
            break;
          }
          positional_number = positional_number * 10 + digit;
        }
      }
      const bool positional_is_set = positional_parameter && positional_number_valid &&
          (positional_number == 0 || positional_number <= positionals.size());
      auto array_it = shell_state().arrays.find(k);
      std::vector<size_t> array_indices;
      if (array_it != shell_state().arrays.end())
        for (size_t index = 0; index < array_it->second.size(); ++index)
          if (shell_array_index_is_set(k, index)) array_indices.push_back(index);
      auto associative_it = shell_state().associative_arrays.find(k);
      auto associative_value_it = associative_it == shell_state().associative_arrays.end()
          ? std::unordered_map<std::string, std::string>::iterator{}
          : associative_it->second.find(associative_index ? associative_key : "0");
      const bool associative_key_is_set = associative_it != shell_state().associative_arrays.end() &&
          associative_value_it != associative_it->second.end();
      bool is_set = !unbound_nameref && !shell_state().unset_vars.count(k) &&
          (positional_parameter ? positional_is_set
           : special_parameter_value.has_value() || it != vars.end() || v != nullptr ||
             array_it != shell_state().arrays.end() ||
             (associative_it != shell_state().associative_arrays.end() &&
              (!associative_index || associative_key_is_set || array_mode)));
      if (indexed_array)
        is_set = shell_array_index_is_set(k, array_index);
      if (associative_index) is_set = associative_key_is_set;
      std::string value = special_parameter_value
          ? *special_parameter_value
          : positional_is_set
                ? (positional_number == 0 ? shell_name : positionals[positional_number - 1])
                : it != vars.end() ? it->second : (v ? v : "");
      if (indirect_expansion) {
        const auto reference = shell_state().namerefs.find(original_k);
        if (reference != shell_state().namerefs.end()) {
          value = reference->second;
          is_set = !value.empty();
        } else {
          const std::string target_name = value;
          auto target = vars.find(target_name);
          const char *target_environment = std::getenv(target_name.c_str());
          value = target != vars.end() ? target->second : target_environment ? target_environment : "";
          is_set = !shell_state().unset_vars.count(target_name) &&
                   (target != vars.end() || target_environment != nullptr);
        }
      }
      if (array_it != shell_state().arrays.end() && !array_mode && !indexed_array)
        if (!array_indices.empty()) value = array_it->second[array_indices.front()];
      if (array_it != shell_state().arrays.end() && indexed_array &&
          shell_array_index_is_set(k, array_index))
        value = array_it->second[array_index];
      if (associative_key_is_set && (associative_index || !array_mode)) value = associative_value_it->second;
      const bool associative_scalar = associative_index ||
          (associative_it != shell_state().associative_arrays.end() && !array_mode);
      const bool indexed_scalar = array_it != shell_state().arrays.end() && !array_mode && !indexed_array;
      auto replacement_for_match = [&](const std::string &text,
                                       const std::string &matched) {
        std::string result;
        for (size_t index = 0; index < text.size(); ++index) {
          const char ch = text[index];
          if (ch == '\x0b' && index + 1 < text.size()) {
            result += text[++index];
          } else if (ch == '\x0c') {
            if (shell_state().patsub_replacement) result += matched;
            else result += '&';
          } else if (ch == '\x0d') {
            result += '&';
          } else {
            result += ch;
          }
        }
        return result;
      };
      auto expand_parameter_pattern = [&](const std::string &source)
          -> std::optional<std::string> {
        const bool previous_expansion_error = expansion_error;
        const size_t previous_expansion_error_index = expansion_error_index;
        const bool previous_expansion_error_reported = expansion_error_reported;
        const int previous_substitution_status = substitution_status;
        const bool previous_substitution_ran = substitution_ran;
        const size_t process_substitution_start = process_substitution_mark();
        begin_word_expansion();
        std::vector<std::string> words{source};
        expand_words(words, WordExpansionMode::Pattern);
        const bool succeeded = word_expansion_succeeded();
        const int nested_substitution_status = substitution_status;
        const bool nested_substitution_ran = substitution_ran;
        finish_process_substitutions(process_substitution_start);
        expansion_error = previous_expansion_error;
        expansion_error_index = previous_expansion_error_index;
        expansion_error_reported = previous_expansion_error_reported ||
                                   expansion_error_reported;
        substitution_status = nested_substitution_ran
                                  ? nested_substitution_status
                                  : previous_substitution_status;
        substitution_ran = previous_substitution_ran || nested_substitution_ran;
        if (!succeeded) return std::nullopt;
        std::string pattern;
        for (const auto &word : words) pattern += word;
        return pattern;
      };
      auto expand_parameter_word = [&](const std::string &source)
          -> std::optional<std::vector<std::string>> {
        const bool previous_expansion_error = expansion_error;
        const size_t previous_expansion_error_index = expansion_error_index;
        const bool previous_expansion_error_reported = expansion_error_reported;
        const int previous_substitution_status = substitution_status;
        const bool previous_substitution_ran = substitution_ran;
        const size_t process_substitution_start = process_substitution_mark();
        begin_word_expansion();
        std::vector<std::string> words{source};
        if (double_quoted) {
          words.front().insert(words.front().begin(), '\x02');
          words.front().push_back('\x02');
        }
        expand_words(words, double_quoted ? WordExpansionMode::Redirection
                                          : WordExpansionMode::Normal);
        const bool succeeded = word_expansion_succeeded();
        const int nested_substitution_status = substitution_status;
        const bool nested_substitution_ran = substitution_ran;
        finish_process_substitutions(process_substitution_start);
        expansion_error = previous_expansion_error;
        expansion_error_index = previous_expansion_error_index;
        expansion_error_reported = previous_expansion_error_reported ||
                                   expansion_error_reported;
        substitution_status = nested_substitution_ran
                                  ? nested_substitution_status
                                  : previous_substitution_status;
        substitution_ran = previous_substitution_ran || nested_substitution_ran;
        if (!succeeded) return std::nullopt;
        return words;
      };
      auto append_parameter_word = [&](const std::vector<std::string> &words) {
        for (size_t index = 0; index < words.size(); ++index) {
          if (index > 0) {
            fields.emplace_back();
            patterns.emplace_back();
            has_pattern.push_back(false);
          }
          append_text(words[index], false);
        }
      };
      auto map_locale_case = [](const std::string &input, bool uppercase,
                                bool first_only, const std::string *pattern,
                                int pattern_flags) {
        std::string output;
        std::mbstate_t input_state{};
        size_t offset = 0;
        while (offset < input.size()) {
          wchar_t character = 0;
          const size_t size = std::mbrtowc(&character, input.data() + offset,
                                           input.size() - offset, &input_state);
          if (size == static_cast<size_t>(-1) || size == static_cast<size_t>(-2)) {
            output += input[offset++];
            input_state = {};
            if (first_only) {
              output.append(input, offset, std::string::npos);
              return output;
            }
            continue;
          }
          const size_t character_size = size == 0 ? 1 : size;
          const std::string original = input.substr(offset, character_size);
          const bool matches = !pattern ||
              fnmatch(pattern->c_str(), original.c_str(), pattern_flags) == 0;
          if (matches) {
            const wchar_t mapped = uppercase ? std::towupper(character) : std::towlower(character);
            char encoded[MB_LEN_MAX];
            std::mbstate_t output_state{};
            const size_t encoded_size = std::wcrtomb(encoded, mapped, &output_state);
            if (encoded_size == static_cast<size_t>(-1)) output += original;
            else output.append(encoded, encoded_size);
          } else {
            output += original;
          }
          offset += character_size;
          if (first_only) {
            output.append(input, offset, std::string::npos);
            return output;
          }
        }
        return output;
      };
      auto transform_value = [&](std::string text) {
        if (parameter_transform == 'A' && shell_valid_name(k)) {
          const auto &shell = shell_state();
          auto quote = [](const std::string &value) {
            std::string result = "'";
            for (char ch : value) {
              if (ch == '\'') result += "'\\''";
              else result += ch;
            }
            result += '\'';
            return result;
          };
          const auto named_reference = shell.namerefs.find(original_k);
          if (named_reference != shell.namerefs.end())
            return "declare -n " + original_k + "=" + quote(named_reference->second);
          std::string flags;
          auto indexed = shell.arrays.find(k);
          auto associative = shell.associative_arrays.find(k);
          if (indexed != shell.arrays.end()) flags += 'a';
          else if (associative != shell.associative_arrays.end()) flags += 'A';
          if (shell.integer_vars.count(k)) flags += 'i';
          if (shell.lowercase_vars.count(k)) flags += 'l';
          if (shell.uppercase_vars.count(k)) flags += 'u';
          if (shell.readonly_vars.count(k)) flags += 'r';
          if (std::getenv(k.c_str())) flags += 'x';
          std::string declaration = "declare -" + (flags.empty() ? std::string("-") : flags) + " " + k + "=";
          if (indexed != shell.arrays.end()) {
            declaration += '(';
            bool first = true;
            for (size_t index = 0; index < indexed->second.size(); ++index) {
              if (!shell_array_index_is_set(k, index)) continue;
              if (!first) declaration += ' ';
              first = false;
              declaration += "[" + std::to_string(index) + "]=" + quote(indexed->second[index]);
            }
            declaration += ')';
          } else if (associative != shell.associative_arrays.end()) {
            std::vector<std::string> keys;
            keys.reserve(associative->second.size());
            for (const auto &[key, value] : associative->second) { (void)value; keys.push_back(key); }
            std::sort(keys.begin(), keys.end());
            declaration += '(';
            for (size_t i = 0; i < keys.size(); ++i) {
              if (i) declaration += ' ';
              declaration += "[" + quote(keys[i]) + "]=" + quote(associative->second.at(keys[i]));
            }
            declaration += ')';
          } else declaration += quote(text);
          return declaration;
        }
        if (parameter_transform == 'a') {
          const auto &shell = shell_state();
          std::string attributes;
          if (shell.namerefs.count(original_k)) attributes += 'n';
          if (shell.associative_arrays.count(k)) attributes += 'A';
          else if (shell.arrays.count(k)) attributes += 'a';
          if (shell.integer_vars.count(k)) attributes += 'i';
          if (shell.lowercase_vars.count(k)) attributes += 'l';
          if (shell.uppercase_vars.count(k)) attributes += 'u';
          if (shell.readonly_vars.count(k)) attributes += 'r';
          if (std::getenv(k.c_str())) attributes += 'x';
          return attributes;
        }
        if (parameter_transform == 'P') {
          std::string expanded;
          auto formatted_time = [](const char *format, const std::time_t *when) {
            std::tm local{};
            if (!localtime_r(when, &local)) return std::string();
            char buffer[256];
            const size_t size = std::strftime(buffer, sizeof(buffer), format, &local);
            return std::string(buffer, size);
          };
          char hostname[256] = {};
          if (gethostname(hostname, sizeof(hostname) - 1) != 0) std::strcpy(hostname, "localhost");
          const std::time_t now = std::time(nullptr);
          std::string home_value;
          if (!state.unset_vars.count("HOME")) {
            const auto home_variable = state.vars.find("HOME");
            if (home_variable != state.vars.end()) home_value = home_variable->second;
            else if (const char *environment_home = std::getenv("HOME"))
              home_value = environment_home;
          }
          const char *home = home_value.c_str();
          std::string cwd;
          bool pwd_is_set = false;
          if (!state.unset_vars.count("PWD")) {
            const auto pwd_variable = state.vars.find("PWD");
            if (pwd_variable != state.vars.end()) {
              cwd = pwd_variable->second;
              pwd_is_set = true;
            } else if (const char *environment_pwd = std::getenv("PWD")) {
              cwd = environment_pwd;
              pwd_is_set = true;
            }
          }
          if (!pwd_is_set && !state.unset_vars.count("PWD")) {
            char cwd_buffer[4096];
            const char *cwd_raw = getcwd(cwd_buffer, sizeof(cwd_buffer));
            if (cwd_raw) cwd = cwd_raw;
          }
          long prompt_dirtrim = 0;
          if (!state.unset_vars.count("PROMPT_DIRTRIM")) {
            const auto trim_variable = state.vars.find("PROMPT_DIRTRIM");
            const char *trim_value = trim_variable != state.vars.end()
                                         ? trim_variable->second.c_str()
                                         : std::getenv("PROMPT_DIRTRIM");
            if (trim_value && *trim_value) {
              char *end = nullptr;
              errno = 0;
              const long parsed = std::strtol(trim_value, &end, 10);
              if (errno == 0 && end != trim_value && *end == '\0' && parsed > 0)
                prompt_dirtrim = parsed;
            }
          }
          auto prompt_path = [&](bool full_path) {
            std::string path = cwd;
            if (home && *home && path == home) path = "~";
            else if (home && *home && path.rfind(std::string(home) + '/', 0) == 0)
              path = "~" + path.substr(std::strlen(home));
            if (!full_path && prompt_dirtrim <= 0) {
              if (path == "~") return path;
              const size_t slash = path.find_last_of('/');
              return slash == std::string::npos ? path : path.substr(slash + 1);
            }
            if (prompt_dirtrim <= 0 || path == "/" || path == "~" || path.empty()) return path;
            std::vector<std::string> components;
            std::stringstream path_stream(path);
            std::string component;
            while (std::getline(path_stream, component, '/'))
              if (!component.empty()) components.push_back(component);
            if (components.size() <= static_cast<size_t>(prompt_dirtrim)) return path;
            std::string trimmed = "...";
            const size_t first = components.size() - static_cast<size_t>(prompt_dirtrim);
            for (size_t index = first; index < components.size(); ++index)
              trimmed += "/" + components[index];
            return trimmed;
          };
          for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] != '\\' || i + 1 == text.size()) {
              expanded += text[i];
              continue;
            }
            const char escape = text[++i];
            switch (escape) {
            case 'a': expanded += '\a'; break;
            case 'd': expanded += formatted_time("%a %b %d", &now); break;
            case 'D':
              if (i + 1 < text.size() && text[i + 1] == '{') {
                const size_t end = text.find('}', i + 2);
                if (end != std::string::npos) {
                  const std::string format = text.substr(i + 2, end - i - 2);
                  expanded += formatted_time(format.empty() ? "%c" : format.c_str(), &now);
                  i = end;
                }
              }
              break;
            case 'e': expanded += '\x1b'; break;
            case 'h': {
              const char *dot = std::strchr(hostname, '.');
              expanded.append(hostname, dot ? static_cast<size_t>(dot - hostname) : std::strlen(hostname));
              break;
            }
            case 'H': expanded += hostname; break;
            case 'j': expanded += std::to_string(state.jobs.size()); break;
            case 'l': {
              const char *terminal = ttyname(STDIN_FILENO);
              expanded += terminal ? std::filesystem::path(terminal).filename().string() : "";
              break;
            }
            case 'n': expanded += '\n'; break;
            case 'r': expanded += '\r'; break;
            case 's': expanded += std::filesystem::path(shell_name).filename().string(); break;
            case '!': expanded += std::to_string(history_base + history_length); break;
            case '#': expanded += std::to_string(state.command_number); break;
            case 't': expanded += formatted_time("%H:%M:%S", &now); break;
            case 'T': expanded += formatted_time("%I:%M:%S", &now); break;
            case '@': expanded += formatted_time("%I:%M %p", &now); break;
            case 'A': expanded += formatted_time("%H:%M", &now); break;
            case 'u':
              if (const passwd *user = getpwuid(geteuid())) expanded += user->pw_name;
              break;
            case 'v': case 'V': expanded += DAX_VERSION; break;
            case 'w':
              expanded += prompt_path(true);
              break;
            case 'W':
              expanded += prompt_path(false);
              break;
            case '$': expanded += geteuid() == 0 ? '#' : '$'; break;
            case '[': expanded += '\x01'; break;
            case ']': expanded += '\x02'; break;
            case '\\': expanded += '\\'; break;
            default:
              if (escape >= '0' && escape <= '7') {
                unsigned value = static_cast<unsigned>(escape - '0');
                size_t digits = 1;
                while (i + 1 < text.size() && digits < 3 &&
                       text[i + 1] >= '0' && text[i + 1] <= '7') {
                  value = value * 8 + static_cast<unsigned>(text[++i] - '0');
                  ++digits;
                }
                expanded += static_cast<char>(value & 0xff);
              } else {
                expanded += '\\';
                expanded += escape;
              }
              break;
            }
          }
          return expanded;
        }
        if (quote_transform) {
          std::string quoted = "'";
          for (char ch : text) {
            if (ch == '\'') quoted += "'\\''";
            else quoted += ch;
          }
          quoted += '\'';
          return quoted;
        }
        if (parameter_transform == 'E') {
          std::string expanded;
          for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] != '\\' || i + 1 == text.size()) {
              expanded += text[i];
              continue;
            }
            const char escaped = text[++i];
            switch (escaped) {
            case 'a': expanded += '\a'; break;
            case 'b': expanded += '\b'; break;
            case 'e': case 'E': expanded += '\x1b'; break;
            case 'f': expanded += '\f'; break;
            case 'n': expanded += '\n'; break;
            case 'r': expanded += '\r'; break;
            case 't': expanded += '\t'; break;
            case 'v': expanded += '\v'; break;
            case '\\': case '\'': case '"': case '?': expanded += escaped; break;
            case 'c':
              if (i + 1 < text.size()) {
                const unsigned char control = static_cast<unsigned char>(text[++i]);
                expanded += static_cast<char>(control & 0x1f);
              }
              break;
            case 'x': {
              unsigned value = 0;
              size_t digits = 0;
              while (i + 1 < text.size() && digits < 2 &&
                     std::isxdigit(static_cast<unsigned char>(text[i + 1]))) {
                const char digit = text[++i];
                value = value * 16 + (std::isdigit(static_cast<unsigned char>(digit))
                    ? digit - '0' : std::tolower(static_cast<unsigned char>(digit)) - 'a' + 10);
                ++digits;
              }
              if (digits) expanded += static_cast<char>(value);
              else expanded += 'x';
              break;
            }
            case 'u': case 'U': {
              const size_t limit = escaped == 'u' ? 4 : 8;
              unsigned long value = 0;
              size_t digits = 0;
              while (i + 1 < text.size() && digits < limit &&
                     std::isxdigit(static_cast<unsigned char>(text[i + 1]))) {
                const char digit = text[++i];
                value = value * 16 + static_cast<unsigned long>(
                    std::isdigit(static_cast<unsigned char>(digit))
                        ? digit - '0'
                        : std::tolower(static_cast<unsigned char>(digit)) - 'a' + 10);
                ++digits;
              }
              if (!digits) {
                expanded += escaped;
              } else if (value <= 0x7f) {
                expanded += static_cast<char>(value);
              } else if (value <= 0x7ff) {
                expanded += static_cast<char>(0xc0 | (value >> 6));
                expanded += static_cast<char>(0x80 | (value & 0x3f));
              } else if (value <= 0xffff) {
                expanded += static_cast<char>(0xe0 | (value >> 12));
                expanded += static_cast<char>(0x80 | ((value >> 6) & 0x3f));
                expanded += static_cast<char>(0x80 | (value & 0x3f));
              } else if (value <= 0x10ffff) {
                expanded += static_cast<char>(0xf0 | (value >> 18));
                expanded += static_cast<char>(0x80 | ((value >> 12) & 0x3f));
                expanded += static_cast<char>(0x80 | ((value >> 6) & 0x3f));
                expanded += static_cast<char>(0x80 | (value & 0x3f));
              }
              break;
            }
            default:
              if (escaped >= '0' && escaped <= '7') {
                unsigned value = static_cast<unsigned>(escaped - '0');
                size_t digits = 1;
                while (i + 1 < text.size() && digits < 3 && text[i + 1] >= '0' && text[i + 1] <= '7') {
                  value = value * 8 + static_cast<unsigned>(text[++i] - '0');
                  ++digits;
                }
                expanded += static_cast<char>(value & 0xff);
              } else expanded += escaped;
            }
          }
          return expanded;
        }
        if (parameter_transform == 'U' || parameter_transform == 'L') {
          return map_locale_case(text, parameter_transform == 'U', false, nullptr, 0);
        }
        if (parameter_transform == 'u')
          return map_locale_case(text, true, true, nullptr, 0);
        if (parameter_transform == 'K' || parameter_transform == 'k') {
          std::string quoted = "\"";
          for (char ch : text) {
            if (ch == '\\' || ch == '"' || ch == '$' || ch == '`') quoted += '\\';
            quoted += ch;
          }
          quoted += '"';
          return quoted;
        }
        if (case_transform) {
          std::string pattern;
          if (transform_pattern.empty()) {
            pattern = "?";
          } else {
            bool quoted_pattern = false;
            for (size_t i = 0; i < transform_pattern.size(); ++i) {
              const char ch = transform_pattern[i];
              if (ch == '\x01' || ch == '\x02') {
                quoted_pattern = !quoted_pattern;
                continue;
              }
              bool escaped_character = ch == '\x0b' && i + 1 < transform_pattern.size();
              const char pattern_character = escaped_character ? transform_pattern[++i] : ch;
              const bool pattern_meta = std::string("*?[]\\-!^@+()").find(pattern_character) != std::string::npos;
              if ((quoted_pattern || escaped_character) && pattern_meta) pattern += '\\';
              pattern += pattern_character;
            }
          }
          return map_locale_case(text, case_transform == '^', !transform_all,
                                 &pattern, shell_state().extglob ? FNM_EXTMATCH : 0);
        }
        return text;
      };
      if ((k == "@" || k == "*") && parameter_transform) {
        if (parameter_transform == 'A') {
          auto quote = [](const std::string &text) {
            std::string result = "'";
            for (char ch : text) {
              if (ch == '\'') result += "'\\''";
              else result += ch;
            }
            return result + "'";
          };
          std::string assignment = "set --";
          for (const auto &item : positionals) assignment += " " + quote(item);
          append_text(assignment, !double_quoted);
          continue;
        }
        std::vector<std::string> items;
        items.reserve(positionals.size());
        for (const auto &item : positionals)
          items.push_back(parameter_transform == 'a' ? std::string() : transform_value(item));
        if (double_quoted && k == "@") {
          for (size_t j = 0; j < items.size(); ++j) {
            if (j) { fields.emplace_back(); patterns.emplace_back(); has_pattern.push_back(false); }
            append_text(items[j], false);
          }
          if (items.empty() && fields.size() == 1 && fields[0].empty() &&
              is_standalone_parameter(i))
            fields.clear();
        } else if (double_quoted || k == "*") {
          auto ifs = vars.find("IFS");
          const char *environment_ifs = std::getenv("IFS");
          const std::string separator = ifs != vars.end() ? ifs->second
              : environment_ifs ? environment_ifs : " \t\n";
          std::string joined;
          for (size_t j = 0; j < items.size(); ++j) {
            if (j && !separator.empty()) joined += separator[0];
            joined += items[j];
          }
          append_text(joined, !double_quoted);
        } else {
          for (size_t j = 0; j < items.size(); ++j) {
            if (j) { fields.emplace_back(); patterns.emplace_back(); has_pattern.push_back(false); }
            append_text(items[j], true);
          }
        }
        if (!double_quoted && items.empty() && fields.size() == 1 && fields[0].empty() &&
            is_standalone_parameter(i))
          fields.clear();
        continue;
      }
      if (is_set && (parameter_transform == 'K' || parameter_transform == 'k') &&
          !array_mode && !indexed_array && !associative_index &&
          (array_it != shell_state().arrays.end() ||
           associative_it != shell_state().associative_arrays.end())) {
        std::vector<std::pair<std::string, std::string>> entries;
        if (associative_it != shell_state().associative_arrays.end()) {
          entries.reserve(associative_it->second.size());
          for (const auto &[key, item] : associative_it->second)
            entries.emplace_back(key, item);
          std::sort(entries.begin(), entries.end(),
                    [](const auto &left, const auto &right) { return left.first < right.first; });
        } else {
          entries.reserve(array_indices.size());
          for (const size_t index : array_indices)
            entries.emplace_back(std::to_string(index), array_it->second[index]);
        }
        auto quote_reusable = [](const std::string &text) {
          std::string quoted = "\"";
          for (char ch : text) {
            if (ch == '\\' || ch == '"' || ch == '$' || ch == '`') quoted += '\\';
            quoted += ch;
          }
          quoted += '"';
          return quoted;
        };
        if (parameter_transform == 'K') {
          std::string transformed;
          for (const auto &[key, item] : entries) {
            if (!transformed.empty()) transformed += ' ';
            transformed += quote_reusable(key);
            transformed += ' ';
            transformed += quote_reusable(item);
          }
          append_text(transformed, !double_quoted);
        } else {
          for (const auto &[key, item] : entries) {
            append_text(key, true);
            append_text(" ", true);
            append_text(item, true);
          }
        }
        continue;
      }
      if (is_set && (case_transform || quote_transform || parameter_transform) &&
          (array_it == shell_state().arrays.end() || indexed_array || indexed_scalar || associative_scalar))
        value = transform_value(std::move(value));
      std::string expanded_replace_value = replace_value;
      if (replace_expansion) {
        const bool previous_expansion_error = expansion_error;
        const size_t previous_expansion_error_index = expansion_error_index;
        const bool previous_expansion_error_reported = expansion_error_reported;
        const int previous_substitution_status = substitution_status;
        const bool previous_substitution_ran = substitution_ran;
        const size_t process_substitution_start = process_substitution_mark();
        begin_word_expansion();
        std::vector<std::string> replacement_words{
            std::string("\x09") + replace_value + "\x09"};
        expand_words(replacement_words, WordExpansionMode::ParameterReplacement);
        const bool replacement_expansion_succeeded = word_expansion_succeeded();
        const int nested_substitution_status = substitution_status;
        const bool nested_substitution_ran = substitution_ran;
        finish_process_substitutions(process_substitution_start);
        expansion_error = previous_expansion_error;
        expansion_error_index = previous_expansion_error_index;
        expansion_error_reported = previous_expansion_error_reported ||
                                  expansion_error_reported;
        substitution_status = nested_substitution_ran
                                  ? nested_substitution_status
                                  : previous_substitution_status;
        substitution_ran = previous_substitution_ran || nested_substitution_ran;
        if (!replacement_expansion_succeeded || replacement_words.empty()) {
          fail_expansion();
          expanded_replace_value.clear();
        } else {
          expanded_replace_value.clear();
          for (const auto &word : replacement_words) expanded_replace_value += word;
        }
      }
      std::string expanded_replace_pattern;
      if (replace_expansion) {
        const auto pattern = expand_parameter_pattern(replace_pattern);
        if (!pattern) { fail_expansion(); continue; }
        expanded_replace_pattern = *pattern;
      }
      auto replace_parameter_value = [&](std::string text) {
        if (!replace_expansion || expanded_replace_pattern.empty()) return text;
        const std::string &pattern = expanded_replace_pattern;
        const int pattern_flags = (shell_state().extglob ? FNM_EXTMATCH : 0) |
                                  (shell_state().nocasematch ? FNM_CASEFOLD : 0);
        size_t cursor = 0;
        while (cursor <= text.size()) {
          size_t match_start = std::string::npos, match_end = std::string::npos;
          const size_t first_start = replace_prefix ? 0 : cursor;
          const size_t last_start = text.size();
          for (size_t start = first_start; start <= last_start; ++start) {
            if (replace_prefix && start != 0) break;
            if (replace_suffix) {
              if (fnmatch(pattern.c_str(), text.substr(start).c_str(), pattern_flags) == 0) {
                match_start = start;
                match_end = text.size();
                break;
              }
            } else {
              for (size_t end = text.size() + 1; end-- > start; ) {
                if (fnmatch(pattern.c_str(), text.substr(start, end - start).c_str(), pattern_flags) == 0) {
                  match_start = start;
                  match_end = end;
                  break;
                }
              }
              if (match_start != std::string::npos) break;
            }
          }
          if (match_start == std::string::npos) break;
          const size_t matched_length = match_end - match_start;
          const std::string matched = text.substr(match_start, matched_length);
          const std::string replacement = replacement_for_match(expanded_replace_value, matched);
          text.replace(match_start, matched_length, replacement);
          if (!replace_all || replace_prefix || replace_suffix) break;
          if (matched_length == 0) {
            if (match_start >= text.size()) break;
            cursor = match_start + 1;
          } else {
            cursor = match_start + replacement.size();
          }
        }
        return text;
      };
      if (replace_expansion && is_set &&
          (array_it == shell_state().arrays.end() || indexed_array || indexed_scalar || associative_scalar)) {
        value = replace_parameter_value(std::move(value));
      }
      auto transform_and_replace = [&](std::string text) {
        if (case_transform || quote_transform || parameter_transform)
          text = transform_value(std::move(text));
        return replace_parameter_value(std::move(text));
      };
      if (substring_expansion && is_set &&
          (array_it == shell_state().arrays.end() || associative_scalar)) {
        const long long size = static_cast<long long>(locale_character_length(value));
        long long start = substring_offset < 0 ? size + substring_offset : substring_offset;
        start = std::clamp(start, 0LL, size);
        long long end = size;
        if (has_substring_length) {
          end = substring_length < 0 ? size + substring_length : start + substring_length;
          end = std::clamp(end, start, size);
        }
        value = locale_character_substring(value, static_cast<size_t>(start),
                                            static_cast<size_t>(end));
      }
      const bool missing_subscript =
          (associative_index && !associative_key_is_set) ||
          (indexed_array && !shell_array_index_is_set(k, array_index));
      const bool default_operator = parameter_operator == "-" || parameter_operator == ":-" ||
          parameter_operator == "=" || parameter_operator == ":=" ||
          parameter_operator == "+" || parameter_operator == ":+" ||
          parameter_operator == "?" || parameter_operator == ":?";
      if (missing_subscript && default_operator) {
        const bool colon_operator = parameter_operator[0] == ':';
        const bool condition = colon_operator ? (!is_set || value.empty()) : !is_set;
        const char op = parameter_operator.back();
        if ((op == '-' && condition) || (op == '+' && !condition)) {
          const auto expanded_word = expand_parameter_word(parameter_word);
          if (!expanded_word) fail_expansion();
          else append_parameter_word(*expanded_word);
        }
        else if (op == '=' && condition) {
          const auto expanded_word = expand_parameter_word(parameter_word);
          if (!expanded_word) {
            fail_expansion();
            continue;
          }
          std::string assigned_value;
          for (const auto &word : *expanded_word) assigned_value += word;
          const std::string target = associative_index
              ? k + "[" + associative_key + "]"
              : k + "[" + std::to_string(array_index) + "]";
          if (set_shell_variable(target, assigned_value))
            append_text(assigned_value, !double_quoted);
          else fail_expansion();
        }
        else if (op == '?' && condition) {
          const auto expanded_word = expand_parameter_word(parameter_word);
          if (!expanded_word) {
            fail_expansion();
            continue;
          }
          std::string message;
          for (const auto &word : *expanded_word) {
            if (!message.empty()) message += ' ';
            message += word;
          }
          std::cerr << "dax: " << k << ": " <<
              (message.empty() ? "parameter null or not set" : message) << '\n';
          expansion_error_reported = true;
          fail_expansion();
          if (!shell_state().interactive_shell) {
            shell_state().exit_value = 1;
            shell_state().exiting = true;
          }
        }
      } else if (array_mode || indexed_array || associative_index) {
        if (associative_it != shell_state().associative_arrays.end()) {
          std::vector<std::string> items;
          if (associative_index) {
            if (length_expansion && !associative_key_is_set && nounset)
              fail_unset_parameter(k + "[" + associative_key + "]");
            else if (length_expansion)
              append_text(std::to_string(associative_key_is_set
                                             ? locale_character_length(value) : 0),
                          !double_quoted);
            else if (associative_key_is_set)
              append_text(value, !double_quoted);
            else if (nounset)
              fail_unset_parameter(k + "[" + associative_key + "]");
          } else {
            items.reserve(associative_it->second.size());
            for (const auto &[key, item] : associative_it->second)
              items.push_back(transform_and_replace(associative_keys ? key : item));
            if (length_expansion) append_text(std::to_string(items.size()), !double_quoted);
            else if (array_mode == '@' && double_quoted) {
              for (size_t j = 0; j < items.size(); ++j) {
                if (j) { fields.emplace_back(); patterns.emplace_back(); has_pattern.push_back(false); }
                append_text(items[j], false);
              }
              if (items.empty() && fields.size() == 1 && fields[0].empty()) fields.clear();
            } else if (array_mode == '*' && double_quoted) {
              auto ifs = vars.find("IFS");
              const char *env_ifs = std::getenv("IFS");
              const std::string sep = ifs != vars.end() ? ifs->second : env_ifs ? env_ifs : " \t\n";
              std::string joined;
              for (size_t j = 0; j < items.size(); ++j) {
                if (j && !sep.empty()) joined += sep[0];
                joined += items[j];
              }
              append_text(joined, false);
            } else if (array_mode == '@') {
              for (size_t j = 0; j < items.size(); ++j) {
                if (j) { fields.emplace_back(); patterns.emplace_back(); has_pattern.push_back(false); }
                append_text(items[j], true);
              }
            } else {
              auto ifs = vars.find("IFS");
              const char *env_ifs = std::getenv("IFS");
              const std::string separator = ifs != vars.end() ? ifs->second : env_ifs ? env_ifs : " \t\n";
              std::string joined;
              for (size_t j = 0; j < items.size(); ++j) {
                if (j && !separator.empty()) joined += separator[0];
                joined += items[j];
              }
              append_text(joined, true);
            }
            if (!double_quoted && items.empty() && fields.size() == 1 && fields[0].empty() &&
                is_standalone_parameter(i)) fields.clear();
          }
        } else if (array_it == shell_state().arrays.end()) {
          if (nounset) fail_unset_parameter(k);
        } else if (length_expansion) {
          const bool element_is_set = shell_array_index_is_set(k, array_index);
          if (indexed_array && !array_mode && !element_is_set && nounset)
            fail_unset_parameter(k + "[" + std::to_string(array_index) + "]");
          else {
            const size_t length = array_mode ? array_indices.size()
                                    : element_is_set
                                        ? locale_character_length(array_it->second[array_index]) : 0;
            append_text(std::to_string(length), !double_quoted);
          }
        } else if (indexed_array) {
          if (shell_array_index_is_set(k, array_index)) {
            std::string element = array_it->second[array_index];
            if (substring_expansion) {
              const long long size = static_cast<long long>(locale_character_length(element));
              const long long start = std::clamp(substring_offset < 0 ? size + substring_offset : substring_offset,
                                                 0LL, size);
              const long long end = has_substring_length
                  ? std::clamp(start + substring_length, start, size) : size;
              element = locale_character_substring(element, static_cast<size_t>(start),
                                                     static_cast<size_t>(end));
            }
            append_text(transform_and_replace(std::move(element)), !double_quoted);
          } else if (nounset)
            fail_unset_parameter(k + "[" + std::to_string(array_index) + "]");
        } else if (array_mode && associative_keys) {
          if (array_mode == '@' && double_quoted) {
            for (size_t j = 0; j < array_indices.size(); ++j) {
              if (j) { fields.emplace_back(); patterns.emplace_back(); has_pattern.push_back(false); }
              append_text(std::to_string(array_indices[j]), false);
            }
            if (array_indices.empty() && fields.size() == 1 && fields[0].empty()) fields.clear();
          } else if (array_mode == '@') {
            for (size_t j = 0; j < array_indices.size(); ++j) {
              if (j) { fields.emplace_back(); patterns.emplace_back(); has_pattern.push_back(false); }
              append_text(std::to_string(array_indices[j]), true);
            }
          } else {
            std::string indices;
            for (size_t j = 0; j < array_indices.size(); ++j) {
              if (j) indices += double_quoted ? (vars.count("IFS") && !vars["IFS"].empty() ? vars["IFS"][0] : ' ') : ' ';
              indices += std::to_string(array_indices[j]);
            }
            append_text(indices, !double_quoted);
          }
          if (!double_quoted && array_indices.empty() && fields.size() == 1 &&
              fields[0].empty() && is_standalone_parameter(i)) fields.clear();
        } else if (array_mode == '@' && double_quoted) {
          size_t first = 0, last = array_indices.size();
          if (substring_expansion) {
            const long long size = static_cast<long long>(last);
            first = static_cast<size_t>(std::clamp(substring_offset < 0 ? size + substring_offset : substring_offset,
                                                   0LL, size));
            if (has_substring_length) {
              const long long end = substring_length < 0 ? size + substring_length
                                                         : static_cast<long long>(first) + substring_length;
              last = static_cast<size_t>(std::clamp(end, static_cast<long long>(first), size));
            }
          }
          for (size_t j = first; j < last; ++j) {
            if (j > first) { fields.emplace_back(); patterns.emplace_back(); has_pattern.push_back(false); }
            append_text(transform_and_replace(array_it->second[array_indices[j]]), false);
          }
          if (first == last && fields.size() == 1 && fields[0].empty() &&
              is_standalone_parameter(i)) fields.clear();
        } else if (array_mode == '*' && double_quoted) {
          auto ifs = vars.find("IFS");
          const char *env_ifs = std::getenv("IFS");
          const std::string sep = ifs != vars.end() ? ifs->second : env_ifs ? env_ifs : " \t\n";
          std::string joined;
          size_t first = 0, last = array_indices.size();
          if (substring_expansion) {
            const long long size = static_cast<long long>(last);
            first = static_cast<size_t>(std::clamp(substring_offset < 0 ? size + substring_offset : substring_offset,
                                                   0LL, size));
            if (has_substring_length) {
              const long long end = substring_length < 0 ? size + substring_length
                                                         : static_cast<long long>(first) + substring_length;
              last = static_cast<size_t>(std::clamp(end, static_cast<long long>(first), size));
            }
          }
          for (size_t j = first; j < last; ++j) {
            if (j > first && !sep.empty()) joined += sep[0];
            joined += transform_and_replace(array_it->second[array_indices[j]]);
          }
          append_text(joined, false);
        } else if (array_mode == '@') {
          size_t first = 0, last = array_indices.size();
          if (substring_expansion) {
            const long long size = static_cast<long long>(last);
            first = static_cast<size_t>(std::clamp(substring_offset < 0 ? size + substring_offset : substring_offset,
                                                   0LL, size));
            if (has_substring_length) {
              const long long end = substring_length < 0 ? size + substring_length
                                                         : static_cast<long long>(first) + substring_length;
              last = static_cast<size_t>(std::clamp(end, static_cast<long long>(first), size));
            }
          }
          for (size_t j = first; j < last; ++j) {
            if (j > first) { fields.emplace_back(); patterns.emplace_back(); has_pattern.push_back(false); }
            append_text(transform_and_replace(array_it->second[array_indices[j]]), true);
          }
          if (first == last && fields.size() == 1 && fields[0].empty() &&
              is_standalone_parameter(i)) fields.clear();
        } else if (array_mode == '*') {
          auto ifs = vars.find("IFS");
          const char *env_ifs = std::getenv("IFS");
          const std::string separator = ifs != vars.end() ? ifs->second : env_ifs ? env_ifs : " \t\n";
          std::string joined;
          size_t first = 0, last = array_indices.size();
          if (substring_expansion) {
            const long long size = static_cast<long long>(last);
            first = static_cast<size_t>(std::clamp(substring_offset < 0 ? size + substring_offset : substring_offset,
                                                   0LL, size));
            if (has_substring_length) {
              const long long end = substring_length < 0 ? size + substring_length
                                                         : static_cast<long long>(first) + substring_length;
              last = static_cast<size_t>(std::clamp(end, static_cast<long long>(first), size));
            }
          }
          for (size_t j = first; j < last; ++j) {
            if (j > first && !separator.empty()) joined += separator[0];
            joined += transform_and_replace(array_it->second[array_indices[j]]);
          }
          if (!double_quoted && first == last && fields.size() == 1 && fields[0].empty() &&
              is_standalone_parameter(i)) {
            fields.clear();
          } else {
            append_text(joined, true);
          }
        } else {
          if (indexed_array && substring_expansion) {
            const long long size = static_cast<long long>(array_indices.size());
            const long long start = std::clamp(substring_offset < 0 ? size + substring_offset : substring_offset,
                                               0LL, size);
            const long long end = has_substring_length
                ? std::clamp(substring_length < 0 ? size + substring_length : start + substring_length,
                             start, size)
                : size;
            for (long long j = start; j < end; ++j) {
              if (j > start) { fields.emplace_back(); patterns.emplace_back(); has_pattern.push_back(false); }
              append_text(transform_and_replace(array_it->second[array_indices[static_cast<size_t>(j)]]), true);
            }
          } else if (shell_array_index_is_set(k, array_index)) {
            append_text(transform_and_replace(array_it->second[array_index]), !double_quoted);
          }
        }
      } else if (length_expansion) {
        append_text(std::to_string(locale_character_length(value)), !double_quoted);
      } else if (parameter_operator == "#" || parameter_operator == "##" ||
                 parameter_operator == "%" || parameter_operator == "%%") {
        const bool prefix = parameter_operator[0] == '#';
        const bool longest = parameter_operator.size() == 2;
        const auto expanded_pattern = expand_parameter_pattern(parameter_word);
        if (!expanded_pattern) { fail_expansion(); continue; }
        const std::string &pattern = *expanded_pattern;
        const int pattern_flags = (shell_state().extglob ? FNM_EXTMATCH : 0) |
                                  (shell_state().nocasematch ? FNM_CASEFOLD : 0);
        if (prefix) {
          size_t match = std::string::npos;
          for (size_t n = 0; n <= value.size(); ++n)
            if (fnmatch(pattern.c_str(), value.substr(0, n).c_str(), pattern_flags) == 0) { match = n; if (!longest) break; }
          if (match != std::string::npos) value.erase(0, match);
        } else {
          size_t match = std::string::npos;
          if (longest) {
            // The longest matching suffix starts at the smallest index.
            for (size_t n = 0; n <= value.size(); ++n)
              if (fnmatch(pattern.c_str(), value.substr(n).c_str(), pattern_flags) == 0) { match = n; break; }
          } else {
            // The shortest matching suffix starts at the largest index.
            for (size_t n = value.size() + 1; n-- > 0; )
              if (fnmatch(pattern.c_str(), value.substr(n).c_str(), pattern_flags) == 0) { match = n; break; }
          }
          if (match != std::string::npos) value.erase(match);
        }
        append_text(value, !double_quoted);
      } else if (!parameter_operator.empty()) {
        const bool colon_operator = parameter_operator[0] == ':';
        const bool condition = colon_operator ? (!is_set || value.empty()) : !is_set;
        const char op = parameter_operator.back();
        if ((op == '-' && condition) || (op == '+' && !condition)) {
          const auto expanded_word = expand_parameter_word(parameter_word);
          if (!expanded_word) fail_expansion();
          else append_parameter_word(*expanded_word);
        }
        else if (op == '=' && condition) {
          const auto expanded_word = expand_parameter_word(parameter_word);
          if (!expanded_word) {
            fail_expansion();
            continue;
          }
          std::string assigned_value;
          for (const auto &word : *expanded_word) assigned_value += word;
          const std::string target = nameref_target_name.empty() ? k : nameref_target_name;
          if (set_shell_variable(target, assigned_value))
            append_text(assigned_value, !double_quoted);
          else fail_expansion();
        }
        else if (op == '?' && condition) {
          const auto expanded_word = expand_parameter_word(parameter_word);
          if (!expanded_word) {
            fail_expansion();
            continue;
          }
          std::string message;
          for (const auto &word : *expanded_word) {
            if (!message.empty()) message += ' ';
            message += word;
          }
          std::cerr << "dax: " << k << ": "
                    << (message.empty() ? "parameter null or not set" : message) << '\n';
          expansion_error_reported = true;
          fail_expansion();
          if (!shell_state().interactive_shell) {
            shell_state().exit_value = 1;
            shell_state().exiting = true;
          }
        } else if (is_set) append_text(value, !double_quoted);
      } else if (is_set) append_text(value, !double_quoted);
      else if (nounset && parameter_transform != 'a' && parameter_transform != 'A') {
        fail_unset_parameter(k);
      }
    }
    if (pending_split && fields.size() > 1 && fields.back().empty()) {
      fields.pop_back(); patterns.pop_back(); has_pattern.pop_back();
    }
    for (size_t i = 0; i < fields.size(); ++i) {
      if (mode == WordExpansionMode::Pattern) {
        expanded_words.push_back(patterns[i]);
        continue;
      }
      if (mode == WordExpansionMode::Redirection) {
        expanded_words.push_back(fields[i]);
        continue;
      }
      if (!has_pattern[i]) { expanded_words.push_back(fields[i]); continue; }
      if (noglob) {
        expanded_words.push_back(fields[i]);
        continue;
      }
      const std::string &pattern = patterns[i];
      const bool has_extglob = shell_state().extglob && contains_extglob_operator(pattern);
      const bool has_globstar = shell_state().globstar && pattern.find("**") != std::string::npos;
      const std::string globignore = active_globignore();
      const bool ignore_patterns_active = globignore_has_patterns(globignore);
      const bool custom_glob = has_extglob || has_globstar || shell_state().nocaseglob ||
                               shell_state().dotglob || ignore_patterns_active;
      std::vector<std::string> matches;
      if (custom_glob) {
        matches = extglob_path_expand(pattern);
      } else {
        glob_t glob_result{};
        const std::string glob_pattern = glob_ascii_ranges(pattern);
        const int rc = glob(glob_pattern.c_str(), GLOB_NOSORT, nullptr, &glob_result);
        if (rc == 0 && glob_result.gl_pathc) {
          for (size_t j = 0; j < glob_result.gl_pathc; ++j)
            matches.emplace_back(glob_result.gl_pathv[j]);
        }
        globfree(&glob_result);
      }
      if (shell_state().globskipdots || ignore_patterns_active) {
        matches.erase(std::remove_if(matches.begin(), matches.end(), [&](const std::string &path) {
          const std::string name = std::filesystem::path(path).filename().string();
          if (shell_state().globskipdots && (name == "." || name == "..")) return true;
          return ignore_patterns_active &&
                 (name == "." || name == ".." || globignore_matches(path, globignore));
        }), matches.end());
      }
      sort_glob_matches(matches);
      if (matches.empty()) {
        if (shell_state().failglob) {
          std::cerr << "dax: no match: " << fields[i] << '\n';
          fail_expansion();
        } else if (!shell_state().nullglob) expanded_words.push_back(fields[i]);
      } else {
        expanded_words.insert(expanded_words.end(), matches.begin(), matches.end());
      }
    }
  }
  words = std::move(expanded_words);
}
void expand_aliases(std::vector<ShellToken> &words) {
  static const std::vector<std::string> reserved_words = {
      "if", "then", "elif", "else", "fi", "for", "while", "until",
      "do", "done", "case", "esac", "in", "function", "select", "time"};
  auto is_assignment = [](const std::string &word) {
    const size_t equal = word.find('=');
    if (equal == std::string::npos || equal == 0) return false;
    const std::string name = word.substr(0, equal);
    return name_ok(name);
  };
  auto is_redirection = [](const std::string &word) {
    static const std::vector<std::string> operators = {
        "<", ">", ">|", ">>", "<<", "<<-", "<<<", "<&", ">&",
        "2>", "2>>", "2>&", "&>", "&>>"};
    return std::find(operators.begin(), operators.end(), word) != operators.end();
  };

  bool command_position = true;
  bool redirection_target = false;
  std::unordered_map<size_t, size_t> alias_expansions;
  for (size_t index = 0; index < words.size();) {
    if (redirection_target) {
      redirection_target = false;
      words[index].alias_expansion_enabled = false;
      ++index;
      continue;
    }
    const std::string token = words[index];
    if (is_redirection(token)) {
      redirection_target = true;
      ++index;
      continue;
    }
    if ((command_position || words[index].alias_expansion_enabled) && !words[index].quoted &&
        std::find(reserved_words.begin(), reserved_words.end(), token) == reserved_words.end() &&
        !is_assignment(token)) {
      auto alias = aliases.find(token);
      if (alias != aliases.end() && alias_expansions[index] < 10) {
        ++alias_expansions[index];
        const std::string replacement_text = alias->second;
        const bool expand_following_word = !replacement_text.empty() &&
            std::isspace(static_cast<unsigned char>(replacement_text.back()));
        const size_t call_line = words[index].line;
        const size_t call_column = words[index].column;
        const size_t call_begin = words[index].source_begin;
        const size_t call_end = words[index].source_end;
        auto replacement = tokenize(alias->second);
        for (auto &expanded : replacement) {
          expanded.line = call_line;
          expanded.column = call_column;
          expanded.source_begin = call_begin;
          expanded.source_end = call_end;
        }
        words.erase(words.begin() + index);
        words.insert(words.begin() + index, replacement.begin(), replacement.end());
        const size_t following_word = index + replacement.size();
        if (expand_following_word && following_word < words.size())
          words[following_word].alias_expansion_enabled = true;
        continue;
      }
      words[index].alias_expansion_enabled = false;
    }

    if (token == ";" || token == "&" || token == "&&" || token == "||" || token == "|") {
      command_position = true;
    } else if (token == "(" || token == "{") {
      command_position = true;
    } else if (is_redirection(token)) {
      command_position = false;
      redirection_target = true;
    } else if (token == ")" || token == "}") {
      command_position = false;
    } else if (token == "if" || token == "then" || token == "elif" || token == "else" ||
               token == "do" || token == "while" || token == "until" || token == "!") {
      command_position = true;
    } else if (token == "for" || token == "case" || token == "function" || token == "select") {
      command_position = false;
    } else if (!is_assignment(token)) {
      command_position = false;
    }
    ++index;
  }
}
bool define_function(const std::string &name, const std::string &body) {
  if (!name_ok(name)) return false;
  if (state.posix_mode && shell_is_posix_special_builtin(name)) {
    std::cerr << "dax: " << name
              << ": cannot define a function with the name of a POSIX special builtin\n";
    return false;
  }
  if (state.readonly_functions.count(name)) {
    std::cerr << "dax: " << name << ": readonly function\n";
    return false;
  }
  functions[name] = body;
  state.function_sources[name] = active_source_name();
  if (state.exported_functions.count(name)) export_functions_to_environment();
  return true;
}
void import_exported_functions() {
  constexpr const char *prefix = "BASH_FUNC_";
  for (char **entry = environ; entry && *entry; ++entry) {
    const std::string variable(*entry);
    const size_t equals = variable.find('=');
    if (equals == std::string::npos || variable.compare(0, 10, prefix) != 0 ||
        equals < 12 || variable.compare(equals - 2, 2, "%%") != 0)
      continue;
    const std::string name = variable.substr(10, equals - 12);
    const std::string definition = variable.substr(equals + 1);
    if (!name_ok(name) || definition.rfind("() {", 0) != 0) continue;
    const size_t closing = definition.find_last_not_of(" \t\r\n");
    if (closing == std::string::npos || definition[closing] != '}') continue;
    const size_t body_start = 4;
    if (closing < body_start) continue;
    if (define_function(name, definition.substr(body_start, closing - body_start)))
      state.exported_functions.insert(name);
  }
}
void export_functions_to_environment() {
  for (const auto &name : state.exported_functions) {
    const auto function = state.functions.find(name);
    if (function == state.functions.end()) continue;
    std::string definition = "() {";
    definition += function->second;
    if (definition.empty() || definition.back() != '\n') definition += '\n';
    definition += '}';
    const std::string variable = "BASH_FUNC_" + name + "%%";
    if (setenv(variable.c_str(), definition.c_str(), 1) != 0)
      std::cerr << "export: failed to export function " << name << '\n';
  }
}
bool function_exists(const std::string &name) {
  return functions.count(name) != 0;
}
bool shell_should_return() { return returning; }
void enter_shell_loop() { ++loop_depth; }
void leave_shell_loop() { if (loop_depth > 0) --loop_depth; }
int take_loop_control() {
  if (!loop_break && !loop_continue) return 0;
  if (loop_control_levels > 1) {
    --loop_control_levels;
    return 3;
  }
  const int result = loop_break ? 1 : 2;
  loop_break = loop_continue = false;
  loop_control_levels = 1;
  return result;
}
bool loop_control_pending() { return loop_break || loop_continue; }
void run_err_trap(int status) {
  if (status == 0 || state.running_err_trap || state.err_trap_suppressed ||
      state.err_trap_ignored_context > 0 || errexit_immune_status)
    return;
  if (!state.errtrace &&
      std::find(state.function_frame_flags.begin(),
                state.function_frame_flags.end(), true) !=
          state.function_frame_flags.end())
    return;
  auto it = traps.find(-1);
  if (it == traps.end()) return;
  ++state.err_trap_generation;
  state.running_err_trap = true;
  const int saved_status = last_status();
  set_last_status(status);
  execute_line(it->second);
  set_last_status(saved_status);
  state.running_err_trap = false;
}
void run_debug_trap(const std::string &command) {
  if (state.running_debug_trap) return;
  if (!command.empty()) state.bash_command = command;
  const size_t function_depth = static_cast<size_t>(std::count(
      state.function_frame_flags.begin(), state.function_frame_flags.end(), true));
  bool traced_function_scope = false;
  for (size_t frame = 0; frame < state.function_stack.size() &&
                         frame < state.function_frame_flags.size(); ++frame) {
    if (state.function_frame_flags[frame] &&
        state.traced_functions.count(state.function_stack[frame])) {
      traced_function_scope = true;
      break;
    }
  }
  if (!state.functrace &&
      (state.subshell_level > 0 ||
       (function_depth > state.debug_trap_function_depth && !traced_function_scope))) return;
  const auto trap = traps.find(-2);
  if (trap == traps.end()) return;
  const int saved_status = last_status();
  const size_t saved_lineno = state.current_lineno;
  state.running_debug_trap = true;
  execute_line(trap->second);
  state.running_debug_trap = false;
  state.current_lineno = saved_lineno;
  set_last_status(saved_status);
}
void run_return_trap() {
  if (state.running_return_trap) return;
  const size_t function_depth = static_cast<size_t>(std::count(
      state.function_frame_flags.begin(), state.function_frame_flags.end(), true));
  bool traced_function_scope = false;
  for (size_t frame = 0; frame < state.function_stack.size() &&
                         frame < state.function_frame_flags.size(); ++frame) {
    if (state.function_frame_flags[frame] &&
        state.traced_functions.count(state.function_stack[frame])) {
      traced_function_scope = true;
      break;
    }
  }
  if (!state.functrace && function_depth > state.return_trap_function_depth &&
      !traced_function_scope) return;
  const auto trap = traps.find(-3);
  if (trap == traps.end()) return;
  const int saved_status = last_status();
  const size_t saved_lineno = state.current_lineno;
  const bool saved_returning = returning;
  state.running_return_trap = true;
  returning = false;
  execute_line(trap->second);
  returning = saved_returning;
  state.running_return_trap = false;
  state.current_lineno = saved_lineno;
  set_last_status(saved_status);
}
void run_exit_trap() {
  if (state.exit_trap_active) {
    auto it = traps.find(0);
    if (it != traps.end()) {
      const int saved_status = last_status();
      execute_line(it->second);
      set_last_status(saved_status);
    }
  }
  if (state.subshell_level == 0 && state.interactive_shell && state.login_shell &&
      state.huponexit) {
    for (const auto &job : state.jobs) {
      if (job.completed || job.ignore_hup) continue;
      if (kill(-job.pid, SIGHUP) != 0 && errno == ESRCH) kill(job.pid, SIGHUP);
      if (job.stopped) {
        if (kill(-job.pid, SIGCONT) != 0 && errno == ESRCH) kill(job.pid, SIGCONT);
      }
    }
  }
}
void enter_subshell_environment() {
  state.exit_trap_active = false;
  ++state.subshell_level;
}
void run_pending_signal_trap() {
  int sig = pending_signal;
  if (!sig)
    return;
  pending_signal = 0;
  auto it = traps.find(sig);
  if (it != traps.end()) {
    const int previous_signal = state.trap_signal;
    state.trap_signal = sig;
    execute_line(it->second);
    state.trap_signal = previous_signal;
  }
}
bool shell_array_index_is_set(const std::string &name, size_t index) {
  auto array = state.arrays.find(name);
  if (array == state.arrays.end() || index >= array->second.size()) return false;
  auto indices = state.array_set_indices.find(name);
  return indices == state.array_set_indices.end() || indices->second.count(index) != 0;
}
size_t shell_array_index_extent(const std::string &name) {
  auto array = state.arrays.find(name);
  if (array == state.arrays.end()) return 0;
  auto indices = state.array_set_indices.find(name);
  if (indices == state.array_set_indices.end()) return array->second.size();
  size_t extent = 0;
  for (const size_t index : indices->second) extent = std::max(extent, index + 1);
  return extent;
}
void shell_array_mark_index_set(const std::string &name, size_t index) {
  auto &array = state.arrays[name];
  auto [indices, inserted] = state.array_set_indices.try_emplace(name);
  if (inserted) {
    for (size_t existing = 0; existing < array.size(); ++existing)
      indices->second.insert(existing);
  }
  indices->second.insert(index);
}
void shell_array_mark_all_set(const std::string &name) {
  auto array = state.arrays.find(name);
  auto &indices = state.array_set_indices[name];
  indices.clear();
  if (array != state.arrays.end())
    for (size_t index = 0; index < array->second.size(); ++index) indices.insert(index);
}
void shell_array_clear_set_indices(const std::string &name) {
  state.array_set_indices[name].clear();
}
bool shell_array_unset_index(const std::string &name, size_t index) {
  auto array = state.arrays.find(name);
  if (array == state.arrays.end() || index >= array->second.size()) return false;
  shell_array_mark_index_set(name, index);
  auto indices = state.array_set_indices.find(name);
  const bool was_set = indices != state.array_set_indices.end() && indices->second.erase(index) != 0;
  array->second[index].clear();
  return was_set;
}
bool set_shell_variable(const std::string &name, const std::string &value,
                        bool export_value) {
  std::string resolved_name = name;
  const size_t reference_bracket = name.find('[');
  const std::string reference_base = reference_bracket == std::string::npos
      ? name : name.substr(0, reference_bracket);
  if (state.namerefs.count(reference_base)) {
    if (reference_bracket == std::string::npos && state.namerefs[reference_base].empty())
      return shell_set_nameref(reference_base, value);
    resolved_name = shell_resolve_nameref(name);
    if (resolved_name.empty()) return false;
  }
  const std::string &assignment_name = resolved_name;
  if (assignment_name == "HISTCMD" && state.interactive_shell &&
      !state.disabled_special_parameters.count(assignment_name))
    return true;
  if (assignment_name == "BASH_ARGV0" &&
      !state.disabled_special_parameters.count(assignment_name)) {
    shell_name = value;
    vars[assignment_name] = value;
    state.unset_vars.erase(assignment_name);
    if (export_value || allexport || getenv(assignment_name.c_str()))
      return setenv(assignment_name.c_str(), value.c_str(), 1) == 0;
    return true;
  }
  if (assignment_name == "DIRSTACK" &&
      !state.disabled_special_parameters.count(assignment_name))
    return true;
  if (assignment_name == "BASHPID" &&
      !state.disabled_special_parameters.count(assignment_name))
    return true;
  if ((assignment_name == "GROUPS" || assignment_name.rfind("GROUPS[", 0) == 0) &&
      !state.disabled_special_parameters.count("GROUPS"))
    return true;
  if (assignment_name.rfind("DIRSTACK[", 0) == 0 &&
      !state.disabled_special_parameters.count("DIRSTACK")) {
    const size_t bracket = assignment_name.find('[');
    const std::string index_text = assignment_name.substr(
        bracket + 1, assignment_name.size() - bracket - 2);
    std::string index_error;
    const auto evaluated_index = evaluate_arithmetic(index_text, &index_error);
    if (!evaluated_index) return false;
    long long parsed = *evaluated_index;
    if (parsed < 0)
      parsed += static_cast<long long>(shell_array_index_extent("DIRSTACK"));
    if (parsed < 0) return false;
    // DIRSTACK[0] reflects the current directory and cannot change it.
    if (parsed == 0) return true;
    const size_t stack_index = static_cast<size_t>(parsed - 1);
    if (stack_index < state.dirs.size()) {
      state.dirs[stack_index] = value;
      state.arrays["DIRSTACK"][static_cast<size_t>(parsed)] = value;
      shell_array_mark_index_set("DIRSTACK", static_cast<size_t>(parsed));
    }
    return true;
  }
  if (assignment_name == "SECONDS" &&
      !state.disabled_special_parameters.count(assignment_name)) {
    char *end = nullptr;
    errno = 0;
    const long long assigned_value = std::strtoll(value.c_str(), &end, 10);
    state.seconds_offset = !errno && end != value.c_str() && *end == '\0'
                               ? assigned_value : 0;
    state.seconds_epoch = std::chrono::steady_clock::now();
    return true;
  }
  if (assignment_name == "RANDOM" &&
      !state.disabled_special_parameters.count(assignment_name)) {
    char *end = nullptr;
    errno = 0;
    const long long seed = std::strtoll(value.c_str(), &end, 10);
    state.random_seed = !errno && end != value.c_str() && *end == '\0'
                            ? static_cast<uint32_t>(seed) : 0U;
    vars[assignment_name] = value;
    return true;
  }
  if (assignment_name == "SRANDOM" &&
      !state.disabled_special_parameters.count(assignment_name))
    return true;
  if ((assignment_name == "EPOCHSECONDS" || assignment_name == "EPOCHREALTIME") &&
      !state.disabled_special_parameters.count(assignment_name))
    return true;
  const size_t bracket = assignment_name.find('[');
  if (bracket != std::string::npos && !assignment_name.empty() && assignment_name.back() == ']') {
    const std::string array_name = assignment_name.substr(0, bracket);
    std::string index_text = assignment_name.substr(bracket + 1, assignment_name.size() - bracket - 2);
    if (!name_ok(array_name) || index_text.empty()) return false;
    auto associative = state.associative_arrays.find(array_name);
    if (associative != state.associative_arrays.end()) {
      if (readonly_vars.count(array_name)) {
        std::cerr << array_name << ": readonly variable\n";
        return false;
      }
      std::string key;
      key.reserve(index_text.size());
      for (size_t i = 0; i < index_text.size(); ++i) {
        if (index_text[i] == '\x01' || index_text[i] == '\x02') continue;
        if (index_text[i] == '\x0b' && i + 1 < index_text.size())
          key += index_text[++i];
        else
          key += index_text[i];
      }
      std::string stored_value = value;
      if (state.integer_vars.count(array_name)) {
        std::string error;
        const auto numeric = evaluate_arithmetic(stored_value, &error);
        if (!numeric) {
          std::cerr << array_name << ": "
                    << (error.empty() ? "invalid arithmetic expression" : error) << '\n';
          return false;
        }
        stored_value = std::to_string(*numeric);
      }
      if (state.lowercase_vars.count(array_name))
        stored_value = locale_case_transform(stored_value, false);
      else if (state.uppercase_vars.count(array_name))
        stored_value = locale_case_transform(stored_value, true);
      associative->second[key] = std::move(stored_value);
      state.unset_vars.erase(array_name);
      return true;
    }
    std::string index_error;
    const auto evaluated_index = evaluate_arithmetic(index_text, &index_error);
    if (!evaluated_index || *evaluated_index < -1000000 || *evaluated_index > 1000000) {
      if (!index_error.empty())
        std::cerr << array_name << ": " << index_error << '\n';
      return false;
    }
    long long parsed = *evaluated_index;
    if (parsed < 0) {
      const size_t extent = shell_array_index_extent(array_name);
      if (static_cast<unsigned long long>(-parsed) > extent) return false;
      parsed += static_cast<long long>(extent);
    }
    const size_t index = static_cast<size_t>(parsed);
    auto &array = state.arrays[array_name];
    if (!state.array_set_indices.count(array_name)) shell_array_mark_all_set(array_name);
    if (array.size() <= index) array.resize(index + 1);
    std::string stored_value = value;
    if (state.integer_vars.count(array_name)) {
      std::string error;
      const auto numeric = evaluate_arithmetic(stored_value, &error);
      if (!numeric) {
        std::cerr << array_name << ": " << (error.empty() ? "invalid arithmetic expression" : error) << '\n';
        return false;
      }
      stored_value = std::to_string(*numeric);
    }
    if (state.lowercase_vars.count(array_name))
      stored_value = locale_case_transform(stored_value, false);
    else if (state.uppercase_vars.count(array_name))
      stored_value = locale_case_transform(stored_value, true);
    array[index] = std::move(stored_value);
    shell_array_mark_index_set(array_name, index);
    state.unset_vars.erase(array_name);
    return true;
  }
  if (!name_ok(assignment_name) || readonly_vars.count(assignment_name)) {
    if (readonly_vars.count(assignment_name))
      std::cerr << assignment_name << ": readonly variable\n";
    return false;
  }
  if (assignment_name == "PATH") {
    std::optional<std::string> previous_path;
    const auto stored_path = vars.find("PATH");
    if (stored_path != vars.end() && !state.unset_vars.count("PATH")) {
      previous_path = stored_path->second;
    } else if (!state.unset_vars.count("PATH")) {
      if (const char *environment_path = std::getenv("PATH"))
        previous_path = environment_path;
    }
    if (!previous_path || *previous_path != value) {
      state.command_hash.clear();
      state.command_hash_hits.clear();
    }
  }
  std::string stored_value = value;
  if (state.integer_vars.count(assignment_name)) {
    std::string error;
    const auto numeric = evaluate_arithmetic(stored_value, &error);
    if (!numeric) {
      std::cerr << assignment_name << ": " << (error.empty() ? "invalid arithmetic expression" : error) << '\n';
      return false;
    }
    stored_value = std::to_string(*numeric);
  }
  if (state.lowercase_vars.count(assignment_name))
    stored_value = locale_case_transform(stored_value, false);
  else if (state.uppercase_vars.count(assignment_name))
    stored_value = locale_case_transform(stored_value, true);
  vars[assignment_name] = stored_value;
  state.unset_vars.erase(assignment_name);
  if (assignment_name == "IGNOREEOF") {
    state.ignoreeof = true;
    refresh_option_parameter_values();
  }
  if (assignment_name == "OPTIND") {
    char *end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(stored_value.c_str(), &end, 10);
    getopts_index = !errno && end != stored_value.c_str() && *end == '\0' && parsed > 0
                        ? static_cast<size_t>(parsed) : 1;
    getopts_char = 1;
  }
  if (export_value || allexport || getenv(assignment_name.c_str()))
    return setenv(assignment_name.c_str(), stored_value.c_str(), 1) == 0;
  return true;
}
std::string shell_resolve_nameref(const std::string &name) {
  auto &shell = shell_state();
  std::string current = name;
  std::unordered_set<std::string> seen;
  while (true) {
    const size_t bracket = current.find('[');
    const std::string base = bracket == std::string::npos ? current : current.substr(0, bracket);
    const auto reference = shell.namerefs.find(base);
    if (reference == shell.namerefs.end()) return current;
    if (reference->second.empty() || !seen.insert(base).second) return {};
    current = reference->second + (bracket == std::string::npos ? std::string() : current.substr(bracket));
  }
}
bool shell_set_nameref(const std::string &name, const std::string &target) {
  auto &shell = shell_state();
  const size_t target_bracket = target.find('[');
  const bool valid_target = target.empty() || shell_valid_name(target) ||
      (target_bracket != std::string::npos && target_bracket > 0 &&
       target.back() == ']' && target_bracket + 1 < target.size() - 1 &&
       shell_valid_name(target.substr(0, target_bracket)));
  if (!shell_valid_name(name) || !valid_target ||
      shell.readonly_vars.count(name) || target == name)
    return false;
  const auto previous = shell.namerefs.find(name);
  const bool had_previous = previous != shell.namerefs.end();
  const std::string old_target = had_previous ? previous->second : std::string();
  shell.namerefs[name] = target;
  if (!target.empty() && shell_resolve_nameref(name).empty()) {
    if (had_previous) shell.namerefs[name] = old_target;
    else shell.namerefs.erase(name);
    return false;
  }
  shell.vars.erase(name);
  shell.unset_vars.erase(name);
  shell.arrays.erase(name);
  shell.array_set_indices.erase(name);
  shell.associative_arrays.erase(name);
  shell.integer_vars.erase(name);
  shell.lowercase_vars.erase(name);
  shell.uppercase_vars.erase(name);
  unsetenv(name.c_str());
  return true;
}
void set_shell_arguments(const std::string &name,
                         const std::vector<std::string> &arguments) {
  shell_name = name;
  state.last_argument = name;
  int version_major = 0, version_minor = 0, version_patch = 0;
  (void)std::sscanf(DAX_VERSION, "%d.%d.%d", &version_major,
                    &version_minor, &version_patch);
  vars["DAX_VERSION"] = DAX_VERSION;
  vars["BASH_VERSION"] = std::string("Dax ") + DAX_VERSION;
  state.readonly_vars["BASH_VERSION"] = "1";
  state.arrays["BASH_VERSINFO"] = {
      std::to_string(version_major), std::to_string(version_minor),
      std::to_string(version_patch), "0", "dax",
      state.vars.count("MACHTYPE") ? state.vars["MACHTYPE"] : "unknown"};
  shell_array_mark_all_set("BASH_VERSINFO");
  state.readonly_vars["BASH_VERSINFO"] = "1";
  state.vars.erase("BASH_VERSINFO");
  state.unset_vars.erase("BASH_VERSION");
  state.unset_vars.erase("BASH_VERSINFO");
  vars["BASH_ARGV0"] = name;
  state.unset_vars.erase("BASH_ARGV0");
  positionals = arguments;
  vars["OPTIND"] = "1";
  vars["OPTERR"] = "1";
  vars.erase("OPTARG");
  setenv("OPTIND", "1", 1);
  unsetenv("OPTARG");
  state.source_stack.clear();
  state.script_source = (!state.interactive_shell && !state.command_string_mode &&
                         !state.stdin_mode)
                            ? name : std::string();
  state.function_stack.clear();
  state.function_frame_flags.clear();
  state.function_source_stack.clear();
  state.function_call_lines.clear();
  state.function_call_sources.clear();
  state.current_lineno = 1;
  refresh_stack_variables();
  getopts_index = 1;
  getopts_char = 1;
}
bool set_shell_option(const std::string &name, bool enabled) {
  if (name == "privileged") {
    if (!enabled && (setegid(getgid()) != 0 || seteuid(getuid()) != 0))
      return false;
    state.privileged = enabled;
  }
  else if (name == "errexit" || name == "ERR_EXIT")
    errexit = enabled;
  else if (name == "errtrace" || name == "ERRTRACE")
    state.errtrace = enabled;
  else if (name == "functrace" || name == "FUNCTRACE")
    state.functrace = enabled;
  else if (name == "nounset" || name == "NO_UNSET")
    nounset = enabled;
  else if (name == "allexport" || name == "ALL_EXPORT")
    allexport = enabled;
  else if (name == "pipefail")
    state.pipefail = enabled;
  else if (name == "notify" || name == "notify_jobs")
    notify_jobs = enabled;
  else if (name == "noclobber")
    noclobber = enabled;
  else if (name == "nolog")
    state.nolog = enabled;
  else if (name == "noglob")
    noglob = enabled;
  else if (name == "monitor")
    monitor = enabled;
  else if (name == "keyword" || name == "keyword_assignments")
    state.keyword_assignments = enabled;
  else if (name == "physical") {
    state.physical_directory = enabled;
    if (enabled) {
      char physical_pwd[4096];
      if (getcwd(physical_pwd, sizeof(physical_pwd))) {
        state.vars["PWD"] = physical_pwd;
        setenv("PWD", physical_pwd, 1);
      }
    }
  }
  else if (name == "onecmd")
    state.one_command = enabled;
  else if (name == "noexec")
    noexec = enabled;
  else if (name == "verbose")
    verbose = enabled;
  else if (name == "xtrace")
    xtrace = enabled;
  else if (name == "hashall")
    state.hashall = enabled;
  else if (name == "braceexpand")
    state.braceexpand = enabled;
  else if (name == "histexpand")
    state.histexpand = enabled;
  else if (name == "history")
    state.history = enabled;
  else if (name == "posix")
    state.posix_mode = enabled;
  else if (name == "emacs") {
    state.emacs_mode = enabled;
    if (enabled) state.vi_mode = false;
  }
  else if (name == "vi") {
    state.vi_mode = enabled;
    if (enabled) state.emacs_mode = false;
  }
  else if (name == "ignoreeof") {
    const bool was_exported = std::getenv("IGNOREEOF") != nullptr;
    state.ignoreeof = enabled;
    if (enabled) {
      state.vars["IGNOREEOF"] = "10";
      state.unset_vars.erase("IGNOREEOF");
      if (was_exported || state.allexport) setenv("IGNOREEOF", "10", 1);
    } else {
      state.vars.erase("IGNOREEOF");
      state.unset_vars.insert("IGNOREEOF");
      unsetenv("IGNOREEOF");
    }
  }
  else if (name == "extglob")
    state.extglob = enabled;
  else if (name == "nullglob")
    state.nullglob = enabled;
  else if (name == "failglob")
    state.failglob = enabled;
  else if (name == "dotglob")
    state.dotglob = enabled;
  else if (name == "globstar")
    state.globstar = enabled;
  else if (name == "globskipdots")
    state.globskipdots = enabled;
  else if (name == "globasciiranges")
    state.globasciiranges = enabled;
  else if (name == "nocaseglob")
    state.nocaseglob = enabled;
  else if (name == "nocasematch")
    state.nocasematch = enabled;
  else if (name == "sourcepath")
    state.sourcepath = enabled;
  else if (name == "expand_aliases")
    state.expand_aliases = enabled;
  else if (name == "patsub_replacement")
    state.patsub_replacement = enabled;
  else if (name == "promptvars")
    state.promptvars = enabled;
  else
    return false;
  if (name == "posix" && enabled && !state.vars.count("POSIXLY_CORRECT") &&
      !std::getenv("POSIXLY_CORRECT"))
    set_shell_variable("POSIXLY_CORRECT", "y", true);
  refresh_option_parameter_values();
  return true;
}
bool shell_option_enabled(const std::string &name) {
  if (name == "privileged") return state.privileged;
  if (name == "errexit" || name == "ERR_EXIT") return errexit;
  if (name == "errtrace" || name == "ERRTRACE") return state.errtrace;
  if (name == "functrace" || name == "FUNCTRACE") return state.functrace;
  if (name == "nounset" || name == "NO_UNSET") return nounset;
  if (name == "allexport" || name == "ALL_EXPORT") return allexport;
  if (name == "pipefail") return state.pipefail;
  if (name == "notify" || name == "notify_jobs") return notify_jobs;
  if (name == "noclobber") return noclobber;
  if (name == "nolog") return state.nolog;
  if (name == "noglob") return noglob;
  if (name == "monitor") return monitor;
  if (name == "keyword" || name == "keyword_assignments") return state.keyword_assignments;
  if (name == "physical") return state.physical_directory;
  if (name == "onecmd") return state.one_command;
  if (name == "noexec") return noexec;
  if (name == "verbose") return verbose;
  if (name == "xtrace") return xtrace;
  if (name == "hashall") return state.hashall;
  if (name == "braceexpand") return state.braceexpand;
  if (name == "histexpand") return state.histexpand;
  if (name == "history") return state.history;
  if (name == "posix") return state.posix_mode;
  if (name == "emacs") return state.emacs_mode;
  if (name == "vi") return state.vi_mode;
  if (name == "ignoreeof") return state.ignoreeof;
  if (name == "extglob") return state.extglob;
  if (name == "nullglob") return state.nullglob;
  if (name == "failglob") return state.failglob;
  if (name == "dotglob") return state.dotglob;
  if (name == "globstar") return state.globstar;
  if (name == "globskipdots") return state.globskipdots;
  if (name == "globasciiranges") return state.globasciiranges;
  if (name == "nocaseglob") return state.nocaseglob;
  if (name == "nocasematch") return state.nocasematch;
  if (name == "sourcepath") return state.sourcepath;
  if (name == "expand_aliases") return state.expand_aliases || state.posix_mode;
  if (name == "patsub_replacement") return state.patsub_replacement;
  if (name == "xpg_echo") return state.xpg_echo;
  if (name == "checkwinsize") return state.checkwinsize;
  if (name == "checkhash") return state.checkhash;
  if (name == "checkjobs") return state.checkjobs;
  if (name == "promptvars") return state.promptvars;
  if (name == "cdable_vars") return state.cdable_vars;
  if (name == "cdspell") return state.cdspell;
  if (name == "autocd") return state.autocd;
  if (name == "execfail") return state.execfail;
  if (name == "localvar_inherit") return state.localvar_inherit;
  if (name == "localvar_unset") return state.localvar_unset;
  if (name == "interactive_comments") return state.interactive_comments;
  if (name == "progcomp") return state.progcomp;
  if (name == "no_empty_cmd_completion") return state.no_empty_cmd_completion;
  if (name == "cmdhist") return state.cmdhist;
  if (name == "lithist") return state.lithist;
  return false;
}
namespace {
std::string enabled_option_list(const std::vector<std::string> &options,
                                bool shopt_options) {
  std::vector<std::string> enabled;
  for (const auto &name : options) {
    const bool is_enabled = shopt_options && name == "huponexit" ? state.huponexit
                            : shopt_options && name == "inherit_errexit" ? state.inherit_errexit
                            : shopt_options && name == "lastpipe" ? state.lastpipe
                            : shopt_options && name == "shift_verbose" ? state.shift_verbose
                            : shopt_options && name == "varredir_close" ? state.varredir_close
                            : shell_option_enabled(name);
    if (is_enabled) enabled.push_back(name);
  }
  std::sort(enabled.begin(), enabled.end());
  std::string result;
  for (const auto &name : enabled) {
    if (!result.empty()) result += ':';
    result += name;
  }
  return result;
}
}
std::string shell_enabled_options() {
  static const std::vector<std::string> options = {
      "allexport", "notify", "noclobber", "errexit", "errtrace", "functrace", "pipefail", "noglob",
      "monitor", "noexec", "nounset", "verbose", "xtrace", "hashall", "history", "keyword", "nolog", "physical", "onecmd", "ignoreeof",
      "braceexpand", "histexpand", "emacs", "vi", "posix", "privileged"};
  return enabled_option_list(options, false);
}
std::string shell_enabled_shopt_options() {
  static const std::vector<std::string> options = {
      "dotglob", "extglob", "failglob", "globasciiranges", "globskipdots", "globstar", "huponexit",
      "inherit_errexit", "lastpipe", "nocaseglob", "nocasematch",
      "nullglob", "patsub_replacement", "sourcepath", "expand_aliases", "shift_verbose",
      "varredir_close", "xpg_echo", "checkwinsize", "checkhash", "checkjobs", "promptvars",
      "cdable_vars", "cdspell", "autocd", "execfail", "localvar_inherit",
      "localvar_unset", "interactive_comments", "progcomp",
      "no_empty_cmd_completion", "cmdhist", "lithist", "histappend"};
  return enabled_option_list(options, true);
}
void refresh_option_parameter_values() {
  const bool inherited_options_exported = !state.ignore_inherited_environment;
  const bool export_shellopts = state.allexport ||
      (inherited_options_exported && std::getenv("SHELLOPTS") != nullptr);
  const bool export_bashopts = state.allexport ||
      (inherited_options_exported && std::getenv("BASHOPTS") != nullptr);
  vars["SHELLOPTS"] = shell_enabled_options();
  vars["BASHOPTS"] = shell_enabled_shopt_options();
  if (export_shellopts) setenv("SHELLOPTS", vars["SHELLOPTS"].c_str(), 1);
  if (export_bashopts) setenv("BASHOPTS", vars["BASHOPTS"].c_str(), 1);
}
void set_shell_modes(bool login, bool interactive, bool monitor_was_explicit,
                     bool histexpand_was_explicit, bool command_string,
                     bool stdin_input, const std::string &execution_string,
                     bool history_was_explicit) {
  login_shell = login;
  interactive_shell = interactive;
  bool inherited_history = false;
  if (!state.ignore_inherited_environment) {
    if (const char *options = std::getenv("SHELLOPTS")) {
      const std::string value(options);
      size_t begin = 0;
      while (begin <= value.size()) {
        const size_t end = value.find(':', begin);
        if (value.substr(begin, end == std::string::npos ? std::string::npos : end - begin) == "history") {
          inherited_history = true;
          break;
        }
        if (end == std::string::npos) break;
        begin = end + 1;
      }
    }
  }
  if (!history_was_explicit) state.history = interactive || inherited_history;
  state.command_string_mode = command_string;
  state.stdin_mode = stdin_input;
  state.execution_string = command_string ? execution_string : std::string{};
  if (command_string) {
    vars["BASH_EXECUTION_STRING"] = execution_string;
    state.unset_vars.erase("BASH_EXECUTION_STRING");
  } else {
    vars.erase("BASH_EXECUTION_STRING");
  }
  if (interactive) state.expand_aliases = true;
  if (!monitor_was_explicit && !monitor) monitor = interactive;
  if (!histexpand_was_explicit && !state.histexpand)
    state.histexpand = interactive;
  refresh_option_parameter_values();
}
bool shell_histexpand_enabled() { return state.histexpand; }
bool shell_noexec_enabled() { return noexec; }
bool shell_verbose_enabled() { return verbose; }
bool shell_xtrace_enabled() { return xtrace; }
bool shell_noglob_enabled() { return noglob; }
bool shell_noclobber_enabled() { return noclobber; }
std::optional<std::string> shell_expand_prompt_variable(const std::string &name) {
  if (state.unset_vars.count(name)) return std::string();
  const auto variable = state.vars.find(name);
  const char *environment_value = variable == state.vars.end()
      ? std::getenv(name.c_str()) : nullptr;
  if (variable == state.vars.end() && !environment_value) return {};

  const bool previous_ps4_guard = ps4_expansion_active;
  if (name == "PS4") ps4_expansion_active = true;
  std::vector<std::string> words = {std::string("\x02$") + "{" + name + "@P}\x02"};
  begin_word_expansion();
  expand_words(words, WordExpansionMode::SingleWord);
  if (!word_expansion_succeeded() || words.empty()) {
    ps4_expansion_active = previous_ps4_guard;
    return std::string();
  }
  if (!state.promptvars) {
    ps4_expansion_active = previous_ps4_guard;
    return words.front();
  }

  std::string second_pass = words.front();
  for (char &ch : second_pass) {
    if (ch == '\x01') ch = '\x11';
    else if (ch == '\x02') ch = '\x12';
  }
  words = {std::string("\x02") + second_pass + "\x02"};
  begin_word_expansion();
  expand_words(words, WordExpansionMode::SingleWord);
  if (!word_expansion_succeeded() || words.empty()) {
    ps4_expansion_active = previous_ps4_guard;
    return std::string();
  }
  for (char &ch : words.front()) {
    if (ch == '\x11') ch = '\x01';
    else if (ch == '\x12') ch = '\x02';
  }
  ps4_expansion_active = previous_ps4_guard;
  return words.front();
}
void trace_shell_command(const std::vector<std::string> &args) {
  if (!xtrace || ps4_expansion_active) return;
  std::string prefix = "+ ";
  if (auto configured = shell_expand_prompt_variable("PS4")) prefix = *configured;
  prefix.erase(std::remove_if(prefix.begin(), prefix.end(), [](char ch) {
    return ch == '\x01' || ch == '\x02';
  }), prefix.end());
  const size_t depth = state.function_stack.size() + state.subshell_level;
  if (!prefix.empty()) {
    const char first = prefix.front();
    prefix = std::string(depth + 1, first) + prefix.substr(1);
  }
  std::cerr << prefix;
  const auto trace_argument = [](const std::string &argument) {
    if (!argument.empty() &&
        std::all_of(argument.begin(), argument.end(), [](unsigned char ch) {
          return ch >= 0x80 || std::isalnum(ch) ||
                 std::string("_@%+=:,./-").find(static_cast<char>(ch)) != std::string::npos;
        }))
      return argument;
    std::string quoted = "'";
    for (char ch : argument) {
      if (ch == '\'') quoted += "'\\''";
      else quoted += ch;
    }
    return quoted + "'";
  };
  for (size_t i = 0; i < args.size(); ++i) {
    if (i) std::cerr << ' ';
    std::cerr << trace_argument(args[i]);
  }
  std::cerr << '\n';
}
bool shell_variable_readonly(const std::string &name) {
  return readonly_vars.count(name) != 0;
}
void begin_word_expansion() { expansion_error = false; expansion_error_reported = false; expansion_error_index = 0; substitution_status = 0; substitution_ran = false; }
bool word_expansion_succeeded() { return !expansion_error; }
bool word_expansion_error_was_reported() { return expansion_error_reported; }
bool word_expansion_ran_substitution() { return substitution_ran; }
int word_expansion_substitution_status() { return substitution_status; }
size_t word_expansion_error_index() { return expansion_error_index; }
void apply_errexit(int status) {
  if (status != 0 && errexit_immune_status) return;
  if (errexit_ignored_context > 0) return;
  if (errexit && status != 0) {
    exiting = true;
    exit_value = status & 255;
  }
}
void begin_errexit_ignored_context() {
  ++errexit_ignored_context;
  ++state.err_trap_ignored_context;
}
void end_errexit_ignored_context() {
  if (errexit_ignored_context > 0) --errexit_ignored_context;
  if (state.err_trap_ignored_context > 0) --state.err_trap_ignored_context;
}
void begin_errexit_only_ignored_context() { ++errexit_ignored_context; }
void end_errexit_only_ignored_context() {
  if (errexit_ignored_context > 0) --errexit_ignored_context;
}
void mark_errexit_immune() { errexit_immune_status = true; }
void clear_errexit_immune() { errexit_immune_status = false; }
