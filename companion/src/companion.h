#pragma once
#include "neutrino/arch_api.h"
#include "aw_bus/aw_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

int  companion_init(const arch_api_t *arch);
void companion_run(void);

#ifdef __cplusplus
}
#endif
