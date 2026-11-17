/* BSD-3-Clause. Read-only syscall probe; no firmware-derived code.
 * Requires a process permitted to call syscall 380. It does not grant permissions.
 * Record the hardware/firmware and startup conditions separately; no golden values
 * are inferred here. The 4.93 kernel copies eight bytes to each argument. */
#include <stdint.h>
extern int printf(const char*, ...);

static int64_t query(uint64_t* a, uint64_t* b, uint64_t* c, uint64_t* d) {
    register uint64_t r3 __asm__("r3") = (uintptr_t)a;
    register uint64_t r4 __asm__("r4") = (uintptr_t)b;
    register uint64_t r5 __asm__("r5") = (uintptr_t)c;
    register uint64_t r6 __asm__("r6") = (uintptr_t)d;
    register uint64_t r11 __asm__("r11") = 380;
    __asm__ volatile("sc" : "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6), "+r"(r11)
        : : "r0", "r7", "r8", "r9", "r10", "r12", "cr0", "ctr", "lr", "xer", "memory");
    return (int64_t)r3;
}

int main(void) {
    const uint64_t sentinel = UINT64_C(0xA5A5A5A5A5A5A5A5);
    struct { uint64_t before, value, after; } output[4];
    for (unsigned i = 0; i < 4; ++i)
        output[i].before = output[i].value = output[i].after = sentinel;
    const int64_t result = query(&output[0].value, &output[1].value, &output[2].value, &output[3].value);
    int intact = 1;
    for (unsigned i = 0; i < 4; ++i)
        if (output[i].before != sentinel || output[i].after != sentinel) intact = 0;
    printf("{\"probe\":\"ps3-native-sm-params-v1\",\"syscall\":380,\"result\":\"%08x\","
           "\"canaries_intact\":%s,\"values\":[\"%016llx\",\"%016llx\",\"%016llx\",\"%016llx\"]}\n",
           (unsigned)result, intact ? "true" : "false",
           (unsigned long long)output[0].value, (unsigned long long)output[1].value,
           (unsigned long long)output[2].value, (unsigned long long)output[3].value);
    return !intact ? 3 : result ? 2 : 0;
}
