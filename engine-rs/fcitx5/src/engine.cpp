#include "engine.h"

#include <fcitx-utils/log.h>
#include <fcitx-utils/standardpath.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace msime
{
namespace
{
class MsimeCandidate : public fcitx::CandidateWord
{
  public:
    MsimeCandidate(MsimeEngine *engine, std::size_t index, const Candidate &candidate) : fcitx::CandidateWord(fcitx::Text(candidate.text)), engine_(engine), index_(index)
    {
        if (!candidate.pinyin.empty())
            setComment(fcitx::Text(candidate.pinyin));
    }

    void select(fcitx::InputContext *ic) const override
    {
        engine_->selectCandidate(ic, index_);
    }

  private:
    MsimeEngine *engine_;
    std::size_t index_;
};

std::string subdirectory(fcitx::StandardPath::Type type)
{
    const auto base = fcitx::StandardPath::global().userDirectory(type);
    return (std::filesystem::path(base) / "fcitx5" / "msime").string();
}
} // namespace

MsimeEngine::MsimeEngine(fcitx::Instance *instance) : instance_(instance)
{
    reloadConfig();
}

void MsimeEngine::reloadConfig()
{
    fcitx::readAsIni(config_, "conf/msime.conf");
    driver_.reset();
}

void MsimeEngine::setConfig(const fcitx::RawConfig &raw)
{
    config_.load(raw, true);
    fcitx::safeSaveAsIni(config_, "conf/msime.conf");
    driver_.reset();
}

void MsimeEngine::ensureDriver()
{
    if (driver_)
        return;
    Options options;
    // 环境变量优先于配置：便携安装和自动测试用（测试环境里 fcitx5 的用户目录是故意不可写的）。
    const auto fromEnvironment = [](const char *name, const std::string &fallback) {
        const char *value = std::getenv(name);
        return value && *value ? std::string(value) : fallback;
    };
    options.resourcesDir = fromEnvironment("MSIME_RESOURCES_DIR", *config_.resourcesDir);
    options.userDataDir = fromEnvironment("MSIME_USER_DATA_DIR", subdirectory(fcitx::StandardPath::Type::Data));
    options.cacheDir = fromEnvironment("MSIME_CACHE_DIR", subdirectory(fcitx::StandardPath::Type::Cache));
    options.scheme = *config_.scheme;
    options.helpcode = *config_.helpcode;
    options.learning = *config_.learning;
    driver_ = std::make_unique<Driver>(options);
    if (!driver_->ok())
        FCITX_ERROR() << "msime: " << driver_->error();
}

void MsimeEngine::activate(const fcitx::InputMethodEntry &, fcitx::InputContextEvent &event)
{
    ensureDriver();
    // 会话只有一个，换了输入上下文就丢掉上一个上下文留下的组合。
    if (driver_->ok())
        driver_->reset();
    refresh(event.inputContext());
}

void MsimeEngine::deactivate(const fcitx::InputMethodEntry &, fcitx::InputContextEvent &event)
{
    if (driver_ && driver_->ok())
        driver_->reset();
    refresh(event.inputContext());
}

void MsimeEngine::reset(const fcitx::InputMethodEntry &, fcitx::InputContextEvent &event)
{
    if (driver_ && driver_->ok())
        driver_->reset();
    refresh(event.inputContext());
}

void MsimeEngine::refresh(fcitx::InputContext *ic)
{
    auto &panel = ic->inputPanel();
    panel.reset();
    if (driver_ && driver_->ok())
    {
        const Snapshot snapshot = driver_->snapshot();
        if (snapshot.composing())
        {
            fcitx::Text preedit;
            preedit.append(snapshot.preedit, fcitx::TextFormatFlag::Underline);
            preedit.setCursor(static_cast<int>(snapshot.preedit.size()));
            if (ic->capabilityFlags().test(fcitx::CapabilityFlag::Preedit))
                panel.setClientPreedit(preedit);
            else
                panel.setPreedit(preedit);

            auto list = std::make_unique<fcitx::CommonCandidateList>();
            list->setPageSize(*config_.pageSize);
            list->setSelectionKey(fcitx::Key::keyListFromString("1 2 3 4 5 6 7 8 9"));
            list->setCursorPositionAfterPaging(fcitx::CursorPositionAfterPaging::ResetToFirst);
            for (std::size_t i = 0; i < snapshot.candidates.size(); ++i)
                list->append<MsimeCandidate>(this, i, snapshot.candidates[i]);
            if (list->size() > 0)
                list->setGlobalCursorIndex(0);
            panel.setCandidateList(std::move(list));
        }
    }
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void MsimeEngine::selectCandidate(fcitx::InputContext *ic, std::size_t index)
{
    if (!driver_ || !driver_->ok())
        return;
    const Outcome outcome = driver_->select(index);
    if (!outcome.commit.empty())
        ic->commitString(outcome.commit);
    refresh(ic);
}

void MsimeEngine::keyEvent(const fcitx::InputMethodEntry &, fcitx::KeyEvent &keyEvent)
{
    if (keyEvent.isRelease())
        return;
    ensureDriver();
    if (!driver_->ok())
        return;
    auto *ic = keyEvent.inputContext();

    const auto states = keyEvent.key().states();
    Modifiers modifiers;
    modifiers.shift = states.test(fcitx::KeyState::Shift);
    modifiers.control = states.test(fcitx::KeyState::Ctrl);
    modifiers.alt = states.test(fcitx::KeyState::Alt);
    modifiers.super = states.test(fcitx::KeyState::Super);

    const bool composing = driver_->snapshot().composing();
    const Action action = mapKey(static_cast<std::uint32_t>(keyEvent.key().sym()), modifiers, composing);
    if (action.kind == ActionKind::PassThrough)
        return;

    auto *common = dynamic_cast<fcitx::CommonCandidateList *>(ic->inputPanel().candidateList().get());
    std::size_t pageStart = 0;
    std::size_t highlighted = 0;
    if (common)
    {
        pageStart = static_cast<std::size_t>(common->currentPage()) * static_cast<std::size_t>(common->size() ? *config_.pageSize : 0);
        highlighted = static_cast<std::size_t>(std::max(0, common->globalCursorIndex()));
    }

    if (action.kind == ActionKind::PageUp || action.kind == ActionKind::PageDown)
    {
        if (common)
        {
            if (action.kind == ActionKind::PageUp && common->hasPrev())
                common->prev();
            else if (action.kind == ActionKind::PageDown && common->hasNext())
                common->next();
            ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        }
        keyEvent.filterAndAccept();
        return;
    }

    const Outcome outcome = driver_->apply(action, pageStart, highlighted);
    if (!outcome.handled)
        return;
    keyEvent.filterAndAccept();
    if (!outcome.commit.empty())
        ic->commitString(outcome.commit);
    refresh(ic);
}
} // namespace msime

FCITX_ADDON_FACTORY(msime::MsimeEngineFactory);
