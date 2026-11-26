#pragma once

void handle_signal(int);
void setup_signals();
void begin_readline_input();
bool finish_readline_input();
bool consume_input_sigint();
