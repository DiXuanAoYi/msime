#include "core/input_session.h"
#include "core/data_path.h"
#include "domain_dictionary/domain_dictionary.h"
#include "quanpin/engine.h"
#include "test_directory_cleanup.h"
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace
{
void require(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}
void sql(sqlite3 *db, const std::string &text)
{
    char *error = nullptr;
    if (sqlite3_exec(db, text.c_str(), nullptr, nullptr, &error) != SQLITE_OK)
    {
        const std::string message = error ? error : "sqlite fixture failure";
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}
struct Sample
{
    const char *id;
    const char *key;
    const char *word;
};
const Sample samples[] = {
    {"animals", "mao'mi", "猫咪"},   {"automotive", "che'lun", "车轮"}, {"finance", "ji'jin", "基金"},
    {"food", "lu'zhu", "卤煮"},      {"historical", "si'ma", "司马"},   {"idioms", "wo'xin", "卧薪"},
    {"it", "suan'fa", "算法"},       {"law", "pan'jue", "判决"},        {"medicine", "yao'ji", "药剂"},
    {"places", "hang'zhou", "杭州"}, {"poetry", "chun'feng", "春风"}};
void create(const std::filesystem::path &file, bool domain)
{
    sqlite3 *db = nullptr;
    require(sqlite3_open(metasequoia::path_to_utf8(file).c_str(), &db) == SQLITE_OK, "Cannot create fixture database");
    for (const auto &length : {"1", "2", "3", "4", "5", "6", "7", "others"})
        for (char initial = 'a'; initial <= 'z'; ++initial)
            sql(db, "CREATE TABLE tbl_" + std::string(length) + "_" + initial +
                        "(pack TEXT,key TEXT,jp TEXT,value TEXT,weight INTEGER)");
    sql(db, "CREATE TABLE entries(pack TEXT,key TEXT,jp TEXT,value TEXT,weight INTEGER)");
    if (domain)
    {
        for (const auto &sample : samples)
        {
            auto segments = quanpin::split_segments(sample.key);
            const auto jp = quanpin::segments_to_jianpin(segments);
            const std::string values = " VALUES('" + std::string(sample.id) + "','";
            sqlite3_stmt *stmt = nullptr;
            const auto insert = [&](const std::string &table) {
                require(sqlite3_prepare_v2(db, ("INSERT INTO " + table + " VALUES(?1,?2,?3,?4,300)").c_str(), -1, &stmt,
                                           nullptr) == SQLITE_OK,
                        "Cannot prepare domain fixture row");
                sqlite3_bind_text(stmt, 1, sample.id, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, sample.key, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, jp.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 4, sample.word, -1, SQLITE_TRANSIENT);
                require(sqlite3_step(stmt) == SQLITE_DONE, "Cannot insert domain fixture row");
                sqlite3_finalize(stmt);
            };
            insert("entries");
            insert(quanpin::build_table_name(segments));
        }
        sql(db,
            "INSERT INTO entries VALUES('it','ni''hao''ni''hao''ni''hao''ni''hao','nhnhnhnh','你好你好你好你好',10)");
        sql(db, "INSERT INTO tbl_others_n SELECT * FROM entries WHERE value='你好你好你好你好'");
    }
    else
        sql(db, "INSERT INTO tbl_2_n VALUES('','ni''hao','nh','你好',100)");
    sqlite3_close(db);
}
const WordItem *find(const std::vector<WordItem> &items, const std::string &word)
{
    const auto item = std::find_if(items.begin(), items.end(), [&](const auto &x) { return x.word == word; });
    return item == items.end() ? nullptr : &*item;
}
void type(metasequoia::InputSession &session, const char *text)
{
    for (const char *p = text; *p; ++p)
        require(session.handle_character(*p).handled, "Input was not handled");
}
int fixture()
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("msime-domains-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    metasequoia::test::ScopedDataDirectoryCleanup cleanup(root);
    create(root / "msime.db", false);
    create(root / "domain-input.db", true);
    domain_dictionary::Dictionary domains(root / "domain-input.db");
    for (const auto &sample : samples)
    {
        domains.set_enabled(sample.id);
        const auto candidates = domains.query(quanpin::split_segments(sample.key), sample.key);
        const auto item = find(candidates, sample.word);
        require(item && item->canonical_pinyin == sample.key &&
                    item->input_domains == std::vector<std::string>{sample.id},
                "A domain pack did not recall a tagged complete lexical candidate");
        domains.set_enabled("");
        require(domains.query(quanpin::split_segments(sample.key), sample.key).empty(), "Disabled pack leaked a row");
    }
    require(domain_dictionary::normalize_enabled("it,../invalid,it,idioms") == "idioms,it",
            "Domain IDs not canonicalized");
    domains.set_enabled("it");
    require(find(domains.query({"s", "f"}, "s'f"), "算法") != nullptr, "Pure jianpin missed domain input");
    require(find(domains.query({"suan", "f"}, "suan'f"), "算法") != nullptr, "Mixed jianpin missed domain input");
    require(find(domains.query({"ni", "hao", "ni", "hao", "ni", "hao", "ni", "hao"}, "nhnhnhnh"), "你好你好你好你好") !=
                nullptr,
            "Eight syllables did not use tbl_others");
    auto lexical = domains.lattice(32)({"suan", "fa"});
    require(!lexical.empty() && lexical.front().input_domains == std::vector<std::string>{"it"},
            "Lattice dropped provenance");
    metasequoia::RuntimePaths paths{root, root / "user", root / "cache", root};
    std::filesystem::create_directories(paths.user_data);
    std::filesystem::create_directories(paths.cache);
    {
        metasequoia::InputSession session(SchemeType::Quanpin, 0, false, true, false, paths);
        session.set_enabled_domain_dictionaries("it");
        type(session, "suanfani");
        const auto candidate = find(session.candidates(), "算法");
        require(candidate && candidate->canonical_pinyin == "suan'fa", "Session missed domain prefix");
        const auto index = static_cast<size_t>(candidate - session.candidates().data());
        require(session.select_candidate(index).commit == "算法" && session.preedit() == "ni",
                "Domain selection consumed wrong input");
        session.handle_command(metasequoia::Command::Cancel);
        type(session, "suanfa");
        require(find(session.candidates(), "算法") != nullptr, "Domain session missing complete input");
        session.set_enabled_domain_dictionaries("");
        require(find(session.candidates(), "算法") == nullptr, "Disabling domain reused stale session cache");
        session.set_enabled_domain_dictionaries("it");
        require(find(session.candidates(), "算法") != nullptr, "Enabling domain did not refresh candidates");
    }
    {
        metasequoia::InputSession session(SchemeType::Shuangpin, GetXiaoheShuangpinProfile(), paths);
        session.set_shuangpin_helpcode_enabled(false);
        session.set_enabled_domain_dictionaries("it");
        type(session, "srfa");
        require(find(session.candidates(), "算法") != nullptr, "Shuangpin did not recall domain input");
        require(session.select_candidate("算法").commit == "算法" && !session.has_composition(),
                "Shuangpin domain consumed wrong raw code");
    }
    {
        QuanpinEngine engine(paths);
        QueryRequest request;
        request.valid = true;
        request.raw_input = "suanfanihao";
        request.segmentation = "suan'fa'ni'hao";
        request.enabled_domain_dictionaries = "it";
        request.sentence_association.word_lattice = true;
        const auto candidates = engine.query(request);
        const auto item = find(candidates, "算法你好");
        require(item && item->sentence_association && item->input_domains == std::vector<std::string>{"it"},
                "Domain lexeme not used in sentence graph");
    }
    domain_dictionary::Dictionary missing(root / "never-created.db");
    missing.set_enabled("it");
    require(missing.query({"suan", "fa"}, "suan'fa").empty() && !std::filesystem::exists(root / "never-created.db"),
            "Missing read-only asset created a database");
    return 0;
}
} // namespace
int main(int argc, char **argv)
{
    try
    {
        if (argc == 3)
        {
            // Optional real pinned asset check, never downloads or writes it.
            domain_dictionary::Dictionary dictionary(std::filesystem::u8path(argv[1]));
            for (const auto &id : domain_dictionary::ids())
            {
                dictionary.set_enabled(id);
                std::ifstream file(std::filesystem::u8path(argv[2]) / (id + ".tsv"));
                std::string line;
                while (std::getline(file, line) && (line.empty() || line.front() == '#'))
                {
                }
                require(bool(file) && !line.empty(), "Missing real source pack");
                const auto tab = line.find('\t');
                const auto end = line.find('\t', tab + 1);
                auto key = line.substr(tab + 1, end - tab - 1);
                std::replace(key.begin(), key.end(), ' ', '\'');
                const auto rows = dictionary.query(quanpin::split_segments(key), key, 1000000);
                require(find(rows, line.substr(0, tab)) != nullptr, "Real pinned domain sample not recallable");
                std::printf("%s: production full-pinyin recall passed\n", id.c_str());
            }
            std::puts("11 real domain packs recalled through production query helper");
            return 0;
        }
        return fixture();
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
