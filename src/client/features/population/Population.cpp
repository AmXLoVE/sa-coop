// Population control for co-op:
//  * one grid for all the players: every game generates random peds, traffic and parked cars around its own player
//    as usual, but keeps only those nearer to its player than to any other player (the other games see them through
//    Peds/Vehicles sync). Around players standing together each game fills its own side, and each game's density is
//    lowered by how much its area overlaps the others', so a group sees the density of one player, no duplicates;
//  * entities owned by this game are not removed while the other player is close to them
//    (CPopulation::ManagePed / CCarCtrl::PossiblyRemoveVehicle measure distance to the nearest player);
//  * farther from this game's player, an ordinary ped or car the other player is nearer to goes to the other game
//    instead of being dropped here (it vanished in front of the other player).
#include <algorithm>
#include <cmath>
#include <vector>

#include "features/population/Population.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

// the other player is in the game (states keep coming): what it is given goes nowhere while it loads a world
bool CanTake(uint8_t who) {
    const RemotePlayer* rp = Players::Instance().Get(who);
    return rp && rp->ped && NowMs() - rp->stateRecvMs < 1500;
}

// ---------------------------------------------------------------- the grid
// A point belongs to the nearest player (in the same interior, in the game): only that player's game generates there.
bool OtherPlayerNearer(const CVector& p, int8_t area) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me) return false;
    float local = (p - me->GetPosition()).Length2D();
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.hasState && rp.state.area == area && CanTake(id) && (rp.state.pos - p).Length2D() < local) return true;
    return false;
}

// How much of this game's population area the others' cover: 1 alone, up to the number of players standing together.
// The engine fills its quota in the part of the area left to it, so its density is divided by this.
float Overlap(float fullAt, float noneAt) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me) return 1.0f;
    float k = 1.0f;
    for (auto& [id, rp] : Players::Instance().All()) {
        if (!rp.hasState || rp.state.area != me->AreaCode() || !CanTake(id)) continue;
        float d = (rp.state.pos - me->GetPosition()).Length2D();
        k += std::clamp((noneAt - d) / (noneAt - fullAt), 0.0f, 1.0f);
    }
    return k;
}

bool GridOn() { return Net().Online() && core::IsPlayerReady() && !Players::Instance().All().empty(); }

// a pool's slots before a call: a slot in use after it that was free or held another entity (its counter changed) is new
std::vector<int8_t> Slots(CPoolHeader* pool) { return std::vector<int8_t>(pool->byteMap, pool->byteMap + pool->size); }
bool IsNew(CPoolHeader* pool, const std::vector<int8_t>& before, int i) {
    return pool->byteMap[i] >= 0 && (i >= (int)before.size() || before[i] != pool->byteMap[i]);
}

bool IsLawVehicle(int model) {
    switch (model) {
    case 596: case 597: case 598: case 599: case 523: case 427: case 490: case 528: case 433: case 470: case 432: case 497: case 601:
        return true;
    }
    return false;
}

int g_droppedPeds = 0, g_droppedCars = 0;
uint64_t g_logMs = 0;

using UpdatePopFn = void(__cdecl*)(bool);
UpdatePopFn g_origPopUpdate = nullptr;
void __cdecl HookPopulationUpdate(bool generate) {
    if (!generate || !GridOn()) return g_origPopUpdate(generate);
    float& density = Ref<float>(0x8D2530);  // CPopulation::PedDensityMultiplier (scripts set it: put back after)
    float saved = density;
    density = saved / Overlap(25.0f, 110.0f);
    CPoolHeader* pool = PedPool();
    auto before = Slots(pool);
    g_origPopUpdate(generate);
    density = saved;
    // the new random peds on another player's side go at once, before anything else sees them
    for (int i = 0; i < pool->size; ++i) {
        if (!IsNew(pool, before, i)) continue;
        auto* ped = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
        if (ped->CreatedBy() != PED_GAME || ped->IsPlayer() || ped->InVehicle() || ped->PedType() == PED_TYPE_COP) continue;
        if (!OtherPlayerNearer(ped->GetPosition(), ped->AreaCode())) continue;
        Call<0x610F20>(ped);  // CPopulation::RemovePed
        ++g_droppedPeds;
    }
}

using VoidFn = void(__cdecl*)();
VoidFn g_origGenerateRandomCars = nullptr;
void __cdecl HookGenerateRandomCars() {
    if (!GridOn()) return g_origGenerateRandomCars();
    float& density = Ref<float>(0x8A5B20);  // CCarCtrl::CarDensityMultiplier
    float saved = density;
    density = saved / Overlap(40.0f, 200.0f);
    CPoolHeader* pool = VehiclePool();
    auto before = Slots(pool);
    g_origGenerateRandomCars();
    density = saved;
    for (int i = 0; i < pool->size; ++i) {
        if (!IsNew(pool, before, i)) continue;
        auto* v = (CVehicle*)(pool->objects + (size_t)i * kVehiclePoolElemSize);
        if (v->CreatedBy() != 1 /* RANDOM_VEHICLE */ || IsLawVehicle(v->ModelIndex())) continue;
        if (!OtherPlayerNearer(v->GetPosition(), v->AreaCode())) continue;
        // the engine's own way to drop a random car with its people (CCarCtrl::RemoveCarsIfThePoolGetsFull)
        Call<0x563280>((CEntity*)v);  // CWorld::Remove
        using DeleteFn = void(__fastcall*)(void*, void*, int);
        (*(DeleteFn**)v)[0](v, nullptr, 1);  // delete
        ++g_droppedCars;
    }
}

// CCarGenerator::Process: a parked car's spot is generated by the game of the player nearest to it
using GenProcessFn = void(__fastcall*)(void*, void*);
GenProcessFn g_origGenProcess = nullptr;
void __fastcall HookCarGeneratorProcess(void* gen, void* edx) {
    if (GridOn() && Field<int16_t>(gen, 0x18) == -1) {  // m_nVehicleHandle: none made yet
        CVector p{Field<int16_t>(gen, 4) / 8.0f, Field<int16_t>(gen, 6) / 8.0f, Field<int16_t>(gen, 8) / 8.0f};
        if (OtherPlayerNearer(p, FindPlayerPed(0)->AreaCode())) return;
    }
    g_origGenProcess(gen, edx);
}

// nearest remote player position (same interior) within maxDist, or nullptr
const CVector* NearestRemote(const CVector& p, float maxDist, int8_t area, uint8_t* who = nullptr) {
    const CVector* best = nullptr;
    float bestD = maxDist;
    for (auto& [id, rp] : Players::Instance().All()) {
        if (!rp.hasState || rp.state.area != area) continue;
        float d = (rp.state.pos - p).Length2D();
        if (d < bestD) {
            bestD = d;
            best = &rp.state.pos;
            if (who) *who = id;
        }
    }
    return best;
}

using ManagePedFn = void(__cdecl*)(CPed*, const CVector*);
ManagePedFn g_origManagePed = nullptr;
void __cdecl HookManagePed(CPed* ped, const CVector* centre) {
    if (Net().Online() && ped && centre) {
        CVector p = ped->GetPosition();
        float local = (p - *centre).Length2D();
        uint8_t who = 0;
        if (const CVector* r = NearestRemote(p, local, ped->AreaCode(), &who)) {
            // far from this game's player and clearly nearer the other one: the other game keeps it (the margin keeps a
            // ped from going back and forth between two players standing together)
            if (local > 60.0f && (p - *r).Length2D() < local - 30.0f && CanTake(who) && Peds::Instance().QueueGiveAway(ped, who))
                return;
            // the other player still needs this ped, yet the engine culls by this game's camera too (off screen and
            // over 25 m: gone in 4 s). While this game has ground under it (collision loads ~120 m around its
            // player), a live one near the other player stays; farther it would fall through the map here, so it
            // goes as usual and the other player's own population takes over
            if (local < 100.0f && (p - *r).Length2D() < 50.0f && ped->Health() > 0.0f) return;
            g_origManagePed(ped, r);
            return;
        }
    }
    g_origManagePed(ped, centre);
}

using PossiblyRemoveFn = void(__cdecl*)(CVehicle*);
PossiblyRemoveFn g_origPossiblyRemove = nullptr;
void __cdecl HookPossiblyRemoveVehicle(CVehicle* v) {
    // kept for the other player while this game has ground under it (see HookManagePed), then given to it; a wreck
    // goes as usual (kept, they filled the vehicle pool around two players)
    uint8_t who = 0;
    if (Net().Online() && v && v->Status() != 5 /* STATUS_WRECKED */)
        if (const CVector* r = NearestRemote(v->GetPosition(), 140.0f, v->AreaCode(), &who)) {
            float local = (v->GetPosition() - FindPlayerPed(0)->GetPosition()).Length2D();
            if (local < 100.0f) return;
            if ((v->GetPosition() - *r).Length2D() < local - 40.0f && CanTake(who) && Vehicles::Instance().QueueGiveAway(v, who))
                return;
        }
    g_origPossiblyRemove(v);
}

class Population : public Module {
public:
    const char* Name() const override { return "Population"; }
    void OnInstallHooks() override {
        mem::HookFn(0x616650, &HookPopulationUpdate, &g_origPopUpdate, "CPopulation::Update");
        mem::HookFn(0x4341C0, &HookGenerateRandomCars, &g_origGenerateRandomCars, "CCarCtrl::GenerateRandomCars");
        mem::HookFn(0x6F3E90, &HookCarGeneratorProcess, &g_origGenProcess, "CCarGenerator::Process");
        mem::HookFn(0x611FC0, &HookManagePed, &g_origManagePed, "CPopulation::ManagePed");
        mem::HookFn(0x424F80, &HookPossiblyRemoveVehicle, &g_origPossiblyRemove, "CCarCtrl::PossiblyRemoveVehicle");
    }
    void OnPostProcess() override {
        if (NowMs() - g_logMs < 30000) return;
        g_logMs = NowMs();
        if (g_droppedPeds || g_droppedCars)
            LOGI("Population grid: %d peds and %d cars on other players' sides dropped (density / %.2f peds, / %.2f cars)",
                 g_droppedPeds, g_droppedCars, GridOn() ? Overlap(25.0f, 110.0f) : 1.0f, GridOn() ? Overlap(40.0f, 200.0f) : 1.0f);
        g_droppedPeds = g_droppedCars = 0;
    }
};

Population g_pop;
struct Reg {
    Reg() { core::RegisterModule(&g_pop); }
} g_reg;
}  // namespace

}  // namespace coop
