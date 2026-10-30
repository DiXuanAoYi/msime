// 引擎集成测试：真实的 fcitx5 Instance，加载构建出来的 libmsime.so，用 testfrontend 敲键，核对上屏。
// 需要 MSIME_APP_DATA（资源目录，含 msime.db 等）；没有就跳过（退出码 77）。
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/testing.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <testfrontend_public.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

using namespace fcitx;

namespace
{
int failures = 0;

void check(bool condition, const char *what)
{
    if (!condition)
    {
        std::cerr << "检查失败：" << what << "\n";
        ++failures;
    }
}
} // namespace

int main()
{
    const char *resources = std::getenv("MSIME_APP_DATA");
    if (!resources || !std::filesystem::exists(std::filesystem::path(resources) / "msime.db"))
    {
        std::cerr << "没有 MSIME_APP_DATA，跳过引擎集成测试\n";
        return 77;
    }
    const auto work = std::filesystem::temp_directory_path() / "msime-fcitx5-engine-test";
    std::filesystem::remove_all(work);
    setenv("MSIME_RESOURCES_DIR", resources, 1);
    setenv("MSIME_USER_DATA_DIR", (work / "user").c_str(), 1);
    setenv("MSIME_CACHE_DIR", (work / "cache").c_str(), 1);

    const std::string binary = FCITX_MSIME_BINARY_DIR;
    setupTestingEnvironment(binary, {binary, FCITX_SYSTEM_ADDON_DIR}, {binary + "/testdata", "/usr/share/fcitx5/testing"});

    char arg0[] = "msime_engine_test";
    char arg1[] = "--disable=all";
    char arg2[] = "--enable=testfrontend,msime";
    char *argv[] = {arg0, arg1, arg2};
    Instance instance(3, argv);
    instance.addonManager().registerDefaultLoader(nullptr);

    EventDispatcher dispatcher;
    dispatcher.attach(&instance.eventLoop());
    dispatcher.schedule([&instance, &dispatcher]() {
        auto *testfrontend = instance.addonManager().addon("testfrontend");
        check(testfrontend != nullptr, "testfrontend 加载");
        if (!testfrontend)
        {
            instance.exit();
            return;
        }
        InputMethodGroup group("Default");
        group.inputMethodList().emplace_back("msime");
        group.setDefaultInputMethod("msime");
        instance.inputMethodManager().setGroup(std::move(group));
        instance.inputMethodManager().setCurrentGroup("Default");

        const auto uuid = testfrontend->call<ITestFrontend::createInputContext>("testapp");
        auto *ic = instance.inputContextManager().findByUUID(uuid);
        check(ic != nullptr, "创建输入上下文");
        if (!ic)
        {
            instance.exit();
            return;
        }
        ic->focusIn();
        check(instance.inputMethod(ic) == "msime", "当前输入法是 msime");

        // 没有组合时空格放行。
        check(!testfrontend->call<ITestFrontend::sendKeyEvent>(uuid, Key(FcitxKey_space), false), "无组合时空格放行");
        // nihao 之后候选窗里应该有候选，预编辑非空。
        for (const char c : std::string("nihao"))
            check(testfrontend->call<ITestFrontend::sendKeyEvent>(uuid, Key(static_cast<KeySym>(c)), false), "字母被接管");
        check(!ic->inputPanel().preedit().toString().empty() || !ic->inputPanel().clientPreedit().toString().empty(), "有预编辑");
        const auto list = ic->inputPanel().candidateList();
        check(list && list->size() > 0, "有候选");
        // 空格上屏第一个候选；它的文字取自候选窗本身。
        if (list && list->size() > 0)
        {
            const std::string first = list->candidate(0).text().toString();
            testfrontend->call<ITestFrontend::pushCommitExpectation>(first);
            check(testfrontend->call<ITestFrontend::sendKeyEvent>(uuid, Key(FcitxKey_space), false), "空格被接管");
        }
        check(!ic->inputPanel().candidateList() || ic->inputPanel().candidateList()->size() == 0, "上屏后候选窗清空");

        // Esc 取消组合，Backspace 回退。
        for (const char c : std::string("ni"))
            testfrontend->call<ITestFrontend::sendKeyEvent>(uuid, Key(static_cast<KeySym>(c)), false);
        check(testfrontend->call<ITestFrontend::sendKeyEvent>(uuid, Key(FcitxKey_BackSpace), false), "Backspace 被接管");
        check(testfrontend->call<ITestFrontend::sendKeyEvent>(uuid, Key(FcitxKey_Escape), false), "Esc 被接管");
        check(!testfrontend->call<ITestFrontend::sendKeyEvent>(uuid, Key(FcitxKey_Escape), false), "取消后 Esc 放行");
        // Ctrl 组合键一律放行。
        check(!testfrontend->call<ITestFrontend::sendKeyEvent>(uuid, Key(FcitxKey_a, KeyState::Ctrl), false), "Ctrl+A 放行");

        testfrontend->call<ITestFrontend::destroyInputContext>(uuid);
        instance.exit();
    });
    instance.exec();
    if (failures)
    {
        std::cerr << failures << " 项失败\n";
        return 1;
    }
    std::cerr << "引擎集成测试通过\n";
    return 0;
}
