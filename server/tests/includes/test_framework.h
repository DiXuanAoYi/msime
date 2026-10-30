#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

// This header is shared with the portable test target, which builds on a bare Linux runner with no
// vcpkg tree and no engine headers. Keep it dependency-free; path helpers live in test_utf8_path.h.

namespace test
{
struct TestCase
{
    std::string name;
    std::function<void()> fn;
};

std::vector<TestCase> &registry();

struct Registrar
{
    Registrar(const char *name, std::function<void()> fn);
};

template <typename Left, typename Right> void RequireEqual(const Left &left, const Right &right, const char *expression)
{
    // Compare inside the same full expression as both arguments. A reference
    // returned by make_vector().at(0) otherwise dangles before a later statement.
    if (!(left == right))
        throw std::runtime_error(std::string("Requirement failed: ") + expression);
}
} // namespace test

#define TEST_CASE(name)                                                                                                \
    static void name();                                                                                                \
    static test::Registrar name##_registrar(#name, name);                                                              \
    static void name()

#define REQUIRE(expr)                                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(expr))                                                                                                   \
        {                                                                                                              \
            throw std::runtime_error("Requirement failed: " #expr);                                                    \
        }                                                                                                              \
    } while (false)

#define REQUIRE_EQ(lhs, rhs)                                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        test::RequireEqual((lhs), (rhs), #lhs " == " #rhs);                                                            \
    } while (false)
