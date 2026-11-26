#include "pattern_match.hpp"

#include <clocale>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <vector>
#if defined(__GLIBC__)
#include <fnmatch.h>
static int native_flags(int flags) {
  return (flags & DAX_PATTERN_EXTENDED ? FNM_EXTMATCH : 0) |
         (flags & DAX_PATTERN_CASEFOLD ? FNM_CASEFOLD : 0) |
         (flags & DAX_PATTERN_PATHNAME ? FNM_PATHNAME : 0) |
         (flags & DAX_PATTERN_PERIOD ? FNM_PERIOD : 0);
}
#endif

struct Case { const char *pattern, *text; int flags; bool matched; };
int main(int argc, char **argv) {
  std::setlocale(LC_ALL, "C");
  if (argc > 1 && std::string(argv[1]) == "--posix") {
    if (dax_pattern_match("[^a]", "^", 0) != 0 ||
        dax_pattern_match("[^a]", "a", 0) != 0 ||
        dax_pattern_match("[^a]", "b", 0) == 0) {
      std::cerr << "POSIX bracket negation test failed\n";
      return 1;
    }
    std::cout << "POSIX bracket negation test passed\n";
    return 0;
  }
  const Case cases[] = {
      {"@(cat|dog)", "dog", DAX_PATTERN_EXTENDED, true},
      {"+(ab|c)", "abcab", DAX_PATTERN_EXTENDED, true},
      {"*(ab|c)", "", DAX_PATTERN_EXTENDED, true},
      {"?(ab|c)", "abc", DAX_PATTERN_EXTENDED, false},
      {"!(foo|bar).txt", "baz.txt", DAX_PATTERN_EXTENDED, true},
      {"!(foo|bar).txt", "foo.txt", DAX_PATTERN_EXTENDED, false},
      {"@(a|@(ab|x))cd", "abcd", DAX_PATTERN_EXTENDED, true},
      {"[[:digit:]]", "8", 0, true},
      {"[!a-c]", "b", 0, false},
      {"a\\*", "a*", 0, true},
      {"?", "/", DAX_PATTERN_PATHNAME, false},
      {"*", ".hidden", DAX_PATTERN_PERIOD, false},
      {"[.]hidden", ".hidden", DAX_PATTERN_PERIOD, false},
      {".hidden", ".hidden", DAX_PATTERN_PERIOD, true},
      {"ABC", "abc", DAX_PATTERN_CASEFOLD, true},
      {"[", "[", 0, true},
      {"abc\\", "abc", 0, false},
      {"*.", ".", DAX_PATTERN_PERIOD, false},
      {"[![:notaclass:]]", "a", 0, false},
      {"[[[:notaclass:]]", "[", 0, true},
      {"[ab[=]", "a", 0, false},
      {"[ab[=]", "[", 0, true},
      {"[!ab[=]", "c", 0, true},
      {"[ab[.]", "a", 0, false},
      {"*!(a|b)", "", DAX_PATTERN_EXTENDED, false},
      {"*!(a|b)", "/", DAX_PATTERN_EXTENDED | DAX_PATTERN_PATHNAME, false},
      {"*?(|a)@(a|!(b|a))", "", DAX_PATTERN_EXTENDED, false},
      {"!(*!(]))?", "*", DAX_PATTERN_EXTENDED | DAX_PATTERN_PERIOD, true},
      {"*?+(*)", "a.", DAX_PATTERN_EXTENDED | DAX_PATTERN_PERIOD, false},
      {"*?!(*)", "a.", DAX_PATTERN_EXTENDED | DAX_PATTERN_PERIOD, true},
      {"**(?()))", "", DAX_PATTERN_EXTENDED | DAX_PATTERN_PERIOD, true},
      {"@(*)*(?()))", "b*", DAX_PATTERN_EXTENDED | DAX_PATTERN_PERIOD, true},
      {"+(*)!()", ".bbaa.", DAX_PATTERN_EXTENDED | DAX_PATTERN_PERIOD | DAX_PATTERN_PATHNAME, true},
      {"+(*)!()", ".*b", DAX_PATTERN_EXTENDED | DAX_PATTERN_PERIOD | DAX_PATTERN_PATHNAME, true},
      {"*(/[!a]|[!a])", "*b*/.", DAX_PATTERN_EXTENDED | DAX_PATTERN_PERIOD | DAX_PATTERN_PATHNAME, false},
      {"*?(@(a|b)@(a|b))", "b", DAX_PATTERN_EXTENDED, false},
  };
  for (const auto &c : cases) {
    bool actual = dax_pattern_match(c.pattern, c.text, c.flags) == 0;
    if (actual != c.matched) {
      std::cerr << "pattern test failed: " << c.pattern << " against " << c.text << '\n';
      return 1;
    }
  }
  std::string repeated(600, 'a');
  repeated += 'b';
  if (dax_pattern_match("*(a|aa|aaa)b", repeated.c_str(), DAX_PATTERN_EXTENDED) != 0) {
    std::cerr << "Repeated extended pattern test failed\n";
    return 1;
  }
  std::string nested_repeated(150, 'a');
  nested_repeated += 'b';
  if (dax_pattern_match("*(*(a|aa))b", nested_repeated.c_str(), DAX_PATTERN_EXTENDED) != 0 ||
      dax_pattern_match("!(*a*a*a*a)b", nested_repeated.c_str(), DAX_PATTERN_EXTENDED) == 0) {
    std::cerr << "Nested repeated pattern test failed\n";
    return 1;
  }
  std::string alternatives, alternative_input;
  for (int i = 0; i < 40; ++i) { alternatives += "@(a|a)"; alternative_input += 'a'; }
  alternatives += 'b';
  alternative_input += 'c';
  if (dax_pattern_match(alternatives.c_str(), alternative_input.c_str(), DAX_PATTERN_EXTENDED) == 0) {
    std::cerr << "Repeated alternatives test failed\n";
    return 1;
  }
  if (std::setlocale(LC_ALL, "C.UTF-8") || std::setlocale(LC_ALL, "en_US.UTF-8")) {
    const Case unicode[] = {
        {"?", "é", 0, true},
        {"[é]", "é", 0, true},
        {"É", "é", DAX_PATTERN_CASEFOLD, true},
        {"@(é|猫)", "猫", DAX_PATTERN_EXTENDED, true},
        // glibc retries matching byte-by-byte after a multibyte mismatch.
        {"??", "é", 0, true},
        {"[[:upper:]]?", "é", 0, false},
    };
    for (const auto &c : unicode) {
      if ((dax_pattern_match(c.pattern, c.text, c.flags) == 0) != c.matched) {
        std::cerr << "Unicode pattern test failed: " << c.pattern << " against " << c.text << '\n';
        return 1;
      }
    }
  } else {
    std::cout << "UTF-8 locale unavailable; Unicode matcher cases skipped\n";
  }
#if defined(__GLIBC__)
  if (std::setlocale(LC_ALL, "en_US.UTF-8") || std::setlocale(LC_ALL, "en_US.utf8")) {
    const Case collating[] = {
        {"[a-b]", "á", 0, true},
        {"[a-b]", "A", 0, false},
        {"[[=a=]]", "A", 0, true},
        {"[[.a.]]", "A", DAX_PATTERN_CASEFOLD, false},
    };
    for (const auto &c : collating) {
      if ((dax_pattern_match(c.pattern, c.text, c.flags) == 0) != c.matched) {
        std::cerr << "Collation test failed: " << c.pattern << " against " << c.text << '\n';
        return 1;
      }
    }
    const std::vector<std::string> locale_patterns = {
        "?", "??", "[a-b]", "[A-Z]", "[[=a=]]", "[[.a.]]", "[[:alpha:]]",
        "[[:upper:]]?", "@(é|猫)", "!(é)", "*[!b]"};
    const std::vector<std::string> locale_texts = {"", "a", "A", "á", "é", "É", "猫", "aa", "éa", ".é"};
    for (const auto &pattern : locale_patterns) for (const auto &text : locale_texts)
      for (int flags = 0; flags < 16; ++flags) {
        bool expected = fnmatch(pattern.c_str(), text.c_str(), native_flags(flags)) == 0;
        bool actual = dax_pattern_match(pattern.c_str(), text.c_str(), flags) == 0;
        if (actual != expected) {
          std::cerr << "Locale mismatch: " << pattern << " against " << text << " flags=" << flags << '\n';
          return 1;
        }
      }
  } else {
    std::cout << "en_US UTF-8 locale unavailable; locale collation cases skipped\n";
  }
#endif
  if (std::setlocale(LC_ALL, "cs_CZ.UTF-8") || std::setlocale(LC_ALL, "cs_CZ.utf8")) {
    if (dax_pattern_match("[[.ch.]]", "ch", 0) != 0 ||
        dax_pattern_match("[[.ch.]]", "c", 0) == 0 ||
        dax_pattern_match("[![.ch.]]", "c", 0) != 0 ||
        dax_pattern_match("[[.ch.]-z]", "a", 0) != 0 ||
        dax_pattern_match("[[.ch.]-z]", "ch", 0) == 0 ||
        dax_pattern_match("[a-[.ch.]]", "a", 0) == 0) {
      std::cerr << "Multi-character collating symbol test failed\n";
      return 1;
    }
#if defined(__GLIBC__)
    const std::vector<std::string> symbols = {
        "[[.ch.]]", "[![.ch.]]", "[[.ch.]a]", "[[.ch.]-z]",
        "[a-[.ch.]]", "[[.ch.]-[.ch.]]", "[a-[.ch.]x]", "[a-[.b.]]", "[[.a.]-c]"};
    const std::vector<std::string> symbol_texts = {"a", "b", "c", "h", "i", "z", "ch", "x", "0", ".", ""};
    for (const auto &pattern : symbols) for (const auto &text : symbol_texts)
      for (int flags = 0; flags < 16; ++flags) {
        bool expected = fnmatch(pattern.c_str(), text.c_str(), native_flags(flags)) == 0;
        bool actual = dax_pattern_match(pattern.c_str(), text.c_str(), flags) == 0;
        if (actual != expected) {
          std::cerr << "Collating symbol mismatch: " << pattern << " against " << text << " flags=" << flags << '\n';
          return 1;
        }
      }
#endif
  } else {
    std::cout << "Czech UTF-8 locale unavailable; collating symbol case skipped\n";
  }
  std::setlocale(LC_ALL, "C");
#if defined(__GLIBC__)
  const std::vector<std::string> patterns = {
      "", "a", "*", "?", "a*", "*a", "a?b", "[a-c]", "[!ab]", "[[:alpha:]]",
      "[[:upper:]]", "[.]", "[a-]", "[]a]", "[", "a\\*", "a\\", "a/b",
      "@(a|b)", "?(a|b)", "*(a|b)", "+(a|b)", "!(a|b)", "!(a|b)*",
      "@(a|@(ab|b))", "a!(b|c)d", "*(a|)", "+(|a)", "@(.a|b)",
      "*(a|b)/?", "@([a-c]|?)", "@(a|b", "!()", "@()", "*(a|aa)b",
      "*?(|a)@(a|!(b|a))", "**(a|b)!(a|b)", "@(\\@(a|!(b|a))",
      "[ab[=]", "[ab[.]", "[![:notaclass:]]", "[[[:notaclass:]]", "*."};
  const std::vector<std::string> texts = {
      "", "a", "b", "c", "A", "ab", "abc", "aab", "aaaab", "d", ".", ".a", "/", "a/b", "a/.a", "a*", "[", "abcd"};
  size_t count = 0, failures = 0;
  for (const auto &pattern : patterns) for (const auto &text : texts) for (int flags = 0; flags < 16; ++flags) {
    bool expected = fnmatch(pattern.c_str(), text.c_str(), native_flags(flags)) == 0;
    bool actual = dax_pattern_match(pattern.c_str(), text.c_str(), flags) == 0;
    if (actual != expected) {
      std::cerr << "glibc mismatch: pattern=" << pattern << " text=" << text
                << " flags=" << flags << " expected=" << expected << '\n';
      if (++failures == 40) return 1;
    }
    ++count;
  }
  std::mt19937 random(0x444158);
  const std::vector<std::string> atoms = {"a", "b", "?", "*", "[ab]", "[!a]", "[[:alpha:]]", "[![:notaclass:]]", "[ab[=]", "\\*", "/", ".", "@(a|b)", "?(a|b)", "*(a|b)", "+(a|b)", "!(a|b)", "@(a|!(b|a))"};
  const std::string alphabet = "ab./*";
  for (size_t trial = 0; trial < 10000; ++trial) {
    std::string pattern, text;
    for (unsigned n = random() % 4; n; --n) pattern += atoms[random() % atoms.size()];
    for (unsigned n = random() % 6; n; --n) text += alphabet[random() % alphabet.size()];
    int flags = random() % 16;
    bool expected = fnmatch(pattern.c_str(), text.c_str(), native_flags(flags)) == 0;
    bool actual = dax_pattern_match(pattern.c_str(), text.c_str(), flags) == 0;
    if (actual != expected) {
      std::cerr << "glibc generated mismatch: pattern=" << pattern << " text=" << text
                << " flags=" << flags << " expected=" << expected << '\n';
      if (++failures == 40) return 1;
    }
    ++count;
  }
  const std::vector<std::string> nested_atoms = {"a", "b", "?", "*", ".", "/", "[ab]", "[!a]", "\\*"};
  const std::string operators = "?*+@!";
  std::function<std::string(int)> make_pattern = [&](int depth) {
    std::string pattern;
    for (unsigned parts = 1 + random() % 3; parts; --parts) {
      if (depth && random() % 5 == 0) {
        pattern += operators[random() % operators.size()];
        pattern += '(';
        pattern += make_pattern(depth - 1);
        if (random() % 2) { pattern += '|'; pattern += make_pattern(depth - 1); }
        pattern += ')';
      } else {
        pattern += nested_atoms[random() % nested_atoms.size()];
      }
    }
    return pattern;
  };
  for (size_t trial = 0; trial < 10000; ++trial) {
    std::string pattern = make_pattern(2), text;
    for (unsigned n = random() % 7; n; --n) text += alphabet[random() % alphabet.size()];
    int flags = random() % 16;
    bool expected = fnmatch(pattern.c_str(), text.c_str(), native_flags(flags)) == 0;
    bool actual = dax_pattern_match(pattern.c_str(), text.c_str(), flags) == 0;
    if (actual != expected) {
      std::cerr << "glibc nested mismatch: pattern=" << pattern << " text=" << text
                << " flags=" << flags << " expected=" << expected << '\n';
      if (++failures == 40) return 1;
    }
    ++count;
  }
  if (failures) return 1;
  std::cout << "Pattern matcher: " << count << " glibc comparisons passed\n";
#else
  std::cout << "Pattern matcher cases passed (glibc comparisons skipped)\n";
#endif
}
