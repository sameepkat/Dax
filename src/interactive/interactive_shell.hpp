#ifndef DAX_INTERACTIVE_SHELL_HPP
#define DAX_INTERACTIVE_SHELL_HPP

#include <string>

void configure_interactive_readline();
int run_interactive_shell();
void print_job_notifications(bool redraw_line = false);
void reset_interactive_renderer();
std::string build_interactive_prompt();
std::string build_continuation_prompt();

#endif
