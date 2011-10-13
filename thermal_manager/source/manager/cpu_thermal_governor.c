/*
 * OMAP CPU THERMAL GOVERNOR
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

#include "cpu_thermal_governor.h"
#include <sys/reboot.h>
#include <utils/Log.h>
/* TODO: Need to make this better */
#include "../../include/thermal_manager.h"

/*
 *
 * Global Variables (CPU Freq parameters + regular thermal manager)
 *
 */
static u32 available_freq[OPPS_NUMBER];
static bool is_panic_zone_reached = false;
static u32 nominal_cpu_scaling_max_freq;
static char *available_governors[GOVS_NUMBER];
static char *nominal_cpu_scaling_governor;
static bool is_conservative_available = false;
static u32 update_rate = 0;
static u32 current_scaling_max_freq = 0;
static int current_t_high = 0; /* temperature threshold (high) at OMAP hot spot level */
static int current_t_low = 0;  /* temperature threshold (low) at OMAP hot spot level */
static int cpu_temp = 0; /* Temperature at OMAP hot spot level */
static int absolute_delta = 0;
/*
 * Global variables used to extrapolate OMAP hot spot temperature from
 * average on-die sensor and PCB temperature
 */
static int pcb_temp = 0;
static int average_cpu_sensor_temp = 0; /* Average temperature at OMAP sensor level */
static int average_period = NORMAL_TEMP_MONITORING_RATE * 1000; /* 1 second */
static int cpu_sensor_temp_table[AVERAGE_NUMBER];

#define DEBUG 1

/*
 *
 * Internal functions
 *
 */

/*
 * Convert the result of ADC conversion into temperature
 * for a thermistor connected to TWL6030's GPADC
 */
static inline int convert_adc_to_temp(u32 adc_value)
{
    int temp;
    float tmp1;
    float tmp2;
    /* Main formula to be used:
     (Beta*T0)/(Beta-T0*LN(-R0*(ADC_VALUE*(Rx+Ry)*Vref-1023*Ry*Vx)/(ADC_VALUE*Rx*Ry*Vref)))-273.15
     Following formula has been simplified assuming that Vref = Vx.
    */
    tmp1 = THERM_R0 * (1023 * THERM_RY -
                       (float)adc_value * (THERM_RX + THERM_RY)) /
                       ((float)adc_value * THERM_RX * THERM_RY);
    tmp2 = ((THERM_BETA * THERM_T0) / (1000 * THERM_BETA - THERM_T0 * log(tmp1)));
    temp = (tmp2 * 1000) - THERM_KELVIN_TO_CELSIUS;

    return temp;
}

/*
 * Convert the temperature from the OMAP on-die temp sensor into
 * OMAP hot spot temperature.
 * This takes care of the existing temperature gradient between
 * the OMAP hot spot and the on-die temp sensor.
 * Note: The "slope" parameter is multiplied by 1000 in the configuration
 * file to avoid using floating values.
 * Note: The "offset" is defined in milli-celsius degrees.
 */
static inline int convert_omap_sensor_temp_to_hotspot_temp(u32 type, int temp)
{
    int slope; /* multiplied by 1000 */
    int constant; /* milli-celsius degrees */
    u32 adc_value = 0;
    u32 i = 0;

    if (type == OMAP_CPU) {
        slope = config_file.omap_cpu_temperature_slope;
        constant = config_file.omap_cpu_temperature_offset;
        if (config_file.pcb_temp_sensor_used == true) {
            if (config_file.omap_pcb_temp_sensor_id) {
                if (strcmp(config_file.pcb_temp_sensor_type,"thermistor") == 0) {
                    pcb_temp = 0;
                    i = 0;
                    /* Ensure that pcb_temp value is in acceptable range */
                    /* Restart the measurement if needed */
                    /* Potential problem with collision on TWL6030 GPADC channel conversion */
                    do {
                        adc_value = atoi(read_from_file(config_file.temperature_file_sensors[PCB_FILE]));
		        pcb_temp = convert_adc_to_temp(adc_value);
                        i++;
                    } while ((pcb_temp < -40000) && (pcb_temp > 125000) && (i < 10));
                } else if (strcmp(config_file.pcb_temp_sensor_type,"temp_sensor") == 0) {
                    pcb_temp = atoi(read_from_file(config_file.temperature_file_sensors[PCB_FILE]));
                } else {
                    LOGD("Thermal Manager:Type of PCB temperature sensor is not supported \"%s\"\n",
                         config_file.pcb_temp_sensor_type);
                    return 0;
                }
                absolute_delta = (((average_cpu_sensor_temp - pcb_temp) * slope / 1000) + constant);
                /* Ensure that this formula never returns negative value due to PCB temp > On-die sensor */
                if (absolute_delta < 0) absolute_delta = 0;
            } else {
                LOGD("Thermal Manager:PCB Temp ID not found in config\n");
                return 0;
            }
        } else { /* pcb_temp_sensor_used == false or not defined */
            absolute_delta = ((temp * slope / 1000) + constant);
        }
        return (temp + absolute_delta);
    } else {
        return 0;
    }
}

/*
 * Convert the temperature from the OMAP hot spot temperature into
 * the OMAP on-die temp sensor. This is useful to configure the thresholds
 * at OMAP on-die sensor level.
 * This takes care of the existing temperature gradient between
 * the OMAP hot spot and the on-die temp sensor.
 * Note: the "slope" parameter is multiplied by 1000 in the configuration
 * file to avoid using floating values.
 * Note: the "offset" is defined in milli-celsius degrees.
 */
static inline int convert_hotspot_temp_to_omap_sensor_temp(u32 type, int temp)
{
    int slope; /* multiplied by 1000 */
    int constant; /* milli-celsius degrees */
    if (type == OMAP_CPU) {
        if (config_file.pcb_temp_sensor_used == true) {
            return (temp - absolute_delta);
        } else { /* pcb_temp_sensor_used == false or not defined */
            slope = config_file.omap_cpu_temperature_slope;
            constant = config_file.omap_cpu_temperature_offset;
            return (((temp - constant) * 1000) / (1000 + slope));
        }
    } else {
        return 0;
    }
}

/* Update the SYSFS parameters */
static void update_cpu_scaling_governor(char *buf)
{
    write_to_file(config_file.cpufreq_file_paths[SCALING_GOVERNOR_PATH], buf);
}

static void update_cpu_scaling_set_speed(u32 value)
{
    char buf[SIZE];
    sprintf(buf, "%ld\n", value);
    write_to_file(config_file.cpufreq_file_paths[SCALING_SET_SPEED_PATH], buf);
}

static void update_cpu_scaling_max_freq(u32 value)
{
    char buf[SIZE];

    if (current_scaling_max_freq != value) {
        sprintf(buf, "%ld\n", value);
        write_to_file(config_file.cpufreq_file_paths[SCALING_MAX_FREQ_PATH], buf);
        current_scaling_max_freq = value;
    }
}

/*
 * The goal of this function is to update the setting of monitoring
 * rate only if this is required to avoid useless SYSFS accesses.
 */
static void update_omap_rate(u32 rate)
{
    char buf[SIZE];

    if (update_rate != rate) {
        sprintf(buf, "%ld\n", rate);
        write_to_file(
            config_file.omaptemp_file_paths[OMAP_CPU_UPDATE_RATE_PATH],
            buf);
        update_rate = rate;
    }
}

/*
 * The goal of these functions is to update the settings of thermal thresholds
 * only if this is required to avoid useless SYSFS accesses.
 */
static void update_t_high(int t_high)
{
    char buf[SIZE];
#ifdef DEBUG
    if (current_t_high != t_high) {
        LOGD("update_t_high (%d)\n", t_high);
    }
#endif
    sprintf(buf, "%d\n", convert_hotspot_temp_to_omap_sensor_temp(OMAP_CPU, t_high));
    write_to_file(
        config_file.omaptemp_file_paths[OMAP_CPU_THRESHOLD_HIGH_PATH],
        buf);
}

static void update_t_low(int t_low)
{
    char buf[SIZE];
#ifdef DEBUG
    if (current_t_low != t_low) {
        LOGD("update_t_low (%d)\n", t_low);
    }
#endif
    sprintf(buf, "%d\n", convert_hotspot_temp_to_omap_sensor_temp(OMAP_CPU, t_low));
    write_to_file(
        config_file.omaptemp_file_paths[OMAP_CPU_THRESHOLD_LOW_PATH],
        buf);
}

/*
 * This function should ensure that high threshold is greater than low threshold
 * The sequence to change these thresholds will depend on current and next values
 * If the PCB temperature is used, always reconfigure the thresholds because
 * PCB temperature and absolute delta always change
 * current_t_high is the current thermal threshold (high)
 * next_t_high is the next thermal threshold (high)
 * theshold_low is the current thermal threshold (low)
 * t_low is the next thermal threshold (low)
 */
static void update_thresholds(int next_t_high, int next_t_low)
{
    bool t_high_done = false;
    if (config_file.pcb_temp_sensor_used == true) {
        if (next_t_high > current_t_low) {
            update_t_high(next_t_high);
            current_t_high = next_t_high;
            t_high_done = true;
        }

        if (next_t_low < current_t_high) {
            update_t_low(next_t_low);
            current_t_low = next_t_low;
        }

        if (t_high_done == false) {
            update_t_high(next_t_high);
            current_t_high = next_t_high;
        }
    } else { /* pcb_temp_sensor_used == false or not defined */
        if (current_t_high != next_t_high) {
            if (next_t_high > current_t_low) {
                update_t_high(next_t_high);
                current_t_high = next_t_high;
            }
        }

        if (current_t_low != next_t_low) {
            if (next_t_low < current_t_high) {
                update_t_low(next_t_low);
                current_t_low = next_t_low;
            }
        }

        if (current_t_high != next_t_high) {
            if (next_t_high > current_t_low) {
                update_t_high(next_t_high);
                current_t_high = next_t_high;
            }
        }
    } /* end of if (config_file.pcb_temp_sensor_used == true) */
}

/* Read some SYSFS parameters */
static void read_cpu_scaling_available_freq(void)
{
    char *pch;
    char *buffer;
    int i;
    /* Initialize the available_freq array */
    for (i = 0; i < OPPS_NUMBER; i++) {
        available_freq[i] = 0;
    }

    /* Update the available_freq array from the SYSFS entry */
    if ((buffer = read_from_file(
        config_file.cpufreq_file_paths[AVAILABLE_FREQS_PATH])) != NULL) {
        pch = strtok (buffer, " ");
        i = 0;
        while (pch != NULL)
        {
            available_freq[i] = atoi(pch);
            i++;
            pch = strtok (NULL, " ");
        }
    }
}

static void read_cpu_scaling_available_governors(void)
{
    char *buffer;
    char *pch;
    int i;

    /* Initialize the available_governors array */
    for (i = 0; i < GOVS_NUMBER; i++) {
        available_governors[i] = (char*)"unavailable";
    }

    /* Update the available_governors array from the SYSFS entry */
    if ((buffer = read_from_file(
        config_file.cpufreq_file_paths[AVAILABLE_GOVS_PATH])) != NULL) {
        pch = strtok (buffer, " ");
        i = 0;
        while (pch != NULL)
        {
            available_governors[i] = pch;
            i++;
            pch = strtok (NULL, " ");
        }
    }
}

static void read_cpu_scaling_governor(void)
{
    nominal_cpu_scaling_governor =
        read_from_file(config_file.cpufreq_file_paths[SCALING_GOVERNOR_PATH]);
#ifdef DEBUG
    LOGD("nominal_cpu_scaling_governor is %s\n",
        nominal_cpu_scaling_governor);
    fflush(stdout);
#endif
}

/*
 * For debug purpose, display all SYSFS settings done by the thermal governor.
 */
static void print_latest_settings(void)
{
#ifdef DEBUG
    /* Print the current scaling_max_freq and nominal value */
    LOGD("scaling_max_freq = %d\n",
        atoi(read_from_file(
            config_file.cpufreq_file_paths[SCALING_MAX_FREQ_PATH])));
    fflush(stdout);
    LOGD("nominal scaling_max_freq = %ld\n",
        nominal_cpu_scaling_max_freq);
    fflush(stdout);

    /* Print the current scaling_governor */
    LOGD("scaling_governor = %s\n",
        read_from_file(
            config_file.cpufreq_file_paths[SCALING_GOVERNOR_PATH]));
    fflush(stdout);

    /* Print the current cpuinfo_cur_freq */
    LOGD("cpuinfo_cur_freq = %d\n",
        atoi(read_from_file(
            config_file.cpufreq_file_paths[CPUINFO_CUR_FREQ_PATH])));
    fflush(stdout);

    /* Print the current omap_update_rate */
    LOGD("omap_update_rate = %d\n",
        atoi(read_from_file(
            config_file.omaptemp_file_paths[OMAP_CPU_UPDATE_RATE_PATH])));
    fflush(stdout);

    /* Print the current Threshold High */
    LOGD("omap_threshold_high = %d\n",
        atoi(read_from_file(
            config_file.omaptemp_file_paths[OMAP_CPU_THRESHOLD_HIGH_PATH])));
    fflush(stdout);

    /* Print the current Threshold High */
    LOGD("omap_threshold_low = %d\n",
        atoi(read_from_file(
            config_file.omaptemp_file_paths[OMAP_CPU_THRESHOLD_LOW_PATH])));
    fflush(stdout);

    /* Has the panic zone been reached? */
    LOGD("is_panic_zone_reached = %d\n", is_panic_zone_reached);
    fflush(stdout);
#endif
}

/*
 * THERMAL "Safe Zone" definition:
 *  - No constraint about Max CPU frequency (See scaling_max_freq)
 *  - No constraint about CPU freq governor (See scaling_governor)
 *  - Normal temperature monitoring rate (See update_rate)
 *  - Thresholds configuration:
 *     . High = TEMP_MONITORING_HIGH
 *     . Low = TEMP_MONITORING_LOW
 */
static void safe_zone(void)
{
    LOGD("OMAP CPU THERMAL - Safe Zone (hot spot temp: %i)\n", cpu_temp);
    fflush(stdout);

    update_cpu_scaling_max_freq(nominal_cpu_scaling_max_freq);

    update_cpu_scaling_governor(nominal_cpu_scaling_governor);

    update_omap_rate(NORMAL_TEMP_MONITORING_RATE);

    if (config_file.pcb_temp_sensor_used == true) {
        average_period = NORMAL_TEMP_MONITORING_RATE * 1000;
    }

    update_thresholds(config_file.omap_cpu_threshold_monitoring,
            config_file.omap_cpu_threshold_monitoring - HYSTERESIS_VALUE);

    is_panic_zone_reached = false;

    print_latest_settings();
}

/*
 * THERMAL "Monitoring Zone" definition:
 *  - No constraint about Max CPU frequency (See scaling_max_freq)
 *  - Select "conservative" (if available) CPU freq governor to avoid selecting higher frequency first
 *  - Increase temperature monitoring rate (See update_rate)
 *  - Thresholds configuration:
 *     . High: Max = TEMP_ALERT_HIGH
 *     . Low: Min = TEMP_MONITORING_LOW
 */
static void monitoring_zone(void)
{
    LOGD("OMAP CPU THERMAL - Monitoring Zone (hot spot temp: %i)\n", cpu_temp);
    fflush(stdout);

    update_cpu_scaling_max_freq(nominal_cpu_scaling_max_freq);

    if (is_conservative_available == true) {
        update_cpu_scaling_governor("conservative");
#ifdef DEBUG
        LOGD("conservative governor is available\n");
        fflush(stdout);
#endif
    } else {
#ifdef DEBUG
        LOGD("conservative governor is not available\n");
        fflush(stdout);
#endif
    }

    update_omap_rate(FAST_TEMP_MONITORING_RATE);

    if (config_file.pcb_temp_sensor_used == true) {
        average_period = FAST_TEMP_MONITORING_RATE * 1000;
    }

    update_thresholds(config_file.omap_cpu_threshold_alert,
            config_file.omap_cpu_threshold_monitoring - HYSTERESIS_VALUE);

    print_latest_settings();
}

/*
 * THERMAL "Alert Zone" definition:
 *  - If the Panic Zone has never been reached, then
 *     - Define constraint about Max CPU frequency (See scaling_max_freq)
 *       if Current frequency < Max frequency, then select lower value for Max frequency
 *  - Else keep the constraints set previously until temperature falls to Monitoring zone
 *  - Select "conservative" (if available) CPU freq governor to avoid selecting higher frequency first
 *  - Increase temperature monitoring rate (See update_rate)
 *  - Thresholds configuration:
 *     . High: Max = TEMP_PANIC_HIGH
 *     . Low: Min = TEMP_ALERT_LOW
 */
static void alert_zone(void)
{
    u32 current_freq;
    int i;

    LOGD("OMAP CPU THERMAL - Alert Zone (hot spot temp: %i)\n", cpu_temp);
    fflush(stdout);

    if (is_panic_zone_reached == false) {
        /* temperature rises and enters into ALERT zone */
        current_freq = atoi(read_from_file(
            config_file.cpufreq_file_paths[CPUINFO_CUR_FREQ_PATH]));
        if (current_freq < current_scaling_max_freq) {
#ifdef DEBUG
            LOGD("ALERT Zone: current_freq < current_scaling_max_freq\n");
#endif
            /* Identify the index of current_scaling_max_freq in the available_freq table */
            for (i = 0; i < OPPS_NUMBER; i++) {
                if (current_scaling_max_freq == available_freq[i]) break;
            }
            /* Select the next lowest one from available_freq table */
            update_cpu_scaling_max_freq(available_freq[i-1]);
        }
    } else {
        /* temperature falls from PANIC zone and enters into ALERT zone */
        /* Do nothing here as we should wait until temperature falls again */
    }
    if (is_conservative_available == true) {
        update_cpu_scaling_governor("conservative");
#ifdef DEBUG
        LOGD("conservative governor is available\n");
        fflush(stdout);
#endif
    } else {
#ifdef DEBUG
        LOGD("conservative governor is not available\n");
        fflush(stdout);
#endif
    }

    update_omap_rate(FAST_TEMP_MONITORING_RATE);

    if (config_file.pcb_temp_sensor_used == true) {
        average_period = FAST_TEMP_MONITORING_RATE * 1000;
    }

    update_thresholds(config_file.omap_cpu_threshold_panic,
            config_file.omap_cpu_threshold_alert - HYSTERESIS_VALUE);

    print_latest_settings();
}

/*
 * THERMAL "Panic Zone" definition:
 *  - Force CPU frequency to a "safe frequency" (See scaling_setspeed and scaling_max_freq)
 *     . Select userspace CPUFreq governor to force the frequency/OPP update (See scaling_governor)
 *     . Force the CPU frequency to a “safe” frequency (See scaling_setspeed)
 *     . Limit max CPU frequency to the “safe” frequency (See scaling_max_freq)
 *     . Select "conservative" (if available) CPU freq governor
 *  - Increase temperature monitoring rate (See update_rate)
 *  - Thresholds configuration:
 *     . High: Max = TEMP_FATAL_HIGH
 *     . Low: Min = TEMP_PANIC_LOW
 */
static void panic_zone(void)
{
    char *governor;
    int i;
    u32 panic_zone_cpu_freq;
    u32 current_freq;
    int threshold_fatal;

    LOGD("OMAP CPU THERMAL - Panic Zone (hot spot temp: %i)\n", cpu_temp);
    fflush(stdout);

    /* Read current frequency */
    current_freq = atoi(read_from_file(
        config_file.cpufreq_file_paths[CPUINFO_CUR_FREQ_PATH]));

    /* Identify the index of current frequency in the available_freq table */
    for (i = 0; i < OPPS_NUMBER; i++) {
        if (current_freq == available_freq[i])
            break;
    }

    /* Select the next lowest one from available_freq table */
    if (i > 0) panic_zone_cpu_freq = available_freq[i-1];
    else panic_zone_cpu_freq = available_freq[0];

    if (is_conservative_available == true) {
        if ((governor = calloc(16,sizeof(char))) == NULL) {
            LOGD("Error in allocating memory\n");
            return;
        }
        strcpy(governor, "conservative");
#ifdef DEBUG
        LOGD("conservative governor is available\n");
        fflush(stdout);
#endif
    } else {

        if ((governor = calloc(strlen(read_from_file(config_file.cpufreq_file_paths[SCALING_GOVERNOR_PATH])),
                           sizeof(char))) == NULL) {
            LOGD("Error in allocating memory\n");
            return;
        }
        strcpy(governor, read_from_file(config_file.cpufreq_file_paths[SCALING_GOVERNOR_PATH]));

#ifdef DEBUG
        LOGD("initial governor is %s\n", governor);
        fflush(stdout);
#endif
    }
    /*
     * Force the new frequency through userspace governor
     * and come back to the original cpufreq governor
     */
    update_cpu_scaling_governor("userspace");
    update_cpu_scaling_set_speed(panic_zone_cpu_freq);
    update_cpu_scaling_max_freq(panic_zone_cpu_freq);
    update_cpu_scaling_governor(governor);

    update_omap_rate(FAST_TEMP_MONITORING_RATE);

    /*
     * Safety mechanism in panic zone to avoid reaching fatal zone:
     * Define a threshold between TPanic and TFatal to detect if the
     * temperature still rises after forcing lower frequency
     * Select lowest OPP when entering into Pre-Fatal Zone
     */
    threshold_fatal = ((config_file.omap_cpu_threshold_panic +
            OMAP_CPU_THRESHOLD_FATAL) / 2);

    if (cpu_temp >= threshold_fatal) {
        threshold_fatal = OMAP_CPU_THRESHOLD_FATAL;
        update_cpu_scaling_governor("userspace");
        update_cpu_scaling_set_speed(available_freq[0]);
        update_cpu_scaling_max_freq(available_freq[0]);
        update_cpu_scaling_governor(governor);
#ifdef DEBUG
        LOGD("OMAP CPU THERMAL - Pre-Fatal Zone (hot spot temp: %i)\n", cpu_temp);
        fflush(stdout);
#endif
    }

    if (config_file.pcb_temp_sensor_used == true) {
        average_period = FAST_TEMP_MONITORING_RATE * 1000;
    }

    update_thresholds(threshold_fatal,
            config_file.omap_cpu_threshold_panic - HYSTERESIS_VALUE);

    is_panic_zone_reached = true;

    print_latest_settings();

    free(governor);
}

/*
 * THERMAL "Fatal Zone" definition:
 *  - Shut-down the system to ensure OMAP Junction temperature decreases enough
 *  Note: on OMAP5, this thermal zone should be managed by the HW protection
 */
static void fatal_zone(void)
{
    LOGD("OMAP CPU THERMAL - FATAL ZONE (hot spot temp: %i)\n", cpu_temp);
    fflush(stdout);

    is_panic_zone_reached = true;

    sync();
    reboot(RB_POWER_OFF);
}

/*
 *
 * External functions
 *
 */

/*
 * Main CPU Thermal Governor:
 * It defines various thermal zones (safe, monitoring, alert, panic and fatal)
 * with associated thermal Thresholds including the hysteresis effect.
 */
int cpu_thermal_governor(int sensor_temp)
{
    cpu_temp = convert_omap_sensor_temp_to_hotspot_temp(OMAP_CPU, sensor_temp);

    if (cpu_temp >= OMAP_CPU_THRESHOLD_FATAL) {
        fatal_zone();
        return FATAL_ZONE;
    } else if (cpu_temp >= config_file.omap_cpu_threshold_panic) {
        panic_zone();
        return PANIC_ZONE;
    } else if (cpu_temp < (config_file.omap_cpu_threshold_panic - HYSTERESIS_VALUE)) {
        if (cpu_temp >= config_file.omap_cpu_threshold_alert) {
            alert_zone();
            return ALERT_ZONE;
        } else if (cpu_temp < (config_file.omap_cpu_threshold_alert - HYSTERESIS_VALUE)) {
            if (cpu_temp >= config_file.omap_cpu_threshold_monitoring) {
                monitoring_zone();
                return MONITOR_ZONE;
            } else {
                /*
                 * this includes the case where :
                 * MONITORING_LOW <= T < MONITORING_HIGH
                 */
                safe_zone();
                return SAFE_ZONE;
            }
        } else {
               /*
                * this includes the case where :
                * ALERT_LOW <= T < ALERT_HIGH
                */
               monitoring_zone();
               return MONITOR_ZONE;
        }
    } else {
           /*
            * this includes the case where :
            * PANIC_LOW <= T < PANIC_HIGH
            */
           alert_zone();
           return ALERT_ZONE;
    }
}

/*
 * Initialize some internal parameters.
 */
void init_cpu_thermal_governor(int sensor_temp)
{
    int i;

    /* Initialize the nominal_cpu_scaling_max_freq variable */
    current_scaling_max_freq = atoi(read_from_file(
        config_file.cpufreq_file_paths[SCALING_MAX_FREQ_PATH]));

    /* Initialize the nominal_cpu_scaling_governor variable */
    read_cpu_scaling_governor();

    /* Initialize the available_freq[] int array */
    read_cpu_scaling_available_freq();

    /* Initialize the nominal maximum CPU frequency from the available_freq array */
    nominal_cpu_scaling_max_freq = available_freq[0];
    for (i = 1; i < OPPS_NUMBER; i++) {
        if (available_freq[i] > nominal_cpu_scaling_max_freq)
            nominal_cpu_scaling_max_freq = available_freq[i];
    }
#ifdef DEBUG
    LOGD("nominal/current_scaling_max_freq is %ld / %ld\n",
          nominal_cpu_scaling_max_freq, current_scaling_max_freq);
#endif

    /* Initialize the available_governors[] string array */
    read_cpu_scaling_available_governors();

    /* Check if "conservative" governor is available */
    for (i = 0; i < GOVS_NUMBER; i++) {
        if (strcmp(available_governors[i],("conservative")) == 0) {
            is_conservative_available = true;
        }
    }
#ifdef DEBUG
    if (is_conservative_available == true) {
        LOGD("conservative governor is available\n");
        fflush(stdout);
    } else {
        LOGD("conservative governor is not available\n");
        fflush(stdout);
    }
#endif

    if (config_file.pcb_temp_sensor_used == true) {
        /* initialize the temperature table with current temperature */
        for (i = 0; i < AVERAGE_NUMBER; i++)
            cpu_sensor_temp_table[i] = sensor_temp;
        average_cpu_sensor_temp = sensor_temp;
    }

    /*
     * Force to initialize all temperature thresholds according to
     * the current thermal zone */
    cpu_thermal_governor(sensor_temp);

#ifdef DEBUG
    for (i = 0; i < OPPS_NUMBER; i++) {
        LOGD("available_freq[%d] = %ld\n",
                i,
                available_freq[i]);
        fflush(stdout);
    }
#endif
}

/*
 * Make an average of the OMAP on-die temperature
 * this is helpful to handle burst activity of OMAP when extrapolating
 * the OMAP hot spot temperature from on-die sensor and PCB temperature
 * Re-evaluate the temperature gradient between hot spot and on-die sensor
 * (See absolute_delta) and reconfigure the thresholds if needed
 */
int average_on_die_temperature(void)
{
    int sensor_temp = 0;
    u32 i;

    if (config_file.pcb_temp_sensor_used == true) {
        /* Read current temperature */
        sensor_temp = atoi(read_from_file(config_file.temperature_file_sensors[OMAP_CPU_FILE]));
        /* if on-die sensor does not report a correct temperature value, keep the previous measurement */
        if ((abs(sensor_temp - cpu_sensor_temp_table[0]) > 40000) || /* if difference between 2 measurements is > 40 C */
            (sensor_temp > OMAP_CPU_MAX_TEMP) ||
            (sensor_temp < OMAP_CPU_MIN_TEMP)) {
            LOGD("Invalid value from on-die sensor %d, use %d instead",
                  sensor_temp, cpu_sensor_temp_table[0]);
            sensor_temp = cpu_sensor_temp_table[0];
        }

        /* Update historical buffer */
        for (i = 1; i < AVERAGE_NUMBER; i++) {
            cpu_sensor_temp_table[AVERAGE_NUMBER - i] =
            cpu_sensor_temp_table[AVERAGE_NUMBER - i - 1];
        }
        cpu_sensor_temp_table[0] = sensor_temp;

        /* Compute the new average value */
        average_cpu_sensor_temp = 0;
        for (i = 0; i < AVERAGE_NUMBER; i++) {
            average_cpu_sensor_temp += cpu_sensor_temp_table[i];
        }
        average_cpu_sensor_temp = (average_cpu_sensor_temp / AVERAGE_NUMBER);

        /* Reconfigure the current temperature threshold according to the current PCB temperature */
        cpu_temp = convert_omap_sensor_temp_to_hotspot_temp(OMAP_CPU, sensor_temp);
        update_thresholds(current_t_high, current_t_low);

        return average_period;
    } else {
        return 10000000; /* wait for 10 seconds */
    }
}
