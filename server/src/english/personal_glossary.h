#pragma once

#include <map>
#include <string>
#include <string_view>

namespace EnglishIme
{
// An immutable, language-scoped personal glossary. Parse before publishing to
// the lookup worker; no file I/O or parsing belongs on the candidate path.
class PersonalGlossary
{
  public:
    static constexpr size_t kMaxBytes = 65536;
    static constexpr size_t kMaxFieldBytes = 1024;

    // UTF-8 TSV: language<TAB>source<TAB>gloss[<TAB>second gloss].
    // Reject the complete update on an invalid row, preserving the old table.
    bool Parse(std::string_view text);
    std::string Lookup(const std::string &language, const std::string &word) const;
    const std::string &Source() const
    {
        return source_;
    }
    size_t Size() const
    {
        return entries_.size();
    }
    static bool IsLanguage(std::string_view language);

  private:
    std::string source_;
    std::map<std::pair<std::string, std::string>, std::string> entries_;
};
} // namespace EnglishIme
