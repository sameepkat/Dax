#include "shell_state.hpp"
#include "builtins.hpp"
#include <chrono>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace {
bool startup_privileged_requested = false;
bool startup_inherited_environment_ignored = false;
}

bool prepare_shell_startup_privilege(bool requested) {
  startup_privileged_requested = requested;
  const bool mismatched_ids = geteuid() != getuid() || getegid() != getgid();
  startup_inherited_environment_ignored = requested || mismatched_ids;
  if (!requested && mismatched_ids) {
    if (setegid(getgid()) != 0 || seteuid(getuid()) != 0)
      return false;
  }
  return true;
}

ShellState &shell_state() {
  static ShellState state;
  static const bool initialized = [&]() {
    state.shell_pid = static_cast<int>(getpid());
    state.privileged = startup_privileged_requested;
    state.ignore_inherited_environment = startup_inherited_environment_ignored;
#ifdef NSIG
    for (int signal_number = 1; signal_number < NSIG; ++signal_number) {
      struct sigaction disposition {};
      if (sigaction(signal_number, nullptr, &disposition) == 0 &&
          disposition.sa_handler == SIG_IGN)
        state.initially_ignored_signals.insert(signal_number);
    }
#endif
    if (!std::getenv("PATH")) {
      const size_t path_length = confstr(_CS_PATH, nullptr, 0);
      std::string default_path(path_length ? path_length : 1, '\0');
      if (path_length) {
        confstr(_CS_PATH, default_path.data(), default_path.size());
        default_path.resize(default_path.find('\0'));
      } else {
        default_path = "/bin:/usr/bin";
      }
      setenv("PATH", default_path.c_str(), 1);
    }
    char initial_pwd[4096];
    if (getcwd(initial_pwd, sizeof(initial_pwd))) {
      state.arrays["DIRSTACK"] = {initial_pwd};
      state.array_set_indices["DIRSTACK"].insert(0);
    }
    state.vars["UID"] = std::to_string(getuid());
    state.vars["EUID"] = std::to_string(geteuid());
    state.vars["PPID"] = std::to_string(getppid());
    state.readonly_vars["UID"] = "1";
    state.readonly_vars["EUID"] = "1";
    state.readonly_vars["PPID"] = "1";
    struct utsname system_info {};
    const bool have_system_info = uname(&system_info) == 0;
    char host_name[256] = {};
    if (gethostname(host_name, sizeof(host_name) - 1) != 0) host_name[0] = '\0';
    auto host_value = [](const char *name, const std::string &fallback) {
      const char *inherited = std::getenv(name);
      return inherited ? std::string(inherited) : fallback;
    };
    const std::string host_type = have_system_info ? system_info.machine : "unknown";
    std::string os_type;
#if defined(__linux__)
#if defined(__GLIBC__)
    os_type = "linux-gnu";
#else
    os_type = "linux-musl";
#endif
#elif defined(__APPLE__)
    os_type = "darwin";
#elif defined(__FreeBSD__)
    os_type = "freebsd";
#elif defined(__OpenBSD__)
    os_type = "openbsd";
#elif defined(__NetBSD__)
    os_type = "netbsd";
#else
    os_type = have_system_info ? system_info.sysname : "unknown";
#endif
    std::string vendor = "unknown";
    if (host_type.rfind("x86_64", 0) == 0 || host_type.rfind("i386", 0) == 0 ||
        host_type.rfind("i486", 0) == 0 || host_type.rfind("i586", 0) == 0 ||
        host_type.rfind("i686", 0) == 0)
      vendor = "pc";
#if defined(__APPLE__)
    vendor = "apple";
#endif
    const std::string machine_type = host_type + "-" + vendor + "-" + os_type;
    state.vars["HOSTNAME"] = host_value("HOSTNAME", host_name);
    state.vars["HOSTTYPE"] = host_value("HOSTTYPE", host_type);
    state.vars["OSTYPE"] = host_value("OSTYPE", os_type);
    state.vars["MACHTYPE"] = host_value("MACHTYPE", machine_type);
    state.readonly_vars["SHELLOPTS"] = "1";
    state.readonly_vars["BASHOPTS"] = "1";
    state.vars["SHELLOPTS"] = "";
    state.vars["BASHOPTS"] = "";
    auto has_option = [](const char *list, const char *option) {
      if (!list) return false;
      const std::string values(list);
      const std::string wanted(option);
      size_t begin = 0;
      while (begin <= values.size()) {
        const size_t end = values.find(':', begin);
        if (values.compare(begin, end == std::string::npos ? std::string::npos
                                                             : end - begin,
                           wanted) == 0 &&
            (end == std::string::npos ? values.size() - begin : end - begin) ==
                wanted.size())
          return true;
        if (end == std::string::npos) break;
        begin = end + 1;
      }
      return false;
    };
    const char *inherited_shell_options = state.ignore_inherited_environment
                                              ? nullptr : std::getenv("SHELLOPTS");
    if (has_option(inherited_shell_options, "privileged")) state.privileged = true;
    if (has_option(inherited_shell_options, "allexport")) state.allexport = true;
    if (has_option(inherited_shell_options, "notify")) state.notify_jobs = true;
    if (has_option(inherited_shell_options, "noclobber")) state.noclobber = true;
    if (has_option(inherited_shell_options, "nolog")) state.nolog = true;
    if (has_option(inherited_shell_options, "errexit")) state.errexit = true;
    if (has_option(inherited_shell_options, "errtrace")) state.errtrace = true;
    if (has_option(inherited_shell_options, "functrace")) state.functrace = true;
    if (has_option(inherited_shell_options, "pipefail")) state.pipefail = true;
    if (has_option(inherited_shell_options, "noglob")) state.noglob = true;
    if (has_option(inherited_shell_options, "monitor")) state.monitor = true;
    if (has_option(inherited_shell_options, "noexec")) state.noexec = true;
    if (has_option(inherited_shell_options, "nounset")) state.nounset = true;
    if (has_option(inherited_shell_options, "verbose")) state.verbose = true;
    if (has_option(inherited_shell_options, "xtrace")) state.xtrace = true;
    if (has_option(inherited_shell_options, "hashall")) state.hashall = true;
    if (has_option(inherited_shell_options, "braceexpand")) state.braceexpand = true;
    if (has_option(inherited_shell_options, "histexpand")) state.histexpand = true;
    if (has_option(inherited_shell_options, "history")) state.history = true;
    if (has_option(inherited_shell_options, "posix")) state.posix_mode = true;
    if (has_option(inherited_shell_options, "vi")) {
      state.vi_mode = true;
      state.emacs_mode = false;
    } else if (has_option(inherited_shell_options, "emacs")) {
      state.emacs_mode = true;
    }
    if (state.privileged) state.ignore_inherited_environment = true;
    if (const char *ignoreeof = std::getenv("IGNOREEOF")) {
      state.vars["IGNOREEOF"] = ignoreeof;
      state.ignoreeof = true;
    }
    const char *inherited_bash_options = state.ignore_inherited_environment
                                             ? nullptr : std::getenv("BASHOPTS");
    if (has_option(inherited_bash_options, "dotglob")) state.dotglob = true;
    if (has_option(inherited_bash_options, "extglob")) state.extglob = true;
    if (has_option(inherited_bash_options, "failglob")) state.failglob = true;
    if (has_option(inherited_bash_options, "globstar")) state.globstar = true;
    if (has_option(inherited_bash_options, "globskipdots")) state.globskipdots = true;
    if (has_option(inherited_bash_options, "globasciiranges")) state.globasciiranges = true;
    if (has_option(inherited_bash_options, "huponexit")) state.huponexit = true;
    if (has_option(inherited_bash_options, "inherit_errexit")) state.inherit_errexit = true;
    if (has_option(inherited_bash_options, "lastpipe")) state.lastpipe = true;
    if (has_option(inherited_bash_options, "shift_verbose")) state.shift_verbose = true;
    if (has_option(inherited_bash_options, "nocaseglob")) state.nocaseglob = true;
    if (has_option(inherited_bash_options, "nocasematch")) state.nocasematch = true;
    if (has_option(inherited_bash_options, "patsub_replacement")) state.patsub_replacement = true;
    if (has_option(inherited_bash_options, "nullglob")) state.nullglob = true;
    if (has_option(inherited_bash_options, "sourcepath")) state.sourcepath = true;
    if (has_option(inherited_bash_options, "expand_aliases")) state.expand_aliases = true;
    if (has_option(inherited_bash_options, "varredir_close")) state.varredir_close = true;
    if (has_option(inherited_bash_options, "xpg_echo")) state.xpg_echo = true;
    if (has_option(inherited_bash_options, "checkwinsize")) state.checkwinsize = true;
    if (has_option(inherited_bash_options, "checkhash")) state.checkhash = true;
    if (has_option(inherited_bash_options, "checkjobs")) state.checkjobs = true;
    if (has_option(inherited_bash_options, "promptvars")) state.promptvars = true;
    if (has_option(inherited_bash_options, "cdable_vars")) state.cdable_vars = true;
    if (has_option(inherited_bash_options, "cdspell")) state.cdspell = true;
    if (has_option(inherited_bash_options, "autocd")) state.autocd = true;
    if (has_option(inherited_bash_options, "execfail")) state.execfail = true;
    if (has_option(inherited_bash_options, "localvar_inherit")) state.localvar_inherit = true;
    if (has_option(inherited_bash_options, "localvar_unset")) state.localvar_unset = true;
    if (has_option(inherited_bash_options, "interactive_comments")) state.interactive_comments = true;
    if (has_option(inherited_bash_options, "progcomp")) state.progcomp = true;
    if (has_option(inherited_bash_options, "no_empty_cmd_completion")) state.no_empty_cmd_completion = true;
    if (has_option(inherited_bash_options, "cmdhist")) state.cmdhist = true;
    if (has_option(inherited_bash_options, "lithist")) state.lithist = true;
    if (has_option(inherited_bash_options, "histappend")) state.histappend = true;
    int group_count = getgroups(0, nullptr);
    if (group_count > 0) {
      std::vector<gid_t> groups(static_cast<size_t>(group_count));
      group_count = getgroups(group_count, groups.data());
      if (group_count > 0) {
        auto &values = state.arrays["GROUPS"];
        auto &indices = state.array_set_indices["GROUPS"];
        for (int i = 0; i < group_count; ++i) {
          values.push_back(std::to_string(groups[static_cast<size_t>(i)]));
          indices.insert(static_cast<size_t>(i));
        }
      }
    }
    if (!state.arrays.count("GROUPS")) {
      state.arrays["GROUPS"] = {std::to_string(getgid())};
      state.array_set_indices["GROUPS"].insert(0);
    }
    long inherited_level = 0;
    if (const char *value = std::getenv("SHLVL")) {
      char *end = nullptr;
      errno = 0;
      const long parsed = std::strtol(value, &end, 10);
      if (!errno && end != value && *end == '\0') inherited_level = parsed;
    }
    if (inherited_level < 0) inherited_level = 0;
    long shell_level = 1;
    if (inherited_level >= 1000) {
      const long reported_level = inherited_level == LONG_MAX
                                      ? inherited_level : inherited_level + 1;
      std::fprintf(stderr, "dax: warning: shell level (%ld) too high, resetting to 1\n",
                   reported_level);
    } else shell_level = inherited_level + 1;
    state.vars["SHLVL"] = std::to_string(shell_level);
    setenv("SHLVL", state.vars["SHLVL"].c_str(), 1);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    state.random_seed = static_cast<uint32_t>(micros) ^
                        static_cast<uint32_t>(getpid()) ^
                        static_cast<uint32_t>(getppid());
    return true;
  }();
  (void)initialized;
  return state;
}

ShellOptionSnapshot capture_shell_options(const ShellState &state) {
  return {state.errexit, state.errtrace, state.functrace, state.nounset, state.allexport, state.pipefail,
          state.notify_jobs, state.noclobber, state.nolog, state.noglob, state.monitor,
          state.keyword_assignments,
          state.physical_directory,
          state.one_command,
          state.noexec, state.verbose, state.xtrace, state.hashall,
          state.braceexpand, state.histexpand, state.history, state.ignoreeof,
          state.privileged,
          state.extglob, state.nullglob,
          state.failglob, state.dotglob, state.globstar, state.globskipdots,
          state.globasciiranges, state.nocaseglob,
          state.nocasematch, state.sourcepath, state.expand_aliases,
          state.huponexit, state.lastpipe, state.inherit_errexit, state.shift_verbose,
          state.patsub_replacement, state.varredir_close, state.xpg_echo,
          state.checkwinsize, state.checkhash, state.checkjobs, state.promptvars,
          state.cdable_vars, state.cdspell, state.autocd, state.execfail,
          state.localvar_inherit, state.localvar_unset,
          state.interactive_comments, state.progcomp,
          state.no_empty_cmd_completion, state.cmdhist, state.lithist,
          state.histappend, state.posix_mode};
}

void restore_shell_options(ShellState &state, const ShellOptionSnapshot &snapshot) {
  const bool ignoreeof_changed = state.ignoreeof != snapshot.ignoreeof;
  state.errexit = snapshot.errexit;
  state.errtrace = snapshot.errtrace;
  state.functrace = snapshot.functrace;
  state.nounset = snapshot.nounset;
  state.allexport = snapshot.allexport;
  state.pipefail = snapshot.pipefail;
  state.notify_jobs = snapshot.notify_jobs;
  state.noclobber = snapshot.noclobber;
  state.nolog = snapshot.nolog;
  state.noglob = snapshot.noglob;
  state.monitor = snapshot.monitor;
  state.keyword_assignments = snapshot.keyword_assignments;
  state.physical_directory = snapshot.physical_directory;
  state.one_command = snapshot.one_command;
  state.noexec = snapshot.noexec;
  state.verbose = snapshot.verbose;
  state.xtrace = snapshot.xtrace;
  state.hashall = snapshot.hashall;
  state.braceexpand = snapshot.braceexpand;
  state.histexpand = snapshot.histexpand;
  state.history = snapshot.history;
  state.ignoreeof = snapshot.ignoreeof;
  state.privileged = snapshot.privileged;
  if (ignoreeof_changed) {
    if (state.ignoreeof) {
      state.vars["IGNOREEOF"] = "10";
      state.unset_vars.erase("IGNOREEOF");
      if (state.allexport) setenv("IGNOREEOF", "10", 1);
    } else {
      state.vars.erase("IGNOREEOF");
      state.unset_vars.insert("IGNOREEOF");
      unsetenv("IGNOREEOF");
    }
  }
  state.extglob = snapshot.extglob;
  state.nullglob = snapshot.nullglob;
  state.failglob = snapshot.failglob;
  state.dotglob = snapshot.dotglob;
  state.globstar = snapshot.globstar;
  state.globskipdots = snapshot.globskipdots;
  state.globasciiranges = snapshot.globasciiranges;
  state.nocaseglob = snapshot.nocaseglob;
  state.nocasematch = snapshot.nocasematch;
  state.sourcepath = snapshot.sourcepath;
  state.expand_aliases = snapshot.expand_aliases;
  state.huponexit = snapshot.huponexit;
  state.lastpipe = snapshot.lastpipe;
  state.inherit_errexit = snapshot.inherit_errexit;
  state.shift_verbose = snapshot.shift_verbose;
  state.patsub_replacement = snapshot.patsub_replacement;
  state.varredir_close = snapshot.varredir_close;
  state.xpg_echo = snapshot.xpg_echo;
  state.checkwinsize = snapshot.checkwinsize;
  state.checkhash = snapshot.checkhash;
  state.checkjobs = snapshot.checkjobs;
  state.promptvars = snapshot.promptvars;
  state.cdable_vars = snapshot.cdable_vars;
  state.cdspell = snapshot.cdspell;
  state.autocd = snapshot.autocd;
  state.execfail = snapshot.execfail;
  state.localvar_inherit = snapshot.localvar_inherit;
  state.localvar_unset = snapshot.localvar_unset;
  state.interactive_comments = snapshot.interactive_comments;
  state.progcomp = snapshot.progcomp;
  state.no_empty_cmd_completion = snapshot.no_empty_cmd_completion;
  state.cmdhist = snapshot.cmdhist;
  state.lithist = snapshot.lithist;
  state.histappend = snapshot.histappend;
  state.posix_mode = snapshot.posix_mode;
  refresh_option_parameter_values();
}

void refresh_shell_terminal_dimensions(int descriptor, bool force) {
  auto &state = shell_state();
  if ((!force && !state.checkwinsize) || !isatty(descriptor)) return;
  struct winsize dimensions {};
  if (ioctl(descriptor, TIOCGWINSZ, &dimensions) != 0 ||
      dimensions.ws_row == 0 || dimensions.ws_col == 0)
    return;
  const auto update = [&](const char *name, unsigned short value) {
    const std::string text = std::to_string(value);
    const auto current = state.vars.find(name);
    if (current != state.vars.end() && !state.unset_vars.count(name) &&
        current->second == text)
      return;
    set_shell_variable(name, text);
  };
  update("LINES", dimensions.ws_row);
  update("COLUMNS", dimensions.ws_col);
}
