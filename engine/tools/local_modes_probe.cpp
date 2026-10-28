// 英文词典与本地模式探针：用真实的 EnglishDictionary / local_modes::* 跑固定种子的输入，当 Rust 实现的行为规格。
//
// 用法：msime_local_modes_probe <msime.db> <others.db> <english.db> <翻译侧车文件> <临时目录> [输出文件]
// 输出 JSONL，kind 有：unicode、vmode、number_words、datetime、datetime_meta、emoji、kaomoji、jianpin、
// qp_fixture（快捷短语合成库的内容）、quick_phrase、place_qp、english_prefix、english_gloss。
// 候选一律写成 [pinyin, canonical_pinyin, word, weight, source, fixed_position]。
#include "english/english_dictionary.h"
#include "local_modes/date_time_query.h"
#include "local_modes/emoji_query.h"
#include "local_modes/jianpin_query.h"
#include "local_modes/kaomoji_query.h"
#include "local_modes/quick_phrase_query.h"
#include "local_modes/unicode_query.h"
#include "local_modes/v_mode_query.h"
#include "shuangpin/shuangpin_profile.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace metasequoia::local_modes;

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

std::string items_json(const std::vector<WordItem> &items)
{
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        const auto &w = items[i];
        out += std::string(i ? "," : "") + "[" + quote(w.pinyin) + "," + quote(w.canonical_pinyin) + "," + quote(w.word) + "," +
               std::to_string(w.weight) + "," + std::to_string(static_cast<int>(w.source)) + "," +
               std::to_string(w.fixed_position) + "]";
    }
    return out + "]";
}

std::string result_json(const LocalQueryResult &result)
{
    return "{\"items\":" + items_json(result.candidates) + ",\"diagnostic\":" +
           (result.diagnostic ? quote(*result.diagnostic) : std::string("null")) + "}";
}

std::vector<std::string> column(sqlite3 *db, const char *sql, std::size_t wanted)
{
    std::vector<std::string> all;
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK)
        return all;
    while (sqlite3_step(statement) == SQLITE_ROW)
        if (const auto *text = reinterpret_cast<const char *>(sqlite3_column_text(statement, 0)))
            all.emplace_back(text);
    sqlite3_finalize(statement);
    std::vector<std::string> picked;
    const std::size_t stride = std::max<std::size_t>(1, all.size() / std::max<std::size_t>(wanted, 1));
    for (std::size_t i = 0; i < all.size() && picked.size() < wanted; i += stride)
        picked.push_back(all[i]);
    return picked;
}

std::string random_letters(Lcg &rng, std::uint32_t min_length, std::uint32_t max_length, const char *alphabet)
{
    const std::size_t size = std::char_traits<char>::length(alphabet);
    std::string text;
    const std::uint32_t length = min_length + rng.below(max_length - min_length + 1);
    for (std::uint32_t i = 0; i < length; ++i)
        text.push_back(alphabet[rng.below(static_cast<std::uint32_t>(size))]);
    return text;
}

// ---------- unicode / v 模式 ----------

void emit_unicode(std::ostream &out)
{
    Lcg rng{0x0c0de001ULL};
    std::vector<std::string> inputs{"", "+", "4e2d", "+4E2D", "1F600", "10FFFF", "110000", "D800", "DFFF", "0", "+0", "g1", "12345678", "+123456", "20 ", "00041", "e"};
    for (int i = 0; i < 300; ++i)
        inputs.push_back((rng.below(3) == 0 ? "+" : "") + random_letters(rng, 0, 7, "0123456789abcdefABCDEFgx"));
    for (const auto &input : inputs)
        out << "{\"kind\":\"unicode\",\"input\":" << quote(input) << ",\"items\":" << items_json(query_unicode(input)) << "}\n";
}

void emit_vmode(std::ostream &out)
{
    Lcg rng{0x57a1e5c0ULL};
    std::vector<std::string> inputs{"0", "00", "123", "12345", "123.45", "123.456", "10", "15", "100", "1000", "10000", "100000000", "10001", "1.5", "0.05", "0.5", ".5",
                                    "5.", "1+2*3", "(1+2)*3", "1/0", "-3", "--3", "2*(3+4", "1/3", "0.1+0.2", "1e5", "1+", "()", "9999999999999999", "99999999999999999", "100000000.01"};
    for (int i = 0; i < 260; ++i)
    {
        std::string text = random_letters(rng, 1, 18, "0123456789");
        if (rng.below(4) == 0)
            text.insert(0, std::string(rng.below(3), '0'));
        if (rng.below(3) == 0)
            text += "." + random_letters(rng, 1, 4, "0123456789");
        inputs.push_back(text);
    }
    for (int i = 0; i < 260; ++i)
    {
        std::string text;
        const std::uint32_t parts = 1 + rng.below(5);
        for (std::uint32_t k = 0; k < parts; ++k)
        {
            if (rng.below(6) == 0)
                text += "(";
            text += random_letters(rng, 1, 3, "0123456789");
            if (rng.below(5) == 0)
                text += "." + random_letters(rng, 1, 2, "0123456789");
            if (rng.below(6) == 0)
                text += ")";
            if (k + 1 < parts || rng.below(12) == 0)
                text += std::string(1, "+-*/"[rng.below(4)]);
        }
        inputs.push_back(text);
    }
    for (const auto &input : inputs)
    {
        std::string evaluated;
        const bool ok = evaluate_v_mode_expression(input, evaluated);
        out << "{\"kind\":\"vmode\",\"input\":" << quote(input) << ",\"items\":" << items_json(query_v_mode(input))
            << ",\"limit3\":" << items_json(query_v_mode(input, 3)) << ",\"eval\":" << (ok ? quote(evaluated) : std::string("null")) << "}\n";
    }
    for (int i = 0; i < 300; ++i)
    {
        const std::string digits = random_letters(rng, 0, 19, "0123456789");
        const std::string fraction = random_letters(rng, 0, 3, "0123456789");
        out << "{\"kind\":\"number_words\",\"digits\":" << quote(digits) << ",\"fraction\":" << quote(fraction)
            << ",\"reading\":" << quote(chinese_number_reading(digits)) << ",\"financial\":" << quote(chinese_financial_number(digits))
            << ",\"money\":" << quote(chinese_money(digits, fraction)) << "}\n";
    }
}

// ---------- 日期时间 ----------

std::string datetime_json(const LocalDateTime &t)
{
    return "[" + std::to_string(t.year) + "," + std::to_string(t.month) + "," + std::to_string(t.day) + "," + std::to_string(t.weekday) + "," +
           std::to_string(t.hour) + "," + std::to_string(t.minute) + "," + std::to_string(t.second) + "]";
}

std::string pad(unsigned value, std::size_t width)
{
    std::string text = std::to_string(value);
    return std::string(text.size() < width ? width - text.size() : 0, '0') + text;
}

void emit_datetime(std::ostream &out)
{
    Lcg rng{0xda7e71e0ULL};
    const auto random_now = [&] {
        LocalDateTime t;
        t.year = rng.below(10) == 0 ? 1880 + rng.below(240) : 1900 + rng.below(201);
        t.month = 1 + rng.below(12);
        t.day = 1 + rng.below(28 + rng.below(4));
        t.weekday = rng.below(20) == 0 ? 7 + rng.below(3) : rng.below(7);
        t.hour = rng.below(24);
        t.minute = rng.below(60);
        t.second = rng.below(60);
        return t;
    };
    std::vector<std::string> keywords{"rq", "riqi", "date", "sj", "shijian", "time", "xq", "xingqi", "week", "x", "RQ", ""};
    for (const auto &keyword : keywords)
    {
        out << "{\"kind\":\"datetime_meta\",\"input\":" << quote(keyword) << ",\"keyword\":" << (is_date_time_keyword(keyword) ? "true" : "false")
            << ",\"category\":" << quote(date_time_category(keyword)) << ",\"ids\":[";
        const auto ids = date_time_format_ids(keyword);
        for (std::size_t i = 0; i < ids.size(); ++i)
            out << (i ? "," : "") << quote(ids[i]);
        const auto menu = date_time_menu_item(keyword);
        out << "],\"menu\":" << items_json({menu}) << ",\"menu_keyword\":" << quote(date_time_menu_keyword(menu)) << "}\n";
    }
    // 唤醒词：随机的此刻，含农历越界的年份。
    for (int i = 0; i < 250; ++i)
    {
        const auto now = random_now();
        const std::string keyword = keywords[rng.below(9)];
        const int limit = rng.below(4) == 0 ? 1 + static_cast<int>(rng.below(6)) : 17;
        out << "{\"kind\":\"datetime\",\"input\":" << quote(keyword) << ",\"now\":" << datetime_json(now) << ",\"limit\":" << limit
            << ",\"is_query\":" << (is_date_time_query(keyword, &now) ? "true" : "false") << ",\"category\":" << quote(date_time_category(keyword))
            << ",\"items\":" << items_json(query_date_time(keyword, &now, limit)) << "}\n";
    }
    // 指定的日期时间：各种形状，取值有对有错，也有写不完的。
    for (int i = 0; i < 650; ++i)
    {
        const auto now = random_now();
        std::string input;
        const unsigned year = rng.below(8) == 0 ? rng.below(10000) : 1900 + rng.below(220);
        const unsigned month = rng.below(8) == 0 ? rng.below(15) : 1 + rng.below(12);
        const unsigned day = rng.below(8) == 0 ? rng.below(35) : 1 + rng.below(30);
        const unsigned hour = rng.below(8) == 0 ? rng.below(30) : rng.below(24);
        const unsigned minute = rng.below(8) == 0 ? rng.below(70) : rng.below(60);
        const unsigned second = rng.below(8) == 0 ? rng.below(70) : rng.below(60);
        switch (rng.below(11))
        {
        case 0: input = pad(year, 4) + pad(month, 2) + pad(day, 2); break;
        case 1: input = pad(year, 4) + pad(month, 2) + pad(day, 2) + pad(hour, 2); break;
        case 2: input = pad(year, 4) + pad(month, 2) + pad(day, 2) + pad(hour, 2) + ":" + pad(minute, 2); break;
        case 3: input = pad(year, 4) + pad(month, 2) + pad(day, 2) + pad(hour, 2) + ":" + pad(minute, 2) + ":" + pad(second, 2); break;
        case 4: input = pad(year, 4) + "/" + std::to_string(month) + "/" + std::to_string(day); break;
        case 5: input = pad(year, 4) + "/" + pad(month, 2) + "/" + pad(day, 2); break;
        case 6: input = pad(year, 4) + "/" + std::to_string(month); break;
        case 7: input = std::to_string(month) + "/" + std::to_string(day); break;
        case 8: input = std::to_string(hour) + ":" + pad(minute, 2); break;
        case 9: input = std::to_string(hour) + ":" + pad(minute, 2) + ":" + pad(second, 2); break;
        default: input = random_letters(rng, 1, 17, "0123456789/:0123"); break;
        }
        out << "{\"kind\":\"datetime\",\"input\":" << quote(input) << ",\"now\":" << datetime_json(now) << ",\"limit\":17,\"is_query\":"
            << (is_date_time_query(input, &now) ? "true" : "false") << ",\"category\":" << quote(date_time_category(input))
            << ",\"items\":" << items_json(query_date_time(input, &now, 17)) << "}\n";
    }
}

// ---------- 表情 / 颜文字 / 超级简拼 ----------

void emit_emoji_kaomoji_jianpin(std::ostream &out, sqlite3 *main_db, sqlite3 *others_db, const std::filesystem::path &main_path,
                                const std::filesystem::path &others_path)
{
    Lcg rng{0xe301a001ULL};
    const auto &profile = GetXiaoheShuangpinProfile();

    std::vector<std::string> emoji_codes = column(others_db, "SELECT DISTINCT key FROM emoji_pinyin ORDER BY key", 120);
    std::vector<std::string> kaomoji_codes = column(others_db, "SELECT pinyin FROM kaomoji ORDER BY pinyin", 60);
    for (const auto &code : column(others_db, "SELECT jianpin FROM kaomoji ORDER BY jianpin", 40))
        kaomoji_codes.push_back(code);
    for (const char *extra : {"", "a", "xiao", "XIAO", "ha'ha", "x1", "a b", "wo", "ai", "ni'hao", "ud", "vv"})
    {
        emoji_codes.push_back(extra);
        kaomoji_codes.push_back(extra);
    }
    for (int i = 0; i < 60; ++i)
    {
        emoji_codes.push_back(random_letters(rng, 1, 5, "abcdefghijklmnopqrstuvwxyz"));
        kaomoji_codes.push_back(random_letters(rng, 1, 4, "abcdefghijklmnopqrstuvwxyz"));
    }
    for (const auto &code : emoji_codes)
        for (const SchemeType scheme : {SchemeType::Quanpin, SchemeType::Shuangpin})
        {
            const int limit = rng.below(5) == 0 ? 1 + static_cast<int>(rng.below(5)) : 10;
            out << "{\"kind\":\"emoji\",\"code\":" << quote(code) << ",\"shuangpin\":" << (scheme == SchemeType::Shuangpin ? "true" : "false")
                << ",\"limit\":" << limit << ",\"result\":" << result_json(query_emoji(code, scheme, others_path, limit, profile)) << "}\n";
        }
    for (const auto &code : kaomoji_codes)
        for (const SchemeType scheme : {SchemeType::Quanpin, SchemeType::Shuangpin})
        {
            const int limit = rng.below(5) == 0 ? 1 + static_cast<int>(rng.below(5)) : 10;
            out << "{\"kind\":\"kaomoji\",\"code\":" << quote(code) << ",\"shuangpin\":" << (scheme == SchemeType::Shuangpin ? "true" : "false")
                << ",\"limit\":" << limit << ",\"result\":" << result_json(query_kaomoji(code, scheme, others_path, limit, profile)) << "}\n";
        }
    // 缺库：诊断文字。
    out << "{\"kind\":\"emoji_missing\",\"result\":" << result_json(query_emoji("xiao", SchemeType::Quanpin, std::filesystem::path{}, 10, profile)) << "}\n";

    // 超级简拼：全拼的字母串，双拼按声母键解码。
    std::vector<std::string> codes{"nh", "wm", "zg", "zgr", "sh", "zh", "n", "a", "nhao", "srfa", "zzzz", "abcdefgh", "jt", "xx", "N", "N1", "", "ud", "vv", "uu"};
    for (const auto &key : column(main_db, "SELECT jp FROM tbl_2_n", 40))
        codes.push_back(key);
    for (const auto &key : column(main_db, "SELECT jp FROM tbl_3_z", 40))
        codes.push_back(key);
    for (const auto &key : column(main_db, "SELECT jp FROM tbl_others_s", 20))
        codes.push_back(key);
    for (int i = 0; i < 160; ++i)
        codes.push_back(random_letters(rng, 1, 9, "abcdefghijklmnopqrstuvwxyz"));
    for (const auto &code : codes)
        for (const SchemeType scheme : {SchemeType::Quanpin, SchemeType::Shuangpin})
        {
            const int limit = rng.below(4) == 0 ? 1 + static_cast<int>(rng.below(8)) : 20;
            out << "{\"kind\":\"jianpin\",\"code\":" << quote(code) << ",\"shuangpin\":" << (scheme == SchemeType::Shuangpin ? "true" : "false")
                << ",\"limit\":" << limit << ",\"context\":" << quote(jianpin_ranking_context(code, scheme, profile))
                << ",\"result\":" << result_json(query_jianpin(code, scheme, main_path, limit, profile)) << "}\n";
        }
}

// ---------- 快捷短语 ----------

void emit_quick_phrases(std::ostream &out, const std::filesystem::path &directory)
{
    Lcg rng{0x91c4f001ULL};
    const auto path = directory / "quick_fixture.db";
    std::filesystem::remove(path);
    sqlite3 *db = nullptr;
    sqlite3_open(path.string().c_str(), &db);
    sqlite3_exec(db, "CREATE TABLE quick_parases(key TEXT, value TEXT, weight INTEGER)", nullptr, nullptr, nullptr);
    std::string rows = "[";
    std::vector<std::string> keys;
    for (int i = 0; i < 400; ++i)
    {
        const std::string key = random_letters(rng, 1, 4, "abcde");
        const std::string value = "短语" + std::to_string(rng.below(60));
        const int weight = static_cast<int>(rng.below(8));
        keys.push_back(key);
        sqlite3_stmt *statement = nullptr;
        sqlite3_prepare_v2(db, "INSERT INTO quick_parases VALUES(?1,?2,?3)", -1, &statement, nullptr);
        sqlite3_bind_text(statement, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, value.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 3, weight);
        sqlite3_step(statement);
        sqlite3_finalize(statement);
        rows += std::string(i ? "," : "") + "[" + quote(key) + "," + quote(value) + "," + std::to_string(weight) + "]";
    }
    sqlite3_close(db);
    out << "{\"kind\":\"qp_fixture\",\"rows\":" << rows << "]}\n";

    std::vector<std::string> codes{"a", "ab", "abc", "e", "z", "A", "a1", "", "abcde", "dd"};
    for (int i = 0; i < 80; ++i)
        codes.push_back(random_letters(rng, 1, 4, "abcdef"));
    for (const auto &code : codes)
    {
        const int limit = rng.below(4) == 0 ? 1 + static_cast<int>(rng.below(5)) : 100;
        out << "{\"kind\":\"quick_phrase\",\"code\":" << quote(code) << ",\"limit\":" << limit << ",\"prefix\":"
            << result_json(query_quick_phrases(code, path, limit)) << ",\"exact\":" << result_json(query_quick_phrases_by_code(code, path, limit)) << "}\n";
    }

    // 插入位置：随机的候选列表。
    static const CandidateSource sources[] = {CandidateSource::Database, CandidateSource::UserDatabase, CandidateSource::Generated,
                                              CandidateSource::Fallback, CandidateSource::QuickPhrase, CandidateSource::CloudSuggestion,
                                              CandidateSource::EnglishDictionary, CandidateSource::Emoji, CandidateSource::Kaomoji,
                                              CandidateSource::DateTime, CandidateSource::AiSuggestion};
    for (int scenario = 0; scenario < 300; ++scenario)
    {
        const auto make = [&](std::uint32_t count, bool quick) {
            std::vector<WordItem> items;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                WordItem item("p" + std::to_string(i), "词" + std::to_string(rng.below(14)), static_cast<std::int64_t>(rng.below(100)),
                              quick ? CandidateSource::QuickPhrase : sources[rng.below(11)]);
                if (!quick && rng.below(5) == 0)
                    item.fixed_position = 1 + static_cast<int>(rng.below(5));
                items.push_back(item);
            }
            return items;
        };
        auto candidates = make(rng.below(12), false);
        const auto phrases = make(rng.below(4), true);
        const auto before = candidates;
        const std::size_t slot = rng.below(8);
        place_quick_phrases(candidates, phrases, slot);
        out << "{\"kind\":\"place_qp\",\"slot\":" << slot << ",\"candidates\":" << items_json(before) << ",\"phrases\":" << items_json(phrases)
            << ",\"result\":" << items_json(candidates) << "}\n";
    }
}

// ---------- 英文 ----------

void emit_english(std::ostream &out, const std::string &english_path, const std::string &translations_path, sqlite3 *english_db,
                  const std::filesystem::path &directory)
{
    Lcg rng{0xe1915001ULL};
    EnglishDictionary dictionary(english_path, false, translations_path);
    std::vector<std::string> prefixes{"a", "ab", "the", "hello", "zzzz", "Hello", "a1", "", "ap", "app", "apple", "z", "q", "xyz", "wor"};
    for (int i = 0; i < 150; ++i)
        prefixes.push_back(random_letters(rng, 1, 5, "abcdefghijklmnopqrstuvwxyz"));
    for (const auto &word : column(english_db, "SELECT DISTINCT word FROM english_words ORDER BY word", 150))
        prefixes.push_back(word.substr(0, 1 + rng.below(static_cast<std::uint32_t>(word.size()))));
    for (const auto &prefix : prefixes)
    {
        const std::size_t limit = rng.below(4) == 0 ? rng.below(12) : 5;
        out << "{\"kind\":\"english_prefix\",\"prefix\":" << quote(prefix) << ",\"limit\":" << limit
            << ",\"items\":" << items_json(dictionary.query_prefix(prefix, limit)) << "}\n";
    }

    std::vector<std::string> english_words = column(english_db, "SELECT english FROM en_zh_glosses ORDER BY english", 150);
    for (const auto &word : column(english_db, "SELECT DISTINCT word FROM english_words ORDER BY word", 120))
        english_words.push_back(word);
    for (const char *extra : {"apple", "run", "hello", "ai controller", "AI Controller", "", "love", "sun", "water", "kitty", "ZZZ-none"})
        english_words.push_back(extra);
    std::vector<std::string> chinese_words = column(english_db, "SELECT chinese FROM zh_en_glosses ORDER BY chinese", 150);
    for (const char *extra : {"苹果", "你好", "太阳", "水", "跑", "", "不存在的词语", "爱", "猫", "爱情"})
        chinese_words.push_back(extra);

    const std::vector<std::string> disabled_sets{"", "legacy-en", "qingjian-zh", "qingjian-en", "qingjian-en,legacy-en", "upstream-cn-en", "frequency-3000,frequency-10000,frequency-20000"};
    for (const auto &word : english_words)
    {
        const auto &disabled = disabled_sets[rng.below(7)];
        dictionary.set_disabled_dictionaries(disabled);
        out << "{\"kind\":\"english_gloss\",\"direction\":\"zh\",\"word\":" << quote(word) << ",\"disabled\":" << quote(disabled)
            << ",\"gloss\":" << quote(dictionary.query_chinese_gloss(word)) << "}\n";
    }
    for (const auto &word : chinese_words)
    {
        const auto &disabled = disabled_sets[rng.below(7)];
        dictionary.set_disabled_dictionaries(disabled);
        out << "{\"kind\":\"english_gloss\",\"direction\":\"en\",\"word\":" << quote(word) << ",\"disabled\":" << quote(disabled)
            << ",\"gloss\":" << quote(dictionary.query_english_gloss(word)) << "}\n";
    }
    dictionary.set_disabled_dictionaries("");
    for (const char *language : {"en", "zh", "ja", "es", "vi", "fr"})
        for (const auto &word : column(english_db, "SELECT DISTINCT word FROM english_words ORDER BY word", 40))
            out << "{\"kind\":\"english_translation\",\"language\":" << quote(language) << ",\"word\":" << quote(word)
                << ",\"gloss\":" << quote(dictionary.query_translation_gloss(word, language)) << "}\n";
    for (const char *language : {"en", "zh", "ja", "es", "vi"})
        for (const auto &word : chinese_words)
            out << "{\"kind\":\"english_translation\",\"language\":" << quote(language) << ",\"word\":" << quote(word)
                << ",\"gloss\":" << quote(dictionary.query_translation_gloss(word, language)) << "}\n";
    (void)directory;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 6)
    {
        std::cerr << "用法：msime_local_modes_probe <msime.db> <others.db> <english.db> <翻译侧车文件> <临时目录> [输出文件]\n";
        return 2;
    }
    std::ofstream file;
    if (argc > 6)
        file.open(argv[6], std::ios::binary);
    std::ostream &out = argc > 6 ? static_cast<std::ostream &>(file) : std::cout;

    sqlite3 *main_db = nullptr, *others_db = nullptr, *english_db = nullptr;
    if (sqlite3_open_v2(argv[1], &main_db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK ||
        sqlite3_open_v2(argv[2], &others_db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK ||
        sqlite3_open_v2(argv[3], &english_db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
    {
        std::cerr << "打不开词库\n";
        return 1;
    }
    const std::filesystem::path directory(argv[5]);
    std::filesystem::create_directories(directory);

    emit_unicode(out);
    emit_vmode(out);
    emit_datetime(out);
    emit_emoji_kaomoji_jianpin(out, main_db, others_db, argv[1], argv[2]);
    emit_quick_phrases(out, directory);
    emit_english(out, argv[3], argv[4], english_db, directory);
    sqlite3_close(main_db);
    sqlite3_close(others_db);
    sqlite3_close(english_db);
    return 0;
}
