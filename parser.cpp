#include "parser.hpp"
#include <stdexcept>

void CommandNode::print(int indent) const {
    std::string padding(indent, ' ');
    std::cout << padding << "Command: ";
    for (const auto& arg : args) {
        std::cout << arg << " ";
    }
    for (const auto& red : redirections) {
        std::cout << "[" << red.type << " " << red.filename << "] ";
    }
    std::cout << "\n";
}

void PipeNode::print(int indent) const {
    std::string padding(indent, ' ');
    std::cout << padding << "Pipe:\n";
    if (left) left->print(indent + 2);
    if (right) right->print(indent + 2);
}

void ListNode::print(int indent) const {
    std::string padding(indent, ' ');
    std::cout << padding << "List:\n";
    if (left) left->print(indent + 2);
    if (right) right->print(indent + 2);
}

class Parser {
    const std::vector<std::string>& tokens;
    size_t pos = 0;

    std::string peek() const {
        if (pos < tokens.size()) return tokens[pos];
        return "";
    }

    std::string consume() {
        if (pos < tokens.size()) return tokens[pos++];
        return "";
    }

    bool match(const std::string& type) {
        if (peek() == type) {
            consume();
            return true;
        }
        return false;
    }

public:
    Parser(const std::vector<std::string>& t) : tokens(t) {}

    std::unique_ptr<ASTNode> parse_command() {
        auto node = std::make_unique<CommandNode>();
        while (pos < tokens.size()) {
            std::string t = peek();
            if (t == "|" || t == ";") break;

            if (t == "<" || t == ">" || t == ">>") {
                std::string type = consume();
                if (pos >= tokens.size()) {
                    throw std::runtime_error("Missing filename for redirection");
                }
                node->redirections.push_back({type, consume()});
            } else {
                node->args.push_back(consume());
            }
        }
        if (node->args.empty() && node->redirections.empty()) return nullptr;
        return node;
    }

    std::unique_ptr<ASTNode> parse_pipe() {
        auto left = parse_command();
        if (!left) return nullptr;

        while (match("|")) {
            auto right = parse_command();
            if (!right) {
                 throw std::runtime_error("Missing command after pipe");
            }
            left = std::make_unique<PipeNode>(std::move(left), std::move(right));
        }
        return left;
    }

    std::unique_ptr<ASTNode> parse_list() {
        auto left = parse_pipe();
        if (!left) return nullptr;

        while (match(";")) {
            auto right = parse_pipe();
            // Allow trailing semicolon (right might be null)
            if (right) {
                left = std::make_unique<ListNode>(std::move(left), std::move(right));
            }
        }
        return left;
    }
};

std::unique_ptr<ASTNode> parse(const std::vector<std::string>& tokens) {
    Parser parser(tokens);
    return parser.parse_list();
}
