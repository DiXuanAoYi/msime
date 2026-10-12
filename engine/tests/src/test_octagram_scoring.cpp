#include "ngram/octagram/octagram_gram.h"
#include "quanpin/word_lattice.h"
#include "neural/sentence_model.h"
#include "core/data_path.h"
#include "test_directory_cleanup.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

void require(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}
int main()
{
    try
    {
        const auto root = std::filesystem::temp_directory_path() /
                          ("msime-gram-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        metasequoia::test::ScopedDataDirectoryCleanup cleanup(root);
        {
            // Tiny synthetic safetensors, sufficient to prove UTF-8 filesystem loading. No model download.
            std::string model_header =
                R"({"__metadata__":{"config":"{\"vocab\":2,\"n_layer\":1,\"n_head\":1,\"n_embd\":1,\"context\":4}","vocab":"[\"<pad>\",\"你\"]"})";
            std::vector<float> tensors;
            const auto tensor = [&](const char *name, size_t count) {
                const auto start = tensors.size() * sizeof(float);
                tensors.insert(tensors.end(), count, 1.0f);
                model_header += ",\"" + std::string(name) + "\":{\"dtype\":\"F32\",\"shape\":[" +
                                std::to_string(count) + "],\"data_offsets\":[" + std::to_string(start) + "," +
                                std::to_string(tensors.size() * sizeof(float)) + "]}";
            };
            tensor("tok.weight", 2);
            tensor("pos.weight", 4);
            tensor("blocks.0.ln1.weight", 1);
            tensor("blocks.0.ln1.bias", 1);
            tensor("blocks.0.qkv.weight", 3);
            tensor("blocks.0.qkv.bias", 3);
            tensor("blocks.0.proj.weight", 1);
            tensor("blocks.0.proj.bias", 1);
            tensor("blocks.0.ln2.weight", 1);
            tensor("blocks.0.ln2.bias", 1);
            tensor("blocks.0.fc.weight", 4);
            tensor("blocks.0.fc.bias", 4);
            tensor("blocks.0.out.weight", 4);
            tensor("blocks.0.out.bias", 1);
            tensor("ln_f.weight", 1);
            tensor("ln_f.bias", 1);
            model_header += '}';
            const auto model_path = root / std::filesystem::u8path("中文模型目录");
            std::filesystem::create_directories(model_path);
            const auto model_file = model_path / std::filesystem::u8path("整句测试.safetensors");
            {
                std::ofstream out(model_file, std::ios::binary);
                const auto length = static_cast<uint64_t>(model_header.size());
                out.write(reinterpret_cast<const char *>(&length), sizeof(length));
                out.write(model_header.data(), model_header.size());
                out.write(reinterpret_cast<const char *>(tensors.data()), tensors.size() * sizeof(float));
            }
            require(neural::SentenceModel::load_file(metasequoia::path_to_utf8(model_file)) != nullptr,
                    "Neural model did not load a synthetic safetensors from a Chinese path");
        }
        const auto file = root / std::filesystem::u8path("搭配测试.gram");
        std::vector<std::pair<std::string, int>> data = {{gram::GramDb::encode("你好"), 100000},
                                                         {gram::GramDb::encode("好") + "$", 200000}};
        std::sort(data.begin(), data.end());
        std::vector<const char *> keys;
        std::vector<int> values;
        for (const auto &[key, value] : data)
        {
            keys.push_back(key.c_str());
            values.push_back(value);
        }
        Darts::DoubleArray trie;
        require(trie.build(keys.size(), keys.data(), nullptr, values.data()) == 0, "Cannot build gram fixture");
        char header[44]{};
        std::memcpy(header, "Rime::Grammar/", 14);
        const auto count = static_cast<uint32_t>(trie.size());
        const int32_t offset = 4;
        std::memcpy(header + 36, &count, 4);
        std::memcpy(header + 40, &offset, 4);
        {
            std::ofstream out(file, std::ios::binary);
            out.write(header, sizeof(header));
            out.write(static_cast<const char *>(trie.array()), static_cast<std::streamsize>(trie.total_size()));
            require(bool(out), "Cannot write gram fixture");
        }
        gram::GramDb db;
        require(db.open(file) && db.valid(), "Gram reader failed on a Unicode path");
        gram::GrammarConfig config;
        gram::QueryStats stats;
        require(std::abs(db.query("你", "好", false, config, &stats) + 4.0) < 1e-9 && stats.context_windows == 1 &&
                    stats.matched_windows == 1 && stats.matches == 1 && stats.best_value == 10.0,
                "Gram query score or diagnostic counts changed");
        require(db.query("你", "好", true, config) == 2.0, "Sentence rear penalty not applied");
        auto weak = config;
        weak.collocation_min_length = 3;
        require(db.query("你", "好啊", false, config) == -4.0 && db.query("你", "好啊", false, weak) == -6.0,
                "Minimum collocation length did not select weak penalty");
        weak.weak_collocation_penalty = -8.0;
        require(db.query("你", "好啊", false, weak) == 2.0, "Custom weak penalty ignored");
        weak.rear_penalty = -1.0;
        require(db.query("你", "好", true, weak) == 19.0, "Custom rear penalty ignored");
        auto changed = config;
        changed.collocation_penalty = -9;
        require(db.query("你", "好", false, changed) == 1.0, "Custom collocation penalty ignored");
        changed.non_collocation_penalty = -2;
        require(db.query("无", "匹配", false, changed) == -2.0, "Custom unmatched penalty ignored");
        changed.collocation_max_length = 1;
        gram::QueryStats none;
        require(db.query("你", "好", false, changed, &none) == -2.0 && none.context_windows == 0,
                "Custom context length ignored");
        const auto lookup = [](const quanpin::Segments &span) {
            if (span == quanpin::Segments{"ni"})
                return std::vector<quanpin::LatticeLexeme>{{"ni", "你", 100}};
            if (span == quanpin::Segments{"hao"})
                return std::vector<quanpin::LatticeLexeme>{{"hao", "好", 100}};
            return std::vector<quanpin::LatticeLexeme>{};
        };
        quanpin::WordLatticeOptions plain;
        const auto original = quanpin::decode_word_lattice({"ni", "hao"}, lookup, plain);
        auto scored = plain;
        scored.collocation_weight = 1.0;
        scored.collocation_scorer = [&db, config](std::string_view context, std::string_view word, bool rear) {
            return db.query(std::string(context), std::string(word), rear, config) * quanpin::kLnToLog10;
        };
        const auto paths = quanpin::decode_word_lattice({"ni", "hao"}, lookup, scored);
        // Empty first context contributes -6, final 好 gets rear score +2: exactly one -4*ln/log10 injection.
        require(original.size() == 1 && paths.size() == 1 &&
                    std::abs(paths[0].log_prob - original[0].log_prob + 4.0 * quanpin::kLnToLog10) < 1e-9,
                "Octagram score was converted incorrectly or injected twice");
        scored.include_lattice_best = false;
        scored.include_collocation_best = true;
        std::vector<WordItem> candidates;
        quanpin::merge_lattice_candidates(candidates, {"ni", "hao"}, lookup, "ni'hao", scored);
        require(candidates.size() == 1 && candidates[0].source == CandidateSource::Collocation &&
                    candidates[0].sentence_association && candidates[0].word == "你好",
                "Octagram source did not display decoded best path once");
        std::puts("Gram fixture: matched_windows=1 matches=1; Unicode path and single log10 injection verified");
        return 0;
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
