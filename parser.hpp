#ifndef PARSER_HPP
#define PARSER_HPP

#include <string>
#include <vector>
#include <memory>
#include <iostream>

struct Redirection {
    std::string type; // "<", ">", ">>"
    std::string filename;
};

struct ASTNode {
    virtual ~ASTNode() = default;
    virtual void print(int indent = 0) const = 0;
    virtual int execute() = 0;
};

struct CommandNode : public ASTNode {
    std::vector<std::string> args;
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

std::unique_ptr<ASTNode> parse(const std::vector<std::string>& tokens);

#endif // PARSER_HPP
