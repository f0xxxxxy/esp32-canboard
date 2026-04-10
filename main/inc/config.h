#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#define CONFIG_CHANNELS 16       ///< Number of ADC input channels.
#define CONFIG_NAME_LEN 32       ///< Maximum characters for channel name.
#define CONFIG_VERSION 5         ///< Configuration structure version number.

/** @brief Pressure units selectable in UI and stored per-channel. */
typedef enum {
    UNIT_KPA = 0,
    UNIT_BAR = 1,
    UNIT_PSI = 2
} pressure_unit_t;

/**
 * @brief Per-channel median filter strength levels.
 * Stored in the 8-bit filtering field below.
 * 0 = none, 1 = low (5-sample), 2 = medium (10-sample), 3 = high (15-sample).
 */
typedef enum {
    FILTER_NONE = 0,
    FILTER_LOW  = 1,
    FILTER_MED  = 2,
    FILTER_HIGH = 3
} filter_level_t;

/** @brief Sensor measurement types for ADC channels. */
typedef enum {
    SENSOR_RAW = 0,
    SENSOR_NTC = 1,
    SENSOR_PRESSURE = 2
} sensor_type_t;

/** @brief Per-channel configuration structure. */
typedef struct {
    char name[CONFIG_NAME_LEN]; ///< ASCII name/description of the channel.
    uint32_t pullup_ohms;       ///< Optional pullup resistor value in ohms (0 if not present).
    sensor_type_t type;         ///< Sensor type (RAW, NTC, PRESSURE).
    uint8_t filtering;          ///< Median filter level (see filter_level_t enum).
    union {
        struct { ///< NTC thermistor configuration.
            uint8_t table_id;   ///< Reference table index for NTC lookup.
        } ntc;
        struct { ///< Pressure sensor configuration.
            uint16_t min_mv;    ///< Minimum voltage in millivolts.
            uint16_t max_mv;    ///< Maximum voltage in millivolts.
            float min_kpa;      ///< Minimum pressure in kilopascals.
            float max_kpa;      ///< Maximum pressure in kilopascals.
            uint8_t pressure_unit; ///< Unit enum (pressure_unit_t).
        } pressure;
        struct { ///< Raw voltage measurement (no conversion).
        } raw;
    } params;
} channel_config_t;

/** @brief Main board configuration structure persisted to SPIFFS. */
typedef struct {
    uint32_t version;                      ///< Config version for migration/compatibility.
    uint32_t can_start_id;                 ///< CAN message ID base (incremented for each message).
    uint32_t can_speed_kbps;               ///< CAN bus speed (125, 250, 500, or 1000 kbps).
    channel_config_t channels[CONFIG_CHANNELS]; ///< Per-channel configuration array.
    uint32_t crc32;                        ///< CRC32 checksum of all fields above.
} board_config_t;

/**
 * @brief Persist board configuration to SPIFFS with CRC32 verification.
 * @param cfg Pointer to configuration structure to save.
 * @return true on success; false on persistence error.
 */
bool config_save(const board_config_t *cfg);

/**
 * @brief Load and validate board configuration from SPIFFS.
 * @param cfg Pointer to configuration structure to populate.
 * @return true when data is loaded and valid; false on error or invalid CRC.
 */
bool config_load(board_config_t *cfg);

/**
 * @brief Initialize configuration structure with factory defaults.
 * @param cfg Pointer to configuration structure to initialize.
 */
void config_set_defaults(board_config_t *cfg);

#endif // CONFIG_H
