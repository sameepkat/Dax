#include "helper.hpp"
#include <cerrno>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <iomanip>
#include <iterator>
#include <readline/history.h>
#include <sstream>
#include <unordered_map>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
namespace fs = std::filesystem;

void report_command_launch_error(const std::string &command, int error) {
  if (error == ENOENT) {
    std::fprintf(stderr, "dax: %s: command not found\n", command.c_str());
    return;
  }
  std::fprintf(stderr, "dax: %s: %s\n", command.c_str(), std::strerror(error));
}

std::vector<std::string> extract_key_value(const std::string &token) {
  auto pos = token.find('=');
  if (pos == std::string::npos) {
    std::cerr << "export: invalid format, expected VAR=value\n";
    return {};
  }

  std::string key = token.substr(0, pos);
  std::string value = token.substr(pos + 1);

  return {key, value};
}

void print_vector(const std::vector<std::string> &vector) {
  for (const auto &it : vector) {
    std::cout << it << "\n";
  }
}

void print_table(
    const std::unordered_map<std::string, std::string> &shellvar_map) {
  for (const auto &[key, value] : shellvar_map) {
    // Print each key-value pair
    std::cout << "Key: [" << key << "] Value: [" << value << "]\n";
  }
}

void print_prompt() {
  char cwd[1024];
  if (getcwd(cwd, sizeof(cwd)) != nullptr) {
    fs::path path(cwd);
    std::string dir_name = path.filename().string();
    if (dir_name.empty())
      dir_name = "/";

    std::cout << "\033[1;32m" << dir_name << " > \033[0m";
  } else {
    std::cout << "\033[1;32m> \033[0m";
  }
  std::cout.flush();
}

namespace History {
static std::deque<std::string> history;
static std::deque<std::string> session_entries;
static std::unordered_map<std::string, size_t> session_entries_written;
static std::unordered_map<std::string, size_t> observed_file_entries;
static Config settings;
static bool loaded = false;
static const std::string multiline_history_prefix = "\x1e" "DAXH1:";

static std::string encode_history_entry(const std::string &entry) {
  if (entry.find('\n') == std::string::npos &&
      entry.rfind(multiline_history_prefix, 0) != 0) return entry;
  std::string encoded = multiline_history_prefix;
  for (char ch : entry) {
    if (ch == '\\') encoded += "\\\\";
    else if (ch == '\n') encoded += "\\n";
    else encoded += ch;
  }
  return encoded;
}

static std::string decode_history_entry(const std::string &entry) {
  if (entry.rfind(multiline_history_prefix, 0) != 0) return entry;
  std::string decoded;
  for (size_t i = multiline_history_prefix.size(); i < entry.size(); ++i) {
    if (entry[i] != '\\' || i + 1 == entry.size()) {
      decoded += entry[i];
      continue;
    }
    const char escaped = entry[++i];
    if (escaped == 'n') decoded += '\n';
    else if (escaped == '\\') decoded += '\\';
    else { decoded += '\\'; decoded += escaped; }
  }
  return decoded;
}

static std::string history_path() {
  if (!settings.file.empty()) {
    std::string path = settings.file;
    if (path == "~") {
      if (const char *home = getenv("HOME")) return home;
    } else if (path.rfind("~/", 0) == 0) {
      if (const char *home = getenv("HOME")) return std::string(home) + path.substr(1);
    }
    return path;
  }
  const char *home = getenv("HOME");
  return home ? std::string(home) + "/.dax_history" : std::string();
}

class HistoryLock {
public:
  explicit HistoryLock(const std::string &path) {
    fd_ = open((path + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd_ >= 0 && flock(fd_, LOCK_EX) != 0) { close(fd_); fd_ = -1; }
  }
  ~HistoryLock() { if (fd_ >= 0) { flock(fd_, LOCK_UN); close(fd_); } }
  bool valid() const { return fd_ >= 0; }
  int fd() const { return fd_; }
private:
  int fd_ = -1;
};

static void sync_readline() {
  clear_history();
  stifle_history(static_cast<int>(settings.memory_limit));
  for (const auto &entry : history) add_history(entry.c_str());
}

static std::vector<std::string> read_file(const std::string &path) {
  std::vector<std::string> entries;
  std::ifstream file(path);
  std::string line;
  while (std::getline(file, line)) entries.push_back(decode_history_entry(line));
  return entries;
}

static size_t file_size(const std::string &path) {
  struct stat info {};
  return stat(path.c_str(), &info) == 0 ? static_cast<size_t>(info.st_size) : 0;
}

static void store_count(const HistoryLock &lock, size_t count, size_t bytes) {
  if (!lock.valid()) return;
  const std::string metadata = std::to_string(count) + " " + std::to_string(bytes) + "\n";
  if (ftruncate(lock.fd(), 0) == 0)
    (void)pwrite(lock.fd(), metadata.data(), metadata.size(), 0);
}

static size_t current_file_count(const HistoryLock &lock, const std::string &path) {
  const size_t bytes = file_size(path);
  char metadata[96] = {};
  const ssize_t length = lock.valid() ? pread(lock.fd(), metadata, sizeof(metadata) - 1, 0) : -1;
  if (length > 0) {
    size_t count = 0, recorded_bytes = 0;
    std::istringstream input(std::string(metadata, static_cast<size_t>(length)));
    if (input >> count >> recorded_bytes && recorded_bytes == bytes) return count;
  }
  const size_t count = read_file(path).size();
  store_count(lock, count, bytes);
  return count;
}

static bool write_all(int fd, const std::string &text) {
  size_t offset = 0;
  while (offset < text.size()) {
    ssize_t count = write(fd, text.data() + offset, text.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    offset += static_cast<size_t>(count);
  }
  return true;
}

static bool replace_file(const std::string &path, const std::vector<std::string> &entries) {
  const std::string temporary = path + ".tmp." + std::to_string(getpid());
  int fd = open(temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  bool ok = true;
  for (const auto &entry : entries) {
    if (!write_all(fd, encode_history_entry(entry) + "\n")) { ok = false; break; }
  }
  if (ok && fsync(fd) != 0) ok = false;
  if (close(fd) != 0) ok = false;
  if (ok && rename(temporary.c_str(), path.c_str()) == 0) return true;
  unlink(temporary.c_str());
  return false;
}

Config config() { return settings; }

void configure(const Config &value) {
  settings = value;
  if (settings.memory_limit == 0) settings.memory_limit = 1;
  if (settings.file_limit == 0) settings.file_limit = 1;
  if (settings.memory_limit > 1000000) settings.memory_limit = 1000000;
  if (settings.file_limit > 1000000) settings.file_limit = 1000000;
  while (history.size() > settings.memory_limit) history.pop_front();
  if (loaded) {
    sync_readline();
    save_to_file();
  }
}

bool add(const std::string &line) {
  if (settings.ignore_space && !line.empty() && line.front() == ' ') return false;
  if (settings.ignore_duplicates && !history.empty() && history.back() == line) return false;
  history.push_back(line);
  session_entries.push_back(line);
  if (history.size() > settings.memory_limit) history.pop_front();
  if (loaded) add_history(line.c_str());

  const std::string path = history_path();
  if (path.empty()) return true;
  std::error_code ec;
  const auto parent = fs::path(path).parent_path();
  if (!parent.empty()) fs::create_directories(parent, ec);
  HistoryLock lock(path);
  if (!lock.valid()) {
    std::fprintf(stderr, "dax: cannot lock history file '%s': %s\n", path.c_str(), std::strerror(errno));
    return true;
  }
  size_t count = current_file_count(lock, path);
  int fd = open(path.c_str(), O_CREAT | O_APPEND | O_WRONLY | O_CLOEXEC, 0600);
  if (fd < 0 || !write_all(fd, encode_history_entry(line) + "\n")) {
    const int error = errno;
    if (fd >= 0) close(fd);
    std::fprintf(stderr, "dax: cannot append history file '%s': %s\n", path.c_str(), std::strerror(error));
    return true;
  }
  close(fd);
  ++count;
  observed_file_entries[path] = count;
  session_entries_written[path] = session_entries.size();
  if (count > settings.file_limit) {
    auto entries = read_file(path);
    entries.erase(entries.begin(), entries.end() - settings.file_limit);
    if (!replace_file(path, entries))
      std::fprintf(stderr, "dax: cannot trim history file '%s': %s\n", path.c_str(), std::strerror(errno));
    else count = entries.size();
  }
  store_count(lock, count, file_size(path));
  return true;
}

bool append_new_to_file(const std::string &filename) {
  const std::string path = filename.empty() ? history_path() : filename;
  if (path.empty()) return false;
  HistoryLock lock(path);
  if (!lock.valid()) return false;
  size_t &written = session_entries_written[path];
  if (written > session_entries.size()) written = session_entries.size();
  int fd = open(path.c_str(), O_CREAT | O_APPEND | O_WRONLY | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  bool ok = true;
  size_t count = current_file_count(lock, path);
  for (size_t i = written; i < session_entries.size(); ++i) {
    if (!write_all(fd, encode_history_entry(session_entries[i]) + "\n")) {
      ok = false;
      break;
    }
    ++count;
  }
  if (close(fd) != 0) ok = false;
  if (!ok) return false;
  if (count > settings.file_limit) {
    auto entries = read_file(path);
    entries.erase(entries.begin(), entries.end() - settings.file_limit);
    if (!replace_file(path, entries)) return false;
    count = entries.size();
  }
  written = session_entries.size();
  observed_file_entries[path] = count;
  store_count(lock, count, file_size(path));
  return true;
}

bool read_new_from_file(const std::string &filename) {
  const std::string path = filename.empty() ? history_path() : filename;
  if (path.empty()) return false;
  HistoryLock lock(path);
  auto entries = read_file(path);
  if (access(path.c_str(), R_OK) != 0) return false;
  size_t &observed = observed_file_entries[path];
  const size_t start = entries.size() < observed ? 0 : observed;
  history.insert(history.end(), entries.begin() + static_cast<std::ptrdiff_t>(start), entries.end());
  observed = entries.size();
  while (history.size() > settings.memory_limit) history.pop_front();
  if (loaded) sync_readline();
  if (lock.valid()) store_count(lock, entries.size(), file_size(path));
  return true;
}

bool read_from_file(const std::string &filename, bool append) {
  const std::string path = filename.empty() ? history_path() : filename;
  if (path.empty()) return false;
  std::ifstream input(path);
  if (!input) return false;
  std::vector<std::string> entries;
  std::string line;
  while (std::getline(input, line)) entries.push_back(decode_history_entry(line));
  if (!input.eof()) return false;
  if (!append) history.clear();
  history.insert(history.end(), entries.begin(), entries.end());
  observed_file_entries[path] = entries.size();
  while (history.size() > settings.memory_limit) history.pop_front();
  if (loaded) sync_readline();
  return true;
}

bool write_to_file(const std::string &filename) {
  const std::string path = filename.empty() ? history_path() : filename;
  if (path.empty()) return false;
  HistoryLock lock(path);
  if (!lock.valid()) return false;
  std::vector<std::string> entries(history.begin(), history.end());
  if (entries.size() > settings.file_limit)
    entries.erase(entries.begin(), entries.end() - settings.file_limit);
  if (!replace_file(path, entries)) return false;
  store_count(lock, entries.size(), file_size(path));
  observed_file_entries[path] = entries.size();
  session_entries_written[path] = session_entries.size();
  return true;
}

void print() {
  print(static_cast<int>(history.size()));
}

void print(int num) {
  if (num <= 0)
    return;
  size_t start = history.size() > static_cast<size_t>(num)
                     ? history.size() - static_cast<size_t>(num)
                     : 0;
  for (size_t i = start; i < history.size(); ++i)
    std::cout << std::setw(5) << history_base + static_cast<int>(i)
              << "  " << history[i] << "\n";
}

void clear() {
  history.clear();
  session_entries.clear();
  session_entries_written.clear();
  if (loaded) sync_readline();
  const std::string path = history_path();
  if (path.empty()) return;
  HistoryLock lock(path);
  if (!lock.valid()) return;
  if (!replace_file(path, {}))
    std::fprintf(stderr, "dax: cannot clear history file '%s': %s\n", path.c_str(), std::strerror(errno));
  else {
    observed_file_entries[path] = 0;
    store_count(lock, 0, 0);
  }
}

void remove(int index) {
  if (index < 0 || static_cast<size_t>(index) >= history.size())
    return;
  const std::string removed = history[static_cast<size_t>(index)];
  history.erase(history.begin() + index);
  const auto session_item = std::find(session_entries.rbegin(), session_entries.rend(), removed);
  if (session_item != session_entries.rend()) {
    const size_t removed_index = static_cast<size_t>(
        std::distance(session_item, session_entries.rend()) - 1);
    session_entries.erase(session_entries.begin() + static_cast<std::ptrdiff_t>(removed_index));
    for (auto &[path, written] : session_entries_written) {
      (void)path;
      if (written > removed_index) --written;
    }
  }
  if (loaded) sync_readline();
  const std::string path = history_path();
  if (path.empty()) return;
  HistoryLock lock(path);
  if (!lock.valid()) return;
  auto entries = read_file(path);
  auto found = std::find(entries.rbegin(), entries.rend(), removed);
  if (found != entries.rend()) entries.erase(std::next(found).base());
  if (!replace_file(path, entries))
    std::fprintf(stderr, "dax: cannot update history file '%s': %s\n", path.c_str(), std::strerror(errno));
  else {
    observed_file_entries[path] = entries.size();
    store_count(lock, entries.size(), file_size(path));
  }
}

const std::deque<std::string> &get_all() { return history; }

void load_from_file() {
  history.clear();
  session_entries.clear();
  session_entries_written.clear();
  const std::string path = history_path();
  if (!path.empty()) {
    HistoryLock lock(path);
    auto entries = read_file(path);
    observed_file_entries[path] = entries.size();
    if (entries.size() > settings.file_limit)
      entries.erase(entries.begin(), entries.end() - settings.file_limit);
    if (entries.size() > settings.memory_limit)
      entries.erase(entries.begin(), entries.end() - settings.memory_limit);
    history.assign(entries.begin(), entries.end());
    if (lock.valid()) store_count(lock, read_file(path).size(), file_size(path));
  }
  loaded = true;
  sync_readline();
}

void save_to_file(bool append) {
  const std::string path = history_path();
  if (path.empty()) return;
  HistoryLock lock(path);
  if (!lock.valid()) return;
  if (!append) {
    std::vector<std::string> entries(history.begin(), history.end());
    if (entries.size() > settings.file_limit)
      entries.erase(entries.begin(), entries.end() - settings.file_limit);
    if (!replace_file(path, entries))
      std::fprintf(stderr, "dax: cannot write history file '%s': %s\n", path.c_str(), std::strerror(errno));
    else {
      observed_file_entries[path] = entries.size();
      session_entries_written[path] = session_entries.size();
      store_count(lock, entries.size(), file_size(path));
    }
    return;
  }
  auto entries = read_file(path);
  if (entries.size() > settings.file_limit) {
    entries.erase(entries.begin(), entries.end() - settings.file_limit);
    if (!replace_file(path, entries))
      std::fprintf(stderr, "dax: cannot trim history file '%s': %s\n", path.c_str(), std::strerror(errno));
    else {
      observed_file_entries[path] = entries.size();
      store_count(lock, entries.size(), file_size(path));
    }
  }
}
} // namespace History
