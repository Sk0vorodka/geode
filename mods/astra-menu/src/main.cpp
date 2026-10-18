// Astra Menu - personal utility menu for GD 2.2091 (Geode v6 SDK).
// Private build for personal use only; not published to the Geode index.
//
// v0.2.0:
//   - full-screen overlay menu toggled with TAB (main menu, levels, pause)
//   - Noclip toggle, Speed cycling, Practice Mode toggle, Instant Restart
//   - plain CCLayerColor overlay: NO FLAlertLayer, NO keyboard delegate
//     registration, no Popup lifetimes (v0.1's FLAlertLayer popup was the
//     source of a keyboard-delegate use-after-free crash on 2.2091).
//
// API notes verified against Geode v6 headers + the 2.2091 bindings dataset:
//   - PlayLayer::update does not exist in 2.2091; the frame entry point is
//     GJBaseGameLayer::update (Game classes have win addresses in the
//     dataset; plain cocos calls link through the cocos2d.dll import lib).
//   - CCKeyboardDispatcher::dispatchKeyboardMSG is not win-hookable, so TAB
//     is detected via GetAsyncKeyState polling inside the update hook plus
//     MenuLayer::keyDown on the main menu.
//   - PauseLayer::init(bool) has no win address (inline); the pause hook is
//     customSetup() with direct button injection.
//   - Pause state uses PlayLayer's bound m_isPaused field plus a shadow flag
//     reset on level enter/exit; overlay touch handling mirrors FLAlertLayer:
//     targeted delegate registered early, swallows touches outside the panel,
//     forwards touches inside the panel to the CCMenu (buttons) by returning
//     false there.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <Geode/Geode.hpp>
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

// Active PlayLayer singleton (for Practice/Restart actions).
PlayLayer*& activePL() {
    static PlayLayer* s_pl = nullptr;
    return s_pl;
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

// Overlay menu ----------------------------------------------------------------

class AstraOverlay : public cocos2d::CCLayerColor {
protected:
    ButtonSprite* m_noclipSpr = nullptr;
    ButtonSprite* m_speedSpr = nullptr;
    ButtonSprite* m_practiceSpr = nullptr;
    cocos2d::CCMenu* m_menu = nullptr;

    static const char* noclipText() {
        return astra::noclipOn() ? "Noclip: ON" : "Noclip: OFF";
    }

    static const char* practiceText() {
        auto pl = astra::activePL();
        if (!pl) return "Practice: -";
        return pl->m_isPracticeMode ? "Practice: ON" : "Practice: OFF";
    }

    bool init() override {
        auto win = cocos2d::CCDirector::get()->getWinSize();
        if (!CCLayerColor::initWithColor(cocos2d::ccc4(0, 0, 0, 150), win.width, win.height)) {
            return false;
        }
        this->setTouchEnabled(true); // triggers registerWithTouchDispatcher on enter

        // Panel ----------------------------------------------------------------
        auto panel = cocos2d::CCLayerColor::create(cocos2d::ccc4(24, 20, 34, 235), 300.f, 290.f);
        panel->ignoreAnchorPointForPosition(false);
        panel->setAnchorPoint({ 0.5f, 0.5f });
        panel->setPosition({ win.width * 0.5f, win.height * 0.5f });
        this->addChild(panel, 1, 42);

        auto title = CCLabelBMFont::create("Astra Menu", "bigFont.fnt");
        title->setScale(0.55f);
        title->setPosition({ 150.f, 258.f });
        panel->addChild(title);

        auto hint = CCLabelBMFont::create("TAB toggles this menu", "bigFont.fnt");
        hint->setScale(0.28f);
        hint->setOpacity(140);
        hint->setPosition({ 150.f, 232.f });
        panel->addChild(hint);

        // Buttons --------------------------------------------------------------
        m_noclipSpr = ButtonSprite::create(noclipText(), "bigFont.fnt", "GJ_button_05.png", 0.9f);
        m_noclipSpr->setScale(0.85f);
        auto noclipBtn = CCMenuItemSpriteExtra::create(
            m_noclipSpr, this, menu_selector(AstraOverlay::onNoclip)
        );

        m_speedSpr = ButtonSprite::create(
            fmt::format("Speed: x{}", astra::speedMult()).c_str(),
            "bigFont.fnt", "GJ_button_01.png", 0.9f
        );
        m_speedSpr->setScale(0.85f);
        auto speedBtn = CCMenuItemSpriteExtra::create(
            m_speedSpr, this, menu_selector(AstraOverlay::onSpeedCycle)
        );

        m_practiceSpr = ButtonSprite::create(practiceText(), "bigFont.fnt", "GJ_button_03.png", 0.9f);
        m_practiceSpr->setScale(0.85f);
        auto practiceBtn = CCMenuItemSpriteExtra::create(
            m_practiceSpr, this, menu_selector(AstraOverlay::onPracticeToggle)
        );

        auto restartSpr = ButtonSprite::create("Restart", "bigFont.fnt", "GJ_button_04.png", 0.9f);
        restartSpr->setScale(0.85f);
        auto restartBtn = CCMenuItemSpriteExtra::create(
            restartSpr, this, menu_selector(AstraOverlay::onRestart)
        );

        auto closeSpr = ButtonSprite::create("Close", "bigFont.fnt", "GJ_button_02.png", 0.9f);
        closeSpr->setScale(0.85f);
        auto closeBtn = CCMenuItemSpriteExtra::create(
            closeSpr, this, menu_selector(AstraOverlay::onClose)
        );

        // The menu covers the panel: coordinates are mechanic-relative to it, so
        // it must sit at the panel center with the panel's content size.
        m_menu = cocos2d::CCMenu::create();
        m_menu->ignoreAnchorPointForPosition(false);
        m_menu->setContentSize({ 300.f, 290.f });
        m_menu->setPosition(panel->getPosition());
        this->addChild(m_menu, 2, 43);

        m_menu->addChildAtPosition(noclipBtn, Anchor::Center, { 0.f, 78.f });
        m_menu->addChildAtPosition(speedBtn, Anchor::Center, { 0.f, 30.f });
        m_menu->addChildAtPosition(practiceBtn, Anchor::Center, { 0.f, -18.f });
        m_menu->addChildAtPosition(restartBtn, Anchor::Center, { -62.f, -68.f });
        m_menu->addChildAtPosition(closeBtn, Anchor::Center, { 62.f, -68.f });

        return true;
    }

    // Touch routing mirrors FLAlertLayer's pattern ... but on a plain layer:
    // registered first (lower priority than menus), swallows everything
    // outside the panel; inside the panel touches are forwarded to the CCMenu
    // so buttons keep working and empty panel spots swallow nothing.
    void registerWithTouchDispatcher() override {
        cocos2d::CCTouchDispatcher::sharedDispatcher()
            ->addTargetedDelegate(this, -130, true);
    }

    bool ccTouchBegan(cocos2d::CCTouch* touch, cocos2d::CCEvent*) override {
        auto pos = touch->getLocation();
        if (m_menu && m_menu->boundingBox().containsPoint(pos)) {
            return false; // let the CCMenu buttons handle panel-internal touches
        }
        return true; // swallow gameplay/menus while the menu is open
    }

    void refreshAll() {
        if (m_noclipSpr) {
            m_noclipSpr->setString(noclipText());
        }
        if (m_speedSpr) {
            m_speedSpr->setString(
                fmt::format("Speed: x{}", astra::speedMult()).c_str()
            );
        }
        if (m_practiceSpr) {
            m_practiceSpr->setString(practiceText());
        }
    }

    void onNoclip(CCObject*) {
        astra::setNoclipOn(!astra::noclipOn());
        this->refreshAll();
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
        astra::setSpeedMult(astra::SPEED_STEPS[(idx + 1) % std::size(astra::SPEED_STEPS)]);
        this->refreshAll();
    }

    void onPracticeToggle(CCObject*) {
        if (auto pl = astra::activePL()) {
            pl->togglePracticeMode(!pl->m_isPracticeMode);
            this->refreshAll();
        }
    }

    void onRestart(CCObject*) {
        if (auto pl = astra::activePL()) {
            pl->resetLevel();
        }
    }

    void onClose(CCObject*) {
        astra::toggleOverlay();
    }

public:
    static AstraOverlay* create() {
        auto ret = new AstraOverlay();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

namespace astra {

void toggleOverlay() {
    auto scene = cocos2d::CCDirector::get()->getRunningScene();
    if (!scene) return;
    if (auto old = scene->getChildByID("sk0v.astra-menu/overlay"_s)) {
        old->removeFromParent();
        return;
    }
    auto overlay = AstraOverlay::create();
    if (!overlay) return;
    overlay->setID("sk0v.astra-menu/overlay"_s);
    scene->addChild(overlay, 9999);
}

// Edge-detected TAB polling (safe on every frame in levels and pause).
void pollTabKey() {
    static bool s_prev = false;
    bool down = (GetAsyncKeyState(VK_TAB) & 0x8000) != 0;
    if (down && !s_prev) {
        toggleOverlay();
    }
    s_prev = down;
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

    void keyDown(cocos2d::enumKeyCodes key, double timestamp) {
        if (key == cocos2d::KEY_Tab) {
            astra::toggleOverlay();
            return;
        }
        MenuLayer::keyDown(key, timestamp);
    }

    void onAstra(CCObject* sender) {
        astra::toggleOverlay();
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
        astra::toggleOverlay();
    }
};

// PlayLayer -------------------------------------------------------------------

class $modify(AstraPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool dontCreateObjects) {
        astra::levelPaused() = false;
        astra::activePL() = this;
        return PlayLayer::init(level, dontCreateObjects);
    }

    void onExit() {
        astra::levelPaused() = false;
        astra::activePL() = nullptr;
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
        astra::pollTabKey(); // TAB opens/closes the menu anywhere mid-level

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
