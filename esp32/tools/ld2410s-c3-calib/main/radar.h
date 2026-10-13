#pragma once
#include "protocol.h"
#include "esp_err.h"
#include "cJSON.h"
#define HISTORY_SIZE 120
#define LOG_SIZE 32
#define LOG_LEN 144

typedef enum { JOB_READ, JOB_WRITE, JOB_STANDARD, JOB_MINIMAL, JOB_CALIBRATE, JOB_RESTORE } job_kind;
typedef struct { job_kind kind; ld_settings settings; uint16_t trigger, hold, seconds, delay; } radar_job;
typedef struct { uint32_t uptime_ms; ld_reading reading; } radar_sample;
typedef struct {
    bool seen, settings_valid, backup_valid, job_busy;
    ld_reading latest;
    ld_settings settings, backup;
    uint32_t last_ms, reports, bad_bytes;
    char firmware[24], firmware_raw[96], serial[40], error[160];
    uint32_t job_id;
    char job_status[160], job_action[24];
    int calibration_state; /* 0 idle, 1 countdown, 2 running, 3 done, 4 unconfirmed/failed */
    uint16_t calibration_progress, calibration_seconds;
    uint32_t calibration_started_ms, calibration_due_ms;
    radar_sample history[HISTORY_SIZE];
    unsigned history_next, history_count;
    char logs[LOG_SIZE][LOG_LEN];
    unsigned log_next, log_count;
} radar_state;
void radar_start(void);
bool radar_submit(const radar_job *job, uint32_t *id, const char **error);
void radar_copy(radar_state *out);
cJSON *radar_settings_json(const ld_settings *s);
bool radar_parse_settings(const cJSON *object, ld_settings *out, const char **error);
