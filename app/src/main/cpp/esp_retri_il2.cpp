#include "esp_retri_il2.hpp"
#include "mem.hpp"
#include "remote_call.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

static uintptr_t g_rtInvoke = 0;
static uintptr_t g_tryUseSkillMi = 0;

/** Summon retribution spell id (VIP / in-game retri). */
static constexpr int32_t kRetriSkillId = 20020;

static void write_u32(pid_t pid, uintptr_t addr, uint32_t v) { mem_write(pid, addr, &v, sizeof(v)); }

} // namespace

void esp_retri_il2_bind(uintptr_t runtime_invoke, uintptr_t try_use_skill_mi) {
    g_rtInvoke = runtime_invoke;
    g_tryUseSkillMi = try_use_skill_mi;
}

bool esp_retri_il2_ready() { return g_rtInvoke && g_tryUseSkillMi; }

bool esp_il2_try_use_skill(pid_t pid, uintptr_t showSelfPlayer, int32_t skillId, float dirX,
                           float dirY, float dirZ, float posX, float posY, float posZ) {
    if (!esp_retri_il2_ready() || pid <= 0 || !mem_ptr_ok(showSelfPlayer)) return false;

    RemoteSession s{};
    if (!remote_session_begin_try(pid, s)) return false;

    const uintptr_t slotSkill = remote_session_scratch(s, 0, 8);
    const uintptr_t slotDir = remote_session_scratch(s, 1, 16);
    const uintptr_t slotDirDef = remote_session_scratch(s, 2, 8);
    const uintptr_t slotPos = remote_session_scratch(s, 3, 16);
    const uintptr_t slotCommon = remote_session_scratch(s, 4, 8);
    const uintptr_t slotAlong = remote_session_scratch(s, 5, 8);
    const uintptr_t slotFirstDrag = remote_session_scratch(s, 6, 8);
    const uintptr_t slotIgnore = remote_session_scratch(s, 7, 8);
    const uintptr_t slotDragTime = remote_session_scratch(s, 8, 8);
    const uintptr_t paramArr = remote_session_scratch(s, 9, 9 * 8);
    const uintptr_t excSlot = remote_session_scratch(s, 10, 8);
    if (!slotSkill || !slotDir || !paramArr || !excSlot) {
        remote_session_end(s);
        return false;
    }

    const float dir[3] = {dirX, dirY, dirZ};
    const float pos[3] = {posX, posY, posZ};
    write_u32(pid, slotSkill, (uint32_t)skillId);
    mem_write(pid, slotDir, dir, sizeof(dir));
    write_u32(pid, slotDirDef, 1);
    mem_write(pid, slotPos, pos, sizeof(pos));
    write_u32(pid, slotCommon, 0);
    write_u32(pid, slotAlong, 1);
    write_u32(pid, slotFirstDrag, 1);
    write_u32(pid, slotIgnore, 0);
    write_u32(pid, slotDragTime, 0);

    uintptr_t zeros = 0;
    mem_write(pid, excSlot, &zeros, sizeof(zeros));

    uintptr_t argPtrs[9];
    argPtrs[0] = slotSkill;
    argPtrs[1] = slotDir;
    argPtrs[2] = slotDirDef;
    argPtrs[3] = slotPos;
    argPtrs[4] = slotCommon;
    argPtrs[5] = slotAlong;
    argPtrs[6] = slotFirstDrag;
    argPtrs[7] = slotIgnore;
    argPtrs[8] = slotDragTime;
    mem_write(pid, paramArr, argPtrs, sizeof(argPtrs));

    uintptr_t ret = 0;
    const bool ok =
        remote_session_call(s, g_rtInvoke, {g_tryUseSkillMi, showSelfPlayer, paramArr, excSlot}, ret);
    remote_session_end(s);
    return ok;
}

bool esp_retri_il2_cast_summon(pid_t pid, uintptr_t showSelfPlayer, float dirX, float dirY,
                               float dirZ) {
    return esp_il2_try_use_skill(pid, showSelfPlayer, kRetriSkillId, dirX, dirY, dirZ, 0.f, 0.f,
                                 0.f);
}
