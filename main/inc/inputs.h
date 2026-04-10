#pragma once

#include "freertos/semphr.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/temperature_sensor.h"
#define ADC_UNIT ADC_UNIT_1                        ///< ADC unit to use (ADC unit 1)
#define ADC_CHANNEL_START ADC_CHANNEL_0            ///< First ADC channel
#define ADC_CHANNEL_END ADC_CHANNEL_9              ///< Last ADC channel
#define NUM_ADC_CHANNELS (ADC_CHANNEL_END - ADC_CHANNEL_START + 1) ///< Total number of ADC channels (10)
#define NTC_TABLE_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/** @brief Two ADS7830 devices provide 16 analog inputs (logical indices 0..15). */
#define NUM_ANALOG_INPUTS 16

/**
 * @brief ADC channels used internally on ESP32 for rail monitoring.
 * GPIO3  = ADC1_CH2 -> 5V regulated monitoring (V5 reference)
 * GPIO9  = ADC1_CH8 -> External voltage monitoring
 * GPIO38 = digital USB presence (not an ADC channel)
 */
#define V5_REF_ADC_CHANNEL ADC_CHANNEL_2
#define EXT_VOLT_ADC_CHANNEL ADC_CHANNEL_8
/** @brief Per-board gain correction from measured midpoint (DMM) vs ADC-converted midpoint. */
#define V5_REF_CORRECTION_FACTOR 1.0264f
/** @brief ADS7830 conversion full-scale in mV (internal reference mode). */
#define ADS7830_REF_MV 2500.0f

/** @brief Median filter sample depths for selectable levels. */
#define FILTER_DEPTH_LOW 5                         ///< Samples for "low" filter level
#define FILTER_DEPTH_MED 10                        ///< Samples for "medium" filter level (default)
#define FILTER_DEPTH_HIGH 15                       ///< Samples for "high" filter level
#define FILTER_DEPTH_MAX FILTER_DEPTH_HIGH        ///< Maximum buffer size required for filtering

/**
 * @brief Divider constants used by scaling helpers.
 * Top resistor (series to input) and bottom resistor (pull to ground).
 * Per hardware: top = 5.1k, bottom = 10k for all 16 ADC input pull-ups.
 */
#define DIVIDER_HIGH_OHM 5100                      ///< Series resistor value (5.1k ohm)
#define DIVIDER_LOW_OHM 10000                      ///< Pull-to-ground resistor value (10k ohm)
#define DIVIDER_TOTAL_OHM (DIVIDER_HIGH_OHM + DIVIDER_LOW_OHM) ///< Total divider impedance (15.1k ohm)

/** @brief Log tag for ADC module. */
static const char *adc_log = "adc";

/** @brief NTC thermistor lookup table point. */
typedef struct {
    int16_t temp_c;      ///< Temperature in °C
    int32_t resistance;  ///< Resistance in ohms
} ntc_point_t;

/** @brief NTC lookup table structure with metadata. */
typedef struct {
    const char *name;           ///< Human-readable table name (e.g., "Bosch NTC 0280130026")
    const char *description;    ///< Part number/additional info
    const ntc_point_t *points;  ///< Array of temperature/resistance calibration points
    size_t points_count;        ///< Number of points in the array
} ntc_table_def_t;

/**
 * @brief Initialize internal CPU temperature sensor.
 * @return ESP_OK on success, ESP_ERR_* on failure.
 */
esp_err_t initCpuTempSensor(void);

/**
 * @brief Read internal CPU temperature.
 * @return Temperature in degC.
 */
int8_t getCpuTemperature(void);

/** @brief Initialize all ADC channels for measurement. */
void initAdcChannels(void);

/** @brief Mutex protecting access to filtered_voltages array. */
extern SemaphoreHandle_t filtered_voltages_mutex;
/** @brief Shared voltage buffer updated by ADC task and read by CAN TX task. */
extern volatile uint16_t filtered_voltages[NUM_ANALOG_INPUTS];

/** @brief Mutex protecting access to scaled_pressures array. */
extern SemaphoreHandle_t scaled_pressures_mutex;
/** @brief Shared pressure buffer for pressure-only fast path. */
extern volatile uint16_t scaled_pressures[4];

/**
 * @brief Read raw/converted value for logical analog channel index.
 * @param index Logical analog input index (0..NUM_ANALOG_INPUTS-1).
 * @param out_mv Pointer to output voltage in millivolts.
 * @return true on success; false on read error or invalid index.
 */
bool read_analog_raw(int index, uint16_t *out_mv);

/** @brief Read current pullup reference rail voltage in millivolts. */
uint16_t get_v5_rail_mv(void);
/** @brief Read USB rail monitor voltage in millivolts. */
uint16_t get_usb_voltage_mv(void);
/** @brief Read external voltage monitor value in millivolts. */
uint16_t get_external_voltage_mv(void);

/**
 * @brief Convert NTC voltage reading to temperature using lookup-table interpolation.
 * @param v_mv Measured NTC node voltage in millivolts.
 * @param r_pullup Pullup resistance in ohms.
 * @param v_ref_mv Pullup rail reference in millivolts.
 * @param table NTC lookup table points array.
 * @param table_size Number of points in lookup table.
 * @return Temperature in degC, or -128 on error.
 */
int16_t getSensorTemperature(int v_mv, int r_pullup, int v_ref_mv, const ntc_point_t *table, size_t table_size);

/**
 * @brief Convert voltage reading to pressure using linear scaling.
 * @param v_mv Measured voltage in millivolts.
 * @param v_min_mv Minimum voltage corresponding to minimum pressure.
 * @param v_max_mv Maximum voltage corresponding to maximum pressure.
 * @param p_min Minimum pressure in kilopascals.
 * @param p_max Maximum pressure in kilopascals.
 * @return Pressure value clamped to [p_min, p_max], scaled x100.
 */
uint16_t getSensorPressure(int v_mv, int v_min_mv, int v_max_mv, float p_min, float p_max);

/**
 * @brief Read raw ADC value and apply calibration with optional scaling.
 * @param channel ADC channel to read.
 * @param scaled If true, apply scaling_factor.
 * @param scaling_factor Multiplier for voltage conversion.
 * @return Voltage in millivolts.
 */
uint16_t getScaledMillivolts(adc_channel_t channel, bool scaled, float scaling_factor);

/**
 * @brief Calculate median of sample array using bubble sort.
 * @param samples Array of voltage samples.
 * @param count Number of samples in array.
 * @return Median value.
 */
uint16_t medianFilterHelper(uint16_t *samples, int count);

/**
 * @brief FreeRTOS task for ADC acquisition and processing.
 * Runs continuously, applies filtering, and updates filtered_voltages.
 * @param arg Unused FreeRTOS task parameter.
 */
void adcProcess(void *arg);

/** @brief NTC lookup table definitions. */

static const ntc_point_t bosch_ntc_0280130026_points[] = { // Bosch 0280130026, Bosch 0280130039
    { -40, 45313 }, { -30, 26114 }, { -20, 15462 }, { -10,  9397 },
    {   0,  5896 }, {  10,  3792 }, {  20,  2500 }, {  30,  1707 },
    {  40,  1175 }, {  50,   834 }, {  60,   596 }, {  70,   436 },
    {  80,   323 }, {  90,   243 }, { 100,   187 }, { 110,   144 },
    { 120,   113 }, { 130,    89 }, { 140,    71 },
    // Estimated extrapolation beyond published points for high-temperature operation.
    { 150,    56 }, { 160,    45 }, { 170,    36 }, { 180,    29 }
};

static const ntc_point_t bosch_tmap_0281002437_points[] = { // Bosch TMAP 0281002437
    {   0,  4094 }, {  5,  3362 }, { 10,  2854 }, { 15,  2425 }, { 20,  2039 },
    { 25,  1745 }, { 30,  1489 }, { 35,  1291 }, { 40,  1110 }, { 45,   950 },
    { 50,   710 }, { 51,   698 }, { 52,   692 }, { 53,   687 }, { 54,   634 },
    { 55,   691 }, { 56,   707 }, { 57,   724 }, { 58,   687 }, { 59,   528 },
    { 60,   500 }, { 61,   492 }, { 62,   472 }, { 63,   468 }, { 64,   464 },
    { 65,   458 }, { 66,   444 }, { 67,   427 }, { 68,   421 }, { 69,   416 },
    { 70,   411 }, { 71,   398 }, { 72,   385 }, { 73,   375 }, { 74,   366 },
    { 75,   362 }, { 76,   359 }, { 77,   344 }, { 78,   335 }, { 79,   341 },
    { 80,   358 }, { 81,   332 }, { 82,   323 }, { 83,   326 }, { 84,   328 },
    { 85,   321 }, { 90,   284 }, { 95,   252 }, {100,   225 }, {105,   200 },
    {110,   178 }, {115,   159 }, {120,   142 }
};

static const ntc_point_t universal_18_npt_points[] = { // Universal 1/8 NPT
    {  20,   850 }, {  25,   640 }, {  30,   540 }, {  35,   430 },
    {  40,   360 }, {  45,   290 }, {  50,   240 }, {  55,   200 },
    {  60,   170 }, {  65,   150 }, {  70,   120 }, {  75,   110 },
    {  80,    90 }, {  85,    77 }, {  90,    60 }, {  95,    57 },
    { 100,    52 }, { 105,    48 }, { 110,    43 }, { 115,    39 },
    { 120,    34 }, { 125,    30 }, { 130,    27 }, { 135,    24 },
    { 140,    21 }
};

/** @brief Array of available NTC tables for lookup and enumeration. */
static const ntc_table_def_t ntc_tables[] = {
    {
        .name = "Bosch NTC 0280130026",
        .description = "Bosch 0280130026 / 0280130039",
        .points = bosch_ntc_0280130026_points,
        .points_count = NTC_TABLE_SIZE(bosch_ntc_0280130026_points)
    },
    {
        .name = "Bosch TMAP 0281002437",
        .description = "Bosch TMAP 0281002437 (BMW TMAP 13627843531)",
        .points = bosch_tmap_0281002437_points,
        .points_count = NTC_TABLE_SIZE(bosch_tmap_0281002437_points)
    },{
        .name = "Universal 1/8 NPT",
        .description = "Universal 18 NPT (EFI Parts)",
        .points = universal_18_npt_points,
        .points_count = NTC_TABLE_SIZE(universal_18_npt_points)
    }
};

#define NUM_NTC_TABLES (sizeof(ntc_tables) / sizeof(ntc_tables[0]))

/**
 * @brief Get NTC table descriptor by index.
 * @param index Table index.
 * @return Pointer to table descriptor, or NULL when index is out of range.
 */
static inline const ntc_table_def_t* ntc_get_table(size_t index) {
    if (index >= NUM_NTC_TABLES) {
        return NULL;
    }
    return &ntc_tables[index];
}

