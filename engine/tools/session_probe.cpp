// 平台入口 Session（含九键 NineKeySession）探针：真实的 metasequoia::Session 吃随机操作序列——字符（九键开着时偏向数字键
// 2-9）、命令、选词键、标点、选词、边缘字、置顶、删词、固定位置、整句上屏、选九键拼写、开关九键、方案切换、辅助码开关、
// 专用英文、五笔混输、标点开关、云/AI 候选——每步导出操作结果和快照（方案、本地模式、预编辑、切分、候选、专用英文、
// 编辑文本、光标、九键拼写、在线查询）。
//
// 日期时间本地模式读系统时钟，探针关掉它，其余本地模式照开。词库会被学习、调频、置顶改写，所以探针先把 msime.db、
// english.db 拷到工作目录，序列之间主库的改动照常累积（Rust 测试从同一份拷贝开始、走同样的序列）。
//
// 用法：msime_session_probe <资源目录> <工作目录> <输出文件>
#include "include/metasequoia/session.h"

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
           std::to_string(item.fixed_position) + "," + std::to_string(static_cast<int>(item.scheme)) + "," + (item.sentence_association ? "1" : "0") + "," + (item.fuzzy ? "1" : "0") + "]";
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

std::string optional_json(const std::optional<std::string> &value)
{
    return value ? quote(*value) : "null";
}

std::string key_result_json(const KeyResult &result)
{
    return std::string("{\"h\":") + (result.handled ? "1" : "0") + ",\"c\":" + optional_json(result.commit) + ",\"d\":" + optional_json(result.diagnostic) + "}";
}

std::string strings_json(const std::vector<std::string> &values)
{
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i)
        out += (i ? "," : "") + quote(values[i]);
    return out + "]";
}

std::string online_json(const std::optional<OnlineQuery> &query)
{
    if (!query)
        return "null";
    return "{\"scheme\":" + std::to_string(static_cast<int>(query->scheme)) + ",\"identity\":" + quote(query->identity) + ",\"text\":" + quote(query->query_text) + ",\"key\":" + quote(query->cache_key) +
           ",\"segments\":" + strings_json(query->pinyin_segments) + ",\"cloud\":" + (query->cloud_eligible ? "1" : "0") + ",\"ai\":" + (query->ai_eligible ? "1" : "0") + "}";
}

std::string snapshot_json(const Session &session)
{
    const auto view = session.snapshot();
    return std::string("{\"scheme\":") + std::to_string(static_cast<int>(view.scheme)) + ",\"mode\":" + std::to_string(static_cast<int>(view.local_mode)) + ",\"preedit\":" + quote(view.preedit) +
           ",\"rawseg\":" + quote(view.raw_segmentation) + ",\"normseg\":" + quote(view.normalized_segmentation) + ",\"dedicated\":" + (view.dedicated_english ? "1" : "0") +
           ",\"editing\":" + quote(view.editing_text) + ",\"caret\":" + std::to_string(view.caret_position) + ",\"spellings\":" + strings_json(view.nine_key_spellings) +
           ",\"online\":" + online_json(session.online_query()) + ",\"candidates\":" + list_json(view.candidates) + "}";
}

void link_or_copy(const fs::path &from, const fs::path &to, bool force_copy)
{
    if (!force_copy)
    {
        std::error_code error;
        fs::create_hard_link(from, to, error);
        if (!error)
            return;
    }
    fs::copy_file(from, to, fs::copy_options::overwrite_existing);
}

void mirror(const fs::path &from, const fs::path &to)
{
    fs::create_directories(to);
    for (const auto &entry : fs::directory_iterator(from))
    {
        const auto target = to / entry.path().filename();
        if (entry.is_directory())
            mirror(entry.path(), target);
        else if (!fs::exists(target))
        {
            const auto name = entry.path().filename().string();
            link_or_copy(entry.path(), target, name == "msime.db" || name == "english.db");
        }
    }
}

char random_character(Lcg &rng, bool &shift_only, bool nine_key_round)
{
    shift_only = false;
    const std::uint32_t pick = rng.below(100);
    if (nine_key_round && pick < 55)
        return static_cast<char>('2' + rng.below(8));
    if (pick < 72)
        return static_cast<char>('a' + rng.below(26));
    if (pick < 80)
    {
        static const char triggers[] = "UTKEMJYRABXZ";
        const char c = triggers[rng.below(sizeof(triggers) - 1)];
        shift_only = rng.below(4) != 0;
        return c;
    }
    static const char others[] = "'''`/;;+-0123456789:.,";
    return others[rng.below(sizeof(others) - 1)];
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::cerr << "用法：msime_session_probe <资源目录> <工作目录> <输出文件>\n";
        return 2;
    }
    const fs::path resources = fs::absolute(argv[1]);
    const fs::path work = fs::absolute(argv[2]);
    fs::remove_all(work);
    fs::create_directories(work);
    const fs::path dictionaries = work / "dicts";
    mirror(resources, dictionaries);
    std::ofstream out(argv[3], std::ios::binary);
    Lcg rng{0x51ab9d07ULL};

    int sequences = 0, steps = 0, nine_key_steps = 0, committed = 0, nine_candidates = 0, nine_spellings = 0;
    const SchemeType schemes[] = {SchemeType::Quanpin, SchemeType::Shuangpin, SchemeType::Wubi, SchemeType::JapaneseRomaji};
    const int rounds = 420;
    for (int round = 0; round < rounds; ++round)
    {
        Lcg &r = rng;
        // 300 轮之后是九键定向轮：全拼、学习打开、调频生效，长数字串、选词、固定位置、置顶、删词、选拼写都多给机会。
        const bool directed = round >= 300;
        const fs::path user = work / "user";
        fs::remove_all(user);
        fs::create_directories(user);

        SessionOptions options;
        options.paths = RuntimePaths{resources, user, work / "cache", dictionaries};
        // 九键只在全拼下用，一半的序列从全拼开始。
        options.scheme = round % 2 == 0 ? SchemeType::Quanpin : schemes[r.below(4)];
        options.helpcode_schema = "xiaohe";
        options.autocorrect_types = r.below(4) == 0 ? 3u : 0u;
        options.helpcode = r.below(2) == 0;
        options.chinese_punctuation = r.below(7) != 0;
        options.learning = r.below(5) != 0;
        options.fuzzy_pinyin = FuzzyPinyinOptions{r.below(4) == 0 ? r.below(1u << 11) : 0u};
        if (r.below(5) < 4)
        {
            options.frequency.mode = static_cast<FrequencyAdjustmentMode>(r.below(5));
            options.frequency.trigger_count = 1 + static_cast<int>(r.below(3));
            options.frequency.linear_step = 1 + static_cast<int>(r.below(3));
        }
        options.local_modes.unicode = r.below(5) != 0;
        options.local_modes.date_time = false;
        options.local_modes.quick_phrase = r.below(5) != 0;
        options.local_modes.quick_phrase_candidates = r.below(4) != 0;
        options.local_modes.quick_phrase_frequency = r.below(4) != 0;
        options.local_modes.emoji = r.below(5) != 0;
        options.local_modes.kaomoji = r.below(5) != 0;
        options.local_modes.super_jianpin = r.below(5) != 0;
        options.local_modes.temporary_english = r.below(5) != 0;
        options.local_modes.temporary_japanese = r.below(5) != 0;
        options.english.mixed_candidates = r.below(3) == 0;
        options.english.minimum_prefix = 1 + r.below(3);
        options.expressive.emoji_candidates = r.below(4) == 0;
        options.expressive.kaomoji_candidates = r.below(4) == 0;
        options.wubi.mixed_pinyin = r.below(2) == 0;
        options.wubi.z_wildcard = r.below(2) == 0;
        bool nine_key_round = options.scheme == SchemeType::Quanpin && r.below(4) != 0;
        if (directed)
        {
            options.scheme = SchemeType::Quanpin;
            nine_key_round = true;
            options.learning = true;
            options.frequency.mode = static_cast<FrequencyAdjustmentMode>(1 + round % 4);
            options.frequency.trigger_count = 1;
        }

        Session session(options);
        if (nine_key_round)
            session.set_nine_key_enabled(true);

        out << "{\"round\":" << round << ",\"start\":" << static_cast<int>(options.scheme) << ",\"ac\":" << options.autocorrect_types << ",\"help\":" << options.helpcode
            << ",\"punct\":" << options.chinese_punctuation << ",\"learn\":" << options.learning << ",\"fuzzy\":" << options.fuzzy_pinyin.rules << ",\"freq\":["
            << static_cast<int>(options.frequency.mode) << "," << options.frequency.trigger_count << "," << options.frequency.linear_step << "],\"local\":[" << options.local_modes.unicode << ","
            << options.local_modes.quick_phrase << "," << options.local_modes.quick_phrase_candidates << "," << options.local_modes.quick_phrase_frequency << "," << options.local_modes.emoji << ","
            << options.local_modes.kaomoji << "," << options.local_modes.super_jianpin << "," << options.local_modes.temporary_english << "," << options.local_modes.temporary_japanese
            << "],\"english\":[" << options.english.mixed_candidates << "," << options.english.minimum_prefix << "],\"expressive\":[" << options.expressive.emoji_candidates << ","
            << options.expressive.kaomoji_candidates << "],\"wubi\":[" << options.wubi.mixed_pinyin << "," << options.wubi.z_wildcard << "],\"nine\":" << nine_key_round << ",\"init\":"
            << snapshot_json(session) << ",\"ops\":[";

        const std::uint32_t length = directed ? 40 + r.below(30) : 12 + r.below(40);
        std::string pending;
        for (std::uint32_t i = 0; i < length; ++i)
        {
            if (i)
                out << ",";
            std::uint32_t pick = r.below(100);
            bool burst_digit = false;
            if (directed)
            {
                if (!pending.empty())
                    burst_digit = true;
                else
                {
                    const std::uint32_t category = r.below(100);
                    if (category < 30)
                    {
                        const std::size_t burst = r.below(6) == 0 ? 34 : 2 + r.below(8);
                        for (std::size_t k = 0; k < burst; ++k)
                            pending += static_cast<char>('2' + r.below(8));
                        burst_digit = true;
                    }
                    else if (category < 48)
                        pick = 55;
                    else if (category < 56)
                        pick = 70;
                    else if (category < 62)
                        pick = 66;
                    else if (category < 66)
                        pick = 68;
                    else if (category < 72)
                        pick = 76;
                    else if (category < 80)
                        pick = 82;
                    else if (category < 86)
                        pick = 40;
                }
            }
            std::string result = "null";
            const std::size_t count = session.snapshot().candidates.size();
            const std::size_t index = count == 0 ? 0 : (r.below(6) == 0 ? r.below(static_cast<std::uint32_t>(count + 2)) : r.below(std::min<std::uint32_t>(static_cast<std::uint32_t>(count), 6)));
            if (burst_digit)
            {
                const char c = pending.front();
                pending.erase(0, 1);
                result = key_result_json(session.character(c, false));
                out << "{\"op\":\"char\",\"c\":" << static_cast<int>(c) << ",\"shift\":0";
            }
            else if (pick < 38)
            {
                bool shift_only = false;
                const char c = random_character(r, shift_only, nine_key_round);
                result = key_result_json(session.character(c, shift_only));
                out << "{\"op\":\"char\",\"c\":" << static_cast<int>(c) << ",\"shift\":" << shift_only;
            }
            else if (pick < 47)
            {
                const auto command = static_cast<Command>(r.below(9));
                result = key_result_json(session.command(command));
                out << "{\"op\":\"command\",\"c\":" << static_cast<int>(command);
            }
            else if (pick < 51)
            {
                const char c = static_cast<char>('0' + r.below(11));
                result = key_result_json(session.candidate_key(c));
                out << "{\"op\":\"candidate_key\",\"c\":" << static_cast<int>(c);
            }
            else if (pick < 54)
            {
                static const char marks[] = ",.?!;:()[]\\`$^_\"'<>a";
                const char c = marks[r.below(sizeof(marks) - 1)];
                result = key_result_json(session.punctuation(c));
                out << "{\"op\":\"punctuation\",\"c\":" << static_cast<int>(c);
            }
            else if (pick < 62)
            {
                result = key_result_json(session.select(index));
                out << "{\"op\":\"select\",\"i\":" << index;
            }
            else if (pick < 64)
            {
                const CandidateEdge edge = r.below(2) ? CandidateEdge::FirstHan : CandidateEdge::LastHan;
                result = key_result_json(session.select_edge(index, edge));
                out << "{\"op\":\"edge\",\"i\":" << index << ",\"last\":" << (edge == CandidateEdge::LastHan);
            }
            else if (pick < 67)
            {
                result = key_result_json(session.pin(index));
                out << "{\"op\":\"pin\",\"i\":" << index;
            }
            else if (pick < 69)
            {
                result = key_result_json(session.remove(index));
                out << "{\"op\":\"remove\",\"i\":" << index;
            }
            else if (pick < 72)
            {
                const int position = static_cast<int>(r.below(8)) - 1;
                result = key_result_json(session.fix_position(index, position));
                out << "{\"op\":\"fix\",\"i\":" << index << ",\"p\":" << position;
            }
            else if (pick < 74)
            {
                result = key_result_json(session.clear_position(index));
                out << "{\"op\":\"clear\",\"i\":" << index;
            }
            else if (pick < 78)
            {
                const std::size_t first = r.below(3) == 0 ? index : 0;
                result = key_result_json(session.finish(first));
                out << "{\"op\":\"finish\",\"i\":" << first;
            }
            else if (pick < 86)
            {
                const std::size_t spelling = r.below(8);
                result = key_result_json(session.choose_nine_key_spelling(spelling));
                out << "{\"op\":\"spelling\",\"i\":" << spelling;
            }
            else if (pick < 88)
            {
                const bool enabled = r.below(3) != 0;
                session.set_nine_key_enabled(enabled);
                out << "{\"op\":\"nine\",\"v\":" << enabled;
            }
            else if (pick < 90)
            {
                const SchemeType next = schemes[r.below(4)];
                session.switch_scheme(next);
                out << "{\"op\":\"switch\",\"to\":" << static_cast<int>(next);
            }
            else if (pick < 91)
            {
                const bool enabled = r.below(2) == 0;
                session.set_helpcode_enabled(enabled);
                out << "{\"op\":\"helpcode\",\"v\":" << enabled;
            }
            else if (pick < 92)
            {
                const bool enabled = r.below(3) == 0;
                session.set_dedicated_english(enabled);
                out << "{\"op\":\"dedicated\",\"v\":" << enabled;
            }
            else if (pick < 93)
            {
                const bool enabled = r.below(2) == 0;
                session.set_wubi_mixed_pinyin(enabled);
                out << "{\"op\":\"wubi_mixed\",\"v\":" << enabled;
            }
            else if (pick < 94)
            {
                const bool enabled = r.below(2) == 0;
                session.set_chinese_punctuation_enabled(enabled);
                out << "{\"op\":\"punct\",\"v\":" << enabled;
            }
            else
            {
                const auto query = session.online_query();
                const bool cloud = r.below(2) == 0;
                const std::string word = std::string(cloud ? "云" : "智") + std::to_string(r.below(3));
                bool applied = false;
                if (query)
                    applied = session.apply_online_candidate(*query, word, cloud ? CandidateSource::CloudSuggestion : CandidateSource::AiSuggestion);
                result = applied ? "1" : "0";
                out << "{\"op\":\"online\",\"cloud\":" << cloud << ",\"w\":" << quote(word) << ",\"q\":" << online_json(query);
            }
            const auto view = session.snapshot();
            out << ",\"result\":" << result << ",\"state\":" << snapshot_json(session) << "}";
            ++steps;
            const bool nine_active = !view.nine_key_spellings.empty() || (view.scheme == SchemeType::Quanpin && view.editing_text.find_first_not_of("23456789") == std::string::npos && !view.editing_text.empty());
            nine_key_steps += nine_active;
            nine_candidates += nine_active && !view.candidates.empty();
            nine_spellings += !view.nine_key_spellings.empty();
            committed += result.find("\"c\":\"") != std::string::npos;
        }
        out << "]}\n";
        ++sequences;
    }
    std::cerr << "会话门面：" << sequences << " 个序列 " << steps << " 步，九键 " << nine_key_steps << "，九键有候选 " << nine_candidates << "，有拼写 " << nine_spellings << "，有上屏 " << committed << "\n";
    return 0;
}
