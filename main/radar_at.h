#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

struct radar_parameter { const char *name; unsigned min, max; const char *description; bool queryable; };
extern const struct radar_parameter radar_parameters[];
extern const size_t radar_parameter_count;
#define RADAR_PARAMETER_COUNT 19
struct radar_at_state {
  bool busy;
  unsigned id, value, baud;
  char parameter[12];
  const char *phase;
  const char *error;
  bool written, restarted;
  bool query, resumed;
  unsigned values[RADAR_PARAMETER_COUNT];
  /* 0: not read, 1: available, 2: query failed; unsupported via metadata. */
  unsigned char results[RADAR_PARAMETER_COUNT];
};
esp_err_t radar_at_init(void);
esp_err_t radar_at_submit(const char *parameter, unsigned value);
esp_err_t radar_at_query_all(void);
void radar_at_get(struct radar_at_state *out);

/* Byte cursor permits independent readers; overwritten bytes are reported. */
size_t radar_at_output(uint64_t after, bool latest, uint8_t *data, size_t capacity,
                       uint64_t *next, uint64_t *dropped, bool *reset, bool *more);
