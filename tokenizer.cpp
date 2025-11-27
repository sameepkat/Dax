#include <cctype>
#include <iostream>
#include <vector>
#include <sstream>

std::vector<std::string> tokenize(const std::string &line) {
  enum State { NORMAL, IN_SINGLE, IN_DOUBLE } state = NORMAL;
  std::vector<std::string> tokens;
  std::string buf;

  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    switch (state) {
    case NORMAL:
      if (c == '\'') {
        state = IN_SINGLE;
      } else if (c == '"') {
        state = IN_DOUBLE;
      } else if (c == '|' || c == ';' || c == '<' || c == '>') {
        if (!buf.empty()) {
          tokens.push_back(buf);
          buf.clear();
        }
        if (c == '>' && i + 1 < line.size() && line[i + 1] == '>') {
          tokens.push_back(">>");
          i++; 
        } else {
          tokens.push_back(std::string(1, c));
        }
      } else if (std::isspace(c)) {
        if (!buf.empty()) {
          tokens.push_back(buf);
          buf.clear();
        }
      } else {
        buf += c;
      }
      break;

    case IN_SINGLE:
      if (c == '\'') {
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

  if (!buf.empty())
    tokens.push_back(buf);
  return tokens;
}
