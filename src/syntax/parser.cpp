#include "parser.hpp"
#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <optional>
#include <stdexcept>

class Parser {
  const std::vector<ShellToken> &tokens;
  const std::string &source;
  size_t pos = 0;

  std::string peek() const {
    if (pos < tokens.size())
      return tokens[pos];
    return "";
  }

  std::string consume() {
    if (pos < tokens.size())
      return tokens[pos++];
    return "";
  }

  bool match(const std::string &type) {
    if (peek() == type) {
      consume();
      return true;
    }
    return false;
  }

  std::unique_ptr<ASTNode> parse_if() {
    auto node = std::make_unique<IfNode>();
    while (true) {
      match("if");
      auto condition = parse_list();
      if (!condition || !match("then")) throw std::runtime_error("Expected condition followed by then");
      auto body = parse_list();
      node->branches.emplace_back(std::move(condition), std::move(body));
      if (!match("elif")) break;
      // The next condition is parsed by the next iteration without consuming a second if.
      auto condition2 = parse_list();
      if (!condition2 || !match("then")) throw std::runtime_error("Expected elif condition followed by then");
      auto body2 = parse_list();
      node->branches.emplace_back(std::move(condition2), std::move(body2));
      while (match("elif")) {
        auto condition_n = parse_list();
        if (!condition_n || !match("then")) throw std::runtime_error("Expected elif condition followed by then");
        auto body_n = parse_list();
        node->branches.emplace_back(std::move(condition_n), std::move(body_n));
      }
      break;
    }
    if (match("else")) node->otherwise = parse_list();
    if (!match("fi")) throw std::runtime_error("Missing fi for if statement");
    return node;
  }

  std::unique_ptr<ASTNode> parse_case() {
    consume();
    if (pos >= tokens.size()) throw std::runtime_error("case requires a subject word");
    auto node = std::make_unique<CaseNode>(consume());
    if (!match("in")) throw std::runtime_error("Expected in after case subject");
    while (match(";")) {}
    while (pos < tokens.size() && peek() != "esac") {
      std::vector<std::string> patterns;
      while (pos < tokens.size() && peek() != ")") {
        if (match("|")) continue;
        if (peek() == ";") { consume(); continue; }
        patterns.push_back(consume());
      }
      if (patterns.empty() || !match(")")) throw std::runtime_error("Expected case pattern followed by )");
      auto body = parse_list();
      node->clauses.push_back({std::move(patterns), std::move(body), CaseNode::Terminator::Stop});
      if (match(";;")) {
        while (match(";")) {}
        continue;
      }
      if (match(";&")) {
        node->clauses.back().terminator = CaseNode::Terminator::FallThrough;
        while (match(";")) {}
        continue;
      }
      if (match(";;&")) {
        node->clauses.back().terminator = CaseNode::Terminator::Rematch;
        while (match(";")) {}
        continue;
      }
      if (peek() != "esac") throw std::runtime_error("Expected ;;, ;&, ;;& or esac after case clause");
    }
    if (!match("esac")) throw std::runtime_error("Missing esac for case statement");
    return node;
  }

  std::unique_ptr<ASTNode> parse_loop(bool is_until) {
    consume(); // while or until
    auto condition = parse_list();
    if (!condition || !match("do")) throw std::runtime_error("Expected loop condition followed by do");
    auto body = parse_list();
    if (!match("done")) throw std::runtime_error("Missing done for loop");
    return std::make_unique<LoopNode>(std::move(condition), std::move(body), is_until);
  }

  std::optional<std::string> parse_arithmetic_group() {
    if (peek() != "(" || pos + 1 >= tokens.size() || tokens[pos + 1] != "(") return {};
    const size_t content_begin = tokens[pos + 1].source_end;
    size_t cursor = pos + 2;
    int depth = 0;
    for (; cursor < tokens.size(); ++cursor) {
      if (tokens[cursor] == "(") ++depth;
      else if (tokens[cursor] == ")") {
        if (depth > 0) --depth;
        else if (cursor + 1 < tokens.size() && tokens[cursor + 1] == ")") {
          const size_t content_end = tokens[cursor].source_begin;
          const std::string expression = content_end >= content_begin
              ? source.substr(content_begin, content_end - content_begin) : std::string();
          pos = cursor + 2;
          return expression;
        }
      }
    }
    throw std::runtime_error("Missing )) for arithmetic expression");
  }

  static std::vector<std::string> split_arithmetic_for(const std::string &text) {
    std::vector<std::string> parts;
    size_t begin = 0;
    int depth = 0;
    char quote = 0;
    for (size_t i = 0; i < text.size(); ++i) {
      const char ch = text[i];
      if (quote) {
        if (ch == '\\' && i + 1 < text.size()) { ++i; continue; }
        if (ch == quote) quote = 0;
      } else if (ch == '\'' || ch == '"') quote = ch;
      else if (ch == '(') ++depth;
      else if (ch == ')' && depth > 0) --depth;
      else if (ch == ';' && depth == 0) {
        parts.push_back(text.substr(begin, i - begin));
        begin = i + 1;
      }
    }
    parts.push_back(text.substr(begin));
    return parts;
  }

  std::unique_ptr<ASTNode> parse_for() {
    consume();
    if (peek() == "(" && pos + 1 < tokens.size() && tokens[pos + 1] == "(") {
      auto expression = parse_arithmetic_group();
      auto parts = split_arithmetic_for(*expression);
      if (parts.size() != 3)
        throw std::runtime_error("arithmetic for requires initialization; condition; increment");
      match(";");
      if (!match("do")) throw std::runtime_error("Expected do after arithmetic for clauses");
      auto body = parse_list();
      if (!match("done")) throw std::runtime_error("Missing done for arithmetic for loop");
      return std::make_unique<ArithmeticForNode>(std::move(parts[0]), std::move(parts[1]),
                                                 std::move(parts[2]), std::move(body));
    }
    const std::string variable = consume();
    if (variable.empty() || !(std::isalpha(static_cast<unsigned char>(variable[0])) || variable[0] == '_'))
      throw std::runtime_error("for requires a valid variable name");
    for (char ch : variable)
      if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'))
        throw std::runtime_error("for requires a valid variable name");
    std::vector<std::string> values;
    if (match("in")) {
      while (pos < tokens.size() && peek() != ";" && peek() != "do") values.push_back(consume());
    } else {
      values.push_back("\x02$@\x02");
    }
    match(";");
    if (!match("do")) throw std::runtime_error("Expected do after for list");
    auto body = parse_list();
    if (!match("done")) throw std::runtime_error("Missing done for for loop");
    return std::make_unique<ForNode>(variable, std::move(values), std::move(body));
  }

  std::unique_ptr<ASTNode> parse_select() {
    consume();
    const std::string variable = consume();
    if (variable.empty() || !(std::isalpha(static_cast<unsigned char>(variable[0])) || variable[0] == '_'))
      throw std::runtime_error("select requires a valid variable name");
    for (char ch : variable)
      if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'))
        throw std::runtime_error("select requires a valid variable name");
    std::vector<std::string> values;
    if (match("in")) {
      while (pos < tokens.size() && peek() != ";" && peek() != "do") values.push_back(consume());
    } else {
      values.push_back("\x02$@\x02");
    }
    match(";");
    if (!match("do")) throw std::runtime_error("Expected do after select list");
    auto body = parse_list();
    if (!match("done")) throw std::runtime_error("Missing done for select loop");
    return std::make_unique<SelectNode>(variable, std::move(values), std::move(body));
  }

  std::unique_ptr<ASTNode> parse_array_assignment() {
    if (pos >= tokens.size()) return nullptr;
    const std::string lhs = peek();
    const size_t append_at = lhs.find("+=");
    const size_t equal_at = append_at == std::string::npos ? lhs.find('=') : append_at;
    if (equal_at == std::string::npos) return nullptr;
    const bool array_literal = pos + 1 < tokens.size() && tokens[pos + 1] == "(";
    const std::string variable = lhs.substr(0, equal_at);
    const size_t bracket = variable.find('[');
    const bool indexed_element = !array_literal && bracket != std::string::npos &&
                                 !variable.empty() && variable.back() == ']';
    if (!array_literal && !indexed_element) return nullptr;
    if (indexed_element && append_at != std::string::npos)
      throw std::runtime_error("array element append assignment is invalid");
    auto node = std::make_unique<ArrayAssignmentNode>();
    node->append = append_at != std::string::npos;
    if (!variable.empty() && variable.back() == ']') {
      if (bracket == std::string::npos || bracket == 0)
        throw std::runtime_error("Invalid indexed array assignment");
      const std::string index = variable.substr(bracket + 1, variable.size() - bracket - 2);
      if (index.empty()) throw std::runtime_error("Array index cannot be empty");
      node->name = variable.substr(0, bracket);
      node->index_expression = index;
      node->indexed = true;
    } else {
      node->name = std::move(variable);
    }
    if (node->name.empty() || !(std::isalpha(static_cast<unsigned char>(node->name[0])) || node->name[0] == '_'))
      throw std::runtime_error("Invalid array name");
    for (char ch : node->name)
      if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'))
        throw std::runtime_error("Invalid array name");
    if (array_literal) {
      pos += 2;
      while (pos < tokens.size() && peek() != ")") {
        if (peek() == ";" && tokens[pos].source_begin < source.size() &&
            source[tokens[pos].source_begin] == '\n') {
          consume();
          continue;
        }
        node->values.push_back(consume());
      }
      if (!match(")")) throw std::runtime_error("Missing ) in array assignment");
    } else {
      node->values.push_back(lhs.substr(equal_at + 1));
      ++pos;
    }
    return node;
  }

  std::unique_ptr<ASTNode> parse_array_declaration() {
    if (peek() != "declare" && peek() != "typeset" && peek() != "local" &&
        peek() != "readonly") return nullptr;
    const std::string command = peek();
    size_t cursor = pos + 1;
    std::string flags = "-";
    bool associative = false;
    bool indexed = false;
    while (cursor < tokens.size() && tokens[cursor] != "--" &&
           tokens[cursor].size() > 1 && tokens[cursor][0] == '-') {
      const std::string &option = tokens[cursor];
      associative = associative || option.find('A') != std::string::npos;
      indexed = indexed || option.find('a') != std::string::npos;
      flags += option.substr(1);
      ++cursor;
    }
    if (cursor + 1 >= tokens.size() || tokens[cursor + 1] != "(") return nullptr;
    if (command == "readonly") {
      for (size_t flag = 1; flag < flags.size(); ++flag) {
        if (flags[flag] != 'a' && flags[flag] != 'A')
          throw std::runtime_error(std::string("readonly: invalid option -- '") +
                                   flags[flag] + "'");
      }
    }
    if (command == "readonly" && !associative && !indexed) return nullptr;
    const std::string assignment = tokens[cursor];
    const size_t append_at = assignment.find("+=");
    const bool append = append_at != std::string::npos;
    const size_t equal = append ? append_at : assignment.find('=');
    if (equal == std::string::npos ||
        assignment.substr(equal + (append ? 2 : 1)).size() != 0) return nullptr;
    const std::string name = assignment.substr(0, equal);
    if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_'))
      throw std::runtime_error("Invalid array name");
    for (char ch : name)
      if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'))
        throw std::runtime_error("Invalid array name");

    size_t entry_cursor = cursor + 2;
    std::vector<std::string> entries;
    while (entry_cursor < tokens.size() && tokens[entry_cursor] != ")")
      entries.push_back(tokens[entry_cursor++]);
    if (entry_cursor >= tokens.size())
      throw std::runtime_error("Missing ) in array assignment");
    pos = entry_cursor + 1;
    if (!associative && !indexed) flags += 'a';
    return std::make_unique<ArrayDeclarationNode>(command, flags, name,
                                                  std::move(entries), associative, append);
  }

  std::unique_ptr<ASTNode> parse_conditional() {
    consume(); // [[
    std::vector<std::string> expression;
    while (pos < tokens.size() && peek() != "]]" ) {
      expression.push_back(consume());
      if (expression.back() != "=~") continue;
      std::string regex_pattern;
      while (pos < tokens.size() && peek() != "]]" && peek() != "&&" && peek() != "||") {
        if (!regex_pattern.empty() && pos > 0 && tokens[pos].source_begin > tokens[pos - 1].source_end)
          regex_pattern += source.substr(tokens[pos - 1].source_end,
                                         tokens[pos].source_begin - tokens[pos - 1].source_end);
        regex_pattern += consume();
      }
      if (!regex_pattern.empty()) expression.push_back(std::move(regex_pattern));
    }
    if (!match("]]")) throw std::runtime_error("Missing ]] for conditional expression");
    if (expression.empty()) throw std::runtime_error("Empty [[ ]] conditional expression");
    return std::make_unique<ConditionalNode>(std::move(expression));
  }

public:
  Parser(const std::vector<ShellToken> &t, const std::string &input)
      : tokens(t), source(input) {}
  bool at_end() const { return pos >= tokens.size(); }
  size_t position() const { return pos; }
  std::string next_token() const { return peek(); }
  void validate_lexical_tokens() {
    for (pos = 0; pos < tokens.size(); ++pos) {
      for (const auto &[marker, description] : {
               std::pair<char, const char *>{'\x01', "Unterminated single quote"},
               {'\x02', "Unterminated double quote"},
               {'\x03', "Missing closing ) for command substitution"}}) {
        const size_t count = std::count(tokens[pos].begin(), tokens[pos].end(), marker);
        if (count % 2) throw std::runtime_error(description);
      }
    }
    pos = 0;
  }

  std::unique_ptr<ASTNode> parse_command() {
    const size_t command_start = pos;
    auto node = std::make_unique<CommandNode>();
    auto take_dynamic_descriptor_variable = [&](size_t redirection_token_index) {
      std::string descriptor_variable;
      if (redirection_token_index == 0 || node->args.empty() ||
          tokens[redirection_token_index - 1].source_end !=
              tokens[redirection_token_index].source_begin)
        return descriptor_variable;
      const std::string &candidate = node->args.back();
      if (candidate.size() <= 2 || candidate.front() != '{' || candidate.back() != '}')
        return descriptor_variable;
      const std::string name = candidate.substr(1, candidate.size() - 2);
      if (name.empty() ||
          !(std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_') ||
          !std::all_of(name.begin() + 1, name.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '_';
          }))
        return descriptor_variable;
      descriptor_variable = name;
      node->args.pop_back();
      if (!node->arg_locations.empty()) node->arg_locations.pop_back();
      return descriptor_variable;
    };
    while (pos < tokens.size()) {
      std::string t = peek();
      if (t == "|" || t == "|&" || t == ";" || t == ";;" || t == ";&" || t == ";;&" || t == "&" || t == "&&" || t == "||" || t == ")" || t == "}")
        break;
      const bool reserved_terminator = node->args.empty() &&
          (t == "then" || t == "elif" || t == "else" || t == "fi" ||
           t == "do" || t == "done" || t == "in" || t == "esac");
      if (reserved_terminator) break;
      const size_t numeric_operator_start = t.find_first_not_of("0123456789");
      const std::string numeric_operator = numeric_operator_start == std::string::npos
          ? std::string() : t.substr(numeric_operator_start);
      const bool numeric_redirection = numeric_operator_start != std::string::npos &&
          numeric_operator_start > 0 &&
          (numeric_operator == "<" || numeric_operator == ">" ||
           numeric_operator == ">>" || numeric_operator == ">|" ||
           numeric_operator == "<>" || numeric_operator == "<&" ||
           numeric_operator == ">&");
      if (t == "<<<" || numeric_operator == "<<<") {
        const size_t redirection_token_index = pos;
        const auto op = consume();
        int descriptor = -1;
        std::string type = op;
        if (numeric_operator_start != std::string::npos &&
            numeric_operator_start > 0 && numeric_operator == "<<<") {
          const size_t operator_start = type.find_first_not_of("0123456789");
          try {
            descriptor = static_cast<int>(std::stol(type.substr(0, operator_start)));
          } catch (...) {
            throw std::runtime_error("Invalid file descriptor in redirection");
          }
          type.erase(0, operator_start);
        }
        const std::string descriptor_variable = descriptor < 0
            ? take_dynamic_descriptor_variable(redirection_token_index) : std::string();
        if (pos >= tokens.size() || peek() == ";" || peek() == "|" ||
            peek() == "&&" || peek() == "||")
          throw std::runtime_error("Missing word after here-string operator");
        const auto &target = tokens[pos];
        node->redirections.push_back({type, consume(), true, true,
                                      target.line, target.column, true, descriptor,
                                      descriptor_variable});
        continue;
      } else if ((t == "<" || t == ">") && pos + 1 < tokens.size() && tokens[pos + 1] == "(")
        throw std::runtime_error("syntax error near unexpected token `('");
      if (t == "(")
        throw std::runtime_error("Unsupported compound shell syntax: (");
      if (node->args.empty() && (t == "{" || t == "("))
        throw std::runtime_error("Unsupported compound shell syntax: " + t);
      if (node->args.empty() && (t == "[[" || t == "]]" || t == "coproc"))
        throw std::runtime_error("Unsupported shell syntax: " + t);

      if (t == "<<" || t == "<<-" || numeric_operator == "<<" ||
          numeric_operator == "<<-") {
        const size_t redirection_token_index = pos;
        std::string type = consume();
        int descriptor = -1;
        if (numeric_operator_start != std::string::npos &&
            numeric_operator_start > 0 &&
            (numeric_operator == "<<" || numeric_operator == "<<-")) {
          const size_t operator_start = type.find_first_not_of("0123456789");
          try {
            descriptor = static_cast<int>(std::stol(type.substr(0, operator_start)));
          } catch (...) {
            throw std::runtime_error("Invalid file descriptor in redirection");
          }
          type.erase(0, operator_start);
        }
        const std::string descriptor_variable = descriptor < 0
            ? take_dynamic_descriptor_variable(redirection_token_index) : std::string();
        if (pos + 1 >= tokens.size()) throw std::runtime_error("Missing here-document delimiter or body");
        const ShellToken delimiter_token = tokens[pos];
        consume();
        const std::string body = consume();
        bool delimiter_quoted = delimiter_token.quoted;
        for (char ch : delimiter_token)
          if (ch == '\x01' || ch == '\x02' || ch == '\x0b')
            delimiter_quoted = true;
        bool expand_document = false;
        std::string document_content;
        if (!body.empty() && body[0] == '\x06') {
          std::string wanted;
          for (size_t i = 0; i < delimiter_token.size(); ++i) {
            const char ch = delimiter_token[i];
            if (ch == '\x01' || ch == '\x02') continue;
            if (ch == '\x0b' && i + 1 < delimiter_token.size()) {
              wanted += delimiter_token[++i];
              continue;
            }
            wanted += ch;
          }
          std::cerr << "dax: warning: here-document at line "
                    << delimiter_token.line
                    << " delimited by end-of-file (wanted '" << wanted << "')\n";
          document_content = body.substr(1);
          expand_document = !delimiter_quoted;
        } else if (!body.empty() && (body[0] == '\x04' || body[0] == '\x05')) {
          document_content = body.substr(1);
          expand_document = body[0] == '\x04';
        } else {
          throw std::runtime_error("Invalid here-document body");
        }
        node->redirections.push_back({type, document_content, true, expand_document,
                                      delimiter_token.line, delimiter_token.column,
                                      false, descriptor, descriptor_variable});
      } else if (t == "<" || t == "<>" || t == ">" || t == ">|" || t == ">>" || t == "&>" || t == "&>>" || t == ">&" || t == "<&" ||
                 numeric_redirection) {
        const size_t redirection_token_index = pos;
        std::string type = consume();
        int descriptor = -1;
        const size_t operator_start = type.find_first_not_of("0123456789");
        if (operator_start != std::string::npos && operator_start > 0) {
          try {
            const long parsed = std::stol(type.substr(0, operator_start));
            if (parsed > INT_MAX) throw std::out_of_range("file descriptor");
            descriptor = static_cast<int>(parsed);
          } catch (...) {
            throw std::runtime_error("Invalid file descriptor in redirection");
          }
          type.erase(0, operator_start);
        }
        const std::string descriptor_variable = descriptor < 0
            ? take_dynamic_descriptor_variable(redirection_token_index) : std::string();
        if (pos >= tokens.size()) {
          throw std::runtime_error("Missing filename for redirection");
        }
        const auto &target = tokens[pos];
        node->redirections.push_back({type, consume(), false, false,
                                      target.line, target.column, false, descriptor,
                                      descriptor_variable});
      } else {
        const auto &source = tokens[pos];
        node->args.push_back(consume());
        node->arg_locations.emplace_back(source.line, source.column);
      }
    }
    if (node->args.empty() && node->redirections.empty())
      return nullptr;
    if (pos > command_start) {
      const size_t begin = tokens[command_start].source_begin;
      const size_t end = tokens[pos - 1].source_end;
      if (begin <= end && end <= source.size())
        node->raw_command = source.substr(begin, end - begin);
    }
    return node;
  }

  std::unique_ptr<ASTNode> parse_pipeline() {
    bool negate_before_time = false;
    while (match("!")) negate_before_time = !negate_before_time;
    const bool coprocess = match("coproc");
    std::string coprocess_name = "COPROC";
    if (coprocess && pos + 1 < tokens.size() && tokens[pos + 1] == "{") {
      const std::string candidate = peek();
      if (!candidate.empty() &&
          (std::isalpha(static_cast<unsigned char>(candidate.front())) || candidate.front() == '_') &&
          std::all_of(candidate.begin() + 1, candidate.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '_';
          })) {
        coprocess_name = consume();
      }
    }
    bool timed = match("time");
    bool portable_time = false;
    if (timed && peek() == "-p") { consume(); portable_time = true; }
    bool negate_after_time = false;
    if (timed) while (match("!")) negate_after_time = !negate_after_time;
    auto attach_raw_command = [&](std::unique_ptr<ASTNode> node, size_t start) {
      if (node && start < pos && start < tokens.size()) {
        const size_t begin = tokens[start].source_begin;
        const size_t end = tokens[pos - 1].source_end;
        if (begin <= end && end <= source.size())
          node->raw_command = source.substr(begin, end - begin);
      }
      return node;
    };
    auto begins_redirection = [&]() {
      if (pos >= tokens.size()) return false;
      const std::string &next = peek();
      if (next.size() > 2 && next.front() == '{' && next.back() == '}' &&
          pos + 1 < tokens.size() &&
          tokens[pos].source_end == tokens[pos + 1].source_begin) {
        const std::string name = next.substr(1, next.size() - 2);
        const bool valid_name =
            (std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_') &&
            std::all_of(name.begin() + 1, name.end(), [](unsigned char ch) {
              return std::isalnum(ch) || ch == '_';
            });
        if (valid_name) {
          const std::string &operator_token = tokens[pos + 1];
          const size_t operator_start = operator_token.find_first_not_of("0123456789");
          const std::string operator_suffix = operator_start == std::string::npos
              ? std::string() : operator_token.substr(operator_start);
          if (operator_suffix == "<" || operator_suffix == "<>" ||
              operator_suffix == ">" || operator_suffix == ">>" ||
              operator_suffix == ">|" || operator_suffix == "<&" ||
              operator_suffix == ">&" || operator_suffix == "<<" ||
              operator_suffix == "<<-" || operator_suffix == "<<<")
            return true;
        }
      }
      const size_t first_non_digit = next.find_first_not_of("0123456789");
      const std::string suffix = first_non_digit == std::string::npos
          ? std::string() : next.substr(first_non_digit);
      return next == "<" || next == ">" || next == ">>" || next == ">|" ||
             next == "&>" || next == "&>>" || next == ">&" || next == "<&" ||
             next == "<>" ||
             next == "<<" || next == "<<-" || next == "<<<" ||
             suffix == "<" || suffix == ">" || suffix == ">>" ||
             suffix == ">|" || suffix == ">&" || suffix == "<&" ||
             suffix == "<>" ||
             suffix == "<<" || suffix == "<<-" || suffix == "<<<";
    };
    auto attach_compound_redirections = [&](std::unique_ptr<ASTNode> node) {
      if (!node || !begins_redirection()) return node;
      auto parsed_redirections = parse_command();
      auto *command = dynamic_cast<CommandNode *>(parsed_redirections.get());
      if (!command || !command->args.empty() || command->redirections.empty())
        throw std::runtime_error("Unexpected word after compound command redirection");
      return std::unique_ptr<ASTNode>(std::make_unique<RedirectionNode>(
          std::move(node), std::move(command->redirections)));
    };
    auto parse_unit = [&]() -> std::unique_ptr<ASTNode> {
      const size_t unit_start = pos;
      if (auto array_assignment = parse_array_assignment())
        return attach_raw_command(attach_compound_redirections(std::move(array_assignment)), unit_start);
      if (auto array_declaration = parse_array_declaration())
        return attach_raw_command(attach_compound_redirections(std::move(array_declaration)), unit_start);
      if (peek() == "[[")
        return attach_raw_command(attach_compound_redirections(parse_conditional()), unit_start);
      if (peek() == "(" && pos + 1 < tokens.size() && tokens[pos + 1] == "(") {
        auto expression = parse_arithmetic_group();
        return attach_raw_command(
            attach_compound_redirections(
                std::make_unique<ArithmeticCommandNode>(std::move(*expression))), unit_start);
      }
      if (peek() == "if") return attach_raw_command(attach_compound_redirections(parse_if()), unit_start);
      if (peek() == "case") return attach_raw_command(attach_compound_redirections(parse_case()), unit_start);
      if (peek() == "for") return attach_raw_command(attach_compound_redirections(parse_for()), unit_start);
      if (peek() == "select") return attach_raw_command(attach_compound_redirections(parse_select()), unit_start);
      if (peek() == "while") return attach_raw_command(attach_compound_redirections(parse_loop(false)), unit_start);
      if (peek() == "until") return attach_raw_command(attach_compound_redirections(parse_loop(true)), unit_start);
      bool subshell = false;
      std::string closing;
      if (match("(")) { subshell = true; closing = ")"; }
      else if (match("{")) { closing = "}"; }
      else return attach_raw_command(parse_command(), unit_start);
      auto body = parse_list();
      if (!subshell) {
        if (!body) throw std::runtime_error("Empty brace command group");
        if (pos > 0 && tokens[pos - 1] != ";" && tokens[pos - 1] != "&")
          throw std::runtime_error("Expected ; or newline before } in command group");
      }
      if (!match(closing)) throw std::runtime_error("Missing closing " + closing + " for command group");
      auto group = std::make_unique<GroupNode>(std::move(body), subshell);
      return attach_raw_command(
          attach_compound_redirections(std::move(group)), unit_start);
    };
    auto left = parse_unit();
    if (!left) {
      if (coprocess || negate_before_time || negate_after_time)
        throw std::runtime_error(coprocess ? "Missing command after coproc" : "Missing command after !");
      return nullptr;
    }

    while (peek() == "|" || peek() == "|&") {
      const bool merge_stderr = consume() == "|&";
      auto right = parse_unit();
      if (!right) {
        throw std::runtime_error("Missing command after pipe");
      }
      left = std::make_unique<PipeNode>(std::move(left), std::move(right), merge_stderr);
    }
    if (coprocess) left = std::make_unique<CoprocNode>(std::move(coprocess_name), std::move(left));
    if (negate_after_time) left = std::make_unique<NotNode>(std::move(left));
    if (timed) left = std::make_unique<TimeNode>(std::move(left), portable_time);
    if (negate_before_time) left = std::make_unique<NotNode>(std::move(left));
    return left;
  }

  std::unique_ptr<ASTNode> parse_and_or() {
    auto left = parse_pipeline();
    if (!left) return nullptr;
    while (peek() == "&&" || peek() == "||") {
      const bool is_and = consume() == "&&";
      auto right = parse_pipeline();
      if (!right) throw std::runtime_error("Missing command after logical operator");
      left = std::make_unique<AndOrNode>(std::move(left), std::move(right), is_and);
    }
    return left;
  }

  std::unique_ptr<ASTNode> parse_list() {
    while (match(";")) {}
    size_t current_start = pos;
    auto current = parse_and_or();
    if (!current)
      return nullptr;
    std::vector<std::unique_ptr<ASTNode>> commands;
    while (pos < tokens.size() && peek() != ")" && peek() != "}" &&
           (peek() == ";" || peek() == "&")) {
      const size_t command_end = pos > 0 ? tokens[pos - 1].source_end : 0;
      bool background = match("&");
      if (!background)
        match(";");
      if (background) {
        const size_t begin = current_start < tokens.size()
                                 ? tokens[current_start].source_begin : 0;
        std::string text = command_end >= begin && command_end <= source.size()
                               ? source.substr(begin, command_end - begin)
                               : std::string();
        current = std::make_unique<BackgroundNode>(std::move(current), std::move(text));
        while (match(";")) {}
      }
      commands.push_back(std::move(current));
      current_start = pos;
      auto right = parse_and_or();
      if (!right) break;
      current = std::move(right);
    }
    if (current) commands.push_back(std::move(current));
    auto root = std::move(commands.front());
    for (size_t i = 1; i < commands.size(); ++i)
      root = std::make_unique<ListNode>(std::move(root), std::move(commands[i]));
    return root;
  }
};

std::unique_ptr<ASTNode> parse(const std::vector<ShellToken> &tokens,
                               const std::string &source) {
  Parser parser(tokens, source);
  try {
    parser.validate_lexical_tokens();
    auto result = parser.parse_list();
    if (!parser.at_end())
      throw std::runtime_error("Unexpected token: " + parser.next_token());
    return result;
  } catch (const std::exception &error) {
    const ShellToken *source = nullptr;
    if (!parser.at_end() && parser.position() < tokens.size())
      source = &tokens[parser.position()];
    else if (!tokens.empty())
      source = &tokens.back();
    if (source)
      throw std::runtime_error("line " + std::to_string(source->line) +
                               ", column " + std::to_string(source->column) +
                               ": " + error.what());
    throw;
  }
}
