#ifndef DAX_PATTERN_MATCH_HPP
#define DAX_PATTERN_MATCH_HPP

// Options for shell pattern matching; values are independent of libc's flags.
enum DaxPatternFlag {
  DAX_PATTERN_EXTENDED = 1 << 0,
  DAX_PATTERN_CASEFOLD = 1 << 1,
  DAX_PATTERN_PATHNAME = 1 << 2,
  DAX_PATTERN_PERIOD = 1 << 3,
};

// Returns zero for a match and nonzero otherwise.
int dax_pattern_match(const char *pattern, const char *text, int flags);

#endif
