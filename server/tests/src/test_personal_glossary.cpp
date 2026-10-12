#include "english/personal_glossary.h"
#include "tests/includes/test_framework.h"

using EnglishIme::PersonalGlossary;

TEST_CASE(personal_glossary_is_language_scoped_and_offline)
{
    PersonalGlossary glossary;
    REQUIRE(glossary.Parse("en\t开发\tdevelopment\tdevelop\nja\t开发\t開発\nes\t开发\tdesarrollo\n"));
    REQUIRE_EQ(glossary.Size(), size_t{3});
    REQUIRE_EQ(glossary.Lookup("en", "开发"), std::string("development; develop"));
    REQUIRE_EQ(glossary.Lookup("ja", "开发"), std::string("開発"));
    REQUIRE_EQ(glossary.Lookup("es", "开发"), std::string("desarrollo"));
    REQUIRE(glossary.Lookup("fr", "开发").empty());
    REQUIRE(glossary.Lookup("ja", "不存在").empty());
}

TEST_CASE(personal_glossary_accepts_bom_crlf_comments_and_last_override)
{
    PersonalGlossary glossary;
    const std::string text = "\xEF\xBB\xBF# Personal translations\r\n\nen\t 苹果 \tapple\r\nen\t苹果\tfruit\tfruit\r\n";
    REQUIRE(glossary.Parse(text));
    REQUIRE_EQ(glossary.Source(), text);
    REQUIRE_EQ(glossary.Size(), size_t{1});
    REQUIRE_EQ(glossary.Lookup("en", "苹果"), std::string("fruit"));
    REQUIRE(glossary.Parse(""));
    REQUIRE_EQ(glossary.Size(), size_t{0});
}

TEST_CASE(personal_glossary_rejects_bad_update_without_losing_previous_entries)
{
    PersonalGlossary glossary;
    REQUIRE(glossary.Parse("en\t开发\tdevelopment"));
    const auto original = glossary.Source();
    for (const std::string bad : {"xx\t开发\tdevelopment", "en\t\tapple", "en\t苹果\t", "en\t苹果",
                                  "en\t苹果\tapple\ta\tb", "en\t苹果\tapple\t", "en\t苹果\t\xFF"})
    {
        REQUIRE(!glossary.Parse(bad));
        REQUIRE_EQ(glossary.Source(), original);
        REQUIRE_EQ(glossary.Lookup("en", "开发"), std::string("development"));
    }
    REQUIRE(!glossary.Parse(std::string(PersonalGlossary::kMaxBytes + 1, 'a')));
    REQUIRE(!glossary.Parse("en\t苹果\t" + std::string(PersonalGlossary::kMaxFieldBytes + 1, 'a')));
    REQUIRE(!glossary.Parse(std::string("en\t苹果\tapp") + char{0} + "le"));
}

TEST_CASE(personal_glossary_preserves_unicode_and_punctuation)
{
    PersonalGlossary glossary;
    REQUIRE(glossary.Parse("ja\t猫\t猫（ねこ）\nfr\t咖啡\tcafé\nko\t你好\t안녕하세요\n"));
    REQUIRE_EQ(glossary.Lookup("ja", "猫"), std::string("猫（ねこ）"));
    REQUIRE_EQ(glossary.Lookup("fr", "咖啡"), std::string("café"));
    REQUIRE_EQ(glossary.Lookup("ko", "你好"), std::string("안녕하세요"));
}
TEST_CASE(personal_glossary_new_candidate_languages_preserve_user_override)
{
    PersonalGlossary glossary;
    REQUIRE(glossary.Parse("th\t你好\tสวัสดี\nit\t你好\tciao\nvi\t你好\txin chào"));
    REQUIRE_EQ(glossary.Lookup("th", "你好"), std::string("สวัสดี"));
    REQUIRE_EQ(glossary.Lookup("it", "你好"), std::string("ciao"));
    REQUIRE_EQ(glossary.Lookup("vi", "你好"), std::string("xin chào"));
    REQUIRE(glossary.Lookup("en", "你好").empty());
}
TEST_CASE(personal_glossary_full_senses_survive_compatible_multilingual_export)
{
    PersonalGlossary glossary;
    const std::string exported = "en\t你好\thello\thi; how do you do; welcome; greetings; salutations; good day";
    REQUIRE(glossary.Parse(exported));
    REQUIRE_EQ(glossary.Source(), exported);
    REQUIRE_EQ(glossary.Lookup("en", "你好"),
               std::string("hello; hi; how do you do; welcome; greetings; salutations; good day"));
    PersonalGlossary reimported;
    REQUIRE(reimported.Parse(glossary.Source()));
    REQUIRE_EQ(reimported.Lookup("en", "你好"), glossary.Lookup("en", "你好"));
}
