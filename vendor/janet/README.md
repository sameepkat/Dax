# Vendored Janet Runtime

This directory contains the official Janet 1.42.1 amalgamated C source and
header, plus Janet's upstream license. The pinned upstream release is
[v1.42.1](https://github.com/janet-lang/janet/releases/tag/v1.42.1).

`janet_runtime.cpp` is Dax's embedding layer; it is maintained outside this
directory. To refresh Janet, obtain the exact upstream release source, generate
the amalgamation using that release's documented build procedure, update
`janet.c`, `janet.h`, and `LICENSE` together, confirm the version macros in
`janet.h`, then build Dax from clean objects without a system Janet install.
Review upstream license changes before accepting a new pin.
