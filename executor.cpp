#include "parser.hpp"
#include "builtins.hpp"
#include "helper.hpp"
#include "janet_commands.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fnmatch.h>
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
  if (!word_expansion_succeeded()) {
    const size_t index = word_expansion_error_index();
    if (index < arg_locations.size())
      std::cerr << "dax: line " << arg_locations[index].first << ", column "
                << arg_locations[index].second << ": word expansion failed\n";
    return 1;
  }
  trace_shell_command(expanded);
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
    return word_expansion_ran_substitution() ? word_expansion_substitution_status() : 0;
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
  int saved_stderr = dup(STDERR_FILENO);

  for (const auto &red : redirections) {
    if (red.here_document) {
      std::string content = red.filename;
      if (red.expand_document) {
        std::vector<std::string> expanded_content = {std::string("\x02") + content + "\x02"};
        expand_words(expanded_content);
        if (!word_expansion_succeeded()) {
          dup2(saved_stdin, STDIN_FILENO); dup2(saved_stdout, STDOUT_FILENO); dup2(saved_stderr, STDERR_FILENO);
          close(saved_stdin); close(saved_stdout); close(saved_stderr); restore_environment();
          return 1;
        }
        content = expanded_content.empty() ? std::string() : expanded_content.front();
      }
      FILE *temporary = tmpfile();
      if (!temporary || fwrite(content.data(), 1, content.size(), temporary) != content.size() ||
          fseek(temporary, 0, SEEK_SET) != 0 || dup2(fileno(temporary), STDIN_FILENO) < 0) {
        perror("here-document");
        if (temporary) fclose(temporary);
        dup2(saved_stdin, STDIN_FILENO); dup2(saved_stdout, STDOUT_FILENO); dup2(saved_stderr, STDERR_FILENO);
        close(saved_stdin); close(saved_stdout); close(saved_stderr); restore_environment();
        return 1;
      }
      fclose(temporary);
      continue;
    }
    int fd;
    std::vector<std::string> target = {red.filename};
    expand_words(target);
    if (!word_expansion_succeeded() || target.size() != 1) {
      if (!word_expansion_succeeded())
        std::cerr << "dax: line " << red.line << ", column " << red.column
                  << ": redirection expansion failed\n";
      dup2(saved_stdin, STDIN_FILENO);
      dup2(saved_stdout, STDOUT_FILENO);
      dup2(saved_stderr, STDERR_FILENO);
      close(saved_stdin);
      close(saved_stdout);
      close(saved_stderr);
      restore_environment();
      return 1;
    }
    const std::string &filename = target.front();
    if (red.type == "2>&" || red.type == ">&" || red.type == "<&") {
      const int destination = red.type == "2>&" ? STDERR_FILENO :
                             red.type == "<&" ? STDIN_FILENO : STDOUT_FILENO;
      if (filename == "-") {
        close(destination);
      } else {
        char *end = nullptr;
        long source = std::strtol(filename.c_str(), &end, 10);
        if (end == filename.c_str() || *end || source < 0 || source > 1024 ||
            dup2(static_cast<int>(source), destination) < 0) {
          std::cerr << "dax: " << red.type << ": bad file descriptor: " << filename << '\n';
          dup2(saved_stdin, STDIN_FILENO); dup2(saved_stdout, STDOUT_FILENO); dup2(saved_stderr, STDERR_FILENO);
          close(saved_stdin); close(saved_stdout); close(saved_stderr); restore_environment();
          return 1;
        }
      }
    } else if (red.type == "<") {
      fd = open(filename.c_str(), O_RDONLY);
      if (fd < 0) {
        perror("open input");
        dup2(saved_stdin, STDIN_FILENO);
        dup2(saved_stdout, STDOUT_FILENO);
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stdin);
        close(saved_stdout);
        close(saved_stderr);
        restore_environment();
        return 1;
      }
      dup2(fd, STDIN_FILENO);
      close(fd);
    } else if (red.type == ">" || red.type == ">|" || red.type == "2>" || red.type == "&>") {
      const int create_flags = O_WRONLY | O_CREAT | O_TRUNC |
                               (red.type != ">|" && shell_noclobber_enabled()
                                    ? O_EXCL : 0);
      fd = open(filename.c_str(), create_flags, 0644);
      if (fd < 0) {
        if (errno == EEXIST && shell_noclobber_enabled())
          std::cerr << "dax: cannot overwrite existing file: " << filename << '\n';
        else
          perror("open output");
        dup2(saved_stdin, STDIN_FILENO);
        dup2(saved_stdout, STDOUT_FILENO);
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stdin);
        close(saved_stdout);
        close(saved_stderr);
        restore_environment();
        return 1;
      }
      if (red.type == "2>") dup2(fd, STDERR_FILENO);
      else {
        dup2(fd, STDOUT_FILENO);
        if (red.type == "&>") dup2(fd, STDERR_FILENO);
      }
      close(fd);
    } else if (red.type == ">>" || red.type == "2>>" || red.type == "&>>") {
      fd = open(filename.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
      if (fd < 0) {
        perror("open append");
        dup2(saved_stdin, STDIN_FILENO);
        dup2(saved_stdout, STDOUT_FILENO);
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stdin);
        close(saved_stdout);
        close(saved_stderr);
        restore_environment();
        return 1;
      }
      if (red.type == "2>>") dup2(fd, STDERR_FILENO);
      else {
        dup2(fd, STDOUT_FILENO);
        if (red.type == "&>>") dup2(fd, STDERR_FILENO);
      }
      close(fd);
    }
  }

  if (!expanded.empty() && expanded[0] == "exec") {
    if (expanded.size() == 1 && redirections.empty()) {
      dup2(saved_stdin, STDIN_FILENO); dup2(saved_stdout, STDOUT_FILENO); dup2(saved_stderr, STDERR_FILENO);
      close(saved_stdin); close(saved_stdout); close(saved_stderr); restore_environment();
      return 0;
    }
    if (expanded.size() == 1) {
      close(saved_stdin); close(saved_stdout); close(saved_stderr);
      restore_environment();
      return 0;
    }
    std::vector<char *> exec_args;
    for (size_t i = 1; i < expanded.size(); ++i) exec_args.push_back(const_cast<char *>(expanded[i].c_str()));
    exec_args.push_back(nullptr);
    fcntl(saved_stdin, F_SETFD, FD_CLOEXEC); fcntl(saved_stdout, F_SETFD, FD_CLOEXEC); fcntl(saved_stderr, F_SETFD, FD_CLOEXEC);
    execvp(exec_args[0], exec_args.data());
    const int exec_error = errno;
    report_command_launch_error(expanded[1], exec_error);
    dup2(saved_stdin, STDIN_FILENO); dup2(saved_stdout, STDOUT_FILENO); dup2(saved_stderr, STDERR_FILENO);
    close(saved_stdin); close(saved_stdout); close(saved_stderr);
    restore_environment();
    return exec_error == ENOENT ? 127 : 126;
  }

  // 2. Handle Builtins
  if (!expanded.empty())
    if (auto status = handle_builtin(expanded)) {
      // Restore FDs
      std::cout.flush();
      dup2(saved_stdin, STDIN_FILENO);
      dup2(saved_stdout, STDOUT_FILENO);
      dup2(saved_stderr, STDERR_FILENO);
      close(saved_stdin);
      close(saved_stdout);
      close(saved_stderr);
      restore_environment();
      return *status;
    }

  if (!expanded.empty()) {
    if (auto status = janet_dispatch_command(expanded)) {
      std::cout.flush();
      std::cerr.flush();
      dup2(saved_stdin, STDIN_FILENO);
      dup2(saved_stdout, STDOUT_FILENO);
      dup2(saved_stderr, STDERR_FILENO);
      close(saved_stdin);
      close(saved_stdout);
      close(saved_stderr);
      restore_environment();
      return *status;
    }
  }

  if (expanded.empty()) {
    std::cout.flush();
    dup2(saved_stdin, STDIN_FILENO);
    dup2(saved_stdout, STDOUT_FILENO);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stdin);
    close(saved_stdout);
    close(saved_stderr);
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
    report_command_launch_error(c_args[0], exec_error);
    _exit(exec_error == ENOENT ? 127 : 126);
  } else if (pid < 0) {
    perror("fork");
    dup2(saved_stdin, STDIN_FILENO);
    dup2(saved_stdout, STDOUT_FILENO);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stdin);
    close(saved_stdout);
    close(saved_stderr);
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
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stdin);
    close(saved_stdout);
    close(saved_stderr);

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
  run_pending_signal_trap();
  set_last_status(status);
  apply_errexit(status);
  if (right && !shell_noexec_enabled() && !shell_should_exit() &&
      !shell_should_return() && !loop_control_pending()) {
    clear_errexit_immune();
    status = right->execute();
  }
  set_last_status(status);
  apply_errexit(status);
  return status;
}

void AndOrNode::print(int indent) const {
  std::cout << std::string(indent, ' ') << (and_mode ? "And:\n" : "Or:\n");
  if (left) left->print(indent + 2);
  if (right) right->print(indent + 2);
}

int AndOrNode::execute() {
  begin_errexit_ignored_context();
  int status = left ? left->execute() : 0;
  end_errexit_ignored_context();
  set_last_status(status);
  if (!shell_noexec_enabled() &&
      ((and_mode && status == 0) || (!and_mode && status != 0))) {
    clear_errexit_immune();
    status = right ? right->execute() : 2;
  } else if (status != 0) {
    mark_errexit_immune();
  }
  set_last_status(status);
  return status;
}

void NotNode::print(int indent) const {
  std::cout << std::string(indent, ' ') << "Not:\n";
  if (child) child->print(indent + 2);
}

int NotNode::execute() {
  begin_errexit_ignored_context();
  int status = child ? child->execute() : 0;
  end_errexit_ignored_context();
  status = status == 0 ? 1 : 0;
  set_last_status(status);
  return status;
}

void GroupNode::print(int indent) const {
  std::cout << std::string(indent, ' ') << (subshell ? "Subshell:\n" : "Group:\n");
  if (child) child->print(indent + 2);
}

int GroupNode::execute() {
  if (!subshell) {
    int status = child ? child->execute() : 0;
    set_last_status(status);
    return status;
  }
  pid_t pid = fork();
  if (pid < 0) { perror("fork"); return 1; }
  if (pid == 0) {
    int status = child ? child->execute() : 0;
    std::cout.flush();
    std::cerr.flush();
    _exit(status & 255);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  int result = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  set_last_status(result);
  return result;
}

void IfNode::print(int indent) const {
  std::cout << std::string(indent, ' ') << "If:\n";
  for (const auto &branch : branches) {
    std::cout << std::string(indent + 2, ' ') << "Condition:\n";
    if (branch.first) branch.first->print(indent + 4);
    if (branch.second) branch.second->print(indent + 4);
  }
  if (otherwise) { std::cout << std::string(indent + 2, ' ') << "Else:\n"; otherwise->print(indent + 4); }
}

int IfNode::execute() {
  for (auto &branch : branches) {
    begin_errexit_ignored_context();
    const int condition_status = branch.first ? branch.first->execute() : 0;
    end_errexit_ignored_context();
    set_last_status(condition_status);
    if (shell_noexec_enabled()) return condition_status;
    if (condition_status == 0) {
      int status = branch.second ? branch.second->execute() : 0;
      set_last_status(status);
      return status;
    }
  }
  int status = otherwise ? otherwise->execute() : 0;
  set_last_status(status);
  return status;
}

void LoopNode::print(int indent) const {
  std::cout << std::string(indent, ' ') << (until ? "Until:\n" : "While:\n");
  if (condition) condition->print(indent + 2);
  if (body) body->print(indent + 2);
}

int LoopNode::execute() {
  int status = 0;
  enter_shell_loop();
  while (!shell_should_exit() && !shell_should_return()) {
    begin_errexit_ignored_context();
    int condition_status = condition ? condition->execute() : 0;
    end_errexit_ignored_context();
    set_last_status(condition_status);
    if (shell_noexec_enabled()) { status = condition_status; break; }
    if (take_loop_break()) break;
    if (take_loop_continue()) continue;
    if (until ? condition_status == 0 : condition_status != 0) break;
    status = body ? body->execute() : 0;
    set_last_status(status);
    if (shell_noexec_enabled()) break;
    if (take_loop_break()) break;
    if (take_loop_continue()) continue;
  }
  leave_shell_loop();
  return status;
}

void ForNode::print(int indent) const {
  std::cout << std::string(indent, ' ') << "For " << variable << ":\n";
  for (const auto &value : values) std::cout << std::string(indent + 2, ' ') << value << '\n';
  if (body) body->print(indent + 2);
}

int ForNode::execute() {
  begin_word_expansion();
  expand_words(values);
  if (!word_expansion_succeeded()) return 1;
  int status = 0;
  enter_shell_loop();
  for (const auto &value : values) {
    if (shell_should_exit() || shell_should_return()) break;
    if (!set_shell_variable(variable, value)) return 1;
    status = body ? body->execute() : 0;
    set_last_status(status);
    if (shell_noexec_enabled()) break;
    if (take_loop_break()) break;
    if (take_loop_continue()) continue;
  }
  leave_shell_loop();
  return status;
}

void CaseNode::print(int indent) const {
  std::cout << std::string(indent, ' ') << "Case " << value << ":\n";
  for (const auto &clause : clauses) {
    for (const auto &pattern : clause.first) std::cout << std::string(indent + 2, ' ') << pattern << '\n';
    if (clause.second) clause.second->print(indent + 4);
  }
}

int CaseNode::execute() {
  std::vector<std::string> subject{value};
  begin_word_expansion();
  expand_words(subject);
  if (!word_expansion_succeeded()) return 1;
  const std::string matched_value = subject.empty() ? std::string() : subject.front();
  for (auto &clause : clauses) {
    for (const auto &raw_pattern : clause.first) {
      std::string pattern;
      bool quoted = false;
      for (char ch : raw_pattern) {
        if (ch == '\x01' || ch == '\x02') { quoted = !quoted; continue; }
        if (quoted && (ch == '*' || ch == '?' || ch == '[' || ch == '\\')) pattern += '\\';
        pattern += ch;
      }
      if (fnmatch(pattern.c_str(), matched_value.c_str(), 0) == 0) {
        int status = clause.second ? clause.second->execute() : 0;
        set_last_status(status);
        return status;
      }
    }
  }
  set_last_status(0);
  return 0;
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
  remember_job(pid, command);
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

