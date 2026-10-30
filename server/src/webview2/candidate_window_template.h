#pragma once

#include <fmt/xchar.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <boost/locale/encoding_utf.hpp>

inline std::wstring BuildCandidateTemplatePayload(const std::wstring &preedit, const std::vector<std::wstring> &items)
{
    nlohmann::json payload = nlohmann::json::array();
    payload.push_back(boost::locale::conv::utf_to_utf<char>(preedit));
    for (const auto &item : items)
        payload.push_back(boost::locale::conv::utf_to_utf<char>(item));
    return boost::locale::conv::utf_to_utf<wchar_t>(payload.dump());
}

// Candidate template arguments use slot 0 for the preedit text and slots 1-9
// for the candidates on the current page.
//
// Candidates are joined with ',' in the payload. Kaomoji (and rarely other
// text) legitimately contain ASCII commas, so the writer escapes them with
// \uF000; split here first and restore the comma afterwards.
inline std::vector<std::wstring> SplitCandidateTemplatePayload(const std::wstring &text)
{
    if (!text.empty() && text.front() == L'[')
    {
        const auto payload = nlohmann::json::parse(boost::locale::conv::utf_to_utf<char>(text), nullptr, false);
        if (payload.is_array() && payload.size() <= 10)
        {
            std::vector<std::wstring> result;
            for (const auto &item : payload)
            {
                if (!item.is_string())
                    return {};
                result.push_back(boost::locale::conv::utf_to_utf<wchar_t>(item.get<std::string>()));
            }
            return result;
        }
    }
    std::wstringstream input(text);
    std::wstring token;
    std::vector<std::wstring> words;
    while (std::getline(input, token, L','))
    {
        std::replace(token.begin(), token.end(), L'\uF000', L',');
        words.push_back(std::move(token));
    }
    return words;
}

inline std::wstring InflateCandidateTemplate(const std::wstring &templ, const std::wstring &text)
{
    std::vector<std::wstring> words = SplitCandidateTemplatePayload(text);

    const int size = static_cast<int>(words.size());
    while (words.size() < 10)
    {
        words.push_back(L"");
    }

    std::wstring result = fmt::format(templ, words[0], words[1], words[2], words[3], words[4], words[5], words[6],
                                      words[7], words[8], words[9]);
    if (size < 10)
    {
        const size_t pos = result.find(fmt::format(L"<!--{}Anchor-->", size));
        result = result.substr(0, pos);
    }
    return result;
}
