#!/usr/bin/env python3
"""Compare shell pattern matching with Bash under the C locale."""
import os
import locale
import subprocess
import sys
import tempfile

binary = os.path.abspath(sys.argv[1])
env = dict(os.environ, LC_ALL="C")
cases = [
    "[[ cat == c?t ]]",
    "[[ cat == [a-c]* ]]",
    "[[ cat == @(cat|dog) ]]",
    "[[ dog == @(cat|dog) ]]",
    "[[ bird == @(cat|dog) ]]",
    "[[ '' == ?(cat|dog) ]]",
    "[[ catdog == *(cat|dog) ]]",
    "[[ catdog == +(cat|dog) ]]",
    "[[ '' == +(cat|dog) ]]",
    "[[ fox == !(cat|dog) ]]",
    "[[ cat == !(cat|dog) ]]",
    "[[ abcd == @(a|@(ab|x))cd ]]",
    "[[ food.txt == !(bar).txt ]]",
    "[[ bar.txt == !(bar).txt ]]",
    "[[ '*' == \\* ]]",
    "shopt -s nocasematch; [[ CAT == @(cat|dog) ]]",
    "shopt -s extglob; case dog in @(cat|dog)) echo yes;; *) echo no;; esac",
    "shopt -s extglob; value=catdog; printf '%s\\n' \"${value##@(cat|dog)}\"",
    "shopt -s extglob; value=catdog; printf '%s\\n' \"${value//@(cat|dog)/X}\"",
    "shopt -s extglob; printf '<%s>\\n' @(cat|dog).txt",
    "shopt -s extglob dotglob; printf '<%s>\\n' @(.hidden|cat.txt)",
    "shopt -s nocaseglob; printf '<%s>\\n' CAT.*",
    "GLOBIGNORE='dog*'; printf '<%s>\\n' *.txt",
    "shopt -s nocasematch; case DOG in dog) echo matched;; *) echo no;; esac",
    "shopt -s extglob; value=catdog; printf '%s\\n' \"${value#@(cat|dog)}\"",
]

with tempfile.TemporaryDirectory(prefix="dax-pattern-") as directory:
    for name in ("cat.txt", "dog.txt", ".hidden"):
        with open(os.path.join(directory, name), "w", encoding="utf-8"):
            pass
    for case in cases:
        command = case + "; printf 'result=%s\\n' \"$?\""
        results = []
        for shell in ("bash", binary):
            args = [shell, "-O", "extglob"] if shell == "bash" else [shell]
            run = subprocess.run(args + ["-c", command], cwd=directory, env=env,
                                 capture_output=True, text=True, timeout=5)
            results.append((run.returncode, run.stdout, run.stderr))
        if results[0] != results[1]:
            raise AssertionError(f"{case}\nBash: {results[0]}\nDax:  {results[1]}")

print(f"Pattern matching Bash comparisons passed ({len(cases)} cases)")

try:
    locale.setlocale(locale.LC_ALL, "cs_CZ.UTF-8")
except locale.Error:
    print("Czech UTF-8 locale unavailable; shell collating symbol case skipped")
else:
    locale_env = dict(os.environ, LC_ALL="cs_CZ.UTF-8")
    result = subprocess.run([binary, "-c", "[[ ch == [[.ch.]] ]]"], env=locale_env,
                            capture_output=True, text=True, timeout=5)
    if result.returncode != 0:
        raise AssertionError(f"Czech collating symbol: {result.stderr}")
    print("Czech shell collating symbol case passed")
