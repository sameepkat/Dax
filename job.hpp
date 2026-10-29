#ifndef DAX_JOB_HPP
#define DAX_JOB_HPP

#include <string>

struct Job {
  int id;
  int pid;
  std::string command;
  bool stopped = false;
};

#endif
