#ifndef DAX_SHELL_HPP
#define DAX_SHELL_HPP

#include <string>

void execute_line(const std::string &line);
bool shell_input_incomplete(const std::string &source);

#endif
