// Replication of animations that the pad replay cannot reproduce.
//
// Idle animations: CTaskSimplePlayerOnFoot::PlayIdleAnimations (0x6872C0) starts a random
// "playidles" animation after 20 s of inactivity, judged by the *local* pad touch timer. For the
// remote player ped this would use the wrong timer and a different random choice, so the function is
// skipped for it; instead the owner reports the idle animation it plays and the other side blends the
// same animation on the remote ped.
//
// The same channel (G_PLAYER_ANIM) is used for other one-shot animations (see kReplicatedGroups).
//
// Script animations (G_SCRIPT_ANIM): what a script plays on the player through an animation task (TASK_PLAY_ANIM:
// the barber's chair, eating, the gym, drinks, the parachute) does not come from the pad either. While the local
// player runs such a task its animation (IFP block and index, flags, time) goes to the others, who play it on the
// remote player's copy the way the NPC copies play theirs (Peds), and fade it out when it ends. An animation blended by
// name outside such a task (no animation group, so not from the pad: a vending machine) counts the same.
//
// The peds a shop's script creates (the cashier, the barber) are not networked: every game has its own at the same
// spot. What a script plays on ours near a partner (G_PED_SCRIPT_ANIM) is played on the others' ped of that model there.
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <tuple>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Anim.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

constexpr int ANIM_GROUP_PLAYIDLES = 49;
constexpr uint16_t ANIMATION_IDLE_FLAG = 0x200;  // set by PlayIdleAnimations on its animations

enum : uint8_t { OP_PLAY = 1, OP_STOP_IDLE = 2 };

// groups whose animations are replicated as events (they are not produced by the pad replay)
constexpr int kReplicatedGroups[] = {ANIM_GROUP_PLAYIDLES};

bool IsReplicatedGroup(int g) {
    for (int r : kReplicatedGroups)
        if (r == g) return true;
    return false;
}

bool EnsureBlockLoaded(const char* block) {
    int idx = Call<0x4D3990, int>(block);  // CAnimManager::GetAnimationBlockIndex
    if (idx < 0) return false;
    if (anim::BlockLoaded(idx)) return true;
    RequestModel(anim::kIfpBase + idx, 2);
    LoadAllRequestedModels(false);
    return anim::BlockLoaded(idx);
}

const char* BlockForGroup(int group) { return group == ANIM_GROUP_PLAYIDLES ? "playidles" : nullptr; }

constexpr uint16_t kNone = 0xFFFF;
struct ScriptAnim {
    uint16_t block = kNone, idx = 0, flags = 0;
    float time = 0.0f;
    uint64_t recvMs = 0;
    bool applied = true;               // what is wanted plays already (or nothing is wanted)
    uint16_t playBlock = kNone, playIdx = 0;  // what was blended on the copy
};

// TASK_SIMPLE_ANIM, TASK_SIMPLE_NAMED_ANIM, TASK_SIMPLE_TIMED_ANIM: the animation tasks scripts give, and
// TASK_SIMPLE_SLIDE_TO_COORD (a named animation once the ped stands where the script wants it: machines, counters)
// (CTaskManager::FindActiveTaskByType: the primary chain and the secondary tasks)
constexpr int TASK_SIMPLE_SLIDE_TO_COORD = 934;
bool RunsScriptAnim(CPed* ped) {
    void* intel = ped->Intelligence();
    if (!intel) return false;
    for (int type : {400, 401, 402, TASK_SIMPLE_SLIDE_TO_COORD})
        if (CallMethod<0x681740, void*>((uint8_t*)intel + 4, type)) return true;
    return false;
}

bool Usable(void* a, int motion) {
    return anim::BlendAmount(a) >= 0.3f && anim::BlendDelta(a) >= 0.0f && anim::Group(a) != motion &&
           !(anim::Flags(a) & anim::F_INDESTRUCTIBLE);
}

// the animation a script (or other code outside the pad) plays: while an animation task runs, the strongest one that is
// not the ped's own walk/idle set or its face; otherwise the strongest one blended by name (no animation group: the pad
// never plays such ones, CAnimManager::BlendAnimation by hierarchy leaves the group at ANIM_GROUP_NONE)
void* ScriptAnimOf(CPed* ped) {
    int motion = Field<int32_t>(ped, 0x4D4);  // m_nAnimGroup
    bool task = RunsScriptAnim(ped);
    void* best = nullptr;
    for (void* a = anim::First(ped->RwObject()); a; a = anim::Next(a)) {
        if (!Usable(a, motion) || (!task && anim::Group(a) >= 0)) continue;
        if (!best || anim::BlendAmount(a) > anim::BlendAmount(best)) best = a;
    }
    return best;
}

// the script animation as (block, index, flags, time); false when there is none
bool ScriptAnimState(CPed* ped, int& block, int& idx, uint16_t& flags, float& t) {
    void* a = ScriptAnimOf(ped);
    if (!a || !anim::Locate(anim::Hierarchy(a), block, idx)) return false;
    flags = anim::Flags(a);
    t = anim::CurrentTime(a);
    return true;
}

// the task types of the active primary chain and the secondary tasks, for the log
std::string TaskChain(CPed* ped) {
    void* intel = ped->Intelligence();
    if (!intel) return "-";
    std::string s;
    for (int i = 0; i < 5; ++i) {
        void* t = Field<void*>(intel, 0x04 + i * 4);  // CTaskManager::m_aPrimaryTasks
        if (!t) continue;
        for (int d = 0; t && d < 8; ++d, t = CallVirtual<2, void*>(t))  // CTask::GetSubTask
            s += Format("%s%d", d ? ">" : "", CallVirtual<4, int>(t));  // CTask::GetTaskType
        break;
    }
    for (int i = 0; i < 6; ++i)
        if (void* t = Field<void*>(intel, 0x18 + i * 4)) s += Format(" s%d:%d", i, CallVirtual<4, int>(t));  // m_aSecondaryTasks
    return s.empty() ? "-" : s;
}

const char* BlockName(int block) { return (const char*)anim::Block(block); }  // CAnimBlock::szName

void* FindPlaying(void* clump, int block, int idx) {
    int b, i;
    for (void* a = anim::First(clump); a; a = anim::Next(a))
        if (anim::Locate(anim::Hierarchy(a), b, i) && b == block && i == idx) return a;
    return nullptr;
}

using PlayIdleFn = void(__fastcall*)(void*, void*, CPlayerPed*);
PlayIdleFn g_origPlayIdle = nullptr;
void __fastcall HookPlayIdleAnimations(void* task, void* edx, CPlayerPed* player) {
    if (Players::Instance().FindByPed(player)) return;  // remote player: idles come from the network
    g_origPlayIdle(task, edx, player);
}

class Anims : public Module {
public:
    struct Sent {
        int block = -1, idx = -1, prevBlock = -1, prevIdx = -1;
        float time = 0.0f;
        uint64_t ms = 0;
        bool changed = false;
    };
    struct LocalPed {
        uint16_t model = 0;
        CVector pos{};
        int8_t area = 0;
        Sent sent;
    };
    struct OtherPed : ScriptAnim {
        uint16_t model = 0;
        CVector pos{};
        CPed* ped = nullptr;
        int handle = 0;
    };
    using OtherKey = std::tuple<uint8_t, uint16_t, int, int, int>;

    const char* Name() const override { return "Anims"; }

    void OnInstallHooks() override {
        mem::HookFn(0x6872C0, &HookPlayIdleAnimations, &g_origPlayIdle, "CTaskSimplePlayerOnFoot::PlayIdleAnimations");
        Net().SetHandler(proto::G_PLAYER_ANIM, [](uint8_t from, Reader& r) { Instance().Handle(from, r); });
        Net().SetHandler(proto::G_SCRIPT_ANIM, [](uint8_t from, Reader& r) { Instance().HandleScript(from, r); });
        Net().SetHandler(proto::G_PED_SCRIPT_ANIM, [](uint8_t from, Reader& r) { Instance().HandlePedScript(from, r); });
    }

    void OnPreProcess() override {
        if (!Net().Online() || !core::IsPlayerReady() || Ref<uint8_t>(0xB5F851)) return;
        for (auto it = m_remote.begin(); it != m_remote.end();) {
            RemotePlayer* rp = Players::Instance().Get(it->first);
            if (!rp) {
                it = m_remote.erase(it);
                continue;
            }
            if (rp->ped && rp->ped->RwObject()) ApplyScript(rp->ped, it->second, Format("Remote player %d", it->first).c_str());
            ++it;
        }
        ApplyOtherPeds(FindPlayerPed(0));
    }

    void OnPostProcess() override {
        // in a cutscene both games animate the partner themselves (ScenePartner), idle variations included
        if (!Net().Online() || !core::IsPlayerReady() || Ref<uint8_t>(0xB5F851)) return;  // CCutsceneMgr::ms_running
        CPlayerPed* me = FindPlayerPed(0);
        SendScript(me);
        SendLocalPeds(me);
        // the replicated animation currently playing on the local player (blending in or full)
        int group = -1, id = -1;
        for (void* a = anim::First(me->RwObject()); a; a = anim::Next(a)) {
            if (!IsReplicatedGroup(anim::Group(a)) || anim::BlendDelta(a) < 0.0f) continue;
            group = anim::Group(a);
            id = anim::Id(a);
            break;
        }
        if (group == m_lastGroup && id == m_lastId) return;
        Writer w(proto::G_PLAYER_ANIM);
        if (id >= 0) {
            w.U8(OP_PLAY);
            w.I16((int16_t)group);
            w.I16((int16_t)id);
            w.F32(8.0f);
        } else {
            w.U8(OP_STOP_IDLE);
        }
        Net().Send(proto::CH_RELIABLE, w);
        LOGI("Local animation %s: group %d id %d", id >= 0 ? "started" : "stopped", id >= 0 ? group : m_lastGroup,
             id >= 0 ? id : m_lastId);
        m_lastGroup = group;
        m_lastId = id;
    }

    void Handle(uint8_t from, Reader& r) {
        uint8_t op = r.U8();
        RemotePlayer* rp = Players::Instance().Get(from);
        if (!rp || !rp->ped || !core::IsPlayerReady()) return;
        void* clump = rp->ped->RwObject();
        if (!clump) return;
        if (op == OP_PLAY) {
            int group = r.I16();
            int id = r.I16();
            float delta = r.F32();
            if (!r.Ok() || !IsReplicatedGroup(group)) return;
            const char* block = BlockForGroup(group);
            if (block && !EnsureBlockLoaded(block)) return;
            uintptr_t grp = Ref<uintptr_t>(0xB4EA34) + (uintptr_t)group * 0x14;  // CAnimManager::ms_aAnimAssocGroups
            int first = (int)Ref<uint32_t>(grp + 0x0C), count = (int)Ref<uint32_t>(grp + 0x08);  // m_IdOffset, m_NumAnims
            if (id < first || id >= first + count) return;  // from the network
            void* a = Call<0x4D4610, void*>(clump, group, id, std::clamp(delta, 1.0f, 50.0f));  // CAnimManager::BlendAnimation
            if (a) anim::Flags(a) |= ANIMATION_IDLE_FLAG;
            LOGI("Remote player %d animation: group %d id %d (%s)", from, group, id, a ? "ok" : "failed");
        } else if (op == OP_STOP_IDLE) {
            for (void* a = anim::First(clump); a; a = anim::Next(a))
                if ((anim::Flags(a) & ANIMATION_IDLE_FLAG) && IsReplicatedGroup(anim::Group(a))) {
                    anim::BlendDelta(a) = -8.0f;
                    anim::Flags(a) |= anim::F_AUTO_REMOVE;  // deleted once out
                }
        }
    }

    // whether to send now what a ped plays (sent: what went last); a refresh every 2 s for a game that joined meanwhile
    static bool ShouldSend(Sent& sent, int block, int idx, uint16_t flags, float t) {
        uint64_t now = NowMs();
        bool restarted = block >= 0 && block == sent.block && idx == sent.idx && !(flags & anim::F_LOOPED) && t + 0.25f < sent.time;
        bool changed = block != sent.block || (block >= 0 && idx != sent.idx) || restarted;
        bool refresh = block >= 0 && now - sent.ms > 2000;
        if (block >= 0) sent.time = t;
        if (!changed && !refresh) return false;
        sent.changed = changed;
        sent.prevBlock = sent.block;
        sent.prevIdx = sent.idx;
        sent.block = block;
        sent.idx = idx;
        sent.ms = now;
        return true;
    }

    static void WriteAnim(Writer& w, int block, int idx, uint16_t flags, float t) {
        w.U8(block >= 0);
        if (block >= 0) {
            w.U16((uint16_t)block);
            w.U16((uint16_t)idx);
            w.U16(flags);
            w.F32(t);
        }
    }

    // reads "u8 on[, block, index, flags, time]" into what the receiver wants on that ped
    static bool ReadAnim(Reader& r, ScriptAnim& cur) {
        ScriptAnim in;
        bool on = r.U8() != 0;
        if (on) {
            in.block = r.U16();
            in.idx = r.U16();
            in.flags = r.U16();
            in.time = r.F32();
        }
        if (!r.Ok() || (on && (in.block == kNone || !std::isfinite(in.time)))) return false;
        bool same = cur.block == in.block && (!on || cur.idx == in.idx);
        bool restarted = on && same && !(in.flags & anim::F_LOOPED) && in.time + 0.25f < cur.time;
        cur.time = in.time;
        cur.recvMs = NowMs();
        if (same && !restarted) return true;  // a refresh of what plays
        cur.block = in.block;
        cur.idx = in.idx;
        cur.flags = in.flags;
        cur.applied = false;
        return true;
    }

    void SendScript(CPlayerPed* me) {
        int block = -1, idx = -1;
        uint16_t flags = 0;
        float t = 0.0f;
        if (!ScriptAnimState(me, block, idx, flags, t)) block = -1;
        if (!ShouldSend(m_sent, block, idx, flags, t)) return;
        Writer w(proto::G_SCRIPT_ANIM);
        WriteAnim(w, block, idx, flags, t);
        Net().Send(proto::CH_RELIABLE, w);
        if (!m_sent.changed) return;
        if (block >= 0)
            LOGI("Script animation on the player: %d/%d (%s, flags %04X, tasks %s)", block, idx, BlockName(block), flags,
                 TaskChain(me).c_str());
        else
            LOGI("Script animation over: %d/%d", m_sent.prevBlock, m_sent.prevIdx);
    }

    void HandleScript(uint8_t from, Reader& r) { ReadAnim(r, m_remote[from]); }

    // plays on the ped what is wanted (tried again next frame while its IFP streams in)
    // an NPC (idle): with nothing else playing once ours fades, its stand animation comes back
    static void ApplyScript(CPed* ped, ScriptAnim& s, const char* who, bool idle = false) {
        if (s.applied) return;
        void* clump = ped->RwObject();
        if (s.playBlock != kNone) {  // the one before ends (or starts over)
            if (void* a = FindPlaying(clump, s.playBlock, s.playIdx)) anim::FadeOut(a);
            s.playBlock = kNone;
            if (idle && s.block == kNone) Call<0x4D4610, void*>(clump, Field<int32_t>(ped, 0x4D4), 3 /* ANIM_ID_IDLE */, 4.0f);
        }
        if (s.block == kNone) {
            s.applied = true;
            LOGI("%s: script animation over", who);
            return;
        }
        void* h = anim::Resolve(s.block, s.idx);
        if (!h) {  // its IFP streams in: tried again next frame, given up after a while
            if (NowMs() - s.recvMs > 5000) s.applied = true;
            return;
        }
        void* a = anim::Blend(clump, h, s.flags, 4.0f);
        s.applied = true;
        if (!a) return;
        float total = anim::TotalTime(h), t = s.time + (float)(NowMs() - s.recvMs) / 1000.0f;
        anim::SetCurrentTime(a, (s.flags & anim::F_LOOPED) && total > 0 ? std::fmod(t, total) : std::min(t, total));
        s.playBlock = s.block;
        s.playIdx = s.idx;
        LOGI("%s: script animation %d/%d (%s) at %.2f of %.2f s", who, s.block, s.idx, BlockName(s.block), t, total);
    }

    // ---- the game's own script peds (a cashier, a barber, a bartender): every game has its own, not networked.
    // What a script plays on ours near a partner goes out with the ped's model and spot; the other games play it on
    // their ped of that model standing there.
    static bool PartnerHere(CPed* me) {
        for (auto& [id, rp] : Players::Instance().All())
            if (rp.hasState && rp.state.area == me->AreaCode() && (rp.state.pos - me->GetPosition()).Length() < 60.0f) return true;
        return false;
    }

    static bool IsLocalScriptPed(CPed* ped, CPlayerPed* me) {
        return ped != me && !ped->IsPlayer() && !Players::Instance().FindByPed(ped) && !Peds::Instance().Find(ped) &&
               ped->Health() > 0.0f && ped->RwObject();
    }

    // it plays what another game's ped plays (not to be sent back)
    bool PlaysForOthers(CPed* ped) const {
        for (auto& [key, o] : m_otherPeds)
            if (o.ped == ped && o.playBlock != kNone) return true;
        return false;
    }

    void SendLocalPeds(CPlayerPed* me) {
        uint64_t now = NowMs();
        if (now - m_localScanMs < 100) return;
        m_localScanMs = now;
        std::map<CPed*, bool> seen;
        CPoolHeader* pool = PedPool();
        if (pool && PartnerHere(me)) {
            CVector mine = me->GetPosition();
            for (int i = 0; i < pool->size; ++i) {
                if (pool->byteMap[i] < 0) continue;
                auto* ped = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
                if (!IsLocalScriptPed(ped, me) || (ped->GetPosition() - mine).Length() > 40.0f || PlaysForOthers(ped)) continue;
                int block, idx;
                uint16_t flags;
                float t;
                if (!ScriptAnimState(ped, block, idx, flags, t)) continue;
                auto it = m_localPeds.find(ped);
                if (it != m_localPeds.end() && it->second.model != ped->ModelIndex()) {  // another ped in that slot
                    SendLocalPed(it->second, -1, 0, 0, 0.0f);
                    m_localPeds.erase(it);
                    it = m_localPeds.end();
                }
                if (it == m_localPeds.end()) {
                    it = m_localPeds.emplace(ped, LocalPed{}).first;
                    it->second.model = ped->ModelIndex();
                    it->second.pos = ped->GetPosition();
                    it->second.area = me->AreaCode();
                }
                seen[ped] = true;
                if (ShouldSend(it->second.sent, block, idx, flags, t)) {
                    SendLocalPed(it->second, block, idx, flags, t);
                    if (it->second.sent.changed)
                        LOGI("Script animation on ped %d at %.1f,%.1f,%.1f: %d/%d (%s, tasks %s)", it->second.model, it->second.pos.x,
                             it->second.pos.y, it->second.pos.z, block, idx, BlockName(block), TaskChain(ped).c_str());
                }
            }
        }
        for (auto it = m_localPeds.begin(); it != m_localPeds.end();) {
            if (seen.count(it->first)) {
                ++it;
                continue;
            }
            SendLocalPed(it->second, -1, 0, 0, 0.0f);
            it = m_localPeds.erase(it);
        }
    }


    static void SendLocalPed(const LocalPed& p, int block, int idx, uint16_t flags, float t) {
        Writer w(proto::G_PED_SCRIPT_ANIM);
        w.U16(p.model);
        w.Vec({p.pos.x, p.pos.y, p.pos.z});
        w.I8(p.area);
        WriteAnim(w, block, idx, flags, t);
        Net().Send(proto::CH_RELIABLE, w);
    }


    void HandlePedScript(uint8_t from, Reader& r) {
        uint16_t model = r.U16();
        Vec3 v = r.Vec();
        int8_t area = r.I8();
        if (!r.Ok() || !std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) return;
        OtherKey key{from, model, (int)std::lround(v.x * 2), (int)std::lround(v.y * 2), (int)std::lround(v.z * 2)};
        auto it = m_otherPeds.find(key);
        CPlayerPed* me = core::IsPlayerReady() ? FindPlayerPed(0) : nullptr;
        if (it == m_otherPeds.end()) {
            if (!me || area != me->AreaCode()) return;  // not where we are: nothing of ours plays it
            it = m_otherPeds.emplace(key, OtherPed{}).first;
            it->second.model = model;
            it->second.pos = {v.x, v.y, v.z};
        }
        if (!ReadAnim(r, it->second)) return;
    }

    // our ped of that model standing at that spot (the same script put it there in every game)
    static CPed* FindLocalPed(uint16_t model, const CVector& at, CPlayerPed* me) {
        CPoolHeader* pool = PedPool();
        if (!pool) return nullptr;
        CPed* best = nullptr;
        float bestD = 2.5f;
        for (int i = 0; i < pool->size; ++i) {
            if (pool->byteMap[i] < 0) continue;
            auto* ped = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
            if (ped->ModelIndex() != model || !IsLocalScriptPed(ped, me)) continue;
            if (float d = (ped->GetPosition() - at).Length(); d < bestD) best = ped, bestD = d;
        }
        return best;
    }

    void ApplyOtherPeds(CPlayerPed* me) {
        for (auto it = m_otherPeds.begin(); it != m_otherPeds.end();) {
            OtherPed& o = it->second;
            bool alive = o.ped && IsPedValid(o.ped) && GetPedRef(o.ped) == o.handle && o.ped->RwObject();
            if (!alive) {
                o.ped = nullptr;
                o.playBlock = kNone;
                if (o.block != kNone && !o.applied) {
                    if (CPed* p = FindLocalPed(o.model, o.pos, me)) {
                        o.ped = p;
                        o.handle = GetPedRef(p);
                    } else if (NowMs() - o.recvMs > 5000) {
                        o.applied = true;
                    }
                }
            }
            if (o.ped && !o.applied) {
                int b, i;
                uint16_t f;
                float t;
                // our own script plays something unlooped on it (it serves our player): that one stays
                if (o.block != kNone && ScriptAnimState(o.ped, b, i, f, t) && !(f & anim::F_LOOPED) && !(b == o.playBlock && i == o.playIdx)) {
                    o.applied = true;
                    LOGI("Ped %d of player %d: script animation %d/%d left out, ours plays %d/%d", o.model, std::get<0>(it->first),
                         o.block, o.idx, b, i);
                } else {
                    ApplyScript(o.ped, o, Format("Ped %d of player %d", o.model, std::get<0>(it->first)).c_str(), true);
                }
            }
            if (o.applied && o.block == kNone && o.playBlock == kNone) {
                it = m_otherPeds.erase(it);
                continue;
            }
            ++it;
        }
    }

    void OnGameStarted() override {
        m_lastGroup = m_lastId = -1;
        m_sent = Sent{};
        m_remote.clear();
        m_localPeds.clear();
        m_otherPeds.clear();
    }

    static Anims& Instance() {
        static Anims a;
        return a;
    }

private:
    int m_lastGroup = -1, m_lastId = -1;
    Sent m_sent;
    std::map<uint8_t, ScriptAnim> m_remote;
    std::map<CPed*, LocalPed> m_localPeds;  // ours, playing a script animation near a partner
    uint64_t m_localScanMs = 0;
    std::map<OtherKey, OtherPed> m_otherPeds;
};

struct Reg {
    Reg() { core::RegisterModule(&Anims::Instance()); }
} g_reg;
}  // namespace

}  // namespace coop
