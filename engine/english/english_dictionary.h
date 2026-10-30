#pragma once

#include "../core/word_item.h"
#include <cstddef>
#include <sqlite3.h>
#include <string>
#include <unordered_map>
#include <vector>

class EnglishDictionary
{
  public:
    explicit EnglishDictionary(std::string db_path, bool initialize_schema = true, std::string translations_path = {});
    ~EnglishDictionary();

    EnglishDictionary(const EnglishDictionary &) = delete;
    EnglishDictionary &operator=(const EnglishDictionary &) = delete;

    std::vector<WordItem> query_prefix(const std::string &prefix, size_t limit = 5);
    std::string query_chinese_gloss(const std::string &english);
    std::string query_english_gloss(const std::string &chinese);
    std::string query_translation_gloss(const std::string &word, const std::string &language);
    void set_disabled_dictionaries(const std::string &ids);
    bool ready();
    static bool ensure_schema(const std::string &db_path);
    static bool upsert_gloss(const std::string &db_path, bool chinese_to_english, const std::string &key,
                             const std::string &gloss);

  private:
    bool dictionary_enabled(const std::string &id) const;
    std::string query_qingjian_gloss(const std::string &word, const std::string &language);
    std::string disabled_dictionaries_;
    bool packs_open_attempted_ = false;
    sqlite3 *packs_db_ = nullptr;
    sqlite3_stmt *packs_statement_ = nullptr;
    bool ensure_query_statement();
    bool ensure_gloss_statements();
    void load_custom_translations();
    std::string translations_path_;
    void close_database();

  private:
    std::string db_path_;
    sqlite3 *db_ = nullptr;
    sqlite3_stmt *query_statement_ = nullptr;
    sqlite3_stmt *en_zh_statement_ = nullptr;
    sqlite3_stmt *zh_en_statement_ = nullptr;
    bool bundled_open_attempted_ = false;
    sqlite3 *bundled_db_ = nullptr;
    sqlite3_stmt *bundled_statement_ = nullptr;
    std::unordered_map<std::string, std::string> custom_en_zh_;
    std::unordered_map<std::string, std::string> custom_zh_en_;
};
