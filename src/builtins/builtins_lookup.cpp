#include "builtins_internal.hpp"
#include "shell.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fnmatch.h>
#include <fstream>
#include <grp.h>
#include <glob.h>
#include <iomanip>
#include <iostream>
#include <netdb.h>
#include <pwd.h>
#include <set>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

std::optional<int> run_lookup_builtin(const std::vector<std::string> &a,
                                      ShellState &state) {
  if (a.empty()) return 0;
  const auto &c = a[0];
  static const std::vector<std::string> shell_keywords = {
      "!", "[[", "]]", "case", "coproc", "do", "done", "elif", "else", "esac",
      "fi", "for", "function", "if", "in", "select", "then", "time", "until",
      "while", "{", "}", "((", "))"};
  auto is_shell_keyword = [&](const std::string &name) {
    return std::find(shell_keywords.begin(), shell_keywords.end(), name) != shell_keywords.end();
  };
  auto shell_quote = [](const std::string &value) {
    std::string quoted = "'";
    for (char ch : value) {
      if (ch == '\'') quoted += "'\\''";
      else quoted += ch;
    }
    return quoted + "'";
  };
  auto function_definition = [&](const std::string &name) {
    const auto function = state.functions.find(name);
    if (function == state.functions.end()) return std::string();
    std::string definition = name + " () \n{\n" + function->second;
    if (definition.empty() || definition.back() != '\n') definition += '\n';
    definition += "}";
    return definition;
  };
  if (c == "complete") {
    bool print = false, remove = false, default_mode = false;
    bool empty_mode = false, initial_mode = false;
    std::vector<std::string> generator_options;
    auto special_key = [](char mode) {
      const char *name = mode == 'D' ? "default" : mode == 'E' ? "empty" : "initial";
      return std::string(1, '\x01') + name;
    };
    auto selected_mode = [&]() {
      return default_mode ? 'D' : empty_mode ? 'E' : initial_mode ? 'I' : '\0';
    };
    auto display_spec = [&](const std::string &key) {
      const auto spec = state.completion_specs.find(key);
      if (spec == state.completion_specs.end()) return;
      std::cout << "complete";
      if (key == special_key('D')) std::cout << " -D";
      else if (key == special_key('E')) std::cout << " -E";
      else if (key == special_key('I')) std::cout << " -I";
      for (const auto &option : spec->second) std::cout << ' ' << option;
      if (key != special_key('D') && key != special_key('E') && key != special_key('I'))
        std::cout << ' ' << key;
      std::cout << '\n';
    };
    size_t i = 1;
    auto option_argument = [&](size_t &index, size_t &flag, std::string &value,
                               const std::string &option) {
      if (flag + 1 < a[index].size()) {
        value = a[index].substr(flag + 1);
        flag = a[index].size();
        return true;
      }
      if (index + 1 >= a.size()) {
        std::cerr << "complete: " << option << ": option requires an argument\n";
        return false;
      }
      value = a[++index];
      flag = a[index].size();
      return true;
    };
    for (; i < a.size() && a[i].size() > 1 && a[i][0] == '-'; ++i) {
      if (a[i] == "--") { ++i; break; }
      for (size_t flag = 1; flag < a[i].size(); ++flag) {
        const char option = a[i][flag];
        if (option == 'p') { print = true; continue; }
        if (option == 'r') { remove = true; continue; }
        if (option == 'D') { default_mode = true; continue; }
        if (option == 'E') { empty_mode = true; continue; }
        if (option == 'I') { initial_mode = true; continue; }
        if (std::string("abcdefgjksuv").find(option) != std::string::npos) {
          generator_options.push_back(std::string("-") + option);
          continue;
        }
        if (std::string("ACFGPSVWXo").find(option) != std::string::npos) {
          std::string value;
          if (!option_argument(i, flag, value, std::string("-") + option)) return 2;
          generator_options.push_back(std::string("-") + option);
          generator_options.push_back(std::move(value));
          continue;
        }
        std::cerr << "complete: -" << option << ": invalid option\n";
        return 2;
      }
    }
    if (print) {
      const char mode = selected_mode();
      if (mode) {
        if (i != a.size()) {
          std::cerr << "complete: mode selectors cannot be combined with command names\n";
          return 2;
        }
        display_spec(special_key(mode));
        return 0;
      }
      std::vector<std::string> names;
      if (i == a.size()) {
        for (const auto &[name, spec] : state.completion_specs) { (void)spec; names.push_back(name); }
      } else {
        names.assign(a.begin() + i, a.end());
      }
      std::sort(names.begin(), names.end());
      for (const auto &name : names) display_spec(name);
      return 0;
    }
    if (remove) {
      const char mode = selected_mode();
      if (mode) {
        if (i != a.size()) {
          std::cerr << "complete: mode selectors cannot be combined with command names\n";
          return 2;
        }
        state.completion_specs.erase(special_key(mode));
        return 0;
      }
      if (i == a.size()) state.completion_specs.clear();
      else for (; i < a.size(); ++i) state.completion_specs.erase(a[i]);
      return 0;
    }
    const char mode = selected_mode();
    if (i == a.size() && mode) {
      state.completion_specs[special_key(mode)] = generator_options;
      return 0;
    }
    if (i == a.size() && generator_options.empty()) {
      std::vector<std::string> names;
      for (const auto &[name, spec] : state.completion_specs) { (void)spec; names.push_back(name); }
      std::sort(names.begin(), names.end());
      for (const auto &name : names) display_spec(name);
      return 0;
    }
    if (i == a.size()) {
      std::cerr << "complete: command name required\n";
      return 2;
    }
    if (mode) {
      std::cerr << "complete: -D, -E, and -I may not be combined with command names\n";
      return 2;
    }
    for (; i < a.size(); ++i) state.completion_specs[a[i]] = generator_options;
    return 0;
  }
  if (c == "compopt") {
    std::vector<std::pair<bool, std::string>> changes;
    bool default_mode = false, empty_mode = false, initial_mode = false;
    size_t i = 1;
    auto option_argument = [&](size_t &index, size_t &flag, std::string &value,
                               const std::string &option) {
      if (flag + 1 < a[index].size()) {
        value = a[index].substr(flag + 1);
        flag = a[index].size();
        return true;
      }
      if (index + 1 >= a.size()) {
        std::cerr << "compopt: " << option << ": option requires an argument\n";
        return false;
      }
      value = a[++index];
      flag = a[index].size();
      return true;
    };
    static const std::set<std::string> valid_options = {
        "bashdefault", "default", "dirnames", "filenames", "fullquote", "noquote",
        "nosort", "nospace", "plusdirs"};
    for (; i < a.size() && a[i].size() > 1 && (a[i][0] == '-' || a[i][0] == '+'); ++i) {
      if (a[i] == "--") { ++i; break; }
      const bool enabled = a[i][0] == '-';
      for (size_t flag = 1; flag < a[i].size(); ++flag) {
        const char option = a[i][flag];
        if (option == 'D' && enabled) { default_mode = true; continue; }
        if (option == 'E' && enabled) { empty_mode = true; continue; }
        if (option == 'I' && enabled) { initial_mode = true; continue; }
        if (option == 'o') {
          std::string value;
          if (!option_argument(i, flag, value, enabled ? "-o" : "+o")) return 2;
          if (!valid_options.count(value)) {
            std::cerr << "compopt: " << value << ": invalid completion option\n";
            return 2;
          }
          changes.emplace_back(enabled, std::move(value));
          continue;
        }
        std::cerr << "compopt: " << a[i] << ": invalid option\n";
        return 2;
      }
    }
    auto special_key = [](char mode) {
      const char *name = mode == 'D' ? "default" : mode == 'E' ? "empty" : "initial";
      return std::string(1, '\x01') + name;
    };
    const char mode = default_mode ? 'D' : empty_mode ? 'E' : initial_mode ? 'I' : '\0';
    std::vector<std::string> names;
    if (i < a.size()) names.assign(a.begin() + i, a.end());
    if (mode && !names.empty()) {
      std::cerr << "compopt: mode selectors cannot be combined with command names\n";
      return 2;
    }
    auto option_vector = [&](const std::string &key) -> std::vector<std::string> * {
      if (state.completion_active && names.empty() && !mode)
        return &state.active_completion_options;
      const auto spec = state.completion_specs.find(key);
      return spec == state.completion_specs.end() ? nullptr : &spec->second;
    };
    if (names.empty() && !mode && !state.completion_active) {
      std::cerr << "compopt: not currently executing a completion function\n";
      return 1;
    }
    std::vector<std::string> keys;
    if (mode) keys.push_back(special_key(mode));
    else if (!names.empty()) keys = names;
    else keys.push_back(state.active_completion_key);
    int status = 0;
    for (const auto &key : keys) {
      auto *options = option_vector(key);
      if (!options) {
        std::cerr << "compopt: " << key << ": no completion specification\n";
        status = 1;
        continue;
      }
      if (changes.empty()) {
        for (size_t n = 0; n + 1 < options->size(); ++n) {
          if ((*options)[n] == "-o")
            std::cout << "compopt -o " << (*options)[++n] << '\n';
        }
        continue;
      }
      for (const auto &[enabled, value] : changes) {
        auto found = std::find(options->begin(), options->end(), "-o");
        bool was_present = false;
        while (found != options->end()) {
          if (found + 1 != options->end() && *(found + 1) == value) {
            options->erase(found, found + 2);
            was_present = true;
            break;
          }
          found = std::find(found + 1, options->end(), "-o");
        }
        if (enabled && !was_present) {
          options->push_back("-o");
          options->push_back(value);
        }
      }
    }
    return status;
  }
  if (c == "compgen") {
    std::set<std::string> actions, completion_options;
    std::string prefix, suffix, wordlist, array_name, glob_pattern, filter_pattern;
    std::string completion_function;
    std::string completion_command;
    bool have_wordlist = false, have_filter_pattern = false;
    bool have_completion_function = false, have_completion_command = false;
    bool have_glob_pattern = false, have_array_name = false, sort_results = true;
    auto option_argument = [&](size_t &i, size_t &flag, std::string &value,
                               const std::string &option) {
      if (flag + 1 < a[i].size()) {
        value = a[i].substr(flag + 1);
        flag = a[i].size();
        return true;
      }
      if (i + 1 >= a.size()) {
        std::cerr << "compgen: " << option << ": option requires an argument\n";
        return false;
      }
      value = a[++i];
      flag = a[i].size();
      return true;
    };
    size_t i = 1;
    for (; i < a.size() && a[i].size() > 1 && a[i][0] == '-'; ++i) {
      if (a[i] == "--") { ++i; break; }
      for (size_t flag = 1; flag < a[i].size(); ++flag) {
        switch (a[i][flag]) {
        case 'a': actions.insert("alias"); break;
        case 'b': actions.insert("builtin"); break;
        case 'c': actions.insert("command"); break;
        case 'd': actions.insert("directory"); break;
        case 'e': actions.insert("export"); break;
        case 'f': actions.insert("file"); break;
        case 'g': actions.insert("group"); break;
        case 'j': actions.insert("job"); break;
        case 'k': actions.insert("keyword"); break;
        case 's': actions.insert("service"); break;
        case 'u': actions.insert("user"); break;
        case 'v': actions.insert("variable"); break;
        case 'A': {
          std::string value;
          if (!option_argument(i, flag, value, "-A")) return 2;
          actions.insert(value);
          break;
        }
        case 'P':
          if (!option_argument(i, flag, prefix, "-P")) return 2;
          break;
        case 'S':
          if (!option_argument(i, flag, suffix, "-S")) return 2;
          break;
        case 'W':
          if (!option_argument(i, flag, wordlist, "-W")) return 2;
          have_wordlist = true;
          break;
        case 'V':
          if (!option_argument(i, flag, array_name, "-V")) return 2;
          have_array_name = true;
          break;
        case 'F':
          if (!option_argument(i, flag, completion_function, "-F")) return 2;
          have_completion_function = true;
          break;
        case 'C':
          if (!option_argument(i, flag, completion_command, "-C")) return 2;
          have_completion_command = true;
          break;
        case 'G':
          if (!option_argument(i, flag, glob_pattern, "-G")) return 2;
          have_glob_pattern = true;
          break;
        case 'X':
          if (!option_argument(i, flag, filter_pattern, "-X")) return 2;
          have_filter_pattern = true;
          break;
        case 'o': {
          std::string value;
          if (!option_argument(i, flag, value, "-o")) return 2;
          static const std::set<std::string> valid_options = {
              "bashdefault", "default", "dirnames", "filenames", "fullquote", "noquote",
              "nosort", "nospace", "plusdirs"};
          if (!valid_options.count(value)) {
            std::cerr << "compgen: " << value << ": invalid completion option\n";
            return 2;
          }
          completion_options.insert(value);
          if (value == "nosort") sort_results = false;
          break;
        }
        default:
          std::cerr << "compgen: -" << a[i][flag] << ": invalid option\n";
          return 2;
        }
      }
    }
    if (a.size() - i > 1) {
      std::cerr << "compgen: too many arguments\n";
      return 2;
    }
    const std::string word = i < a.size() ? a[i] : std::string();
    std::vector<std::string> candidates;
    auto add_if_matching = [&](const std::string &candidate) {
      if (candidate.rfind(word, 0) == 0) candidates.push_back(candidate);
    };
    static const std::vector<std::string> keywords = {
        "!", "[[", "]]", "case", "coproc", "do", "done", "elif", "else",
        "esac", "fi", "for", "function", "if", "in", "select", "then",
        "time", "until", "while", "{", "}", "(", ")"};
    auto add_files = [&](bool directories_only) {
      const std::filesystem::path word_path(word);
      const std::string directory = word_path.has_parent_path()
          ? word_path.parent_path().string() : std::string();
      const std::string file_prefix = word_path.filename().string();
      const auto found = shell_completion_candidates(file_prefix, false, false, directory);
      const std::string path_prefix = directory.empty() ? std::string() : directory + "/";
      for (const auto &entry : found) {
        const bool is_directory = !entry.empty() && entry.back() == '/';
        if (directories_only && !is_directory) continue;
        std::string value = path_prefix + entry;
        if (!directories_only && is_directory && !value.empty()) value.pop_back();
        add_if_matching(value);
      }
    };
    for (const auto &action : actions) {
      if (action == "alias") {
        for (const auto &[name, value] : state.aliases) { (void)value; add_if_matching(name); }
      } else if (action == "builtin" || action == "helptopic") {
        for (const auto &name : shell_builtin_names()) add_if_matching(name);
      } else if (action == "disabled") {
        for (const auto &name : state.disabled_builtins) add_if_matching(name);
      } else if (action == "command") {
        for (const auto &name : shell_completion_candidates(word, true, false))
          candidates.push_back(name);
      } else if (action == "directory") {
        add_files(true);
      } else if (action == "export" || action == "exported") {
        extern char **environ;
        for (char **entry = environ; entry && *entry; ++entry) {
          const char *equals = std::strchr(*entry, '=');
          if (equals) add_if_matching(std::string(*entry, static_cast<size_t>(equals - *entry)));
        }
      } else if (action == "file") {
        add_files(false);
      } else if (action == "function") {
        for (const auto &[name, body] : state.functions) { (void)body; add_if_matching(name); }
      } else if (action == "group") {
        setgrent();
        while (const struct group *entry = getgrent()) add_if_matching(entry->gr_name);
        endgrent();
      } else if (action == "job" || action == "running" || action == "stopped") {
        for (const auto &job : state.jobs) {
          if (job.completed || (action == "running" && job.stopped) ||
              (action == "stopped" && !job.stopped)) continue;
          add_if_matching("%" + std::to_string(job.id));
        }
      } else if (action == "keyword") {
        for (const auto &name : keywords) add_if_matching(name);
      } else if (action == "hostname") {
        std::ifstream hosts("/etc/hosts");
        std::string line;
        while (std::getline(hosts, line)) {
          const size_t comment = line.find('#');
          if (comment != std::string::npos) line.resize(comment);
          std::istringstream fields(line);
          std::string hostname;
          while (fields >> hostname) add_if_matching(hostname);
        }
      } else if (action == "setopt") {
        static const std::vector<std::string> options = {
            "allexport", "braceexpand", "emacs", "errexit", "errtrace", "hashall",
            "histexpand", "history", "ignoreeof", "interactive-comments", "monitor",
            "noclobber", "noexec", "noglob", "nolog", "notify", "nounset", "onecmd",
            "physical", "pipefail", "posix", "privileged", "verbose", "vi", "xtrace"};
        for (const auto &name : options) add_if_matching(name);
      } else if (action == "shopt") {
        static const std::vector<std::string> options = {
            "autocd", "assoc_expand_once", "cdable_vars", "cdspell", "checkhash",
            "checkjobs", "checkwinsize", "cmdhist", "compat31", "compat32", "compat40",
            "compat41", "compat42", "compat43", "compat44", "compat50", "compat51",
            "compat52", "complete_fullquote", "direxpand", "dirspell", "dotglob",
            "execfail", "expand_aliases", "extdebug", "extglob", "errtrace", "failglob", "globasciiranges",
            "globskipdots", "globstar", "gnu_errfmt", "histappend", "histreedit", "histverify", "hostcomplete",
            "huponexit", "inherit_errexit", "interactive_comments", "lastpipe", "lithist",
            "localvar_inherit", "localvar_unset", "login_shell", "mailwarn", "no_empty_cmd_completion",
            "nocaseglob", "nocasematch", "nullglob", "patsub_replacement", "progcomp", "promptvars", "restricted_shell",
            "shift_verbose", "sourcepath", "varredir_close", "xpg_echo",
            "checkwinsize"};
        for (const auto &name : options) add_if_matching(name);
      } else if (action == "user") {
        setpwent();
        while (const struct passwd *entry = getpwent()) add_if_matching(entry->pw_name);
        endpwent();
      } else if (action == "service") {
        setservent(0);
        while (const struct servent *entry = getservent()) {
          if (entry->s_name) add_if_matching(entry->s_name);
          for (char **alias = entry->s_aliases; alias && *alias; ++alias)
            add_if_matching(*alias);
        }
        endservent();
      } else if (action == "signal") {
        static const std::vector<std::string> signals = {
            "EXIT", "SIGHUP", "SIGINT", "SIGQUIT", "SIGILL", "SIGTRAP", "SIGABRT",
            "SIGBUS", "SIGFPE", "SIGKILL", "SIGUSR1", "SIGSEGV", "SIGUSR2", "SIGPIPE",
            "SIGALRM", "SIGTERM", "SIGCHLD", "SIGCONT", "SIGSTOP", "SIGTSTP", "SIGTTIN",
            "SIGTTOU", "SIGURG", "SIGXCPU", "SIGXFSZ", "SIGVTALRM", "SIGPROF", "SIGWINCH",
            "SIGIO", "SIGSYS"};
        for (const auto &name : signals) add_if_matching(name);
      } else if (action == "variable") {
        for (const auto &name : shell_completion_candidates(word, false, true))
          candidates.push_back(name);
      } else if (action != "hostname" && action != "service" && action != "signal") {
        std::cerr << "compgen: " << action << ": invalid action name\n";
        return 2;
      }
    }
    if (have_wordlist) {
      // Treat -W's value as shell word-list syntax: quote removal happens in
      // the tokenizer, then each word receives the same expansions as other
      // shell words before it is compared with the completion prefix. Protect
      // whitespace that is not in IFS so the tokenizer doesn't split it.
      const auto ifs_variable = state.vars.find("IFS");
      const char *environment_ifs = std::getenv("IFS");
      const std::string ifs = ifs_variable != state.vars.end() ? ifs_variable->second
                              : environment_ifs ? environment_ifs : " \t\n";
      std::string normalized_wordlist;
      char quote = 0;
      size_t nested_parens = 0, nested_braces = 0;
      for (size_t offset = 0; offset < wordlist.size(); ++offset) {
        const char ch = wordlist[offset];
        if (ch == '\\' && quote != '\'') {
          normalized_wordlist += ch;
          if (offset + 1 < wordlist.size()) normalized_wordlist += wordlist[++offset];
          continue;
        }
        if (quote) {
          normalized_wordlist += ch;
          if (ch == quote) quote = 0;
          continue;
        }
        if (ch == '\'' || ch == '"') {
          quote = ch;
          normalized_wordlist += ch;
          continue;
        }
        if (ch == '$' && offset + 1 < wordlist.size() && wordlist[offset + 1] == '(') {
          ++nested_parens;
          normalized_wordlist += ch;
          normalized_wordlist += wordlist[++offset];
          continue;
        }
        if (ch == '$' && offset + 1 < wordlist.size() && wordlist[offset + 1] == '{') {
          ++nested_braces;
          normalized_wordlist += ch;
          normalized_wordlist += wordlist[++offset];
          continue;
        }
        if (nested_parens && ch == '(') ++nested_parens;
        else if (nested_parens && ch == ')') --nested_parens;
        if (nested_braces && ch == '{') ++nested_braces;
        else if (nested_braces && ch == '}') --nested_braces;

        if (nested_parens == 0 && nested_braces == 0 &&
            ifs.find(ch) != std::string::npos) {
          normalized_wordlist += ' ';
        } else if (nested_parens == 0 && nested_braces == 0 &&
                   std::isspace(static_cast<unsigned char>(ch))) {
          // A shell word may contain a literal space when IFS excludes it.
          normalized_wordlist += '\\';
          normalized_wordlist += ch;
        } else {
          normalized_wordlist += ch;
        }
      }
      for (const auto &token : tokenize(normalized_wordlist)) {
        if (token.empty()) continue;
        std::vector<std::string> entries{static_cast<const std::string &>(token)};
        const size_t process_substitution_start = process_substitution_mark();
        begin_word_expansion();
        expand_words(entries);
        finish_process_substitutions(process_substitution_start);
        if (!word_expansion_succeeded()) continue;
        for (const auto &entry : entries) add_if_matching(entry);
      }
    }
    if (have_glob_pattern) {
      glob_t matches{};
      const int flags =
#ifdef GLOB_TILDE
          GLOB_TILDE;
#else
          0;
#endif
      if (glob(glob_pattern.c_str(), flags, nullptr, &matches) == 0) {
        for (size_t match = 0; match < matches.gl_pathc; ++match)
          add_if_matching(matches.gl_pathv[match]);
      }
      globfree(&matches);
    }
    auto set_completion_context = [&]() {
      state.vars.erase("COMP_WORDS");
      state.vars["COMP_CWORD"] = "0";
      state.vars["COMP_LINE"] = word;
      state.vars["COMP_POINT"] = std::to_string(word.size());
      state.unset_vars.erase("COMP_WORDS");
      state.unset_vars.erase("COMP_CWORD");
      state.unset_vars.erase("COMP_LINE");
      state.unset_vars.erase("COMP_POINT");
      state.arrays.erase("COMP_WORDS");
      state.array_set_indices.erase("COMP_WORDS");
      state.associative_arrays.erase("COMP_WORDS");
      state.arrays["COMP_WORDS"] = {word};
      shell_array_mark_all_set("COMP_WORDS");
      state.associative_arrays.erase("COMP_WORDS");
      state.vars.erase("COMPREPLY");
      state.unset_vars.erase("COMPREPLY");
      state.arrays["COMPREPLY"].clear();
      state.array_set_indices["COMPREPLY"].clear();
      state.associative_arrays.erase("COMPREPLY");
    };
    if (have_completion_function) {
      if (!state.functions.count(completion_function)) {
        std::cerr << "compgen: " << completion_function << ": not a shell function\n";
        return 1;
      }
      set_completion_context();
      (void)handle_builtin({completion_function, word, "", word, "0"});
      const auto replies = state.arrays.find("COMPREPLY");
      if (replies != state.arrays.end()) {
        const auto set_indices = state.array_set_indices.find("COMPREPLY");
        if (set_indices == state.array_set_indices.end()) {
          for (const auto &reply : replies->second) candidates.push_back(reply);
        } else {
          std::vector<size_t> indices(set_indices->second.begin(), set_indices->second.end());
          std::sort(indices.begin(), indices.end());
          for (const size_t index : indices)
            if (index < replies->second.size()) candidates.push_back(replies->second[index]);
        }
      }
    }
    if (have_completion_command) {
      set_completion_context();
      FILE *capture = std::tmpfile();
      if (!capture) {
        std::cerr << "compgen: could not create command output capture\n";
        return 1;
      }
      std::cout.flush();
      const pid_t child = fork();
      if (child < 0) {
        std::fclose(capture);
        std::cerr << "compgen: could not start completion command\n";
        return 1;
      }
      if (child == 0) {
        state.err_trap_suppressed = state.err_trap_suppressed || !state.errtrace;
        if (dup2(fileno(capture), STDOUT_FILENO) < 0) _exit(125);
        execute_line(completion_command);
        std::cout.flush();
        std::fflush(stdout);
        _exit(0);
      }
      int child_status = 0;
      while (waitpid(child, &child_status, 0) < 0 && errno == EINTR) {}
      if (std::fseek(capture, 0, SEEK_SET) == 0) {
        char *line = nullptr;
        size_t capacity = 0;
        ssize_t length = 0;
        while ((length = ::getline(&line, &capacity, capture)) >= 0) {
          std::string candidate(line, static_cast<size_t>(length));
          if (!candidate.empty() && candidate.back() == '\n') candidate.pop_back();
          if (!candidate.empty() && candidate.back() == '\r') candidate.pop_back();
          candidates.push_back(std::move(candidate));
        }
        std::free(line);
      }
      std::fclose(capture);
    }
    if (completion_options.count("plusdirs") ||
        (completion_options.count("dirnames") && candidates.empty()))
      add_files(true);
    if (have_filter_pattern) {
      const bool invert = filter_pattern.front() == '!';
      const std::string pattern = invert ? filter_pattern.substr(1) : filter_pattern;
      candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
          [&](const std::string &candidate) {
            const bool matched = fnmatch(pattern.c_str(), candidate.c_str(), 0) == 0;
            return invert ? !matched : matched;
          }), candidates.end());
    }
    std::set<std::string> seen;
    std::vector<std::string> unique_candidates;
    unique_candidates.reserve(candidates.size());
    for (auto &candidate : candidates)
      if (seen.insert(candidate).second) unique_candidates.push_back(std::move(candidate));
    candidates = std::move(unique_candidates);
    if (sort_results) std::sort(candidates.begin(), candidates.end());
    for (auto &candidate : candidates) candidate = prefix + candidate + suffix;
    if (have_array_name) {
      if (!shell_valid_name(array_name) || shell_variable_readonly(array_name)) {
        std::cerr << "compgen: " << array_name << ": invalid or readonly array\n";
        return 1;
      }
      state.vars.erase(array_name);
      state.unset_vars.erase(array_name);
      state.associative_arrays.erase(array_name);
      state.arrays[array_name] = candidates;
      state.integer_vars.erase(array_name);
      shell_array_mark_all_set(array_name);
      unsetenv(array_name.c_str());
    } else {
      for (const auto &candidate : candidates) std::cout << candidate << '\n';
    }
    return 0;
  }
  if (c == "command") {
    bool default_path = false, lookup = false, verbose = false;
    size_t i = 1;
    for (; i < a.size() && a[i].size() > 1 && a[i][0] == '-'; ++i) {
      if (a[i] == "--") { ++i; break; }
      char invalid_option = '\0';
      for (size_t j = 1; j < a[i].size(); ++j) {
        if (a[i][j] == 'p') default_path = true;
        else if (a[i][j] == 'v') { lookup = true; verbose = false; }
        else if (a[i][j] == 'V') { lookup = true; verbose = true; }
        else { invalid_option = a[i][j]; break; }
      }
      if (invalid_option != '\0') {
        std::cerr << "command: -" << invalid_option << ": invalid option\n";
        return 2;
      }
    }
    auto with_default_path = [&](auto &&operation) {
      const char *old_path = std::getenv("PATH");
      const std::string saved_path = old_path ? old_path : "";
      const bool had_path = old_path != nullptr;
      const bool saved_hashall = state.hashall;
      const auto shell_path = state.vars.find("PATH");
      const bool had_shell_path = shell_path != state.vars.end();
      const std::string saved_shell_path = had_shell_path ? shell_path->second : "";
      const bool path_was_unset = state.unset_vars.count("PATH") != 0;
      size_t length = confstr(_CS_PATH, nullptr, 0);
      std::string path(length ? length : 1, '\0');
      if (length) {
        confstr(_CS_PATH, path.data(), path.size());
        path.resize(path.find('\0'));
      } else path = "/bin:/usr/bin";
      setenv("PATH", path.c_str(), 1);
      state.vars["PATH"] = path;
      state.unset_vars.erase("PATH");
      state.hashall = false;
      const int result = operation();
      state.hashall = saved_hashall;
      if (had_shell_path) state.vars["PATH"] = saved_shell_path;
      else state.vars.erase("PATH");
      if (path_was_unset) state.unset_vars.insert("PATH");
      else state.unset_vars.erase("PATH");
      if (had_path) setenv("PATH", saved_path.c_str(), 1);
      else unsetenv("PATH");
      return result;
    };
    if (lookup) {
      auto list_commands = [&]() {
        bool any_found = false;
        for (size_t n = i; n < a.size(); ++n) {
          bool found = false;
          const auto alias = state.aliases.find(a[n]);
          if (alias != state.aliases.end()) {
            if (verbose)
              std::cout << a[n] << " is aliased to `" << alias->second << "'\n";
            else
              std::cout << "alias " << a[n] << '=' << shell_quote(alias->second) << '\n';
            found = true;
          }
          else if (is_shell_keyword(a[n])) {
            if (verbose) std::cout << a[n] << " is a shell keyword\n";
            else std::cout << a[n] << '\n';
            found = true;
          }
          else if (!verbose && (is_builtin(a[n]) || state.functions.count(a[n]))) {
            std::cout << a[n] << '\n';
            found = true;
          } else if (verbose && shell_function_precedes_builtin(a[n])) {
            std::cout << a[n] << " is a function\n"
                      << function_definition(a[n]) << '\n';
            found = true;
          } else if (verbose && is_builtin(a[n])) {
            std::cout << a[n] << " is a shell builtin\n";
            found = true;
          } else if (verbose && state.functions.count(a[n])) {
            std::cout << a[n] << " is a function\n"
                      << function_definition(a[n]) << '\n';
            found = true;
          } else if (auto path = shell_find_command(a[n], false, !default_path)) {
            const auto hashed = default_path ? state.command_hash.end()
                                             : state.command_hash.find(a[n]);
            std::string display_path = *path;
            if (verbose && a[n].find('/') == std::string::npos) {
              std::error_code error;
              const auto absolute_path = std::filesystem::absolute(display_path, error);
              if (!error) display_path = absolute_path.lexically_normal().string();
            }
            if (verbose && hashed != state.command_hash.end() &&
                hashed->second == *path)
              std::cout << a[n] << " is hashed (" << display_path << ")\n";
            else {
              if (verbose) std::cout << a[n] << " is ";
              std::cout << display_path << '\n';
            }
            found = true;
          }
          if (found) any_found = true;
          else if (verbose) std::cerr << "command: " << a[n] << ": not found\n";
        }
        return any_found ? 0 : 1;
      };
      return default_path ? with_default_path(list_commands) : list_commands();
    }
    if (i == a.size()) return 0;
    std::vector<std::string> command_args(a.begin() + i, a.end());
    auto run_command = [&]() {
      // `command` suppresses shell-function lookup, but Bash still executes
      // builtins directly. Only commands that are not builtins should reach
      // the external process launcher.
      if (is_builtin(command_args.front()))
        return handle_builtin(command_args).value_or(1);
      return shell_external_command(command_args, !default_path);
    };
    return default_path ? with_default_path(run_command) : run_command();
  }
  if (c == "type") {
    bool all = false, type_only = false, path_only = false;
    bool force_path = false, skip_functions = false;
    size_t i = 1;
    for (; i < a.size() && a[i].size() > 1 && a[i][0] == '-'; ++i) {
      if (a[i] == "-type" || a[i] == "--type") {
        type_only = true;
        path_only = false;
        continue;
      }
      if (a[i] == "-path" || a[i] == "--path") {
        type_only = false;
        path_only = true;
        continue;
      }
      if (a[i] == "-all" || a[i] == "--all") {
        all = true;
        continue;
      }
      if (a[i] == "--") { ++i; break; }
      for (size_t j = 1; j < a[i].size(); ++j) {
        switch (a[i][j]) {
        case 'a': all = true; break;
        case 't': type_only = true; path_only = false; break;
        case 'p': type_only = false; path_only = true; break;
        case 'P': type_only = false; force_path = true; path_only = true; break;
        case 'f': skip_functions = true; break;
        default:
          std::cerr << "type: invalid option -- '" << a[i][j] << "'\n";
          return 2;
        }
      }
    }
    if (i == a.size()) return 2;
    const bool saved_hashall = state.hashall;
    if (force_path) state.hashall = false;
    auto find_all_path_matches = [&](const std::string &command_name) {
      std::vector<std::string> paths;
      auto add_executable = [&](std::string path) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error ||
            access(path.c_str(), X_OK) != 0)
          return;
        paths.push_back(std::move(path));
      };
      if (command_name.find('/') != std::string::npos) {
        add_executable(command_name);
        return paths;
      }
      const auto path_value = shell_path_value();
      if (!path_value) return paths;
      size_t begin = 0;
      while (begin <= path_value->size()) {
        const size_t end = path_value->find(':', begin);
        const std::string directory = path_value->substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        add_executable((std::filesystem::path(directory.empty() ? "." : directory) /
                        command_name).string());
        if (end == std::string::npos) break;
        begin = end + 1;
      }
      return paths;
    };
    int status = 0;
    for (; i < a.size(); ++i) {
      const std::string &name = a[i];
      std::vector<std::pair<std::string, std::string>> matches;
      auto add = [&](const std::string &kind, const std::string &description) {
        matches.emplace_back(kind, description);
      };
      if (!force_path) {
        auto alias = state.aliases.find(name);
        if (alias != state.aliases.end())
          add("alias", name + " is aliased to `" + alias->second + "'");
        if (is_shell_keyword(name))
          add("keyword", name + " is a shell keyword");
        const bool special_builtin_precedes_function =
            state.posix_mode && shell_is_posix_special_builtin(name);
        if (special_builtin_precedes_function && is_builtin(name))
          add("builtin", name + " is a shell builtin");
        if (!skip_functions && state.functions.count(name) &&
            !special_builtin_precedes_function)
          add("function", name + " is a function\n" + function_definition(name));
        if (is_builtin(name) && !special_builtin_precedes_function)
          add("builtin", name + " is a shell builtin");
        if (!skip_functions && state.functions.count(name) &&
            special_builtin_precedes_function)
          add("function", name + " is a function\n" + function_definition(name));
      }
      if (force_path || all || matches.empty()) {
        if (all) {
          for (const auto &path : find_all_path_matches(name)) add("file", path);
        } else if (auto path = shell_find_command(name, false, !force_path)) {
          add("file", *path);
        }
      }
      if (path_only) {
        bool found_path = false;
        for (const auto &[kind, description] : matches) {
          if (kind == "file") { std::cout << description << '\n'; found_path = true; }
        }
        if (!found_path) status = 1;
      } else if (matches.empty()) {
        if (!path_only && !type_only)
          std::cerr << "type: " << name << ": not found\n";
        status = 1;
      } else if (type_only) {
        const size_t count = all ? matches.size() : 1;
        for (size_t n = 0; n < count; ++n) std::cout << matches[n].first << '\n';
      } else {
        const size_t count = all ? matches.size() : 1;
        for (size_t n = 0; n < count; ++n) std::cout << matches[n].second << '\n';
      }
    }
    state.hashall = saved_hashall;
    return status;
  }
  if (c == "builtin" || c == "whence" ||
      c == "which") {
    if (a.size() < 2)
      return 2;
    size_t i = (a[1] == "-v" || a[1] == "-V" || a[1] == "-p") ? 2 : 1;
    if (c == "builtin" && i == 1) {
      std::vector<std::string> nested(a.begin() + 1, a.end());
      auto result = handle_builtin(nested);
      return result ? *result : 1;
    }
    int rc = 0;
    for (; i < a.size(); ++i) {
      if (is_builtin(a[i]))
        std::cout << a[i] << " is a shell builtin\n";
      else if (state.functions.count(a[i]))
        std::cout << a[i] << " is a shell function\n";
      else if (state.aliases.count(a[i]))
        std::cout << a[i] << " is an alias\n";
      else if (auto p = shell_find_command(a[i]))
        std::cout << *p << '\n';
      else {
        std::cerr << a[i] << ": not found\n";
        rc = 1;
      }
    }
    return rc;
  }
  if (c == "hash") {
    enum class HashAction { List, Insert, Delete, Show, Reset };
    HashAction action = HashAction::List;
    bool action_selected = false, reusable = false;
    std::string pathname;
    size_t i = 1;
    auto select_action = [&](HashAction selected) {
      if (action_selected && action != selected) return false;
      action = selected;
      action_selected = true;
      return true;
    };
    for (; i < a.size() && a[i].size() > 1 && a[i][0] == '-'; ++i) {
      if (a[i] == "--") { ++i; break; }
      for (size_t flag = 1; flag < a[i].size(); ++flag) {
        if (a[i][flag] == 'l') reusable = true;
        else if (a[i][flag] == 'r') {
          if (!select_action(HashAction::Reset)) return 2;
        } else if (a[i][flag] == 'd') {
          if (!select_action(HashAction::Delete)) return 2;
        } else if (a[i][flag] == 't') {
          if (!select_action(HashAction::Show)) return 2;
        } else if (a[i][flag] == 'p') {
          if (!select_action(HashAction::Insert)) return 2;
          if (flag + 1 < a[i].size()) {
            pathname = a[i].substr(flag + 1);
            flag = a[i].size();
          } else if (i + 1 < a.size()) {
            pathname = a[++i];
          } else {
            std::cerr << "hash: -p: option requires an argument\n";
            return 2;
          }
        } else {
          std::cerr << "hash: -" << a[i][flag] << ": invalid option\n";
          return 2;
        }
      }
    }
    if (action == HashAction::Reset) {
      if (i != a.size()) return 2;
      state.command_hash.clear();
      state.command_hash_hits.clear();
      return 0;
    }
    if (action == HashAction::Insert) {
      if (pathname.empty() || i == a.size()) return 2;
      for (; i < a.size(); ++i) {
        state.command_hash[a[i]] = pathname;
        state.command_hash_hits[a[i]] = 0;
      }
      return 0;
    }
    auto quote = [](const std::string &value) {
      if (value.empty()) return std::string("''");
      bool safe = true;
      for (unsigned char ch : value)
        if (!(std::isalnum(ch) || std::string("_@%+=:,./-").find(static_cast<char>(ch)) != std::string::npos)) {
          safe = false;
          break;
        }
      if (safe) return value;
      std::string result = "'";
      for (char ch : value) {
        if (ch == '\'') result += "'\\''";
        else result += ch;
      }
      return result + "'";
    };
    if (action == HashAction::List) {
      if (i != a.size()) {
        int status = 0;
        for (; i < a.size(); ++i) {
          auto path = shell_find_command(a[i]);
          if (!path) {
            std::cerr << "hash: " << a[i] << ": not found\n";
            status = 1;
          } else {
            state.command_hash[a[i]] = *path;
            state.command_hash_hits.try_emplace(a[i], 0);
          }
        }
        return status;
      }
      std::vector<std::string> names;
      names.reserve(state.command_hash.size());
      for (const auto &[name, path] : state.command_hash) { (void)path; names.push_back(name); }
      std::sort(names.begin(), names.end());
      if (names.empty()) {
        if (!reusable) std::cerr << "hash: hash table empty\n";
        return 0;
      }
      if (!reusable) std::cout << "hits\tcommand\n";
      for (const auto &name : names) {
        const auto &path = state.command_hash.at(name);
        if (reusable)
          std::cout << "builtin hash -p " << quote(path) << ' ' << quote(name) << '\n';
        else
          std::cout << std::setw(4) << state.command_hash_hits[name] << '\t'
                    << path << '\n';
      }
      return 0;
    }
    if (i == a.size()) {
      if (action == HashAction::Show) {
        std::cerr << "hash: -t: option requires an argument\n";
        return 1;
      }
      if (action == HashAction::Delete) {
        std::cerr << "hash: -d: option requires an argument\n";
        return 1;
      }
      return 0;
    }
    int status = 0;
    const bool show_names = action == HashAction::Show && a.size() - i > 1;
    for (; i < a.size(); ++i) {
      const auto found = state.command_hash.find(a[i]);
      if (action == HashAction::Show) {
        if (found == state.command_hash.end()) {
          std::cerr << "hash: " << a[i] << ": not found\n";
          status = 1;
        } else if (show_names) {
          std::cout << a[i] << '\t' << found->second << '\n';
        } else {
          std::cout << found->second << '\n';
        }
      } else if (action == HashAction::Delete) {
        if (!state.command_hash.erase(a[i])) {
          std::cerr << "hash: " << a[i] << ": not found\n";
          status = 1;
        } else state.command_hash_hits.erase(a[i]);
      }
    }
    return status;
  }
  return {};
}
