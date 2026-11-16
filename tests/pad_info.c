/* Own XInput fixtures exercise the actual poll, capability and guest ABI path. */
#include "../external/ps3recomp/libs/input/cellPad.c"
#include "vm.h"
uint8_t* vm_base;
extern u32 g_ppu_main_stack_addr, g_ppu_main_stack_size;
extern u32 g_ps3_sdk_version;
static unsigned attached;
static unsigned has_features = 1;
static XINPUT_GAMEPAD own_gamepad;
static unsigned rumble_calls;
static DWORD rumble_result;
static XINPUT_VIBRATION own_vibration;
static DWORD WINAPI own_rumble(DWORD port, XINPUT_VIBRATION* vibration)
{
    if (port != 0) return ERROR_DEVICE_NOT_CONNECTED;
    own_vibration = *vibration; ++rumble_calls; return rumble_result;
}
static DWORD WINAPI own_state(DWORD port, XINPUT_STATE* state)
{
    memset(state, 0, sizeof *state);
    state->Gamepad = own_gamepad;
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
int main(int argc, char** argv)
{
    if ((u32)cellPadSetPortSetting(0, 0) != (u32)CELL_PAD_ERROR_NOT_OPENED) return 10;
    if (vm_init() || vm_commit(0x30000, 0x10000)) return 1;
    g_ppu_main_stack_addr = 0x30000; g_ppu_main_stack_size = 0x10000;
    s_xi_tried = 1; p_xi_get_state = own_state; p_xi_get_caps = own_caps;
    p_xi_set_state = own_rumble; g_ps3_sdk_version = 0x280001;
    if (cellPadInit(7) || cellPadSetPortSetting(6, CELL_PAD_SETTING_PRESS_ON) ||
        (u32)cellPadSetPortSetting(7, 0) != (u32)CELL_PAD_ERROR_INVALID_PARAMETER) return 2;
    const u32 out = 0x30104;
    vm_write32(out - 4, 0xA5A5A5A5); vm_write32(out + sizeof(CellPadInfo2), 0xA5A5A5A5);
    if (cellPadGetInfo2((void*)(uintptr_t)out) || vm_read32(out) != 7 || vm_read32(out + 4) ||
        vm_read32(out + 64) != CELL_PAD_SETTING_PRESS_ON || vm_read32(out + 92)) return 3;
    const u32 data = 0x30204;
    const u32 actuator = 0x30300;
    memset(vm_base + actuator, 0, 8);
    if (sizeof(CellPadActParam) != 8 || (u32)cellPadSetActDirect(0, (void*)(uintptr_t)actuator) != 0x80121107u ||
        (u32)cellPadSetActDirect(0, (void*)(uintptr_t)(actuator + 0x100000000ull)) != 0x80121102u ||
        (u32)cellPadSetActDirect(0, (void*)(uintptr_t)0x3FFFC) != 0x80121102u || rumble_calls) return 24;
    vm_write8(actuator + 7, 1);
    if ((u32)cellPadSetActDirect(0, (void*)(uintptr_t)actuator) != 0x80121102u) return 25;
    vm_write8(actuator + 7, 0);
    memset(vm_base + data - 4, 0xA5, sizeof(CellPadData) + 8);
    if ((u32)cellPadGetData(0, (void*)(uintptr_t)data) != 0x80121107u || vm_read32(data) != 0xA5A5A5A5 ||
        (u32)cellPadGetData(7, (void*)(uintptr_t)data) != (u32)CELL_PAD_ERROR_INVALID_PARAMETER ||
        (u32)cellPadGetData(0, (void*)(uintptr_t)(0x100000000ull + data)) != (u32)CELL_PAD_ERROR_INVALID_PARAMETER) return 13;
    for (unsigned port = 0; port < 7; ++port) if (vm_read32(out + 12 + port * 4)) return 4;
    if (cellPadSetPortSetting(2, 0xFFFFFFFFu)) return 11;
    attached = 1u << 2; refresh();
    if (cellPadGetInfo2((void*)(uintptr_t)out) || vm_read32(out + 4) != 1 || vm_read32(out + 20) != 3 ||
        vm_read32(out + 48) != 6 ||
        vm_read32(out + 76) != (CELL_PAD_CAPABILITY_PS3_CONFORMITY | CELL_PAD_CAPABILITY_HP_ANALOG_STICK | CELL_PAD_CAPABILITY_ACTUATOR)) return 5;
    if (cellPadGetInfo2((void*)(uintptr_t)out) || vm_read32(out + 20) != 1) return 6;
    attached = 0; refresh();
    if (cellPadGetInfo2((void*)(uintptr_t)out) || vm_read32(out + 4) || vm_read32(out + 20) != 2 ||
        vm_read32(out + 48) != 6 || vm_read32(out + 76)) return 7;
    if (cellPadSetPortSetting(2, 0) || cellPadGetInfo2((void*)(uintptr_t)out) || vm_read32(out + 48)) return 12;
    attached = 1; has_features = 0; refresh();
    if (cellPadGetInfo2((void*)(uintptr_t)out) || vm_read32(out + 4) != 1 ||
        vm_read32(out + 68) != CELL_PAD_CAPABILITY_PS3_CONFORMITY || vm_read32(out - 4) != 0xA5A5A5A5 ||
        vm_read32(out + sizeof(CellPadInfo2)) != 0xA5A5A5A5) return 8;
    if ((u32)cellPadSetActDirect(0, (void*)(uintptr_t)actuator) != 0x80121108u || rumble_calls) return 26;
    has_features = 1;
    own_gamepad.wButtons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_LEFT_SHOULDER;
    own_gamepad.bLeftTrigger = 255;
    own_gamepad.sThumbLX = -32768; own_gamepad.sThumbLY = 32767;
    own_gamepad.sThumbRX = 32767; own_gamepad.sThumbRY = -32768;
    refresh();
    vm_write8(actuator, 1); vm_write8(actuator + 1, 128);
    if (cellPadSetActDirect(0, (void*)(uintptr_t)actuator) || rumble_calls != 1 ||
        own_vibration.wLeftMotorSpeed != 32896 || own_vibration.wRightMotorSpeed != 65535) return 27;
    rumble_result = ERROR_DEVICE_NOT_CONNECTED;
    if ((u32)cellPadSetActDirect(0, (void*)(uintptr_t)actuator) != 0x80121107u) return 28;
    rumble_result = ERROR_SUCCESS;
    memset(vm_base + actuator, 0, 8);
    if (cellPadSetActDirect(0, (void*)(uintptr_t)actuator) || own_vibration.wLeftMotorSpeed || own_vibration.wRightMotorSpeed) return 29;
    if (argc == 2) {
        if (!strcmp(argv[1], "actuator")) {
            p_xi_set_state = NULL; cellPadSetActDirect(0, (void*)(uintptr_t)actuator); return 30;
        }
        cellPadSetPortSetting(0, !strcmp(argv[1], "sensors") ? CELL_PAD_SETTING_SENSOR_ON : CELL_PAD_SETTING_PRESS_ON);
        cellPadGetData(0, (void*)(uintptr_t)data); return 23;
    }
    _putenv_s("PAD_AUTOPRESS", "1");
    if (cellPadGetData(0, (void*)(uintptr_t)data) || vm_read32(data) != 8 || vm_read16(data + 4) ||
        vm_read16(data + 6) != 0x74 || vm_read16(data + 8) != 0x18 || vm_read16(data + 10) != 0x45 ||
        vm_read16(data + 12) != 255 || vm_read16(data + 14) != 255 || vm_read16(data + 16) || vm_read16(data + 18) ||
        vm_read32(data - 4) != 0xA5A5A5A5 || vm_read32(data + sizeof(CellPadData)) != 0xA5A5A5A5 ||
        vm_read16(data + 44) != 0xA5A5) return 14;
    for (u32 i = 8; i < 20; ++i) if (vm_read16(data + 4 + i * 2)) return 15;
    if (cellPadGetData(0, (void*)(uintptr_t)data) || vm_read32(data) || vm_read16(data + 10) != 0x45) return 16;
    refresh();
    if (cellPadGetData(0, (void*)(uintptr_t)data) || vm_read32(data)) return 17;
    memset(&own_gamepad, 0, sizeof own_gamepad); refresh();
    if (cellPadGetData(0, (void*)(uintptr_t)data) || vm_read32(data) != 8 || vm_read32(data + 8) ||
        vm_read16(data + 12) != 128 || vm_read16(data + 14) != 128 ||
        vm_read16(data + 16) != 128 || vm_read16(data + 18) != 128) return 18;
    attached = 0; refresh(); vm_write32(data, 0xA5A5A5A5);
    if ((u32)cellPadGetData(0, (void*)(uintptr_t)data) != 0x80121107u || vm_read32(data) != 0xA5A5A5A5) return 19;
    if (cellPadEnd() || (u32)cellPadGetData(0, (void*)(uintptr_t)data) != (u32)CELL_PAD_ERROR_NOT_OPENED) return 9;
    vm_shutdown();
    puts("Verified real-state pad packets, digital mapping, stick endpoints/neutral, change delivery, absent devices and BE guards");
    return 0;
}
