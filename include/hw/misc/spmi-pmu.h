#pragma once

#include "qemu/osdep.h"
#include "hw/sysbus.h"

#define TYPE_APPLE_SPMI_PMU "apple-spmi-pmu"
OBJECT_DECLARE_SIMPLE_TYPE(AppleSPMIPMUState, APPLE_SPMI_PMU)
