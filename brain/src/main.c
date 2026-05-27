#include "neutrino/arch_api.h"
#include "brain.h"

int main(void) {
    const arch_api_t *arch = arch_api();
    if (arch->init) arch->init();
    brain_init(arch);
    for (;;) brain_run();
    return 0;
}
