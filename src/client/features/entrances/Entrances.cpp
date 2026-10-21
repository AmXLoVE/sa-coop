// Entrances (shops, barbers, houses) open and close as in the host's game. Its missions open some for their time
// (the prologue closes the barbers, "Ryder" opens one for the haircut): the partner's game does not run those
// missions and kept them shut. Each entrance keeps its state in its flags (they are what a save stores), and the host
// sends them when they change, except the bits about where this game's player went in. An entrance is found by its
// name and place: with other mods (more interiors) or entrances a script made later its slot here can differ.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"

namespace coop {

namespace {
using namespace game;

constexpr uintptr_t kEnexPool = 0x96A7D8;  // CEntryExitManager::mp_poolEntryExits, CEntryExit 0x3C bytes
constexpr uint16_t kOwnBits = 0x2000 | 0x10;  // ENTERED_WITHOUT_EXIT, USED_REWARD_ENTRANCE: this player's history

CPoolHeader* Pool() { return Ref<CPoolHeader*>(kEnexPool); }
uintptr_t Entry(CPoolHeader* pool, int i) { return (uintptr_t)pool->objects + (uintptr_t)i * 0x3C; }  // +0 m_szName[8]
uint16_t* Flags(CPoolHeader* pool, int i) { return &Ref<uint16_t>(Entry(pool, i) + 0x30); }
void Center(uintptr_t e, float& x, float& y) {  // of CEntryExit::m_recEntrance
    const float* r = &Ref<float>(e + 0x08);
    x = (r[0] + r[2]) * 0.5f;
    y = (r[1] + r[3]) * 0.5f;
}

std::vector<int32_t> g_sent;  // host: what the partner has, -1 for a free slot
uint64_t g_scanMs = 0;

// partner: the flags the host sent, and the doors kept open here (see KeepWayOut)
constexpr uint16_t kEnableAccess = 0x4000, kDisableExit = 0x800;
std::map<int, uint16_t> g_hostFlags;
std::map<int, bool> g_keptOpen;
uint64_t g_wayOutMs = 0;

// The host's mission may shut the doors of an interior for its scene and take its player out by script ("Ryder":
// the pizzeria during the robbery). The host's flags came here, but nothing took the partners out: they stayed shut in.
// A partner in an interior the host is not in can always go out through its doors; once it is out (or the host is
// in there with it) those doors are as the host has them again.
void KeepWayOut() {
    CPoolHeader* pool = Pool();
    if (!pool || NowMs() - g_wayOutMs < 500) return;
    g_wayOutMs = NowMs();
    CPlayerPed* me = FindPlayerPed(0);
    int8_t here = me ? me->AreaCode() : 0;
    const RemotePlayer* host = Players::Instance().Get(Net().HostId());
    bool alone = here != 0 && host && host->hasState && host->state.area != here;
    for (int i = 0; i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        uint16_t* f = Flags(pool, i);
        // CEntryExit::m_nArea: the door is in an interior of this number. Many interiors share a number (they sit apart
        // on the map): only the doors around the player are this one's
        float ex, ey;
        Center(Entry(pool, i), ex, ey);
        bool inside = Ref<int8_t>(Entry(pool, i) + 0x32) == here && me &&
                      (CVector{ex, ey, 0} - CVector{me->GetPosition().x, me->GetPosition().y, 0}).Length() < 60.0f;
        if (alone && inside) {
            uint16_t open = (uint16_t)((*f | kEnableAccess) & ~kDisableExit);
            if (open != *f) {
                if (!g_keptOpen.count(i)) LOGI("Entrance %d (%.8s) shut by the host's game: kept open, the host is not in here", i, (const char*)Entry(pool, i));
                g_keptOpen[i] = true;
                *f = open;
            }
        } else if (g_keptOpen.erase(i)) {
            if (auto it = g_hostFlags.find(i); it != g_hostFlags.end()) *f = (uint16_t)((*f & kOwnBits) | (it->second & ~kOwnBits));
        }
    }
}

class Entrances : public Module {
public:
    const char* Name() const override { return "Entrances"; }

    void OnInstallHooks() override {
        Net().SetHandler(proto::G_ENEX, [](uint8_t from, Reader& r) {
            int n = r.U16();
            CPoolHeader* pool = Pool();
            if (from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady() || !pool) return;
            for (int k = 0; k < n; ++k) {
                char name[8];
                r.Raw(name, 8);
                float x = r.F32(), y = r.F32();
                uint16_t flags = r.U16();
                if (!r.Ok()) return;
                for (int i = 0; i < pool->size; ++i) {
                    float ex, ey;
                    if (pool->byteMap[i] < 0 || _strnicmp(name, (const char*)Entry(pool, i), 8)) continue;
                    Center(Entry(pool, i), ex, ey);
                    if (std::fabs(ex - x) > 2.0f || std::fabs(ey - y) > 2.0f) continue;
                    uint16_t* f = Flags(pool, i);
                    if (n < 10) LOGD("Entrance %d (%.8s): flags %04X -> %04X", i, name, *f, flags);
                    *f = (uint16_t)((*f & kOwnBits) | (flags & ~kOwnBits));
                    g_hostFlags[i] = flags;
                    g_keptOpen.erase(i);  // the next check opens it again if it still has to be
                    break;
                }
            }
            LOGD("Entrances: %d from the host", n);
        });
    }
    void OnSessionEnd() override { g_sent.clear(), g_hostFlags.clear(), g_keptOpen.clear(); }
    void OnGameStarted() override { g_sent.clear(), g_hostFlags.clear(), g_keptOpen.clear(); }
    void OnPeerResync(uint8_t) override { g_sent.clear(); }  // the next scan sends them all

    void OnPostProcess() override {
        CPoolHeader* pool = Pool();
        if (Net().Online() && !Net().IsHost() && core::IsPlayerReady()) KeepWayOut();
        if (!Net().Online() || !Net().IsHost() || !core::IsPlayerReady() || !pool || NowMs() - g_scanMs < 500) return;
        g_scanMs = NowMs();
        bool all = g_sent.size() != (size_t)pool->size;
        g_sent.resize(pool->size, -1);
        std::vector<std::pair<uint16_t, uint16_t>> changed;
        for (int i = 0; i < pool->size; ++i) {
            int32_t now = pool->byteMap[i] < 0 ? -1 : (*Flags(pool, i) & ~kOwnBits);
            if (!all && now == g_sent[i]) continue;
            g_sent[i] = now;
            if (now >= 0) changed.emplace_back((uint16_t)i, (uint16_t)now);
        }
        if (changed.empty() || Net().Peers().empty()) return;
        for (size_t at = 0; at < changed.size(); at += 200) {
            size_t n = std::min<size_t>(200, changed.size() - at);
            Writer w(proto::G_ENEX);
            w.U16((uint16_t)n);
            for (size_t k = at; k < at + n; ++k) {
                uintptr_t e = Entry(pool, changed[k].first);
                float x, y;
                Center(e, x, y);
                w.Raw((const void*)e, 8);
                w.F32(x);
                w.F32(y);
                w.U16(changed[k].second);
            }
            Net().Send(proto::CH_RELIABLE, w);
        }
        if (!all) LOGD("Entrances: %zu changed", changed.size());  // mostly entrances the map streams in and out
    }

    static Entrances& Instance() {
        static Entrances m;
        return m;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Entrances::Instance()); }
} g_reg;
}  // namespace

}  // namespace coop
