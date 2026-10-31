#include "builtins_internal.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <limits>
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
    size_t index = 1;
    bool symbolic = false, reusable = false;
    while (index < a.size() && a[index].size() > 1 && a[index][0] == '-') {
      if (a[index] == "--") { ++index; break; }
      for (size_t flag = 1; flag < a[index].size(); ++flag) {
        if (a[index][flag] == 'S') symbolic = true;
        else if (a[index][flag] == 'p') reusable = true;
        else {
          std::cerr << "umask: invalid option -- '" << a[index][flag] << "'\n";
          return 2;
        }
      }
      ++index;
    }
    if (index == a.size()) {
      const mode_t current = umask(0);
      umask(current);
      if (reusable) std::cout << "umask ";
      if (symbolic) {
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
      } else {
        const char previous_fill = std::cout.fill();
        std::cout << std::oct;
        std::cout.width(4);
        std::cout.fill('0');
        std::cout << current << std::dec << '\n';
        std::cout.fill(previous_fill);
      }
      return 0;
    }
    if (index + 1 != a.size()) return 2;
    const std::string &mode = a[index];
    if (mode.empty()) {
      std::cerr << "umask: invalid symbolic mode\n";
      return 2;
    }
    mode_t value = 0;
    if (!mode.empty() && std::isdigit(static_cast<unsigned char>(mode.front()))) {
      char *end = nullptr;
      errno = 0;
      const unsigned long parsed = std::strtoul(mode.c_str(), &end, 8);
      if (errno || end == mode.c_str() || *end || parsed > 0777) {
        std::cerr << "umask: expected an octal mask from 000 to 777\n";
        return 2;
      }
      value = static_cast<mode_t>(parsed);
    } else {
      const mode_t current_mask = umask(0);
      umask(current_mask);
      mode_t allowed = static_cast<mode_t>(~current_mask) & 0777;
      constexpr std::array<int, 3> shifts = {6, 3, 0};
      constexpr std::array<mode_t, 3> class_masks = {0700, 0070, 0007};
      size_t cursor = 0;
      while (cursor < mode.size()) {
        unsigned targets = 0;
        const size_t who_begin = cursor;
        while (cursor < mode.size() && std::string("ugoa").find(mode[cursor]) != std::string::npos) {
          if (mode[cursor] == 'u') targets |= 1;
          else if (mode[cursor] == 'g') targets |= 2;
          else if (mode[cursor] == 'o') targets |= 4;
          else targets |= 7;
          ++cursor;
        }
        if (cursor == who_begin) targets = 7;
        if (cursor >= mode.size() || std::string("+=-").find(mode[cursor]) == std::string::npos) {
          std::cerr << "umask: invalid symbolic mode: " << mode << '\n';
          return 2;
        }
        while (cursor < mode.size() && std::string("+=-").find(mode[cursor]) != std::string::npos) {
          const char operation = mode[cursor++];
          const size_t permissions_begin = cursor;
          const mode_t source_mode = allowed;
          std::array<mode_t, 3> additions{};
          while (cursor < mode.size() && mode[cursor] != ',' &&
                 std::string("+=-").find(mode[cursor]) == std::string::npos) {
            const char permission = mode[cursor++];
            int source_class = -1;
            if (permission == 'u') source_class = 0;
            else if (permission == 'g') source_class = 1;
            else if (permission == 'o') source_class = 2;
            if (source_class >= 0) {
              const mode_t copied = (source_mode >> shifts[source_class]) & 7;
              for (size_t target = 0; target < 3; ++target)
                if (targets & (1u << target)) additions[target] |= copied << shifts[target];
            } else if (permission == 'r' || permission == 'w' || permission == 'x' || permission == 'X') {
              const mode_t bit = permission == 'r' ? 4 : permission == 'w' ? 2 : 1;
              if (permission != 'X' || (source_mode & 0111)) {
                for (size_t target = 0; target < 3; ++target)
                  if (targets & (1u << target)) additions[target] |= bit << shifts[target];
              }
            } else {
              std::cerr << "umask: invalid symbolic mode: " << mode << '\n';
              return 2;
            }
          }
          if (cursor == permissions_begin && operation != '=') {
            std::cerr << "umask: invalid symbolic mode: " << mode << '\n';
            return 2;
          }
          for (size_t target = 0; target < 3; ++target) {
            if (!(targets & (1u << target))) continue;
            if (operation == '=')
              allowed = (allowed & ~class_masks[target]) | additions[target];
            else if (operation == '+') allowed |= additions[target];
            else allowed &= ~additions[target];
          }
          if (cursor < mode.size() && mode[cursor] != ',' &&
              std::string("+=-").find(mode[cursor]) == std::string::npos) {
            std::cerr << "umask: invalid symbolic mode: " << mode << '\n';
            return 2;
          }
        }
        if (cursor < mode.size()) {
          if (mode[cursor] != ',') {
            std::cerr << "umask: invalid symbolic mode: " << mode << '\n';
            return 2;
          }
          ++cursor;
          if (cursor == mode.size()) {
            std::cerr << "umask: invalid symbolic mode: " << mode << '\n';
            return 2;
          }
        }
      }
      value = static_cast<mode_t>(~allowed) & 0777;
    }
    umask(value);
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
    struct Resource {
      char option;
      int id;
      const char *description;
      unsigned long long unit;
      bool nice_priority = false;
    };
    std::vector<Resource> resources;
    resources.push_back({'c', RLIMIT_CORE, "core file size (blocks)", 512});
#ifdef RLIMIT_DATA
    resources.push_back({'d', RLIMIT_DATA, "data seg size (kbytes)", 1024});
#endif
    resources.push_back({'f', RLIMIT_FSIZE, "file size (blocks)", 512});
#ifdef RLIMIT_SIGPENDING
    resources.push_back({'i', RLIMIT_SIGPENDING, "pending signals", 1});
#endif
#ifdef RLIMIT_MEMLOCK
    resources.push_back({'l', RLIMIT_MEMLOCK, "max locked memory (kbytes)", 1024});
#endif
#ifdef RLIMIT_RSS
    resources.push_back({'m', RLIMIT_RSS, "max memory size (kbytes)", 1024});
#endif
#ifdef RLIMIT_NOFILE
    resources.push_back({'n', RLIMIT_NOFILE, "open files", 1});
#endif
#ifdef RLIMIT_NICE
    resources.push_back({'e', RLIMIT_NICE, "scheduling priority", 1, true});
#endif
#ifdef RLIMIT_MSGQUEUE
    resources.push_back({'q', RLIMIT_MSGQUEUE, "POSIX message queues (bytes)", 1});
#endif
#ifdef RLIMIT_RTPRIO
    resources.push_back({'r', RLIMIT_RTPRIO, "real-time priority", 1});
#endif
#ifdef RLIMIT_STACK
    resources.push_back({'s', RLIMIT_STACK, "stack size (kbytes)", 1024});
#endif
#ifdef RLIMIT_CPU
    resources.push_back({'t', RLIMIT_CPU, "cpu time (seconds)", 1});
#endif
#ifdef RLIMIT_NPROC
    resources.push_back({'u', RLIMIT_NPROC, "max user processes", 1});
#endif
#ifdef RLIMIT_AS
    resources.push_back({'v', RLIMIT_AS, "virtual memory (kbytes)", 1024});
#endif
#ifdef RLIMIT_LOCKS
    resources.push_back({'x', RLIMIT_LOCKS, "file locks", 1});
#endif
    resources.push_back({'p', -1, "pipe size (512 bytes)", 512});
    auto resource_for = [&](char option) -> const Resource * {
      auto found = std::find_if(resources.begin(), resources.end(),
          [&](const Resource &resource) { return resource.option == option; });
      return found == resources.end() ? nullptr : &*found;
    };
    char option = 'f';
    bool all = false, use_soft = true, use_hard = false;
    bool selected_soft = false, selected_hard = false;
    size_t index = 1;
    for (; index < a.size() && a[index].size() > 1 && a[index][0] == '-'; ++index) {
      if (a[index] == "--") { ++index; break; }
      for (size_t j = 1; j < a[index].size(); ++j) {
        const char flag = a[index][j];
        if (flag == 'S') selected_soft = true;
        else if (flag == 'H') selected_hard = true;
        else if (flag == 'a') all = true;
        else if (resource_for(flag)) option = flag;
        else {
          std::cerr << "ulimit: invalid option -- '" << flag << "'\n";
          return 2;
        }
      }
    }
    if (selected_soft || selected_hard) {
      use_soft = selected_soft;
      use_hard = selected_hard;
    }
    auto get_limit = [&](const Resource &resource, struct rlimit &limit) {
      if (getrlimit(resource.id, &limit) == 0) return true;
      perror("ulimit");
      return false;
    };
    auto print_value = [&](rlim_t value, const Resource &resource) {
      if (value == RLIM_INFINITY) std::cout << "unlimited";
      else if (resource.nice_priority)
        std::cout << (value <= 20 ? 20 - static_cast<unsigned long long>(value) : 0);
      else std::cout << static_cast<unsigned long long>(value) / resource.unit;
    };
    const long pipe_buf = fpathconf(STDIN_FILENO, _PC_PIPE_BUF);
    const auto print_pipe_size = [&]() {
      std::cout << (pipe_buf > 0 ? static_cast<unsigned long>(pipe_buf) / 512 : 8) << '\n';
    };
    if (all) {
      if (index != a.size()) return 2;
      for (const auto &resource : resources) {
        if (resource.option == 'p') {
          std::cout << resource.description << " (-p) ";
          print_pipe_size();
          continue;
        }
        struct rlimit limit{};
        if (!get_limit(resource, limit)) return 1;
        std::cout << resource.description << " (-" << resource.option << ") ";
        print_value(use_hard ? limit.rlim_max : limit.rlim_cur, resource);
        std::cout << '\n';
      }
      return 0;
    }
    const Resource *resource = resource_for(option);
    if (!resource) {
      std::cerr << "ulimit: unsupported resource limit -" << option << '\n';
      return 2;
    }
    struct rlimit limit{};
    if (resource->option == 'p') {
      if (index != a.size()) {
        std::cerr << "ulimit: this system does not support setting pipe size\n";
        return 1;
      }
      print_pipe_size();
      return 0;
    }
    if (!get_limit(*resource, limit)) return 1;
    if (index == a.size()) {
      if (use_soft) { print_value(limit.rlim_cur, *resource); std::cout << '\n'; }
      if (use_hard) { print_value(limit.rlim_max, *resource); std::cout << '\n'; }
      return 0;
    }
    if (index + 1 != a.size()) return 2;
    rlim_t parsed = RLIM_INFINITY;
    if (a[index] != "unlimited") {
      char *end = nullptr;
      errno = 0;
      const unsigned long long value = std::strtoull(a[index].c_str(), &end, 10);
      if (errno || end == a[index].c_str() || *end ||
          value > std::numeric_limits<rlim_t>::max() / resource->unit) {
        std::cerr << "ulimit: invalid limit: " << a[index] << '\n';
        return 2;
      }
      if (resource->nice_priority) {
        if (value > 19) {
          std::cerr << "ulimit: scheduling priority must be between 0 and 19\n";
          return 2;
        }
        parsed = static_cast<rlim_t>(20 - value);
      } else parsed = static_cast<rlim_t>(value * resource->unit);
    }
    if (use_soft) limit.rlim_cur = parsed;
    if (use_hard) limit.rlim_max = parsed;
    if (setrlimit(resource->id, &limit) != 0) { perror("ulimit"); return 1; }
    return 0;
  }
  return {};
}
