#include "builtins_internal.hpp"
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <unistd.h>
#include <vector>

std::optional<int> run_system_builtin(const std::vector<std::string> &a) {
  if (a.empty()) return 0;
  const auto &c = a[0];
  if (c == "umask") {
    if (a.size() == 1) {
      const mode_t current = umask(0);
      umask(current);
      std::cout << std::oct;
      std::cout.width(4);
      std::cout.fill('0');
      std::cout << current << std::dec << '\n';
      return 0;
    }
    size_t index = 1;
    bool symbolic = a[index] == "-S";
    if (symbolic) {
      const mode_t current = umask(0);
      umask(current);
      const mode_t allowed = static_cast<mode_t>(~current) & 0777;
      auto perms = [&](mode_t read, mode_t write, mode_t execute) {
        std::string value;
        value += (allowed & read) ? 'r' : '-';
        value += (allowed & write) ? 'w' : '-';
        value += (allowed & execute) ? 'x' : '-';
        return value;
      };
      std::cout << "u=" << perms(S_IRUSR, S_IWUSR, S_IXUSR)
                << ",g=" << perms(S_IRGRP, S_IWGRP, S_IXGRP)
                << ",o=" << perms(S_IROTH, S_IWOTH, S_IXOTH) << '\n';
      return a.size() == 2 ? 0 : 2;
    }
    if (a.size() != 2) return 2;
    char *end = nullptr;
    errno = 0;
    const unsigned long value = std::strtoul(a[index].c_str(), &end, 8);
    if (errno || end == a[index].c_str() || *end || value > 0777) {
      std::cerr << "umask: expected an octal mask from 000 to 777\n";
      return 2;
    }
    umask(static_cast<mode_t>(value));
    return 0;
  }
  if (c == "times") {
    if (a.size() != 1) return 2;
    struct tms usage{};
    if (times(&usage) == static_cast<clock_t>(-1)) {
      perror("times");
      return 1;
    }
    const long ticks = sysconf(_SC_CLK_TCK);
    auto print_time = [&](clock_t value) {
      const double seconds = ticks > 0 ? static_cast<double>(value) / ticks : 0.0;
      const long minutes = static_cast<long>(seconds / 60.0);
      std::cout << minutes << 'm' << (seconds - minutes * 60.0) << "s";
    };
    print_time(usage.tms_utime); std::cout << ' ';
    print_time(usage.tms_stime); std::cout << '\n';
    print_time(usage.tms_cutime); std::cout << ' ';
    print_time(usage.tms_cstime); std::cout << '\n';
    return 0;
  }
  if (c == "ulimit") {
    char option = 'f';
    size_t index = 1;
    if (index < a.size() && a[index].size() == 2 && a[index][0] == '-') {
      option = a[index][1];
      ++index;
    }
    auto resource_for = [&](char which) -> int {
      if (which == 'f') return RLIMIT_FSIZE;
#ifdef RLIMIT_NOFILE
      if (which == 'n') return RLIMIT_NOFILE;
#endif
      return -1;
    };
    auto describe = [&](char which) {
      const int resource = resource_for(which);
      if (resource < 0) return false;
      struct rlimit limit{};
      if (getrlimit(resource, &limit) != 0) return false;
      if (limit.rlim_cur == RLIM_INFINITY) std::cout << "unlimited\n";
      else if (which == 'f') std::cout << (limit.rlim_cur / 512) << '\n';
      else std::cout << limit.rlim_cur << '\n';
      return true;
    };
    if (option == 'a') {
      if (index != a.size()) return 2;
      std::cout << "file size (blocks) "; describe('f');
      std::cout << "open files         "; describe('n');
      return 0;
    }
    const int resource = resource_for(option);
    if (resource < 0) {
      std::cerr << "ulimit: supported limits are -f, -n, and -a\n";
      return 2;
    }
    struct rlimit limit{};
    if (getrlimit(resource, &limit) != 0) { perror("ulimit"); return 1; }
    if (index == a.size()) return describe(option) ? 0 : 1;
    if (index + 1 != a.size()) return 2;
    rlim_t parsed = RLIM_INFINITY;
    if (a[index] != "unlimited") {
      char *end = nullptr;
      errno = 0;
      unsigned long long value = std::strtoull(a[index].c_str(), &end, 10);
      if (errno || end == a[index].c_str() || *end) return 2;
      if (option == 'f') value *= 512;
      parsed = static_cast<rlim_t>(value);
    }
    limit.rlim_cur = parsed;
    if (setrlimit(resource, &limit) != 0) { perror("ulimit"); return 1; }
    return 0;
  }
  return {};
}
