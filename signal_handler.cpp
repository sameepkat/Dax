#include "signal_handler.hpp"
#include <iostream>
#include <csignal>

void handle_signal(int signum) {
    std::cout << "\nCaught signal " << signum << ", press enter to continue\n";
}

void setup_signals() {
    std::signal(SIGINT, SIG_IGN);
    std::signal(SIGCHLD, SIG_IGN);
}
