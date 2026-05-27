#include "neutrino/arch_api.h"
#include "companion.h"

void app_main(void) {
    const arch_api_t *arch = arch_api();
    if (arch->init) arch->init();
    companion_init(arch);
    for (;;) companion_run();
}
