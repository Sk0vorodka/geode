// Astra Menu - personal utility menu for GD 2.2091 (Geode v6 SDK).
// Private build for personal use only; not published to the Geode index.
//
// v0.1.0 features:
//   - floating "Astra" button on the main menu and the pause screen
//   - Noclip toggle (cancels player destruction)
//   - Speed cycling presets (x0.25 .. x3) via GJBaseGameLayer::update dt scale
//   - state persisted through Geode saved values
//
// API notes verified against Geode v6 + 2.2091 bindings:
//   - geode::Popup is a plain class here: subclasses implement bool init() and
//     call Popup::init(width, height); buttons go into m_buttonMenu, labels
//     into m_mainLayer (see loader's own UITest popup).
//   - PlayLayer::update does not exist in 2.2091 (only postUpdate), the frame
//     entry point is GJBaseGameLayer::update - PlayLayer inherits it.
//   - PauseLayer::init(bool) has no win address in bindings (inline), so the
//     pause hook is customSetup() with direct button injection (the button
//     menu node is ours and does not depend on base-class UI).
//   - Pause state uses PlayLayer's bound m_isPaused field (accessible inside
//     modify classes) plus a global shadow flag reset on level enter/exit.

#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/Layout.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

using namespace geode::prelude;

namespace astra {

// Saved-value helpers --------------------------------------------------------

constexpr const char* SAVED_NOCLIP = "astra.noclip";
constexpr const char* SAVED_SPEED = "astra.speed";

bool noclipOn() {
    return Mod::get()->getSavedValue<bool>(SAVED_NOCLIP, false);
}

void setNoclipOn(bool on) {
    Mod::get()->setSavedValue<bool>(SAVED_NOCLIP, on);
}

float speedMult() {
    float v = Mod::get()->getSavedValue<float>(SAVED_SPEED, 1.0f);
    if (v < 0.04f || v > 4.0f) v = 1.0f; // safety: ignore corrupted saves
    return v;
}

void setSpeedMult(float v) {
    Mod::get()->setSavedValue<float>(SAVED_SPEED, v);
}

constexpr float SPEED_STEPS[] = { 0.25f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f };

// Global "level is paused" shadow flag: the only cheap cross-class signal,
// maintained from hooks below. Reset on every level enter/exit, so a missed
// resume path can never stay stuck past the next level.
bool& levelPaused() {
    static bool s_paused = false;
    return s_paused;
}

} // namespace astra

// Status popup ----------------------------------------------------------------

class AstraPopup : public Popup {
protected:
    ButtonSprite* m_noclipBtnSpr = nullptr;
    ButtonSprite* m_speedBtnSpr = nullptr;
    CCLabelBMFont* m_hintLabel = nullptr;

    static const char* noclipText() {
        return astra::noclipOn() ? "Noclip: ON" : "Noclip: OFF";
    }

    bool init() override {
        if (!Popup::init(290.f, 200.f, "GJ_square05.png")) return false;

        this->setTitle("Astra Menu");

        // Noclip toggle -------------------------------------------------------
        m_noclipBtnSpr = ButtonSprite::create(
            noclipText(), "bigFont.fnt", "GJ_button_05.png", 0.9f
        );
        m_noclipBtnSpr->setScale(0.9f);
        auto noclipBtn = CCMenuItemSpriteExtra::create(
            m_noclipBtnSpr, this, menu_selector(AstraPopup::onNoclip)
        );
        m_buttonMenu->addChildAtPosition(noclipBtn, Anchor::Center, ccp(0.f, 35.f));

        // Speed cycler --------------------------------------------------------
        m_speedBtnSpr = ButtonSprite::create(
            fmt::format("Speed: x{}", astra::speedMult()).c_str(),
            "bigFont.fnt", "GJ_button_01.png", 0.9f
        );
        m_speedBtnSpr->setScale(0.9f);
        auto speedBtn = CCMenuItemSpriteExtra::create(
            m_speedBtnSpr, this, menu_selector(AstraPopup::onSpeedCycle)
        );
        m_buttonMenu->addChildAtPosition(speedBtn, Anchor::Center, ccp(0.f, -10.f));

        // Hint line -----------------------------------------------------------
        m_hintLabel = CCLabelBMFont::create(
            "Toggles apply while a level is running", "bigFont.fnt"
        );
        m_hintLabel->setScale(0.35f);
        m_hintLabel->setOpacity(150);
        m_mainLayer->addChildAtPosition(m_hintLabel, Anchor::Center, ccp(0.f, -58.f));

        return true;
    }

    void refreshNoclip() {
        if (m_noclipBtnSpr) {
            m_noclipBtnSpr->setString(noclipText());
        }
    }

    void refreshSpeed() {
        if (m_speedBtnSpr) {
            m_speedBtnSpr->setString(
                fmt::format("Speed: x{}", astra::speedMult()).c_str()
            );
        }
        if (m_hintLabel) {
            if (std::abs(astra::speedMult() - 1.0f) > 0.001f) {
                m_hintLabel->setString("Speed is active in levels only");
            } else {
                m_hintLabel->setString("Toggles apply while a level is running");
            }
        }
    }

    void onNoclip(CCObject*) {
        astra::setNoclipOn(!astra::noclipOn());
        this->refreshNoclip();
    }

    void onSpeedCycle(CCObject*) {
        float cur = astra::speedMult();
        size_t idx = 0;
        for (size_t i = 0; i < std::size(astra::SPEED_STEPS); ++i) {
            if (std::abs(astra::SPEED_STEPS[i] - cur) < 0.001f) {
                idx = i;
                break;
            }
        }
        size_t next = (idx + 1) % std::size(astra::SPEED_STEPS);
        astra::setSpeedMult(astra::SPEED_STEPS[next]);
        this->refreshSpeed();
    }

public:
    static AstraPopup* create() {
        auto ret = new AstraPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

namespace astra {

void openPopup() {
    if (auto popup = AstraPopup::create()) {
        popup->show();
    }
}

// Shared button anchor helper. `owner` is the layer that receives the menu and
// acts as the button target (each modify class wires its own member callback).
void placeAstraButton(
    cocos2d::CCNode* owner,
    SEL_MenuHandler callback,
    std::string_view menuId,
    Anchor anchor,
    cocos2d::CCPoint const& offset
) {
    if (!owner || owner->getChildByID(std::string(menuId))) return;

    auto menu = cocos2d::CCMenu::create();
    menu->setID(std::string(menuId));

    auto spr = ButtonSprite::create("Astra", "bigFont.fnt", "GJ_button_02.png", 0.9f);
    spr->setScale(0.8f);
    auto btn = CCMenuItemSpriteExtra::create(spr, owner, callback);
    menu->addChild(btn);

    owner->addChildAtPosition(menu, anchor, offset);
}

} // namespace astra

// Main menu -------------------------------------------------------------------

class $modify(AstraMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        // Bottom-right corner: the Geode loader button lives bottom-left.
        astra::placeAstraButton(
            this,
            menu_selector(AstraMenuLayer::onAstra),
            "sk0v.astra-menu/menu-layer-button",
            Anchor::BottomRight,
            ccp(-30.f, 30.f)
        );
        return true;
    }

    void onAstra(CCObject* sender) {
        astra::openPopup();
    }
};

// Pause screen ----------------------------------------------------------------

class $modify(AstraPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        astra::levelPaused() = true;
        // The button menu node is our own creation, so it does not depend on
        // anything the base class builds later in its init chain.
        astra::placeAstraButton(
            this,
            menu_selector(AstraPauseLayer::onAstra),
            "sk0v.astra-menu/pause-layer-button",
            Anchor::TopRight,
            ccp(-30.f, -30.f)
        );
    }

    // Paths that leave the pause screen drop the shadow flag. onNormalMode is
    // deliberately not hooked: it has no win address in 2.2091 bindings.
    void onResume(cocos2d::CCObject* sender) {
        astra::levelPaused() = false;
        PauseLayer::onResume(sender);
    }
    void keyBackClicked() {
        astra::levelPaused() = false;
        PauseLayer::keyBackClicked();
    }
    void onPracticeMode(cocos2d::CCObject* sender) {
        astra::levelPaused() = false;
        PauseLayer::onPracticeMode(sender);
    }
    void onRestart(cocos2d::CCObject* sender) {
        astra::levelPaused() = false;
        PauseLayer::onRestart(sender);
    }
    void onRestartFull(cocos2d::CCObject* sender) {
        astra::levelPaused() = false;
        PauseLayer::onRestartFull(sender);
    }

    void onAstra(CCObject* sender) {
        astra::openPopup();
    }
};

// PlayLayer -------------------------------------------------------------------

class $modify(AstraPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool dontCreateObjects) {
        astra::levelPaused() = false;
        return PlayLayer::init(level, dontCreateObjects);
    }

    void onExit() {
        astra::levelPaused() = false;
        PlayLayer::onExit();
    }

    // Noclip: while enabled and not paused, never let the player die.
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (astra::noclipOn() && !astra::levelPaused() && !this->m_isPaused) {
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }
};

// Frame entry point -----------------------------------------------------------

class $modify(AstraBaseGameLayer, GJBaseGameLayer) {
    void update(float dt) {
        float speed = astra::speedMult();
        bool active = (speed > 1.001f || speed < 0.999f)
            && typeinfo_cast<PlayLayer*>(this) != nullptr
            && !astra::levelPaused();
        if (active) {
            GJBaseGameLayer::update(dt * speed);
        } else {
            GJBaseGameLayer::update(dt);
        }
    }
};
