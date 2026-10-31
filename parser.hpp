#ifndef PARSER_HPP
#define PARSER_HPP

#include <iostream>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "tokenizer.hpp"

struct Redirection {
  std::string type; // input/output, append, stderr, and combined-output forms
  std::string filename;
  bool here_document = false;
  bool expand_document = false;
  size_t line = 1;
  size_t column = 1;
  bool here_string = false;
  int descriptor = -1;
  std::string descriptor_variable;
};

struct ASTNode {
  std::string raw_command;
  virtual ~ASTNode() = default;
  virtual void print(int indent = 0) const = 0;
  virtual int execute() = 0;
};

struct CommandNode : public ASTNode {
  std::vector<std::string> args;
  std::vector<std::pair<size_t, size_t>> arg_locations;
  std::vector<Redirection> redirections;
  std::function<int()> deferred_execution;

  void print(int indent = 0) const override;
  int execute() override;
  int execute_command();
};

struct RedirectionNode : public ASTNode {
  std::unique_ptr<ASTNode> child;
  std::vector<Redirection> redirections;
  RedirectionNode(std::unique_ptr<ASTNode> command,
                  std::vector<Redirection> redirects)
      : child(std::move(command)), redirections(std::move(redirects)) {}
  void print(int indent = 0) const override;
  int execute() override;
};

struct ArrayAssignmentNode : public ASTNode {
  std::string name;
  std::vector<std::string> values;
  std::string index_expression;
  bool indexed = false;
  bool append = false;
  void print(int indent = 0) const override;
  int execute() override;
};

struct ArrayDeclarationNode : public ASTNode {
  std::string command;
  std::string flags;
  std::string name;
  std::vector<std::string> entries;
  bool associative = false;
  bool append = false;
  ArrayDeclarationNode(std::string builtin, std::string options,
                       std::string variable, std::vector<std::string> values,
                       bool is_associative, bool append_values = false)
      : command(std::move(builtin)), flags(std::move(options)),
        name(std::move(variable)), entries(std::move(values)),
        associative(is_associative), append(append_values) {}
  void print(int indent = 0) const override;
  int execute() override;
};

struct ConditionalNode : public ASTNode {
  std::vector<std::string> expression;
  explicit ConditionalNode(std::vector<std::string> tokens)
      : expression(std::move(tokens)) {}
  void print(int indent = 0) const override;
  int execute() override;
};

struct ArithmeticCommandNode : public ASTNode {
  std::string expression;
  explicit ArithmeticCommandNode(std::string expr) : expression(std::move(expr)) {}
  void print(int indent = 0) const override;
  int execute() override;
};

struct TimeNode : public ASTNode {
  std::unique_ptr<ASTNode> command;
  bool portable = false;
  TimeNode(std::unique_ptr<ASTNode> node, bool use_portable_format)
      : command(std::move(node)), portable(use_portable_format) {}
  void print(int indent = 0) const override;
  int execute() override;
};

struct CoprocNode : public ASTNode {
  std::string name;
  std::unique_ptr<ASTNode> command;
  CoprocNode(std::string coproc_name, std::unique_ptr<ASTNode> node)
      : name(std::move(coproc_name)), command(std::move(node)) {}
  void print(int indent = 0) const override;
  int execute() override;
};

struct PipeNode : public ASTNode {
  std::unique_ptr<ASTNode> left;
  std::unique_ptr<ASTNode> right;
  bool stderr_to_pipe = false;

  PipeNode(std::unique_ptr<ASTNode> l, std::unique_ptr<ASTNode> r,
           bool merge_stderr = false)
      : left(std::move(l)), right(std::move(r)), stderr_to_pipe(merge_stderr) {}

  void print(int indent = 0) const override;
  int execute() override;
};

struct ListNode : public ASTNode {
  std::unique_ptr<ASTNode> left;
  std::unique_ptr<ASTNode> right;

  ListNode(std::unique_ptr<ASTNode> l, std::unique_ptr<ASTNode> r)
      : left(std::move(l)), right(std::move(r)) {}

  void print(int indent = 0) const override;
  int execute() override;
};

struct BackgroundNode : public ASTNode {
  std::unique_ptr<ASTNode> child;
  std::string command;
  BackgroundNode(std::unique_ptr<ASTNode> node, std::string source_command)
      : child(std::move(node)), command(std::move(source_command)) {}
  void print(int indent = 0) const override;
  int execute() override;
};

struct AndOrNode : public ASTNode {
  std::unique_ptr<ASTNode> left;
  std::unique_ptr<ASTNode> right;
  bool and_mode;
  AndOrNode(std::unique_ptr<ASTNode> l, std::unique_ptr<ASTNode> r, bool is_and)
      : left(std::move(l)), right(std::move(r)), and_mode(is_and) {}
  void print(int indent = 0) const override;
  int execute() override;
};

struct NotNode : public ASTNode {
  std::unique_ptr<ASTNode> child;
  explicit NotNode(std::unique_ptr<ASTNode> node) : child(std::move(node)) {}
  void print(int indent = 0) const override;
  int execute() override;
};

struct GroupNode : public ASTNode {
  std::unique_ptr<ASTNode> child;
  bool subshell;
  GroupNode(std::unique_ptr<ASTNode> node, bool forked)
      : child(std::move(node)), subshell(forked) {}
  void print(int indent = 0) const override;
  int execute() override;
};

struct IfNode : public ASTNode {
  std::vector<std::pair<std::unique_ptr<ASTNode>, std::unique_ptr<ASTNode>>> branches;
  std::unique_ptr<ASTNode> otherwise;
  void print(int indent = 0) const override;
  int execute() override;
};

struct LoopNode : public ASTNode {
  std::unique_ptr<ASTNode> condition;
  std::unique_ptr<ASTNode> body;
  bool until;
  LoopNode(std::unique_ptr<ASTNode> test, std::unique_ptr<ASTNode> loop_body, bool is_until)
      : condition(std::move(test)), body(std::move(loop_body)), until(is_until) {}
  void print(int indent = 0) const override;
  int execute() override;
};

struct ForNode : public ASTNode {
  std::string variable;
  std::vector<std::string> values;
  std::unique_ptr<ASTNode> body;
  ForNode(std::string name, std::vector<std::string> items, std::unique_ptr<ASTNode> loop_body)
      : variable(std::move(name)), values(std::move(items)), body(std::move(loop_body)) {}
  void print(int indent = 0) const override;
  int execute() override;
};

struct SelectNode : public ASTNode {
  std::string variable;
  std::vector<std::string> values;
  std::unique_ptr<ASTNode> body;
  SelectNode(std::string name, std::vector<std::string> options,
             std::unique_ptr<ASTNode> loop_body)
      : variable(std::move(name)), values(std::move(options)), body(std::move(loop_body)) {}
  void print(int indent = 0) const override;
  int execute() override;
};

struct ArithmeticForNode : public ASTNode {
  std::string initialization, condition, increment;
  std::unique_ptr<ASTNode> body;
  ArithmeticForNode(std::string init, std::string test, std::string step,
                    std::unique_ptr<ASTNode> loop_body)
      : initialization(std::move(init)), condition(std::move(test)),
        increment(std::move(step)), body(std::move(loop_body)) {}
  void print(int indent = 0) const override;
  int execute() override;
};

struct CaseNode : public ASTNode {
  enum class Terminator { Stop, FallThrough, Rematch };
  struct Clause {
    std::vector<std::string> patterns;
    std::unique_ptr<ASTNode> body;
    Terminator terminator = Terminator::Stop;
  };
  std::string value;
  std::vector<Clause> clauses;
  explicit CaseNode(std::string subject) : value(std::move(subject)) {}
  void print(int indent = 0) const override;
  int execute() override;
};

std::unique_ptr<ASTNode> parse(const std::vector<ShellToken> &tokens,
                               const std::string &source);

#endif // PARSER_HPP
