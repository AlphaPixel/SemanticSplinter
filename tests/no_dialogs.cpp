// The test runner never shows Windows assertion or crash dialogs (see src/cli/no_dialogs.hpp).
#include "../src/cli/no_dialogs.hpp"

namespace {
[[maybe_unused]] const bool kNoDialogs = (splinter::cli::no_dialogs(), true);
}
