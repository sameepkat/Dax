#include <functional>
#include <string>
#include <vector>

using BuiltinFunc = std::function<bool(const std::vector<std::string>&)>;

bool builtin_exit(const std::vector<std::string>&);
bool builtin_cd(const std::vector<std::string>&);
bool builtin_pwd(const std::vector<std::string>&);
bool builtin_echo(const std::vector<std::string>&);
bool builtin_unset(const std::vector<std::string>&);
bool builtin_export(const std::vector<std::string>&);
bool builtin_alias(const std::vector<std::string>&);
bool builtin_unalias(const std::vector<std::string>&);
bool builtin_jobs(const std::vector<std::string>&);
bool builtin_fg(const std::vector<std::string>&);
bool builtin_bg(const std::vector<std::string>&);
bool builtin_history(const std::vector<std::string>&);
// bool builtin_man(const std::vector<std::string>&);
bool builtin_type(const std::vector<std::string>&);
bool builtin_set(const std::vector<std::string>&);
bool builtin_hash(const std::vector<std::string>&);
bool builtin_read(const std::vector<std::string>&);
bool builtin_test(const std::vector<std::string>&);
bool builtin_true(const std::vector<std::string>&);
bool builtin_false(const std::vector<std::string>&);
BuiltinFunc handle_builtin(const std::vector<std::string> &);
