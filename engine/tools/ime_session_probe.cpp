// 输入会话探针：真实的 ImeSession 吃随机操作序列（按键、整串替换、重置、清缓存、动态候选、单字母展开、方案切换、
// 光标前缀查询、不带句中辅助码的再查），全拼、双拼、五笔（含拼音混输与通配）、日语各一批，每步导出预编辑、
// 请求的主要字段和候选列表。直接辅助码不开（解析器的接线还在 C++ 里）。
//
// 用法：msime_ime_session_probe <资源目录> <工作目录> <输出文件>
#include "core/ime_session.h"
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

struct Key
{
    ImeKeyCode vk;
    ImeCharacter wch;
};

// 偏向能出候选的字母：常见拼音/双拼/五笔码里的字母，掺撇号、反引号、分号、退格。
Key random_key(Lcg &rng)
{
    const std::uint32_t pick = rng.below(40);
    if (pick < 28)
    {
        const ImeKeyCode vk = 'A' + rng.below(26);
        const std::uint32_t form = rng.below(10);
        return {vk, static_cast<ImeCharacter>(form < 7 ? vk + 32 : (form < 9 ? vk : 0))};
    }
    switch (pick)
    {
    case 28:
    case 29:
        return {ImeKey::Backspace, 0};
    case 30:
        return {ImeKey::Apostrophe, u'\''};
    case 31:
    case 32:
        return {ImeKey::Backquote, u'`'};
    case 33:
    case 34:
        return {ImeKey::Semicolon, u';'};
    case 35:
        return {ImeKey::Minus, u'-'};
    case 36:
        return {ImeKey::Return, 0};
    default:
        return {ImeKey::Slash, u'/'};
    }
}

std::string state_json(ImeSession &session)
{
    const QueryRequest &r = session.get_request();
    return std::string("{\"scheme\":") + std::to_string(static_cast<int>(session.current_scheme_type())) + ",\"preedit\":" + quote(session.get_preedit()) + ",\"raw\":" + quote(r.raw_input) +
           ",\"seg\":" + quote(r.segmentation) + ",\"rawseg\":" + quote(r.raw_segmentation) + ",\"valid\":" + (r.valid ? "1" : "0") + ",\"wild\":" + (r.wubi_z_wildcard ? "1" : "0") +
           ",\"complete\":" + (session.wubi_code_is_complete() ? "1" : "0") + ",\"candidates\":" + list_json(session.get_candidates()) + "}";
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::cerr << "用法：msime_ime_session_probe <资源目录> <工作目录> <输出文件>\n";
        return 2;
    }
    const fs::path resources = fs::absolute(argv[1]);
    const fs::path work = fs::absolute(argv[2]);
    fs::create_directories(work);
    std::ofstream out(argv[3], std::ios::binary);
    Lcg rng{0x2d9b47e1ULL};

    int sequences = 0, steps = 0, nonempty = 0, mixed = 0, dynamic = 0, switched = 0;
    const SchemeType schemes[] = {SchemeType::Quanpin, SchemeType::Shuangpin, SchemeType::Wubi, SchemeType::JapaneseRomaji};
    for (int round = 0; round < 480; ++round)
    {
        const SchemeType start = schemes[round % 4];
        const fs::path user = work / ("user_" + std::to_string(round % 4));
        fs::remove_all(user);
        fs::create_directories(user);
        const metasequoia::RuntimePaths paths{resources, user, work / "cache", resources};
        ImeSession session(start, GetXiaoheShuangpinProfile(), paths);
        session.set_helpcode_keymap(HelpcodeUtils::load_helpcode_keymap(resources, "xiaohe"));

        const bool shuangpin_helpcode = rng.below(2) == 0;
        const bool mid = rng.below(2) == 0;
        const bool upper = rng.below(2) == 0;
        const bool quanpin_helpcode = rng.below(2) == 0;
        const unsigned autocorrect = rng.below(3) == 0 ? 3u : 0u;
        const std::uint32_t fuzzy = rng.below(5) == 0 ? rng.below(1u << 11) : 0;
        metasequoia::WubiInputOptions wubi;
        wubi.mixed_pinyin = rng.below(2) == 0;
        wubi.z_wildcard = rng.below(2) == 0;
        SentenceAssociationOptions association;
        association.word_lattice = rng.below(3) != 0;
        session.set_shuangpin_helpcode_enabled(shuangpin_helpcode);
        session.set_mid_sentence_helpcode_enabled(mid);
        session.set_mid_sentence_uppercase_trigger_enabled(upper);
        session.set_quanpin_helpcode_enabled(quanpin_helpcode);
        session.set_quanpin_autocorrect_types(autocorrect);
        session.set_fuzzy_pinyin_options(metasequoia::FuzzyPinyinOptions{fuzzy});
        session.set_sentence_association(association);
        session.set_wubi_input_options(wubi);
        session.set_rescoring_context(rng.below(2) ? "今天" : "");

        out << "{\"round\":" << round << ",\"start\":" << static_cast<int>(start) << ",\"sh\":" << shuangpin_helpcode << ",\"mid\":" << mid << ",\"upper\":" << upper << ",\"qh\":" << quanpin_helpcode
            << ",\"ac\":" << autocorrect << ",\"fuzzy\":" << fuzzy << ",\"mixed\":" << wubi.mixed_pinyin << ",\"wildcard\":" << wubi.z_wildcard << ",\"lat\":" << association.word_lattice
            << ",\"ctx\":" << quote(session.get_request().rescoring_context) << ",\"ops\":[";
        const std::uint32_t length = 3 + rng.below(12);
        for (std::uint32_t i = 0; i < length; ++i)
        {
            if (i)
                out << ",";
            const std::uint32_t pick = rng.below(40);
            std::string extra = "null";
            if (pick < 28)
            {
                const Key key = random_key(rng);
                session.handle_key(key.vk, 0, key.wch);
                out << "{\"op\":\"key\",\"a\":" << key.vk << ",\"b\":" << static_cast<unsigned>(key.wch);
            }
            else if (pick < 31)
            {
                static const char *raws[] = {"nihao", "ni", "zhongguo", "NiH", "a", "wo'men", "ggll", "vf", "u`ab", "kyouto", "ni'hao"};
                const std::string raw = raws[rng.below(11)];
                const std::string cases = rng.below(2) ? raw : std::string{};
                session.replace_active_raw_input(raw, cases);
                out << "{\"op\":\"replace\",\"s\":" << quote(raw) << ",\"c\":" << quote(cases);
            }
            else if (pick < 32)
            {
                session.reset();
                out << "{\"op\":\"reset\"";
            }
            else if (pick < 34)
            {
                session.reset_cache();
                out << "{\"op\":\"reset_cache\"";
            }
            else if (pick < 37)
            {
                const bool cloud = rng.below(2) == 0;
                const std::string word = std::string("动态") + std::to_string(rng.below(2));
                const int rc = session.apply_dynamic_candidate(word, cloud ? CandidateSource::CloudSuggestion : CandidateSource::AiSuggestion);
                extra = std::to_string(rc);
                out << "{\"op\":\"dynamic\",\"cloud\":" << cloud << ",\"w\":" << quote(word);
                ++dynamic;
            }
            else if (pick < 38)
            {
                const bool changed = session.expand_initial_candidates();
                extra = changed ? "1" : "0";
                out << "{\"op\":\"expand\"";
            }
            else if (pick < 39)
            {
                const SchemeType next = schemes[rng.below(4)];
                session.switch_scheme(next);
                out << "{\"op\":\"switch\",\"to\":" << static_cast<int>(next);
                ++switched;
            }
            else
            {
                const std::string preedit = session.get_preedit();
                const std::string prefix = preedit.substr(0, preedit.size() / 2 + rng.below(2));
                out << "{\"op\":\"raw_query\",\"s\":" << quote(prefix);
                extra = list_json(session.query_raw_candidates(prefix, prefix));
                extra = "{\"raw\":" + extra + ",\"plain\":" + list_json(session.query_without_syllable_helpcodes()) + "}";
            }
            out << ",\"extra\":" << extra << ",\"state\":" << state_json(session) << "}";
            ++steps;
            nonempty += !session.get_candidates().empty();
            mixed += session.current_scheme_type() == SchemeType::Wubi && wubi.mixed_pinyin && !session.get_candidates().empty();
        }
        out << "]}\n";
        ++sequences;
    }
    std::cerr << "会话：" << sequences << " 个序列 " << steps << " 步，候选非空 " << nonempty << "，五笔混输非空 " << mixed << "，动态候选 " << dynamic << "，切方案 " << switched << "\n";
    return 0;
}
