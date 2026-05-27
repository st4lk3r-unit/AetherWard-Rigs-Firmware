#include <Arduino.h>
#include "neutrino/arch_api.h"

extern "C" {
    int  brain_init(const arch_api_t *arch);
    void brain_run(void);
}

void setup() {
    const arch_api_t *arch = arch_api();
    if (arch->init) arch->init();
    brain_init(arch);
}

void loop() { brain_run(); }
