#include "pattern_match.hpp"

#include <climits>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
bool bracket_negation(wchar_t c) {
  static const bool posix = std::getenv("POSIXLY_CORRECT") != nullptr;
  return c == L'!' || (c == L'^' && !posix);
}

// Convert using the active locale. Invalid bytes remain distinct and match
// themselves, rather than being discarded.
std::wstring wide(const char *input) {
  std::wstring out;
  std::mbstate_t state{};
  while (*input) {
    wchar_t ch;
    size_t n = std::mbrtowc(&ch, input, MB_CUR_MAX, &state);
    if (n == static_cast<size_t>(-1) || n == static_cast<size_t>(-2)) {
      ch = static_cast<wchar_t>(0x110000u + static_cast<unsigned char>(*input));
      n = 1;
      state = {};
    } else if (n == 0) {
      break;
    }
    out.push_back(ch);
    input += n;
  }
  return out;
}

std::wstring bytes(const char *input) {
  std::wstring out;
  while (*input) {
    unsigned char ch = static_cast<unsigned char>(*input++);
    out.push_back(ch < 0x80 ? ch : static_cast<wchar_t>(0x110000u + ch));
  }
  return out;
}

struct Sequence;
struct Node {
  enum Kind { Literal, Any, Star, Bracket, Group } kind = Literal;
  wchar_t ch = 0;
  std::wstring bracket;
  std::wstring source;
  std::vector<std::shared_ptr<Sequence>> alternatives;
};
struct Sequence { std::vector<Node> nodes; };

class Parser {
 public:
  Parser(std::wstring pattern, bool extended) : pattern_(std::move(pattern)), extended_(extended) {}
  std::shared_ptr<Sequence> parse() { size_t at = 0; return sequence(at, false); }
  size_t star_boundary(size_t open) const { return star_group_end(open); }
 private:
  std::wstring pattern_;
  bool extended_;

  // Bracket expressions can contain POSIX classes such as [[:alpha:]].
  size_t bracket_end(size_t at) const {
    size_t i = at + 1;
    if (i < pattern_.size() && bracket_negation(pattern_[i])) ++i;
    if (i < pattern_.size() && pattern_[i] == L']') ++i;
    for (; i < pattern_.size(); ++i) {
      if (pattern_[i] == L'\\' && i + 1 < pattern_.size()) { ++i; continue; }
      if (pattern_[i] == L'[' && i + 1 < pattern_.size() &&
          (pattern_[i + 1] == L':' || pattern_[i + 1] == L'.' || pattern_[i + 1] == L'=')) {
        wchar_t marker = pattern_[i + 1];
        size_t end = pattern_.find(std::wstring{marker, L']'}, i + 2);
        if (end != std::wstring::npos) { i = end + 1; continue; }
      }
      if (pattern_[i] == L']') return i;
    }
    return std::wstring::npos;
  }

  // When a plain star is followed by an optional/repeating extended group,
  // libc's star scan validates and skips that group before matching the rest.
  size_t star_group_end(size_t open) const {
    size_t at = open;
    while (++at < pattern_.size()) {
      if (pattern_[at] == L'[') {
        ++at;
        if (at < pattern_.size() && bracket_negation(pattern_[at])) ++at;
        if (at < pattern_.size() && pattern_[at] == L']') ++at;
        while (at < pattern_.size() && pattern_[at] != L']') ++at;
        if (at == pattern_.size()) return std::wstring::npos;
      } else if (std::wstring(L"?*+@!").find(pattern_[at]) != std::wstring::npos &&
                 at + 1 < pattern_.size() && pattern_[at + 1] == L'(') {
        at = star_group_end(at + 1);
        if (at == std::wstring::npos || at >= pattern_.size()) return std::wstring::npos;
      } else if (pattern_[at] == L')') {
        return at + 1;
      }
    }
    return std::wstring::npos;
  }

  std::shared_ptr<Sequence> sequence(size_t &at, bool in_group) {
    auto result = std::make_shared<Sequence>();
    size_t literal_parentheses = 0;
    bool star_scan = false;
    while (at < pattern_.size()) {
      const size_t node_start = at;
      wchar_t ch = pattern_[at];
      if (in_group && literal_parentheses == 0 && (ch == L'|' || ch == L')')) break;
      Node node;
      if (ch == L'\\') {
        ++at;
        // The extended-pattern delimiter scan counts an operator followed
        // by '(' even when the operator itself is escaped.
        if (in_group && at + 1 < pattern_.size() &&
            std::wstring(L"?*+@!").find(pattern_[at]) != std::wstring::npos && pattern_[at + 1] == L'(')
          ++literal_parentheses;
        // A trailing backslash never matches, including an empty string.
        node.ch = at < pattern_.size() ? pattern_[at++] : 0;
      } else if (extended_ && std::wstring(L"?*+@!").find(ch) != std::wstring::npos &&
                 at + 1 < pattern_.size() && pattern_[at + 1] == L'(' &&
                 !(star_scan && (ch == L'?' || ch == L'*') &&
                   star_group_end(at + 1) == std::wstring::npos)) {
        const size_t start = at;
        const size_t star_end = star_group_end(at + 1);
        at += 2;
        node.kind = Node::Group;
        node.ch = ch;
        while (true) {
          node.alternatives.push_back(sequence(at, true));
          if (at >= pattern_.size() || pattern_[at] == L')') break;
          ++at;  // |
        }
        if (at == pattern_.size()) {
          // An unclosed extglob is interpreted as ordinary pattern text.
          at = start + 1;
          node = Node{};
          if (ch == L'*') node.kind = Node::Star;
          else if (ch == L'?') node.kind = Node::Any;
          else node.ch = ch;
        } else {
          ++at;
          if (star_scan && (ch == L'?' || ch == L'*') && star_end != at) {
            // The star scanner can find a different boundary from the
            // normal group parser around nested groups.
            at = star_end;
          }
          node.source = pattern_.substr(start, at - start);
        }
      } else if (ch == L'[') {
        size_t end = bracket_end(at);
        if (end == std::wstring::npos) { node.ch = ch; ++at; }
        else { node.kind = Node::Bracket; node.bracket = pattern_.substr(at + 1, end - at - 1); at = end + 1; }
      } else {
        ++at;
        if (in_group && ch == L')' && literal_parentheses) --literal_parentheses;
        if (ch == L'*') node.kind = Node::Star;
        else if (ch == L'?') node.kind = Node::Any;
        else node.ch = ch;
      }
      if (node.source.empty()) node.source = pattern_.substr(node_start, at - node_start);
      star_scan = node.kind == Node::Star ||
          (star_scan && (node.kind == Node::Any ||
           (node.kind == Node::Group && (node.ch == L'?' || node.ch == L'*'))));
      result->nodes.push_back(std::move(node));
    }
    return result;
  }
};

class Matcher {
 public:
  Matcher(std::wstring text, int flags, bool initial_period = true)
      : text_(std::move(text)), flags_(flags), initial_period_(initial_period) {}
  bool matches(const std::shared_ptr<Sequence> &root) {
    const auto &ends = sequence(root.get(), 0, 0);
    return ends[text_.size()];
  }
 private:
  using Positions = std::vector<unsigned char>;
  struct Key {
    const Sequence *seq;
    size_t node, pos;
    bool after_star;
    bool star_leading;
    bool operator==(const Key &other) const { return seq == other.seq && node == other.node && pos == other.pos && after_star == other.after_star && star_leading == other.star_leading; }
  };
  struct KeyHash {
    size_t operator()(const Key &k) const {
      return std::hash<const void *>{}(k.seq) ^ (k.node * 0x9e3779b9u) ^ (k.pos * 0x85ebca6bu) ^ static_cast<size_t>(k.after_star) ^ (static_cast<size_t>(k.star_leading) << 1);
    }
  };
  struct SubKey {
    const Sequence *seq;
    size_t begin, end;
    bool operator==(const SubKey &other) const {
      return seq == other.seq && begin == other.begin && end == other.end;
    }
  };
  struct SubKeyHash {
    size_t operator()(const SubKey &k) const {
      return std::hash<const void *>{}(k.seq) ^ (k.begin * 0x9e3779b9u) ^ (k.end * 0x85ebca6bu);
    }
  };
  struct SuffixKey {
    const Sequence *seq;
    size_t index;
    bool operator==(const SuffixKey &other) const { return seq == other.seq && index == other.index; }
  };
  struct SuffixKeyHash {
    size_t operator()(const SuffixKey &k) const {
      return std::hash<const void *>{}(k.seq) ^ (k.index * 0x9e3779b9u);
    }
  };
  std::wstring text_;
  int flags_;
  bool initial_period_;
  std::unordered_map<Key, Positions, KeyHash> cache_;
  std::unordered_map<const Node *, std::vector<std::shared_ptr<Sequence>>> combined_;
  std::unordered_map<const Node *, std::shared_ptr<Sequence>> star_fallbacks_;
  std::unordered_map<SubKey, bool, SubKeyHash> substring_cache_;
  std::unordered_map<std::string, std::shared_ptr<Sequence>> interned_;
  std::unordered_map<SuffixKey, std::shared_ptr<Sequence>, SuffixKeyHash> suffixes_;

  static void append_number(std::string &out, size_t number) {
    for (size_t i = 0; i < sizeof(number); ++i)
      out.push_back(static_cast<char>((number >> (i * CHAR_BIT)) & 0xff));
  }
  static void append_wide(std::string &out, const std::wstring &text) {
    append_number(out, text.size());
    for (wchar_t ch : text) append_number(out, static_cast<size_t>(ch));
  }
  static void sequence_key(std::string &out, const Sequence &seq) {
    append_number(out, seq.nodes.size());
    for (const Node &node : seq.nodes) {
      append_number(out, static_cast<size_t>(node.kind));
      append_number(out, static_cast<size_t>(node.ch));
      append_wide(out, node.bracket);
      append_wide(out, node.source);
      append_number(out, node.alternatives.size());
      for (const auto &alt : node.alternatives) sequence_key(out, *alt);
    }
  }
  std::shared_ptr<Sequence> intern(std::shared_ptr<Sequence> seq) {
    std::string key;
    sequence_key(key, *seq);
    auto entry = interned_.emplace(std::move(key), std::move(seq));
    return entry.first->second;
  }
  std::shared_ptr<Sequence> suffix(const Sequence *seq, size_t index) {
    SuffixKey key{seq, index};
    auto found = suffixes_.find(key);
    if (found != suffixes_.end()) return found->second;
    auto result = std::make_shared<Sequence>();
    result->nodes.assign(seq->nodes.begin() + index, seq->nodes.end());
    result = intern(std::move(result));
    suffixes_.emplace(key, result);
    return result;
  }

  wchar_t fold(wchar_t c) const {
    return (flags_ & DAX_PATTERN_CASEFOLD) && c <= 0x10ffff ? std::towlower(c) : c;
  }
  // The first collation level determines an equivalence class in locales
  // whose transformed keys contain collation-level separators.
  std::wstring primary_weight(const std::wstring &input) const {
    for (wchar_t c : input) if (c > 0x10ffff) return input;
    size_t size = std::wcsxfrm(nullptr, input.c_str(), 0);
    std::wstring key(size + 1, L'\0');
    std::wcsxfrm(&key[0], input.c_str(), key.size());
    key.resize(size);
    size_t separator = key.find(L'\1');
    if (separator != std::wstring::npos) key.resize(separator);
    return key;
  }
  std::wstring primary_weight(wchar_t c) const { return primary_weight(std::wstring(1, c)); }
  bool valid_collating_symbol(const std::wstring &symbol) const {
    if (symbol.empty()) return false;
    if (symbol.size() == 1) return true;
    std::wstring separate;
    for (wchar_t ch : symbol) separate += primary_weight(ch);
    return primary_weight(symbol) != separate;
  }
  bool multilevel_collation() const {
    const wchar_t sample[] = L"a";
    size_t size = std::wcsxfrm(nullptr, sample, 0);
    std::wstring key(size + 1, L'\0');
    std::wcsxfrm(&key[0], sample, key.size());
    return key.find(L'\1') < size;
  }
  bool in_range(wchar_t first, wchar_t last, wchar_t c) const {
    first = fold(first);
    last = fold(last);
    c = fold(c);
    if (first > 0x10ffff || last > 0x10ffff || c > 0x10ffff)
      return first <= c && c <= last;
    if (!(flags_ & DAX_PATTERN_CASEFOLD)) {
      if (std::iswlower(first) && std::iswlower(last) && std::iswupper(c)) return false;
      if (std::iswupper(first) && std::iswupper(last) && std::iswlower(c)) return false;
    }
    const wchar_t a[] = {first, 0}, b[] = {last, 0}, value[] = {c, 0};
    return std::wcscoll(a, value) <= 0 && std::wcscoll(value, b) <= 0;
  }
  bool wildcard_allowed(size_t pos) const {
    if (pos >= text_.size()) return false;
    if ((flags_ & DAX_PATTERN_PATHNAME) && text_[pos] == L'/') return false;
    if (protected_dot(pos)) return false;
    return true;
  }
  bool protected_dot(size_t pos) const {
    return pos < text_.size() && (flags_ & DAX_PATTERN_PERIOD) && text_[pos] == L'.' &&
        ((pos == 0 && initial_period_) ||
         (pos > 0 && (flags_ & DAX_PATTERN_PATHNAME) && text_[pos - 1] == L'/'));
  }
  bool bracket(const std::wstring &body, wchar_t c) const {
    size_t i = 0;
    bool negate = i < body.size() && bracket_negation(body[i]);
    if (negate) ++i;
    // Handle incomplete collating/equivalence constructs as glibc does.
    // An incomplete collating symbol makes the expression invalid; an
    // incomplete equivalence expression is treated as a literal suffix.
    for (size_t j = i; j + 1 < body.size(); ++j) {
      if (body[j] == L'\\') { ++j; continue; }
      if (body[j] != L'[' || (body[j + 1] != L'.' && body[j + 1] != L'=' && body[j + 1] != L':')) continue;
      wchar_t marker = body[j + 1];
      size_t end = body.find(std::wstring{marker, L']'}, j + 2);
      if (end != std::wstring::npos) { j = end + 1; continue; }
      if (marker == L'.') return false;
      if (marker == L'=' && !negate) i = j;
    }
    bool matched = false;
    while (i < body.size()) {
      if (body[i] == L'[' && i + 1 < body.size() &&
          (body[i + 1] == L':' || body[i + 1] == L'=' || body[i + 1] == L'.')) {
        wchar_t marker = body[i + 1];
        size_t end = body.find(std::wstring{marker, L']'}, i + 2);
        if (end != std::wstring::npos) {
          std::wstring name = body.substr(i + 2, end - i - 2);
          if (marker == L':') {
            std::string narrow(name.begin(), name.end());
            wctype_t type = std::wctype(narrow.c_str());
            if (!type) return false;
            if (c <= 0x10ffff && std::iswctype(c, type)) matched = true;
          } else if (marker == L'.') {
            if (!valid_collating_symbol(name)) return false;
            if (end + 3 < body.size() && body[end + 2] == L'-') {
              wchar_t last = body[end + 3];
              if (last == L'[' && end + 4 < body.size() && body[end + 4] == L'.') {
                size_t last_end = body.find(L".]", end + 5);
                if (last_end == std::wstring::npos ||
                    !valid_collating_symbol(body.substr(end + 5, last_end - end - 5))) return false;
                std::wstring last_symbol = body.substr(end + 5, last_end - end - 5);
                if (name.size() == 1 && last_symbol.size() == 1 && !multilevel_collation() &&
                    in_range(name[0], last_symbol[0], c)) matched = true;
                end = last_end;
                if (matched) return !negate;
                i = end + 2;
                continue;
              }
              const wchar_t value[] = {fold(c), 0}, bound[] = {fold(last), 0};
              if (name.size() == 1 ? in_range(name[0], last, c)
                                   : (c <= 0x10ffff &&
                                      ((!std::iswalpha(c) && c < 128) || std::wcscoll(value, bound) <= 0))) matched = true;
              end += 2;
            } else if (name.size() == 1 && name[0] == c) matched = true;
          } else if (name.size() == 1 && primary_weight(name[0]) == primary_weight(c)) matched = true;
          if (matched) return !negate;
          i = end + 2;
          continue;
        }
      }
      wchar_t first = body[i++];
      if (first == L'\\' && i < body.size()) first = body[i++];
      if (i + 1 < body.size() && body[i] == L'-') {
        ++i;
        if (i + 1 < body.size() && body[i] == L'[' && body[i + 1] == L'.') {
          size_t end = body.find(L".]", i + 2);
          if (end == std::wstring::npos || !valid_collating_symbol(body.substr(i + 2, end - i - 2)))
            return false;
          const std::wstring last_symbol = body.substr(i + 2, end - i - 2);
          if (last_symbol.size() == 1 && !multilevel_collation() && in_range(first, last_symbol[0], c)) return !negate;
          i = end + 2;
          continue;
        }
        wchar_t last = body[i++];
        if (last == L'\\' && i < body.size()) last = body[i++];
        if (in_range(first, last, c)) matched = true;
      } else if (fold(first) == fold(c)) matched = true;
      if (matched) return !negate;
    }
    return negate ? !matched : matched;
  }
  void collating_symbols(const std::wstring &body, size_t pos, Positions &next) const {
    if (body.empty() || bracket_negation(body.front())) return;
    for (size_t i = 0; i + 1 < body.size(); ++i) {
      if (body[i] == L'\\') { ++i; continue; }
      if (body[i] != L'[' || body[i + 1] != L'.') continue;
      size_t end = body.find(L".]", i + 2);
      if (end == std::wstring::npos) break;
      std::wstring symbol = body.substr(i + 2, end - i - 2);
      if (symbol.size() > 1 && (i == 0 || body[i - 1] != L'-') &&
          (end + 2 == body.size() || body[end + 2] != L'-') &&
          symbol.size() <= text_.size() - pos &&
          text_.compare(pos, symbol.size(), symbol) == 0 &&
          (!(flags_ & DAX_PATTERN_PATHNAME) || symbol.find(L'/') == std::wstring::npos)) {
        if (valid_collating_symbol(symbol)) next[pos + symbol.size()] = 1;
      }
      i = end + 1;
    }
  }
  void merge(Positions &into, const Positions &from) const {
    for (size_t i = 0; i < into.size(); ++i) into[i] |= from[i];
  }
  bool alternative_matches(const std::shared_ptr<Sequence> &alt, size_t begin, size_t end) {
    SubKey key{alt.get(), begin, end};
    auto found = substring_cache_.find(key);
    if (found != substring_cache_.end()) return found->second;
    bool initial_period = begin == 0 ? initial_period_ :
        ((flags_ & DAX_PATTERN_PATHNAME) && text_[begin - 1] == L'/');
    bool result = Matcher(text_.substr(begin, end - begin), flags_, initial_period).matches(alt);
    substring_cache_.emplace(key, result);
    return result;
  }
  bool endpoints_independent(const Sequence &seq) const {
    bool has_group = false, has_star = false;
    for (const Node &node : seq.nodes) {
      has_star |= node.kind == Node::Star;
      if (node.kind != Node::Group) continue;
      has_group = true;
      if (node.ch != L'*' && node.ch != L'+') return false;
      for (const auto &alt : node.alternatives)
        for (const Node &part : alt->nodes)
          if (part.kind == Node::Star || part.kind == Node::Group) return false;
    }
    return !(has_group && has_star);
  }
  const Positions &sequence(const Sequence *seq, size_t index, size_t pos, bool after_star = false,
                            bool star_leading = false) {
    if (index > 0) {
      auto remaining = suffix(seq, index);
      return sequence(remaining.get(), 0, pos, after_star, star_leading);
    }
    Key key{seq, index, pos, after_star, star_leading};
    auto found = cache_.find(key);
    if (found != cache_.end()) return found->second;
    Positions result(text_.size() + 1, 0);
    if (index == seq->nodes.size()) {
      result[pos] = 1;
    } else {
      const Node &node = seq->nodes[index];
      Positions next(text_.size() + 1, 0);
      if (after_star && star_leading && pos < text_.size() && text_[pos] == L'.' &&
          node.kind == Node::Bracket)
        return cache_.emplace(key, std::move(result)).first->second;
      if (after_star && star_leading && pos < text_.size() && text_[pos] == L'.' &&
          node.kind == Node::Group && (node.ch == L'@' || node.ch == L'+' || node.ch == L'!')) {
        Matcher suffix(text_.substr(pos), flags_, true);
        const Positions &ends = suffix.sequence(seq, index, 0);
        for (size_t end = 0; end < ends.size(); ++end)
          if (ends[end]) result[pos + end] = 1;
        return cache_.emplace(key, std::move(result)).first->second;
      }
      if (after_star && node.kind == Node::Group &&
          std::wstring(L"@+!").find(node.ch) != std::wstring::npos &&
          !wildcard_allowed(pos))
        return cache_.emplace(key, std::move(result)).first->second;
      switch (node.kind) {
        case Node::Literal:
          if (pos < text_.size() && node.ch && fold(node.ch) == fold(text_[pos])) next[pos + 1] = 1;
          break;
        case Node::Any:
          if (wildcard_allowed(pos)) next[pos + 1] = 1;
          break;
        case Node::Bracket:
          if (wildcard_allowed(pos)) {
            if (bracket(node.bracket, text_[pos])) next[pos + 1] = 1;
            collating_symbols(node.bracket, pos, next);
          }
          break;
        case Node::Star:
          if (protected_dot(pos)) break;
          next[pos] = 1;
          for (size_t i = pos; wildcard_allowed(i); ++i) next[i + 1] = 1;
          break;
        case Node::Group: {
          if (after_star && (node.ch == L'?' || node.ch == L'*')) {
            std::wstring original = node.source;
            for (size_t tail = index + 1; tail < seq->nodes.size(); ++tail) original += seq->nodes[tail].source;
            Parser scan(original, flags_ & DAX_PATTERN_EXTENDED);
            size_t boundary = scan.star_boundary(1);
            if (boundary == node.source.size()) {
              next[pos] = 1;
              break;
            }
            auto found = star_fallbacks_.find(&node);
            if (found == star_fallbacks_.end()) {
              auto fallback = std::make_shared<Sequence>();
              if (boundary == std::wstring::npos) {
                Node wildcard;
                wildcard.kind = node.ch == L'?' ? Node::Any : Node::Star;
                wildcard.source = node.source.substr(0, 1);
                fallback->nodes.push_back(std::move(wildcard));
                boundary = 1;
              }
              Parser parser(original.substr(boundary), flags_ & DAX_PATTERN_EXTENDED);
              auto contents = parser.parse();
              fallback->nodes.insert(fallback->nodes.end(), contents->nodes.begin(), contents->nodes.end());
              found = star_fallbacks_.emplace(&node, intern(std::move(fallback))).first;
            }
            auto fallback = found->second;
            merge(result, sequence(fallback.get(), 0, pos, true, star_leading));
            return cache_.emplace(key, std::move(result)).first->second;
          }
          if (node.ch == L'@' || node.ch == L'?') {
            // Optional and single alternatives are matched together with the
            // remaining pattern, so adjacent wildcard behavior is preserved.
            if (node.ch == L'?') merge(result, sequence(seq, index + 1, pos));
            auto &alternatives = combined_[&node];
            if (alternatives.empty()) {
              for (const auto &alt : node.alternatives) {
                auto joined = std::make_shared<Sequence>();
                joined->nodes = alt->nodes;
                joined->nodes.insert(joined->nodes.end(), seq->nodes.begin() + index + 1,
                                     seq->nodes.end());
                alternatives.push_back(intern(std::move(joined)));
              }
            }
            // Recursive matching can insert into combined_, so retain copies
            // of its shared pointers rather than an iterator into the map.
            const auto joined_alternatives = alternatives;
            for (const auto &alt : joined_alternatives) merge(result, sequence(alt.get(), 0, pos));
            return cache_.emplace(key, std::move(result)).first->second;
          }
          if (node.ch == L'!') {
            Positions excluded(text_.size() + 1, 0);
            for (const auto &alt : node.alternatives) {
              if (endpoints_independent(*alt)) {
                const Positions &ends = sequence(alt.get(), 0, pos);
                for (size_t end = pos; end <= text_.size(); ++end)
                  if (protected_dot(end) ? alternative_matches(alt, pos, end) : ends[end])
                    excluded[end] = 1;
              } else {
                for (size_t end = pos; end <= text_.size(); ++end)
                  if (alternative_matches(alt, pos, end)) excluded[end] = 1;
              }
            }
            if (!excluded[pos]) next[pos] = 1;
            // GNU extended negation considers every possible suffix, including
            // slashes and leading dots even when those flags restrict '*'.
            for (size_t i = pos; i < text_.size(); ++i)
              if (!excluded[i + 1]) next[i + 1] = 1;
          } else {
            if (node.ch == L'?' || node.ch == L'*') next[pos] = 1;
            std::deque<size_t> todo;
            Positions visited(text_.size() + 1, 0);
            todo.push_back(pos); visited[pos] = 1;
            while (!todo.empty()) {
              size_t start = todo.front(); todo.pop_front();
              for (const auto &alt : node.alternatives) {
                if (endpoints_independent(*alt)) {
                  const Positions &ends = sequence(alt.get(), 0, start);
                  for (size_t end = start; end < ends.size(); ++end)
                    if (protected_dot(end) ? alternative_matches(alt, start, end) : ends[end]) {
                    next[end] = 1;
                    if ((node.ch == L'*' || node.ch == L'+') && !visited[end]) {
                      visited[end] = 1; todo.push_back(end);
                    }
                  }
                  continue;
                }
                for (size_t end = start; end <= text_.size(); ++end) if (alternative_matches(alt, start, end)) {
                  next[end] = 1;
                  if ((node.ch == L'*' || node.ch == L'+') && !visited[end]) {
                    visited[end] = 1; todo.push_back(end);
                  }
                }
              }
            }
          }
          break;
        }
      }
      // glibc searches for @, + and ! groups following a star only at
      // positions where the star can consume a character. It also applies
      // this while skipping intervening ? and optional/repeating groups.
      bool next_after_star = node.kind == Node::Star ||
          (after_star && (node.kind == Node::Any ||
           (node.kind == Node::Group && (node.ch == L'?' || node.ch == L'*'))));
      for (size_t end = pos; end < next.size(); ++end) if (next[end]) {
        bool next_star_leading = false;
        if (node.kind == Node::Star) {
          const bool leading = after_star ? star_leading :
              ((flags_ & DAX_PATTERN_PERIOD) &&
               (pos == 0 ? initial_period_ :
                ((flags_ & DAX_PATTERN_PATHNAME) && text_[pos - 1] == L'/')));
          next_star_leading = leading && end == pos;
        } else if (next_after_star) {
          next_star_leading = star_leading;
        }
        merge(result, sequence(seq, index + 1, end, next_after_star, next_star_leading));
      }
    }
    return cache_.emplace(key, std::move(result)).first->second;
  }
};
}  // namespace

int dax_pattern_match(const char *pattern, const char *text, int flags) {
  Parser parser(wide(pattern), flags & DAX_PATTERN_EXTENDED);
  auto root = parser.parse();
  if (Matcher(wide(text), flags).matches(root)) return 0;
  // glibc also tries the byte representation after an unsuccessful
  // multibyte match. Preserve this behavior when replacing its fnmatch.
  auto byte_text = bytes(text);
  auto byte_pattern = bytes(pattern);
  if (byte_text.size() != wide(text).size() || byte_pattern.size() != wide(pattern).size()) {
    Parser byte_parser(std::move(byte_pattern), flags & DAX_PATTERN_EXTENDED);
    return Matcher(std::move(byte_text), flags).matches(byte_parser.parse()) ? 0 : 1;
  }
  return 1;
}
