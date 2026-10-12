#include "personal_glossary.h"

#include <utf8.h>
#include <vector>

namespace EnglishIme
{
namespace
{
std::string_view Trim(std::string_view text)
{
    const auto first = text.find_first_not_of(" \r");
    if (first == std::string_view::npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \r") - first + 1);
}
} // namespace

bool PersonalGlossary::IsLanguage(std::string_view language)
{
    return language == "en" || language == "ja" || language == "es" || language == "fr" || language == "de" ||
           language == "ru" || language == "ko" || language == "vi" || language == "th" || language == "it";
}

bool PersonalGlossary::Parse(std::string_view text)
{
    if (text.size() > kMaxBytes || !utf8::is_valid(text.begin(), text.end()))
        return false;
    for (const unsigned char ch : text)
        if ((ch < 0x20 && ch != '\t' && ch != '\r' && ch != '\n') || ch == 0x7f)
            return false;
    const std::string source(text);
    if (text.substr(0, 3) == "\xEF\xBB\xBF")
        text.remove_prefix(3);
    decltype(entries_) parsed;
    while (!text.empty())
    {
        const auto end = text.find('\n');
        auto line = Trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.empty() || line.front() == '#')
            continue;
        std::vector<std::string> fields;
        while (true)
        {
            const auto tab = line.find('\t');
            const auto field = Trim(line.substr(0, tab));
            if (field.empty() || field.size() > kMaxFieldBytes || fields.size() >= 4)
                return false;
            fields.emplace_back(field);
            if (tab == std::string_view::npos)
                break;
            line.remove_prefix(tab + 1);
        }
        if (fields.size() < 3 || !IsLanguage(fields[0]))
            return false;
        auto gloss = fields[2];
        if (fields.size() == 4 && fields[3] != gloss)
            gloss += "; " + fields[3];
        parsed[{fields[0], fields[1]}] = std::move(gloss);
    }
    entries_ = std::move(parsed);
    source_ = source;
    return true;
}

std::string PersonalGlossary::Lookup(const std::string &language, const std::string &word) const
{
    const auto found = entries_.find({language, word});
    return found == entries_.end() ? std::string{} : found->second;
}
} // namespace EnglishIme
