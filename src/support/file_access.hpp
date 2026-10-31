#ifndef DAX_FILE_ACCESS_HPP
#define DAX_FILE_ACCESS_HPP

#include <fcntl.h>
#include <unistd.h>

inline bool dax_effective_access(const char *path, int mode) {
#ifdef AT_EACCESS
  return faccessat(AT_FDCWD, path, mode, AT_EACCESS) == 0;
#else
  return access(path, mode) == 0;
#endif
}

#endif
