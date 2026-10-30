// 候选查询探针：真实的 CandidateQueries 跑本地模式（九种模式 × 方案 × 一批码）和混排（英文、表情、颜文字
// 混进随机候选列表，覆盖云/AI 槽位、去重、各开关组合），每条导出候选列表。
//
// 用法：msime_candidate_queries_probe <资源目录> <工作目录> <输出文件>
#include "core/candidate_queries.h"
#include "core/runtime_paths.h"
#include "shuangpin/shuangpin_profile.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace metasequoia;

namespace
{
struct Lcg
{
    std::uint64_t state;
    std::uint32_t next()
    {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(state >> 33);
    }
    std::uint32_t below(std::uint32_t n)
    {
        return next() % n;
    }
};

std::string quote(const std::string &text)
{
    std::string out = "\"";
    for (const unsigned char c : text)
    {
        if (c == '"' || c == '\\')
            out += std::string("\\") + static_cast<char>(c);
        else if (c < 0x20)
        {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
            out += buffer;
        }
        else
            out += static_cast<char>(c);
    }
    return out + "\"";
}

std::string hash_text(const std::string &text)
{
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const unsigned char c : text)
    {
        hash ^= c;
        hash *= 0x100000001b3ULL;
    }
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

std::string item_json(const WordItem &item)
{
    return "[" + quote(item.pinyin) + "," + quote(item.canonical_pinyin) + "," + quote(item.word) + "," + std::to_string(item.weight) + "," + std::to_string(static_cast<int>(item.source)) + "," +
           std::to_string(item.fixed_position) + "," + std::to_string(static_cast<int>(item.scheme)) + "]";
}

std::string list_json(const std::vector<WordItem> &items)
{
    std::string all;
    for (const auto &item : items)
        all += item_json(item) + "\n";
    std::string top = "[";
    for (std::size_t i = 0; i < items.size() && i < 12; ++i)
        top += (i ? "," : "") + item_json(items[i]);
    return "{\"n\":" + std::to_string(items.size()) + ",\"h\":\"" + hash_text(all) + "\",\"top\":" + top + "]}";
}

std::string diagnostic_json(const std::optional<std::string> &diagnostic)
{
    return diagnostic ? quote(*diagnostic) : "null";
}

template <std::size_t N> const char *pick(Lcg &rng, const char *(&pool)[N])
{
    return pool[rng.below(N)];
}

WordItem make_item(Lcg &rng)
{
    static const char *words[] = {"你好", "世界", "happy", "动态0", "动态1", "😀", "笑", "hello", "smile", "ni", "hao", "(^_^)"};
    static const CandidateSource sources[] = {CandidateSource::Database,   CandidateSource::Database,        CandidateSource::UserDatabase,
                                              CandidateSource::Generated,  CandidateSource::CloudSuggestion, CandidateSource::AiSuggestion,
                                              CandidateSource::Fallback,   CandidateSource::EnglishDictionary};
    WordItem item(std::string(pick(rng, words)), std::string(pick(rng, words)), static_cast<std::int64_t>(rng.below(100)), sources[rng.below(8)]);
    if (rng.below(3) == 0)
        item.canonical_pinyin = "canon";
    item.fixed_position = rng.below(6) == 0 ? 1 + static_cast<int>(rng.below(3)) : 0;
    return item;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::cerr << "用法：msime_candidate_queries_probe <资源目录> <工作目录> <输出文件>\n";
        return 2;
    }
    const fs::path resources = fs::absolute(argv[1]);
    const fs::path work = fs::absolute(argv[2]);
    fs::create_directories(work);
    std::ofstream out(argv[3], std::ios::binary);
    Lcg rng{0x4b1d7e93ULL};
    const RuntimePaths paths{resources, work / "user", work / "cache", resources};
    CandidateQueries queries(paths, GetXiaoheShuangpinProfile());

    const SchemeType schemes[] = {SchemeType::Quanpin, SchemeType::Shuangpin, SchemeType::Wubi, SchemeType::JapaneseRomaji};
    const local_modes::LocalDateTime clocks[] = {{2024, 12, 25, 3, 14, 30, 15}, {2025, 1, 1, 3, 0, 0, 0}, {2026, 2, 28, 6, 23, 59, 59}};

    // ---------- 本地模式 ----------
    struct ModeCodes
    {
        LocalInputMode mode;
        char trigger;
        std::vector<std::string> codes;
    };
    const std::vector<ModeCodes> modes = {
        {LocalInputMode::None, 'x', {"abc", ""}},
        {LocalInputMode::Unicode, 'u', {"+4e2d", "4e2d", "41", "1f600", "zz", "", "110000", "D800", "+", "00041"}},
        {LocalInputMode::DateTime, 't', {"rq", "sj", "xq", "20241225", "12/25", "14:30", "", "xx", "riqi", "2024/12/25"}},
        {LocalInputMode::QuickPhrase, 'k', {"addr", "a", "ph", "xx", "", "school", "Z", "phone", "ad"}},
        {LocalInputMode::Emoji, 'e', {"xiao", "ha", "ai", "wo", "love", "hh", "xl", "ku", "", "a'b", "XIAO", "xiaol"}},
        {LocalInputMode::Kaomoji, 'k', {"xiao", "ha", "ai", "wo", "love", "hh", "xl", "ku", "", "a'b", "kaixin"}},
        {LocalInputMode::SuperJianpin, 'j', {"ni", "nh", "hh", "wm", "zg", "a", "", "nihao", "bsj", "n", "w"}},
        {LocalInputMode::TemporaryEnglish, 'o', {"Hello", "hel", "a", "", "xyz", "happy", "HAPPY", "a'b", "ni", "He", "world"}},
        {LocalInputMode::TemporaryJapanese, 'v', {"nihon", "", "kyouto"}},
    };
    int records = 0, nonempty = 0;
    for (int round = 0; round < 3; ++round)
        for (const auto &entry : modes)
            for (const auto &code : entry.codes)
                for (const SchemeType scheme : schemes)
                {
                    const auto &clock = clocks[rng.below(3)];
                    std::vector<WordItem> engine;
                    const std::uint32_t count = rng.below(4);
                    for (std::uint32_t i = 0; i < count; ++i)
                        engine.push_back(make_item(rng));
                    const std::string preedit = std::string(1, entry.trigger) + code;
                    const auto result = queries.local(entry.mode, preedit, scheme, [&clock] { return clock; }, engine);
                    out << "{\"kind\":\"local\",\"mode\":" << static_cast<int>(entry.mode) << ",\"preedit\":" << quote(preedit) << ",\"scheme\":" << static_cast<int>(scheme)
                        << ",\"clock\":[" << clock.year << "," << clock.month << "," << clock.day << "," << clock.weekday << "," << clock.hour << "," << clock.minute << "," << clock.second
                        << "],\"engine\":[";
                    for (std::size_t i = 0; i < engine.size(); ++i)
                        out << (i ? "," : "") << item_json(engine[i]);
                    out << "],\"candidates\":" << list_json(result.candidates) << ",\"diagnostic\":" << diagnostic_json(result.diagnostic) << "}\n";
                    ++records;
                    nonempty += !result.candidates.empty();
                }

    // ---------- 混排 ----------
    static const char *prefixes[] = {"ni", "xiao", "ha", "hao", "wo", "ai", "love", "hello", "happy", "he", "ok", "cat", "a", "Hello", "hel'lo", "x", "hh", "wx", "kk", "hel", "smile", "xiaol", "ku", "ab", ""};
    int mixed_records = 0, mixed_changed = 0;
    for (int i = 0; i < 1500; ++i)
    {
        std::vector<WordItem> candidates;
        const std::uint32_t count = rng.below(7);
        for (std::uint32_t k = 0; k < count; ++k)
            candidates.push_back(make_item(rng));
        const std::string prefix = pick(rng, prefixes);
        const SchemeType scheme = rng.below(8) == 0 ? schemes[rng.below(4)] : schemes[rng.below(2)];
        EnglishInputOptions english;
        english.mixed_candidates = rng.below(3) != 0;
        english.minimum_prefix = 1 + rng.below(3);
        MixedExpressiveOptions expressive;
        expressive.emoji_candidates = rng.below(2) == 0;
        expressive.kaomoji_candidates = rng.below(2) == 0;
        const bool dedicated = rng.below(12) == 0;
        const LocalInputMode local_mode = rng.below(12) == 0 ? LocalInputMode::Emoji : LocalInputMode::None;
        const auto before = list_json(candidates);
        const auto mixed = queries.mixed(candidates, prefix, scheme, english, expressive, dedicated, local_mode);
        out << "{\"kind\":\"mixed\",\"prefix\":" << quote(prefix) << ",\"scheme\":" << static_cast<int>(scheme) << ",\"english\":[" << english.mixed_candidates << "," << english.minimum_prefix
            << "],\"expressive\":[" << expressive.emoji_candidates << "," << expressive.kaomoji_candidates << "],\"dedicated\":" << dedicated << ",\"mode\":" << static_cast<int>(local_mode)
            << ",\"input\":[";
        for (std::size_t k = 0; k < candidates.size(); ++k)
            out << (k ? "," : "") << item_json(candidates[k]);
        out << "],\"candidates\":" << list_json(mixed) << "}\n";
        ++mixed_records;
        mixed_changed += list_json(mixed) != before;
    }
    std::cerr << "候选查询：本地模式 " << records << " 条（非空 " << nonempty << "），混排 " << mixed_records << " 条（有变化 " << mixed_changed << "）\n";
    return 0;
}
