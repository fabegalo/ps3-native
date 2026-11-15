/* Own XInput fixtures exercise the actual poll, capability and guest ABI path. */
#include "../external/ps3recomp/libs/input/cellPad.c"
#include "vm.h"
uint8_t* vm_base;
static unsigned attached;
static unsigned has_features = 1;
static DWORD WINAPI own_state(DWORD port, XINPUT_STATE* state)
{
    memset(state, 0, sizeof *state);
    return attached & (1u << port) ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}
static DWORD WINAPI own_caps(DWORD port, DWORD flags, XINPUT_CAPABILITIES* caps)
{
    (void)flags;
    memset(caps, 0, sizeof *caps);
    if (!(attached & (1u << port))) return ERROR_DEVICE_NOT_CONNECTED;
    caps->Type = XINPUT_DEVTYPE_GAMEPAD; caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    if (has_features) {
        caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = -64;
        caps->Vibration.wLeftMotorSpeed = caps->Vibration.wRightMotorSpeed = 65535;
    }
    return ERROR_SUCCESS;
}
static void refresh(void)
{
    memset(s_pad_empty_until, 0, sizeof s_pad_empty_until);
    const unsigned long long start = pad_now_ms();
    do Sleep(1); while (pad_now_ms() - start < PAD_POLL_INTERVAL_MS);
}
int main(void)
{
    if (vm_init() || vm_commit(0x30000, 0x10000)) return 1;
    s_xi_tried = 1; p_xi_get_state = own_state; p_xi_get_caps = own_caps;
    if (cellPadInit(4)) return 2;
    const u32 out = 0x30104;
    vm_write32(out - 4, 0xA5A5A5A5); vm_write32(out + sizeof(CellPadInfo2), 0xA5A5A5A5);
    if (cellPadGetInfo2((void*)(uintptr_t)out) || vm_read32(out + 4)) return 3;
    for (unsigned port = 0; port < 7; ++port) if (vm_read32(out + 12 + port * 4)) return 4;
    attached = 1u << 2; refresh();
    if (cellPadGetInfo2((void*)(uintptr_t)out) || vm_read32(out + 4) != 1 || vm_read32(out + 20) != 3 ||
        vm_read32(out + 76) != (CELL_PAD_CAPABILITY_PS3_CONFORMITY | CELL_PAD_CAPABILITY_HP_ANALOG_STICK | CELL_PAD_CAPABILITY_ACTUATOR)) return 5;
    if (cellPadGetInfo2((void*)(uintptr_t)out) || vm_read32(out + 20) != 1) return 6;
    attached = 0; refresh();
    if (cellPadGetInfo2((void*)(uintptr_t)out) || vm_read32(out + 4) || vm_read32(out + 20) != 2 || vm_read32(out + 76)) return 7;
    attached = 1; has_features = 0; refresh();
    if (cellPadGetInfo2((void*)(uintptr_t)out) || vm_read32(out + 4) != 1 ||
        vm_read32(out + 68) != CELL_PAD_CAPABILITY_PS3_CONFORMITY || vm_read32(out - 4) != 0xA5A5A5A5 ||
        vm_read32(out + sizeof(CellPadInfo2)) != 0xA5A5A5A5) return 8;
    if (cellPadEnd()) return 9;
    vm_shutdown();
    puts("Verified absent/attached/detached controllers, measured capabilities, one-shot changes and BE guards");
    return 0;
}
