#include "ipc/candidate_translation_policy.h"
#include "tests/includes/test_framework.h"

TEST_CASE(ctrl_enter_alone_commits_the_secondary_candidate)
{
    using FanyImeIpc::IsTranslationCommitKey;
    REQUIRE(IsTranslationCommitKey(0x0D, FanyImeIpc::kModifierControl));
    // The UI-less flag lives outside the modifier mask and must not disarm the shortcut.
    REQUIRE(IsTranslationCommitKey(0x0D, FanyImeIpc::kModifierControl | FanyImeIpc::kModifierUiLess));

    REQUIRE(!IsTranslationCommitKey(0x0D, 0));
    REQUIRE(!IsTranslationCommitKey(0x0D, FanyImeIpc::kModifierControl | FanyImeIpc::kModifierShift));
    REQUIRE(!IsTranslationCommitKey(0x0D, FanyImeIpc::kModifierControl | FanyImeIpc::kModifierAlt));
    REQUIRE(!IsTranslationCommitKey(0x20, FanyImeIpc::kModifierControl));
    REQUIRE(!IsTranslationCommitKey('J', FanyImeIpc::kModifierControl));
}

TEST_CASE(translation_gloss_splits_on_both_semicolon_widths)
{
    using FanyImeIpc::SplitTranslationGloss;
    // en->zh glosses join senses with the fullwidth semicolon.
    const auto apple = SplitTranslationGloss("苹果；家伙；[计]苹果公司");
    REQUIRE_EQ(apple.size(), static_cast<size_t>(3));
    REQUIRE_EQ(apple[0], std::string("苹果"));
    REQUIRE_EQ(apple[1], std::string("家伙"));
    REQUIRE_EQ(apple[2], std::string("[计]苹果公司"));

    // zh->en glosses use "; " and the trailing space must not survive.
    const auto chuan = SplitTranslationGloss("string; trail; chuan");
    REQUIRE_EQ(chuan.size(), static_cast<size_t>(3));
    REQUIRE_EQ(chuan[0], std::string("string"));
    REQUIRE_EQ(chuan[1], std::string("trail"));
    REQUIRE_EQ(chuan[2], std::string("chuan"));
}

TEST_CASE(translation_gloss_keeps_a_single_sense_whole)
{
    using FanyImeIpc::SplitTranslationGloss;
    // A cloud gloss is one sentence: commas inside it are not sense separators.
    const auto cloud = SplitTranslationGloss("今天天气不错，我们出去走走");
    REQUIRE_EQ(cloud.size(), static_cast<size_t>(1));
    REQUIRE_EQ(cloud[0], std::string("今天天气不错，我们出去走走"));

    REQUIRE(SplitTranslationGloss("").empty());
    REQUIRE(SplitTranslationGloss("   ").empty());
    // Empty senses are dropped rather than becoming blank candidates.
    REQUIRE_EQ(SplitTranslationGloss("；；").size(), static_cast<size_t>(0));
    const auto ragged = SplitTranslationGloss("；apple； ；pear；");
    REQUIRE_EQ(ragged.size(), static_cast<size_t>(2));
    REQUIRE_EQ(ragged[0], std::string("apple"));
    REQUIRE_EQ(ragged[1], std::string("pear"));
}

TEST_CASE(translation_commit_removes_glossary_display_annotations)
{
    const auto values = FanyImeIpc::SplitTranslationGloss("v. develop; n. development; v. 開発する (かいはつする)");
    REQUIRE_EQ(values.size(), size_t{3});
    REQUIRE_EQ(values[0], "develop");
    REQUIRE_EQ(values[1], "development");
    REQUIRE_EQ(values[2], "開発する");
}
