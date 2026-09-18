/*
 * Nested error-code demonstration.
 *
 * Build from the source directory with MinGW:
 *
 *   gcc -std=c11 -Wall -Wextra -Wconversion -Wshadow -Wpedantic -Werror \
 *       -Wno-unknown-pragmas -I. Demo/errcode_demo.c errcode.c \
 *       -o errcode_demo.exe
 *
 * Each layer records the line at which it observes an error.  The first
 * record occupies the low 16 bits; callers then fill successively higher
 * slots while the failure unwinds.
 */

#include "errcode.h"

#include <stdio.h>

#define DEMO_OPERATION_FAILED 0
#define DEMO_RECORDING_FAILED (-1)

static int demo_record_line(uint64_t *code, uint32_t line)
{
    if (ErrCode_Pack(code, line) != ERR_PUSH_OK) {
        fprintf(stderr, "could not record line %u\n", (unsigned int)line);
        return 0;
    }
    return 1;
}

static int demo_read_device(uint64_t *code)
{
    /* Simulate the innermost operation discovering a failure. */
    if (!demo_record_line(code, (uint32_t)__LINE__)) {
        return DEMO_RECORDING_FAILED;
    }
    return DEMO_OPERATION_FAILED;
}

static int demo_parse_packet(uint64_t *code)
{
    int result = demo_read_device(code);

    if (result == DEMO_OPERATION_FAILED) {
        if (!demo_record_line(code, (uint32_t)__LINE__)) {
            return DEMO_RECORDING_FAILED;
        }
    }
    return result;
}

static int demo_process_request(uint64_t *code)
{
    int result = demo_parse_packet(code);

    if (result == DEMO_OPERATION_FAILED) {
        if (!demo_record_line(code, (uint32_t)__LINE__)) {
            return DEMO_RECORDING_FAILED;
        }
    }
    return result;
}

static int demo_service_request(uint64_t *code)
{
    int result = demo_process_request(code);

    if (result == DEMO_OPERATION_FAILED) {
        if (!demo_record_line(code, (uint32_t)__LINE__)) {
            return DEMO_RECORDING_FAILED;
        }
    }
    return result;
}

static void demo_print_code(uint64_t code)
{
    uint32_t high = (uint32_t)(code >> 32U);
    uint32_t low = (uint32_t)(code & UINT64_C(0xFFFFFFFF));

    printf("packed error code: 0x%08X%08X\n",
           (unsigned int)high, (unsigned int)low);
}

int main(void)
{
    uint64_t code = UINT64_C(0);
    uint16_t lines[4] = {0U, 0U, 0U, 0U};
    const char *levels[4] = {
        "device read", "packet parser", "request processor", "service"
    };
    unsigned int index = 0U;
    int result = demo_service_request(&code);

    if (result == DEMO_RECORDING_FAILED) {
        return 1;
    }
    if (result != DEMO_OPERATION_FAILED) {
        fputs("the simulated operation unexpectedly succeeded\n", stderr);
        return 1;
    }

    ErrCode_Unpack(code, &lines[0], &lines[1], &lines[2], &lines[3]);

    demo_print_code(code);
    puts("recorded call chain (innermost -> outermost):");
    while (index < 4U && lines[index] != 0U) {
        printf("  slot[%u] %-17s line %u\n", index, levels[index],
               (unsigned int)lines[index]);
        ++index;
    }

    return index == 4U ? 0 : 1;
}
