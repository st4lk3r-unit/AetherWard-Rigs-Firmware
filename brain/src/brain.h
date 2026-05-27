#pragma once
#include "neutrino/arch_api.h"
#include "konsole/konsole.h"
#include "konsole/static.h"
#include "aw_bus/aw_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

int  brain_init(const arch_api_t *arch);
void brain_run(void);

/* Exposed so konsole commands can call them */
awbus_t *brain_bus(void);

#ifdef __cplusplus
}
#endif
