#include "builtins_internal.hpp"
#include "arithmetic.hpp"
#include "shell_state.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
SavedVar capture_variable_state(ShellState &state, const std::string &name) {
  SavedVar saved;
  auto variable = state.vars.find(name);
  saved.had_var = variable != state.vars.end();
  if (saved.had_var) saved.var = variable->second;
  if (const char *environment = std::getenv(name.c_str())) {
    saved.had_env = true;
    saved.env = environment;
  }
  auto indexed = state.arrays.find(name);
  saved.had_indexed_array = indexed != state.arrays.end();
  if (saved.had_indexed_array) saved.indexed_array = indexed->second;
  auto indices = state.array_set_indices.find(name);
  saved.had_indexed_array_indices = indices != state.array_set_indices.end();
  if (saved.had_indexed_array_indices) saved.indexed_array_indices = indices->second;
  auto associative = state.associative_arrays.find(name);
  saved.had_associative_array = associative != state.associative_arrays.end();
  if (saved.had_associative_array) saved.associative_array = associative->second;
  saved.had_integer_attribute = state.integer_vars.count(name) != 0;
  saved.had_lowercase_attribute = state.lowercase_vars.count(name) != 0;
  saved.had_uppercase_attribute = state.uppercase_vars.count(name) != 0;
  saved.had_readonly_attribute = state.readonly_vars.count(name) != 0;
  saved.had_unset_state = state.unset_vars.count(name) != 0;
  saved.had_ignoreeof_state = name == "IGNOREEOF";
  if (saved.had_ignoreeof_state) saved.ignoreeof_state = state.ignoreeof;
  auto nameref = state.namerefs.find(name);
  saved.had_nameref = nameref != state.namerefs.end();
  if (saved.had_nameref) saved.nameref_target = nameref->second;
  return saved;
}

void restore_variable_state(ShellState &state, const std::string &name,
                            const SavedVar &saved) {
  state.vars.erase(name);
  state.arrays.erase(name);
  state.array_set_indices.erase(name);
  state.associative_arrays.erase(name);
  state.integer_vars.erase(name);
  state.lowercase_vars.erase(name);
  state.uppercase_vars.erase(name);
  state.readonly_vars.erase(name);
  state.namerefs.erase(name);
  state.unset_vars.erase(name);
  if (saved.had_var) state.vars[name] = saved.var;
  if (saved.had_indexed_array) state.arrays[name] = saved.indexed_array;
  if (saved.had_indexed_array_indices)
    state.array_set_indices[name] = saved.indexed_array_indices;
  else if (saved.had_indexed_array) shell_array_mark_all_set(name);
  if (saved.had_associative_array)
    state.associative_arrays[name] = saved.associative_array;
  if (saved.had_integer_attribute) state.integer_vars.insert(name);
  if (saved.had_lowercase_attribute) state.lowercase_vars.insert(name);
  if (saved.had_uppercase_attribute) state.uppercase_vars.insert(name);
  if (saved.had_readonly_attribute) state.readonly_vars[name] = "1";
  if (saved.had_nameref) state.namerefs[name] = saved.nameref_target;
  if (saved.had_unset_state) state.unset_vars.insert(name);
  if (saved.had_env) setenv(name.c_str(), saved.env.c_str(), 1);
  else unsetenv(name.c_str());
  if (saved.had_ignoreeof_state) {
    state.ignoreeof = saved.ignoreeof_state;
    refresh_option_parameter_values();
  }
}

enum class LocalUnsetDisposition { NotLocal, Hidden, RevealedPrevious };

LocalUnsetDisposition unset_local_binding(ShellState &state,
                                          const std::string &name) {
  for (size_t reverse_index = state.local_scopes.size(); reverse_index > 0;
       --reverse_index) {
    auto &scope = state.local_scopes[reverse_index - 1];
    const auto binding = scope.find(name);
    if (binding == scope.end()) continue;
    if (reverse_index == state.local_scopes.size() || state.localvar_unset)
      return LocalUnsetDisposition::Hidden;
    restore_variable_state(state, name, binding->second);
    return LocalUnsetDisposition::RevealedPrevious;
  }
  return LocalUnsetDisposition::NotLocal;
}

} // namespace

GlobalDeclarationView::GlobalDeclarationView(ShellState &state,
                                             const std::string &name,
                                             bool enabled)
    : state_(state), name_(name) {
  if (!enabled) return;
  for (auto &scope : state_.local_scopes) {
    auto binding = scope.find(name_);
    if (binding == scope.end()) continue;
    global_ = &binding->second;
    local_ = capture_variable_state(state_, name_);
    restore_variable_state(state_, name_, *global_);
    active_ = true;
    break;
  }
}

GlobalDeclarationView::~GlobalDeclarationView() {
  if (!active_) return;
  *global_ = capture_variable_state(state_, name_);
  restore_variable_state(state_, name_, local_);
}

std::optional<int> run_environment_builtin(const std::vector<std::string> &a,
                                           ShellState &state) {
  if (a.empty()) return 0;
  const auto &c = a[0];
  if (c == "export" || c == "typeset" || c == "declare" || c == "local" || c == "readonly") {
    size_t i = 1;
    bool associative = false;
    bool indexed = false;
    bool integer = false;
    bool integer_option_seen = false;
    bool lowercase = false, uppercase = false;
    std::vector<std::pair<char, bool>> case_attribute_operations;
    bool exported = false, export_option_seen = false, unexport = false;
    bool readonly_attribute = false, global_declaration = false;
    bool inherit_local = false;
    bool nameref_attribute = false, disable_nameref = false;
    bool print = false;
    bool local_options_requested = false;
    bool list_functions = false, function_names_only = false;
    bool readonly_function_mode = false;
    bool export_function_mode = false;
    bool trace_function_mode = false;
    bool trace_function_enabled = false;
    while (i < a.size() && ((a[i].size() > 1 &&
           (a[i][0] == '-' || a[i][0] == '+')) || (c == "local" && a[i] == "-"))) {
      if (c == "local" && a[i] == "-") {
        if (state.local_scopes.empty() || state.local_option_scopes.empty()) {
          std::cerr << "local: can only be used in a function\n";
          return 1;
        }
        local_options_requested = true;
        if (!state.local_option_scopes.back())
          state.local_option_scopes.back() = capture_shell_options(state);
        ++i;
        continue;
      }
      const bool enable_attributes = a[i][0] == '-';
      for (size_t flag = 1; flag < a[i].size(); ++flag) {
        if (a[i][flag] == 'i' &&
            (c == "declare" || c == "typeset" || c == "local")) {
          integer = enable_attributes;
          integer_option_seen = true;
        } else if (a[i][flag] == 'l' &&
                   (c == "declare" || c == "typeset" || c == "local")) {
          lowercase = enable_attributes;
          case_attribute_operations.emplace_back('l', enable_attributes);
        } else if (a[i][flag] == 'u' &&
                   (c == "declare" || c == "typeset" || c == "local")) {
          uppercase = enable_attributes;
          case_attribute_operations.emplace_back('u', enable_attributes);
        } else if (a[i][flag] == 'x' &&
                   (c == "export" || c == "declare" || c == "typeset" || c == "local")) {
          exported = enable_attributes;
          export_option_seen = true;
          unexport = !enable_attributes;
        } else if (a[i][flag] == 'r' && enable_attributes &&
                   (c == "declare" || c == "typeset" || c == "local")) {
          readonly_attribute = true;
        } else if (a[i][flag] == 'g' && enable_attributes &&
                   (c == "declare" || c == "typeset" || c == "local")) {
          global_declaration = true;
        } else if (a[i][flag] == 'n' && c == "export") {
          export_option_seen = true;
          exported = false;
          unexport = true;
        } else if (a[i][flag] == 'I' && enable_attributes &&
                   (c == "declare" || c == "typeset" || c == "local")) {
          inherit_local = true;
        } else if (a[i][flag] == 'n' &&
                   (c == "declare" || c == "typeset" || c == "local")) {
          nameref_attribute = enable_attributes;
          disable_nameref = !enable_attributes;
        } else if (a[i][flag] == 'r' &&
                   (c == "declare" || c == "typeset" || c == "local")) {
          // Bash accepts +r, but readonly attributes cannot be removed.
        } else if (enable_attributes && a[i][flag] == 'A') associative = true;
        else if (enable_attributes && a[i][flag] == 'a') indexed = true;
        else if (enable_attributes && a[i][flag] == 'p') print = true;
        else if (enable_attributes && a[i][flag] == 'f' &&
                 (c == "declare" || c == "typeset")) list_functions = true;
        else if (enable_attributes && a[i][flag] == 'f' && c == "readonly")
          readonly_function_mode = true;
        else if (enable_attributes && a[i][flag] == 'f' && c == "export")
          export_function_mode = true;
        else if (enable_attributes && a[i][flag] == 'F' &&
                 (c == "declare" || c == "typeset")) {
          list_functions = true;
          function_names_only = true;
        }
        else if (a[i][flag] == 't' &&
                 (c == "declare" || c == "typeset")) {
          trace_function_mode = true;
          trace_function_enabled = enable_attributes;
        }
        else {
          std::cerr << c << ": invalid option -- '" << a[i][flag] << "'\n";
          return 2;
        }
      }
      ++i;
    }
    if (local_options_requested && i == a.size()) return 0;
    if (trace_function_mode) {
      std::vector<std::string> names;
      for (size_t n = i; n < a.size(); ++n) names.push_back(a[n]);
      if (names.empty()) {
        names.assign(state.traced_functions.begin(), state.traced_functions.end());
        std::sort(names.begin(), names.end());
        for (const auto &name : names) std::cout << "declare -ft " << name << '\n';
        return 0;
      }
      int rc = 0;
      for (const auto &name : names) {
        if (!state.functions.count(name)) {
          std::cerr << c << ": " << name << ": not a function\n";
          rc = 1;
        } else if (trace_function_enabled) {
          state.traced_functions.insert(name);
        } else {
          state.traced_functions.erase(name);
        }
      }
      return rc;
    }
    if (export_function_mode) {
      if (i == a.size() || print) {
        std::vector<std::string> names;
        for (size_t n = i; n < a.size(); ++n) names.push_back(a[n]);
        if (names.empty()) {
          names.assign(state.exported_functions.begin(), state.exported_functions.end());
          std::sort(names.begin(), names.end());
        }
        for (const auto &name : names) {
          if (!state.exported_functions.count(name) || !state.functions.count(name)) continue;
          std::cout << "declare -fx " << name << '\n';
        }
        return 0;
      }
      int rc = 0;
      for (; i < a.size(); ++i) {
        if (!state.functions.count(a[i])) {
          std::cerr << "export: " << a[i] << ": not a function\n";
          rc = 1;
          continue;
        }
        const std::string environment_name = "BASH_FUNC_" + a[i] + "%%";
        if (unexport) {
          state.exported_functions.erase(a[i]);
          unsetenv(environment_name.c_str());
        } else state.exported_functions.insert(a[i]);
      }
      if (!unexport) export_functions_to_environment();
      return rc;
    }
    if ((c == "declare" || c == "typeset") && list_functions &&
        export_option_seen) {
      if (i == a.size() || print) {
        std::vector<std::string> names;
        for (size_t n = i; n < a.size(); ++n) names.push_back(a[n]);
        if (names.empty()) {
          names.assign(state.exported_functions.begin(), state.exported_functions.end());
          std::sort(names.begin(), names.end());
        }
        for (const auto &name : names) {
          if (!state.exported_functions.count(name) || !state.functions.count(name)) continue;
          std::cout << "declare -fx " << name << '\n';
        }
        return 0;
      }
      int rc = 0;
      for (; i < a.size(); ++i) {
        if (!state.functions.count(a[i])) {
          std::cerr << c << ": " << a[i] << ": not a function\n";
          rc = 1;
          continue;
        }
        const std::string environment_name = "BASH_FUNC_" + a[i] + "%%";
        if (unexport) {
          state.exported_functions.erase(a[i]);
          unsetenv(environment_name.c_str());
        } else state.exported_functions.insert(a[i]);
      }
      if (!unexport) export_functions_to_environment();
      return rc;
    }
    if (readonly_function_mode) {
      if (i == a.size() || print) {
        std::vector<std::string> names;
        for (size_t n = i; n < a.size(); ++n) names.push_back(a[n]);
        if (names.empty()) {
          names.assign(state.readonly_functions.begin(), state.readonly_functions.end());
          std::sort(names.begin(), names.end());
        }
        for (const auto &name : names) {
          if (!state.readonly_functions.count(name)) continue;
          const auto function = state.functions.find(name);
          if (function == state.functions.end()) continue;
          if (print) std::cout << "declare -fr " << name << '\n';
          else {
            std::cout << name << " ()\n{\n" << function->second;
            if (function->second.empty() || function->second.back() != '\n')
              std::cout << '\n';
            std::cout << "}\n";
          }
        }
        return 0;
      }
      int rc = 0;
      for (; i < a.size(); ++i) {
        const auto function = state.functions.find(a[i]);
        if (function == state.functions.end()) {
          std::cerr << "readonly: " << a[i] << ": not a function\n";
          rc = 1;
          continue;
        }
        state.readonly_functions.insert(a[i]);
      }
      return rc;
    }
    if (list_functions) {
      std::vector<std::string> names;
      for (size_t n = i; n < a.size(); ++n) names.push_back(a[n]);
      if (names.empty()) {
        for (const auto &[name, body] : state.functions) { (void)body; names.push_back(name); }
        std::sort(names.begin(), names.end());
      }
      for (const auto &name : names) {
        const auto function = state.functions.find(name);
        if (function == state.functions.end()) continue;
        if (function_names_only) {
          std::cout << "declare -f " << name << '\n';
        } else {
          std::cout << name << " ()\n{\n" << function->second;
          if (function->second.empty() || function->second.back() != '\n') std::cout << '\n';
          std::cout << "}\n";
        }
      }
      return 0;
    }
    if (i == a.size() || print) {
      auto quote = [](const std::string &value) {
        std::string result = "'";
        for (char ch : value) {
          if (ch == '\'') result += "'\\''";
          else result += ch;
        }
        return result + "'";
      };
      std::vector<std::string> names;
      for (size_t n = i; n < a.size(); ++n) {
        const size_t equals = a[n].find('=');
        const std::string name = a[n].substr(0, equals);
        const size_t bracket = name.find('[');
        names.push_back(bracket == std::string::npos ? name : name.substr(0, bracket));
      }
      if (names.empty()) {
        for (const auto &[name, value] : state.vars) { (void)value; names.push_back(name); }
        for (const auto &[name, value] : state.arrays) { (void)value; names.push_back(name); }
        for (const auto &[name, value] : state.associative_arrays) { (void)value; names.push_back(name); }
        for (const auto &name : state.unset_vars) names.push_back(name);
        for (const auto &[name, target] : state.namerefs) { (void)target; names.push_back(name); }
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
      }
      for (const auto &name : names) {
        const auto scalar = state.vars.find(name);
        const auto indexed_array = state.arrays.find(name);
        const auto assoc_array = state.associative_arrays.find(name);
        const bool is_array = indexed_array != state.arrays.end();
        const bool is_assoc = assoc_array != state.associative_arrays.end();
        const bool is_unset = state.unset_vars.count(name) != 0;
        const auto nameref = state.namerefs.find(name);
        const bool is_nameref = nameref != state.namerefs.end();
        const bool is_exported = std::getenv(name.c_str()) != nullptr;
        const bool is_readonly = state.readonly_vars.count(name) != 0;
        if (c == "export" && !is_exported) continue;
        if (c == "readonly" && !is_readonly) continue;
        if (c == "declare" || c == "typeset" || c == "local") {
          if (scalar == state.vars.end() && !is_array && !is_assoc && !is_unset && !is_nameref) {
            std::cerr << c << ": " << name << ": not found\n";
            continue;
          }
        }
        std::string flags;
        if (is_array) flags += 'a';
        if (is_assoc) flags += 'A';
        if (state.integer_vars.count(name)) flags += 'i';
        if (state.lowercase_vars.count(name)) flags += 'l';
        if (state.uppercase_vars.count(name)) flags += 'u';
        if (is_nameref) flags += 'n';
        if ((c == "export" || is_exported) && !unexport) flags += 'x';
        if (c == "readonly" || is_readonly || readonly_attribute) flags += 'r';
        if (c == "export" && flags.empty()) flags = "x";
        if (c == "readonly" && flags.empty()) flags = "r";
        std::cout << "declare -" << (flags.empty() ? "-" : flags) << ' ' << name;
        if (is_array) {
          std::cout << "=(";
          bool first = true;
          for (size_t index = 0; index < indexed_array->second.size(); ++index) {
            if (!shell_array_index_is_set(name, index)) continue;
            if (!first) std::cout << ' ';
            first = false;
            std::cout << '[' << index << "]=" << quote(indexed_array->second[index]);
          }
          std::cout << ')';
        } else if (is_assoc) {
          std::vector<std::string> keys;
          for (const auto &[key, value] : assoc_array->second) { (void)value; keys.push_back(key); }
          std::sort(keys.begin(), keys.end());
          std::cout << "=(";
          for (size_t index = 0; index < keys.size(); ++index) {
            if (index) std::cout << ' ';
            std::cout << '[' << quote(keys[index]) << "]=" << quote(assoc_array->second.at(keys[index]));
          }
          std::cout << ')';
        } else if (is_nameref) {
          if (!nameref->second.empty()) std::cout << '=' << quote(nameref->second);
        } else if (scalar != state.vars.end()) {
          std::cout << '=' << quote(scalar->second);
        }
        std::cout << '\n';
      }
      return 0;
    }
    for (; i < a.size(); ++i) {
      auto p = a[i].find('=');
      std::string assignment_name = a[i].substr(0, p);
      const size_t bracket = assignment_name.find('[');
      std::string key = bracket == std::string::npos ? assignment_name : assignment_name.substr(0, bracket);
      if (!shell_valid_name(key)) {
        std::cerr << c << ": " << key << ": invalid identifier\n";
        return 1;
      }
      GlobalDeclarationView global_view(state, key, global_declaration);
      if (c == "local" && state.local_scopes.empty()) {
        std::cerr << "local: can only be used in a function\n";
        return 1;
      }
      const bool local_declaration = (c == "local" && !global_declaration) ||
          ((c == "typeset" || c == "declare") && !global_declaration && !state.local_scopes.empty());
      const bool inherit_local_value = inherit_local ||
          (local_declaration && state.localvar_inherit);
      if (local_declaration && !state.local_scopes.empty() &&
          shell_valid_name(key)) {
        if (!state.local_scopes.back().count(key)) {
          const char *old = getenv(key.c_str());
          auto vi = state.vars.find(key);
          SavedVar saved;
          saved.had_var = vi != state.vars.end();
          if (saved.had_var) saved.var = vi->second;
          saved.had_env = old != nullptr;
          if (saved.had_env) saved.env = old;
          auto indexed = state.arrays.find(key);
          saved.had_indexed_array = indexed != state.arrays.end();
          if (saved.had_indexed_array) saved.indexed_array = indexed->second;
          auto indexed_indices = state.array_set_indices.find(key);
          saved.had_indexed_array_indices = indexed_indices != state.array_set_indices.end();
          if (saved.had_indexed_array_indices)
            saved.indexed_array_indices = indexed_indices->second;
          auto associative = state.associative_arrays.find(key);
          saved.had_associative_array = associative != state.associative_arrays.end();
          if (saved.had_associative_array) saved.associative_array = associative->second;
          saved.had_integer_attribute = state.integer_vars.count(key) != 0;
          saved.had_lowercase_attribute = state.lowercase_vars.count(key) != 0;
          saved.had_uppercase_attribute = state.uppercase_vars.count(key) != 0;
          saved.had_readonly_attribute = state.readonly_vars.count(key) != 0;
          saved.had_unset_state = state.unset_vars.count(key) != 0;
          if (key == "IGNOREEOF") {
            saved.had_ignoreeof_state = true;
            saved.ignoreeof_state = state.ignoreeof;
          }
          auto nameref = state.namerefs.find(key);
          saved.had_nameref = nameref != state.namerefs.end();
          if (saved.had_nameref) saved.nameref_target = nameref->second;
          state.local_scopes.back()[key] = std::move(saved);
          // A new local declaration shadows the outer value. Repeated
          // declarations in this frame keep the already-local value.
          state.vars.erase(key);
          state.arrays.erase(key);
          state.array_set_indices.erase(key);
          state.associative_arrays.erase(key);
          state.integer_vars.erase(key);
          state.lowercase_vars.erase(key);
          state.uppercase_vars.erase(key);
          state.readonly_vars.erase(key);
          state.namerefs.erase(key);
          unsetenv(key.c_str());
          state.unset_vars.insert(key);
          if (key == "IGNOREEOF") {
            state.ignoreeof = false;
            refresh_option_parameter_values();
          }
          if (inherit_local_value) {
            const SavedVar &outer = state.local_scopes.back().at(key);
            if (outer.had_var) state.vars[key] = outer.var;
            else if (outer.had_env) state.vars[key] = outer.env;
            if (outer.had_env) setenv(key.c_str(), outer.env.c_str(), 1);
            if (outer.had_indexed_array) state.arrays[key] = outer.indexed_array;
            if (outer.had_indexed_array_indices)
              state.array_set_indices[key] = outer.indexed_array_indices;
            else if (outer.had_indexed_array) shell_array_mark_all_set(key);
            if (outer.had_associative_array)
              state.associative_arrays[key] = outer.associative_array;
            if (outer.had_integer_attribute) state.integer_vars.insert(key);
            if (outer.had_lowercase_attribute) state.lowercase_vars.insert(key);
            if (outer.had_uppercase_attribute) state.uppercase_vars.insert(key);
            if (outer.had_readonly_attribute) state.readonly_vars[key] = "1";
            // Bash deliberately does not inherit the nameref attribute.
            if (outer.had_unset_state) state.unset_vars.insert(key);
            else state.unset_vars.erase(key);
            if (key == "IGNOREEOF" && outer.had_ignoreeof_state) {
              state.ignoreeof = outer.ignoreeof_state;
              refresh_option_parameter_values();
            }
          }
        }
      }
      if (disable_nameref) {
        const auto reference = state.namerefs.find(key);
        if (reference != state.namerefs.end()) {
          state.vars[key] = reference->second;
          state.namerefs.erase(reference);
          state.unset_vars.erase(key);
        }
      }
      if (integer_option_seen) {
        if (integer) state.integer_vars.insert(key);
        else state.integer_vars.erase(key);
      }
      for (const auto &[attribute, enabled] : case_attribute_operations) {
        if (attribute == 'l') {
          if (enabled) {
            state.lowercase_vars.insert(key);
            state.uppercase_vars.erase(key);
          } else state.lowercase_vars.erase(key);
        } else if (enabled) {
          state.uppercase_vars.insert(key);
          state.lowercase_vars.erase(key);
        } else state.uppercase_vars.erase(key);
      }
      if (associative) {
        auto &array = state.associative_arrays[key];
        state.unset_vars.erase(key);
        if (local_declaration && !inherit_local_value)
          array.clear();
        state.arrays.erase(key);
        state.array_set_indices.erase(key);
        state.vars.erase(key);
        unsetenv(key.c_str());
      } else if (indexed) {
        auto &array = state.arrays[key];
        state.unset_vars.erase(key);
        if (local_declaration && !inherit_local_value) {
          array.clear();
          shell_array_clear_set_indices(key);
        }
        state.associative_arrays.erase(key);
      }
      if (associative) {
        if (p == std::string::npos) state.associative_arrays.try_emplace(key);
        else if (a[i].substr(p + 1).find('(') != std::string::npos) {
          std::cerr << c << ": associative compound assignment is not supported in this form\n";
          return 2;
        } else if (!shell_set_assignment(key + "[0]=" + a[i].substr(p + 1), false))
          return 1;
      } else if (indexed && p == std::string::npos) {
        state.arrays.try_emplace(key);
      } else if (p != std::string::npos) {
        if (nameref_attribute) {
          if (!shell_set_nameref(key, a[i].substr(p + 1))) return 1;
          continue;
        }
        const bool should_export = c == "export" ? !unexport
            : export_option_seen && exported;
        if (!shell_set_assignment(a[i], should_export))
          return 1;
        if (unexport) {
          const auto current = state.vars.find(key);
          if (current != state.vars.end()) setenv(key.c_str(), current->second.c_str(), 1);
          unsetenv(key.c_str());
        }
        if (c == "readonly" || readonly_attribute)
          state.readonly_vars[key] = 1;
      } else if (c == "local" || c == "typeset" || c == "declare" ||
                 c == "export" || c == "readonly" || readonly_attribute ||
                 export_option_seen || unexport || disable_nameref) {
        if (nameref_attribute) {
          if (!shell_set_nameref(key, "")) return 1;
          continue;
        }
        const auto existing = state.vars.find(key);
        const char *environment_value = getenv(key.c_str());
        const std::string current = existing != state.vars.end()
            ? existing->second : environment_value ? environment_value : "";
        if ((integer || lowercase || uppercase) && !current.empty()) {
          if (!shell_set_assignment(key + "=" + current)) return 1;
        } else if (!local_declaration || export_option_seen || readonly_attribute) {
          state.vars[key] = current;
          state.unset_vars.erase(key);
        }
        const bool should_export = c == "export" ? !unexport
            : export_option_seen && exported;
        if (unexport) {
          if (environment_value && existing == state.vars.end()) state.vars[key] = environment_value;
          if (unsetenv(key.c_str()) != 0) return 1;
        } else if (should_export && setenv(key.c_str(), current.c_str(), 1) != 0) return 1;
        if (c == "readonly" || readonly_attribute) state.readonly_vars[key] = 1;
      }
    }
    return 0;
  }
  if (c == "unset") {
    int rc = 0;
    bool unset_function = false;
    bool unset_nameref = false;
    bool parse_options = true;
    for (size_t i = 1; i < a.size(); ++i) {
      if (parse_options && a[i] == "--") {
        parse_options = false;
        continue;
      }
      if (parse_options && a[i].size() > 1 && a[i][0] == '-') {
        for (size_t option = 1; option < a[i].size(); ++option) {
          switch (a[i][option]) {
          case 'f': unset_function = true; unset_nameref = false; break;
          case 'v': unset_function = false; unset_nameref = false; break;
          case 'n': unset_function = false; unset_nameref = true; break;
          default:
            std::cerr << "unset: -" << a[i][option] << ": invalid option\n";
            return 2;
          }
        }
        continue;
      }
      if (parse_options && a[i] == "-") parse_options = false;
      if (unset_function) {
        if (state.readonly_functions.count(a[i])) {
          std::cerr << "unset: " << a[i]
                    << ": cannot unset: readonly function\n";
          rc = 1;
          continue;
        }
        state.functions.erase(a[i]);
        state.function_sources.erase(a[i]);
        state.traced_functions.erase(a[i]);
        if (state.exported_functions.erase(a[i]))
          unsetenv(("BASH_FUNC_" + a[i] + "%%").c_str());
        continue;
      }
      const std::string name = unset_nameref ? a[i] : shell_resolve_nameref(a[i]);
      if (name.empty()) continue;
      if (!unset_function && (name == "EPOCHSECONDS" || name == "EPOCHREALTIME" ||
                              name == "SECONDS" || name == "RANDOM" || name == "SRANDOM" ||
                              name == "GROUPS" || name == "BASHPID" || name == "BASH_ARGV0" ||
                              name == "BASH_COMMAND" || name == "BASH_SUBSHELL" ||
                              name == "HISTCMD"))
        state.disabled_special_parameters.insert(name);
      if (!unset_function && (name == "DIRSTACK" || name == "DIRSTACK[@]" ||
                              name == "DIRSTACK[*]"))
        state.disabled_special_parameters.insert("DIRSTACK");
      const size_t bracket = name.find('[');
      if (bracket != std::string::npos && !name.empty() && name.back() == ']') {
        const std::string array_name = name.substr(0, bracket);
        const std::string index_text = name.substr(bracket + 1, name.size() - bracket - 2);
        const bool unset_whole_array = index_text == "@" || index_text == "*";
        if (unset_whole_array &&
            (state.arrays.count(array_name) || state.associative_arrays.count(array_name))) {
          if (state.readonly_vars.count(array_name)) {
            std::cerr << "unset: " << array_name << ": readonly variable\n";
            rc = 1;
            continue;
          }
          const auto local_unset = unset_local_binding(state, array_name);
          if (local_unset == LocalUnsetDisposition::RevealedPrevious) continue;
          const bool local_binding = local_unset == LocalUnsetDisposition::Hidden;
          state.vars.erase(array_name);
          state.arrays.erase(array_name);
          state.array_set_indices.erase(array_name);
          state.associative_arrays.erase(array_name);
          state.integer_vars.erase(array_name);
          state.lowercase_vars.erase(array_name);
          state.uppercase_vars.erase(array_name);
          state.namerefs.erase(array_name);
          if (local_binding) state.unset_vars.insert(array_name);
          else state.unset_vars.erase(array_name);
          unsetenv(array_name.c_str());
        } else if (state.readonly_vars.count(array_name)) {
          std::cerr << "unset: " << array_name << ": readonly variable\n";
          rc = 1;
        } else if (state.associative_arrays.count(array_name)) {
          state.associative_arrays[array_name].erase(index_text);
        } else if (state.arrays.count(array_name)) {
          if (array_name == "DIRSTACK" &&
              !state.disabled_special_parameters.count("DIRSTACK"))
            continue;
          std::string index_error;
          const auto evaluated_index = evaluate_arithmetic(index_text, &index_error);
          if (evaluated_index) {
            long long index = *evaluated_index;
            auto &array = state.arrays[array_name];
            if (index < 0) index += static_cast<long long>(shell_array_index_extent(array_name));
            if (index >= 0 && static_cast<size_t>(index) < array.size())
              shell_array_unset_index(array_name, static_cast<size_t>(index));
          } else {
            std::cerr << "unset: " << array_name << ": "
                      << (index_error.empty() ? "invalid array subscript" : index_error) << '\n';
            rc = 1;
          }
        }
      } else if (state.readonly_vars.count(name)) {
        std::cerr << "unset: " << name << ": readonly variable\n";
        rc = 1;
      } else {
        if (unset_nameref) state.namerefs.erase(name);
        if (name == "PATH") {
          state.command_hash.clear();
          state.command_hash_hits.clear();
        }
        const auto local_unset = unset_local_binding(state, name);
        if (local_unset == LocalUnsetDisposition::RevealedPrevious) continue;
        const bool local_binding = local_unset == LocalUnsetDisposition::Hidden;
        state.vars.erase(name);
        if (name == "IGNOREEOF") {
          state.ignoreeof = false;
          refresh_option_parameter_values();
        }
        if (local_binding) state.unset_vars.insert(name);
        else state.unset_vars.erase(name);
        state.integer_vars.erase(name);
        state.lowercase_vars.erase(name);
        state.uppercase_vars.erase(name);
        state.arrays.erase(name);
        state.array_set_indices.erase(name);
        state.associative_arrays.erase(name);
        state.namerefs.erase(name);
        unsetenv(name.c_str());
      }
    }
    return rc;
  }
  if (c == "alias") {
    if (a.size() == 1) {
      for (auto &[k, v] : state.aliases)
        std::cout << "alias " << k << "='" << v << "'\n";
      return 0;
    }
    for (size_t i = 1; i < a.size(); ++i) {
      auto p = a[i].find('=');
      if (p == std::string::npos) {
        auto it = state.aliases.find(a[i]);
        if (it == state.aliases.end())
          return 1;
        std::cout << "alias " << it->first << "='" << it->second << "'\n";
      } else
        state.aliases[a[i].substr(0, p)] = a[i].substr(p + 1);
    }
    return 0;
  }
  if (c == "unalias") {
    if (a.size() < 2)
      return 2;
    int rc = 0;
    for (size_t i = 1; i < a.size(); ++i)
      if (!state.aliases.erase(a[i]))
        rc = 1;
    return rc;
  }
  return {};
}
