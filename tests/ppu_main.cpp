#include "ppu_recomp.h"
#include "vm.h"
#include <cstdio>
extern "C" {
uint8_t* vm_base = nullptr;
uint32_t ppu_load_elf(const char*);
void ppu_recomp_register(void);
int ppu_run(uint32_t, uint32_t);
}
int main(int argc, char** argv) {
    if ((argc != 2 && argc != 3) || vm_init() != 0) return 1;
    auto entry = ppu_load_elf(argv[1]);
    if (entry != 0x20000) return 2;
    ppu_recomp_register();
    int rc = ppu_run(entry, 0xD00FFFF0);
    auto result = vm_read32(0x20010);
    const unsigned expected = argc == 3 ? 43 : 42; // Deliberately wrong oracle for the negative control.
    std::printf("PPU AOT: return=%d, big-endian memory result=%u (expected %u)\n", rc, result, expected);
    vm_shutdown();
    return rc != 0 || result != expected;
}
