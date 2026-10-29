// 前端核心的测试：快照解析、按键映射不需要资源；端到端（打 nihao、翻页换算、选词上屏）走真实的 msime-capi，
// 资源目录取 MSIME_APP_DATA，没有就跳过端到端部分（退出码 77，CTest 记为 skipped）。
#include "driver.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace
{
int failures = 0;

#define CHECK(condition)                                                                                                                                                                               \
    do                                                                                                                                                                                                 \
    {                                                                                                                                                                                                  \
        if (!(condition))                                                                                                                                                                              \
        {                                                                                                                                                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << " 检查失败：" #condition "\n";                                                                                                                 \
            ++failures;                                                                                                                                                                                \
        }                                                                                                                                                                                              \
    } while (0)

void testSnapshot()
{
    msime::Snapshot snapshot;
    CHECK(msime::parseSnapshot(R"({"scheme":"quanpin","preedit":"ni'hao","raw":"nihao","normalized":"ni'hao","caret":5,)"
                               R"("candidates":[{"text":"你好","pinyin":"ni'hao","weight":12,"fixed":0,"fuzzy":false},)"
                               R"({"text":"a\"b\\c你","pinyin":"","weight":-3,"fixed":2,"fuzzy":true}]})",
                               snapshot));
    CHECK(snapshot.scheme == "quanpin");
    CHECK(snapshot.preedit == "ni'hao");
    CHECK(snapshot.caret == 5);
    CHECK(snapshot.candidates.size() == 2);
    CHECK(snapshot.candidates[0].text == "你好");
    CHECK(snapshot.candidates[1].text == "a\"b\\c你");
    CHECK(snapshot.candidates[1].weight == -3 && snapshot.candidates[1].fixed == 2 && snapshot.candidates[1].fuzzy);
    CHECK(snapshot.composing());

    CHECK(!msime::parseSnapshot("{\"scheme\":", snapshot));
    CHECK(!snapshot.composing());
    CHECK(!msime::parseSnapshot("[1,2]", snapshot));
    CHECK(msime::parseSnapshot(R"({"preedit":"","candidates":[]})", snapshot));
    CHECK(!snapshot.composing());
}

void testKeyMap()
{
    using namespace msime;
    CHECK(mapKey('a', {}, false).kind == ActionKind::Character);
    CHECK(mapKey('A', {true, false, false, false}, false).shiftOnly);
    CHECK(!mapKey('A', {}, false).shiftOnly);
    CHECK(mapKey('a', {false, true, false, false}, true).kind == ActionKind::PassThrough);
    CHECK(mapKey(keysym::Space, {}, false).kind == ActionKind::PassThrough);
    CHECK(mapKey(keysym::Space, {}, true).kind == ActionKind::CommitHighlighted);
    CHECK(mapKey('3', {}, false).kind == ActionKind::PassThrough);
    CHECK(mapKey('3', {}, true).kind == ActionKind::CandidateDigit && mapKey('3', {}, true).value == 3);
    CHECK(mapKey(keysym::Return, {}, true).command == Command::CommitRaw);
    CHECK(mapKey(keysym::Return, {}, false).kind == ActionKind::PassThrough);
    CHECK(mapKey(keysym::BackSpace, {}, true).command == Command::Backspace);
    CHECK(mapKey(keysym::PageDown, {}, true).kind == ActionKind::PageDown);
    CHECK(mapKey(keysym::Up, {}, true).kind == ActionKind::Swallow);
    CHECK(mapKey('\'', {}, true).kind == ActionKind::Character);
    CHECK(mapKey(',', {}, false).kind == ActionKind::Punctuation);
}

int testEndToEnd()
{
    const char *resources = std::getenv("MSIME_APP_DATA");
    if (!resources || !std::filesystem::exists(std::filesystem::path(resources) / "msime.db"))
    {
        std::cerr << "没有 MSIME_APP_DATA，跳过端到端部分\n";
        return 77;
    }
    const auto work = std::filesystem::temp_directory_path() / "msime-fcitx5-test";
    std::filesystem::remove_all(work);
    msime::Options options;
    options.resourcesDir = resources;
    options.userDataDir = (work / "user").string();
    options.cacheDir = (work / "cache").string();
    msime::Driver driver(options);
    CHECK(driver.ok());
    if (!driver.ok())
    {
        std::cerr << driver.error() << "\n";
        return 1;
    }
    CHECK(!driver.snapshot().composing());

    for (const char c : std::string("nihao"))
        CHECK(driver.apply(msime::mapKey(static_cast<unsigned char>(c), {}, driver.snapshot().composing())).handled);
    msime::Snapshot snapshot = driver.snapshot();
    CHECK(snapshot.composing());
    CHECK(snapshot.candidates.size() > 3);

    // 第 2 个候选按数字键 2（第一页），上屏文字应等于快照里的第 2 个。
    const std::string second = snapshot.candidates.at(1).text;
    msime::Outcome outcome = driver.apply(msime::mapKey('2', {}, true), 0, 0);
    CHECK(outcome.handled);
    CHECK(outcome.commit.find(second) == 0);

    // 重新输入，空格上屏高亮的候选（序号 0）。
    driver.reset();
    CHECK(!driver.snapshot().composing());
    for (const char c : std::string("nihao"))
        driver.apply(msime::mapKey(static_cast<unsigned char>(c), {}, driver.snapshot().composing()));
    snapshot = driver.snapshot();
    const std::string first = snapshot.candidates.at(0).text;
    outcome = driver.apply(msime::mapKey(msime::keysym::Space, {}, true), 0, 0);
    CHECK(outcome.commit.find(first) == 0);

    // 第二页：pageStart=7 时数字键 1 选总序号 7。
    driver.reset();
    for (const char c : std::string("shi"))
        driver.apply(msime::mapKey(static_cast<unsigned char>(c), {}, driver.snapshot().composing()));
    snapshot = driver.snapshot();
    if (snapshot.candidates.size() > 8)
    {
        const std::string expected = snapshot.candidates[7].text;
        outcome = driver.apply(msime::mapKey('1', {}, true), 7, 0);
        CHECK(!outcome.commit.empty());
        CHECK(outcome.commit.find(expected) == 0);
    }

    // Esc 取消，Backspace 删字符。
    driver.reset();
    driver.apply(msime::mapKey('n', {}, false));
    driver.apply(msime::mapKey('i', {}, true));
    CHECK(driver.snapshot().composing());
    CHECK(driver.apply(msime::mapKey(msime::keysym::BackSpace, {}, true)).handled);
    CHECK(driver.apply(msime::mapKey(msime::keysym::Escape, {}, true)).handled);
    CHECK(!driver.snapshot().composing());
    return 0;
}
} // namespace

int main()
{
    testSnapshot();
    testKeyMap();
    const int endToEnd = testEndToEnd();
    if (failures)
    {
        std::cerr << failures << " 项失败\n";
        return 1;
    }
    std::cerr << "前端核心测试通过" << (endToEnd == 77 ? "（端到端跳过）" : "") << "\n";
    return endToEnd == 77 ? 77 : 0;
}
