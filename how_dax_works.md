# How the Dax Shell Works: A Deep Dive

If you've ever typed a command into a terminal, you've used a shell. But have you ever wondered what's happening under the hood? How does the shell understand your commands, run programs, and manage input and output? In this blog post, we'll take a deep dive into the inner workings of a custom Unix shell called Dax. We'll explore its source code to understand how it implements core shell features like command execution, piping, and redirection.

## The Heart of the Shell: The Main Loop

At its core, a shell is a simple program that runs in a loop. In each iteration, it does the following:

1.  **Prints a prompt**: This is to let the user know that the shell is ready to accept a command.
2.  **Reads a line of input**: The shell waits for the user to type a command and press Enter.
3.  **Processes the command**: The shell parses the input, figures out what the user wants to do, and then does it.

The main loop of the Dax shell is in the `main.cpp` file. It uses the `readline` library to provide a rich user experience with features like command history and arrow key navigation.

```cpp
// main.cpp
while (true) {
    // 1. Print a prompt
    char* input = readline(prompt_str.c_str());

    // 2. Read a line of input
    if (!input) {
      std::cout << "\n";
      break;
    }

    line = input;
    free(input);

    if(line.empty()) continue;

    // 3. Process the command
    tokens = tokenize(line);
    if(tokens.empty()) continue;

    try {
        auto ast = parse(tokens);
        if (ast) {
            ast->execute();
        }
    } catch (const std::exception& e) {
        std::cerr << "Parse/Exec Error: " << e.what() << "\n";
    }
}
```

## Breaking It Down: Tokenization

Before the shell can understand a command, it needs to break it down into smaller, more manageable pieces. This process is called **tokenization**. The tokenizer in Dax scans the input string and splits it into a sequence of tokens. For example, the command `ls -l | grep cpp` would be tokenized into `ls`, `-l`, `|`, `grep`, and `cpp`.

The tokenizer in `tokenizer.cpp` is implemented as a state machine. It can handle single quotes, double quotes, and special characters like `|`, `;`, `<`, `>`, and `>>`.

## Making Sense of It All: Parsing and the AST

Once the input has been tokenized, the shell needs to make sense of the tokens. This is where the **parser** comes in. The parser takes the sequence of tokens and builds a data structure called an **Abstract Syntax Tree (AST)**. The AST is a tree representation of the command that captures its structure and meaning.

For example, the command `ls -l | grep cpp` would be parsed into an AST that looks something like this:

```
    Pipe
   /    \
Command  Command
  |        |
ls -l    grep cpp
```

The Dax parser, implemented in `parser.cpp`, is a recursive descent parser. It has functions for parsing simple commands, pipes, and lists of commands. The `parse` function returns a `std::unique_ptr<ASTNode>`, which is the root of the AST.

## Getting Things Done: Command Execution

After the AST has been built, the shell can execute the command by traversing the tree. The `execute` method is called on the root of the AST, which then recursively calls the `execute` method on its children.

### Simple Commands

For a simple command like `ls -l`, the `CommandNode::execute` method is called. This method does the following:

1.  **Handles Redirections**: It sets up any input or output redirections.
2.  **Checks for Built-ins**: It checks if the command is a built-in command (like `cd` or `exit`). If it is, it executes the built-in function directly.
3.  **Forks and Execs**: If the command is not a built-in, it uses the `fork` and `execvp` system calls to run the command in a new process.

```cpp
// CommandNode::execute in parser.cpp
pid_t pid = fork();
if (pid == 0) {
    // Child process
    execvp(c_args[0], c_args.data());
    perror("execvp");
    exit(1);
} else if (pid > 0) {
    // Parent process
    int status;
    waitpid(pid, &status, 0);
}
```

### Pipes

For a command with a pipe, like `ls -l | grep cpp`, the `PipeNode::execute` method is called. This method does the following:

1.  **Creates a pipe**: It uses the `pipe` system call to create a pipe, which is a pair of file descriptors.
2.  **Forks two child processes**: It forks two child processes, one for the command on the left side of the pipe and one for the command on the right.
3.  **Sets up the pipe**: The left child's standard output is redirected to the write end of the pipe, and the right child's standard input is redirected to the read end of the pipe.
4.  **Waits for the children to complete**: The parent process waits for both child processes to finish.

## Bells and Whistles: History and Other Features

In addition to the core features we've discussed, the Dax shell also implements a number of other features, including:

*   **Command History**: The shell keeps a history of the commands that have been entered. The user can navigate through the history using the arrow keys.
*   **Built-in Commands**: The shell provides a number of built-in commands, such as `cd`, `exit`, and `pwd`.
*   **Signal Handling**: The shell handles signals like `SIGINT` (Ctrl+C) gracefully.

## Conclusion

We've taken a deep dive into the inner workings of the Dax shell and explored how it implements core shell features like command execution, piping, and redirection. By examining its source code, we've seen how a shell is a complex program that combines parsing, process management, and system calls to provide a powerful and flexible command-line interface. I hope this has given you a better appreciation for what's happening the next time you type a command into your terminal!
