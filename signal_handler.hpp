#pragma once

void handle_signal(int);
void setup_signals();
void begin_readline_input(const char *primary_prompt);
bool finish_readline_input();
