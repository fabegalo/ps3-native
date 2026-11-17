#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint64_t ps3_native_sm_begin(void);
/* Host ingress only. Supplying bytes is not proof of their hardware provenance. */
int ps3_native_sm_receive_boot(uint64_t session, const uint8_t* message, size_t size);
#ifdef __cplusplus
}
#endif
