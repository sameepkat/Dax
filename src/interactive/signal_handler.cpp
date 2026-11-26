#include "signal_handler.hpp"
#include <csignal>
#include <cstdio>
#include <readline/readline.h>

namespace {
volatile sig_atomic_t readline_active = 0;
volatile sig_atomic_t readline_interrupted = 0;
volatile sig_atomic_t input_sigint = 0;
rl_hook_func_t *previous_readline_event_hook = nullptr;

int readline_input_event() {
  if (input_sigint) {
    input_sigint = 0;
    readline_interrupted = 1;
    rl_replace_line("", 0);
    rl_done = 1;
    return 0;
  }
  return previous_readline_event_hook ? previous_readline_event_hook() : 0;
}
}

void handle_signal(int signum) {
  if (signum != SIGINT) return;
  input_sigint = 1;
  if (readline_active) readline_interrupted = 1;
}

void setup_signals() { std::signal(SIGINT, handle_signal); }

void begin_readline_input() {
  readline_interrupted = 0;
  readline_active = 1;
  previous_readline_event_hook = rl_event_hook;
  rl_event_hook = readline_input_event;
}

bool finish_readline_input() {
  readline_active = 0;
  rl_event_hook = previous_readline_event_hook;
  previous_readline_event_hook = nullptr;
  const bool interrupted = readline_interrupted != 0 || input_sigint != 0;
  readline_interrupted = 0;
  input_sigint = 0;
  return interrupted;
}

bool consume_input_sigint() {
  const bool interrupted = input_sigint != 0;
  input_sigint = 0;
  return interrupted;
}
