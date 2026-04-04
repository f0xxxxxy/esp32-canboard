#include "esp_err.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/temperature_sensor.h"
#include "inc/inputs.h"
#include "inc/config.h"
#include "ads7830.h"

#include <math.h>
#include <stdint.h>

extern board_config_t board_cfg;

static adc_oneshot_unit_handle_t adc_handle = NULL;
static adc_cali_handle_t cali_handles[ADC_CHANNEL_END + 1] = {NULL};

static temperature_sensor_handle_t tempSensor_handle = NULL;
static bool tempSensor_initialized = false;

SemaphoreHandle_t filtered_voltages_mutex;
volatile uint16_t filtered_voltages[NUM_ANALOG_INPUTS];
SemaphoreHandle_t scaled_pressures_mutex;
volatile uint16_t scaled_pressures[4];

static bool read_analog_raw_ex(int index, uint16_t *out_mv, uint8_t *out_raw_code, uint8_t *out_cmd, uint8_t *out_addr)
{
    if (out_mv == NULL) return false;
    if (index < 0 || index >= NUM_ANALOG_INPUTS) {
        *out_mv = 0;
        return false;
    }

    int device_idx = index / 8; // two ADS7830 devices, 8 channels each
    int ch = index % 8;
    uint8_t raw = 0;
    uint8_t cmd = 0;
    uint8_t addr = 0;
    if (!ads7830_read_channel_meta(device_idx, ch, &raw, &cmd, &addr)) {
        *out_mv = 0;
        return false;
    }

    if (out_raw_code) {
        *out_raw_code = raw;
    }
    if (out_cmd) {
        *out_cmd = cmd;
    }
    if (out_addr) {
        *out_addr = addr;
    }

    // ADS7830 conversion is referenced to its supply (3.3V on this board).
    // Each analog input is measured through a fixed divider:
    // Vadc = Vin * (DIVIDER_LOW_OHM / DIVIDER_TOTAL_OHM)
    float vadc_mv = ((float)raw / 255.0f) * ADS7830_REF_MV;
    float vin_mv = vadc_mv * ((float)DIVIDER_TOTAL_OHM / (float)DIVIDER_LOW_OHM);

    // Keep outputs within physically valid range for this ADC path.
    if (vin_mv < 0.0f) vin_mv = 0.0f;
    if (vin_mv > 5000.0f) vin_mv = 5000.0f;

    *out_mv = (uint16_t)(vin_mv + 0.5f);
    return true;
}

/* Read raw/converted value for logical analog channel index (0..NUM_ANALOG_INPUTS-1)
   Returns true on success and fills out_mv with millivolts (0 when read fails) */
bool read_analog_raw(int index, uint16_t *out_mv)
{
    return read_analog_raw_ex(index, out_mv, NULL, NULL, NULL);
}


/**
 * @brief Initializes the CPU temperature sensor.
 *
 * This function installs and enables the temperature sensor if it hasn't been
 * initialized yet. It configures the sensor to operate within a temperature
 * range of 10°C to 50°C. If the initialization is successful, it marks the
 * sensor as initialized to prevent re-initialization.
 *
 * @return
 *      - ESP_OK on successful initialization
 *      - Error code from `temperature_sensor_install` or `temperature_sensor_enable`
 *        if initialization fails
 */
esp_err_t initCpuTempSensor(void){
    if (tempSensor_initialized) return ESP_OK;
    temperature_sensor_config_t config = { .range_min = 10, .range_max = 50 };

    esp_err_t err = temperature_sensor_install(&config, &tempSensor_handle);
    if (err != ESP_OK) return err;

    err = temperature_sensor_enable(tempSensor_handle);
    if (err != ESP_OK) return err;

    tempSensor_initialized = true;
    return ESP_OK;
}

/**
 * @brief Returns the current CPU temperature in degrees Celsius.
 *
 * If the CPU temperature sensor has not been initialized yet, this function
 * initializes it. If the initialization fails, -128 is returned.
 *
 * @return
 *      - The current CPU temperature in degrees Celsius
 *      - -128 if the initialization or readout fails
 */
int8_t getCpuTemperature(void){
    if (!tempSensor_initialized) {
        if(initCpuTempSensor() != ESP_OK) {
            return -128;
        }
    }
    float cpuTemp = -128;
    esp_err_t err = temperature_sensor_get_celsius(tempSensor_handle, &cpuTemp);
    if (err != ESP_OK) return -128;
    return (int8_t)cpuTemp;
}

/**
 * @brief Initializes the ADC channels for the ESP32.
 *
 * This function initializes the ADC unit and configures each channel to use a
 * 12-bit ADC with an attenuation of 12 dB. It then creates a calibration scheme
 * for each channel using the curve fitting method. The calibration handles are
 * stored in the `cali_handles` array.
 *
 * @note If a channel's configuration or calibration fails, the function will
 *       log a warning message and continue to the next channel.
 */
void initAdcChannels(void){
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &adc_handle));

    // Only configure ADC channels used for monitoring (V5 rail, external voltage)
    // USB presence is on GPIO38 (digital) and is not configured as an ADC channel.
    adc_channel_t monitor_chs[] = { V5_REF_ADC_CHANNEL, EXT_VOLT_ADC_CHANNEL };
    for (size_t i = 0; i < sizeof(monitor_chs)/sizeof(monitor_chs[0]); ++i) {
        adc_channel_t ch = monitor_chs[i];
        adc_oneshot_chan_cfg_t chan_cfg = { .bitwidth = ADC_BITWIDTH_DEFAULT, .atten = ADC_ATTEN_DB_12 };
        esp_err_t err = adc_oneshot_config_channel(adc_handle, ch, &chan_cfg);
        if (err != ESP_OK) {
            ESP_LOGW(adc_log, "Failed to Configure ADC Channel: %d (%s)", ch, esp_err_to_name(err));
            continue;
        } else {
            ESP_LOGI(adc_log, "Configured Monitor ADC Channel: %d", ch);
        }

        adc_cali_curve_fitting_config_t cali_cfg = { .unit_id = ADC_UNIT, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
        adc_cali_handle_t cali_handle = NULL;
        esp_err_t ret = adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali_handle);
        if (ret == ESP_OK) {
            cali_handles[ch] = cali_handle;
            ESP_LOGI(adc_log, "Calibration Created for Monitor ADC Channel: %d", ch);
        } else {
            ESP_LOGW(adc_log, "Failed to Create Calibration for Monitor ADC Channel: %d (%s)", ch, esp_err_to_name(ret));
        }
    }

    // Initialize external ADS7830 ADCs on I2C (two devices expected)
    if (!ads7830_init()) {
        ESP_LOGW(adc_log, "ADS7830 initialization/reporting encountered issues");
    }
}

// Read USB presence on GPIO38 (digital). GPIO38 is not ADC-capable on this package.
uint16_t get_usb_voltage_mv(void)
{
    static bool usb_gpio_initialized = false;
    if (!usb_gpio_initialized) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << 38),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        gpio_config(&io_conf);
        usb_gpio_initialized = true;
    }

    int level = gpio_get_level(GPIO_NUM_38);
    if (level) {
        // USB present — return an approximate 5V indication in mV
        return 5000;
    }
    return 0;
}

// Read external voltage (GPIO9) and reconstruct actual voltage using 47k/10k divider
uint16_t get_external_voltage_mv(void)
{
    uint16_t measured = getScaledMillivolts(EXT_VOLT_ADC_CHANNEL, true, 1.0f);
    if (measured == 0) return 0;
    float v = ((float)measured) * (47000.0f + 10000.0f) / 10000.0f; // V = measured*(57000/10000)
    return (uint16_t)(v + 0.5f);
}

/**
 * @brief Calculates pressure from voltage using linear scaling.
 *
 * Converts a measured voltage to pressure using a linear mapping between min/max
 * voltage and min/max pressure values. The voltage is clamped to the valid range
 * before conversion.
 *
 * @param v_mv Measured voltage in millivolts
 * @param v_min_mv Minimum valid voltage in millivolts (corresponds to p_min)
 * @param v_max_mv Maximum valid voltage in millivolts (corresponds to p_max)
 * @param p_min Minimum pressure value (kPa)
 * @param p_max Maximum pressure value (kPa)
 *
 * @return Pressure value × 100 (as uint16_t) for 0.01 kPa resolution
 *         Result is clamped to [0, (p_max - p_min) × 100]
 */
uint16_t getSensorPressure(int v_mv, int v_min_mv, int v_max_mv, float p_min, float p_max)
{
    if (v_mv <= 0) {
        return 0;
    }

    if (v_mv < v_min_mv) v_mv = v_min_mv;
    if (v_mv > v_max_mv) v_mv = v_max_mv;

    float voltage_span = v_max_mv - v_min_mv;
    float pressure_span = p_max - p_min;
    float relative_voltage = v_mv - v_min_mv;

    float pressure = p_min + (relative_voltage / voltage_span) * pressure_span;

    if(pressure < 0.0f) pressure = 0.0f; 
    return (uint16_t)(pressure * 100.0f);
}

/**
 * @brief Calculates temperature from voltage using NTC thermistor lookup table.
 *
 * Converts a measured voltage to temperature using a voltage divider circuit with
 * a known pull-up resistor. Calculates NTC resistance from voltage, then interpolates
 * temperature from the provided lookup table.
 *
 * @param v_mv Measured voltage across NTC in millivolts
 * @param r_pullup Pull-up resistor value in ohms
 * @param v_ref_mv Reference voltage (usually 5000 mV) in millivolts
 * @param table Pointer to NTC lookup table (array of ntc_point_t)
 * @param table_size Number of entries in the NTC table
 *
 * @return Temperature in degrees Celsius (int8_t)
 *         -128 if input parameters are invalid or table lookup fails
 *
 * @note Table must be sorted in descending order of resistance.
 *       Performs linear interpolation between table points.
 *       Input validation: v_mv must be in (0, v_ref_mv), r_pullup > 0, table != NULL
 */
int16_t getSensorTemperature(int v_mv, int r_pullup, int v_ref_mv, const ntc_point_t *table, size_t table_size) {
    if (r_pullup <= 0 || v_ref_mv <= 0 || table == NULL || table_size < 2)
        return (int16_t)-128;

    if (v_mv <= 0)
        return (int16_t)-128;

    // Open-circuit NTC (node at/above Vref) should map to table maximum endpoint.
    if (v_mv >= v_ref_mv)
        return (int16_t)table[table_size - 1].temp_c;

    float v_ntc = v_mv / 1000.0f;
    float v_ref = v_ref_mv / 1000.0f;
    float r_ntc_f = (r_pullup * v_ntc) / (v_ref - v_ntc);
    int32_t r_ntc = (int32_t)(r_ntc_f + 0.5f);

    if (r_ntc >= table[0].resistance) return table[0].temp_c;
    if (r_ntc <= table[table_size - 1].resistance)
        return table[table_size - 1].temp_c;

    for (size_t i = 0; i < table_size - 1; i++) {
        int32_t r1 = table[i].resistance;
        int32_t r2 = table[i + 1].resistance;

        if (r_ntc <= r1 && r_ntc > r2) {
            int16_t t1 = table[i].temp_c;
            int16_t t2 = table[i + 1].temp_c;

            float frac = (float)(r_ntc - r2) / (r1 - r2);
            int16_t temp = (int16_t)(t2 + frac * (t1 - t2));
            return temp;
        }
    }

    return (int16_t)-128;
}

/**
 * @brief Reads and scales raw ADC value from a channel.
 *
 * Reads the raw ADC value from the specified channel, applies calibration,
 * and optionally scales it based on the input voltage divider ratio.
 *
 * @param channel ADC channel to read (must be in range [ADC_CHANNEL_START, ADC_CHANNEL_END])
 * @param scaled If true, scales output to 0-5000 mV range using scaling_factor;
 *               if false, returns raw calibrated voltage in mV
 * @param scaling_factor Scaling multiplier applied to voltage (e.g., 1.47 for voltage divider)
 *                       Only used if scaled == true
 *
 * @return Scaled/raw voltage in millivolts (uint16_t)
 *         0 if channel is out of range, no calibration handle, or ADC read fails
 *
 * @note Calibration must have been created in initAdcChannels() before calling this.
 *       Typical scaling factors: 1.47 (no pullup), 1.68 (2k4 pullup)
 */
uint16_t getScaledMillivolts(adc_channel_t channel, bool scaled, float scaling_factor) {
    if (channel < ADC_CHANNEL_START || channel > ADC_CHANNEL_END) {
        ESP_LOGE(adc_log, "Requested ADC Channel %d Out of Range!", channel);
        return 0;
    }

    if (cali_handles[channel] == NULL) {
        ESP_LOGE(adc_log, "No Calibration Handle for ADC Channel: %d", channel);
        return 0;
    }

    int raw, voltage = 0;
    esp_err_t err = ESP_ERR_TIMEOUT;
    const int max_tries = 3;
    int tries = 0;
    while (tries < max_tries) {
        err = adc_oneshot_read(adc_handle, channel, &raw);
        if (err == ESP_OK) break;
        tries++;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (err != ESP_OK) {
        ESP_LOGE(adc_log, "ADC Read Failed on Channel: %d (%s)", channel, esp_err_to_name(err));
        return 0;
    }

    err = adc_cali_raw_to_voltage(cali_handles[channel], raw, &voltage);
    if (err != ESP_OK) {
        ESP_LOGE(adc_log, "Voltage Conversion Failed for ADC Channel: %d (%s)", channel, esp_err_to_name(err));
        return 0;
    }

    float v_input_mv = (scaled) ? voltage * scaling_factor : (float)voltage;

    return (uint16_t)(v_input_mv);
}

/**
 * @brief Calculates median value from array of samples.
 *
 * Sorts the sample array in ascending order using bubble sort, then returns
 * the middle value (median). Useful for filtering noise from sensor readings.
 *
 * @param samples Pointer to array of uint16_t sample values (will be sorted in-place)
 * @param count Number of samples in the array (must be > 0)
 *
 * @return Median value from the sorted samples
 *         0 if samples pointer is NULL or count <= 0
 *
 * @note Input array is modified (sorted) during execution.  The caller may
 *       supply any sample depth (e.g. 3,5,7) as determined by the configured
 *       filter level.  For odd count: returns middle element.  For even
 *       count: returns lower-middle element (count/2).  Bubble sort O(n²) is
 *       acceptable for the small depths used by this application.
 */
uint16_t medianFilterHelper(uint16_t *samples, int count) {
    if (samples == NULL || count <= 0) {
        return 0;
    }
    
    // Simple bubble sort for median filtering
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (samples[j] < samples[i]) {
                uint16_t tmp = samples[i];
                samples[i] = samples[j];
                samples[j] = tmp;
            }
        }
    }
    return samples[count / 2];
}

/**
 * @brief FreeRTOS task that continuously processes ADC inputs.
 *
 * Runs in a background task, periodically reading all ADC channels and storing
 * filtered voltage values in shared array (protected by mutex). Applies median
 * filtering if enabled per-channel in board configuration. Scaling factor is
 * calculated dynamically based on each channel's pull-up resistor configuration.
 *
 * @param arg Unused (FreeRTOS task parameter)
 *
 * @note This function should be spawned as a FreeRTOS task using xTaskCreatePinnedToCore().
 *       Runs in infinite loop until task is deleted.
 *       Updates global filtered_voltages[] array (protected by filtered_voltages_mutex).
 *       Scaling formula: (pullup_ohms + 14.7k) / 14.7k when pullup present
 *       Scaling formula: 14.7k / 10k = 1.47 when no pullup
 *       Loop timing: ~10 ms between complete channel cycles
 *
 * @see filtered_voltages, filtered_voltages_mutex, board_cfg.channels[].filtering
 */
void adcProcess(void *arg) {
    ESP_LOGI(adc_log, "ADC Processing Task Started");
    // allocate buffer using maximum possible depth
    uint16_t samples[FILTER_DEPTH_MAX];
    
    while (1) {
        for (int ch = 0; ch < NUM_ANALOG_INPUTS; ch++) {
            int depth = FILTER_DEPTH_MED;
            if (board_cfg.channels[ch].filtering == FILTER_LOW) depth = FILTER_DEPTH_LOW;
            if (board_cfg.channels[ch].filtering == FILTER_HIGH) depth = FILTER_DEPTH_HIGH;

            bool sample_valid = true;
            for (int i = 0; i < depth; ++i) {
                if (!read_analog_raw(ch, &samples[i]) || samples[i] == 0) {
                    sample_valid = false;
                }
                vTaskDelay(pdMS_TO_TICKS(2));
            }

            if (sample_valid) {
                uint16_t filtered = medianFilterHelper(samples, depth);
                if (xSemaphoreTake(filtered_voltages_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    filtered_voltages[ch] = filtered;
                    xSemaphoreGive(filtered_voltages_mutex);
                }
            } else {
                if (xSemaphoreTake(filtered_voltages_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    filtered_voltages[ch] = 0;
                    xSemaphoreGive(filtered_voltages_mutex);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(NULL);
}

void pressureProcess(void *arg) {
    ESP_LOGI(adc_log, "Pressure Sensor Processing Task Started");
    while (1) {
        if (xSemaphoreTake(scaled_pressures_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            scaled_pressures[0] = (filtered_voltages[0] > 0) ? getSensorPressure(filtered_voltages[0], 500, 4500 , 50, 300) : 0;
            scaled_pressures[1] = (filtered_voltages[1] > 0) ? getSensorPressure(filtered_voltages[1], 500, 4500, 0, 100) : 0;
            scaled_pressures[2] = (filtered_voltages[2] > 0) ? getSensorPressure(filtered_voltages[2], 400, 4650, 20, 300) : 0;
            scaled_pressures[3] = (filtered_voltages[3] > 0) ? getSensorPressure(filtered_voltages[3], 500, 4500, 0, 6.89) : 0;
            xSemaphoreGive(scaled_pressures_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    vTaskDelete(NULL);
}

uint16_t get_v5_rail_mv(void)
{
    uint16_t measured_mv = getScaledMillivolts(V5_REF_ADC_CHANNEL, true, 1.0f);
    if (measured_mv == 0) return 0;
    // Divider: top = 18k, bottom = 33k -> V5 = measured * (51/33)
    float v5 = ((float)measured_mv) * (51.0f / 33.0f) * V5_REF_CORRECTION_FACTOR;
    return (uint16_t)(v5 + 0.5f);
}
