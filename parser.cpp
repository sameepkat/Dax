#include "parser.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
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
      node->clauses.emplace_back(std::move(patterns), std::move(body));
      if (match(";;")) {
        while (match(";")) {}
        continue;
      }
      if (peek() != "esac") throw std::runtime_error("Expected ;; or esac after case clause");
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

  std::unique_ptr<ASTNode> parse_for() {
    consume();
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

  std::unique_ptr<ASTNode> parse_array_assignment() {
    if (pos + 1 >= tokens.size()) return nullptr;
    const std::string lhs = peek();
    const size_t append_at = lhs.find("+=");
    const size_t equal_at = append_at == std::string::npos ? lhs.find('=') : append_at;
    if (equal_at == std::string::npos || tokens[pos + 1] != "(") return nullptr;
    auto node = std::make_unique<ArrayAssignmentNode>();
    node->append = append_at != std::string::npos;
    std::string variable = lhs.substr(0, equal_at);
    if (!variable.empty() && variable.back() == ']') {
      const size_t bracket = variable.find('[');
      if (bracket == std::string::npos || bracket == 0)
        throw std::runtime_error("Invalid indexed array assignment");
      const std::string index = variable.substr(bracket + 1, variable.size() - bracket - 2);
      if (index.empty()) throw std::runtime_error("Array index cannot be empty");
      char *end = nullptr;
      unsigned long parsed = std::strtoul(index.c_str(), &end, 10);
      if (end == index.c_str() || *end || parsed > 1000000)
        throw std::runtime_error("Array index must be an integer from 0 to 1000000");
      node->name = variable.substr(0, bracket);
      node->index = static_cast<size_t>(parsed);
      node->indexed = true;
    } else {
      node->name = std::move(variable);
    }
    if (node->name.empty() || !(std::isalpha(static_cast<unsigned char>(node->name[0])) || node->name[0] == '_'))
      throw std::runtime_error("Invalid array name");
    for (char ch : node->name)
      if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'))
        throw std::runtime_error("Invalid array name");
    pos += 2;
    while (pos < tokens.size() && peek() != ")") node->values.push_back(consume());
    if (!match(")")) throw std::runtime_error("Missing ) in array assignment");
    return node;
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
    auto node = std::make_unique<CommandNode>();
    while (pos < tokens.size()) {
      std::string t = peek();
      if (t == "|" || t == ";" || t == ";;" || t == "&" || t == "&&" || t == "||" || t == ")" || t == "}")
        break;
      const bool reserved_terminator = node->args.empty() &&
          (t == "then" || t == "elif" || t == "else" || t == "fi" ||
           t == "do" || t == "done" || t == "in" || t == "esac");
      if (reserved_terminator) break;
      if (t == "<<<") {
        const auto op = consume();
        if (pos >= tokens.size() || peek() == ";" || peek() == "|" ||
            peek() == "&&" || peek() == "||")
          throw std::runtime_error("Missing word after here-string operator");
        const auto &target = tokens[pos];
        node->redirections.push_back({op, consume(), true, true,
                                      target.line, target.column, true});
        continue;
      } else if ((t == "<" || t == ">") && pos + 1 < tokens.size() && tokens[pos + 1] == "(")
        throw std::runtime_error("Process substitution is not supported yet");
      if (t == "(")
        throw std::runtime_error("Unsupported compound shell syntax: (");
      if (node->args.empty() && (t == "{" || t == "("))
        throw std::runtime_error("Unsupported compound shell syntax: " + t);
      if (node->args.empty() && (t == "[[" || t == "]]" || t == "select" ||
                                 t == "coproc" || t == "time"))
        throw std::runtime_error("Unsupported shell syntax: " + t);

      if (t == "<<" || t == "<<-") {
        const std::string type = consume();
        if (pos + 1 >= tokens.size()) throw std::runtime_error("Missing here-document delimiter or body");
        consume(); // delimiter; the tokenizer already matched its closing line
        const std::string body = consume();
        if (body.size() == 1 && body[0] == '\x06')
          throw std::runtime_error("Missing here-document delimiter line");
        if (body.empty() || (body[0] != '\x04' && body[0] != '\x05'))
          throw std::runtime_error("Invalid here-document body");
        node->redirections.push_back({type, body.substr(1), true, body[0] == '\x04',
                                      tokens[pos - 2].line, tokens[pos - 2].column});
      } else if (t == "<" || t == ">" || t == ">|" || t == ">>" || t == "2>" || t == "2>>" || t == "&>" || t == "&>>" || t == ">&" || t == "2>&" || t == "<&") {
        std::string type = consume();
        if (pos >= tokens.size()) {
          throw std::runtime_error("Missing filename for redirection");
        }
        const auto &target = tokens[pos];
        node->redirections.push_back({type, consume(), false, false,
                                      target.line, target.column});
      } else {
        const auto &source = tokens[pos];
        node->args.push_back(consume());
        node->arg_locations.emplace_back(source.line, source.column);
      }
    }
    if (node->args.empty() && node->redirections.empty())
      return nullptr;
    return node;
  }

  std::unique_ptr<ASTNode> parse_pipeline() {
    bool negate = false;
    while (match("!")) negate = !negate;
    auto parse_unit = [&]() -> std::unique_ptr<ASTNode> {
      if (auto array_assignment = parse_array_assignment()) return array_assignment;
      if (peek() == "if") return parse_if();
      if (peek() == "case") return parse_case();
      if (peek() == "for") return parse_for();
      if (peek() == "while") return parse_loop(false);
      if (peek() == "until") return parse_loop(true);
      bool subshell = false;
      std::string closing;
      if (match("(")) { subshell = true; closing = ")"; }
      else if (match("{")) { closing = "}"; }
      else return parse_command();
      auto body = parse_list();
      if (!match(closing)) throw std::runtime_error("Missing closing " + closing + " for command group");
      return std::make_unique<GroupNode>(std::move(body), subshell);
    };
    auto left = parse_unit();
    if (!left) {
      if (negate) throw std::runtime_error("Missing command after !");
      return nullptr;
    }

    while (match("|")) {
      auto right = parse_unit();
      if (!right) {
        throw std::runtime_error("Missing command after pipe");
      }
      left = std::make_unique<PipeNode>(std::move(left), std::move(right));
    }
    if (negate) left = std::make_unique<NotNode>(std::move(left));
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
