#include <cctype>
#include <iostream>
#include <sstream>
#include <vector>

std::vector<std::string> tokenize(const std::string &line) {
  enum State { NORMAL, IN_SINGLE, IN_DOUBLE } state = NORMAL;
  std::vector<std::string> tokens;
  std::string buf;
  bool token_started = false;

  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    switch (state) {
    case NORMAL:
      if (c == '\'') {
        buf += '\x01';
        token_started = true;
        state = IN_SINGLE;
      } else if (c == '"') {
        token_started = true;
        state = IN_DOUBLE;
      } else if (c == '|' || c == ';' || c == '&' || c == '<' || c == '>' ||
                 c == '{' || c == '}' || c == '(' || c == ')') {
        if (token_started) {
          tokens.push_back(buf);
          buf.clear();
          token_started = false;
        }
        if (c == '>' && i + 1 < line.size() && line[i + 1] == '>') {
          tokens.push_back(">>");
          i++;
        } else {
          tokens.push_back(std::string(1, c));
        }
      } else if (std::isspace(c)) {
        if (token_started) {
          tokens.push_back(buf);
          buf.clear();
          token_started = false;
        }
      } else {
        buf += c;
        token_started = true;
      }
      break;

    case IN_SINGLE:
      if (c == '\'') {
        buf += '\x01';
        state = NORMAL;
      } else {
        buf += c;
      }
      break;

    case IN_DOUBLE:
      if (c == '"') {
        state = NORMAL;
      } else {
        buf += c;
      }
      break;
    }
  }

  if (token_started)
    tokens.push_back(buf);
  return tokens;
}
