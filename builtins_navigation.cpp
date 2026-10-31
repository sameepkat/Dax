#include "builtins_internal.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <iostream>
#include <pwd.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
std::optional<std::string> shell_value(const ShellState &state,
                                       const std::string &name) {
  if (state.unset_vars.count(name)) return std::nullopt;
  const auto value = state.vars.find(name);
  if (value != state.vars.end()) return value->second;
  if (state.ignore_inherited_environment &&
      (name == "CDPATH" || name == "GLOBIGNORE"))
    return std::nullopt;
  if (const char *environment = std::getenv(name.c_str())) return environment;
  return std::nullopt;
}

std::string normalized_logical_path(const std::string &path) {
  const bool absolute = !path.empty() && path.front() == '/';
  std::vector<std::string> parts;
  std::stringstream input(path);
  std::string part;
  while (std::getline(input, part, '/')) {
    if (part.empty() || part == ".") continue;
    if (part == "..") {
      if (!parts.empty() && parts.back() != "..") parts.pop_back();
      else if (!absolute) parts.push_back(part);
    } else parts.push_back(part);
  }
  std::string result = absolute ? "/" : "";
  for (const auto &item : parts) {
    if (!result.empty() && result.back() != '/') result += '/';
    result += item;
  }
  if (result.empty()) return absolute ? "/" : ".";
  return result;
}

std::string current_logical_pwd(const ShellState &state) {
  char physical[4096];
  if (!getcwd(physical, sizeof(physical))) return {};
  const auto pwd = shell_value(state, "PWD");
  if (!pwd || pwd->empty() || pwd->front() != '/') return physical;
  struct stat logical_info {}, current_info {};
  if (stat(pwd->c_str(), &logical_info) == 0 && stat(".", &current_info) == 0 &&
      logical_info.st_dev == current_info.st_dev && logical_info.st_ino == current_info.st_ino)
    return *pwd;
  return physical;
}

void sync_directory_stack(ShellState &state) {
  if (state.disabled_special_parameters.count("DIRSTACK")) return;
  std::vector<std::string> stack;
  const std::string current = current_logical_pwd(state);
  if (!current.empty()) stack.push_back(current);
  stack.insert(stack.end(), state.dirs.begin(), state.dirs.end());
  state.arrays["DIRSTACK"] = std::move(stack);
  shell_array_mark_all_set("DIRSTACK");
}

std::string expand_cd_tilde(std::string path, const ShellState &state) {
  if (path.empty() || path[0] != '~') return path;
  const size_t slash = path.find('/');
  const std::string user = path.substr(1, slash == std::string::npos ? std::string::npos : slash - 1);
  const char *home = nullptr;
  const auto configured_home = user.empty() ? shell_value(state, "HOME") : std::nullopt;
  if (user.empty()) home = configured_home ? configured_home->c_str() : nullptr;
  else if (const passwd *entry = getpwnam(user.c_str())) home = entry->pw_dir;
  if (!home) return path;
  return std::string(home) + (slash == std::string::npos ? "" : path.substr(slash));
}

bool one_directory_typo(const std::string &typed, const std::string &actual) {
  if (typed.size() == actual.size()) {
    size_t first = 0;
    while (first < typed.size() && typed[first] == actual[first]) ++first;
    if (first + 1 >= typed.size()) return false;
    size_t last = typed.size();
    while (last > first && typed[last - 1] == actual[last - 1]) --last;
    return last == first + 2 && typed[first] == actual[first + 1] &&
           typed[first + 1] == actual[first];
  }
  const std::string *longer = &typed;
  const std::string *shorter = &actual;
  if (typed.size() < actual.size()) std::swap(longer, shorter);
  if (longer->size() != shorter->size() + 1) return false;
  for (size_t skipped = 0; skipped < longer->size(); ++skipped) {
    if (longer->compare(0, skipped, *shorter, 0, skipped) == 0 &&
        longer->compare(skipped + 1, std::string::npos, *shorter, skipped,
                        std::string::npos) == 0)
      return true;
  }
  return false;
}

std::optional<std::string> correct_directory_spelling(const std::string &path) {
  if (path.empty()) return std::nullopt;
  const bool absolute = path.front() == '/';
  std::string corrected = absolute ? "/" : std::string();
  bool changed = false;
  size_t begin = absolute ? 1 : 0;
  while (begin < path.size()) {
    const size_t slash = path.find('/', begin);
    const std::string component = path.substr(
        begin, slash == std::string::npos ? std::string::npos : slash - begin);
    begin = slash == std::string::npos ? path.size() : slash + 1;
    if (component.empty()) continue;
    auto append_component = [&](const std::string &value) {
      if (corrected.empty()) corrected = value;
      else if (corrected.back() == '/') corrected += value;
      else corrected += '/' + value;
    };
    if (component == "." || component == "..") {
      append_component(component);
      continue;
    }
    const std::string parent = corrected.empty() ? "." : corrected;
    const std::string exact = parent == "/" ? parent + component
                                               : parent + "/" + component;
    struct stat info {};
    if (stat(exact.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
      append_component(component);
      continue;
    }
    DIR *directory = opendir(parent.c_str());
    if (!directory) return std::nullopt;
    std::string match;
    bool ambiguous = false;
    while (const dirent *entry = readdir(directory)) {
      const std::string name = entry->d_name;
      if (name == "." || name == ".." || !one_directory_typo(component, name)) continue;
      const std::string candidate = parent == "/" ? parent + name
                                                     : parent + "/" + name;
      if (stat(candidate.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) continue;
      if (!match.empty()) { ambiguous = true; break; }
      match = name;
    }
    closedir(directory);
    if (match.empty() || ambiguous) return std::nullopt;
    append_component(match);
    changed = true;
  }
  if (!changed) return std::nullopt;
  if (path.back() == '/' && corrected.back() != '/') corrected += '/';
  return corrected;
}
}

std::optional<int> run_navigation_builtin(const std::vector<std::string> &a,
                                          ShellState &state) {
  if (a.empty()) return 0;
  const auto &c = a[0];
  if (c == "cd") {
    bool logical = !state.physical_directory;
    bool physical = state.physical_directory, require_physical_pwd = false;
    size_t i = 1;
    for (; i < a.size() && a[i].size() > 1 && a[i][0] == '-' && a[i] != "-"; ++i) {
      if (a[i] == "--") { ++i; break; }
      for (size_t j = 1; j < a[i].size(); ++j) {
        if (a[i][j] == 'L') { logical = true; physical = false; }
        else if (a[i][j] == 'P') { logical = false; physical = true; }
        else if (a[i][j] == 'e') require_physical_pwd = true;
        else {
          std::cerr << "cd: invalid option -- '" << a[i][j] << "'\n";
          return 2;
        }
      }
    }
    if (a.size() - i > 1) {
      std::cerr << "cd: too many arguments\n";
      return 1;
    }
    const auto home = shell_value(state, "HOME");
    const std::string old_pwd = current_logical_pwd(state);
    const auto old = shell_value(state, "OLDPWD");
    std::string p = i < a.size() ? a[i] : home.value_or("");
    bool print_old_pwd = p == "-";
    if (print_old_pwd) p = old.value_or("");
    if (p.empty()) {
      std::cerr << (print_old_pwd || (i < a.size() && a[i] == "-")
                        ? "cd: OLDPWD not set\n" : "cd: HOME not set\n");
      return 1;
    }
    p = expand_cd_tilde(std::move(p), state);
    const bool use_cdpath = p[0] != '/' && p != "." && p.rfind("./", 0) != 0 &&
                            p != ".." && p.rfind("../", 0) != 0 && !print_old_pwd;
    std::vector<std::pair<std::string, bool>> candidates;
    const auto cdpath = use_cdpath ? shell_value(state, "CDPATH") : std::nullopt;
    if (cdpath && !cdpath->empty()) {
      std::stringstream paths(*cdpath);
      std::string prefix;
      while (std::getline(paths, prefix, ':')) {
        const std::string candidate = prefix.empty() ? p : prefix + "/" + p;
        candidates.emplace_back(candidate, !prefix.empty());
      }
    }
    candidates.emplace_back(p, false);
    if (state.cdable_vars) {
      const auto variable_directory = shell_value(state, p);
      if (variable_directory && !variable_directory->empty())
        candidates.emplace_back(expand_cd_tilde(*variable_directory, state), false);
    }
    bool changed = false, cdpath_used = false, spelling_corrected = false;
    std::string corrected_display;
    std::string logical_target;
    int last_error = ENOENT;
    auto try_candidate = [&](const std::string &candidate, bool from_cdpath,
                             bool corrected) {
      std::string target = candidate;
      if (logical) {
        if (target.empty() || target[0] != '/') target = old_pwd + "/" + target;
        target = normalized_logical_path(target);
      }
      if (chdir(target.c_str()) == 0) {
        changed = true;
        cdpath_used = from_cdpath;
        spelling_corrected = corrected;
        if (corrected) corrected_display = candidate;
        logical_target = logical ? target : std::string();
        return true;
      }
      last_error = errno;
      return false;
    };
    for (const auto &[candidate, from_cdpath] : candidates) {
      if (try_candidate(candidate, from_cdpath, false)) break;
    }
    if (!changed && state.interactive_shell && state.cdspell) {
      for (const auto &[candidate, from_cdpath] : candidates) {
        const auto correction = correct_directory_spelling(candidate);
        if (correction && try_candidate(*correction, from_cdpath, true)) break;
      }
    }
    if (!changed) {
      std::cerr << "cd: " << strerror(last_error) << '\n';
      return 1;
    }
    char physical_pwd[4096];
    const bool physical_pwd_available = getcwd(physical_pwd, sizeof(physical_pwd)) != nullptr;
    if (physical_pwd_available) {
      const std::string new_pwd = logical ? logical_target : physical_pwd;
      setenv("OLDPWD", old_pwd.c_str(), 1);
      setenv("PWD", new_pwd.c_str(), 1);
      state.vars["OLDPWD"] = old_pwd;
      state.vars["PWD"] = new_pwd;
      sync_directory_stack(state);
    } else if (logical && !logical_target.empty()) {
      setenv("OLDPWD", old_pwd.c_str(), 1);
      setenv("PWD", logical_target.c_str(), 1);
      state.vars["OLDPWD"] = old_pwd;
      state.vars["PWD"] = logical_target;
      sync_directory_stack(state);
    } else if (physical && require_physical_pwd) {
      perror("cd: cannot determine current directory");
      return 1;
    }
    if (spelling_corrected) std::cout << corrected_display << '\n';
    else if (print_old_pwd || cdpath_used) std::cout << current_logical_pwd(state) << '\n';
    return 0;
  }
  if (c == "pwd") {
    bool logical = true;
    size_t i = 1;
    for (; i < a.size() && a[i].size() > 1 && a[i][0] == '-'; ++i) {
      if (a[i] == "--") { ++i; break; }
      for (size_t j = 1; j < a[i].size(); ++j) {
        if (a[i][j] == 'L') logical = true;
        else if (a[i][j] == 'P') logical = false;
        else {
          std::cerr << "pwd: invalid option -- '" << a[i][j] << "'\n";
          return 2;
        }
      }
    }
    if (i != a.size()) {
      std::cerr << "pwd: too many arguments\n";
      return 2;
    }
    char b[4096];
    if (logical) {
      const std::string pwd = current_logical_pwd(state);
      if (pwd.empty()) { perror("pwd"); return 1; }
      std::cout << pwd << '\n';
      return 0;
    }
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
      sync_directory_stack(state);
      const auto &stack = state.disabled_special_parameters.count("DIRSTACK")
                              ? state.dirs
                              : state.arrays["DIRSTACK"];
      bool first = true;
      if (state.disabled_special_parameters.count("DIRSTACK")) {
        std::cout << current_logical_pwd(state);
        first = false;
      }
      for (const auto &directory : stack) {
        if (!first) std::cout << ' ';
        std::cout << directory;
        first = false;
      }
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
    state.vars["PWD"] = cwd;
    sync_directory_stack(state);
    std::cout << cwd << '\n';
    return 0;
  }
  return {};
}
