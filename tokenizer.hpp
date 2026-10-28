#ifndef DAX_TOKENIZER_HPP
#define DAX_TOKENIZER_HPP

#include <iostream>
#include <string>
#include <vector>

struct ShellToken : std::string {
  size_t line = 1;
  size_t column = 1;
  size_t source_begin = 0;
  size_t source_end = 0;
  bool quoted = false;

  ShellToken() = default;
  ShellToken(const std::string &text, size_t source_line = 1,
             size_t source_column = 1, bool was_quoted = false,
             size_t begin = 0, size_t end = 0)
      : std::string(text), line(source_line), column(source_column),
        source_begin(begin), source_end(end), quoted(was_quoted) {}
  ShellToken &operator=(const std::string &text) {
    std::string::operator=(text);
    return *this;
  }
};

std::vector<ShellToken> tokenize(const std::string &line);

#endif
