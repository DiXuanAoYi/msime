// 双拼输入串处理探针：对九套内置方案，用真实的 shuangpin_query / mid_sentence_helpcode 跑随机输入串，
// 导出分段、转全拼、分隔符处理、全码辅助长度、大小写还原、句中辅助码解析和各个“能否接反引号”的判断，
// 当 Rust 实现的行为规格。
//
// 输入串由方案里真能解出音节的两键码拼成，再掺入单键、大写辅助码、反引号段、手动分隔符和杂字符，
// 这样每一条分支都跑得到。全部是 ASCII。
//
// 用法：msime_shuangpin_query_probe [输出文件]
#include "shuangpin/shuangpin_query.h"
#include "shuangpin/shuangpin_utils.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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
    std::uint32_t below(std::uint32_t n) { return next() % n; }
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

std::string lower(std::string text)
{
    for (char &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

template <typename T> std::string list(const std::vector<T> &items)
{
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i)
        out += (i ? "," : "") + std::to_string(items[i]);
    return out + "]";
}

std::string random_input(Lcg &rng, const std::vector<std::string> &codes)
{
    std::string input;
    const std::uint32_t pieces = 1 + rng.below(7);
    for (std::uint32_t i = 0; i < pieces; ++i)
    {
        switch (rng.below(14))
        {
        case 0:
            input += "'";
            break;
        case 1: // 反引号段：`x、`xY、光杆反引号
        {
            input += "`";
            if (rng.below(4))
                input += static_cast<char>((rng.below(2) ? 'a' : 'A') + rng.below(26));
            if (rng.below(2))
                input += static_cast<char>('A' + rng.below(26));
            break;
        }
        case 2: // 大写辅助码
            input += static_cast<char>('A' + rng.below(26));
            if (rng.below(2))
                input += static_cast<char>('A' + rng.below(26));
            break;
        case 3: // 单键
            input += static_cast<char>('a' + rng.below(26));
            break;
        case 4: // 大写码
        {
            std::string code = codes[rng.below(static_cast<std::uint32_t>(codes.size()))];
            code[rng.below(2)] = static_cast<char>(std::toupper(static_cast<unsigned char>(code[0])));
            input += code;
            break;
        }
        case 5: // 杂字符
            input += std::string("1;-,.")[rng.below(5)];
            break;
        default:
            input += codes[rng.below(static_cast<std::uint32_t>(codes.size()))];
            break;
        }
    }
    return input;
}
} // namespace

int main(int argc, char **argv)
{
    std::ofstream file;
    if (argc > 1)
        file.open(argv[1], std::ios::binary);
    std::ostream &out = argc > 1 ? static_cast<std::ostream &>(file) : std::cout;

    static const char *names[] = {"xiaohe", "ziranma", "shoudao", "microsoft", "sogou", "ziguang", "zhinengabc", "guobiao", "pinyinjiajia"};
    Lcg rng{0x5409a1c3ULL};
    int rows = 0, with_blocks = 0, active = 0;
    for (const char *name : names)
    {
        const ShuangpinProfile &profile = GetShuangpinProfile(name);
        std::vector<std::string> codes;
        const std::string keys = "abcdefghijklmnopqrstuvwxyz;";
        for (const char a : keys)
            for (const char b : keys)
            {
                const std::string code{a, b};
                if (a != ';' && ShuangpinUtil::is_accepted_syllable_code(code, profile))
                    codes.push_back(code);
            }
        for (int scenario = 0; scenario < 260; ++scenario)
        {
            const std::string raw = random_input(rng, codes);
            const bool upper = rng.below(2) == 0;
            const std::string raw_lower = lower(raw);
            const std::string segmented = shuangpin::segment_input(raw, profile);
            const std::size_t effective = shuangpin::effective_input_length(raw);

            std::vector<std::size_t> prefix_lengths, trims_ok;
            std::string trims = "[";
            for (std::size_t k = 0; k <= effective + 1; ++k)
            {
                prefix_lengths.push_back(shuangpin::raw_length_for_effective_prefix(raw, k));
                trims += (k ? "," : "") + quote(shuangpin::trim_trailing_letters_preserve_delimiters(raw, k));
            }
            trims += "]";

            const auto parsed = shuangpin::parse_mid_sentence_helpcodes(raw, profile, upper);
            std::string helpcodes = "[", decorations = "[";
            for (std::size_t i = 0; i < parsed.helpcodes.size(); ++i)
                helpcodes += std::string(i ? "," : "") + "[" + std::to_string(parsed.helpcodes[i].syllable) + "," +
                             std::to_string(static_cast<int>(parsed.helpcodes[i].first)) + "," + std::to_string(static_cast<int>(parsed.helpcodes[i].second)) + "]";
            for (std::size_t i = 0; i < parsed.decorations.size(); ++i)
                decorations += std::string(i ? "," : "") + "[" + std::to_string(parsed.decorations[i].first) + "," + quote(parsed.decorations[i].second) + "]";
            helpcodes += "]";
            decorations += "]";
            const std::string parsed_segmentation = shuangpin::segment_input(lower(parsed.input), profile);

            std::vector<int> marker_at, semicolon_at, second_at;
            for (std::size_t caret = 0; caret <= raw.size() + 1; ++caret)
            {
                marker_at.push_back(shuangpin::accepts_mid_sentence_helpcode_marker_at(raw, caret, upper));
                semicolon_at.push_back(FanyImeMidSentenceHelpcode::AcceptsSemicolonFinalAt(raw.data(), raw.size(), caret, upper));
                second_at.push_back(FanyImeMidSentenceHelpcode::AcceptsSecondCodeAt(raw.data(), raw.size(), caret, upper));
            }

            const std::size_t active_length = shuangpin::detect_active_double_helpcode_length(raw_lower, raw, profile);
            active += active_length > 0;
            with_blocks += shuangpin::has_mid_sentence_helpcode(raw, upper);
            ++rows;
            out << "{\"kind\":\"query\",\"profile\":" << quote(name) << ",\"raw\":" << quote(raw) << ",\"upper\":" << (upper ? "true" : "false")
                << ",\"segment\":" << quote(segmented) << ",\"boundaries\":" << list(shuangpin::segment_raw_boundaries(raw, profile))
                << ",\"quanpin_delims\":" << quote(shuangpin::normalize_input_with_delimiters(raw, profile))
                << ",\"normalize\":" << quote(shuangpin::normalize_input(raw, profile)) << ",\"effective\":" << effective
                << ",\"prefix_lengths\":" << list(prefix_lengths) << ",\"trims\":" << trims
                << ",\"complete\":" << (shuangpin::is_complete_input(raw, profile) ? "true" : "false")
                << ",\"complete_lower\":" << (shuangpin::is_complete_input(raw_lower, profile) ? "true" : "false")
                << ",\"active\":" << active_length << ",\"cases\":" << quote(shuangpin::apply_segmentation_cases(shuangpin::segment_input(raw_lower, profile), raw))
                << ",\"has_mid\":" << (shuangpin::has_mid_sentence_helpcode(raw, upper) ? "true" : "false") << ",\"parsed\":{\"input\":" << quote(parsed.input)
                << ",\"source_index\":" << list(parsed.source_index) << ",\"helpcodes\":" << helpcodes << ",\"decorations\":" << decorations << "}"
                << ",\"decorated\":" << quote(shuangpin::decorate_mid_sentence_segmentation(parsed_segmentation, raw, profile, upper))
                << ",\"accepts_marker\":" << (shuangpin::accepts_mid_sentence_helpcode_marker(raw, upper) ? "true" : "false")
                << ",\"marker_at\":" << list(marker_at) << ",\"semicolon_at\":" << list(semicolon_at) << ",\"second_at\":" << list(second_at)
                << ",\"second_code\":" << (shuangpin::accepts_mid_sentence_second_code(raw, 'A', upper) ? "true" : "false")
                << ",\"chunk_start\":" << FanyImeMidSentenceHelpcode::ChunkStart(raw.data(), raw.size(), upper) << "}\n";
        }
    }
    // 辅助码约束的匹配与签名。
    for (const char *code : {"", "e", "ek", "ex", "k", "kk", "ekz"})
        for (const SyllableHelpcode helpcode : {SyllableHelpcode{0, 'e', 0}, SyllableHelpcode{1, 'e', 'k'}, SyllableHelpcode{2, 'k', 'x'}})
            out << "{\"kind\":\"match\",\"code\":" << quote(code) << ",\"syllable\":" << helpcode.syllable << ",\"first\":" << static_cast<int>(helpcode.first)
                << ",\"second\":" << static_cast<int>(helpcode.second) << ",\"matches\":" << (syllable_helpcode_matches(helpcode, code) ? "true" : "false")
                << ",\"signature\":" << quote(syllable_helpcodes_signature({helpcode, SyllableHelpcode{3, 'a', 'b'}})) << "}\n";
    std::cerr << rows << " 条输入串；含句中辅助码段 " << with_blocks << "，处于双辅助码状态 " << active << "\n";
    return 0;
}
