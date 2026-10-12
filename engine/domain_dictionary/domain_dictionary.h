#pragma once
#include "../core/word_item.h"
#include "../quanpin/quanpin_query.h"
#include "../quanpin/word_lattice.h"
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace domain_dictionary
{
// Public IDs are stable configuration/export identifiers, independent of source filenames.
const std::vector<std::string> &ids();
std::string normalize_enabled(const std::string &csv);
class Dictionary
{
  public:
    explicit Dictionary(std::filesystem::path path);
    ~Dictionary();
    Dictionary(const Dictionary &) = delete;
    Dictionary &operator=(const Dictionary &) = delete;
    bool set_enabled(const std::string &csv);
    const std::string &enabled() const;
    std::vector<WordItem> query(const quanpin::Segments &segments, const std::string &code, int limit = 128,
                                quanpin::QuerySource source = quanpin::QuerySource::Quanpin);
    std::vector<WordItem> exact(const std::vector<quanpin::Segments> &cuts, int limit = 128);
    quanpin::WordLatticeLookup lattice(int limit, metasequoia::FuzzyPinyinOptions fuzzy = {});
    // Merge duplicates without replacing the base dictionary's pronunciation, weight or user overrides.
    static void merge(std::vector<WordItem> &base, std::vector<WordItem> extra);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace domain_dictionary
