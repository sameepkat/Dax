# Dax Shell

Dax is a custom Unix shell implementation written in C++. It provides a familiar command-line interface with support for standard shell features, including command execution, piping, redirection, and built-in commands.

![Dax Shell Screenshot](assets/dax_screenshot_01.png)

## Features

- **Custom Prompt**: Displays the current directory name in green.
- **Command Execution**: Run standard Linux commands (e.g., `ls`, `grep`, `vim`).
- **Piping**: Connect commands using pipes (`|`).
- **Redirection**: Support for input (`<`), output (`>`), and append (`>>`) redirection.
- **Command Lists**: Execute multiple commands sequentially using semicolons (`;`).
- **Shell Variables and Functions**: Basic variable assignment and expansion, aliases, and shell functions with positional parameters and local variables.
- **Background Jobs**: Start a command with `&` and manage basic jobs with `jobs`, `fg`, `bg`, and `wait`.
- **Built-in Commands**:
  - `cd`: Change directory.
  - `pwd`: Print working directory.
  - `echo`: Print arguments to standard output.
  - `export`: Set environment variables.
  - `alias` / `unalias`: Create and remove command aliases.
  - `history`: View command history.
  - `exit`: Exit the shell.
- **History Management**: Persistent command history saved to `~/.dax_history`. Supports arrow key navigation (via `readline`).
- **Signal Handling**: Gracefully handles `SIGINT` (Ctrl+C).

## Installation

### Build

1. Clone the repository:

   ```bash
   git clone https://github.com/sameepkat/Dax.git
   cd Dax
   ```

2. Compile the project using `make`:

   ```bash
   make
   ```

   This will create an executable named `dax`.

## Usage

Start the shell by running the executable:

```bash
./dax
```

### Examples

**Navigation and File Operations:**

```bash
Dax > ls -la
Dax > cd ..
Dax > pwd
```

**Piping and Redirection:**

```bash
Dax > ls | grep cpp > sources.txt
Dax > cat sources.txt
```

**History:**

```bash
Dax > history
Dax > history 10  # Show last 10 commands
```

**Environment Variables:**

```bash
Dax > export MY_VAR=hello
Dax > echo $MY_VAR
```

## Project Structure

- `main.cpp`: Entry point, main loop, and prompt handling.
- `parser.cpp` / `parser.hpp`: AST construction and execution logic.
- `tokenizer.cpp` / `tokenizer.hpp`: Lexical analysis and token generation.
- `builtins.cpp` / `builtins.hpp`: Implementation of built-in shell commands.
- `helper.cpp` / `helper.hpp`: Utility functions.
- `signal_handler.cpp` / `signal_handler.hpp`: Signal handling setup.
- `Makefile`: Build configuration.
