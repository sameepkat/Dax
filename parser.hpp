#ifndef PARSER_HPP
#define PARSER_HPP

#include <iostream>
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
};

struct ASTNode {
  virtual ~ASTNode() = default;
  virtual void print(int indent = 0) const = 0;
  virtual int execute() = 0;
};

struct CommandNode : public ASTNode {
  std::vector<std::string> args;
  std::vector<std::pair<size_t, size_t>> arg_locations;
  std::vector<Redirection> redirections;

  void print(int indent = 0) const override;
  int execute() override;
};

struct PipeNode : public ASTNode {
  std::unique_ptr<ASTNode> left;
  std::unique_ptr<ASTNode> right;

  PipeNode(std::unique_ptr<ASTNode> l, std::unique_ptr<ASTNode> r)
      : left(std::move(l)), right(std::move(r)) {}

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

struct CaseNode : public ASTNode {
  std::string value;
  std::vector<std::pair<std::vector<std::string>, std::unique_ptr<ASTNode>>> clauses;
  explicit CaseNode(std::string subject) : value(std::move(subject)) {}
  void print(int indent = 0) const override;
  int execute() override;
};

std::unique_ptr<ASTNode> parse(const std::vector<ShellToken> &tokens,
                               const std::string &source);

#endif // PARSER_HPP
