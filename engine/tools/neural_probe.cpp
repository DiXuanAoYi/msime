// 神经重排探针：用真实的 neural::SentenceModel / rerank_order 在随包 sentence-model.safetensors 上导出，当 Rust 实现的行为规格。
//
//  load   —— 载入：整份文件、各个截断长度、损坏的头；记成败，成功时记上下文长度、署名、许可、若干字符的 token id。
//  next   —— next_log_probabilities：随机文本，记前 5 名的 (id, 值) 和整个分布的和。
//  score  —— score_sentences：随机前文 × 一批随机句子（含词表外字符、空句、超长句、前文超过窗口）。
//  rerank —— rerank_order：随机句子、静态分、λ、条数上限、前文。
//  last   —— last_characters。
//
// 数值用 %.9g（单精度）/ %.17g（双精度）写出；Rust 侧按容差比较，次序必须一致。
// 用法：msime_neural_probe <sentence-model.safetensors> <输出文件>
#include "neural/neural_decoder.h"
#include "neural/sentence_model.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
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
    double unit() { return static_cast<double>(next()) / 2147483648.0; }
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

std::string number(double value, const char *format)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), format, value);
    return buffer;
}

// 码点转 UTF-8。
std::string utf8(char32_t code)
{
    std::string out;
    if (code < 0x80)
        out += static_cast<char>(code);
    else if (code < 0x800)
    {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
    else if (code < 0x10000)
    {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
    return out;
}

std::vector<char32_t> pool_chars()
{
    // 常用字 + 标点 + 字母数字 + 词表外的字符（生僻字、emoji）。
    const std::u32string common = U"的一是在不了有和人这中大为上个国我以要他时来用们生到作地于出就分对成会可主发年动同工也能下过子说产种面而方后多定行学法所民得经十三之进着等部度家电力里如水化高自二理起小物现实加量都两体制机当使点从业本去把性好应开它合还因由其些然前外天政四日那社义事平形相全表间样与关各重新线内数正心反你明看原又么利比或但质气第向道命此变条只没结解问意建月公无系军很情者最立代想已通并提直题党程展五果料象员革位入常文总次品式活设及管特件长求老头基资边流路级少图山统接知较将组见计别她手角期根论运农指几九区强放决西被干做必战先回则任取据处队南给色光门即保治北造百规热领七海口东导器压志世金增争济阶油思术极交受联什认六共权收证改清己美再采转更单风切打白教速花带安场身车例真务具万每目至达走积示议声报斗完类八离华名确才科张信马节话米整空元况今集温传土许步群广石记需段研界拉林律叫且究观越织装影算低持音众书布复容儿须际商非验连断深难近矿千周委素技备半办青省列习响约支般史感劳便团往酸历市克何除消构府称太准精值号率族维划选标写存候毛亲快效斯院查江型眼王按格养易置派层片始却专状育厂京识适属圆包火住调满县局照参红细引听该铁价严";
    std::vector<char32_t> chars(common.begin(), common.end());
    for (const char32_t extra : std::u32string(U"，。！？、abcXYZ019 𠀀😀龘㐀"))
        chars.push_back(extra);
    return chars;
}

std::string random_text(Lcg &rng, const std::vector<char32_t> &pool, std::uint32_t min_length, std::uint32_t max_length)
{
    const std::uint32_t length = min_length + rng.below(max_length - min_length + 1);
    std::string text;
    for (std::uint32_t i = 0; i < length; ++i)
        text += utf8(pool[rng.below(static_cast<std::uint32_t>(pool.size()))]);
    return text;
}

std::string strings_json(const std::vector<std::string> &items)
{
    std::string json = "[";
    for (std::size_t i = 0; i < items.size(); ++i)
        json += (i ? "," : "") + quote(items[i]);
    return json + "]";
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::cerr << "用法：msime_neural_probe <sentence-model.safetensors> <输出文件>\n";
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty())
    {
        std::cerr << "读不到模型\n";
        return 1;
    }
    std::ofstream out(argv[2], std::ios::binary);
    const auto model = neural::SentenceModel::load(bytes);
    if (!model)
    {
        std::cerr << "模型载入失败\n";
        return 1;
    }
    const auto pool = pool_chars();
    Lcg rng{0x6e657572ULL};

    // ---- load ----
    std::uint64_t header_len = 0;
    for (int i = 0; i < 8; ++i)
        header_len |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
    const std::size_t header_end = 8 + static_cast<std::size_t>(header_len);
    std::string tokens = "[";
    for (std::size_t i = 0; i < pool.size(); ++i)
    {
        const auto id = model->token(pool[i]);
        tokens += (i ? "," : "") + std::to_string(id ? static_cast<long long>(*id) : -1);
    }
    tokens += "]";
    out << "{\"kind\":\"load\",\"what\":\"full\",\"ok\":true,\"context\":" << model->context_length() << ",\"attribution\":" << quote(model->attribution()) << ",\"license\":" << quote(model->license())
        << ",\"tokens\":" << tokens << "}\n";
    const std::vector<std::size_t> cuts = {0, 4, 7, 8, 9, 100, header_end - 1, header_end, header_end + 1, header_end + 100, bytes.size() / 2, bytes.size() - 1};
    for (const std::size_t cut : cuts)
    {
        const std::vector<std::uint8_t> part(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut));
        out << "{\"kind\":\"load\",\"what\":\"cut\",\"length\":" << cut << ",\"ok\":" << (neural::SentenceModel::load(part) ? "true" : "false") << "}\n";
    }
    // 损坏的头：把头里一个字节改掉。
    for (const std::size_t at : {std::size_t{8}, std::size_t{9}, std::size_t{40}, header_end / 2, header_end - 2})
    {
        auto copy = bytes;
        copy[at] = static_cast<std::uint8_t>(copy[at] ^ 0x20);
        out << "{\"kind\":\"load\",\"what\":\"flip\",\"at\":" << at << ",\"ok\":" << (neural::SentenceModel::load(copy) ? "true" : "false") << "}\n";
    }

    // ---- next ----
    for (int i = 0; i < 40; ++i)
    {
        const std::string text = i == 0 ? "" : random_text(rng, pool, 0, i % 7 == 0 ? 90 : 12);
        const auto distribution = model->next_log_probabilities(text);
        std::vector<std::size_t> order(distribution.size());
        for (std::size_t k = 0; k < order.size(); ++k)
            order[k] = k;
        std::partial_sort(order.begin(), order.begin() + 5, order.end(), [&](std::size_t a, std::size_t b) { return distribution[a] != distribution[b] ? distribution[a] > distribution[b] : a < b; });
        std::string top = "[";
        for (int k = 0; k < 5; ++k)
            top += (k ? "," : "") + std::string("[") + std::to_string(order[k]) + "," + number(distribution[order[k]], "%.9g") + "]";
        double total = 0.0;
        for (const float value : distribution)
            total += value;
        out << "{\"kind\":\"next\",\"text\":" << quote(text) << ",\"size\":" << distribution.size() << ",\"top\":" << top << "],\"sum\":" << number(total, "%.12g") << "}\n";
    }

    // ---- score ----
    for (int scenario = 0; scenario < 160; ++scenario)
    {
        const std::string context = rng.below(5) == 0 ? "" : random_text(rng, pool, 0, rng.below(6) == 0 ? 200 : 40);
        std::vector<std::string> texts;
        const std::uint32_t count = rng.below(10) == 0 ? 0 : 1 + rng.below(14);
        for (std::uint32_t i = 0; i < count; ++i)
            texts.push_back(random_text(rng, pool, rng.below(8) == 0 ? 0 : 1, rng.below(10) == 0 ? 160 : 18));
        const auto scores = model->score_sentences(context, texts);
        std::string json = "[";
        for (std::size_t i = 0; i < scores.size(); ++i)
            json += (i ? "," : "") + number(scores[i], "%.17g");
        out << "{\"kind\":\"score\",\"context\":" << quote(context) << ",\"texts\":" << strings_json(texts) << ",\"ret\":" << json << "]}\n";
        // 同一个前文再来一批：命中前文缓存的那条路径。
        if (!texts.empty() && rng.below(3) == 0)
        {
            std::vector<std::string> more;
            for (std::uint32_t i = 0; i < 1 + rng.below(6); ++i)
                more.push_back(random_text(rng, pool, 1, 14));
            const auto again = model->score_sentences(context, more);
            std::string again_json = "[";
            for (std::size_t i = 0; i < again.size(); ++i)
                again_json += (i ? "," : "") + number(again[i], "%.17g");
            out << "{\"kind\":\"score\",\"context\":" << quote(context) << ",\"texts\":" << strings_json(more) << ",\"ret\":" << again_json << "]}\n";
        }
    }

    // ---- rerank ----
    for (int scenario = 0; scenario < 120; ++scenario)
    {
        const std::string context = rng.below(4) == 0 ? "" : random_text(rng, pool, 0, 100);
        std::vector<std::string> sentences;
        std::vector<double> statics;
        const std::uint32_t count = rng.below(12) == 0 ? rng.below(2) : 2 + rng.below(16);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            sentences.push_back(random_text(rng, pool, 2, 10));
            statics.push_back(-3.0 - 12.0 * rng.unit());
        }
        if (rng.below(15) == 0 && !statics.empty())
            statics.pop_back(); // 长度不一致：不重排
        neural::RerankOptions options;
        options.lambda = rng.below(10) == 0 ? 0.0 : (rng.below(3) == 0 ? 1.0 : 0.1 + 0.8 * rng.unit());
        options.max_paths = rng.below(3) == 0 ? 1 + rng.below(6) : 12;
        options.context_chars = rng.below(3) == 0 ? rng.below(8) : 64;
        const auto order = neural::rerank_order(*model, context, sentences, statics, options);
        std::string statics_json = "[";
        for (std::size_t i = 0; i < statics.size(); ++i)
            statics_json += (i ? "," : "") + number(statics[i], "%.17g");
        std::string order_json = "[";
        for (std::size_t i = 0; i < order.size(); ++i)
            order_json += (i ? "," : "") + std::to_string(order[i]);
        out << "{\"kind\":\"rerank\",\"context\":" << quote(context) << ",\"sentences\":" << strings_json(sentences) << ",\"statics\":" << statics_json << "],\"lambda\":" << number(options.lambda, "%.17g")
            << ",\"max_paths\":" << options.max_paths << ",\"context_chars\":" << options.context_chars << ",\"ret\":" << order_json << "]}\n";
    }

    // ---- last_characters ----
    for (int i = 0; i < 60; ++i)
    {
        const std::string text = random_text(rng, pool, 0, 12);
        const std::size_t count = rng.below(16);
        out << "{\"kind\":\"last\",\"text\":" << quote(text) << ",\"count\":" << count << ",\"ret\":" << quote(neural::last_characters(text, count)) << "}\n";
    }
    return 0;
}
