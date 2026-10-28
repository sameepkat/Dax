#include "parser.hpp"
#include "builtins.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

void CommandNode::print(int indent) const {
  std::string padding(indent, ' ');
  std::cout << padding << "Command: ";
  for (const auto &arg : args) {
    std::cout << arg << " ";
  }
  for (const auto &red : redirections) {
    std::cout << "[" << red.type << " " << red.filename << "] ";
  }
  std::cout << "\n";
}

void PipeNode::print(int indent) const {
  std::string padding(indent, ' ');
  std::cout << padding << "Pipe:\n";
  if (left)
    left->print(indent + 2);
  if (right)
    right->print(indent + 2);
}

int CommandNode::execute() {
  if (args.empty() && redirections.empty())
    return 0;
  std::vector<std::string> expanded = args;
  begin_word_expansion();
  expand_words(expanded);
  if (!word_expansion_succeeded())
    return 1;
  size_t assignment_count = 0;
  while (assignment_count < expanded.size() &&
         expanded[assignment_count].find('=') != std::string::npos)
    ++assignment_count;
  if (!expanded.empty() && assignment_count == expanded.size()) {
    for (const auto &assignment : expanded) {
      const auto p = assignment.find('=');
      if (!set_shell_variable(assignment.substr(0, p),
                              assignment.substr(p + 1)))
        return 1;
    }
    return 0;
  }
  std::vector<std::pair<std::string, std::optional<std::string>>>
      saved_environment;
  for (size_t i = 0; i < assignment_count; ++i) {
    const auto p = expanded[i].find('=');
    std::string name = expanded[i].substr(0, p);
    if (shell_variable_readonly(name)) {
      std::cerr << name << ": readonly variable\n";
      return 1;
    }
    const char *old = getenv(name.c_str());
    saved_environment.push_back(
        {name, old ? std::optional<std::string>(old) : std::nullopt});
    if (setenv(name.c_str(), expanded[i].substr(p + 1).c_str(), 1) != 0)
      return 1;
  }
  expanded.erase(expanded.begin(), expanded.begin() + assignment_count);
  auto restore_environment = [&]() {
    for (const auto &[name, value] : saved_environment) {
      if (value)
        setenv(name.c_str(), value->c_str(), 1);
      else
        unsetenv(name.c_str());
    }
  };
  // 1. Handle Redirections
  int saved_stdin = dup(STDIN_FILENO);
  int saved_stdout = dup(STDOUT_FILENO);

  for (const auto &red : redirections) {
    int fd;
    std::vector<std::string> target = {red.filename};
    expand_words(target);
    if (!word_expansion_succeeded()) {
      dup2(saved_stdin, STDIN_FILENO);
      dup2(saved_stdout, STDOUT_FILENO);
      close(saved_stdin);
      close(saved_stdout);
      restore_environment();
      return 1;
    }
    const std::string &filename = target.front();
    if (red.type == "<") {
      fd = open(filename.c_str(), O_RDONLY);
      if (fd < 0) {
        perror("open input");
        dup2(saved_stdin, STDIN_FILENO);
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdin);
        close(saved_stdout);
        restore_environment();
        return 1;
      }
      dup2(fd, STDIN_FILENO);
      close(fd);
    } else if (red.type == ">") {
      fd = open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd < 0) {
        perror("open output");
        dup2(saved_stdin, STDIN_FILENO);
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdin);
        close(saved_stdout);
        restore_environment();
        return 1;
      }
      dup2(fd, STDOUT_FILENO);
      close(fd);
    } else if (red.type == ">>") {
      fd = open(filename.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
      if (fd < 0) {
        perror("open append");
        dup2(saved_stdin, STDIN_FILENO);
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdin);
        close(saved_stdout);
        restore_environment();
        return 1;
      }
      dup2(fd, STDOUT_FILENO);
      close(fd);
    }
  }

  // 2. Handle Builtins
  if (!expanded.empty())
    if (auto status = handle_builtin(expanded)) {
      // Restore FDs
      std::cout.flush();
      dup2(saved_stdin, STDIN_FILENO);
      dup2(saved_stdout, STDOUT_FILENO);
      close(saved_stdin);
      close(saved_stdout);
      restore_environment();
      return *status;
    }

  if (expanded.empty()) {
    std::cout.flush();
    dup2(saved_stdin, STDIN_FILENO);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdin);
    close(saved_stdout);
    restore_environment();
    return 0;
  }

  // 3. Prepare arguments
  std::vector<char *> c_args;
  for (const auto &arg : expanded) {
    c_args.push_back(const_cast<char *>(arg.c_str()));
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
    int exec_error = errno;
    perror("execvp");
    _exit(exec_error == ENOENT ? 127 : 126);
  } else if (pid < 0) {
    perror("fork");
    dup2(saved_stdin, STDIN_FILENO);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdin);
    close(saved_stdout);
    restore_environment();
    return 1;
  } else {
    // Parent process
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }

    // Restore FDs
    dup2(saved_stdin, STDIN_FILENO);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdin);
    close(saved_stdout);

    if (WIFEXITED(status)) {
      restore_environment();
      return WEXITSTATUS(status);
    }
    restore_environment();
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

    if (left)
      exit(left->execute());
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

    if (right)
      exit(right->execute());
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
  if (left)
    status = left->execute();
  set_last_status(status);
  apply_errexit(status);
  if (right && !shell_should_exit() && !shell_should_return())
    status = right->execute();
  set_last_status(status);
  apply_errexit(status);
  return status;
}

void BackgroundNode::print(int indent) const {
  std::cout << std::string(indent, ' ') << "Background:\n";
  if (child)
    child->print(indent + 2);
}

int BackgroundNode::execute() {
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return 1;
  }
  if (pid == 0) {
    setpgid(0, 0);
    signal(SIGINT, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
      int status = child ? child->execute() : 0;
      std::cout.flush();
      _exit(status);
  }
  setpgid(pid, pid);
  remember_job(pid, "(background command)");
  return 0;
}

void ListNode::print(int indent) const {
  std::string padding(indent, ' ');
  std::cout << padding << "List:\n";
  if (left)
    left->print(indent + 2);
  if (right)
    right->print(indent + 2);
}

class Parser {
  const std::vector<std::string> &tokens;
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

public:
  Parser(const std::vector<std::string> &t) : tokens(t) {}

  std::unique_ptr<ASTNode> parse_command() {
    auto node = std::make_unique<CommandNode>();
    while (pos < tokens.size()) {
      std::string t = peek();
      if (t == "|" || t == ";" || t == "&")
        break;

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
    if (node->args.empty() && node->redirections.empty())
      return nullptr;
    return node;
  }

  std::unique_ptr<ASTNode> parse_pipe() {
    auto left = parse_command();
    if (!left)
      return nullptr;

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
    if (!left)
      return nullptr;

    while (pos < tokens.size() && (peek() == ";" || peek() == "&")) {
      bool background = match("&");
      if (!background)
        match(";");
      if (background)
        left = std::make_unique<BackgroundNode>(std::move(left));
      auto right = parse_pipe();
      // Allow trailing semicolon (right might be null)
      if (right) {
        left = std::make_unique<ListNode>(std::move(left), std::move(right));
      }
    }
    return left;
  }
};

std::unique_ptr<ASTNode> parse(const std::vector<std::string> &tokens) {
  Parser parser(tokens);
  return parser.parse_list();
}
