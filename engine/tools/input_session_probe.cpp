// 输入会话（InputSession）探针：真实的 InputSession 吃随机操作序列——字符、命令、选词键、标点、选词、整句上屏、
// 边缘字上屏、置顶、删词、固定位置、光标、整串替换、重置、清缓存、方案切换、云/AI 候选、选项切换、专用英文——
// 全拼、双拼、五笔（含拼音混输与通配）、日语各一批。每步导出操作结果和会话状态（预编辑、编辑文本、光标、各种切分、
// 候选、云查询状态、在线查询）。
//
// 词库会被学习、造词、删词改写，所以探针先把 msime.db、english.db 拷到工作目录，其余资源硬链接过去；
// 序列之间主库的改动照常累积（Rust 测试从同一份拷贝开始、走同样的序列）。
//
// 用法：msime_input_session_probe <资源目录> <工作目录> <输出文件>
#include "core/input_session.h"
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
           std::to_string(item.fixed_position) + "," + std::to_string(static_cast<int>(item.scheme)) + "," + (item.sentence_association ? "1" : "0") + "]";
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

std::string numbers_json(const std::vector<std::size_t> &values)
{
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i)
        out += (i ? "," : "") + std::to_string(values[i]);
    return out + "]";
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

std::string state_json(InputSession &session)
{
    const auto cloud = session.get_cloud_query_state();
    const auto forms = session.get_shuangpin_preedit_forms();
    return std::string("{\"scheme\":") + std::to_string(static_cast<int>(session.scheme())) + ",\"engine_scheme\":" + std::to_string(static_cast<int>(session.current_scheme_type())) +
           ",\"mode\":" + std::to_string(static_cast<int>(session.local_input_mode())) + ",\"has\":" + (session.has_composition() ? "1" : "0") + ",\"preedit\":" + quote(session.preedit()) +
           ",\"editing\":" + quote(session.editing_text()) + ",\"caret\":" + std::to_string(session.caret_position()) + ",\"rawseg\":" + quote(session.raw_segmentation()) +
           ",\"normseg\":" + quote(session.normalized_segmentation()) + ",\"seq\":" + quote(session.get_pinyin_sequence()) + ",\"cases\":" + quote(session.get_pinyin_sequence_with_cases()) +
           ",\"pure\":" + quote(session.get_pure_pinyin_sequence()) + ",\"seg\":" + quote(session.get_pinyin_segmentation()) + ",\"segcases\":" + quote(session.get_pinyin_segmentation_with_cases()) +
           ",\"forms\":[" + quote(forms.raw) + "," + quote(forms.quanpin) + "],\"quanpin\":" + quote(session.get_quanpin()) + ",\"complete\":" + (session.is_all_complete_pure_pinyin() ? "1" : "0") +
           ",\"reads\":" + (session.reads_as_pinyin() ? "1" : "0") + ",\"wubi_unique\":" + (session.wubi_unique_four_code() ? "1" : "0") + ",\"wubi_complete\":" + (session.wubi_four_code_is_complete() ? "1" : "0") +
           ",\"active_help\":" + (session.has_active_helpcode() ? "1" : "0") + ",\"mid_help\":" + (session.has_mid_sentence_helpcode() ? "1" : "0") + ",\"prefix_end\":" + std::to_string(session.prefix_end()) +
           ",\"pending\":" + quote(session.pending_suffix()) + ",\"bounds\":" + numbers_json(session.segment_raw_boundaries()) + ",\"cloud\":[" + (cloud.should_query ? "1" : "0") + "," + quote(cloud.query_text) + "," +
           quote(cloud.cache_key) + "," + quote(cloud.committed_pinyin) + "],\"online\":" + online_json(session.online_query()) + ",\"candidates\":" + list_json(session.candidates()) + "}";
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

char random_character(Lcg &rng, bool &shift_only, bool idle)
{
    shift_only = false;
    // 没有组合时多给本地模式的触发键（Shift+字母）一些机会。
    if (idle && rng.below(5) == 0)
    {
        static const char triggers[] = "UTKEMJYR";
        shift_only = true;
        return triggers[rng.below(sizeof(triggers) - 1)];
    }
    const std::uint32_t pick = rng.below(100);
    if (pick < 62)
        return static_cast<char>('a' + rng.below(26));
    if (pick < 72)
    {
        // 大写字母：本地模式触发键（shift_only）与辅助码。
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
        std::cerr << "用法：msime_input_session_probe <资源目录> <工作目录> <输出文件>\n";
        return 2;
    }
    const fs::path resources = fs::absolute(argv[1]);
    const fs::path work = fs::absolute(argv[2]);
    fs::remove_all(work);
    fs::create_directories(work);
    const fs::path dictionaries = work / "dicts";
    mirror(resources, dictionaries);
    std::ofstream out(argv[3], std::ios::binary);
    Lcg rng{0x6d2f81a3ULL};

    int sequences = 0, steps = 0, with_composition = 0, committed = 0, local_steps = 0, caret_steps = 0, online_applied = 0;
    const SchemeType schemes[] = {SchemeType::Quanpin, SchemeType::Shuangpin, SchemeType::Wubi, SchemeType::JapaneseRomaji};
    const int rounds = 760;
    for (int round = 0; round < rounds; ++round)
    {
        Lcg &r = rng;
        // 400 轮之后是定向轮：配置偏向学习、调频、辅助码、混输，操作偏向打词、选词、取消、在线候选，专打随机轮碰不到的分支。
        const bool directed = round >= 400;
        // 700 轮之后是长句容错轮：全拼 + 全开纠错，打一串带句中简拼与手误的长句，专打容错仲裁的预编辑与选词消耗。
        const bool fault_round = round >= 700;
        const SchemeType start = fault_round ? SchemeType::Quanpin : schemes[round % 4];
        const fs::path user = work / "user";
        fs::remove_all(user);
        fs::create_directories(user);
        const unsigned autocorrect_draw = r.below(3) == 0 ? 3u : 0u;
        const unsigned autocorrect = fault_round ? (round % 3 == 0 ? 0xdu : 0xfu) : autocorrect_draw;
        const bool chinese_punctuation = r.below(7) != 0;
        const bool learning_draw = r.below(5) != 0;
        const bool learning = learning_draw || directed;
        const RuntimePaths paths{resources, user, work / "cache", dictionaries};
        InputSession session(start, autocorrect, true, chinese_punctuation, learning, paths);
        session.set_helpcode_schema("xiaohe");

        const bool shuangpin_helpcode_draw = r.below(2) == 0;
        const bool mid_draw = r.below(2) == 0;
        const bool upper_draw = r.below(2) == 0;
        const bool quanpin_helpcode_draw = r.below(2) == 0;
        const bool shuangpin_helpcode = shuangpin_helpcode_draw || directed;
        const bool mid = mid_draw || directed;
        const bool upper = upper_draw || directed;
        const bool quanpin_helpcode = quanpin_helpcode_draw || directed;
        const bool direct = start == SchemeType::Shuangpin && r.below(3) == 0;
        const bool slash = r.below(3) != 0;
        const bool uppercase_marker = r.below(2) == 0;
        const std::uint32_t fuzzy = r.below(5) == 0 ? r.below(1u << 11) : 0;
        metasequoia::WubiInputOptions wubi;
        wubi.mixed_pinyin = r.below(2) == 0 || directed;
        wubi.z_wildcard = r.below(2) == 0;
        SentenceAssociationOptions association;
        association.word_lattice = r.below(3) != 0;
        session.set_shuangpin_helpcode_enabled(shuangpin_helpcode);
        session.set_mid_sentence_helpcode_enabled(mid);
        session.set_mid_sentence_uppercase_trigger_enabled(upper);
        session.set_quanpin_helpcode_enabled(quanpin_helpcode);
        session.set_fuzzy_pinyin_options(metasequoia::FuzzyPinyinOptions{fuzzy});
        session.set_sentence_association(association);
        session.set_wubi_input_options(wubi);
        if (direct)
        {
            session.set_direct_helpcode_enabled(true);
            session.set_direct_helpcode_markers(slash, uppercase_marker);
        }
        LocalModeOptions local;
        if (r.below(3) == 0)
        {
            local.unicode = r.below(5) != 0;
            local.date_time = r.below(5) != 0;
            local.quick_phrase = r.below(5) != 0;
            local.quick_phrase_candidates = r.below(4) != 0;
            local.quick_phrase_frequency = r.below(4) != 0;
            local.emoji = r.below(5) != 0;
            local.kaomoji = r.below(5) != 0;
            local.super_jianpin = r.below(5) != 0;
            local.temporary_english = r.below(5) != 0;
            local.temporary_japanese = r.below(5) != 0;
        }
        if (directed)
        {
            local.quick_phrase = true;
            local.quick_phrase_candidates = true;
            local.quick_phrase_frequency = true;
        }
        session.set_local_mode_options(local);
        EnglishInputOptions english;
        english.mixed_candidates = r.below(3) == 0;
        english.minimum_prefix = 1 + r.below(3);
        session.set_english_input_options(english);
        MixedExpressiveOptions expressive;
        expressive.emoji_candidates = r.below(4) == 0;
        expressive.kaomoji_candidates = r.below(4) == 0;
        session.set_mixed_expressive_options(expressive);
        const bool configured_draw = r.below(5) < 3;
        const bool configured = configured_draw || directed;
        FrequencyAdjustmentOptions frequency;
        if (configured)
        {
            frequency.mode = static_cast<FrequencyAdjustmentMode>(r.below(5));
            frequency.trigger_count = 1 + static_cast<int>(r.below(3));
            frequency.linear_step = 1 + static_cast<int>(r.below(3));
            if (directed)
            {
                frequency.mode = static_cast<FrequencyAdjustmentMode>(1 + round % 4);
                frequency.trigger_count = 1;
            }
            session.set_frequency_adjustment(frequency);
        }
        const bool fixed = r.below(5) < 2;
        if (fixed)
            session.enable_fixed_positions();
        const local_modes::LocalDateTime clock{2024, 12, 25, 3, 14, 30, 15};
        session.set_local_date_time_provider([clock] { return clock; });

        out << "{\"round\":" << round << ",\"start\":" << static_cast<int>(start) << ",\"ac\":" << autocorrect << ",\"punct\":" << chinese_punctuation << ",\"learn\":" << learning
            << ",\"sh\":" << shuangpin_helpcode << ",\"mid\":" << mid << ",\"upper\":" << upper << ",\"qh\":" << quanpin_helpcode << ",\"direct\":" << direct << ",\"slash\":" << slash
            << ",\"upm\":" << uppercase_marker << ",\"fuzzy\":" << fuzzy << ",\"mixed\":" << wubi.mixed_pinyin << ",\"wildcard\":" << wubi.z_wildcard << ",\"lat\":" << association.word_lattice
            << ",\"local\":[" << local.unicode << "," << local.date_time << "," << local.quick_phrase << "," << local.quick_phrase_candidates << "," << local.quick_phrase_frequency << ","
            << local.emoji << "," << local.kaomoji << "," << local.super_jianpin << "," << local.temporary_english << "," << local.temporary_japanese << "],\"english\":["
            << english.mixed_candidates << "," << english.minimum_prefix << "],\"expressive\":[" << expressive.emoji_candidates << "," << expressive.kaomoji_candidates
            << "],\"freq\":" << (configured ? "[" + std::to_string(static_cast<int>(frequency.mode)) + "," + std::to_string(frequency.trigger_count) + "," + std::to_string(frequency.linear_step) + "]" : "null")
            << ",\"fixed\":" << fixed << ",\"init\":" << state_json(session) << ",\"ops\":[";

        const std::uint32_t length = directed ? 30 + r.below(40) : 6 + r.below(26);
        std::string pending;
        for (std::uint32_t i = 0; i < length; ++i)
        {
            if (i)
                out << ",";
            std::uint32_t pick = r.below(100);
            bool stale = false, force_cancel = false;
            if (fault_round && i == 0)
            {
                static const char *fault_words[] = {"wozhidaonizaiwaimianduolmangr", "zhegeshurufahenhaoyongwomenyiqichifanb", "jintiantianqihenhaowomenyiqidjiaoxue",
                                                    "shuishanshurfayonghuneicjiaoltiyangqun", "womenyiqikanwdianyingbazhenhaokan", "xianzaizhengzaikaihuiwoyihuirlaizhaoni",
                                                    "tamendoushixuexiaodelaosg", "zhonguorenmindaxuedexueshengmenhenxihuandsh", "woaichizhongguocaidanshizhelichafagnbudui",
                                                    "nihaozhongguoshijiedelaotianyekanbudaon"};
                pending = fault_words[round % (sizeof(fault_words) / sizeof(fault_words[0]))];
            }
            if (directed)
            {
                static const char *words[] = {"nihao", "woshi", "zhongguo", "shurufa", "woaini", "jintiantianqihenhao", "womenyiqichifan", "woxihuanzhongguoderenmin", "niHao", "woAi", "hello", "world", "gggg", "wqvb", "trnt", "ydhy", "kuai", "xiexie", "womenjintianqichifanba", "zhegeshurufahenhaoyong"};
                if (pending.empty())
                {
                    const std::uint32_t category = r.below(100);
                    if (category < 38)
                    {
                        pending = words[r.below(sizeof(words) / sizeof(words[0]))];
                        pick = 0;
                    }
                    else if (category < 56)
                        pick = 62;
                    else if (category < 62)
                        pick = 55;
                    else if (category < 66)
                        pick = 44;
                    else if (category < 70)
                        pick = 92;
                    else if (category < 76)
                        stale = true;
                    else if (category < 80)
                        pick = 95;
                    else if (category < 83)
                        pick = 73;
                    else if (category < 86)
                        pick = 67;
                    else if (category < 94)
                    {
                        pick = 44;
                        force_cancel = true;
                    }
                }
                else
                    pick = 0;
            }
            std::string result = "null";
            const std::size_t count = session.candidates().size();
            std::size_t index = count == 0 ? 0 : (r.below(5) == 0 ? r.below(static_cast<std::uint32_t>(count + 2)) : r.below(std::min<std::uint32_t>(static_cast<std::uint32_t>(count), 6)));
            if (directed && count > 0 && r.below(2) == 0)
                index = 0;
            if (stale)
            {
                // 同一串打字取消后重打，查询的各字段都相同，只有代际变了：旧查询不许再应用。
                const std::string sequence = session.get_pinyin_sequence();
                const auto query = session.online_query();
                session.handle_command(Command::Cancel);
                for (const char typed : sequence)
                    session.handle_character(typed, false);
                const bool applied = query && session.apply_online_candidate(*query, "云9", CandidateSource::CloudSuggestion);
                result = applied ? "1" : "0";
                out << "{\"op\":\"stale_online\",\"s\":" << quote(sequence) << ",\"q\":" << online_json(query);
            }
            else if (pick < 40)
            {
                bool shift_only = false;
                char c;
                if (!pending.empty())
                {
                    c = pending.front();
                    pending.erase(0, 1);
                }
                else
                    c = random_character(r, shift_only, !session.has_composition());
                result = key_result_json(session.handle_character(c, shift_only));
                out << "{\"op\":\"char\",\"c\":" << static_cast<int>(c) << ",\"shift\":" << shift_only;
            }
            else if (pick < 52)
            {
                const auto command = force_cancel ? Command::Cancel : static_cast<Command>(r.below(9));
                result = key_result_json(session.handle_command(command));
                out << "{\"op\":\"command\",\"c\":" << static_cast<int>(command);
            }
            else if (pick < 56)
            {
                const char c = static_cast<char>('0' + r.below(11));
                result = key_result_json(session.handle_candidate_key(c));
                out << "{\"op\":\"candidate_key\",\"c\":" << static_cast<int>(c);
            }
            else if (pick < 60)
            {
                static const char marks[] = ",.?!;:()[]\\`$^_\"'<>a";
                const char c = marks[r.below(sizeof(marks) - 1)];
                result = key_result_json(session.handle_punctuation(c));
                out << "{\"op\":\"punctuation\",\"c\":" << static_cast<int>(c);
            }
            else if (pick < 66)
            {
                result = key_result_json(session.select_candidate(index));
                out << "{\"op\":\"select\",\"i\":" << index;
            }
            else if (pick < 68)
            {
                const std::size_t first = r.below(3) == 0 ? index : 0;
                result = key_result_json(session.finish_composition(first));
                out << "{\"op\":\"finish\",\"i\":" << first;
            }
            else if (pick < 70)
            {
                const std::string word = count > 0 && r.below(4) != 0 ? session.candidates()[index % count].word : std::string("无此词");
                result = key_result_json(session.select_candidate(word));
                out << "{\"op\":\"select_word\",\"w\":" << quote(word);
            }
            else if (pick < 72)
            {
                const CandidateEdge edge = r.below(2) ? CandidateEdge::FirstHan : CandidateEdge::LastHan;
                result = key_result_json(session.select_candidate_edge(index, edge));
                out << "{\"op\":\"edge\",\"i\":" << index << ",\"last\":" << (edge == CandidateEdge::LastHan);
            }
            else if (pick < 74)
            {
                result = key_result_json(session.pin_candidate(index));
                out << "{\"op\":\"pin\",\"i\":" << index;
            }
            else if (pick < 75)
            {
                result = key_result_json(session.remove_candidate(index));
                out << "{\"op\":\"remove\",\"i\":" << index;
            }
            else if (pick < 77)
            {
                const int position = static_cast<int>(r.below(8)) - 1;
                result = key_result_json(session.set_candidate_position(index, position));
                out << "{\"op\":\"position\",\"i\":" << index << ",\"p\":" << position;
            }
            else if (pick < 83)
            {
                const std::size_t size = session.editing_text().size();
                std::optional<std::size_t> caret;
                if (r.below(5) != 0)
                    caret = r.below(static_cast<std::uint32_t>(size + 2));
                session.set_caret(caret);
                session.recompute_candidates();
                out << "{\"op\":\"caret\",\"v\":" << (caret ? std::to_string(*caret) : std::string("-1"));
                ++caret_steps;
            }
            else if (pick < 85)
            {
                static const char *raws[] = {"nihao", "ni", "zhongguo", "NiH", "a", "wo'men", "ggll", "vf", "kyouto", "ni'hao", "abcd", "wqvb"};
                const std::string raw = raws[r.below(12)];
                const bool with_cases = r.below(2) == 0;
                session.set_pinyin_sequence(raw);
                if (with_cases)
                    session.set_pinyin_sequence_with_cases(raw);
                session.recompute_candidates();
                out << "{\"op\":\"sequence\",\"s\":" << quote(raw) << ",\"cases\":" << with_cases;
            }
            else if (pick < 86)
            {
                session.reset_state();
                out << "{\"op\":\"reset_state\"";
            }
            else if (pick < 88)
            {
                session.reset_cache();
                out << "{\"op\":\"reset_cache\"";
            }
            else if (pick < 90)
            {
                const SchemeType next = schemes[r.below(4)];
                session.switch_scheme(next);
                out << "{\"op\":\"switch\",\"to\":" << static_cast<int>(next);
            }
            else if (pick < 94)
            {
                const auto query = session.online_query();
                const bool cloud = r.below(2) == 0;
                const std::string word = std::string(cloud ? "云" : "智") + std::to_string(r.below(3));
                bool applied = false;
                if (query)
                    applied = session.apply_online_candidate(*query, word, cloud ? CandidateSource::CloudSuggestion : CandidateSource::AiSuggestion);
                result = applied ? "1" : "0";
                online_applied += applied;
                out << "{\"op\":\"online\",\"cloud\":" << cloud << ",\"w\":" << quote(word) << ",\"q\":" << online_json(query);
            }
            else if (pick < 95)
            {
                const bool cloud = r.below(2) == 0;
                const std::string word = std::string("缓存") + std::to_string(r.below(2));
                const int rc = session.cache_dynamic_candidate(session.get_pinyin_sequence(), word, cloud ? CandidateSource::CloudSuggestion : CandidateSource::AiSuggestion);
                result = std::to_string(rc);
                out << "{\"op\":\"cache_dynamic\",\"cloud\":" << cloud << ",\"w\":" << quote(word);
            }
            else if (pick < 96)
            {
                const bool enabled = r.below(2) == 0;
                session.set_dedicated_english_mode(enabled);
                out << "{\"op\":\"dedicated\",\"v\":" << enabled;
            }
            else if (pick < 97)
            {
                EnglishInputOptions next;
                next.mixed_candidates = r.below(2) == 0;
                next.minimum_prefix = 1 + r.below(3);
                result = session.set_english_input_options(next) ? "1" : "0";
                MixedExpressiveOptions next_expressive;
                next_expressive.emoji_candidates = r.below(2) == 0;
                next_expressive.kaomoji_candidates = r.below(2) == 0;
                session.set_mixed_expressive_options(next_expressive);
                out << "{\"op\":\"expressive\",\"english\":[" << next.mixed_candidates << "," << next.minimum_prefix << "],\"expressive\":[" << next_expressive.emoji_candidates << "," << next_expressive.kaomoji_candidates << "]";
            }
            else if (pick < 98)
            {
                const bool expanded = session.expand_initial_candidates();
                result = expanded ? "1" : "0";
                out << "{\"op\":\"expand\"";
            }
            else if (pick < 99)
            {
                const std::size_t caret = session.caret_position();
                const char c = static_cast<char>('A' + r.below(26));
                result = std::string("[") + (session.accepts_mid_sentence_helpcode_marker() ? "1" : "0") + "," + (session.accepts_mid_sentence_helpcode_marker_at(caret) ? "1" : "0") + "," +
                         (session.accepts_direct_helpcode_slash_at(caret) ? "1" : "0") + "]";
                out << "{\"op\":\"accepts\",\"c\":" << static_cast<int>(c);
            }
            else
            {
                FrequencyAdjustmentOptions next;
                next.mode = static_cast<FrequencyAdjustmentMode>(r.below(6));
                next.trigger_count = static_cast<int>(r.below(12));
                next.linear_step = static_cast<int>(r.below(12));
                result = session.set_frequency_adjustment(next) ? "1" : "0";
                out << "{\"op\":\"frequency\",\"mode\":" << static_cast<int>(next.mode) << ",\"trigger\":" << next.trigger_count << ",\"step\":" << next.linear_step;
            }
            out << ",\"result\":" << result << ",\"state\":" << state_json(session) << "}";
            ++steps;
            with_composition += session.has_composition();
            local_steps += session.local_input_mode() != LocalInputMode::None;
            committed += result.find("\"c\":\"") != std::string::npos;
        }
        out << "]}\n";
        ++sequences;
    }
    std::cerr << "会话：" << sequences << " 个序列 " << steps << " 步，有组合 " << with_composition << "，本地模式 " << local_steps << "，有上屏 " << committed << "，光标 " << caret_steps << "，在线候选 " << online_applied << "\n";
    return 0;
}
