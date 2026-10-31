#include <cctype>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <vector>
#include "tokenizer.hpp"
#include "shell_state.hpp"

std::vector<ShellToken> tokenize(const std::string &line) {
  enum State { NORMAL, IN_SINGLE, IN_DOUBLE } state = NORMAL;
  std::vector<ShellToken> tokens;
  std::string buf;
  bool token_started = false;
  size_t word_start = 0;
  size_t current_offset = 0;
  bool word_quoted = false;
  bool in_conditional_expression = false;
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
    const bool command_position = tokens.empty() ||
        tokens.back() == ";" || tokens.back() == "&" ||
        tokens.back() == "&&" || tokens.back() == "||" ||
        tokens.back() == "|" || tokens.back() == "|&" ||
        tokens.back() == "!" || tokens.back() == "if" ||
        tokens.back() == "elif" || tokens.back() == "then" ||
        tokens.back() == "else" || tokens.back() == "time" ||
        tokens.back() == "coproc" ||
        tokens.back() == "while" || tokens.back() == "until" ||
        tokens.back() == "do" || tokens.back() == "{" ||
        tokens.back() == "(";
    const bool unquoted_word = !word_quoted;
    const std::string word = buf;
    emit(buf, word_start, word_quoted, current_offset);
    if (unquoted_word && word == "[[" && command_position)
      in_conditional_expression = true;
    else if (unquoted_word && word == "]]" && in_conditional_expression)
      in_conditional_expression = false;
    if (expecting_here_delimiter) {
      std::string delimiter;
      bool quoted = false;
      for (size_t i = 0; i < buf.size(); ++i) {
        const char ch = buf[i];
        if (ch == '\x01' || ch == '\x02') { quoted = true; continue; }
        if (ch == '\x0b' && i + 1 < buf.size()) {
          quoted = true;
          delimiter += buf[++i];
          continue;
        }
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
  auto read_backtick_substitution = [&](size_t &index) {
    const size_t start = index;
    std::string command;
    size_t end = index + 1;
    for (; end < line.size(); ++end) {
      const char ch = line[end];
      if (ch == '`') break;
      if (ch == '\\' && end + 1 < line.size()) {
        const char next = line[end + 1];
        if (next == '\n') { ++end; continue; }
        if (next == '`') {
          // Backslashes protect nested backticks from closing this delimiter;
          // the next shell parse must see them as substitution delimiters.
          command += next;
          ++end;
          continue;
        }
        if (next == '$' || next == '\\') {
          command += '\x0b';
          command += next;
          ++end;
          continue;
        }
      }
      command += ch;
    }
    if (end == line.size()) {
      buf.append(line, start, std::string::npos);
      index = line.size() - 1;
      token_started = true;
      return;
    }
    buf += '\x03';
    buf += command;
    buf += '\x03';
    token_started = true;
    index = end;
  };
  auto read_ansi_c_quoted = [&](size_t &index) {
    size_t end = index + 2;
    std::string decoded;
    bool closed = false;
    auto hex_digit = [](char ch) -> int {
      if (ch >= '0' && ch <= '9') return ch - '0';
      if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
      if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
      return -1;
    };
    auto append_codepoint = [&](uint32_t value) {
      if (value <= 0x7f) decoded += static_cast<char>(value);
      else if (value <= 0x7ff) {
        decoded += static_cast<char>(0xc0 | (value >> 6));
        decoded += static_cast<char>(0x80 | (value & 0x3f));
      } else if (value <= 0xffff && !(value >= 0xd800 && value <= 0xdfff)) {
        decoded += static_cast<char>(0xe0 | (value >> 12));
        decoded += static_cast<char>(0x80 | ((value >> 6) & 0x3f));
        decoded += static_cast<char>(0x80 | (value & 0x3f));
      } else if (value <= 0x10ffff) {
        decoded += static_cast<char>(0xf0 | (value >> 18));
        decoded += static_cast<char>(0x80 | ((value >> 12) & 0x3f));
        decoded += static_cast<char>(0x80 | ((value >> 6) & 0x3f));
        decoded += static_cast<char>(0x80 | (value & 0x3f));
      }
    };
    for (; end < line.size(); ++end) {
      const char ch = line[end];
      if (ch == '\'') { closed = true; break; }
      if (ch != '\\' || end + 1 >= line.size()) {
        decoded += ch;
        continue;
      }
      const char escaped = line[++end];
      switch (escaped) {
      case 'a': decoded += '\a'; break;
      case 'b': decoded += '\b'; break;
      case 'e': case 'E': decoded += '\x1b'; break;
      case 'f': decoded += '\f'; break;
      case 'n': decoded += '\n'; break;
      case 'r': decoded += '\r'; break;
      case 't': decoded += '\t'; break;
      case 'v': decoded += '\v'; break;
      case '\\': decoded += '\\'; break;
      case '\'': decoded += '\''; break;
      case '"': decoded += '"'; break;
      case '?': decoded += '?'; break;
      case 'c':
        if (end + 1 < line.size()) decoded += static_cast<char>(line[++end] & 0x1f);
        else decoded += "\\c";
        break;
      case 'x': case 'u': case 'U': {
        const size_t max_digits = escaped == 'x' ? 2 : escaped == 'u' ? 4 : 8;
        uint32_t value = 0;
        size_t digits = 0;
        while (end + 1 < line.size() && digits < max_digits) {
          const int digit = hex_digit(line[end + 1]);
          if (digit < 0) break;
          value = (value << 4) | static_cast<uint32_t>(digit);
          ++end;
          ++digits;
        }
        if (digits) {
          if (escaped == 'x') decoded += static_cast<char>(value);
          else append_codepoint(value);
        } else decoded += escaped;
        break;
      }
      default:
        if (escaped >= '0' && escaped <= '7') {
          unsigned value = static_cast<unsigned>(escaped - '0');
          size_t digits = 1;
          while (end + 1 < line.size() && digits < 3 && line[end + 1] >= '0' && line[end + 1] <= '7') {
            value = (value << 3) | static_cast<unsigned>(line[++end] - '0');
            ++digits;
          }
          decoded += static_cast<char>(value & 0xff);
        } else {
          decoded += '\\';
          decoded += escaped;
        }
      }
    }
    if (!closed) return false;
    buf += '\x01';
    const std::string marker_bytes = "\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0b\x0e\x0f\x10";
    for (char ch : decoded) {
      if (marker_bytes.find(ch) != std::string::npos) buf += '\x0b';
      buf += ch;
    }
    buf += '\x01';
    word_quoted = true;
    token_started = true;
    index = end;
    return true;
  };
  auto read_process_substitution = [&](size_t &index, char direction) {
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
      if (ch == '(') ++depth;
      else if (ch == ')' && --depth == 0) break;
    }
    buf += '\x08';
    buf += direction;
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
  auto read_extglob = [&](size_t &index) {
    size_t cursor = index;
    int depth = 0;
    char quote = 0;
    std::string encoded;
    for (; cursor < line.size(); ++cursor) {
      const char ch = line[cursor];
      if (quote == '\'') {
        if (ch == '\'') { encoded += '\x01'; quote = 0; }
        else encoded += ch;
        continue;
      }
      if (quote == '"') {
        if (ch == '"') { encoded += '\x02'; quote = 0; }
        else if (ch == '\\' && cursor + 1 < line.size() &&
                 (line[cursor + 1] == '$' || line[cursor + 1] == '`' ||
                  line[cursor + 1] == '"' || line[cursor + 1] == '\\')) {
          encoded += '\x0b'; encoded += line[++cursor];
        } else encoded += ch;
        continue;
      }
      if (ch == '\\' && cursor + 1 < line.size()) {
        if (line[cursor + 1] == '\n') { ++cursor; continue; }
        encoded += '\x0b'; encoded += line[++cursor];
        continue;
      }
      if (ch == '\'') { encoded += '\x01'; quote = '\''; continue; }
      if (ch == '"') { encoded += '\x02'; quote = '"'; continue; }
      if (ch == '(') ++depth;
      else if (ch == ')' && --depth == 0) { encoded += ch; break; }
      encoded += ch;
    }
    if (depth != 0) {
      buf += line[index]; token_started = true;
      return;
    }
    buf += encoded;
    token_started = true;
    index = cursor;
  };

  for (size_t i = 0; i < line.size(); ++i) {
    current_offset = i;
    char c = line[i];
    if (!token_started && !std::isspace(static_cast<unsigned char>(c))) word_start = i;
    switch (state) {
    case NORMAL:
      if (parameter_braces > 0 &&
          (std::isspace(static_cast<unsigned char>(c)) || std::string("|;&<>()").find(c) != std::string::npos)) {
        buf += c;
        token_started = true;
      } else if (c == '#' && !token_started &&
                 (!shell_state().interactive_shell ||
                  shell_state().interactive_comments)) {
        const size_t newline = line.find('\n', i);
        if (newline == std::string::npos) break;
        i = newline - 1;
      } else if (c == '\\' && i + 1 < line.size()) {
        if (line[i + 1] == '\n') { ++i; }
        else { buf += '\x0b'; buf += line[++i]; token_started = true; }
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
      } else if (std::string("@?*+!").find(c) != std::string::npos &&
                 i + 1 < line.size() && line[i + 1] == '(') {
        read_extglob(i);
      } else if (c == '$' && i + 1 < line.size() && line[i + 1] == '(') {
        if (i + 2 < line.size() && line[i + 2] == '(') buf += '\x07';
        read_command_substitution(i);
      } else if (c == '`') {
        read_backtick_substitution(i);
      } else if (c == '$' && i + 1 < line.size() && line[i + 1] == '\'') {
        if (!read_ansi_c_quoted(i)) { buf += c; token_started = true; }
      } else if ((c == '<' || c == '>') && i + 1 < line.size() && line[i + 1] == '(') {
        read_process_substitution(i, c);
      } else if (c == '!' && buf.empty() &&
                 (i + 1 == line.size() || std::isspace(static_cast<unsigned char>(line[i + 1]) ) ||
                  line[i + 1] == '|' || line[i + 1] == '&' || line[i + 1] == ';')) {
        flush_word();
        emit("!", i);
      } else if (std::isdigit(static_cast<unsigned char>(c)) && buf.empty() && !token_started) {
        size_t digits_end = i + 1;
        while (digits_end < line.size() &&
               std::isdigit(static_cast<unsigned char>(line[digits_end]))) ++digits_end;
        if (digits_end < line.size() && (line[digits_end] == '>' || line[digits_end] == '<')) {
          size_t operator_end = digits_end + 1;
          bool numeric_here_document = false;
          bool numeric_here_string = false;
          if (line[digits_end] == '<' && operator_end < line.size() &&
              line[operator_end] == '<') {
            ++operator_end;
            if (operator_end < line.size() && line[operator_end] == '<') {
              ++operator_end;
              numeric_here_string = true;
            } else {
              if (operator_end < line.size() && line[operator_end] == '-') ++operator_end;
              numeric_here_document = true;
            }
          } else if (line[digits_end] == '>' && operator_end < line.size() &&
              (line[operator_end] == '&' || line[operator_end] == '|' ||
               line[operator_end] == '>')) ++operator_end;
          else if (line[digits_end] == '<' && operator_end < line.size() &&
                   line[operator_end] == '>') ++operator_end;
          else if (line[digits_end] == '<' && operator_end < line.size() &&
                   line[operator_end] == '&') ++operator_end;
          emit(line.substr(i, operator_end - i), i);
          if (numeric_here_document) {
            expecting_here_delimiter = true;
            strip_here_tabs = line[operator_end - 1] == '-';
          } else if (numeric_here_string) {
            strip_here_tabs = false;
          }
          i = operator_end - 1;
        } else {
          buf += c;
          token_started = true;
        }
      } else if (c == '<' && i + 1 < line.size() && line[i + 1] == '<') {
        flush_word();
        if (i + 2 < line.size() && line[i + 2] == '<') {
          emit("<<<", i);
          i += 2;
          strip_here_tabs = false;
        } else if (i + 2 < line.size() && line[i + 2] == '-') {
          emit("<<-", i);
          strip_here_tabs = true;
          i += 2;
          expecting_here_delimiter = true;
        } else {
          emit("<<", i);
          strip_here_tabs = false;
          ++i;
          expecting_here_delimiter = true;
        }
      } else if (c == '{' && !buf.empty() && buf.back() == '$') {
        buf += c;
        ++parameter_braces;
        token_started = true;
      } else if (c == '}' && parameter_braces > 0) {
        buf += c;
        --parameter_braces;
      } else if (c == '<' && i + 1 < line.size() && line[i + 1] == '&') {
        flush_word();
        emit("<&", i);
        ++i;
      } else if (c == '<' && i + 1 < line.size() && line[i + 1] == '>') {
        flush_word();
        emit("<>", i);
        ++i;
      } else if (c == '>' && i + 1 < line.size() && line[i + 1] == '&') {
        flush_word();
        emit(">&", i);
        ++i;
      } else if (c == '>' && i + 1 < line.size() && line[i + 1] == '|') {
        flush_word();
        emit(">|", i);
        ++i;
      } else if (c == '|' && i + 1 < line.size() && line[i + 1] == '&') {
        flush_word();
        emit("|&", i);
        ++i;
      } else if (c == '|' || c == ';' || c == '&' || c == '<' || c == '>' ||
                 c == '(' || c == ')' ||
                 (c == '{' && buf.empty() && i + 1 < line.size() &&
                  (std::isspace(static_cast<unsigned char>(line[i + 1])) ||
                   line[i + 1] == ';' || line[i + 1] == '}')) ||
                 (c == '}' && buf.empty() &&
                  (i + 1 == line.size() || std::isspace(static_cast<unsigned char>(line[i + 1])) ||
                   line[i + 1] == ';' || line[i + 1] == '|')) ) {
        flush_word();
        if (c == '&' && i + 1 < line.size() && line[i + 1] == '>') {
          if (i + 2 < line.size() && line[i + 2] == '>') {
            emit("&>>", i);
            i += 2;
          } else {
            emit("&>", i);
            ++i;
          }
        } else if (c == ';' && i + 2 < line.size() && line[i + 1] == ';' && line[i + 2] == '&') {
          emit(";;&", i);
          i += 2;
        } else if (c == ';' && i + 1 < line.size() && line[i + 1] == '&') {
          emit(";&", i);
          ++i;
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
                               tokens.back() != "|" && tokens.back() != "|&"))
          emit(";", i);
        if (!pending_here_docs.empty()) {
          size_t cursor = i + 1;
          for (const auto &doc : pending_here_docs) {
            std::string content;
            std::string logical_line;
            bool found = false;
            while (cursor <= line.size()) {
              size_t line_end = line.find('\n', cursor);
              const bool has_newline = line_end != std::string::npos;
              std::string body_line = line.substr(cursor, line_end == std::string::npos
                                                            ? std::string::npos
                                                            : line_end - cursor);
              std::string compare = body_line;
              if (doc.strip_tabs) {
                const size_t first = compare.find_first_not_of('\t');
                compare = first == std::string::npos ? std::string() : compare.substr(first);
              }
              if (doc.strip_tabs) {
                const size_t first = body_line.find_first_not_of('\t');
                body_line = first == std::string::npos ? std::string() : body_line.substr(first);
              }
              cursor = has_newline ? line_end + 1 : line.size() + 1;
              logical_line += compare;
              bool continued = false;
              if (!doc.quoted && has_newline) {
                size_t trailing_backslashes = 0;
                while (trailing_backslashes < compare.size() &&
                       compare[compare.size() - trailing_backslashes - 1] == '\\')
                  ++trailing_backslashes;
                if (trailing_backslashes % 2 == 1) {
                  logical_line.pop_back();
                  continued = true;
                }
              }
              if (continued) {
                content += body_line;
                content += '\n';
                continue;
              }
              if (logical_line == doc.delimiter) {
                found = true;
                break;
              }
              content += body_line;
              if (has_newline) content += '\n';
              logical_line.clear();
              if (!has_newline) break;
            }
            if (!found) tokens[doc.token_index] = std::string(1, '\x06') + content;
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
      if (c == '\\' && i + 1 < line.size() &&
          (line[i + 1] == '$' || line[i + 1] == '`' || line[i + 1] == '"' ||
           line[i + 1] == '\\' || line[i + 1] == '\n')) {
        if (line[i + 1] == '\n') { ++i; }
        else { buf += '\x0b'; buf += line[++i]; }
      } else if (c == '$' && i + 1 < line.size() && line[i + 1] == '(') {
        if (i + 2 < line.size() && line[i + 2] == '(') buf += '\x07';
        word_quoted = word_quoted || state == IN_DOUBLE;
        read_command_substitution(i);
      } else if (c == '`') {
        word_quoted = true;
        read_backtick_substitution(i);
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
