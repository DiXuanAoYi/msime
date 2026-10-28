// 双拼与辅助码探针：把真实 C++ 实现的行为导成 JSONL，当 Rust 核心的行为规格。
//
// 用法：msime_helpshuang_probe <资源目录> [输出文件]
// 资源目录是放着 helpcodes/ 的目录，仓库里就是 engine/helpcode。
//
// 输出分四类行（kind）：
//   profile  一套内置双拼方案的原始表（声母、零声母、韵母）和它是否把 ; 当韵母键
//   decode   这套方案对所有一到两个字符的码（a-z 和 ;）的解码结果与是否被分段接受
//   segment  这套方案对一批固定种子的随机串的分段、转全拼、全码辅助判断
//   helpcode 一套内置辅助码表，对一批词的取码、单码匹配、双码匹配，以及重排与筛选的结果
// 所有随机都用固定种子的线性同余，重复运行输出逐字节相同。
#include "common/helpcode_utils.h"
#include "core/word_item.h"
#include "shuangpin/shuangpin_profile.h"
#include "shuangpin/shuangpin_utils.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
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

std::string object(const std::unordered_map<std::string, std::string> &map)
{
    const std::map<std::string, std::string> sorted(map.begin(), map.end());
    std::string out = "{";
    bool first = true;
    for (const auto &[key, value] : sorted)
    {
        out += (first ? "" : ",") + quote(key) + ":" + quote(value);
        first = false;
    }
    return out + "}";
}

struct Lcg
{
    std::uint64_t state;
    std::uint32_t next()
    {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<std::uint32_t>(state >> 33);
    }
    std::uint32_t below(std::uint32_t n) { return next() % n; }
};

const std::vector<std::string> kProfileNames = {"xiaohe",  "ziranma",   "shoudao",      "microsoft", "sogou",
                                                "ziguang", "zhinengabc", "guobiao",     "pinyinjiajia"};

void emit_profile(std::ostream &out, const std::string &name)
{
    const ShuangpinProfile &profile = GetShuangpinProfile(name);
    out << "{\"kind\":\"profile\",\"name\":" << quote(name) << ",\"resolved\":" << quote(profile.name)
        << ",\"initials\":" << object(profile.initials) << ",\"zero_initials\":" << object(profile.zero_initials)
        << ",\"finals\":" << object(profile.finals)
        << ",\"semicolon\":" << (ShuangpinProfileUsesSemicolonFinal(profile) ? "true" : "false") << "}\n";

    const std::string alphabet = "abcdefghijklmnopqrstuvwxyz;";
    std::string rows;
    std::vector<std::string> codes;
    for (const char a : alphabet)
    {
        codes.push_back(std::string(1, a));
        for (const char b : alphabet)
            codes.push_back(std::string{a, b});
    }
    for (const auto &code : codes)
    {
        const std::string syllable = ShuangpinUtil::cvt_single_sp_to_pinyin(code, profile);
        const bool accepted = ShuangpinUtil::is_accepted_syllable_code(code, profile);
        if (syllable.empty() && !accepted)
            continue;
        rows += std::string(rows.empty() ? "" : ",") + "[" + quote(code) + "," + quote(syllable) + "," +
                (accepted ? "true" : "false") + "]";
    }
    out << "{\"kind\":\"decode\",\"profile\":" << quote(name) << ",\"rows\":[" << rows << "]}\n";

    // 随机串：合法双拼码拼起来、夹杂噪声字母、末尾带大写的辅助码。
    Lcg rng{0x5eed0001ULL + std::hash<std::string>{}(name) % 997};
    std::vector<std::string> valid;
    for (const auto &code : codes)
        if (code.size() == 2 && ShuangpinUtil::is_accepted_syllable_code(code, profile))
            valid.push_back(code);
    std::set<std::string> inputs = {"", "a", "ni", "nihk", "n", "nh", "ui", "vsvs", "xxxx"};
    for (int i = 0; i < 90; ++i)
    {
        std::string text;
        const int units = 1 + rng.below(5);
        for (int u = 0; u < units; ++u)
        {
            if (rng.below(6) == 0)
                text += static_cast<char>('a' + rng.below(26));
            else
                text += valid[rng.below(static_cast<std::uint32_t>(valid.size()))];
        }
        if (rng.below(3) == 0)
            text += static_cast<char>('A' + rng.below(26));
        if (rng.below(5) == 0)
            text += static_cast<char>('A' + rng.below(26));
        inputs.insert(text);
    }
    std::string segments;
    for (const auto &input : inputs)
    {
        std::string row = "[" + quote(input) + ",";
        row += quote(ShuangpinUtil::pinyin_segmentation(input, profile)) + ",";
        const std::string lowered = [&] {
            std::string copy = input;
            std::transform(copy.begin(), copy.end(), copy.begin(), [](unsigned char c) { return std::tolower(c); });
            return copy;
        }();
        const std::string segmented = ShuangpinUtil::pinyin_segmentation(lowered, profile);
        row += quote(segmented.empty() ? "" : ShuangpinUtil::convert_seg_shuangpin_to_seg_complete_pinyin(segmented, profile)) + ",";
        row += std::string(ShuangpinUtil::IsFullHelpMode(input, profile) ? "true" : "false") + ",";
        row += quote(ShuangpinUtil::GetFullHelpCodes(input)) + "]";
        segments += std::string(segments.empty() ? "" : ",") + row;
    }
    out << "{\"kind\":\"segment\",\"profile\":" << quote(name) << ",\"rows\":[" << segments << "]}\n";
}

char match_letter(HelpcodeUtils::SingleHelpcodeMatch match)
{
    switch (match)
    {
    case HelpcodeUtils::SingleHelpcodeMatch::First:
        return 'F';
    case HelpcodeUtils::SingleHelpcodeMatch::Last:
        return 'L';
    case HelpcodeUtils::SingleHelpcodeMatch::Both:
        return 'B';
    default:
        return 'N';
    }
}

void emit_helpcode(std::ostream &out, const std::filesystem::path &resources, const std::string &schema)
{
    const auto keymap_ptr = HelpcodeUtils::load_helpcode_keymap(resources, schema);
    const auto &keymap = *keymap_ptr;
    std::vector<std::string> keys;
    for (const auto &entry : keymap)
        keys.push_back(entry.first);
    std::sort(keys.begin(), keys.end());
    Lcg rng{0xabc0ffeeULL + schema.size()};

    // 词：单字（等间隔取）、随机两到四字词、含非汉字的混合串、空串。
    std::vector<std::string> words = {"", "a", "abc", "123", "，。"};
    for (std::size_t i = 0; i < keys.size(); i += std::max<std::size_t>(1, keys.size() / 160))
        words.push_back(keys[i]);
    for (int i = 0; i < 100; ++i)
    {
        std::string word;
        const int count = 2 + rng.below(3);
        for (int c = 0; c < count; ++c)
            word += keys[rng.below(static_cast<std::uint32_t>(keys.size()))];
        words.push_back(word);
    }
    words.push_back("a" + keys[0] + "b" + keys[1]);
    words.push_back(keys[2] + "!");
    words.push_back("1" + keys[3]);

    std::string rows;
    for (const auto &word : words)
    {
        std::string singles;
        for (char letter = 'a'; letter <= 'z'; ++letter)
            singles += match_letter(HelpcodeUtils::match_single_helpcode(word, std::string(1, letter), &keymap));
        std::string doubles;
        for (char a = 'a'; a <= 'z'; ++a)
            for (char b = 'a'; b <= 'z'; ++b)
                if (HelpcodeUtils::matches_double_helpcodes(word, std::string{a, b}, &keymap))
                    doubles += std::string(doubles.empty() ? "" : ",") + quote(std::string{a, b});
        rows += std::string(rows.empty() ? "" : ",") + "[" + quote(word) + "," +
                quote(HelpcodeUtils::compute_helpcodes(word, false, &keymap)) + "," +
                quote(HelpcodeUtils::compute_helpcodes(word, true, &keymap)) + "," + quote(singles) + ",[" + doubles +
                "]," + quote(HelpcodeUtils::get_first_han_char(word)) + "," +
                quote(HelpcodeUtils::get_last_han_char(word)) + "," +
                std::to_string(HelpcodeUtils::count_han_chars(word)) + "," +
                std::to_string(HelpcodeUtils::count_utf8_chars(word)) + "]";
    }

    // 重排与筛选：同一列候选（固定 40 个词），按每个单码重排，按挑出的双码筛选。
    std::vector<WordItem> list;
    for (int i = 0; i < 40; ++i)
        list.emplace_back("x", words[5 + (i * 7) % (words.size() - 5)], 100 - i);
    std::string reorders;
    for (char letter = 'a'; letter <= 'z'; ++letter)
    {
        const auto result =
            HelpcodeUtils::reorder_candidates_with_single_helpcode(list, std::string(1, letter), &keymap);
        std::string order;
        for (const auto &item : result)
            order += std::string(order.empty() ? "" : ",") + std::to_string(item.weight);
        reorders += std::string(reorders.empty() ? "" : ",") + "[\"" + std::string(1, letter) + "\",[" + order + "]]";
    }
    std::string filters;
    std::set<std::string> tried;
    for (const auto &word : words)
    {
        for (char a = 'a'; a <= 'z'; ++a)
            for (char b = 'a'; b <= 'z'; ++b)
                if (HelpcodeUtils::matches_double_helpcodes(word, std::string{a, b}, &keymap) &&
                    tried.insert(std::string{a, b}).second && tried.size() <= 24)
                {
                    const auto result = HelpcodeUtils::filter_candidates_with_double_helpcodes(list, std::string{a, b}, &keymap);
                    std::string order;
                    for (const auto &item : result)
                        order += std::string(order.empty() ? "" : ",") + std::to_string(item.weight);
                    filters += std::string(filters.empty() ? "" : ",") + "[" + quote(std::string{a, b}) + ",[" + order + "]]";
                }
    }
    std::string list_words;
    for (const auto &item : list)
        list_words += std::string(list_words.empty() ? "" : ",") + quote(item.word);

    out << "{\"kind\":\"helpcode\",\"schema\":" << quote(schema) << ",\"size\":" << keymap.size()
        << ",\"rows\":[" << rows << "],\"list\":[" << list_words << "],\"reorders\":[" << reorders
        << "],\"filters\":[" << filters << "]}\n";
}

void emit_modes(std::ostream &out)
{
    const std::vector<std::string> samples = {"",     "n",    "ni",   "niH",  "niHA", "nIh", "nihao",  "nihaoH",
                                              "NI",   "niHAo", "A",    "AB",   "ABC",  "niH ", "ni'hao", "ni'haoA"};
    std::string rows;
    for (const auto &sample : samples)
        rows += std::string(rows.empty() ? "" : ",") + "[" + quote(sample) + "," +
                (HelpcodeUtils::is_quanpin_single_help_mode(sample) ? "true" : "false") + "," +
                (HelpcodeUtils::is_quanpin_double_help_mode(sample) ? "true" : "false") + "]";
    out << "{\"kind\":\"modes\",\"rows\":[" << rows << "]}\n";
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::cerr << "用法：msime_helpshuang_probe <资源目录> [输出文件]\n";
        return 2;
    }
    std::ofstream file;
    if (argc > 2)
        file.open(argv[2], std::ios::binary);
    std::ostream &out = argc > 2 ? static_cast<std::ostream &>(file) : std::cout;
    for (const auto &name : kProfileNames)
        emit_profile(out, name);
    for (const char *schema : {"lantian", "ziranma", "shouyou2_0", "shouyouplus", "xiaohe", "jiajia"})
        emit_helpcode(out, argv[1], schema);
    emit_modes(out);
    return 0;
}
