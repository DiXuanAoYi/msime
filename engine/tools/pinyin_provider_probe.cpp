// 拼音候选提供者探针：真实的 PinyinCandidateProvider 吃输入方案生成的请求，覆盖全拼与双拼（三套方案）的
// 引擎层：开关下发、全拼末尾大写辅助码、双拼单码/双码辅助、模糊音并入、句中辅助码约束，以及查询之后把云/AI 候选
// 插进缓存列表再查一次、单字母展开、按方案查词。每条记录带请求的输入串和开关，两次查询的结果以条数、哈希、前 8 项记。
//
// 用法：msime_pinyin_provider_probe <资源目录> <工作目录> <输出文件>
#include "core/runtime_paths.h"
#include "providers/pinyin_candidate_provider.h"
#include "schemes/quanpin_scheme.h"
#include "schemes/shuangpin_scheme.h"
#include "shuangpin/shuangpin_profile.h"
#include "shuangpin/shuangpin_utils.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

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

const char *source_name(CandidateSource source)
{
    switch (source)
    {
    case CandidateSource::Database: return "database";
    case CandidateSource::UserDatabase: return "user";
    case CandidateSource::CloudSuggestion: return "cloud";
    case CandidateSource::AiSuggestion: return "ai";
    case CandidateSource::Generated: return "generated";
    case CandidateSource::Fallback: return "fallback";
    default: return "other";
    }
}

std::string item_json(const WordItem &item)
{
    return "[" + quote(item.pinyin) + "," + quote(item.canonical_pinyin) + "," + quote(item.word) + "," + std::to_string(item.weight) + ",\"" + source_name(item.source) + "\"," +
           (item.sentence_association ? "1" : "0") + "," + std::to_string(static_cast<int>(item.scheme)) + "]";
}

std::string list_json(const std::vector<WordItem> &items)
{
    std::string all;
    for (const auto &item : items)
        all += item_json(item) + "\n";
    std::string top = "[";
    for (std::size_t i = 0; i < items.size() && i < 8; ++i)
        top += (i ? "," : "") + item_json(items[i]);
    return "{\"n\":" + std::to_string(items.size()) + ",\"h\":\"" + hash_text(all) + "\",\"top\":" + top + "]}";
}

const std::vector<std::string> kSyllables = {"ni", "hao", "zhong", "guo", "ren", "shi", "wo", "men", "xue", "lv", "nv", "jiu", "yi", "bu", "wen", "ti", "he", "de", "you", "dian", "shang", "hai", "xian", "tian", "an", "e"};

std::string random_quanpin(Lcg &rng, bool helpcode)
{
    std::string input;
    const std::uint32_t pieces = 1 + rng.below(5);
    for (std::uint32_t i = 0; i < pieces; ++i)
    {
        if (i && rng.below(8) == 0)
            input += "'";
        if (rng.below(8) == 0)
            input += static_cast<char>('a' + rng.below(26));
        else
            input += kSyllables[rng.below(static_cast<std::uint32_t>(kSyllables.size()))];
    }
    if (helpcode && rng.below(2) == 0)
    {
        input += static_cast<char>('A' + rng.below(26));
        if (rng.below(2) == 0)
            input += static_cast<char>('A' + rng.below(26));
    }
    return input;
}

std::string random_shuangpin(Lcg &rng, const std::vector<std::string> &codes, bool helpcode)
{
    std::string input;
    const std::uint32_t pieces = 1 + rng.below(5);
    for (std::uint32_t i = 0; i < pieces; ++i)
    {
        if (i && rng.below(8) == 0)
            input += "'";
        if (rng.below(9) == 0)
            input += static_cast<char>('a' + rng.below(26));
        else
            input += codes[rng.below(static_cast<std::uint32_t>(codes.size()))];
    }
    if (helpcode && rng.below(2) == 0)
    {
        input += static_cast<char>((rng.below(2) ? 'A' : 'a') + rng.below(26));
        if (rng.below(2) == 0)
            input += static_cast<char>((rng.below(2) ? 'A' : 'a') + rng.below(26));
    }
    return input;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::cerr << "用法：msime_pinyin_provider_probe <资源目录> <工作目录> <输出文件>\n";
        return 2;
    }
    const fs::path resources = fs::absolute(argv[1]);
    const fs::path work = fs::absolute(argv[2]);
    fs::create_directories(work);
    std::ofstream out(argv[3], std::ios::binary);
    Lcg rng{0x7a3f19c5ULL};

    static const char *names[] = {"", "xiaohe", "ziranma", "microsoft"}; // 空 = 全拼
    int cases = 0, with_helpcode = 0, with_fuzzy = 0, with_cloud = 0, expanded_count = 0, nonempty = 0;
    for (const char *name : names)
    {
        const bool quanpin = name[0] == 0;
        const ShuangpinProfile &profile = GetShuangpinProfile(quanpin ? "xiaohe" : name);
        std::vector<std::string> codes;
        const std::string keys = "abcdefghijklmnopqrstuvwxyz;";
        for (const char a : keys)
            for (const char b : keys)
                if (a != ';' && ShuangpinUtil::is_accepted_syllable_code(std::string{a, b}, profile))
                    codes.push_back(std::string{a, b});

        const fs::path user = work / (std::string("user_") + (quanpin ? "quanpin" : name));
        fs::remove_all(user);
        fs::create_directories(user);
        const metasequoia::RuntimePaths paths{resources, user, work / "cache", resources};
        PinyinCandidateProvider provider(profile, paths);
        provider.set_helpcode_keymap(HelpcodeUtils::load_helpcode_keymap(resources, "xiaohe"));

        for (int scenario = 0; scenario < 300; ++scenario)
        {
            const bool helpcode = rng.below(2) == 0;
            const std::string typed = quanpin ? random_quanpin(rng, helpcode) : random_shuangpin(rng, codes, helpcode);
            std::unique_ptr<IInputScheme> scheme;
            if (quanpin)
                scheme = std::make_unique<QuanpinScheme>();
            else
                scheme = std::make_unique<ShuangpinScheme>(profile);
            scheme->set_raw_input(typed, typed);
            QueryRequest request = scheme->build_request();
            request.enable_quanpin_helpcode = quanpin && helpcode;
            request.enable_shuangpin_helpcode = !quanpin && helpcode;
            request.enable_quanpin_autocorrect_transposition = rng.below(4) == 0;
            request.enable_quanpin_autocorrect_neighbor = rng.below(5) == 0;
            request.fuzzy_pinyin = metasequoia::FuzzyPinyinOptions{rng.below(3) == 0 ? rng.below(1u << 11) : 0};
            request.sentence_association.word_lattice = rng.below(3) != 0;
            request.sentence_association.show_next_on_duplicate = rng.below(3) == 0;
            if (!quanpin && rng.below(5) == 0)
            {
                request.enable_mid_sentence_helpcode = true;
                SyllableHelpcode constraint;
                constraint.syllable = rng.below(3);
                constraint.first = static_cast<char>('a' + rng.below(26));
                constraint.second = rng.below(2) ? static_cast<char>('a' + rng.below(26)) : 0;
                request.syllable_helpcodes.push_back(constraint);
            }

            provider.reset_cache();
            const auto first = provider.query(request);
            std::string second = "null";
            std::string rc = "null";
            std::string word_json = "null";
            int source_kind = 0;
            if (!first.empty() && rng.below(3) == 0)
            {
                // 云/AI 候选插进缓存里的列表，不清缓存直接再查一次。
                source_kind = 1 + static_cast<int>(rng.below(3));
                const CandidateSource source = source_kind == 2 ? CandidateSource::AiSuggestion : CandidateSource::CloudSuggestion;
                const std::string word = source_kind == 3 ? first.front().word : std::string("动态") + std::to_string(rng.below(3));
                word_json = quote(word);
                rc = std::to_string(provider.cache_dynamic_candidate_for_request(request, word, source));
                second = list_json(provider.query(request));
                ++with_cloud;
            }
            std::string expanded = "null";
            if (request.raw_input.size() <= 2 && !first.empty())
            {
                auto copy = first;
                const bool changed = provider.expand_initial_candidates(request, copy);
                expanded = std::string("{\"changed\":") + (changed ? "1" : "0") + ",\"list\":" + list_json(copy) + "}";
                expanded_count += changed;
            }
            ++cases;
            nonempty += !first.empty();
            with_helpcode += helpcode;
            with_fuzzy += request.fuzzy_pinyin.rules != 0;
            out << "{\"case\":" << cases << ",\"profile\":" << quote(quanpin ? "" : name) << ",\"typed\":" << quote(typed) << ",\"hc\":" << (helpcode ? 1 : 0)
                << ",\"tr\":" << (request.enable_quanpin_autocorrect_transposition ? 1 : 0) << ",\"nb\":" << (request.enable_quanpin_autocorrect_neighbor ? 1 : 0)
                << ",\"fuzzy\":" << request.fuzzy_pinyin.rules << ",\"lat\":" << (request.sentence_association.word_lattice ? 1 : 0)
                << ",\"next\":" << (request.sentence_association.show_next_on_duplicate ? 1 : 0) << ",\"mid\":" << (request.enable_mid_sentence_helpcode ? 1 : 0);
            if (request.enable_mid_sentence_helpcode)
                out << ",\"constraint\":[" << request.syllable_helpcodes[0].syllable << "," << static_cast<int>(request.syllable_helpcodes[0].first) << ","
                    << static_cast<int>(request.syllable_helpcodes[0].second) << "]";
            out << ",\"first\":" << list_json(first) << ",\"kind\":" << source_kind << ",\"word\":" << word_json << ",\"rc\":" << rc << ",\"second\":" << second << ",\"expanded\":" << expanded << "}\n";
        }
    }
    std::cerr << "拼音提供者：" << cases << " 个，非空 " << nonempty << "，带辅助码 " << with_helpcode << "，模糊音 " << with_fuzzy << "，插动态候选 " << with_cloud << "，展开单字母 " << expanded_count << "\n";
    return 0;
}
