#include <semsplinter/version.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

TEST_CASE("version is MAJOR.MINOR.PATCH") {
    auto v = semsplinter::version();
    REQUIRE(std::count(v.begin(), v.end(), '.') == 2);
    REQUIRE(v.find_first_not_of("0123456789.") == std::string_view::npos);
}
