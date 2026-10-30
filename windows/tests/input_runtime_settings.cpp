#include "../../engine/contracts/input_runtime_settings.h"
#include "../../engine/contracts/ipc_negotiation.h"
#include <cassert>

int main()
{
    using namespace FanyImeRuntimeSettings;
    for (std::uint32_t flags = 0; flags <= KnownFlags; ++flags)
    {
        const auto result = Decode(Encode({42, flags}));
        assert(result && result->revision == 42 && result->flags == flags);
    }
    for (auto invalid :
         {L"", L"2\t1\t1", L"1\t0\t1", L"1\t-1\t1", L"1\t1\t32", L"1\t1\t1\t0", L"1\t18446744073709551616\t1"})
        assert(!Decode(invalid));
    assert(ShouldApply({2, 0}, 10, 10, 1));
    assert(!ShouldApply({1, EnglishCandidates}, 10, 10, 2));
    assert(!ShouldApply({2, EnglishCandidates}, 10, 10, 2));
    assert(ShouldApply({1, EnglishCandidates}, 11, 10, 100)); // new Server/worker resets revision
    assert(!ShouldApply({2, 0}, 0, 0, 0));
    assert(!ShouldOfferEnglishCandidates(0, false));
    assert(ShouldOfferEnglishCandidates(0, true)); // retire existing raw word through edit session
    assert(ShouldOfferEnglishCandidates(EnglishCandidates, false));
    auto hello = FanyImeProtocol::Hello(42, 99, FanyImeProtocol::Capabilities | FanyImeProtocol::RuntimeSettings);
    auto old = FanyImeProtocol::Negotiate(hello);
    assert(old.accepted && !(old.capabilities & FanyImeProtocol::RuntimeSettings));
    auto current = FanyImeProtocol::Negotiate(hello, FanyImeProtocol::Capabilities | FanyImeProtocol::RuntimeSettings);
    assert(current.accepted && (current.capabilities & FanyImeProtocol::RuntimeSettings));
}
