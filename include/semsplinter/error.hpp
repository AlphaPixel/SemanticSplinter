#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace semsplinter {

// SPEC §10, D-11. The CLI maps categories to exit codes (SPEC §8).
enum class ErrorCategory { invalid_input, unsupported, limit, io, internal };

std::string_view to_string(ErrorCategory) noexcept;

class Error : public std::runtime_error {
public:
    Error(ErrorCategory category, const std::string& message)
        : std::runtime_error(message), category_(category) {}
    ErrorCategory category() const noexcept { return category_; }

private:
    ErrorCategory category_;
};

} // namespace semsplinter
