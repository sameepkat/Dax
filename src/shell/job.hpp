#ifndef DAX_JOB_HPP
#define DAX_JOB_HPP

#include <string>

struct Job {
  int id;
  int pid;
  std::string command;
  bool stopped = false;
  bool completed = false;
  bool status_change_pending = false;
  bool ignore_hup = false;
  int status = 0;
};

#endif
