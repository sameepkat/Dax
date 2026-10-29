#include "shell.hpp"
#include "builtins.hpp"
#include "helper.hpp"
#include "parser.hpp"
#include "tokenizer.hpp"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

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

bool shell_input_incomplete(const std::string &source) {
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

