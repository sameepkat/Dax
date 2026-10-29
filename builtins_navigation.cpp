#include "builtins_internal.hpp"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

std::optional<int> run_navigation_builtin(const std::vector<std::string> &a,
                                          ShellState &state) {
  if (a.empty()) return 0;
  const auto &c = a[0];
  if (c == "cd") {
    if (a.size() > 2) {
      std::cerr << "cd: too many arguments\n";
      return 1;
    }
    const char *h = getenv("HOME");
    const char *old = getenv("OLDPWD");
    std::string p = a.size() > 1 ? a[1] : (h ? h : "");
    if (p == "-")
      p = old ? old : "";
    if (p.empty()) {
      std::cerr << "cd: HOME not set\n";
      return 1;
    }
    if (p[0] == '~' && (p.size() == 1 || p[1] == '/') && h)
      p = std::string(h) + p.substr(1);
    char before[4096], after[4096];
    getcwd(before, sizeof(before));
    if (chdir(p.c_str())) {
      std::cerr << "cd: " << strerror(errno) << '\n';
      return 1;
    }
    if (getcwd(after, sizeof(after))) {
      setenv("OLDPWD", before, 1);
      setenv("PWD", after, 1);
      state.vars["OLDPWD"] = before;
      state.vars["PWD"] = after;
    }
    if (a.size() > 1 && a[1] == "-")
      std::cout << getenv("PWD") << '\n';
    return 0;
  }
  if (c == "pwd") {
    char b[4096];
    if (!getcwd(b, sizeof(b))) {
      perror("pwd");
      return 1;
    }
    std::cout << b << '\n';
    return 0;
  }
  if (c == "pushd" || c == "popd" || c == "dirs") {
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd)))
      return 1;
    if (c == "dirs") {
      std::cout << cwd;
      for (auto &d : state.dirs)
        std::cout << ' ' << d;
      std::cout << '\n';
      return 0;
    }
    if (c == "pushd" && a.size() > 1) {
      if (chdir(a[1].c_str()))
        return 1;
      state.dirs.insert(state.dirs.begin(), cwd);
    } else if (c == "pushd") {
      if (state.dirs.empty())
        return 1;
      std::string target = state.dirs.front();
      state.dirs.erase(state.dirs.begin());
      if (chdir(target.c_str()))
        return 1;
      state.dirs.insert(state.dirs.begin(), cwd);
    } else {
      if (state.dirs.empty())
        return 1;
      std::string target = state.dirs.front();
      if (chdir(target.c_str()))
        return 1;
      state.dirs.erase(state.dirs.begin());
    }
    getcwd(cwd, sizeof(cwd));
    setenv("PWD", cwd, 1);
    std::cout << cwd << '\n';
    return 0;
  }
  return {};
}
