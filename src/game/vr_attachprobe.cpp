// dev_attachprobe: which game function turns the player inventory's busy word (+0xB4) to 0x13, the suppressor's
// attach animation (mission 2 starts it by itself, NOTES). Each candidate (a function with a store to some +0xB4) is
// wrapped: the busy word before and after the call, logged with the function, ecx and the return address when it
// becomes 0x13. Off without the file (one bool test a call)
#include <cstdint>
#include <filesystem>

#include "app/app.h"
#include "core/log.h"
#include "xdk/hle.h"

namespace weapons {
uint32_t InvBusy();  // weapons.cpp: the player inventory's +0xB4, ~0 out of a level
}

namespace {
bool Probe() {
    static const bool on = std::filesystem::exists(app::ExeDir() / "dev_attachprobe");
    return on;
}
void Check(const char* name, uint32_t before, X86Context& ctx, uint32_t ecx, uint32_t ret) {
    uint32_t after = weapons::InvBusy();
    if (after != before && (after == 0x13 || before == 0x13))
        LOGR("attachprobe: %s busy 0x%X -> 0x%X (ecx %08X, called from %08X)", name, before, after, ecx, ret);
    (void)ctx;
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_001AE3C0);
X86_FUNC(sub_001AE3C0) {
    if (!Probe()) return __imp__sub_001AE3C0(ctx, base);
    uint32_t before = weapons::InvBusy(), ecx = ctx.ecx, ret = kernel::R32(ctx.esp);
    __imp__sub_001AE3C0(ctx, base);
    Check("sub_001AE3C0", before, ctx, ecx, ret);
}
X86_EXTERN_FUNC(__imp__sub_001B40C0);
X86_FUNC(sub_001B40C0) {
    if (!Probe()) return __imp__sub_001B40C0(ctx, base);
    uint32_t before = weapons::InvBusy(), ecx = ctx.ecx, ret = kernel::R32(ctx.esp);
    __imp__sub_001B40C0(ctx, base);
    Check("sub_001B40C0", before, ctx, ecx, ret);
}
X86_EXTERN_FUNC(__imp__sub_001B4410);
X86_FUNC(sub_001B4410) {
    if (!Probe()) return __imp__sub_001B4410(ctx, base);
    uint32_t before = weapons::InvBusy(), ecx = ctx.ecx, ret = kernel::R32(ctx.esp);
    __imp__sub_001B4410(ctx, base);
    Check("sub_001B4410", before, ctx, ecx, ret);
}
X86_EXTERN_FUNC(__imp__sub_001B46E0);
X86_FUNC(sub_001B46E0) {
    if (!Probe()) return __imp__sub_001B46E0(ctx, base);
    uint32_t before = weapons::InvBusy(), ecx = ctx.ecx, ret = kernel::R32(ctx.esp);
    __imp__sub_001B46E0(ctx, base);
    Check("sub_001B46E0", before, ctx, ecx, ret);
}
X86_EXTERN_FUNC(__imp__sub_001B4EB0);
X86_FUNC(sub_001B4EB0) {
    if (!Probe()) return __imp__sub_001B4EB0(ctx, base);
    uint32_t before = weapons::InvBusy(), ecx = ctx.ecx, ret = kernel::R32(ctx.esp);
    __imp__sub_001B4EB0(ctx, base);
    Check("sub_001B4EB0", before, ctx, ecx, ret);
}
X86_EXTERN_FUNC(__imp__sub_001B7730);
X86_FUNC(sub_001B7730) {
    if (!Probe()) return __imp__sub_001B7730(ctx, base);
    uint32_t before = weapons::InvBusy(), ecx = ctx.ecx, ret = kernel::R32(ctx.esp);
    __imp__sub_001B7730(ctx, base);
    Check("sub_001B7730", before, ctx, ecx, ret);
}
X86_EXTERN_FUNC(__imp__sub_001B7D40);
X86_FUNC(sub_001B7D40) {
    if (!Probe()) return __imp__sub_001B7D40(ctx, base);
    uint32_t before = weapons::InvBusy(), ecx = ctx.ecx, ret = kernel::R32(ctx.esp);
    __imp__sub_001B7D40(ctx, base);
    Check("sub_001B7D40", before, ctx, ecx, ret);
}
X86_EXTERN_FUNC(__imp__sub_001BD3A0);
X86_FUNC(sub_001BD3A0) {
    if (!Probe()) return __imp__sub_001BD3A0(ctx, base);
    uint32_t before = weapons::InvBusy(), ecx = ctx.ecx, ret = kernel::R32(ctx.esp);
    __imp__sub_001BD3A0(ctx, base);
    Check("sub_001BD3A0", before, ctx, ecx, ret);
}
X86_EXTERN_FUNC(__imp__sub_001C7ED0);
X86_FUNC(sub_001C7ED0) {
    if (!Probe()) return __imp__sub_001C7ED0(ctx, base);
    uint32_t before = weapons::InvBusy(), ecx = ctx.ecx, ret = kernel::R32(ctx.esp);
    __imp__sub_001C7ED0(ctx, base);
    Check("sub_001C7ED0", before, ctx, ecx, ret);
}
X86_EXTERN_FUNC(__imp__sub_001C8450);
X86_FUNC(sub_001C8450) {
    if (!Probe()) return __imp__sub_001C8450(ctx, base);
    uint32_t before = weapons::InvBusy(), ecx = ctx.ecx, ret = kernel::R32(ctx.esp);
    __imp__sub_001C8450(ctx, base);
    Check("sub_001C8450", before, ctx, ecx, ret);
}
X86_EXTERN_FUNC(__imp__sub_001D0160);
X86_FUNC(sub_001D0160) {
    if (!Probe()) return __imp__sub_001D0160(ctx, base);
    uint32_t before = weapons::InvBusy(), ecx = ctx.ecx, ret = kernel::R32(ctx.esp);
    __imp__sub_001D0160(ctx, base);
    Check("sub_001D0160", before, ctx, ecx, ret);
}
X86_EXTERN_FUNC(__imp__sub_001D9210);
X86_FUNC(sub_001D9210) {
    if (!Probe()) return __imp__sub_001D9210(ctx, base);
    uint32_t before = weapons::InvBusy(), ecx = ctx.ecx, ret = kernel::R32(ctx.esp);
    __imp__sub_001D9210(ctx, base);
    Check("sub_001D9210", before, ctx, ecx, ret);
}
X86_EXTERN_FUNC(__imp__sub_001D9320);
X86_FUNC(sub_001D9320) {
    if (!Probe()) return __imp__sub_001D9320(ctx, base);
    uint32_t before = weapons::InvBusy(), ecx = ctx.ecx, ret = kernel::R32(ctx.esp);
    __imp__sub_001D9320(ctx, base);
    Check("sub_001D9320", before, ctx, ecx, ret);
}
X86_EXTERN_FUNC(__imp__sub_001DA4F0);
X86_FUNC(sub_001DA4F0) {
    if (!Probe()) return __imp__sub_001DA4F0(ctx, base);
    uint32_t before = weapons::InvBusy(), ecx = ctx.ecx, ret = kernel::R32(ctx.esp);
    __imp__sub_001DA4F0(ctx, base);
    Check("sub_001DA4F0", before, ctx, ecx, ret);
}
X86_EXTERN_FUNC(__imp__sub_001E1800);
X86_FUNC(sub_001E1800) {
    if (!Probe()) return __imp__sub_001E1800(ctx, base);
    uint32_t before = weapons::InvBusy(), ecx = ctx.ecx, ret = kernel::R32(ctx.esp);
    __imp__sub_001E1800(ctx, base);
    Check("sub_001E1800", before, ctx, ecx, ret);
}
X86_EXTERN_FUNC(__imp__sub_001E68B0);
X86_FUNC(sub_001E68B0) {
    if (!Probe()) return __imp__sub_001E68B0(ctx, base);
    uint32_t before = weapons::InvBusy(), ecx = ctx.ecx, ret = kernel::R32(ctx.esp);
    __imp__sub_001E68B0(ctx, base);
    Check("sub_001E68B0", before, ctx, ecx, ret);
}
X86_EXTERN_FUNC(__imp__sub_001F49F0);
X86_FUNC(sub_001F49F0) {
    if (!Probe()) return __imp__sub_001F49F0(ctx, base);
    uint32_t before = weapons::InvBusy(), ecx = ctx.ecx, ret = kernel::R32(ctx.esp);
    __imp__sub_001F49F0(ctx, base);
    Check("sub_001F49F0", before, ctx, ecx, ret);
}
