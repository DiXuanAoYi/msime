#include <exception>
#include <fmt/core.h>
#include "tests/includes/test_framework.h"

namespace test
{
std::vector<TestCase> &registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

Registrar::Registrar(const char *name, std::function<void()> fn)
{
    registry().push_back({name, std::move(fn)});
}
} // namespace test

int RunVisionFixtureProbe(const char *path, bool code);
int RunVisionCodeJsonFixtureProbe(const char *path, const char *expected);
int RunVisionAudioFixtureProbe(const char *path, bool loopback);
int RunAiTaskFixtureProbe(const char *endpoint, const char *reportPath);
int RunCandidateTranslationFixtureProbe(const char *endpoint, const char *reportPath);
int main(int argc, char **argv)
{
    if (argc == 4 && std::string(argv[1]) == "--candidate-translation-fixture")
        return RunCandidateTranslationFixtureProbe(argv[2], argv[3]);
    if (argc == 4 && std::string(argv[1]) == "--ai-task-fixture")
        return RunAiTaskFixtureProbe(argv[2], argv[3]);
    if (argc == 3 && (std::string(argv[1]) == "--vision-fixture" || std::string(argv[1]) == "--vision-code-fixture"))
        return RunVisionFixtureProbe(argv[2], std::string(argv[1]) == "--vision-code-fixture");
    if ((argc == 3 || argc == 4) && std::string(argv[1]) == "--vision-code-json-fixture")
        return RunVisionCodeJsonFixtureProbe(argv[2], argc == 4 ? argv[3] : nullptr);
    if (argc == 3 && std::string(argv[1]) == "--vision-audio-fixture")
        return RunVisionAudioFixtureProbe(argv[2], false);
    if (argc == 4 && std::string(argv[1]) == "--vision-loopback-fixture" &&
        std::string(argv[3]) == "--allow-system-audio")
        return RunVisionAudioFixtureProbe(argv[2], true);
    if (argc >= 2 && std::string(argv[1]) == "--vision-loopback-fixture")
    {
        fmt::print(
            stderr,
            "Controlled loopback requires a fixture path and explicit --allow-system-audio after user confirmation.\n");
        return 2;
    }
    int failures = 0;
    for (const auto &test_case : test::registry())
    {
        try
        {
            test_case.fn();
            fmt::print("[PASS] {}\n", test_case.name);
        }
        catch (const std::exception &ex)
        {
            ++failures;
            fmt::print(stderr, "[FAIL] {}: {}\n", test_case.name, ex.what());
        }
        catch (...)
        {
            ++failures;
            fmt::print(stderr, "[FAIL] {}: unknown exception\n", test_case.name);
        }
    }

    if (failures > 0)
    {
        fmt::print(stderr, "{} test(s) failed\n", failures);
        return 1;
    }

    fmt::print("{} test(s) passed\n", test::registry().size());
    return 0;
}
