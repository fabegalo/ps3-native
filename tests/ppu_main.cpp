#include "ppu_recomp.h"
#include "vm.h"
#include <cstdio>
extern "C" {
uint8_t* vm_base = nullptr;
uint32_t ppu_load_elf(const char*);
void ppu_recomp_register(void);
int ppu_run(uint32_t, uint32_t);
void lv2_init_syscalls(void);
void lv2_syscall(ppu_context*);
uint32_t ppu_thread_allocate_main_stack(uint32_t);
uint32_t ppu_primary_stack_size(void);
int32_t sys_process_is_stack(uint32_t);
}
int main(int argc, char** argv) {
    if ((argc != 2 && argc != 3) || vm_init() != 0) return 1;
    auto entry = ppu_load_elf(argv[1]);
    if (entry != 0x20000) return 2;
    lv2_init_syscalls();
    ppu_context process{};
    process.gpr[11] = 1; lv2_syscall(&process);
    const auto pid = process.gpr[3];
    process.gpr[11] = 25; process.gpr[4] = 0x20014; lv2_syscall(&process);
    if (process.gpr[3] || vm_read32(0x20014) != 0x12345678) return 3;
    vm_write32(0x20014, 0xA5A5A5A5);
    process.gpr[3] = pid + 1; lv2_syscall(&process);
    if ((uint32_t)process.gpr[3] != 0x80010005 || vm_read32(0x20014) != 0xA5A5A5A5) return 4;
    ppu_recomp_register();
    const auto stack = ppu_thread_allocate_main_stack(ppu_primary_stack_size());
    if (!stack || !sys_process_is_stack(stack - 1) || ppu_thread_allocate_main_stack(0x10000) ||
        sys_process_is_stack(0xCFFFFFFF) || !sys_process_is_stack(0xD0000000) ||
        !sys_process_is_stack(0xDFFFFFFF) || sys_process_is_stack(0xE0000000)) return 5;
    int rc = ppu_run(entry, stack);
    auto result = vm_read32(0x20010);
    const unsigned expected = argc == 3 ? 43 : 42; // Deliberately wrong oracle for the negative control.
    std::printf("PPU AOT: return=%d, big-endian memory result=%u (expected %u)\n", rc, result, expected);
    vm_shutdown();
    return rc != 0 || result != expected;
}
