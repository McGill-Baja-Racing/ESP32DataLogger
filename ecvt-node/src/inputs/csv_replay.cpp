#include "app/modules_internal.h"
#include "app/ecvt_app.h"

std::vector<ReplayData> data;

bool parse_optional_u32_field(const char *field, uint32_t *out_value, bool *out_valid)
{
    char *endptr = NULL;
    unsigned long parsed = 0;

    while (*field == ' ' || *field == '\t') {
        field++;
    }

    if (*field == '\0') {
        *out_valid = false;
        *out_value = 0;
        return true;
    }

    parsed = strtoul(field, &endptr, 10);
    if (field == endptr) {
        return false;
    }

    while (*endptr == ' ' || *endptr == '\t') {
        endptr++;
    }

    if (*endptr != '\0') {
        return false;
    }

    *out_valid = true;
    *out_value = (uint32_t)parsed;
    return true;
}

bool parse_optional_f32_field(const char *field, float *out_value, bool *out_valid)
{
    char *endptr = NULL;
    float parsed = 0.0f;

    while (*field == ' ' || *field == '\t') {
        field++;
    }

    if (*field == '\0') {
        *out_valid = false;
        *out_value = 0.0f;
        return true;
    }

    parsed = strtof(field, &endptr);
    if (field == endptr) {
        return false;
    }

    while (*endptr == ' ' || *endptr == '\t') {
        endptr++;
    }

    if (*endptr != '\0') {
        return false;
    }

    *out_valid = true;
    *out_value = parsed;
    return true;
}

bool parse_replay_row(char *line, ReplayData *row)
{
    char *cursor = line;
    char *field_start = line;
    char *fields[5] = {0};
    int field_count = 0;
    uint32_t timestamp = 0;
    char *endptr = NULL;

    while (*cursor != '\0' && *cursor != '\n' && *cursor != '\r') {
        if (*cursor == ',') {
            *cursor = '\0';
            if (field_count < 5) {
                fields[field_count++] = field_start;
            }
            field_start = cursor + 1;
        }
        cursor++;
    }

    *cursor = '\0';
    if (field_count < 5) {
        fields[field_count++] = field_start;
    }

    if (field_count != 5) {
        return false;
    }

    while (*fields[0] == ' ' || *fields[0] == '\t') {
        fields[0]++;
    }

    timestamp = (uint32_t)strtoul(fields[0], &endptr, 10);
    if (fields[0] == endptr) {
        return false;
    }
    while (*endptr == ' ' || *endptr == '\t') {
        endptr++;
    }
    if (*endptr != '\0') {
        return false;
    }

    row->timestamp = timestamp;

    return parse_optional_u32_field(fields[1], &row->brake_state, &row->brake_state_valid) &&
           parse_optional_u32_field(fields[2], &row->throttle, &row->throttle_valid) &&
           parse_optional_f32_field(fields[3], &row->engine_rpm, &row->engine_rpm_valid) &&
           parse_optional_f32_field(fields[4], &row->wheel_rpm, &row->wheel_rpm_valid);
}

void read_sample_data(void *arg) {
    ESP_LOGI(TAG, "Opening data file: %s", PATH);

    FILE *f = fopen(PATH, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file");
        return;
    }

    // Skip header line.
    char line[128];
    if (!fgets(line, sizeof(line), f)) {
        ESP_LOGE(TAG, "File is empty or header read failed");
        fclose(f);
        return;
    }

    data.clear();

    // Read each line
    while (!ecvt_app_stop_requested() && fgets(line, sizeof(line), f)) {
        ReplayData row = {};

        if (parse_replay_row(line, &row)) {
            data.push_back(row);
        } else {
            ESP_LOGW(TAG, "Skipping malformed line: %s", line);
        }
    }

    fclose(f);

    uint32_t time_to_wait = 1; //ms

    // The owner has prepared the servo before launching replay.
    if (ecvt_app_stop_requested()) return;

    // Buffer and print loaded data in time incremenets
    for (size_t i = 0; i < data.size() && !ecvt_app_stop_requested(); i++) {
        if (data[i].brake_state_valid) {
            (void)enqueue_replay_sensor(CAN_SENSOR_BRAKE_STATE, data[i].timestamp, data[i].brake_state);
        }
        if (data[i].throttle_valid) {
            (void)enqueue_replay_sensor(CAN_SENSOR_THROTTLE, data[i].timestamp, data[i].throttle);
        }
        if (data[i].engine_rpm_valid) {
            (void)enqueue_replay_sensor_f32(CAN_SENSOR_ENGINE_RPM, data[i].timestamp, data[i].engine_rpm);
        }

        if (i < data.size() - 1){
            // Time bewteen current datapoint and next data point
            time_to_wait = data[i + 1].timestamp - data[i].timestamp;
        } else{
            break; // list is over
        }
        
        if (!ecvt_app_wait_ms(time_to_wait)) break;
    }

    // Return to the wrapper so it can acknowledge cancellation/completion.
    return;
}
