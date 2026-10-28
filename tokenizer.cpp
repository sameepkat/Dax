#include <cctype>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <vector>
#include "tokenizer.hpp"

std::vector<ShellToken> tokenize(const std::string &line) {
  enum State { NORMAL, IN_SINGLE, IN_DOUBLE } state = NORMAL;
  std::vector<ShellToken> tokens;
  std::string buf;
  bool token_started = false;
  size_t word_start = 0;
  size_t current_offset = 0;
  bool word_quoted = false;
  int parameter_braces = 0;
  struct PendingHereDoc { std::string delimiter; bool quoted; bool strip_tabs; size_t token_index; };
  std::vector<PendingHereDoc> pending_here_docs;
  bool expecting_here_delimiter = false;
  bool strip_here_tabs = false;
  auto emit = [&](const std::string &text, size_t offset, bool quoted = false,
                  size_t end_offset = std::string::npos) {
    size_t source_line = 1, source_column = 1;
    for (size_t j = 0; j < offset && j < line.size(); ++j) {
      if (line[j] == '\n') { ++source_line; source_column = 1; }
      else ++source_column;
    }
    if (end_offset == std::string::npos) end_offset = offset + text.size();
    tokens.emplace_back(text, source_line, source_column, quoted, offset, end_offset);
  };
  auto flush_word = [&]() {
    if (!token_started) return;
    emit(buf, word_start, word_quoted, current_offset);
    if (expecting_here_delimiter) {
      std::string delimiter;
      bool quoted = false;
      for (char ch : buf) {
        if (ch == '\x01' || ch == '\x02') { quoted = true; continue; }
        delimiter += ch;
      }
      emit("\x06", word_start);
      pending_here_docs.push_back({delimiter, quoted, strip_here_tabs, tokens.size() - 1});
      expecting_here_delimiter = false;
      strip_here_tabs = false;
    }
    buf.clear();
    token_started = false;
    word_quoted = false;
  };
  auto read_command_substitution = [&](size_t &index) {
    const size_t content_start = index + 2;
    size_t end = content_start;
    int depth = 1;
    char quote = 0;
    for (; end < line.size(); ++end) {
      const char ch = line[end];
      if (quote) {
        if (ch == '\\' && quote == '"' && end + 1 < line.size()) { ++end; continue; }
        if (ch == quote) quote = 0;
        continue;
      }
      if (ch == '\'' || ch == '"') { quote = ch; continue; }
      if (ch == '$' && end + 1 < line.size() && line[end + 1] == '(') {
        ++depth;
        ++end;
        continue;
      }
      if (ch == '(') { ++depth; continue; }
      if (ch == ')' && --depth == 0) break;
    }
    buf += '\x03';
    if (end < line.size()) {
      buf.append(line, content_start, end - content_start);
      buf += '\x03';
      index = end;
    } else {
      buf.append(line, content_start, std::string::npos);
      index = line.size() - 1;
    }
    token_started = true;
  };

  for (size_t i = 0; i < line.size(); ++i) {
    current_offset = i;
    char c = line[i];
    if (!token_started && !std::isspace(static_cast<unsigned char>(c))) word_start = i;
    switch (state) {
    case NORMAL:
      if (parameter_braces > 0 && std::isspace(static_cast<unsigned char>(c))) {
        buf += c;
        token_started = true;
      } else if (c == '\'') {
        buf += '\x01';
        word_quoted = true;
        token_started = true;
        state = IN_SINGLE;
      } else if (c == '"') {
        buf += '\x02';
        word_quoted = true;
        token_started = true;
        state = IN_DOUBLE;
      } else if (c == '$' && i + 1 < line.size() && line[i + 1] == '(') {
        if (i + 2 < line.size() && line[i + 2] == '(') buf += '\x07';
        read_command_substitution(i);
      } else if (c == '!' && buf.empty() &&
                 (i + 1 == line.size() || std::isspace(static_cast<unsigned char>(line[i + 1]) ) ||
                  line[i + 1] == '|' || line[i + 1] == '&' || line[i + 1] == ';')) {
        flush_word();
        emit("!", i);
      } else if (c == '2' && buf.empty() && !token_started && i + 1 < line.size() && line[i + 1] == '>') {
        if (i + 2 < line.size() && line[i + 2] == '&') {
          emit(">&", i);
          i += 2;
        } else if (i + 2 < line.size() && line[i + 2] == '>') {
          emit("2>>", i);
          i += 2;
        } else {
          emit("2>", i);
          ++i;
        }
      } else if (c == '<' && i + 1 < line.size() && line[i + 1] == '<') {
        flush_word();
        if (i + 2 < line.size() && line[i + 2] == '<') {
          emit("<<<", i);
          i += 2;
        } else if (i + 2 < line.size() && line[i + 2] == '-') {
          emit("<<-", i);
          strip_here_tabs = true;
          i += 2;
        } else {
          emit("<<", i);
          strip_here_tabs = false;
          ++i;
        }
        expecting_here_delimiter = true;
      } else if (c == '{' && !buf.empty() && buf.back() == '$') {
        buf += c;
        ++parameter_braces;
        token_started = true;
      } else if (c == '}' && parameter_braces > 0) {
        buf += c;
        --parameter_braces;
      } else if (c == '>' && i + 1 < line.size() && line[i + 1] == '&') {
        flush_word();
        emit(">&", i);
        ++i;
      } else if (c == '|' || c == ';' || c == '&' || c == '<' || c == '>' ||
                 c == '{' || c == '}' || c == '(' || c == ')') {
        flush_word();
        if (c == '&' && i + 1 < line.size() && line[i + 1] == '>') {
          if (i + 2 < line.size() && line[i + 2] == '>') {
            emit("&>>", i);
            i += 2;
          } else {
            emit("&>", i);
            ++i;
          }
        } else if (i + 1 < line.size() && line[i + 1] == c && (c == '>' || c == '<' || c == '|' || c == '&' || c == ';')) {
          emit(std::string(2, c), i);
          i++;
        } else if (c == '>' && i + 1 < line.size() && line[i + 1] == '>') {
          emit(">>", i);
          i++;
        } else {
          emit(std::string(1, c), i);
        }
      } else if (c == '\n') {
        flush_word();
        if (tokens.empty() || (tokens.back() != ";" && tokens.back() != "&" &&
                               tokens.back() != "&&" && tokens.back() != "||" &&
                               tokens.back() != "|"))
          emit(";", i);
        if (!pending_here_docs.empty()) {
          size_t cursor = i + 1;
          for (const auto &doc : pending_here_docs) {
            std::string content;
            bool found = false;
            while (cursor <= line.size()) {
              size_t line_end = line.find('\n', cursor);
              std::string body_line = line.substr(cursor, line_end == std::string::npos
                                                            ? std::string::npos
                                                            : line_end - cursor);
              std::string compare = body_line;
              if (doc.strip_tabs) {
                const size_t first = compare.find_first_not_of('\t');
                compare = first == std::string::npos ? std::string() : compare.substr(first);
              }
              if (compare == doc.delimiter) {
                found = true;
                cursor = line_end == std::string::npos ? line.size() + 1 : line_end + 1;
                break;
              }
              if (doc.strip_tabs) {
                const size_t first = body_line.find_first_not_of('\t');
                body_line = first == std::string::npos ? std::string() : body_line.substr(first);
              }
              content += body_line;
              if (line_end == std::string::npos) { cursor = line.size() + 1; break; }
              content += '\n';
              cursor = line_end + 1;
            }
            if (!found) tokens[doc.token_index] = std::string(1, '\x06');
            else tokens[doc.token_index] = std::string(1, doc.quoted ? '\x05' : '\x04') + content;
            tokens[doc.token_index].source_end = std::min(cursor, line.size());
          }
          pending_here_docs.clear();
          i = cursor > line.size() ? line.size() : cursor - 1;
        }
      } else if (std::isspace(static_cast<unsigned char>(c))) {
        flush_word();
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
      if (c == '$' && i + 1 < line.size() && line[i + 1] == '(') {
        if (i + 2 < line.size() && line[i + 2] == '(') buf += '\x07';
        word_quoted = word_quoted || state == IN_DOUBLE;
        read_command_substitution(i);
      } else if (c == '"') {
        buf += '\x02';
        state = NORMAL;
      } else {
        buf += c;
      }
      break;
    }
  }

  current_offset = line.size();
  flush_word();
  for (const auto &doc : pending_here_docs) tokens[doc.token_index] = std::string(1, '\x06');
  return tokens;
}
