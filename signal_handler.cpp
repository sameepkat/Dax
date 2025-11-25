#include "signal_handler.hpp"
#include <iostream>

void handle_signal(int signum) {
    std::cout << "\nCaught signal " << signum << ", press enter to continue\n";
}
