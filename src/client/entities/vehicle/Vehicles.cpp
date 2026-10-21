#include "entities/vehicle/Vehicles.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Script.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

using namespace game;
using game::script::Cmd;

Vehicles& Vehicles::Instance() {
    static Vehicles v;
    return v;
}

namespace {
// models streamed in for a mirror, with when they were first asked for: required until it is made (see SpawnMirror)
std::map<int, uint64_t> g_mirrorModels;
// The lock of a mirror. A random car locked there does not lock the other players out; a mission's locked car (2) stays
// shut for them too. "Locked with the player inside" (4, CARLOCK_LOCKED_PLAYER_INSIDE: nobody opens it from outside, the
// player in it cannot get out) is a mission's way to keep its CJ in the car: it is the owner's player's, not the
// partners'. Copied as it was, the partners could not get into the host's mission car (Sweet's car, "Tagging Up Turf").
uint8_t MirrorLock(uint8_t lock, bool script) {
    if (lock == 4) return 1;
    return lock == 2 && !script ? 1 : lock;
}
struct Reg {
    Reg() { core::RegisterModule(&Vehicles::Instance()); }
} g_reg;

constexpr uint8_t STATUS_PHYSICS = 3, STATUS_WRECKED = 5;
constexpr uint8_t VEHICLE_CREATED_RANDOM = 1, VEHICLE_CREATED_MISSION = 2, VEHICLE_CREATED_PARKED = 3;
constexpr float kTrafficIn = 170.0f;   // owned vehicles closer than this to the partner are announced (> car spawn range)
constexpr float kTrafficOut = 220.0f;  // and dropped from the network beyond this

void SetVehicleCreatedBy(CVehicle* v, uint8_t by) { CallMethod<0x6D5D70>(v, (int)by); }  // keeps CCarCtrl counters right

// A script of this game holds the car: its mission's list (CTheScripts::MissionCleanUp, 75 x {type, handle}, 1 a car)
// or the car STORE_CAR_CHAR_IS_IN stored (CTheScripts::StoreVehicleIndex). Such a car stays a mission car here.
bool ScriptHolds(CVehicle* v) {
    int h = GetVehicleRef(v);
    if (h == Ref<int32_t>(0xA447EC)) return true;
    for (int i = 0; i < 75; ++i)
        if (Ref<uint8_t>(0xA90850 + (uintptr_t)i * 8) == 1 && Ref<int32_t>(0xA90850 + (uintptr_t)i * 8 + 4) == h) return true;
    return false;
}

bool HasPlayerInside(CVehicle* v) {
    if (v->Driver() && v->Driver()->IsPlayer()) return true;
    for (int i = 0; i < 8; ++i)
        if (v->Passengers()[i] && v->Passengers()[i]->IsPlayer()) return true;
    return false;
}

void SetVehicleMatrix(CVehicle* v, const CVector& pos, const Quat& q) {
    CMatrix* m = v->Matrix();
    if (!m) return;
    q.ToMatrix(*m);
    m->pos = pos;
    CallMethod<0x446F90>(v);  // CEntity::UpdateRW
    CallMethod<0x532B00>(v);  // CEntity::UpdateRwFrame
}
constexpr int kDamageManager = 0x5A0, kBikeTyres = 0x65C;  // CAutomobile::m_damageManager, CBike::m_nWheelStatus

bool HasDamageManager(CVehicle* v) {  // CAutomobile and its subclasses
    switch (v->VehicleSubClass()) {
    case VEHICLE_TYPE_AUTOMOBILE: case VEHICLE_TYPE_MTRUCK: case VEHICLE_TYPE_QUAD: case VEHICLE_TYPE_HELI:
    case VEHICLE_TYPE_PLANE: case VEHICLE_TYPE_TRAILER: return true;
    }
    return false;
}

// shows the damage of the owner's vehicle: parts are reset, then rebuilt from the damage state the way
// a loaded save does it (no debris flies off, the parts are just gone or bent)
void ApplyDamage(CVehicle* v, const uint8_t (&damage)[20]) {
    uint8_t* dm = (uint8_t*)v + kDamageManager + 4;
    if (!memcmp(dm, damage, sizeof(damage))) return;
    CallMethod<0x6A3440>(v);  // CAutomobile::Fix
    memcpy(dm, damage, sizeof(damage));
    switch (v->VehicleSubClass()) {  // SetupDamageAfterLoad of the exact class
    case VEHICLE_TYPE_HELI: CallMethod<0x6C4550>(v); break;
    case VEHICLE_TYPE_PLANE: CallMethod<0x6CAC10>(v); break;
    case VEHICLE_TYPE_QUAD: CallMethod<0x6CE340>(v); break;
    default: CallMethod<0x6B3E90>(v); break;
    }
}

VehicleLook CaptureLook(CVehicle* v) {
    VehicleLook l;
    if (HasDamageManager(v)) memcpy(l.damage, (uint8_t*)v + kDamageManager + 4, sizeof(l.damage));
    if (v->VehicleSubClass() == VEHICLE_TYPE_BIKE) memcpy(l.bikeTyres, (uint8_t*)v + kBikeTyres, 2);
    memcpy(l.colors, v->Colors(), 4);
    memcpy(l.upgrades, v->Upgrades(), sizeof(l.upgrades));
    l.lock = (uint8_t)v->DoorLock();
    int pj = -1;
    Cmd(0x0988).I(GetVehicleRef(v)).Out(&pj).Run();  // GET_CURRENT_VEHICLE_PAINTJOB
    l.paintjob = (int8_t)pj;
    return l;
}
}  // namespace

bool VehicleLook::operator==(const VehicleLook& o) const {
    return !memcmp(colors, o.colors, 4) && !memcmp(upgrades, o.upgrades, sizeof(upgrades)) && paintjob == o.paintjob &&
           !memcmp(damage, o.damage, sizeof(damage)) && !memcmp(bikeTyres, o.bikeTyres, 2) && lock == o.lock;
}

bool IsSyncableVehicle(CVehicle* v) { return !v->IsTrain(); }

// A mirror has the AI-driven status (STATUS_PHYSICS), so every physics step ran its AI steering
// (CCarCtrl::SteerAICarWithPhysics): with no mission it brakes and pulls the handbrake, and the next frame's
// network speed pushed it on, so a mirror lurched like a car that keeps braking. Mirrors are driven by the network.
using SteerFn = void(__cdecl*)(CVehicle*);
SteerFn g_origSteerAI = nullptr;
void __cdecl HookSteerAICarWithPhysics(CVehicle* v) {
    if (Net().Online() && v)
        if (NetVehicle* nv = Vehicles::Instance().Find(v); nv && Vehicles::Instance().IsMirror(*nv)) return;
    g_origSteerAI(v);
}

void Vehicles::OnInstallHooks() {
    mem::HookFn(0x437C20, &HookSteerAICarWithPhysics, &g_origSteerAI, "CCarCtrl::SteerAICarWithPhysics");
    Net().SetHandler(proto::G_VEH_CREATE, [](uint8_t from, Reader& r) { Instance().HandleCreate(from, r); });
    Net().SetHandler(proto::G_VEH_REMOVE, [](uint8_t from, Reader& r) { Instance().HandleRemove(from, r); });
    Net().SetHandler(proto::G_VEH_SYNC, [](uint8_t from, Reader& r) { Instance().HandleSync(from, r); });
    Net().SetHandler(proto::G_VEH_OWNER, [](uint8_t from, Reader& r) { Instance().HandleOwner(from, r); });
    Net().SetHandler(proto::G_VEH_STATUS, [](uint8_t from, Reader& r) { Instance().HandleStatus(from, r); });
    Net().SetHandler(proto::G_VEH_HANDOVER, [](uint8_t from, Reader& r) { Instance().HandleHandover(from, r); });
    Net().SetHandler(proto::G_VEH_SCRIPT_FREE, [](uint8_t from, Reader& r) { Instance().HandleScriptFree(from, r); });
    Net().SetHandler(proto::G_VEH_CLEAR, [](uint8_t from, Reader& r) { Instance().HandleClear(from, r); });
}

NetVehicle* Vehicles::Find(uint16_t netId) {
    auto it = m_byId.find(netId);
    return it == m_byId.end() ? nullptr : &it->second;
}

NetVehicle* Vehicles::Find(CVehicle* v) {
    auto it = m_byPtr.find(v);
    return it == m_byPtr.end() ? nullptr : Find(it->second);
}

bool Vehicles::IsMirror(const NetVehicle& nv) const { return nv.owner != Net().MyId(); }

NetVehicle* Vehicles::EnsureNetworked(CVehicle* v) {
    if (!v || !IsVehicleValid(v)) return nullptr;
    if (NetVehicle* nv = Find(v)) return nv;
    if (!Net().Online()) return nullptr;
    uint16_t id;
    do {
        id = (uint16_t)(((Net().MyId() + 1) << 12) | (m_nextId++ & 0x0FFF));
    } while (m_byId.count(id));
    NetVehicle& nv = m_byId[id];
    nv.netId = id;
    nv.veh = v;
    nv.handle = GetVehicleRef(v);
    nv.owner = Net().MyId();
    nv.model = v->ModelIndex();
    nv.lastChangeMs = NowMs();
    m_byPtr[v] = id;
    Announce(nv);
    LOGI("Vehicle %04X (model %d) is now networked", id, nv.model);
    return &nv;
}

void Vehicles::Announce(const NetVehicle& nv) {
    CVehicle* v = nv.veh;
    VehicleSpawn sp;
    sp.netId = nv.netId;
    sp.model = nv.model;
    sp.owner = nv.owner;
    sp.pos = v->GetPosition();
    if (CMatrix* m = v->Matrix()) sp.rot = Quat::FromMatrix(*m);
    memcpy(sp.colors, v->Colors(), 4);
    sp.health = v->Health();
    sp.area = v->AreaCode();
    sp.lockState = (uint8_t)v->DoorLock();
    sp.script = nv.createdByUs ? nv.script : v->CreatedBy() == VEHICLE_CREATED_MISSION;
    Writer w(proto::G_VEH_CREATE);
    sp.Write(w);
    Net().Send(proto::CH_RELIABLE, w);
}

void Vehicles::Reannounce() {
    for (auto& [id, nv] : m_byId)
        if (!IsMirror(nv) && IsVehicleValid(nv.veh)) {
            Announce(nv);
            SendLook(nv);
        }
}

void Vehicles::SendLook(NetVehicle& nv) {
    nv.look = CaptureLook(nv.veh);
    nv.hasLook = true;
    Writer w(proto::G_VEH_STATUS);
    w.U16(nv.netId);
    w.Raw(nv.look.colors, 4);
    for (int16_t u : nv.look.upgrades) w.I16(u);
    w.I8(nv.look.paintjob);
    w.Raw(nv.look.damage, sizeof(nv.look.damage));
    w.Raw(nv.look.bikeTyres, 2);
    w.U8(nv.look.lock);
    Net().Send(proto::CH_RELIABLE, w);
}

void Vehicles::HandleStatus(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    VehicleLook l;
    r.Raw(l.colors, 4);
    for (int16_t& u : l.upgrades) u = r.I16();
    l.paintjob = r.I8();
    r.Raw(l.damage, sizeof(l.damage));
    r.Raw(l.bikeTyres, 2);
    l.lock = r.U8();
    NetVehicle* nv = Find(id);
    if (!r.Ok() || !nv || nv->owner != from) return;
    nv->look = l;
    nv->hasLook = true;
}

// mod shop work done in the owner's game: same parts (models streamed in first), paintjob, colours
void Vehicles::ApplyLook(NetVehicle& nv) {
    CVehicle* v = nv.veh;
    memcpy(v->Colors(), nv.look.colors, 4);
    int h = nv.handle;
    for (int slot = 0; slot < 15; ++slot) {
        int16_t want = nv.look.upgrades[slot], have = v->Upgrades()[slot];
        if (want == have) continue;
        if (have >= 0) Cmd(0x06E8).I(h).I(have).Run();  // REMOVE_VEHICLE_MOD
        if (want < 0) continue;
        Cmd(0x06E9).I(want).Run();                          // REQUEST_VEHICLE_MOD
        if (!Cmd(0x06EA).I(want).Run()) continue;           // HAS_VEHICLE_MOD_LOADED: try again next frame
        int dummy = 0;
        Cmd(0x06E7).I(h).I(want).Out(&dummy).Run();         // ADD_VEHICLE_MOD
    }
    int pj = -1;
    Cmd(0x0988).I(h).Out(&pj).Run();  // GET_CURRENT_VEHICLE_PAINTJOB
    if (nv.look.paintjob >= 0 && pj != nv.look.paintjob) Cmd(0x06ED).I(h).I(nv.look.paintjob).Run();  // GIVE_VEHICLE_PAINTJOB
    if (HasDamageManager(v)) ApplyDamage(v, nv.look.damage);
    else if (v->VehicleSubClass() == VEHICLE_TYPE_BIKE) memcpy((uint8_t*)v + kBikeTyres, nv.look.bikeTyres, 2);
    // the lock it had when it became known stayed: a mission car locked then held the partner who got in, after the
    // mission it was opened only in the owner's game (the Savanna of "Cesar Vialpando"); same rule as at the spawn
    v->DoorLock() = MirrorLock(nv.look.lock, nv.script);
}

void Vehicles::TakeOwnership(NetVehicle& nv) {
    if (nv.owner == Net().MyId()) return;
    nv.owner = Net().MyId();
    nv.hasState = false;
    nv.lastChangeMs = NowMs();
    Release(nv);
    Writer w(proto::G_VEH_OWNER);
    w.U16(nv.netId);
    w.U8(nv.owner);
    Net().Send(proto::CH_RELIABLE, w);
    LOGI("Took ownership of vehicle %04X", nv.netId);
}

CVehicle* Vehicles::FindAdoptable(uint16_t model, const CVector& pos) {
    CPoolHeader* pool = VehiclePool();
    if (!pool) return nullptr;
    CPlayerPed* me = FindPlayerPed(0);
    CVehicle* best = nullptr;
    float bestD = 3.0f;
    for (int i = 0; i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* v = (CVehicle*)(pool->objects + (size_t)i * kVehiclePoolElemSize);
        if (v->ModelIndex() != model || m_byPtr.count(v)) continue;
        if (me && me->InVehicle() && me->Vehicle() == v) continue;
        // a script's car stays its own: released as an ordinary car later, its script lost it and crashed
        if (v->CreatedBy() == VEHICLE_CREATED_MISSION) continue;
        float d = (v->GetPosition() - pos).Length();
        if (d < bestD) {
            bestD = d;
            best = v;
        }
    }
    return best;
}

CVehicle* Vehicles::SpawnMirror(const VehicleSpawn& sp) {
    // Streamed in the background, CreateMirror is retried every frame: required and first in the queue until the car
    // is made, then left to the streamer as usual. A big model (the partner's tank) still waited minutes: the streamer
    // loads one that does not fit its buffer only when both its channels are idle, which they seldom were with
    // several game copies busy, until something loaded the queue at once (a pickup's model). After half a second the
    // model is loaded at once here too: a short stall instead of a player driving nothing.
    if (!IsModelLoaded(sp.model)) {
        auto [it, first] = g_mirrorModels.try_emplace(sp.model, NowMs());
        if (!first && NowMs() - it->second > 500) {
            bool ok = LoadModelNow(sp.model);
            LOGI("Model %d of vehicle %04X loaded at once (%s)", sp.model, sp.netId, ok ? "ok" : "failed");
            if (!ok) it->second = NowMs();  // tried again in half a second
        } else {
            RequestModel(sp.model, 2 | 0x10 /* GAME_REQUIRED | PRIORITY_REQUEST */);
        }
        if (!IsModelLoaded(sp.model)) return nullptr;
    }
    int handle = -1;
    Cmd(0x00A5).I(sp.model).F(sp.pos.x).F(sp.pos.y).F(sp.pos.z).Out(&handle).Run();  // CREATE_CAR
    if (g_mirrorModels.erase(sp.model)) MarkModelNotNeeded(sp.model);  // the car made of it holds it from now on
    CVehicle* v = GetVehicle(handle);
    if (!v) return nullptr;
    SetVehicleMatrix(v, sp.pos, sp.rot);
    memcpy(v->Colors(), sp.colors, 4);
    v->Health() = sp.health;
    v->AreaCode() = sp.area;
    // a random car locked there does not lock the other player out; a mission's locked car stays shut for him too
    v->DoorLock() = MirrorLock(sp.lockState, sp.script);
    return v;
}

void Vehicles::HandleCreate(uint8_t from, Reader& r) {
    VehicleSpawn sp;
    if (!sp.Read(r) || from == Net().MyId()) return;
    if (!core::IsPlayerReady()) return;
    if (NetVehicle* existing = Find(sp.netId)) {
        existing->owner = sp.owner;
        return;
    }
    if (!IsVehicleModel(sp.model)) {
        LOGW("Vehicle %04X has an unknown model %d", sp.netId, sp.model);
        return;
    }
    if (CreateMirror(sp)) m_pending.erase(sp.netId);
    else m_pending[sp.netId] = sp;
}

bool Vehicles::CreateMirror(const VehicleSpawn& sp) {
    if (Find(sp.netId)) return true;  // announced again meanwhile and made already
    bool adopted = false;
    CVehicle* v = FindAdoptable(sp.model, sp.pos);
    if (v) {
        adopted = true;
        SetVehicleMatrix(v, sp.pos, sp.rot);
        memcpy(v->Colors(), sp.colors, 4);  // the same parked car can have other random colours here
        v->Health() = sp.health;
    } else {
        v = SpawnMirror(sp);
    }
    if (!v) return false;
    NetVehicle& nv = m_byId[sp.netId];
    nv.netId = sp.netId;
    nv.veh = v;
    nv.handle = GetVehicleRef(v);
    nv.owner = sp.owner;
    nv.model = sp.model;
    nv.createdByUs = !adopted;
    nv.script = sp.script;
    if (adopted) Protect(nv);
    m_byPtr[v] = sp.netId;
    LOGI("Vehicle %04X (model %d) %s for player %d", sp.netId, sp.model, adopted ? "adopted" : "created", sp.owner);
    return true;
}

void Vehicles::Forget(uint16_t netId, bool deleteVehicle) {
    auto it = m_byId.find(netId);
    if (it == m_byId.end()) return;
    NetVehicle& nv = it->second;
    if (deleteVehicle && nv.veh && IsVehicleValid(nv.veh)) {
        // a deleted car takes its occupants along: the other player's character gets out first
        for (auto& [id, rp] : Players::Instance().All())
            if (rp.ped && IsPedValid(rp.ped) && rp.ped->InVehicle() && rp.ped->Vehicle() == nv.veh) {
                CVector p = rp.ped->GetPosition();
                Cmd(0x0362).I(rp.pedHandle).F(p.x).F(p.y).F(p.z).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
            }
        Cmd(0x00A6).I(GetVehicleRef(nv.veh)).Run();  // DELETE_CAR
    }
    m_byPtr.erase(nv.veh);
    m_byId.erase(it);
}

// A mirror is the other game's car: this game's population and scripts leave it alone (a car of the population taken
// over by the other player was deleted here as soon as this player got far, with the other player's character in it).
void Vehicles::Protect(NetVehicle& nv) {
    CVehicle* v = nv.veh;
    if (!IsVehicleValid(v) || v->CreatedBy() == VEHICLE_CREATED_MISSION) return;
    if (!nv.createdByUs && !nv.origBy) nv.origBy = v->CreatedBy();
    SetVehicleCreatedBy(v, VEHICLE_CREATED_MISSION);
}

// Ours again: what it was in this game before, and a mirror spawned here an ordinary car (a script's car of the other
// game stays a mission car: dropping it here would make the owner drop it in the middle of its mission)
void Vehicles::Release(NetVehicle& nv) {
    CVehicle* v = nv.veh;
    uint8_t by = nv.createdByUs ? (nv.script ? VEHICLE_CREATED_MISSION : VEHICLE_CREATED_RANDOM) : nv.origBy;
    if (IsVehicleValid(v) && by && v->CreatedBy() != by && !ScriptHolds(v)) SetVehicleCreatedBy(v, by);
    nv.origBy = 0;
}

// The owner let the vehicle go: a mirror spawned here goes too (unless this player sits in it), an instance this game had
// before (adopted, or ours until the other player took it) is this game's again, a script's car its script's.
void Vehicles::Drop(uint16_t netId) {
    NetVehicle* nv = Find(netId);
    if (!nv) return;
    CPlayerPed* me = FindPlayerPed(0);
    bool inside = me && me->InVehicle() && me->Vehicle() == nv->veh;
    bool gone = nv->createdByUs && !inside;
    if (!gone) Release(*nv);
    Forget(netId, gone);
}

std::vector<uint16_t> Vehicles::ClearMirrors(const CVector& at, float radius) {
    std::vector<uint16_t> ids;
    for (auto& [id, nv] : m_byId) {
        CVehicle* v = nv.veh;
        if (!IsMirror(nv) || nv.script || !IsVehicleValid(v) || v->Driver()) continue;
        bool empty = true;
        for (int i = 0; i < 8; ++i) empty = empty && !v->Passengers()[i];
        CVector d = v->GetPosition() - at;
        if (empty && d.x * d.x + d.y * d.y < radius * radius) ids.push_back(id);
    }
    for (uint16_t id : ids) Forget(id, true);
    return ids;
}

// the owner's side of ClearMirrors: what the host's copy no longer has goes here too, as the area's ordinary cars went
void Vehicles::HandleClear(uint8_t from, Reader& r) {
    uint8_t n = r.U8();
    for (uint8_t i = 0; i < n && r.Ok() && from == Net().HostId(); ++i) {
        uint16_t id = r.U16();
        NetVehicle* nv = r.Ok() ? Find(id) : nullptr;
        if (!nv || IsMirror(*nv) || !IsVehicleValid(nv->veh)) continue;
        if (nv->veh->Driver() || HasPlayerInside(nv->veh)) {  // got in meanwhile: the host gets its copy back
            Announce(*nv);
            continue;
        }
        LOGI("Vehicle %04X (model %d) cleared away by the host's mission", id, nv->model);
        Cmd(0x00A6).I(nv->handle).Run();  // DELETE_CAR: its removal goes out with the other vanished ones (Update)
    }
}

void Vehicles::ForgetOwnedBy(uint8_t owner) {
    std::vector<uint16_t> ids;
    for (auto& [vid, nv] : m_byId)
        if (nv.owner == owner) ids.push_back(vid);
    for (auto it = m_pending.begin(); it != m_pending.end();) it = it->second.owner == owner ? m_pending.erase(it) : std::next(it);
    for (uint16_t vid : ids) Drop(vid);
}

void Vehicles::HandleRemove(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    if (!r.Ok()) return;
    m_pending.erase(id);
    NetVehicle* nv = Find(id);
    if (!nv || nv->owner != from) return;
    Drop(id);
}

// An ordinary car of this game (traffic, a parked one) the other player is at while this game's player is far: this
// game would drop it soon (CCarCtrl removes cars beyond its player's reach) and it vanished in front of the other player.
// The other game takes it over with whoever sits in it, as its own, and this game's one goes.
static bool CanGiveAway(CVehicle* v) {
    uint8_t by = v->CreatedBy();
    if ((by != VEHICLE_CREATED_RANDOM && by != VEHICLE_CREATED_PARKED) || v->Status() == STATUS_WRECKED ||
        Field<CVehicle*>(v, 0x4C4) || Field<CVehicle*>(v, 0x4C8) ||  // towing or towed: the pair stays together
        !CallMethod<0x6D1180, bool>(v) ||                             // CVehicle::CanBeDeleted: the engine's own test
        CallMethod<0x534540, bool>(v) || ScriptHolds(v))               // CEntity::GetIsOnScreen: it does not vanish in view
        return false;
    CPed* seats[9] = {v->Driver()};
    for (int i = 0; i < 8; ++i) seats[i + 1] = v->Passengers()[i];
    for (CPed* p : seats) {  // its occupants go along: only this game's own passers-by the other game knows
        NetPed* np = p ? Peds::Instance().Find(p) : nullptr;
        if (p && (p->IsPlayer() || Players::Instance().FindByPed(p) || !np || np->owner != Net().MyId() ||
                  p->CreatedBy() != 1 /* PED_GAME */ || !Peds::Instance().CanGiveAwayType(*np)))
            return false;
    }
    return true;
}

void Vehicles::GiveAway(NetVehicle& nv, uint8_t to) {
    CVehicle* v = nv.veh;
    CPed* seats[9] = {v->Driver()};
    for (int i = 0; i < 8; ++i) seats[i + 1] = v->Passengers()[i];
    for (CPed* p : seats)  // the peds first: the car's message drives them on
        if (NetPed* np = p ? Peds::Instance().Find(p) : nullptr) Peds::Instance().GiveAway(np->netId, to);
    Writer w(proto::G_VEH_HANDOVER);
    w.U16(nv.netId);
    w.U8(to);
    Net().Send(proto::CH_RELIABLE, w);
    LOGI("Vehicle %04X (model %d) handed over to player %d", nv.netId, nv.model, to);
    Forget(nv.netId, true);
}

bool Vehicles::QueueGiveAway(CVehicle* v, uint8_t to) {
    NetVehicle* nv = Find(v);
    if (!nv || IsMirror(*nv) || !CanGiveAway(v)) return false;
    m_giveAway[v] = to;
    return true;
}

void Vehicles::HandleHandover(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    uint8_t to = r.U8();
    if (!r.Ok()) return;
    m_pending.erase(id);
    NetVehicle* nv = Find(id);
    if (!nv) return;
    if (nv->owner != from) {  // this game took it just before: the giver deleted its own, it gets this one back
        if (nv->owner == Net().MyId() && IsVehicleValid(nv->veh)) Announce(*nv);
        return;
    }
    if (to != Net().MyId() || !IsVehicleValid(nv->veh)) return Forget(id, true);
    CVehicle* v = nv->veh;
    int h = nv->handle;
    if (!ScriptHolds(v)) SetVehicleCreatedBy(v, VEHICLE_CREATED_RANDOM);  // this game's traffic now
    if (CPed* driver = v->Driver(); driver && !driver->IsPlayer() && !Players::Instance().FindByPed(driver)) {
        Cmd(0x00A8).I(h).Run();           // CAR_WANDER_RANDOMLY
        Cmd(0x00AD).I(h).F(12.0f).Run();  // SET_CAR_CRUISE_SPEED
        Cmd(0x00AE).I(h).I(0).Run();      // SET_CAR_DRIVING_STYLE: stops for cars and lights
    }
    Forget(id, false);
    LOGI("Vehicle %04X (model %d) taken over from player %d", id, (int)v->ModelIndex(), from);
}

void Vehicles::HandleSync(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    VehicleState st;
    if (!st.Read(r)) return;
    ApplyDriverState(from, id, st);
}

void Vehicles::ApplyDriverState(uint8_t from, uint16_t netId, const VehicleState& st) {
    NetVehicle* nv = Find(netId);
    if (!nv || nv->owner != from) return;
    if (nv->hasState && (int32_t)(st.time - nv->lastTime) < 0) return;
    nv->lastTime = st.time;
    nv->state = st;
    nv->hasState = true;
    nv->recvMs = NowMs();
}

void Vehicles::HandleOwner(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    uint8_t owner = r.U8();
    if (!r.Ok()) return;
    NetVehicle* nv = Find(id);
    if (!nv) return;
    CPlayerPed* me = FindPlayerPed(0);
    bool iDrive = me && nv->veh && me->InVehicle() && me->Vehicle() == nv->veh && nv->veh->Driver() == me;
    if (iDrive && owner != Net().MyId()) {
        // we are driving it right now: keep ownership, tell the others again
        Writer w(proto::G_VEH_OWNER);
        w.U16(id);
        w.U8(Net().MyId());
        Net().Send(proto::CH_RELIABLE, w);
        return;
    }
    bool wasMine = nv->owner == Net().MyId();
    nv->owner = owner;
    nv->hasState = false;
    if (owner == Net().MyId()) {
        Release(*nv);
    } else if (wasMine) {
        nv->heldHere = IsVehicleValid(nv->veh) && ScriptHolds(nv->veh);
        Protect(*nv);
    }
}

// The script that held this car here let go (its mission ended): the owner's copy, kept a mission car for it, is an
// ordinary car again (else it never went away and stood twice when announced again)
void Vehicles::HandleScriptFree(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    NetVehicle* nv = r.Ok() ? Find(id) : nullptr;
    if (!nv || IsMirror(*nv) || !IsVehicleValid(nv->veh)) return;
    nv->script = false;
    if (nv->createdByUs && nv->veh->CreatedBy() == VEHICLE_CREATED_MISSION && !ScriptHolds(nv->veh))
        SetVehicleCreatedBy(nv->veh, VEHICLE_CREATED_RANDOM);
}

// The parts a driver moves: nozzles, hoists and forks (m_wMiscComponentAngle), the landing gear, a turret. Nothing ticks
// them on a vehicle without a player's status, so the owner's values stand here.
void ApplyParts(CVehicle* v, const VehicleState& st) {
    if (st.extras & VehicleState::X_MISC) Field<uint16_t>(v, 0x86C) = st.misc;
    if (st.extras & VehicleState::X_GEAR) {
        float& gear = Field<float>(v, 0x9CC);
        if (st.gear >= 1.0f && gear < 1.0f) CallMethod<0x6CAC20>(v);         // CPlane::SetGearUp: no wheels
        else if (st.gear == 0.0f && gear != 0.0f) CallMethod<0x6CAC70>(v);  // CPlane::SetGearDown
        gear = st.gear;
        Field<float>(v, 0x994) = std::clamp(st.gas - st.brake, -1.0f, 1.0f);  // m_fAccelerationBreakStatus: jet flames
    }
    if (st.extras & VehicleState::X_TURRET) {
        Field<float>(v, 0x94C) = st.turretYaw;
        Field<float>(v, 0x950) = st.turretPitch;
    }
}

// Hydraulics of a car a remote player drives. Its game keeps the wheel heights only while computing them from the
// right stick, so they are computed here from his stick the same way; a mirror has the mission-car status for which
// CAutomobile::HydraulicControl takes them from CVehicle::m_aSpecialHydraulicData (4 slots for the whole game).
void ApplyHydraulics(CVehicle* v, const VehicleState& st) {
    if (v->VehicleSubClass() != VEHICLE_TYPE_AUTOMOBILE || !(Field<uint32_t>(v, 0x38C) & 0x20000)) return;  // bHydraulicInst
    RemotePlayer* rp = v->Driver() ? Players::Instance().FindByPed(v->Driver()) : nullptr;
    if (!rp) return;
    const PadState& pad = rp->state.pad;
    float wheel[4] = {1, 1, 1, 1};  // front left, rear left, front right, rear right; all up while the jump is held
    if (!(pad.buttons & PadState::B_SHOCKR)) {
        float lr = pad.rx, ud = pad.ry;
        float jump = std::sqrt(ud * ud + lr * lr) * 1.5f / 128.0f, a = std::atan2(lr, ud) - 0.7853982f;
        float rr = std::cos(a) * jump, fr = std::sin(a) * jump;
        wheel[0] = std::max(0.0f, -rr);
        wheel[1] = std::max(0.0f, -fr);
        wheel[2] = std::max(0.0f, fr);
        wheel[3] = std::max(0.0f, rr);
    }
    bool active = st.misc != 0 || wheel[0] + wheel[1] + wheel[2] + wheel[3] > 0.0f;
    int8_t slot = Field<int8_t>(v, 0x48B);  // CVehicle::m_vehicleSpecialColIndex
    if (active && slot < 0 && CallMethod<0x6DF3D0, bool>(v)) slot = Field<int8_t>(v, 0x48B);  // GetSpecialColModel
    if (slot >= 0 && slot < 4) memcpy((void*)(0xC1CB60 + (uintptr_t)slot * 0x28 + 0x18), wheel, sizeof(wheel));
}

void Vehicles::ApplyMirror(NetVehicle& nv, float dt) {
    if (!nv.hasState) return;
    CVehicle* v = nv.veh;
    const VehicleState& st = nv.state;
    CPlayerPed* me = FindPlayerPed(0);

    if (nv.heldHere && !ScriptHolds(v)) {  // the script here let go of it (its mission ended): the owner may drop it
        nv.heldHere = false;
        Writer w(proto::G_VEH_SCRIPT_FREE);
        w.U16(nv.netId);
        Net().Send(proto::CH_RELIABLE, w);
    }
    if (v->CreatedBy() != VEHICLE_CREATED_MISSION) Protect(nv);  // a script let it go meanwhile (its mission ended)
    if (v->Status() == STATUS_WRECKED) return;  // burnt out: nothing left to follow
    if (st.flags & VehicleState::F_WRECKED) {
        Cmd(0x020B).I(nv.handle).Run();  // EXPLODE_CAR: it blew up in the owner's game
        return;
    }
    // a remote player (or nobody) drives it: no local pad input must reach this vehicle. A trailer keeps its own
    // status: an abandoned one looks for a cab to hitch (CTrailer::ScanForTowLink), whoever's game that cab is in
    if (v->Driver() != me && v->VehicleSubClass() != VEHICLE_TYPE_TRAILER) v->SetStatus(STATUS_PHYSICS);

    float age = std::min(0.3f, (float)(NowMs() - nv.recvMs) / 1000.0f);
    CVector target = st.pos + st.vel * age;
    CVector cur = v->GetPosition();
    CVector err = target - cur;
    float dist = err.Length();
    bool resting = st.vel.Length() < 0.05f && dist < 0.15f;
    // no ground under it in this game (its collision is not loaded here): physics would drop it through the map,
    // so it stands exactly where its owner has it
    if (NowMs() - nv.groundCheckMs > 250) {
        nv.groundCheckMs = NowMs();
        bool found = false;
        Call<0x5696C0, float>(target.x, target.y, target.z + 2.0f, &found, (CEntity**)nullptr);  // FindGroundZFor3DCoord
        nv.groundless = !found;
    }
    if (nv.groundless) {
        SetVehicleMatrix(v, target, st.rot);
        v->MoveSpeed() = CVector{};
        v->TurnSpeed() = CVector{};
    } else if (dist > 10.0f) {
        SetVehicleMatrix(v, target, st.rot);
        v->MoveSpeed() = st.vel * (1.0f / 50.0f);
        v->TurnSpeed() = st.turn * (1.0f / 50.0f);
    } else if (!resting) {
        CVector cmd = st.vel + err * 4.0f;  // converge in ~1/4 s
        v->MoveSpeed() = cmd * (1.0f / 50.0f);
        v->TurnSpeed() = st.turn * (1.0f / 50.0f);
        if (CMatrix* m = v->Matrix()) {
            Quat q = Quat::Slerp(Quat::FromMatrix(*m), st.rot, std::min(1.0f, dt * 12.0f));
            q.ToMatrix(*m);
            CallMethod<0x446F90>(v);
        }
    }
    if (nv.hasLook && NowMs() - nv.lookCheckMs >= 500) {
        nv.lookCheckMs = NowMs();
        ApplyLook(nv);
    }
    v->SteerAngle() = st.steer;
    v->GasPedal() = st.gas;
    v->BrakePedal() = st.brake;
    v->Health() = st.health;
    v->SetEngineOn((st.flags & VehicleState::F_ENGINE) != 0);
    v->SetSiren((st.flags & VehicleState::F_SIREN) != 0);
    ApplyParts(v, st);
    ApplyHydraulics(v, st);
}

void Vehicles::OnPreProcess() {
    if (!Net().Online() || !core::IsPlayerReady()) return;
    for (auto it = m_pending.begin(); it != m_pending.end();)
        it = CreateMirror(it->second) ? m_pending.erase(it) : std::next(it);
    float dt = TimeStep() / 50.0f;
    std::vector<uint16_t> dead;
    for (auto& [id, nv] : m_byId) {
        if (!nv.veh || !IsVehicleValid(nv.veh) || nv.veh->ModelIndex() != nv.model) {
            dead.push_back(id);
            continue;
        }
        if (IsMirror(nv)) ApplyMirror(nv, dt);
    }
    for (uint16_t id : dead) {
        NetVehicle* nv = Find(id);
        if (nv && !IsMirror(*nv)) {
            Writer w(proto::G_VEH_REMOVE);
            w.U16(id);
            Net().Send(proto::CH_RELIABLE, w);
        }
        if (nv) m_byPtr.erase(nv->veh);
        m_byId.erase(id);
    }
}

void Vehicles::SendState(NetVehicle& nv) {
    VehicleState st = VehicleState::Capture(nv.veh);
    st.time = (uint32_t)NowMs();
    Writer w(proto::G_VEH_SYNC);
    w.U16(nv.netId);
    st.Write(w);
    Net().Send(proto::CH_STATE, w);
}

void Vehicles::ScanTraffic() {
    CPoolHeader* pool = VehiclePool();
    if (!pool) return;
    std::vector<uint16_t> gone;
    for (auto& [id, nv] : m_byId)
        if (!IsMirror(nv) && IsVehicleValid(nv.veh) && !HasPlayerInside(nv.veh) &&
            RemoteDistance(nv.veh->GetPosition(), nv.veh->AreaCode()) > kTrafficOut)
            gone.push_back(id);
    for (uint16_t id : gone) {
        Writer w(proto::G_VEH_REMOVE);
        w.U16(id);
        Net().Send(proto::CH_RELIABLE, w);
        // a copy of the other game's car this player took: off the network an ordinary car (a mission one stayed forever)
        if (NetVehicle* nv = Find(id); nv && nv->createdByUs && IsVehicleValid(nv->veh) &&
                                       nv->veh->CreatedBy() == VEHICLE_CREATED_MISSION && !ScriptHolds(nv->veh) &&
                                       GetVehicleRef(nv->veh) != m_kept)
            SetVehicleCreatedBy(nv->veh, VEHICLE_CREATED_RANDOM);
        Forget(id, false);
    }
    for (int i = 0; i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* v = (CVehicle*)(pool->objects + (size_t)i * kVehiclePoolElemSize);
        if (m_byPtr.count(v) || !IsSyncableVehicle(v)) continue;
        if (RemoteDistance(v->GetPosition(), v->AreaCode()) < kTrafficIn) EnsureNetworked(v);
    }
}

// The car the player came in waits outside while the player is in an interior. An interior lies kilometres from its
// door (CCarCtrl::PossiblyRemoveVehicle measures on the map), so the game dropped the car, or handed it to the other
// player's game as an ordinary car that went as soon as that player got far too: both found nothing outside.
void Vehicles::KeepCarWhileInside(CPlayerPed* me) {
    if (me->InVehicle() && me->Vehicle() && me->Vehicle()->Driver() == me) m_lastDriven = GetVehicleRef(me->Vehicle());
    int8_t area = me->AreaCode();
    if (area != 0 && m_lastArea == 0 && m_kept < 0) {
        CVehicle* v = GetVehicle(m_lastDriven);
        uint8_t by = v ? v->CreatedBy() : 0;
        if (v && v->AreaCode() == 0 && (by == VEHICLE_CREATED_RANDOM || by == VEHICLE_CREATED_PARKED)) {
            m_kept = m_lastDriven;
            m_keptBy = by;
            SetVehicleCreatedBy(v, VEHICLE_CREATED_MISSION);
            LOGI("The player's car (model %d) is kept outside while the player is in interior %d", v->ModelIndex(), area);
        }
    } else if (area == 0 && m_lastArea != 0) {
        ReleaseKeptCar();
    }
    m_lastArea = area;
}

void Vehicles::ReleaseKeptCar() {
    CVehicle* v = m_kept >= 0 ? GetVehicle(m_kept) : nullptr;
    if (v && v->CreatedBy() == VEHICLE_CREATED_MISSION && !ScriptHolds(v)) SetVehicleCreatedBy(v, m_keptBy);
    m_kept = -1;
}

void Vehicles::OnPostProcess() {
    if (!Net().Online() || !core::IsPlayerReady()) return;
    CPlayerPed* me = FindPlayerPed(0);
    uint64_t now = NowMs();
    KeepCarWhileInside(me);
    for (auto& [v, to] : m_giveAway)  // still ours and still free to go
        if (NetVehicle* nv = IsVehicleValid(v) ? Find(v) : nullptr; nv && !IsMirror(*nv) && CanGiveAway(v)) GiveAway(*nv, to);
    m_giveAway.clear();
    // a trailer or a car this player's vehicle tows is simulated here from now on
    if (CVehicle* mine = me->InVehicle() && me->Vehicle() && me->Vehicle()->Driver() == me ? me->Vehicle() : nullptr)
        if (auto* towed = Field<CVehicle*>(mine, 0x4C8))  // CVehicle::m_pVehicleBeingTowed
            if (NetVehicle* tn = Find(towed); tn && IsMirror(*tn)) TakeOwnership(*tn);
    if (now - m_lastScan >= 250) {
        m_lastScan = now;
        ScanTraffic();
    }
    for (auto& [id, nv] : m_byId) {
        if (IsMirror(nv)) {
            // keep the visual controls after the physics step (wheels, brake lights)
            if (nv.hasState) {
                nv.veh->SteerAngle() = nv.state.steer;
                nv.veh->BrakePedal() = nv.state.brake;
            }
            continue;
        }
        // owned: mod shop changes; repeated every 10 s for mirrors that were still streaming their model
        if (now - nv.lookCheckMs >= 1000) {
            nv.lookCheckMs = now;
            if (!nv.hasLook || !(CaptureLook(nv.veh) == nv.look) || now - nv.lookSentMs > 10000) {
                SendLook(nv);
                nv.lookSentMs = now;
            }
        }
        // owned: the local driver's state goes in G_PLAYER_DRIVER (Players); here only unoccupied vehicles
        if (me && nv.veh->Driver() == me) {
            nv.lastChangeMs = now;
            continue;
        }
        float speed = nv.veh->MoveSpeed().Length() * 50.0f;
        if (speed > 0.1f) nv.lastChangeMs = now;
        bool active = now - nv.lastChangeMs < 3000;
        bool towed = Field<CVehicle*>(nv.veh, 0x4C4) != nullptr;  // CVehicle::m_pTowingVehicle
        uint64_t interval = towed ? 50 : active ? 100 : 2000;
        if (now - nv.lastSendMs >= interval) {
            nv.lastSendMs = now;
            SendState(nv);
        }
    }
}

void Vehicles::OnSessionEnd() {
    ReleaseKeptCar();
    // mirrors stay in the world as this game's cars (the other game's scripts hold nothing here any more)
    for (auto& [id, nv] : m_byId)
        if (IsMirror(nv)) {
            nv.script = false;
            Release(nv);
        }
    m_byId.clear();
    m_byPtr.clear();
    m_pending.clear();
}

void Vehicles::OnPeerLeave(uint8_t id) {
    for (auto it = m_pending.begin(); it != m_pending.end();)
        it = it->second.owner == id ? m_pending.erase(it) : std::next(it);
    // the vehicles of the leaving player stay in the world and become ours (its scripts are gone with it)
    for (auto& [vid, nv] : m_byId)
        if (nv.owner == id) {
            nv.owner = Net().MyId();
            nv.hasState = false;
            nv.script = false;
            Release(nv);
        }
}

void Vehicles::OnGameStarted() {
    m_lastDriven = m_kept = -1;  // a new world: the old handles mean other cars
    m_lastArea = 0;
    m_byId.clear();
    m_byPtr.clear();
    m_pending.clear();
    m_giveAway.clear();
}

void Vehicles::OnGameShutdown() { OnGameStarted(); }

}  // namespace coop
