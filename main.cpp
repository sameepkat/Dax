#include "builtins.hpp"
#include "helper.hpp"
#include "janet_runtime.hpp"
#include "parser.hpp"
#include "signal_handler.hpp"
#include "tokenizer.hpp"
#include <version.hpp>
#include <csignal>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <clocale>
#include <iostream>
#include <fstream>
#include <readline/history.h>
#include <readline/readline.h>
#include <sstream>
#include <string>
#include <utility>
#include <unistd.h>
#include <vector>

namespace {
std::vector<std::string> completion_items;
size_t completion_index = 0;
bool renderer_started = false;
size_t renderer_cursor_row = 0;
size_t renderer_input_row = 0;
size_t renderer_input_column = 0;

char *completion_generator(const char *, int state) {
  if (state == 0) completion_index = 0;
  if (completion_index >= completion_items.size()) return nullptr;
  return ::strdup(completion_items[completion_index++].c_str());
}

char **dax_attempted_completion(const char *text, int start, int) {
  const std::string word(text ? text : "");
  const std::string before(rl_line_buffer, static_cast<size_t>(std::max(start, 0)));
  const size_t command_boundary = before.find_last_of(";|&\n");
  std::string command_prefix = command_boundary == std::string::npos
                                   ? before : before.substr(command_boundary + 1);
  const bool command_position = std::all_of(command_prefix.begin(), command_prefix.end(),
      [](unsigned char ch) { return std::isspace(ch); });
  bool variable_name = !word.empty() && word[0] == '$';
  std::string candidate_prefix = variable_name ? word.substr(1) : word;
  std::string directory_prefix;
  if (!command_position && !variable_name) {
    size_t token_start = before.find_last_of(" \t\n;|&");
    const std::string preceding_token = token_start == std::string::npos
        ? before : before.substr(token_start + 1);
    if (preceding_token.find('/') != std::string::npos)
      directory_prefix = preceding_token.substr(0, preceding_token.find_last_of('/') + 1);
  }
  completion_items = shell_completion_candidates(candidate_prefix, command_position,
                                                  variable_name, directory_prefix);
  if (variable_name)
    for (auto &candidate : completion_items) candidate.insert(candidate.begin(), '$');
  if (!completion_items.empty()) {
    rl_attempted_completion_over = 1;
    rl_completion_append_character = completion_items.size() == 1 &&
        (completion_items.front().empty() || completion_items.front().back() != '/') ? ' ' : '\0';
    return rl_completion_matches(word.c_str(), completion_generator);
  }
  if (variable_name) {
    rl_attempted_completion_over = 1;
    return nullptr;
  }
  rl_attempted_completion_over = 0;
  return nullptr;
}

std::string current_suggestion() {
  if (!rl_line_buffer || rl_point != rl_end || rl_end <= 0) return {};
  const std::string current(rl_line_buffer, static_cast<size_t>(rl_end));
  const auto &history = History::get_all();
  for (auto it = history.rbegin(); it != history.rend(); ++it) {
    if (it->size() > current.size() && it->compare(0, current.size(), current) == 0)
      return it->substr(current.size());
  }
  return {};
}

bool colors_enabled() {
  return janet_colorscheme() != "no-color" && std::getenv("NO_COLOR") == nullptr;
}

std::string color_code(const char *role) {
  if (!colors_enabled()) return {};
  const bool contrast = janet_colorscheme() == "high-contrast";
  if (std::strcmp(role, "valid-command") == 0) return contrast ? "\033[1;92m" : "\033[32m";
  if (std::strcmp(role, "invalid-command") == 0) return contrast ? "\033[1;91m" : "\033[31m";
  if (std::strcmp(role, "suggestion") == 0) return contrast ? "\033[1;37m" : "\033[90m";
  if (std::strcmp(role, "prompt") == 0) return contrast ? "\033[1;92m" : "\033[1;32m";
  if (std::strcmp(role, "git-branch") == 0) return contrast ? "\033[1;96m" : "\033[36m";
  return {};
}

std::string current_git_branch() {
  FILE *git = popen("git symbolic-ref --quiet --short HEAD 2>/dev/null", "r");
  if (!git) return {};
  char buffer[256];
  std::string branch;
  if (fgets(buffer, sizeof(buffer), git)) branch = buffer;
  const int status = pclose(git);
  if (status != 0) return {}; // Outside a repository or in detached HEAD state.
  while (!branch.empty() && (branch.back() == '\n' || branch.back() == '\r')) branch.pop_back();
  return branch;
}

std::string command_word_value(const std::string &word) {
  std::string value;
  for (size_t i = 0; i < word.size(); ++i) {
    if (word[i] == '\x01' || word[i] == '\x02') continue;
    if (word[i] == '\\' && i + 1 < word.size()) { value += word[++i]; continue; }
    value += word[i];
  }
  return value;
}

struct CommandColorSpan { size_t begin; size_t end; std::string color; };

std::vector<CommandColorSpan> command_color_spans(const std::string &line) {
  std::vector<CommandColorSpan> spans;
  std::vector<ShellToken> tokens;
  try { tokens = tokenize(line); } catch (...) { return spans; }
  bool command_expected = true;
  bool for_header = false;
  bool case_header = false;
  bool skip_redirection_target = false;
  for (const auto &token : tokens) {
    const std::string word = static_cast<const std::string &>(token);
    if (skip_redirection_target) { skip_redirection_target = false; continue; }
    if (word == "<" || word == ">" || word == ">>" || word == "2>" ||
        word == "2>>" || word == "&>" || word == "&>>" || word == "<<" || word == "<<-") {
      skip_redirection_target = true;
      continue;
    }
    if (for_header) {
      if (word == "do") { for_header = false; command_expected = true; }
      continue;
    }
    if (case_header) {
      if (word == "in") case_header = false;
      continue;
    }
    if (word == ";" || word == "&&" || word == "||" || word == "|" || word == "&" || word == "\n") {
      command_expected = true;
      continue;
    }
    if (!command_expected) continue;
    if (word == "for") { for_header = true; continue; }
    if (word == "case") { case_header = true; continue; }
    if (word == "if" || word == "then" || word == "else" || word == "elif" ||
        word == "while" || word == "until" || word == "do" || word == "!" ||
        word == "in" || word == "esac") continue;
    const std::string value = command_word_value(word);
    if (value.empty()) continue;
    size_t equal = value.find('=');
    if (equal != std::string::npos && equal > 0 &&
        (std::isalpha(static_cast<unsigned char>(value[0])) || value[0] == '_') &&
        std::all_of(value.begin() + 1, value.begin() + equal, [](unsigned char c) { return std::isalnum(c) || c == '_'; }))
      continue;
    if (value.find('$') != std::string::npos || value.find('`') != std::string::npos) {
      command_expected = false;
      continue;
    }
    const std::string color = shell_command_resolves(value) ? color_code("valid-command") : color_code("invalid-command");
    if (!color.empty()) spans.push_back({token.source_begin, token.source_end, color});
    command_expected = false;
  }
  return spans;
}

std::string marked_text(const std::string &text) {
  if (text.empty()) return {};
  return std::string(1, '\001') + text + std::string(1, '\002');
}

void append_cursor_command(std::string &output, char command, size_t count) {
  if (count) output += "\033[" + std::to_string(count) + command;
}

void append_display_text(std::string &output, const std::string &text,
                         int &row, int &column, int width, bool prompt = false) {
  bool nonprinting = false;
  std::mbstate_t state{};
  for (size_t i = 0; i < text.size();) {
    const unsigned char byte = static_cast<unsigned char>(text[i]);
    if (prompt && (byte == '\001' || byte == '\002')) {
      nonprinting = byte == '\001'; ++i; continue;
    }
    if (nonprinting) { output += text[i++]; continue; }
    if (byte == 0x1b && i + 1 < text.size() && text[i + 1] == '[') {
      size_t end = i + 2;
      while (end < text.size() && !(text[end] >= '@' && text[end] <= '~')) ++end;
      if (end < text.size()) { output.append(text, i, end - i + 1); i = end + 1; continue; }
    }
    if (byte == '\n') { output += "\r\n"; ++row; column = 0; ++i; state = {}; continue; }
    if (byte < 0x20 || byte == 0x7f) {
      output += '^'; output += byte == 0x7f ? '?' : static_cast<char>(byte + '@');
      column += 2; ++i; state = {}; continue;
    }
    wchar_t character = 0;
    const size_t bytes = std::mbrtowc(&character, text.data() + i, text.size() - i, &state);
    size_t consumed = bytes;
    int cells = 1;
    if (bytes == static_cast<size_t>(-1) || bytes == static_cast<size_t>(-2)) {
      state = {}; consumed = 1;
    } else if (bytes == 0) { consumed = 1; cells = 0; }
    else cells = std::max(::wcwidth(character), 0);
    if (cells && column + cells > width) { output += "\r\n"; ++row; column = 0; }
    output.append(text, i, consumed); i += consumed; column += cells;
    if (column >= width) { output += "\r\n"; ++row; column = 0; }
  }
}

void dax_redisplay() {
  if (!rl_line_buffer || !rl_prompt) return;
  std::string output;
  int rows = 0, width = 80;
  rl_get_screen_size(&rows, &width);
  if (width <= 0) width = 80;
  int row = 0, column = 0;
  if (renderer_started) {
    append_cursor_command(output, 'A', renderer_cursor_row - renderer_input_row);
    output += '\r';
    append_cursor_command(output, 'C', renderer_input_column);
    output += "\033[J";
    row = static_cast<int>(renderer_input_row);
    column = static_cast<int>(renderer_input_column);
  } else {
    output += "\r\033[J";
    append_display_text(output, rl_prompt, row, column, width, true);
    renderer_input_row = static_cast<size_t>(row);
    renderer_input_column = static_cast<size_t>(column);
    renderer_started = true;
  }
  const std::string line(rl_line_buffer, static_cast<size_t>(std::max(rl_end, 0)));
  const auto spans = command_color_spans(line);
  size_t span_index = 0, target_row = 0, target_column = 0;
  bool target_saved = false;
  const size_t point = static_cast<size_t>(std::clamp(rl_point, 0, rl_end));
  for (size_t i = 0; i <= line.size();) {
    if (span_index < spans.size() && spans[span_index].begin == i && colors_enabled())
      output += spans[span_index].color;
    if (!target_saved && i == point) { target_row = row; target_column = column; target_saved = true; }
    if (i == line.size()) break;
    const unsigned char byte = static_cast<unsigned char>(line[i]);
    size_t consumed = 1;
    int cells = 1;
    if (line[i] == '\n') {
      output += "\r\n"; ++row; column = 0; cells = 0;
    } else if (byte < 0x20 || byte == 0x7f) {
      output += '^'; output += byte == 0x7f ? '?' : static_cast<char>(byte + '@'); column += 2; cells = 0;
    } else {
      wchar_t character = 0;
      std::mbstate_t state{};
      const size_t bytes = std::mbrtowc(&character, line.data() + i, line.size() - i, &state);
      if (bytes != static_cast<size_t>(-1) && bytes != static_cast<size_t>(-2) && bytes != 0) {
        consumed = bytes;
        cells = std::max(::wcwidth(character), 0);
      }
      if (cells && column + cells > width) { output += "\r\n"; ++row; column = 0; }
      output.append(line, i, consumed);
      column += cells;
    }
    const size_t next = i + consumed;
    if (span_index < spans.size() && spans[span_index].end == next) {
      if (colors_enabled()) output += "\033[0m";
      ++span_index;
    }
    if (column >= width) { output += "\r\n"; ++row; column = 0; }
    i = next;
  }
  const std::string suggestion = current_suggestion();
  if (!suggestion.empty()) {
    output += color_code("suggestion");
    output += suggestion;
    if (colors_enabled()) output += "\033[0m";
  }
  output += "\033[J";
  append_cursor_command(output, 'A', static_cast<size_t>(row) - target_row);
  output += '\r';
  append_cursor_command(output, 'C', target_column);
  renderer_cursor_row = target_row;
  std::fwrite(output.data(), 1, output.size(), rl_outstream);
  std::fflush(rl_outstream);
}

int accept_suggestion(int count, int key) {
  const std::string suggestion = current_suggestion();
  if (suggestion.empty()) return rl_forward_char(count, key);
  rl_insert_text(suggestion.c_str());
  return 0;
}

void configure_interactive_readline() {
  std::setlocale(LC_CTYPE, "");
  rl_attempted_completion_function = dax_attempted_completion;
  rl_redisplay_function = dax_redisplay;
  // GNU Readline's menu completion inserts the next candidate on each Tab
  // press instead of printing the full candidate list.
  rl_bind_key('\t', rl_menu_complete);
  rl_bind_keyseq("\e[C", accept_suggestion);
  rl_bind_keyseq("\eOC", accept_suggestion);
  rl_bind_keyseq("\e[F", accept_suggestion);
  rl_bind_keyseq("\eOF", accept_suggestion);
  rl_bind_keyseq("\e[4~", accept_suggestion);
}
} // namespace

void execute_line(const std::string &line) {
  if (shell_verbose_enabled()) {
    std::istringstream input(line);
    std::string source_line;
    while (std::getline(input, source_line)) std::cerr << source_line << '\n';
  }
  auto tokens = tokenize(line);
  if (tokens.empty())
    return;
  static std::string pending_name;
  static std::string pending_body_source;
  size_t function_end = 0;
  auto finish_pending_function = [&](size_t start) {
    for (size_t i = start; i < tokens.size(); ++i) {
      if (tokens[i] == "}") {
        if (!pending_body_source.empty()) pending_body_source += '\n';
        pending_body_source += line.substr(0, tokens[i].source_begin);
        if (!shell_noexec_enabled())
          define_function(pending_name, pending_body_source);
        pending_name.clear();
        pending_body_source.clear();
        function_end = i + 1;
        return true;
      }
    }
    if (!pending_body_source.empty()) pending_body_source += '\n';
    pending_body_source += line;
    return false;
  };
  if (!pending_name.empty()) {
    if (!finish_pending_function(0)) return;
    tokens.erase(tokens.begin(), tokens.begin() + function_end);
    while (!tokens.empty() && tokens.front() == ";") tokens.erase(tokens.begin());
  }
  for (size_t i = 0; i < tokens.size();) {
    size_t name_index = i, search_start = i;
    bool declaration = false;
    if (tokens[i] == "function" && i + 1 < tokens.size()) {
      name_index = i + 1;
      search_start = i + 2;
      declaration = true;
    } else if (i + 2 < tokens.size() && tokens[i + 1] == "(" && tokens[i + 2] == ")") {
      search_start = i + 3;
      declaration = true;
    }
    if (!declaration) { ++i; continue; }
    size_t open = search_start;
    while (open < tokens.size() && tokens[open] != "{") ++open;
    if (open == tokens.size()) { ++i; continue; }
    size_t close = open + 1;
    while (close < tokens.size() && tokens[close] != "}") ++close;
    if (close == tokens.size()) {
      pending_name = tokens[name_index];
      pending_body_source = std::string(tokens[open].line - 1, '\n') +
                            std::string(tokens[open].column, ' ') +
                            line.substr(tokens[open].source_end);
      tokens.erase(tokens.begin() + i, tokens.end());
      break;
    }
    std::string body;
    if (close > open + 1) {
      const auto &first_body_token = tokens[open + 1];
      const auto &last_body_token = tokens[close - 1];
      if (last_body_token.source_end >= first_body_token.source_begin &&
          last_body_token.source_end <= line.size()) {
        body = line.substr(first_body_token.source_begin,
                           last_body_token.source_end - first_body_token.source_begin);
        body.insert(0, std::string(first_body_token.line - 1, '\n') +
                           std::string(first_body_token.column - 1, ' '));
      }
    }
    if (body.empty()) {
      for (size_t j = open + 1; j < close; ++j) {
        if (!body.empty()) body += ' ';
        body += tokens[j];
      }
    }
    if (!shell_noexec_enabled())
      define_function(tokens[name_index], body);
    tokens.erase(tokens.begin() + i, tokens.begin() + close + 1);
    while (i < tokens.size() && tokens[i] == ";") tokens.erase(tokens.begin() + i);
  }
  if (tokens.empty()) return;
  expand_aliases(tokens);
  try {
    auto ast = parse(tokens, line);
    const int status = shell_noexec_enabled() ? 0 : (ast ? ast->execute() : 0);
    set_last_status(status);
    apply_errexit(status);
    clear_errexit_immune();
  } catch (const std::exception &e) {
    std::cerr << "Parse/Exec Error: " << e.what() << "\n";
    set_last_status(2);
  }
  run_pending_signal_trap();
}

static bool shell_input_incomplete(const std::string &source) {
  const auto tokens = tokenize(source);
  int if_depth = 0, loop_depth = 0, paren_depth = 0, brace_depth = 0;
  bool command_start = true;
  bool missing_heredoc = false;
  for (const auto &token : tokens) {
    if (token.size() == 1 && token[0] == '\x06') missing_heredoc = true;
    if (token == "if" && command_start) ++if_depth;
    else if (token == "fi" && if_depth > 0) --if_depth;
    if ((token == "for" || token == "while" || token == "until") && command_start) ++loop_depth;
    else if (token == "done" && loop_depth > 0) --loop_depth;
    if (token == "(") ++paren_depth;
    else if (token == ")" && paren_depth > 0) --paren_depth;
    if (token == "{") ++brace_depth;
    else if (token == "}" && brace_depth > 0) --brace_depth;
    command_start = token == ";" || token == "&" || token == "&&" || token == "||" ||
                    token == "|" || token == "then" || token == "do" || token == "else" || token == "elif";
  }
  std::string token_text;
  for (const auto &token : tokens) token_text += token;
  const auto single_quotes = std::count(token_text.begin(), token_text.end(), '\x01');
  const auto double_quotes = std::count(token_text.begin(), token_text.end(), '\x02');
  const auto command_substitutions = std::count(token_text.begin(), token_text.end(), '\x03');
  bool open_operator = !tokens.empty() &&
      (tokens.back() == "&&" || tokens.back() == "||" || tokens.back() == "|");
  return if_depth > 0 || loop_depth > 0 || paren_depth > 0 || brace_depth > 0 ||
         (single_quotes % 2) || (double_quotes % 2) || (command_substitutions % 2) ||
         missing_heredoc || open_operator;
}

enum class CliMode { Interactive, Command, Script, Stdin, Janet, RestrictedJanet };

struct CliInvocation {
  CliMode mode = CliMode::Interactive;
  bool show_help = false;
  bool show_version = false;
  bool shell_option_seen = false;
  bool login = false;
  bool force_interactive = false;
  bool has_operands = false;
  bool monitor_option_seen = false;
  std::string command;
  std::string shell_name = "dax";
  std::string script_path;
  std::string workspace;
  std::vector<std::string> arguments;
};

static void print_usage(std::ostream &out) {
  out << "Usage: dax [options] [script [args...]]\n"
         "       dax [options] -c COMMAND [NAME [ARG...]]\n"
         "       dax [options] -s [ARG...]\n"
         "       dax --script FILE [ARG...]\n"
         "       dax --janet FILE [ARG...]\n"
         "       dax --janet-restricted WORKSPACE FILE [ARG...]\n\n"
         "Options:\n"
         "  --help, --version       Show help or version and exit\n"
         "  --                      Stop processing startup options\n"
         "  -l                      Start as a login shell (Termux extension)\n"
         "  -i                      Force interactive mode\n"
         "  -s                      Read shell commands from standard input\n"
         "  -c COMMAND             Run one Dax shell command\n"
         "  -o OPTION              Enable a shell option; -o no_OPTION disables it\n"
         "  --OPTION, --no-OPTION  Enable or disable a shell option\n"
         "  +o OPTION              Disable a shell option; +o no_OPTION enables it\n"
         "  +-OPTION, +-no-OPTION  Disable or enable a shell option\n"
         "  -a/-b/-C/-e/-f/-m/-n/-u/-v/-x and + forms\n"
         "                         POSIX shell option flags\n"
         "Named options: allexport, notify, noclobber, errexit, noglob,\n"
         "               monitor, noexec, nounset, verbose, xtrace\n\n"
         "Dax supports POSIX-style invocation and these options; this does not\n"
         "claim full POSIX shell-language conformance.\n";
}

static bool apply_named_cli_option(const std::string &name, bool enabled,
                                   bool &option_seen, std::string &error) {
  if (!set_shell_option(name, enabled)) {
    error = "unknown shell option: " + name;
    return false;
  }
  option_seen = true;
  return true;
}

static bool parse_cli(int argc, char **argv, CliInvocation &invocation,
                      std::string &error) {
  size_t i = 1;
  bool stop_options = false;
  auto set_flag = [&](char flag, bool enabled) {
    const std::string name = flag == 'a' ? "allexport"
                             : flag == 'b' ? "notify"
                             : flag == 'C' ? "noclobber"
                             : flag == 'e' ? "errexit"
                             : flag == 'f' ? "noglob"
                             : flag == 'm' ? "monitor"
                             : flag == 'n' ? "noexec"
                             : flag == 'u' ? "nounset"
                             : flag == 'v' ? "verbose"
                             : flag == 'x' ? "xtrace" : "";
    if (name.empty()) return false;
    if (flag == 'm') invocation.monitor_option_seen = true;
    return apply_named_cli_option(name, enabled,
                                  invocation.shell_option_seen, error);
  };
  auto set_named_flag = [&](std::string name, bool default_enabled) {
    bool enabled = default_enabled;
    if (name.rfind("no_", 0) == 0 || name.rfind("no-", 0) == 0) {
      name.erase(0, 3);
      enabled = !enabled;
    }
    if (name == "monitor") invocation.monitor_option_seen = true;
    return apply_named_cli_option(name, enabled,
                                  invocation.shell_option_seen, error);
  };
  while (i < static_cast<size_t>(argc)) {
    const std::string arg = argv[i];
    if (!stop_options && arg == "--help") {
      invocation.show_help = true;
      return true;
    }
    if (!stop_options && arg == "--version") {
      invocation.show_version = true;
      return true;
    }
    if (!stop_options && arg == "--") {
      stop_options = true;
      ++i;
      continue;
    }
    if (!stop_options &&
        (arg == "--script" || arg == "--janet" ||
         arg == "--janet-restricted")) {
      if (arg == "--janet-restricted") {
        if (i + 2 >= static_cast<size_t>(argc)) {
          error = "--janet-restricted requires a workspace and script path";
          return false;
        }
        invocation.mode = CliMode::RestrictedJanet;
        invocation.workspace = argv[++i];
        invocation.script_path = argv[++i];
      } else {
        if (i + 1 >= static_cast<size_t>(argc)) {
          error = arg + " requires a script path";
          return false;
        }
        invocation.mode = arg == "--janet" ? CliMode::Janet : CliMode::Script;
        invocation.script_path = argv[++i];
      }
      ++i;
      for (; i < static_cast<size_t>(argc); ++i)
        invocation.arguments.emplace_back(argv[i]);
      invocation.has_operands = !invocation.arguments.empty();
      return true;
    }
    if (!stop_options && (arg == "-o" || arg == "+o")) {
      const bool default_enabled = arg == "-o";
      if (++i >= static_cast<size_t>(argc)) {
        error = arg + " requires an option name";
        return false;
      }
      if (!set_named_flag(argv[i], default_enabled)) return false;
      ++i;
      continue;
    }
    if (!stop_options && arg.rfind("+-", 0) == 0) {
      if (!set_named_flag(arg.substr(2), false))
        return false;
      ++i;
      continue;
    }
    if (!stop_options && arg.rfind("--no-", 0) == 0) {
      if (!set_named_flag(arg.substr(5), false))
        return false;
      ++i;
      continue;
    }
    if (!stop_options && arg.rfind("--", 0) == 0) {
      if (!set_named_flag(arg.substr(2), true))
        return false;
      ++i;
      continue;
    }
    if (!stop_options && arg.size() > 1 &&
        (arg[0] == '-' || arg[0] == '+')) {
      const bool enabled = arg[0] == '-';
      for (size_t j = 1; j < arg.size(); ++j) {
        const char flag = arg[j];
        if (flag == 'o') {
          std::string option_name;
          if (j + 1 < arg.size()) {
            option_name = arg.substr(j + 1);
            j = arg.size();
          } else if (i + 1 < static_cast<size_t>(argc)) {
            option_name = argv[++i];
          } else {
            error = std::string(1, arg[0]) + "o requires an option name";
            return false;
          }
          if (!set_named_flag(option_name, enabled)) return false;
          break;
        }
        if (flag == 'c' && enabled) {
          std::string command;
          if (j + 1 < arg.size()) command = arg.substr(j + 1);
          else if (i + 1 < static_cast<size_t>(argc)) command = argv[++i];
          else {
            error = "-c requires a command string";
            return false;
          }
          invocation.mode = CliMode::Command;
          invocation.command = std::move(command);
          invocation.has_operands = true;
          ++i;
          if (i < static_cast<size_t>(argc)) {
            invocation.shell_name = argv[i++];
            for (; i < static_cast<size_t>(argc); ++i)
              invocation.arguments.emplace_back(argv[i]);
            invocation.has_operands = true;
          }
          return true;
        }
        if (flag == 'i' && enabled) {
          invocation.force_interactive = true;
          continue;
        }
        if (flag == 'l' && enabled) {
          invocation.login = true;
          continue;
        }
        if (flag == 's' && enabled) {
          invocation.mode = CliMode::Stdin;
          ++i;
          for (; i < static_cast<size_t>(argc); ++i)
            invocation.arguments.emplace_back(argv[i]);
          invocation.has_operands = !invocation.arguments.empty();
          return true;
        }
        if (!set_flag(flag, enabled)) {
          error = "invalid option: " + arg;
          return false;
        }
      }
      ++i;
      continue;
    }
    if (!stop_options && !arg.empty() && arg[0] == '-') {
      error = "invalid option: " + arg;
      return false;
    }
    if (!stop_options && arg.size() > 1 && arg[0] == '+') {
      error = "invalid option: " + arg;
      return false;
    }

    invocation.mode = CliMode::Script;
    invocation.script_path = arg;
    invocation.shell_name = arg;
    invocation.has_operands = true;
    ++i;
    for (; i < static_cast<size_t>(argc); ++i)
      invocation.arguments.emplace_back(argv[i]);
    return true;
  }
  return true;
}

static bool start_interactive_runtime() {
  if (!janet_runtime_start()) return false;
  const char *config_root = std::getenv("XDG_CONFIG_HOME");
  std::string init_path;
  if (config_root && *config_root)
    init_path = std::string(config_root) + "/dax/init.janet";
  else if (const char *home = std::getenv("HOME"))
    init_path = std::string(home) + "/.config/dax/init.janet";
  if (!init_path.empty()) {
    std::ifstream init(init_path);
    if (init) {
      std::vector<std::string> no_args;
      janet_run_file(init_path, no_args);
      janet_run_startup_hook();
    }
  }
  return true;
}

static void print_job_notifications(bool redraw_line = false) {
  const auto notifications = take_completed_job_notifications();
  if (notifications.empty()) return;
  if (redraw_line) rl_crlf();
  for (const auto &notification : notifications)
    std::cout << notification << '\n';
  std::cout.flush();
  if (redraw_line) {
    rl_on_new_line();
    rl_redisplay();
  }
}

static int readline_job_event() {
  print_job_notifications(true);
  return 0;
}

static void run_stdin_commands(std::istream &input) {
  std::string source_line;
  std::string command_buffer;
  while (std::getline(input, source_line)) {
    if (!command_buffer.empty()) command_buffer += '\n';
    command_buffer += source_line;
    if (shell_input_incomplete(command_buffer)) continue;
    execute_line(command_buffer);
    print_job_notifications();
    command_buffer.clear();
    if (shell_should_exit()) break;
  }
  if (!command_buffer.empty() && !shell_should_exit())
    execute_line(command_buffer);
}

int main(int argc, char **argv) {
  CliInvocation invocation;
  std::string cli_error;
  if (!parse_cli(argc, argv, invocation, cli_error)) {
    std::cerr << "dax: " << cli_error << "\nTry 'dax --help' for usage.\n";
    return 2;
  }
  if (invocation.show_help) {
    print_usage(std::cout);
    return 0;
  }
  if (invocation.show_version) {
    std::cout << "dax " << DAX_VERSION << "\n";
    return 0;
  }

  const bool dax_invocation = invocation.mode != CliMode::Janet &&
                              invocation.mode != CliMode::RestrictedJanet;
  const bool stdin_requested = invocation.mode == CliMode::Stdin;
  const bool terminal_default =
      (invocation.mode == CliMode::Interactive ||
       invocation.mode == CliMode::Stdin) && !invocation.has_operands &&
      isatty(STDIN_FILENO) && isatty(STDERR_FILENO);
  const bool interactive = dax_invocation &&
      (invocation.force_interactive || terminal_default);
  if ((invocation.mode == CliMode::Interactive ||
       invocation.mode == CliMode::Stdin) && !interactive)
    invocation.mode = CliMode::Stdin;
  else if (invocation.mode == CliMode::Stdin && interactive)
    invocation.mode = CliMode::Interactive;
  set_shell_modes(invocation.login, interactive,
                  invocation.monitor_option_seen);

  if (invocation.mode == CliMode::Command)
    set_shell_arguments(invocation.shell_name, invocation.arguments);
  else if (invocation.mode == CliMode::Script)
    set_shell_arguments(invocation.script_path, invocation.arguments);
  else if (invocation.mode == CliMode::Stdin)
    set_shell_arguments(argc > 0 ? argv[0] : "dax", invocation.arguments);
  else if (invocation.mode == CliMode::Interactive)
    set_shell_arguments(argc > 0 ? argv[0] : "dax",
                        stdin_requested ? invocation.arguments
                                        : std::vector<std::string>{});

  if (interactive) {
    if (!start_interactive_runtime()) return 1;
  }

  if (invocation.mode == CliMode::Command) {
    execute_line(invocation.command);
    print_job_notifications();
    run_exit_trap();
    janet_runtime_stop();
    return shell_should_exit() ? requested_exit_status() : last_status();
  }
  if (invocation.mode == CliMode::Script) {
    std::ifstream script(invocation.script_path);
    if (!script) {
      std::cerr << "dax: cannot open script: " << invocation.script_path << "\n";
      janet_runtime_stop();
      return 127;
    }
    std::ostringstream source;
    source << script.rdbuf();
    execute_line(source.str());
    print_job_notifications();
    run_exit_trap();
    janet_runtime_stop();
    return shell_should_exit() ? requested_exit_status() : last_status();
  }
  if (invocation.mode == CliMode::Stdin) {
    run_stdin_commands(std::cin);
    print_job_notifications();
    run_exit_trap();
    janet_runtime_stop();
    return shell_should_exit() ? requested_exit_status() : last_status();
  }
  if (invocation.mode == CliMode::Janet) {
    if (!janet_runtime_start()) return 1;
    const int result = janet_run_file(invocation.script_path, invocation.arguments);
    janet_runtime_stop();
    return result;
  }
  if (invocation.mode == CliMode::RestrictedJanet)
    return janet_run_restricted_file(invocation.script_path, invocation.workspace,
                                     invocation.arguments);

  std::string line;
  std::string command_buffer;

  // Load history from file on startup
  History::load_from_file();

  // Set history size limit to 1000 commands
  stifle_history(1000);

  // Load existing history into readline
  const auto &hist = History::get_all();
  for (const auto &cmd : hist) {
    add_history(cmd.c_str());
  }

  configure_interactive_readline();

  setup_signals();
  rl_event_hook = readline_job_event;

  while (true) {
    char cwd[1024];
    std::string prompt_str;
    if (getcwd(cwd, sizeof(cwd)) != nullptr) {
      std::string path(cwd);
      std::string dir_name;

      if (path == "/") {
        dir_name = "/";
      } else {
        size_t last_slash = path.find_last_of('/');
        if (last_slash != std::string::npos) {
          dir_name = path.substr(last_slash + 1);
        } else {
          dir_name = path;
        }
      }

      // Green color: \033[1;32m, Reset: \033[0m
      // \001 and \002 are used to tell readline these are non-printing
      // characters
      if (janet_prompt().empty()) {
        const std::string prompt_color = color_code("prompt");
        prompt_str = marked_text(prompt_color) + dir_name;
        if (janet_git_branch_enabled()) {
          const std::string branch = current_git_branch();
          if (!branch.empty()) {
            const std::string branch_color = color_code("git-branch");
            prompt_str += " " + marked_text(branch_color) + "[" + branch + "]";
          }
        }
        prompt_str += prompt_color.empty() ? " > " : marked_text("\033[0m") + " > ";
      } else {
        prompt_str = janet_prompt();
      }
    } else {
      const std::string prompt_color = color_code("prompt");
      prompt_str = marked_text(prompt_color) + "> " + (prompt_color.empty() ? "" : marked_text("\033[0m"));
    }

    // Use readline for input with arrow key support
    const std::string continuation_color = color_code("prompt");
    const std::string continuation_prompt = marked_text(continuation_color) + "... " +
        (continuation_color.empty() ? "" : marked_text("\033[0m"));
    const std::string input_prompt = command_buffer.empty() ? prompt_str : continuation_prompt;
    begin_readline_input(prompt_str.c_str());
    renderer_started = false;
    char *input = readline(input_prompt.c_str());
    const bool input_interrupted = finish_readline_input();

    // Handle Ctrl+D (EOF)
    if (!input) {
      std::cout << "\n";
      break;
    }

    line = input;
    free(input);

    // The custom Readline renderer leaves the cursor exactly at rl_point;
    // Readline's newline handler does not advance the terminal in this mode.
    if (!input_interrupted) {
      std::cout << "\r\n";
      std::cout.flush();
    }

    if (input_interrupted)
      command_buffer.clear();

    if (line.empty() && command_buffer.empty())
      continue;

    // Add to both readline history and our History namespace
    add_history(line.c_str());
    History::add(line);

    if (!command_buffer.empty()) command_buffer += '\n';
    command_buffer += line;
    if (shell_input_incomplete(command_buffer)) continue;
    execute_line(command_buffer);
    print_job_notifications();
    command_buffer.clear();
    if (shell_should_exit())
      break;

    // std::cout << "Not a built-in command: " << tokens[0] << "\n";

    //  std::cout << "Tokens: [";
    //  for (size_t i = 0; i < tokens.size(); ++i) {
    //      std::cout << tokens[i];
    //      if (i < tokens.size() - 1) std::cout << ",";
    //  }
    //  std::cout << "]\n";
  }

  // Save history to file before exiting
  run_exit_trap();
  History::save_to_file();

  janet_runtime_stop();
  rl_event_hook = nullptr;

  return shell_should_exit() ? requested_exit_status() : last_status();
}
