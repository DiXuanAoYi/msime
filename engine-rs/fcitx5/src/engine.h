// 水杉输入法的 fcitx5 引擎。按键翻译在 key_map，引擎会话在 driver，这里只管 fcitx5 的接口：
// 收按键、上屏、更新预编辑和候选窗。
#pragma once
#include "driver.h"

#include <fcitx-config/configuration.h>
#include <fcitx-config/iniparser.h>
#include <fcitx-utils/i18n.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/instance.h>

#include <memory>

namespace msime
{
FCITX_CONFIGURATION(
    MsimeConfig,
    fcitx::Option<std::string> scheme{this, "Scheme", _("Input scheme (quanpin, shuangpin, wubi, japanese)"), "quanpin"};
    fcitx::Option<bool> helpcode{this, "Helpcode", _("Enable helpcodes"), true};
    fcitx::Option<bool> learning{this, "Learning", _("Learn selected candidates"), true};
    fcitx::Option<int, fcitx::IntConstrain> pageSize{this, "PageSize", _("Candidates per page"), 7, fcitx::IntConstrain(3, 9)};
    fcitx::Option<std::string> resourcesDir{this, "ResourcesDir", _("Resources directory (dictionaries, models)"), "/usr/share/msime"};);

class MsimeEngine : public fcitx::InputMethodEngineV2
{
  public:
    explicit MsimeEngine(fcitx::Instance *instance);

    void keyEvent(const fcitx::InputMethodEntry &entry, fcitx::KeyEvent &keyEvent) override;
    void activate(const fcitx::InputMethodEntry &entry, fcitx::InputContextEvent &event) override;
    void deactivate(const fcitx::InputMethodEntry &entry, fcitx::InputContextEvent &event) override;
    void reset(const fcitx::InputMethodEntry &entry, fcitx::InputContextEvent &event) override;

    const fcitx::Configuration *getConfig() const override
    {
        return &config_;
    }
    void setConfig(const fcitx::RawConfig &raw) override;
    void reloadConfig() override;

    // 候选窗里鼠标点选某个候选。
    void selectCandidate(fcitx::InputContext *ic, std::size_t index);

  private:
    void ensureDriver();
    void refresh(fcitx::InputContext *ic);

    fcitx::Instance *instance_;
    MsimeConfig config_;
    std::unique_ptr<Driver> driver_;
};

class MsimeEngineFactory : public fcitx::AddonFactory
{
  public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override
    {
        return new MsimeEngine(manager->instance());
    }
};
} // namespace msime
