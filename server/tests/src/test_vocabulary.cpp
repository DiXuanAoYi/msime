#include "english/vocabulary_store.h"
#include "english/vocabulary_pending.h"
#include "tests/includes/test_framework.h"
#include <chrono>

TEST_CASE(vocabulary_counts_distinct_words_per_commit_and_language)
{
    auto store = Vocabulary::Store::Open(":memory:");
    const int64_t now = Vocabulary::NowMs();
    Vocabulary::Batch batch{"en", {"develop", "develop", "hello"}, {"develop"}, {"develop"}, now};
    store->Record(batch, 90);
    store->Record(batch, 90);
    store->Record(batch, 90);
    store->Record({"ja", {"開発"}, {"開発"}, {}, now}, 90);
    const auto rows = store->Read(90, now);
    REQUIRE_EQ(rows.size(), size_t{3});
    for (const auto &row : rows)
        if (row.word == "develop")
        {
            REQUIRE_EQ(row.seen, 3);
            REQUIRE_EQ(row.selected, 3);
            REQUIRE_EQ(row.used, 3);
        }
        else if (row.word == "hello")
        {
            REQUIRE_EQ(row.seen, 3);
            REQUIRE_EQ(row.selected, 0);
        }
        else
        {
            REQUIRE_EQ(row.language, std::string("ja"));
            REQUIRE_EQ(row.used, 0);
        }
}
TEST_CASE(vocabulary_clear_fences_queued_writes_and_retention_prunes)
{
    auto store = Vocabulary::Store::Open(":memory:");
    const int64_t now = Vocabulary::NowMs();
    store->Record({"en", {"old"}, {}, {}, now - 31LL * 86400000}, 90);
    REQUIRE_EQ(store->Read(30, now).size(), size_t{0});
    store->Record({"en", {"before"}, {}, {}, now}, 90);
    store->Clear(now + 1);
    store->Record({"en", {"queued"}, {}, {}, now}, 90);
    REQUIRE(store->Read(90, now).empty());
    store->Record({"en", {"after"}, {}, {}, now + 2}, 90);
    REQUIRE_EQ(store->Read(90, now + 2).size(), size_t{1});
}
TEST_CASE(vocabulary_ack_requires_exact_client_epoch_request_and_only_counts_once)
{
    Vocabulary::Pending pending;
    pending.Arm({1, 2, 3}, {"en", {"word"}, {}, {}, 1}, 100);
    REQUIRE(!pending.Confirm({2, 2, 3}, 101));
    REQUIRE(!pending.Confirm({1, 3, 3}, 101));
    REQUIRE(!pending.Confirm({1, 2, 4}, 101));
    REQUIRE(pending.Confirm({1, 2, 3}, 101).has_value());
    REQUIRE(!pending.Confirm({1, 2, 3}, 102));
    pending.Arm({1, 2, 4}, {}, 200);
    REQUIRE(!pending.Confirm({1, 2, 4}, 10201));
    pending.Arm({1, 2, 5}, {}, 200);
    pending.Reset();
    REQUIRE(!pending.Confirm({1, 2, 5}, 201));
}

TEST_CASE(vocabulary_survives_reopening_and_rejects_invalid_records)
{
    const auto path = std::filesystem::temp_directory_path() /
                      ("msime-vocabulary-test-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    const auto now = Vocabulary::NowMs();
    {
        auto store = Vocabulary::Store::Open(path);
        store->Record({"fr", {"développer"}, {"développer"}, {}, now}, 90);
        store->Record({"invalid", {"word"}, {}, {}, now}, 90);
        store->Record({"en", {"bad\trow", ""}, {}, {}, now}, 90);
    }
    {
        auto store = Vocabulary::Store::Open(path);
        const auto entries = store->Read(90, now);
        REQUIRE_EQ(entries.size(), size_t{1});
        REQUIRE_EQ(entries.front().word, std::string("développer"));
        REQUIRE_EQ(entries.front().selected, 1);
    }
    std::filesystem::remove(path);
}
