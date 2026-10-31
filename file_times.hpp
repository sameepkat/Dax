#ifndef DAX_FILE_TIMES_HPP
#define DAX_FILE_TIMES_HPP

#include <sys/stat.h>

inline int dax_compare_file_mtime(const struct stat &left,
                                 const struct stat &right) {
#if defined(__APPLE__)
  const auto left_seconds = left.st_mtimespec.tv_sec;
  const auto right_seconds = right.st_mtimespec.tv_sec;
  const auto left_nanoseconds = left.st_mtimespec.tv_nsec;
  const auto right_nanoseconds = right.st_mtimespec.tv_nsec;
#else
  const auto left_seconds = left.st_mtim.tv_sec;
  const auto right_seconds = right.st_mtim.tv_sec;
  const auto left_nanoseconds = left.st_mtim.tv_nsec;
  const auto right_nanoseconds = right.st_mtim.tv_nsec;
#endif
  if (left_seconds != right_seconds)
    return left_seconds < right_seconds ? -1 : 1;
  if (left_nanoseconds == right_nanoseconds) return 0;
  return left_nanoseconds < right_nanoseconds ? -1 : 1;
}

inline bool dax_file_modified_since_read(const struct stat &info) {
#if defined(__APPLE__)
  const auto modified_seconds = info.st_mtimespec.tv_sec;
  const auto accessed_seconds = info.st_atimespec.tv_sec;
  const auto modified_nanoseconds = info.st_mtimespec.tv_nsec;
  const auto accessed_nanoseconds = info.st_atimespec.tv_nsec;
#else
  const auto modified_seconds = info.st_mtim.tv_sec;
  const auto accessed_seconds = info.st_atim.tv_sec;
  const auto modified_nanoseconds = info.st_mtim.tv_nsec;
  const auto accessed_nanoseconds = info.st_atim.tv_nsec;
#endif
  return modified_seconds > accessed_seconds ||
         (modified_seconds == accessed_seconds &&
          modified_nanoseconds > accessed_nanoseconds);
}

#endif
