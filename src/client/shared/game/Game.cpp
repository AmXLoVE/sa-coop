#include "shared/game/Game.h"

#include <windows.h>
#undef GetObject  // windows.h renames it GetObjectA: game::GetObject is the script handle lookup

namespace game {

bool IsForeground() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

CPoolHeader* PedPool() { return Ref<CPoolHeader*>(addr::PedPool); }
CPoolHeader* VehiclePool() { return Ref<CPoolHeader*>(addr::VehiclePool); }
CPoolHeader* ObjectPool() { return Ref<CPoolHeader*>(addr::ObjectPool); }

int GetPedRef(CPed* ped) { return ped ? Call<0x54FF60, int>(ped) : -1; }
CPed* GetPed(int handle) { return handle < 0 ? nullptr : Call<0x54FF90, CPed*>(handle); }
int GetVehicleRef(CVehicle* v) { return v ? Call<0x54FFC0, int>(v) : -1; }
CVehicle* GetVehicle(int handle) { return handle < 0 ? nullptr : Call<0x54FFF0, CVehicle*>(handle); }
int GetObjectRef(CObject* o) { return o ? Call<0x550020, int>(o) : -1; }
CObject* GetObject(int handle) { return handle < 0 ? nullptr : Call<0x550050, CObject*>(handle); }

static bool IsInPool(CPoolHeader* pool, void* p, int elemSize) {
    if (!pool || !p) return false;
    uintptr_t base = (uintptr_t)pool->objects;
    uintptr_t addr = (uintptr_t)p;
    if (addr < base) return false;
    uintptr_t off = addr - base;
    if (off % (uintptr_t)elemSize) return false;
    uintptr_t idx = off / (uintptr_t)elemSize;
    if (idx >= (uintptr_t)pool->size) return false;
    return pool->byteMap[idx] >= 0;  // bit 7 clear = slot in use
}

bool IsPedValid(CPed* ped) { return IsInPool(PedPool(), ped, kPedPoolElemSize); }
bool IsVehicleValid(CVehicle* v) { return IsInPool(VehiclePool(), v, kVehiclePoolElemSize); }
bool IsObjectValid(CObject* o) { return IsInPool(ObjectPool(), o, kObjectPoolElemSize); }

void RequestModel(int model, int flags) { Call<0x4087E0>(model, flags); }
void LoadAllRequestedModels(bool onlyPriority) { Call<0x40EA10>(onlyPriority); }
bool IsModelLoaded(int model) {
    return model >= 0 && Ref<uint8_t>(addr::StreamingInfo + (uintptr_t)model * 0x14 + 0x10) == 1;
}
bool LoadModelNow(int model) {
    if (!IsValidModel(model)) return false;
    if (IsModelLoaded(model)) return true;
    RequestModel(model, 2 | 0x10 /* GAME_REQUIRED | PRIORITY_REQUEST */);
    LoadAllRequestedModels(false);
    return IsModelLoaded(model);
}
void MarkModelNotNeeded(int model) {
    // CStreaming::SetModelIsDeletable
    if (IsValidModel(model)) Call<0x409C10>(model);
}

static void* ModelInfo(int model) {
    if (model < 0 || model >= 20000) return nullptr;
    return Ref<void*>(addr::ModelInfoPtrs + (uintptr_t)model * 4);
}
static int ModelType(int model) {
    void* mi = ModelInfo(model);
    return mi ? CallVirtual<4, uint8_t>(mi) : 0;
}
bool IsValidModel(int model) { return ModelInfo(model) != nullptr; }
bool IsVehicleModel(int model) { return ModelType(model) == 6; }
bool IsPedModel(int model) { return ModelType(model) == 7; }

void WorldAdd(CEntity* e) { Call<0x563220>(e); }

void SetEntityPosition(CEntity* e, const CVector& pos) {
    if (!e) return;
    e->SetPositionRaw(pos);
    CallMethod<0x446F90>(e);  // CEntity::UpdateRW
    CallMethod<0x532B00>(e);  // CEntity::UpdateRwFrame
}

bool WorldToScreen(const CVector& world, float& sx, float& sy, float& depth) {
    RwV3d in{world.x, world.y, world.z}, out{};
    float w = 0, h = 0;
    // CSprite::CalcScreenCoors(const RwV3d& in, RwV3d* out, float* w, float* h, bool checkMaxVisible, bool checkMinVisible)
    // clip checks disabled: CDraw near/far values are not valid at the end of the frame; depth is checked here
    Call<0x70CE30, bool>(&in, &out, &w, &h, false, false);
    sx = out.x;
    sy = out.y;
    depth = out.z;
    return out.z > 1.0f && out.z < 1000.0f;
}
void WorldRemove(CEntity* e) { Call<0x563280>(e); }

void LightArea(int area, const CVector& pos) {
    int sky = 0;
    float best = 150.0f * 150.0f;
    auto* pool = Ref<CPoolHeader*>(0x96A7D8);  // CEntryExitManager::mp_poolEntryExits, CEntryExit 0x3C bytes
    for (int i = 0; area != 0 && pool && i < pool->size; ++i) {
        uintptr_t e = (uintptr_t)pool->objects + (uintptr_t)i * 0x3C;
        if (pool->byteMap[i] < 0 || Ref<int8_t>(e + 0x32) != area) continue;  // m_nArea: the door is in this interior
        const float* r = &Ref<float>(e + 0x08);                                 // m_recEntrance
        float dx = (r[0] + r[2]) * 0.5f - pos.x, dy = (r[1] + r[3]) * 0.5f - pos.y;
        if (dx * dx + dy * dy < best) best = dx * dx + dy * dy, sky = Ref<uint8_t>(e + 0x33);  // m_nSkyColor
    }
    if (sky) Call<0x55FEC0>(sky - 1, false);  // CTimeCycle::StartExtraColour
    else Call<0x55FF20>(false);              // CTimeCycle::StopExtraColour
}

}  // namespace game
