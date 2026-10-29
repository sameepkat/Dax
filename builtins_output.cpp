#include "builtins_internal.hpp"
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

std::optional<int> run_output_builtin(const std::vector<std::string> &a) {
  if (a.empty()) return 0;
  const auto &c = a[0];
  if (c == "echo") {
    size_t i = 1;
    bool nl = true;
    if (i < a.size() && a[i] == "-n") {
      ++i;
      nl = false;
    }
    for (size_t j = i; j < a.size(); ++j) {
      if (j > i)
        std::cout << ' ';
      std::cout << a[j];
    }
    if (nl)
      std::cout << '\n';
    return 0;
  }
  if (c == "printf") {
    if (a.size() < 2)
      return 0;
    size_t n = 2;
    for (size_t i = 0; i < a[1].size(); ++i) {
      if (a[1][i] == '%' && i + 1 < a[1].size()) {
        char f = a[1][++i];
        if (f == '%')
          std::cout << '%';
        else if (f == 's')
          std::cout << (n < a.size() ? a[n++] : "");
        else if (f == 'd' || f == 'i')
          std::cout << atoi(n < a.size() ? a[n++].c_str() : "0");
        else
          return 1;
      } else if (a[1][i] == '\\' && i + 1 < a[1].size()) {
        char x = a[1][++i];
        std::cout << (x == 'n' ? '\n' : x == 't' ? '\t' : x);
      } else
        std::cout << a[1][i];
    }
    return 0;
  }
  if (c == "test" || c == "[") {
    size_t end = a.size();
    if (c == "[") {
      if (end < 2 || a.back() != "]") {
        std::cerr << "[: missing ]\n";
        return 2;
      }
      --end;
    }
    auto eval = [&](auto &&self, size_t b, size_t e) -> int {
      if (b == e)
        return 1;
      if (a[b] == "!")
        return self(self, b + 1, e) == 0 ? 0 : 1;
      if (e - b == 1)
        return a[b].empty() ? 1 : 0;
      if (e - b == 2) {
        struct stat st;
        const auto &op = a[b];
        const auto &v = a[b + 1];
        bool ok = false;
        if (op == "-n")
          ok = !v.empty();
        else if (op == "-z")
          ok = v.empty();
        else if (op == "-e" || op == "-a")
          ok = access(v.c_str(), F_OK) == 0;
        else if (op == "-f")
          ok = stat(v.c_str(), &st) == 0 && S_ISREG(st.st_mode);
        else if (op == "-d")
          ok = stat(v.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        else if (op == "-r")
          ok = access(v.c_str(), R_OK) == 0;
        else if (op == "-w")
          ok = access(v.c_str(), W_OK) == 0;
        else if (op == "-x")
          ok = access(v.c_str(), X_OK) == 0;
        else if (op == "-s")
          ok = stat(v.c_str(), &st) == 0 && st.st_size > 0;
        else
          return 2;
        return ok ? 0 : 1;
      }
      for (size_t i = b + 1; i + 1 < e; ++i)
        if (a[i] == "-a" || a[i] == "-o") {
          int l = self(self, b, i), r = self(self, i + 1, e);
          return a[i] == "-a" ? (l == 0 && r == 0 ? 0 : 1)
                              : (l == 0 || r == 0 ? 0 : 1);
        }
      if (e - b != 3)
        return 2;
      const auto &l = a[b];
      const auto &op = a[b + 1];
      const auto &r = a[b + 2];
      if (op == "=" || op == "==")
        return l == r ? 0 : 1;
      if (op == "!=")
        return l != r ? 0 : 1;
      char *le = nullptr;
      char *re = nullptr;
      long lv = strtol(l.c_str(), &le, 10), rv = strtol(r.c_str(), &re, 10);
      if (!le || *le || !re || *re)
        return 2;
      if (op == "-eq")
        return lv == rv ? 0 : 1;
      if (op == "-ne")
        return lv != rv ? 0 : 1;
      if (op == "-gt")
        return lv > rv ? 0 : 1;
      if (op == "-ge")
        return lv >= rv ? 0 : 1;
      if (op == "-lt")
        return lv < rv ? 0 : 1;
      if (op == "-le")
        return lv <= rv ? 0 : 1;
      return 2;
    };
    return eval(eval, 1, end);
  }
  return {};
}
