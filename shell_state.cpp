#include "shell_state.hpp"

ShellState &shell_state() {
  static ShellState state;
  return state;
}
