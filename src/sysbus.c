/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * system bus - Only for dma_testbech simulation access
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "sysbus.h"

// Maximum number of memory banks supported
#define MAX_MEMORY_BANKS 8

// Memory bank structure
typedef struct {
    uint8_t* data;
    size_t size;
    int initialized;
    int owns_data;
} memory_bank_t_struct;

// Array of memory banks
static memory_bank_t_struct g_banks[MAX_MEMORY_BANKS] = {0};
static int g_bank_count = 0;

// Initialize a memory bank with given size in bytes
memory_bank_t memory_bank_init(size_t size_bytes) {
    // Find first free slot
    int free_slot = -1;
    for (int i = 0; i < MAX_MEMORY_BANKS; i++) {
        if (!g_banks[i].initialized) {
            free_slot = i;
            break;
        }
    }

    if (free_slot == -1) {
        printf("Maximum number of memory banks (%d) reached\n", MAX_MEMORY_BANKS);
        return MEMORY_BANK_INVALID;
    }

    if (size_bytes == 0) {
        printf("Invalid bank size: 0\n");
        return MEMORY_BANK_INVALID;
    }

    uint8_t* data = (uint8_t*)malloc(size_bytes);
    if (!data) {
        printf("Failed to allocate memory bank of %zu bytes\n", size_bytes);
        return MEMORY_BANK_INVALID;
    }

    memset(data, 0, size_bytes);
    g_banks[free_slot].data = data;
    g_banks[free_slot].size = size_bytes;
    g_banks[free_slot].initialized = 1;
    g_banks[free_slot].owns_data = 1;
    g_bank_count++;

    printf("Memory bank [%d] initialized: %zu bytes at address %p\n", free_slot, size_bytes, (void*)data);
    return free_slot;
}

int memory_bank_attach(void *ram, size_t size_bytes) {
    if (ram == NULL || size_bytes == 0 || g_banks[0].initialized) {
        printf("Invalid or already attached bank 0\n");
        return -1;
    }

    g_banks[0].data = (uint8_t*)ram;
    g_banks[0].size = size_bytes;
    g_banks[0].initialized = 1;
    g_banks[0].owns_data = 0;
    g_bank_count++;
    return 0;
}

// Cleanup memory bank
void memory_bank_cleanup(void) {
    for (int i = 0; i < MAX_MEMORY_BANKS; i++) {
        if (g_banks[i].initialized) {
            if (g_banks[i].owns_data && g_banks[i].data) {
                free(g_banks[i].data);
            }
            g_banks[i].data = NULL;
            g_banks[i].size = 0;
            g_banks[i].initialized = 0;
            g_banks[i].owns_data = 0;
            g_bank_count--;
            printf("Memory bank [%d] cleaned up\n", i);
        }
    }
}

int memory_bank_is_available(void) {
    // Check if at least one memory bank is initialized
    for (int i = 0; i < MAX_MEMORY_BANKS; i++) {
        if (g_banks[i].initialized) {
            return 1;
        }
    }
    return 0;
}

// Get pointer to locate data at offset
void* memory_bank_get_ptr(uint64_t offset) {
    if (!g_banks[0].initialized || offset >= g_banks[0].size) {
        return NULL;
    }

    return g_banks[0].data + offset;
}

// Get bank size in bytes
size_t memory_bank_get_size() {
    return g_banks[0].size;
}


#ifdef __cplusplus
extern "C" {
#endif

/**
 * DPI Callback: Called from RTL when DMA read access
 */
int dpi_bank_read32_legacy(long long offset) {
    uint32_t data = 0;

    if (offset < 0 || (uint64_t)offset > g_banks[0].size
        || g_banks[0].size - (size_t)offset < sizeof(data))
        return 0;
    void* src = memory_bank_get_ptr((size_t)offset);

    if (src == NULL)
        return 0;
    memcpy(&data, src, sizeof(data));

    return (int)data;
}

/**
 * DPI Callback: Called from RTL when DMA write access
 */
int dpi_bank_write32_legacy(long long offset, int value) {
    if (offset < 0 || (uint64_t)offset > g_banks[0].size
        || g_banks[0].size - (size_t)offset < sizeof(value))
        return -1;
    void* dst = memory_bank_get_ptr((size_t)offset);
    if (dst == NULL)
        return -1;
    memcpy(dst,  &value, 4);
    //fflush(stdout);
       
    return 0;
}


/**
 * DPI Callback: Called from RTL when DMA transfer completes
 */
void dma_done_callback(void)
{
    printf("DMA completed\n");
    fflush(stdout);   // keep ordering vs. printf_IMPORTANT output
}

#ifdef __cplusplus
}
#endif
