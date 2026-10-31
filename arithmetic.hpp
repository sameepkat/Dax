#ifndef DAX_ARITHMETIC_HPP
#define DAX_ARITHMETIC_HPP

#include <optional>
#include <string>

std::optional<long long> evaluate_arithmetic(const std::string &expression,
                                             std::string *error = nullptr);

#endif
