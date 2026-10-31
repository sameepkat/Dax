#include "shell.hpp"
#include "builtins.hpp"
#include "helper.hpp"
#include "parser.hpp"
#include "shell_state.hpp"
#include "tokenizer.hpp"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
bool is_function_redirection(const std::vector<ShellToken> &tokens,
                             size_t index, bool &here_document) {
  here_document = false;
  if (index >= tokens.size()) return false;
  const std::string &token = tokens[index];
  const size_t suffix_start = token.find_first_not_of("0123456789");
  const std::string suffix = suffix_start == std::string::npos
      ? std::string() : token.substr(suffix_start);
  const std::string &op = suffix.empty() ? token : suffix;
  here_document = op == "<<" || op == "<<-";
  return op == "<" || op == "<>" || op == ">" || op == ">>" ||
         op == ">|" || op == "&>" || op == "&>>" || op == ">&" ||
         op == "<&" || here_document || op == "<<<";
}

size_t function_redirection_end(const std::vector<ShellToken> &tokens,
                                size_t index) {
  size_t valid_end = index;
  while (index < tokens.size()) {
    if (index + 1 < tokens.size() && tokens[index].size() > 2 &&
        tokens[index].front() == '{' && tokens[index].back() == '}' &&
        tokens[index].source_end == tokens[index + 1].source_begin) {
      const std::string name = tokens[index].substr(1, tokens[index].size() - 2);
      const bool valid_name =
          (std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_') &&
          std::all_of(name.begin() + 1, name.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '_';
          });
      bool dynamic_operator_is_here_document = false;
      if (valid_name && is_function_redirection(
                            tokens, index + 1, dynamic_operator_is_here_document))
        ++index;
    }
    bool is_here_document = false;
    if (!is_function_redirection(tokens, index, is_here_document)) break;
    ++index;
    if (is_here_document) {
      if (index + 1 >= tokens.size()) return valid_end;
      index += 2;
    } else {
      if (index >= tokens.size() || tokens[index] == ";" ||
          tokens[index] == "&" || tokens[index] == "&&" ||
          tokens[index] == "||" || tokens[index] == "|" ||
          tokens[index] == "|&" || tokens[index] == ")" ||
          tokens[index] == "}")
        return valid_end;
      ++index;
    }
    valid_end = index;
  }
  return valid_end;
}
}

void execute_line(const std::string &line) {
  if (shell_verbose_enabled()) {
    std::istringstream input(line);
    std::string source_line;
    while (std::getline(input, source_line)) std::cerr << source_line << '\n';
  }
  auto tokens = tokenize(line);
  if (tokens.empty())
    return;
  static unsigned execution_line_depth = 0;
  struct ExecutionLineGuard {
    unsigned &depth;
    explicit ExecutionLineGuard(unsigned &value) : depth(value) { ++depth; }
    ~ExecutionLineGuard() { --depth; }
  } execution_line_guard(execution_line_depth);
  ++shell_state().command_number;
  static std::string pending_name;
  static std::string pending_body_source;
  static size_t pending_brace_depth = 0;
  size_t function_end = 0;
  bool function_definition_failed = false;
  auto finish_pending_function = [&](size_t start) {
    for (size_t i = start; i < tokens.size(); ++i) {
      if (tokens[i] == "{") ++pending_brace_depth;
      else if (tokens[i] == "}" && pending_brace_depth > 0 && --pending_brace_depth == 0) {
        const size_t redirection_end = function_redirection_end(tokens, i + 1);
        if (!pending_body_source.empty()) pending_body_source += '\n';
        pending_body_source += line.substr(0, tokens[i].source_begin);
        std::string function_body = pending_body_source;
        if (redirection_end > i + 1) {
          function_body = "{" + function_body + "}";
          const size_t redirection_end_offset =
              tokens[redirection_end - 1].source_end;
          if (redirection_end_offset >= tokens[i].source_end &&
              redirection_end_offset <= line.size())
            function_body += line.substr(tokens[i].source_end,
                                         redirection_end_offset - tokens[i].source_end);
        }
        if (!shell_noexec_enabled() &&
            !define_function(pending_name, function_body))
          function_definition_failed = true;
        pending_name.clear();
        pending_body_source.clear();
        function_end = redirection_end;
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
    size_t brace_depth = 1;
    while (close < tokens.size() && brace_depth > 0) {
      if (tokens[close] == "{") ++brace_depth;
      else if (tokens[close] == "}") --brace_depth;
      if (brace_depth > 0) ++close;
    }
    if (close == tokens.size()) {
      pending_name = tokens[name_index];
      pending_brace_depth = brace_depth;
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
    const size_t redirection_end = function_redirection_end(tokens, close + 1);
    std::string function_body = body;
    if (redirection_end > close + 1) {
      function_body = line.substr(tokens[open].source_begin,
                                  tokens[close].source_end - tokens[open].source_begin);
      const size_t redirection_end_offset = tokens[redirection_end - 1].source_end;
      if (redirection_end_offset >= tokens[close].source_end &&
          redirection_end_offset <= line.size())
        function_body += line.substr(tokens[close].source_end,
                                     redirection_end_offset - tokens[close].source_end);
    }
    if (!shell_noexec_enabled() && !define_function(tokens[name_index], function_body))
      function_definition_failed = true;
    tokens.erase(tokens.begin() + i, tokens.begin() + redirection_end);
    while (i < tokens.size() && tokens[i] == ";") tokens.erase(tokens.begin() + i);
  }
  if (tokens.empty()) {
    set_last_status(function_definition_failed ? 1 : 0);
    return;
  }
  if (shell_option_enabled("expand_aliases")) expand_aliases(tokens);
  try {
    auto ast = parse(tokens, line);
    const int status = shell_noexec_enabled() ? 0 : (ast ? ast->execute() : 0);
    set_last_status(status);
    apply_errexit(status);
    clear_errexit_immune();
  } catch (const std::exception &e) {
    std::cerr << "Parse/Exec Error: " << e.what() << "\n";
    shell_state().arrays["PIPESTATUS"] = {"2"};
    shell_array_clear_set_indices("PIPESTATUS");
    shell_array_mark_all_set("PIPESTATUS");
    set_last_status(2);
  }
  run_pending_signal_trap();
  reap_completed_process_substitutions();
  if (execution_line_depth == 1) shell_finish_one_command();
}

bool shell_input_incomplete(const std::string &source) {
  const auto tokens = tokenize(source);
  int if_depth = 0, loop_depth = 0, paren_depth = 0, brace_depth = 0;
  bool command_start = true;
  bool missing_heredoc = false;
  for (const auto &token : tokens) {
    if (!token.empty() && token[0] == '\x06') missing_heredoc = true;
    if (token == "if" && command_start) ++if_depth;
    else if (token == "fi" && if_depth > 0) --if_depth;
    if ((token == "for" || token == "while" || token == "until") && command_start) ++loop_depth;
    else if (token == "done" && loop_depth > 0) --loop_depth;
    if (token == "(") ++paren_depth;
    else if (token == ")" && paren_depth > 0) --paren_depth;
    if (token == "{") ++brace_depth;
    else if (token == "}" && brace_depth > 0) --brace_depth;
    command_start = token == ";" || token == "&" || token == "&&" || token == "||" ||
                    token == "|" || token == "|&" || token == "then" || token == "do" ||
                    token == "else" || token == "elif";
  }
  std::string token_text;
  for (const auto &token : tokens) token_text += token;
  const auto single_quotes = std::count(token_text.begin(), token_text.end(), '\x01');
  const auto double_quotes = std::count(token_text.begin(), token_text.end(), '\x02');
  const auto command_substitutions = std::count(token_text.begin(), token_text.end(), '\x03');
  bool open_operator = !tokens.empty() &&
      (tokens.back() == "&&" || tokens.back() == "||" || tokens.back() == "|" ||
       tokens.back() == "|&");
  return if_depth > 0 || loop_depth > 0 || paren_depth > 0 || brace_depth > 0 ||
         (single_quotes % 2) || (double_quotes % 2) || (command_substitutions % 2) ||
         missing_heredoc || open_operator;
}

