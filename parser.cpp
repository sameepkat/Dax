#include "parser.hpp"
#include "builtins.hpp"
#include <stdexcept>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <cstring>
#include <vector>


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

int CommandNode::execute() {
    // 1. Handle Redirections
    int saved_stdin = dup(STDIN_FILENO);
    int saved_stdout = dup(STDOUT_FILENO);

    for (const auto& red : redirections) {
        int fd;
        if (red.type == "<") {
            fd = open(red.filename.c_str(), O_RDONLY);
            if (fd < 0) {
                perror("open input");
                return 1;
            }
            dup2(fd, STDIN_FILENO);
            close(fd);
        } else if (red.type == ">") {
            fd = open(red.filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                perror("open output");
                return 1;
            }
            dup2(fd, STDOUT_FILENO);
            close(fd);
        } else if (red.type == ">>") {
            fd = open(red.filename.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd < 0) {
                perror("open append");
                return 1;
            }
            dup2(fd, STDOUT_FILENO);
            close(fd);
        }
    }

    // 2. Handle Builtins
    if (!args.empty() && handle_builtin(args)) {
        // Restore FDs
        dup2(saved_stdin, STDIN_FILENO);
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdin);
        close(saved_stdout);
        return 0;
    }

    // 3. Prepare arguments
    std::vector<char*> c_args;
    for (const auto& arg : args) {
        c_args.push_back(const_cast<char*>(arg.c_str()));
    }
    c_args.push_back(nullptr);

    // 3. Fork and Exec
    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        // Reset signals to default
        signal(SIGINT, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);

        execvp(c_args[0], c_args.data());
        perror("execvp");
        exit(1);
    } else if (pid < 0) {
        perror("fork");
        return 1;
    } else {
        // Parent process
        int status;
        waitpid(pid, &status, 0);
        
        // Restore FDs
        dup2(saved_stdin, STDIN_FILENO);
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdin);
        close(saved_stdout);

        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        return 1;
    }
}

int PipeNode::execute() {
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return 1;
    }

    pid_t pid1 = fork();
    if (pid1 == 0) {
        // Left child (writer)
        // Reset signals to default
        signal(SIGINT, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);

        close(pipefd[0]); // Close read end
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        
        if (left) exit(left->execute());
        exit(0);
    }

    pid_t pid2 = fork();
    if (pid2 == 0) {
        // Right child (reader)
        // Reset signals to default
        signal(SIGINT, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);

        close(pipefd[1]); // Close write end
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);

        if (right) exit(right->execute());
        exit(0);
    }

    // Parent
    close(pipefd[0]);
    close(pipefd[1]);

    int status1, status2;
    waitpid(pid1, &status1, 0);
    waitpid(pid2, &status2, 0);

    if (WIFEXITED(status2)) {
        return WEXITSTATUS(status2);
    }
    return 1;
}

int ListNode::execute() {
    int status = 0;
    if (left) status = left->execute();
    if (right) status = right->execute();
    return status;
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
