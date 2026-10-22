// The emote wheel: while its key is held ([keys] emote_wheel, J), the emotes stand in a ring around the middle of the
// screen and the mouse picks one by its direction, as the weapon wheel of the later games; letting the key go plays it.
// The mouse is ours meanwhile (no camera turn, no shots), the keyboard stays the game's: the player can walk on.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "common/lib/util/Util.h"
#include "features/anims/Emotes.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/input/Input.h"
#include "shared/ui/renderer/Renderer.h"
#include "shared/ui/widgets/Widgets.h"

namespace coop {

namespace {
constexpr float kPi = 3.14159265f;

class EmoteWheel : public Module {
public:
    const char* Name() const override { return "EmoteWheel"; }

    void OnPostProcess() override {
        bool held = game::IsForeground() && (GetAsyncKeyState(Cfg().emoteKey) & 0x8000);
        bool pressed = held && !m_held;
        m_held = held;
        bool usable = core::IsPlayerReady() && !game::MenuActive() && !ui::TheChat().IsOpen() && !core::ConnectMenuVisible();
        if (!m_open) {
            if (!pressed || !usable) return;
            if (const char* why = emotes::Blocked()) {
                if (*why) ui::TheChat().AddError(why);
                return;
            }
            m_items.clear();
            for (int i = 0; i < emotes::Count(); ++i)
                if (emotes::Get(i).wheel) m_items.push_back(i);
            if (m_items.empty()) return;
            m_open = true;
            m_pick = -1;
            ui::In().SetMouseCapture(true);  // the cursor goes to the middle
            return;
        }
        if (!usable) return Close(false);
        m_pick = Hovered();
        if (!held) Close(true);
    }

    void OnRender() override {
        if (!m_open) return;
        auto& g = ui::Gfx();
        float s = g.Scale(), cx = g.Width() * 0.5f, cy = g.Height() * 0.5f, r = 300 * s;
        float bw = 230 * s, bh = 46 * s;
        g.Rect(0, 0, g.Width(), g.Height(), 0x50000000);
        int n = (int)m_items.size();
        for (int k = 0; k < n; ++k) {
            float a = 2 * kPi * k / n, x = cx + r * std::sin(a), y = cy - r * std::cos(a);
            bool lit = k == m_pick;
            g.Rect(x - bw / 2, y - bh / 2, bw, bh, lit ? 0xE8000000 : 0xC0000000);
            g.RectOutline(x - bw / 2, y - bh / 2, bw, bh, lit ? ui::w::kSaBlue : ui::w::kSaLine, std::max(1.0f, (lit ? 3 : 1) * s));
            const emotes::Emote& e = emotes::Get(m_items[k]);
            std::string label = emotes::Name(m_items[k]) + (e.looped ? " ∞" : "");
            if (m_items[k] == emotes::Current()) label = "{5FA04E}" + label;
            g.Text(ui::FONT_NORMAL, x, y - g.LineHeight(ui::FONT_NORMAL) * 0.5f, label, lit ? ui::w::kSaBlue : 0xFFE0E0E0,
                   ui::TEXT_CENTER | ui::TEXT_SHADOW);
        }
        // the middle: what the mouse points at and a dot on the way to it
        float lh = g.LineHeight(ui::FONT_MENU_SMALL);
        g.Text(ui::FONT_MENU_SMALL, cx, cy - lh * 1.6f, TR("Эмоции", "Emotes"), ui::w::kSaMuted, ui::TEXT_CENTER | ui::TEXT_SHADOW);
        std::string what = m_pick >= 0 ? emotes::Name(m_items[m_pick]) : TR("Наведите мышь", "Point the mouse");
        g.Text(ui::FONT_NORMAL, cx, cy - g.LineHeight(ui::FONT_NORMAL) * 0.5f, what, m_pick >= 0 ? ui::w::kSaBlue : 0xFFE0E0E0,
               ui::TEXT_CENTER | ui::TEXT_SHADOW);
        std::string key = emotes::KeyName(Cfg().emoteKey);
        g.Text(ui::FONT_SMALL, cx, cy + lh * 0.9f,
               m_pick >= 0 ? Format(TR("Отпустите %s, чтобы начать", "Release %s to play"), key.c_str())
                           : TR("В середине отмена", "The middle cancels"),
               ui::w::kSaMuted, ui::TEXT_CENTER | ui::TEXT_SHADOW);
        if (m_pick >= 0) {
            float a = 2 * kPi * m_pick / n, d = r * 0.62f, dot = 10 * s;
            g.Rect(cx + d * std::sin(a) - dot / 2, cy - d * std::cos(a) - dot / 2, dot, dot, ui::w::kSaBlue);
        }
    }

    void OnGameShutdown() override { Close(false); }

private:
    // the item the mouse points at from the middle of the screen, -1 near the middle
    int Hovered() const {
        auto& g = ui::Gfx();
        auto& in = ui::In().State();
        float dx = in.mouseX - g.Width() * 0.5f, dy = in.mouseY - g.Height() * 0.5f;
        if (dx * dx + dy * dy < (60 * g.Scale()) * (60 * g.Scale())) return -1;
        int n = (int)m_items.size();
        float a = std::atan2(dx, -dy);  // 0 at the top, clockwise
        if (a < 0) a += 2 * kPi;
        return (int)std::lround(a / (2 * kPi / n)) % n;
    }

    void Close(bool play) {
        if (!m_open) return;
        m_open = false;
        if (!core::ConnectMenuVisible()) ui::In().SetMouseCapture(false);
        if (play && m_pick >= 0) emotes::Play(m_items[m_pick]);
        m_pick = -1;
    }

    bool m_held = false, m_open = false;
    int m_pick = -1;
    std::vector<int> m_items;  // emote indexes, clockwise from the top
};

EmoteWheel g_wheel;
struct Reg {
    Reg() { core::RegisterModule(&g_wheel); }
} g_reg;
}  // namespace

}  // namespace coop
