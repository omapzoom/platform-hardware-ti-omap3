/*
 * OMAP CPU THERMAL GOVERNOR (header file)
 *
 * Copyright (C) 2011 Texas Instruments, Inc.
 *
 * Sebastien Sabatier (s-sabatier1@ti.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the dual BSD / GNU General Public License version 2 as
 * published by the Free Software Foundation. When using or
 * redistributing this file, you may do so under either license.
 */

#ifndef _CPU_THERMAL_GOVERNOR_H
#define _CPU_THERMAL_GOVERNOR_H

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include "sysfs.h"
#include "read_config.h"

#define HYSTERESIS_VALUE 2000 /* in milli-celsius degrees to be compliant with HWMON APIs */
#define NORMAL_TEMP_MONITORING_RATE 1000 /* 1 second */
#define FAST_TEMP_MONITORING_RATE 250 /* 250 milli-seconds */
#define OMAP_CPU_THRESHOLD_FATAL 125000 /* 125 DegC */
#define OMAP_CPU_MIN_TEMP -40000
#define OMAP_CPU_MAX_TEMP 125000

/* Constants defined to compute the temperature measured by an external thermistor
 * These constants are used to translate ADC value into milli-Celsius Degrees
 */
#define THERM_VREF_MV             1250  /* 1250 mV */
#define THERM_VX_MV               1250  /* 1250 mV */
#define THERM_R0                    47  /* 47 kOhms */
#define THERM_RX                    10  /* 10 kOhms */
#define THERM_RY                   220  /* 220 kOhms */
#define THERM_BETA                4050  /* Extracted from thermistor datasheet */
#define THERM_T0                298150  /* mKelvin (25C + 273.15) */
#define THERM_KELVIN_TO_CELSIUS 273150  /* mKelvin */
#define AVERAGE_NUMBER              20
/*
 *
 * External functions
 *
 */
int cpu_thermal_governor(int omap_temp);
void init_cpu_thermal_governor(int omap_temp);
int average_on_die_temperature(void);
#endif
