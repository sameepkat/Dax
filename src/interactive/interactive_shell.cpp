#include "interactive_shell.hpp"
#include "signal_handler.hpp"
#include "shell.hpp"
#include <readline/history.h>
#include "builtins.hpp"
#include "helper.hpp"
#include "janet_runtime.hpp"
#include "shell_state.hpp"
#include "tokenizer.hpp"
#include <algorithm>
#include <cctype>
#include <clocale>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <readline/readline.h>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
std::vector<std::string> completion_items;
size_t completion_index = 0;
std::string history_entry_for_command(const std::string &command,
                                      bool preserve_newlines) {
  if (preserve_newlines || command.find_first_of("'\"`") != std::string::npos ||
      command.find("<<") != std::string::npos ||
      command.find('#') != std::string::npos)
    return command;
  static const std::vector<std::string> compound_tokens = {
      "if", "then", "elif", "else", "fi", "for", "while", "until",
      "select", "do", "done", "case", "esac", "function", "{", "}", "(", ")"};
  for (const auto &token : tokenize(command)) {
    if (!token.quoted && std::find(compound_tokens.begin(), compound_tokens.end(), token) !=
                             compound_tokens.end())
      return command;
  }
  std::string flattened;
  for (size_t i = 0; i < command.size(); ++i) {
    if (command[i] != '\n') {
      flattened += command[i];
      continue;
    }
    size_t backslashes = 0;
    for (size_t end = flattened.size(); end > 0 && flattened[end - 1] == '\\'; --end)
      ++backslashes;
    if (backslashes % 2 == 1) {
      flattened.pop_back();
      continue;
    }
    size_t previous = flattened.find_last_not_of(" \t");
    const bool separator_already_present = previous != std::string::npos &&
        std::string(";&|<>").find(flattened[previous]) != std::string::npos;
    flattened += separator_already_present ? " " : "; ";
  }
  return flattened;
}

bool renderer_started = false;
bool prompt_timeout_active = false;
bool prompt_timeout_hit = false;
std::chrono::steady_clock::time_point prompt_deadline;
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
  if (!command_position && !variable_name) {
    std::istringstream prior_words(command_prefix);
    std::string command_name;
    while (prior_words >> command_name) {
      if (command_name.find('=') == std::string::npos) break;
      command_name.clear();
    }
    if (!command_name.empty()) {
      auto programmable = shell_programmable_completions(command_name, candidate_prefix);
      if (programmable) {
        completion_items = std::move(*programmable);
        rl_attempted_completion_over = 1;
        if (completion_items.empty()) return nullptr;
        rl_completion_append_character = !shell_last_completion_nospace() &&
            completion_items.size() == 1 &&
            (completion_items.front().empty() || completion_items.front().back() != '/') ? ' ' : '\0';
        return rl_completion_matches(word.c_str(), completion_generator);
      }
    }
  }
  const bool blank_line = command_position && command_prefix.find_first_not_of(" \t\n") == std::string::npos &&
                          word.empty() && command_boundary == std::string::npos;
  if (command_position && !variable_name) {
    const std::string special_key = std::string(1, '\x01') +
                                    (blank_line ? "empty" : "initial");
    auto programmable = shell_programmable_completions(special_key, candidate_prefix);
    if (!programmable && special_key != std::string(1, '\x01') + "default")
      programmable = shell_programmable_completions(std::string(1, '\x01') + "default",
                                                    candidate_prefix);
    if (programmable) {
      completion_items = std::move(*programmable);
      rl_attempted_completion_over = 1;
      if (completion_items.empty()) return nullptr;
      rl_completion_append_character = !shell_last_completion_nospace() &&
          completion_items.size() == 1 &&
          (completion_items.front().empty() || completion_items.front().back() != '/') ? ' ' : '\0';
      return rl_completion_matches(word.c_str(), completion_generator);
    }
  }
  completion_items = shell_completion_candidates(candidate_prefix, command_position,
                                                  variable_name, directory_prefix,
                                                  blank_line);
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

void configure_readline() {
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

void configure_interactive_readline() {
  if (shell_option_enabled("vi")) rl_vi_editing_mode(1, 0);
  else if (shell_option_enabled("emacs")) rl_emacs_editing_mode(1, 0);
  configure_readline();
}
void reset_interactive_renderer() { renderer_started = false; }

std::string build_interactive_prompt() {
  if (auto prompt = shell_expand_prompt_variable("PS1")) return *prompt;
  char cwd[1024];
  if (getcwd(cwd, sizeof(cwd)) == nullptr) {
    const std::string color = color_code("prompt");
    return marked_text(color) + "> " + (color.empty() ? "" : marked_text("\033[0m"));
  }
  const std::string path(cwd);
  std::string dir_name;
  if (path == "/") dir_name = "/";
  else {
    const size_t last_slash = path.find_last_of('/');
    dir_name = last_slash == std::string::npos ? path : path.substr(last_slash + 1);
  }
  if (!janet_prompt().empty()) return janet_prompt();
  const std::string prompt_color = color_code("prompt");
  std::string prompt = marked_text(prompt_color) + dir_name;
  if (janet_git_branch_enabled()) {
    const std::string branch = current_git_branch();
    if (!branch.empty()) {
      const std::string branch_color = color_code("git-branch");
      prompt += " " + marked_text(branch_color) + "[" + branch + "]";
    }
  }
  prompt += prompt_color.empty() ? " > " : marked_text("\033[0m") + " > ";
  return prompt;
}

std::string build_continuation_prompt() {
  if (auto prompt = shell_expand_prompt_variable("PS2")) return *prompt;
  const std::string color = color_code("prompt");
  return marked_text(color) + "... " + (color.empty() ? "" : marked_text("\033[0m"));
}

void print_job_notifications(bool redraw_line) {
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
  refresh_shell_terminal_dimensions(STDERR_FILENO,
                                    shell_state().interactive_shell);
  print_job_notifications(true);
  if (prompt_timeout_active && std::chrono::steady_clock::now() >= prompt_deadline) {
    prompt_timeout_active = false;
    prompt_timeout_hit = true;
    rl_done = 1;
  }
  return 0;
}

static void set_prompt_timeout(bool enabled) {
  prompt_timeout_active = false;
  prompt_timeout_hit = false;
  if (!enabled) return;
  const auto &state = shell_state();
  const auto configured = state.vars.find("TMOUT");
  const char *environment = std::getenv("TMOUT");
  const std::string value = configured != state.vars.end()
      ? configured->second : environment ? environment : "";
  if (value.empty()) return;
  char *end = nullptr;
  errno = 0;
  const double seconds = std::strtod(value.c_str(), &end);
  if (errno || end == value.c_str() || !end || *end || !std::isfinite(seconds) ||
      seconds <= 0 || seconds > 2147483.0) return;
  prompt_deadline = std::chrono::steady_clock::now() +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(seconds));
  prompt_timeout_active = true;
}

static void run_prompt_command() {
  auto &state = shell_state();
  if (state.unset_vars.count("PROMPT_COMMAND")) return;
  const auto array = state.arrays.find("PROMPT_COMMAND");
  if (array != state.arrays.end()) {
    for (size_t index = 0; index < array->second.size(); ++index) {
      if (!shell_array_index_is_set("PROMPT_COMMAND", index) ||
          array->second[index].empty())
        continue;
      execute_line(array->second[index]);
      if (shell_should_exit()) break;
    }
    return;
  }
  const auto variable = state.vars.find("PROMPT_COMMAND");
  const char *environment_value = std::getenv("PROMPT_COMMAND");
  const std::string command = variable != state.vars.end()
      ? variable->second : environment_value ? environment_value : std::string();
  if (!command.empty()) execute_line(command);
}

int run_interactive_shell() {
  std::string line;
  std::string command_buffer;
  unsigned long long consecutive_eof_count = 0;

  // Bash only loads the history list when command history is enabled.
  if (shell_option_enabled("history")) History::load_from_file();

  configure_interactive_readline();

  setup_signals();
  rl_event_hook = readline_job_event;

  while (true) {
    if (command_buffer.empty()) run_prompt_command();
    if (shell_should_exit()) break;
    const std::string prompt_str = build_interactive_prompt();

    // Use readline for input with arrow key support
    const std::string continuation_prompt = build_continuation_prompt();
    const std::string input_prompt = command_buffer.empty() ? prompt_str : continuation_prompt;
    set_prompt_timeout(command_buffer.empty());
    configure_interactive_readline();
    begin_readline_input(prompt_str.c_str());
    reset_interactive_renderer();
    char *input = readline(input_prompt.c_str());
    const bool input_interrupted = finish_readline_input();
    prompt_timeout_active = false;

    if (prompt_timeout_hit) {
      prompt_timeout_hit = false;
      if (input) std::free(input);
      std::cout << "\r\n";
      std::cout.flush();
      break;
    }

    // Handle Ctrl+D (EOF)
    if (!input) {
      if (command_buffer.empty() && shell_option_enabled("ignoreeof")) {
        const auto &state = shell_state();
        const auto configured = state.vars.find("IGNOREEOF");
        const char *environment = configured == state.vars.end()
            ? std::getenv("IGNOREEOF") : nullptr;
        const bool has_limit = configured != state.vars.end() || environment != nullptr;
        unsigned long long limit = has_limit ? 10 : 0;
        if (has_limit) {
          const std::string value = configured != state.vars.end()
              ? configured->second : environment;
          if (!value.empty()) {
            char *end = nullptr;
            errno = 0;
            const long long parsed = std::strtoll(value.c_str(), &end, 10);
            if (!errno && end != value.c_str() && *end == '\0')
              limit = parsed > 0 ? static_cast<unsigned long long>(parsed) : 0;
          }
        }
        if (consecutive_eof_count < limit) {
          ++consecutive_eof_count;
          std::cout << '\n';
          std::cout.flush();
          std::cerr << "Use \"" << (shell_state().login_shell ? "logout" : "exit")
                    << "\" to leave the shell.\n";
          continue;
        }
      }
      std::cout << "\n";
      break;
    }

    line = input;
    free(input);
    consecutive_eof_count = 0;

    // The custom Readline renderer leaves the cursor exactly at rl_point;
    // Readline's newline handler does not advance the terminal in this mode.
    if (!input_interrupted) {
      std::cout << "\r\n";
      std::cout.flush();
    }

    if (input_interrupted)
      command_buffer.clear();

    if (!input_interrupted && shell_histexpand_enabled()) {
      char *expanded = nullptr;
      const int expansion_status = history_expand(line.c_str(), &expanded);
      if (expansion_status < 0) {
        std::cerr << (expanded ? expanded : "dax: history expansion failed") << '\n';
        std::free(expanded);
        continue;
      }
      if (expanded && expansion_status > 0) {
        line = expanded;
        std::cout << line << '\n';
        if (expansion_status == 2) {
          if (shell_option_enabled("history")) History::add(line);
          std::free(expanded);
          continue;
        }
      }
      std::free(expanded);
    }

    if (line.empty() && command_buffer.empty())
      continue;

    if (!command_buffer.empty()) command_buffer += '\n';
    command_buffer += line;
    if (shell_option_enabled("history") && !shell_state().cmdhist) History::add(line);
    if (shell_input_incomplete(command_buffer)) continue;
    if (shell_option_enabled("history") && shell_state().cmdhist)
      History::add(history_entry_for_command(command_buffer,
                                             shell_state().lithist));
    if (auto prompt = shell_expand_prompt_variable("PS0")) {
      for (char ch : *prompt)
        if (ch != '\x01' && ch != '\x02') std::cout << ch;
      std::cout.flush();
    }
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

  // Bash saves the history list on exit only while command history is enabled.
  run_exit_trap();
  if (shell_option_enabled("history"))
    History::save_to_file(shell_state().histappend);

  janet_runtime_stop();
  rl_event_hook = nullptr;

  return shell_should_exit() ? requested_exit_status() : last_status();
}
