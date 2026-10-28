#include "signal_handler.hpp"
#include <csignal>
#include <cstdio>
#include <readline/readline.h>

namespace {
volatile sig_atomic_t readline_active = 0;
volatile sig_atomic_t readline_interrupted = 0;
const char *readline_primary_prompt = nullptr;
}

void handle_signal(int signum) {
  if (signum != SIGINT || !readline_active)
    return;

  readline_interrupted = 1;
  if (readline_primary_prompt)
    rl_set_prompt(readline_primary_prompt);
  rl_replace_line("", 0);
  rl_crlf();
  rl_on_new_line();
  rl_redisplay();
}

void setup_signals() { std::signal(SIGINT, handle_signal); }

void begin_readline_input(const char *primary_prompt) {
  readline_primary_prompt = primary_prompt;
  readline_interrupted = 0;
  readline_active = 1;
}

bool finish_readline_input() {
  readline_active = 0;
  readline_primary_prompt = nullptr;
  const bool interrupted = readline_interrupted != 0;
  readline_interrupted = 0;
  return interrupted;
}
