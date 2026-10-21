// Time of day and weather follow the host: the host broadcasts them every second, the others correct
// their clock when it drifts (between updates every game advances its own clock at the same rate).
#include <algorithm>
#include <cstdlib>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/session/Flow.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

uint8_t& ClockHours() { return Ref<uint8_t>(0xB70153); }    // CClock::ms_nGameClockHours
uint8_t& ClockMinutes() { return Ref<uint8_t>(0xB70152); }  // CClock::ms_nGameClockMinutes
uint8_t& ClockWeekday() { return Ref<uint8_t>(0xB7014E); }  // CClock::CurrentDay
uint32_t& LastClockTick() { return Ref<uint32_t>(0xB70158); }  // CClock::ms_nLastClockTick (CTimer ms)
uint32_t MsPerGameMinute() { return Ref<uint32_t>(0xB7015C); }
int16_t& OldWeather() { return Ref<int16_t>(0xC81320); }
int16_t& NewWeather() { return Ref<int16_t>(0xC8131C); }
int16_t& ForcedWeather() { return Ref<int16_t>(0xC81318); }
float& WeatherInterpolation() { return Ref<float>(0xC8130C); }

constexpr int kNumWeatherTypes = 23;

// CGameLogic::PassTime moves the clock on after a death or an arrest (12 h), a save (6 h), a trip or a train skip. The
// world's clock is the host's: on the partner the host's clock took it back a second later and the light jumped twice
// (a moment of night after a respawn by day). Its days passed and its pickups still count.
using SetClockFn = void(__cdecl*)(uint8_t, uint8_t, uint8_t);
SetClockFn g_origSetClock = nullptr;
void __cdecl HookPassTimeClock(uint8_t hours, uint8_t minutes, uint8_t day) {
    if (Net().Online() && !Net().IsHost() && flow::InHostWorld()) return;
    // The host's death or arrest (12 h, the only pass of that length) moved the clock of everybody in the middle of
    // their day: while partners play, it does not. A save (6 h) still passes time: players save to wait for the
    // hours of a mission (the night of the burglaries).
    if (Net().Online() && Net().IsHost() && !Net().Peers().empty()) {
        int now = ClockHours() * 60 + ClockMinutes(), then = hours * 60 + minutes;
        if (((then - now) % 1440 + 1440) % 1440 == 720) {
            LOGI("The host's death or arrest does not move the shared clock (%02d:%02d stays)", ClockHours(), ClockMinutes());
            return;
        }
    }
    g_origSetClock(hours, minutes, day);
}

class World : public Module {
public:
    const char* Name() const override { return "World"; }

    void OnInstallHooks() override {
        mem::RedirectCallFn(0x441534, &HookPassTimeClock, &g_origSetClock);  // CGameLogic::PassTime -> CClock::SetGameClock
        Net().SetHandler(proto::G_WORLD_STATE, [](uint8_t from, Reader& r) { Handle(from, r); });
    }

    void OnPostProcess() override {
        if (!Net().IsHost() || !core::IsPlayerReady() || NowMs() - m_lastSend < 1000) return;
        m_lastSend = NowMs();
        Writer w(proto::G_WORLD_STATE);
        w.U8(ClockHours());
        w.U8(ClockMinutes());
        w.U8(ClockWeekday());
        w.U32(TimeMs() - LastClockTick());  // progress inside the current game minute
        w.I16(OldWeather());
        w.I16(NewWeather());
        w.I16(ForcedWeather());
        w.F32(WeatherInterpolation());
        Net().Send(proto::CH_STATE, w);
    }

    static void Handle(uint8_t from, Reader& r) {
        uint8_t h = r.U8(), m = r.U8(), weekday = r.U8();
        uint32_t into = r.U32();
        int16_t oldW = r.I16(), newW = r.I16(), forced = r.I16();
        r.F32();  // the host's InterpolationValue: ours comes from our clock (see below)
        if (!r.Ok() || Net().IsHost() || from != Net().HostId() || !core::IsPlayerReady()) return;
        uint32_t per = MsPerGameMinute();
        if (h > 23 || m > 59 || per == 0) return;
        into = std::min(into, per - 1);  // the host's clock may be frozen (cutscene)

        int64_t dayLen = 24LL * 60 * per;
        int64_t host = ((int64_t)h * 60 + m) * per + into;
        int64_t mine = ((int64_t)ClockHours() * 60 + ClockMinutes()) * per + (TimeMs() - LastClockTick());
        int64_t diff = ((host - mine) % dayLen + dayLen + dayLen / 2) % dayLen - dayLen / 2;  // shortest way round
        if (std::llabs(diff) > per / 2) {
            if (std::llabs(diff) > per * 2) LOGI("Clock set to the host's %02d:%02d (was %02d:%02d)", h, m, ClockHours(), ClockMinutes());
            ClockHours() = h;
            ClockMinutes() = m;
            if (weekday >= 1 && weekday <= 7) ClockWeekday() = weekday;
            LastClockTick() = TimeMs() - into;
            // CWeather::Update (0x72B8A1) takes InterpolationValue going down for a new hour and moves the weather on
            // to its own next one: a clock set back did that, and so did the host's value written here every second
            // (a bit ahead of ours with the network's delay), so the partner's weather and colours flipped each second
            uint32_t seconds = into * 60 / per;  // as CClock::Update counts them
            WeatherInterpolation() = std::max(0.0f, (seconds / 60.0f + m) / 60.0f - 1e-5f);
        }
        // weather ids index fixed tables: never take an out-of-range one from the network. The pair is the host's
        // hour's: at the turn of the hour one game is in the next hour first and keeps its own pair until the other is
        if (h == ClockHours() && oldW >= 0 && oldW < kNumWeatherTypes && newW >= 0 && newW < kNumWeatherTypes) {
            OldWeather() = oldW;
            NewWeather() = newW;
        }
        if (forced >= -1 && forced < kNumWeatherTypes) ForcedWeather() = forced;
    }

private:
    uint64_t m_lastSend = 0;
};

World g_world;
struct Reg {
    Reg() { core::RegisterModule(&g_world); }
} g_reg;
}  // namespace

}  // namespace coop
