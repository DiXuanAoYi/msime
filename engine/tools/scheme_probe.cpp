// 输入方案探针：全拼、双拼（九套方案 × 直接辅助码/大写触发的开关组合）、五笔（扩展长度/混输/通配的开关组合）、
// 日语罗马字，各自吃一串随机按键（字母大小写、撇号、反引号、斜杠、分号、减号、数字、退格、回车、退出）和
// 偶尔的整串替换 `set_raw_input`，每一步导出方案生成的 `QueryRequest` 和预编辑，当 Rust 实现的行为规格。
//
// 用法：msime_scheme_probe [输出文件]
#include "schemes/japanese_romaji_scheme.h"
#include "schemes/quanpin_scheme.h"
#include "schemes/shuangpin_scheme.h"
#include "schemes/wubi_scheme.h"
#include "shuangpin/shuangpin_profile.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
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

std::string request_json(const IInputScheme &scheme)
{
    const QueryRequest r = scheme.build_request();
    std::string strokes = "[";
    for (std::size_t i = 0; i < r.key_strokes.size(); ++i)
        strokes += (i ? "," : "") + std::string("[") + std::to_string(r.key_strokes[i].vk) + "," + std::to_string(r.key_strokes[i].modifiers_down) + "," +
                   std::to_string(static_cast<unsigned>(r.key_strokes[i].wch)) + "]";
    strokes += "]";
    std::string helpcodes = "[";
    for (std::size_t i = 0; i < r.syllable_helpcodes.size(); ++i)
        helpcodes += (i ? "," : "") + std::string("[") + std::to_string(r.syllable_helpcodes[i].syllable) + "," + std::to_string(static_cast<int>(r.syllable_helpcodes[i].first)) + "," +
                     std::to_string(static_cast<int>(r.syllable_helpcodes[i].second)) + "]";
    helpcodes += "]";
    std::string index = "[";
    for (std::size_t i = 0; i < r.syllable_helpcode_source_index.size(); ++i)
        index += (i ? "," : "") + std::to_string(r.syllable_helpcode_source_index[i]);
    index += "]";
    std::string decorations = "[";
    for (std::size_t i = 0; i < r.syllable_helpcode_decorations.size(); ++i)
        decorations += (i ? "," : "") + std::string("[") + std::to_string(r.syllable_helpcode_decorations[i].first) + "," + quote(r.syllable_helpcode_decorations[i].second) + "]";
    decorations += "]";
    return std::string("{\"scheme\":") + std::to_string(static_cast<int>(r.scheme)) + ",\"raw\":" + quote(r.raw_input) + ",\"cases\":" + quote(r.raw_input_with_cases) +
           ",\"norm\":" + quote(r.normalized_input) + ",\"rawseg\":" + quote(r.raw_segmentation) + ",\"normseg\":" + quote(r.normalized_segmentation) + ",\"seg\":" + quote(r.segmentation) +
           ",\"orig\":" + quote(r.raw_input_with_syllable_helpcodes) + ",\"hc\":" + helpcodes + ",\"layout\":" + (r.has_syllable_helpcode_layout ? "1" : "0") + ",\"index\":" + index +
           ",\"deco\":" + decorations + ",\"wild\":" + (r.wubi_z_wildcard ? "1" : "0") + ",\"valid\":" + (r.valid ? "1" : "0") + ",\"strokes\":" + strokes +
           ",\"preedit\":" + quote(scheme.get_preedit()) + "}";
}

struct Key
{
    ImeKeyCode vk;
    ImeCharacter wch;
};

Key random_key(Lcg &rng)
{
    const std::uint32_t pick = rng.below(40);
    if (pick < 24)
    {
        const ImeKeyCode vk = 'A' + rng.below(26);
        const std::uint32_t form = rng.below(8);
        if (form < 5)
            return {vk, static_cast<ImeCharacter>(vk + 32)}; // 小写
        if (form < 7)
            return {vk, static_cast<ImeCharacter>(vk)}; // 大写
        return {vk, 0}; // 没带字符
    }
    switch (pick)
    {
    case 24:
    case 25:
        return {ImeKey::Backspace, 0};
    case 26:
        return {ImeKey::Apostrophe, u'\''};
    case 27:
        return {ImeKey::Apostrophe, 0};
    case 28:
    case 29:
        return {ImeKey::Backquote, u'`'};
    case 30:
        return {ImeKey::Slash, u'/'};
    case 31:
    case 32:
        return {ImeKey::Semicolon, u';'};
    case 33:
        return {ImeKey::Minus, u'-'};
    case 34:
        return {'1' + rng.below(9), static_cast<ImeCharacter>(u'1' + rng.below(9))};
    case 35:
        return {ImeKey::Space, u' '};
    case 36:
        return {ImeKey::Return, 0};
    case 37:
        return {ImeKey::Escape, 0};
    case 38:
        return {ImeKey::Semicolon, 0};
    default:
        return {ImeKey::Slash, 0};
    }
}

std::string random_raw(Lcg &rng)
{
    static const std::string alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'`;/-1 ";
    std::string raw;
    const std::uint32_t length = rng.below(12);
    for (std::uint32_t i = 0; i < length; ++i)
        raw += alphabet[rng.below(static_cast<std::uint32_t>(alphabet.size()))];
    return raw;
}

int run(std::ostream &out, Lcg &rng, const std::string &config, IInputScheme &scheme, int sequences)
{
    int records = 0;
    for (int s = 0; s < sequences; ++s)
    {
        scheme.reset();
        out << "{\"config\":" << quote(config) << ",\"steps\":[";
        const std::uint32_t length = 1 + rng.below(14);
        for (std::uint32_t i = 0; i < length; ++i)
        {
            if (i)
                out << ",";
            if (rng.below(12) == 0)
            {
                const std::string raw = random_raw(rng);
                const std::string cases = rng.below(2) ? random_raw(rng) : std::string{};
                scheme.set_raw_input(raw, cases);
                out << "{\"set\":[" << quote(raw) << "," << quote(cases) << "],\"state\":" << request_json(scheme) << "}";
            }
            else
            {
                const Key key = random_key(rng);
                const ImeModifierMask modifiers = rng.below(4) == 0 ? 0x10 : 0;
                scheme.handle_key(key.vk, modifiers, key.wch);
                out << "{\"key\":[" << key.vk << "," << static_cast<unsigned>(key.wch) << "," << modifiers << "],\"state\":" << request_json(scheme) << "}";
            }
            ++records;
        }
        out << "]}\n";
    }
    return records;
}
} // namespace

int main(int argc, char **argv)
{
    std::ofstream file;
    if (argc > 1)
        file.open(argv[1], std::ios::binary);
    std::ostream &out = argc > 1 ? static_cast<std::ostream &>(file) : std::cout;

    Lcg rng{0x5c4e91d7ULL};
    int steps = 0;
    {
        QuanpinScheme scheme;
        steps += run(out, rng, "quanpin", scheme, 160);
    }
    static const char *names[] = {"xiaohe", "ziranma", "shoudao", "microsoft", "sogou", "ziguang", "zhinengabc", "guobiao", "pinyinjiajia"};
    for (const char *name : names)
    {
        for (int flags = 0; flags < 4; ++flags)
        {
            ShuangpinScheme scheme(GetShuangpinProfile(name));
            scheme.set_direct_helpcode((flags & 1) != 0);
            scheme.set_mid_sentence_uppercase_trigger((flags & 2) != 0);
            steps += run(out, rng, std::string("shuangpin:") + name + ":" + std::to_string(flags), scheme, 40);
        }
    }
    for (int flags = 0; flags < 8; ++flags)
    {
        WubiScheme scheme;
        scheme.set_extended_length_allowed((flags & 1) != 0);
        scheme.set_mixed_pinyin_allowed((flags & 2) != 0);
        scheme.set_z_wildcard((flags & 4) != 0);
        steps += run(out, rng, "wubi:" + std::to_string(flags), scheme, 40);
    }
    {
        JapaneseRomajiScheme scheme;
        steps += run(out, rng, "japanese", scheme, 160);
    }
    std::cerr << "输入方案：" << steps << " 步\n";
    return 0;
}
