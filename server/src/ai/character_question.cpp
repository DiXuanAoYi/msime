#include "character_question.h"
#include "engine/core/data_path.h"
#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string_view>
#include <utf8.h>

namespace
{
using Text = std::u32string;
Text Decode(const std::string &text)
{
    Text result;
    if (text.size() <= 12000 && utf8::is_valid(text.begin(), text.end()))
        utf8::utf8to32(text.begin(), text.end(), std::back_inserter(result));
    return result;
}
std::string Encode(const Text &text)
{
    std::string result;
    utf8::utf32to8(text.begin(), text.end(), std::back_inserter(result));
    return result;
}
char32_t Canonical(char32_t value)
{
    static constexpr std::u32string_view groups[] = {U"人亻", U"水氵", U"手扌",     U"心忄", U"言讠", U"金钅",
                                                     U"食饣", U"糸纟", U"艹⺿⺾⻀", U"月⺼", U"阝𨸏"};
    for (const auto &group : groups)
        if (group.find(value) != std::u32string_view::npos)
            return group.front();
    return value;
}
struct Query
{
    Text parts;
    char32_t layout = 0;
};
Query ParseQuery(const std::string &text)
{
    auto input = Decode(text);
    for (auto &ch : input)
        if (ch >= U'A' && ch <= U'Z')
            ch += U'a' - U'A';
    const auto colon = input.find_first_of(U"：:");
    if (colon == 1) // Allow examples such as 草：草字头＋日＋十.
        input.erase(0, colon + 1);
    while (!input.empty() && Text(U" ?？。!！").find(input.back()) != Text::npos)
        input.pop_back();
    for (const Text suffix :
         {U"是什么字", U"是哪个字", U"怎么读", U"是什么", U"什么字", U"shishenmezi", U"zenmedu", U"shishenme"})
        if (input.size() >= suffix.size() && input.compare(input.size() - suffix.size(), suffix.size(), suffix) == 0)
            input.resize(input.size() - suffix.size());
    if (input.size() >= 3 && (input.front() == U'四' || input.front() == U'五' || input.front() == U'六') &&
        input[1] == U'个')
        input = Text(input.front() == U'四' ? 4 : input.front() == U'五' ? 5 : 6, input[2]) + input.substr(3);
    Query query;
    const std::pair<Text, Text> names[] = {
        {U"草字头", U"艹"},     {U"提手旁", U"扌"},      {U"三点水", U"氵"},     {U"两点水", U"冫"},
        {U"竖心旁", U"忄"},     {U"心字底", U"心"},      {U"单人旁", U"亻"},     {U"单立人", U"亻"},
        {U"双人旁", U"彳"},     {U"双立人", U"彳"},      {U"提土旁", U"土"},     {U"言字旁", U"讠"},
        {U"绞丝旁", U"纟"},     {U"金字旁", U"钅"},      {U"食字旁", U"饣"},     {U"反犬旁", U"犭"},
        {U"反文旁", U"攵"},     {U"四点底", U"灬"},      {U"四点水", U"灬"},     {U"宝盖头", U"宀"},
        {U"穴宝盖", U"穴"},     {U"病字头", U"疒"},      {U"病字旁", U"疒"},     {U"走之底", U"辶"},
        {U"走之旁", U"辶"},     {U"双耳旁", U"阝"},      {U"耳刀旁", U"阝"},     {U"示字旁", U"礻"},
        {U"衣字旁", U"衤"},     {U"斜玉旁", U"王"},      {U"禾木旁", U"禾"},     {U"大口框", U"囗"},
        {U"木字旁", U"木"},     {U"口字旁", U"口"},      {U"女字旁", U"女"},     {U"石字旁", U"石"},
        {U"月字旁", U"月"},     {U"虫字旁", U"虫"},      {U"马字旁", U"马"},     {U"目字旁", U"目"},
        {U"caozitou", U"艹"},   {U"sandianshui", U"氵"}, {U"tishoupang", U"扌"}, {U"baogaitou", U"宀"},
        {U"shuxinpang", U"忄"}, {U"kou", U"口"},         {U"mu", U"木"},         {U"ri", U"日"},
        {U"shi", U"十"},        {U"yue", U"月"},         {U"ren", U"人"},        {U"huo", U"火"},
        {U"tu", U"土"},         {U"jin", U"金"},         {U"shui", U"水"},       {U"nv", U"女"},
        {U"zi", U"子"}};
    const Text fillers[] = {U"是什么字",   U"这是什麽字", U"是哪个字", U"怎么读", U"读什么", U"是什么", U"什么字",
                            U"这是什么字", U"请问",       U"再加",     U"加上",   U"一个",   U"一個",   U"shishenmezi",
                            U"zenmedu",    U"shishenme",  U"jiashang", U"yige",   U"zai",    U"jia",    U"结构",
                            U"组成",       U"合起来",     U"组合",     U"个字",   U"和",     U"加",     U"字",
                            U"是",         U"的",         U"？",       U"?",      U"。",     U"！",     U"!",
                            U"+",          U"＋",         U"、",       U"，",     U",",      U" ",      U"\t",
                            U"\n",         U"'"};
    for (size_t pos = 0; pos < input.size();)
    {
        bool found = false;
        const std::pair<Text, char32_t> layouts[] = {{U"左右", U'⿰'}, {U"上下", U'⿱'}, {U"全包围", U'⿴'},
                                                     {U"左边", U'⿰'}, {U"右边", U'⿰'}, {U"上面", U'⿱'},
                                                     {U"下面", U'⿱'}};
        for (const auto &[name, layout] : layouts)
            if (input.compare(pos, name.size(), name) == 0)
            {
                if (query.layout && query.layout != layout)
                    return {}; // Mixed nested prose is left to the text model, not guessed.
                query.layout = layout;
                pos += name.size();
                found = true;
                break;
            }
        if (found)
            continue;
        for (const auto &[name, component] : names)
            if (input.compare(pos, name.size(), name) == 0)
            {
                query.parts += component;
                pos += name.size();
                found = true;
                break;
            }
        if (!found)
            for (const auto &filler : fillers)
                if (input.compare(pos, filler.size(), filler) == 0)
                {
                    pos += filler.size();
                    found = true;
                    break;
                }
        if (!found)
        {
            const char32_t ch = input[pos++];
            if ((ch >= 0x3400 && ch <= 0x9fff) || (ch >= 0x20000 && ch <= 0x323af) || (ch >= 0x2e80 && ch <= 0x2fff))
                query.parts += ch;
            else
                return {};
        }
        if (query.parts.size() > 8)
            return {};
    }
    if (query.parts.size() < 2)
        return {};
    return query;
}
int Arity(char32_t ch)
{
    if (ch == U'⿲' || ch == U'⿳')
        return 3;
    return ch >= U'⿰' && ch <= U'⿻' ? 2 : 0;
}
struct Node
{
    char32_t value;
    std::vector<Node> children;
};
Node ParseNode(const Text &text, size_t &pos, int depth)
{
    if (depth > 16 || pos >= text.size())
        throw std::runtime_error("Invalid IDS tree");
    Node node{text[pos++], {}};
    for (int i = 0; i < Arity(node.value); ++i)
        node.children.push_back(ParseNode(text, pos, depth + 1));
    return node;
}
struct Entry
{
    std::string character, reading;
    std::vector<std::pair<Node, std::string>> variants;
};
struct Catalog
{
    std::map<char32_t, Entry> entries;
    explicit Catalog(const nlohmann::json &json)
    {
        if (!json.is_object() || json.value("version", 0) != 1 || !json.contains("entries") ||
            !json["entries"].is_array())
            return;
        for (const auto &row : json["entries"])
        {
            if (!row.is_array() || row.size() != 3 || !row[0].is_string() || !row[1].is_string() || !row[2].is_array())
                continue;
            const auto character = row[0].get<std::string>();
            const auto codepoints = Decode(character);
            if (codepoints.size() != 1)
                continue;
            Entry entry{character, row[1].get<std::string>(), {}};
            for (const auto &variant : row[2])
            {
                if (!variant.is_string() || entry.variants.size() >= 4)
                    continue;
                const auto ids = variant.get<std::string>();
                const auto sequence = Decode(ids);
                size_t pos = 0;
                try
                {
                    auto node = ParseNode(sequence, pos, 0);
                    if (pos == sequence.size() && !node.children.empty())
                        entry.variants.emplace_back(std::move(node), ids);
                }
                catch (...)
                {
                }
            }
            entries.emplace(codepoints.front(), std::move(entry));
        }
    }
};
using Positions = std::set<size_t>;
Positions Match(const Node &node, const Query &query, size_t pos, const Catalog &catalog, Text &visited, int &budget)
{
    if (pos >= query.parts.size() || --budget <= 0 || visited.size() >= 12)
        return {};
    if (!node.children.empty())
    {
        Positions positions{pos};
        for (const auto &child : node.children)
        {
            Positions next;
            for (const auto current : positions)
            {
                auto matched = Match(child, query, current, catalog, visited, budget);
                next.insert(matched.begin(), matched.end());
            }
            positions = std::move(next);
            if (positions.empty())
                break;
        }
        return positions;
    }
    Positions result;
    if (Canonical(node.value) == Canonical(query.parts[pos]))
        result.insert(pos + 1);
    const auto entry = catalog.entries.find(node.value);
    if (entry != catalog.entries.end() && visited.find(node.value) == Text::npos)
    {
        visited += node.value;
        for (const auto &[variant, _] : entry->second.variants)
        {
            auto matched = Match(variant, query, pos, catalog, visited, budget);
            result.insert(matched.begin(), matched.end());
        }
        visited.pop_back();
    }
    return result;
}
std::vector<AiAssistant::Answer> Lookup(const Query &query, const Catalog &catalog)
{
    std::vector<std::pair<char32_t, AiAssistant::Answer>> matched;
    for (const auto &[character, entry] : catalog.entries)
    {
        for (const auto &[tree, ids] : entry.variants)
        {
            if (query.layout && tree.value != query.layout)
                continue;
            Text visited{character};
            int budget = 256;
            if (Match(tree, query, 0, catalog, visited, budget).count(query.parts.size()))
            {
                std::string components;
                for (const auto part : query.parts)
                {
                    if (!components.empty())
                        components += "＋";
                    components += Encode(Text{part});
                }
                matched.push_back({character,
                                   {entry.character, entry.reading, "本地 IDS 字形表",
                                    entry.character + "＝" + components + "；结构：" + ids}});
                break;
            }
        }
    }
    std::stable_sort(matched.begin(), matched.end(), [](const auto &a, const auto &b) {
        const auto rank = [](const auto &item) {
            return std::pair{item.first >= 0x4e00 && item.first <= 0x9fff ? 0 : 1, item.second.pinyin.empty() ? 1 : 0};
        };
        return rank(a) < rank(b);
    });
    std::vector<AiAssistant::Answer> answers;
    for (auto &[_, answer] : matched)
    {
        answers.push_back(std::move(answer));
        if (answers.size() == 10)
            break;
    }
    return answers;
}
} // namespace
namespace AiAssistant
{
namespace detail
{
std::vector<Answer> LookupComponentQuestion(const std::string &text, const nlohmann::json &catalog)
{
    const auto query = ParseQuery(text);
    if (query.parts.empty())
        return {};
    return Lookup(query, Catalog(catalog));
}
} // namespace detail
std::vector<Answer> LookupComponentQuestion(const std::string &text)
{
    const auto query = ParseQuery(text);
    if (query.parts.empty())
        return {};
    static std::mutex mutex;
    static std::filesystem::path loadedPath;
    static std::shared_ptr<const Catalog> cached;
    std::shared_ptr<const Catalog> catalog;
    {
        std::lock_guard lock(mutex);
        const auto path = metasequoia::data_directory() / "hanzi-question" / "catalog.json";
        if (!cached || loadedPath != path)
        {
            loadedPath = path;
            cached = std::make_shared<Catalog>(nlohmann::json::object());
            std::ifstream file(path, std::ios::binary);
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (file && !error && size <= 16 * 1024 * 1024)
                try
                {
                    cached = std::make_shared<Catalog>(nlohmann::json::parse(file));
                }
                catch (...)
                {
                }
        }
        catalog = cached;
    }
    return Lookup(query, *catalog);
}
} // namespace AiAssistant
