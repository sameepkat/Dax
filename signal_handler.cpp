#include "signal_handler.hpp"
#include <csignal>
#include <iostream>

void handle_signal(int signum) {
  std::cout << "\nCaught signal " << signum << ", press enter to continue\n";
}

void setup_signals() { std::signal(SIGINT, SIG_IGN); }
