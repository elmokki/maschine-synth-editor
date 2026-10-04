/*
 * Patch library on the LittleFS partition ("storage" in partitions.csv),
 * mounted at STORAGE_ROOT. Populated from data/ with `pio run -t uploadfs`.
 */
#pragma once
#include <stdbool.h>

#define STORAGE_ROOT "/lfs"

/* Mounts the filesystem (formatting it if it's blank) and logs usage. */
bool storage_init(void);
