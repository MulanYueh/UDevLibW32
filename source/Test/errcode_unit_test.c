/*
 * Standalone C unit tests for errcode.c.
 *
 * Build from the source directory with MinGW:
 *
 *   gcc -std=c11 -Wall -Wextra -Wconversion -Wshadow -Wpedantic -Werror \
 *       -Wno-unknown-pragmas -I. Test/errcode_unit_test.c errcode.c \
 *       -o errcode_unit_test.exe
 */

#include "errcode.h"

#include <stdio.h>

#define TEST_OVERFLOW_MARKER UINT16_C(0xFFFF)

static unsigned int g_check_count = 0U;
static unsigned int g_test_count = 0U;
static unsigned int g_failure_count = 0U;

#define CHECK(condition)                                                     \
    do {                                                                     \
        ++g_check_count;                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n",                  \
                    __FILE__, __LINE__, #condition);                         \
            return 0;                                                        \
        }                                                                    \
    } while (0)

typedef int (*test_function_t)(void);

typedef struct nested_context_t {
    uint64_t code;
    uint16_t expected[4];
    unsigned int count;
} nested_context_t;

static int nested_record(nested_context_t *context, uint32_t line)
{
    if (context->count >= 4U) {
        return 0;
    }
    context->expected[context->count] = (uint16_t)line;
    ++context->count;
    return ErrCode_Pack(&context->code, line) == ERR_PUSH_OK;
}

static int nested_level_1(nested_context_t *context)
{
    return nested_record(context, (uint32_t)__LINE__);
}

static int nested_level_2(nested_context_t *context)
{
    if (!nested_level_1(context)) {
        return 0;
    }
    return nested_record(context, (uint32_t)__LINE__);
}

static int nested_level_3(nested_context_t *context)
{
    if (!nested_level_2(context)) {
        return 0;
    }
    return nested_record(context, (uint32_t)__LINE__);
}

static int nested_level_4(nested_context_t *context)
{
    if (!nested_level_3(context)) {
        return 0;
    }
    return nested_record(context, (uint32_t)__LINE__);
}

static int test_empty_code(void)
{
    uint16_t e1 = UINT16_C(0xAAAA);
    uint16_t e2 = UINT16_C(0xBBBB);
    uint16_t e3 = UINT16_C(0xCCCC);
    uint16_t e4 = UINT16_C(0xDDDD);

    ErrCode_Unpack(UINT64_C(0), &e1, &e2, &e3, &e4);
    CHECK(e1 == UINT16_C(0));
    CHECK(e2 == UINT16_C(0));
    CHECK(e3 == UINT16_C(0));
    CHECK(e4 == UINT16_C(0));
    return 1;
}

static int test_pack_layout_and_capacity(void)
{
    uint64_t code = UINT64_C(0);
    uint64_t full_code = UINT64_C(0);

    CHECK(ErrCode_Pack(&code, UINT32_C(0x1234)) == ERR_PUSH_OK);
    CHECK(code == UINT64_C(0x0000000000001234));
    CHECK(ErrCode_Pack(&code, UINT32_C(0x2345)) == ERR_PUSH_OK);
    CHECK(code == UINT64_C(0x0000000023451234));
    CHECK(ErrCode_Pack(&code, UINT32_C(0x3456)) == ERR_PUSH_OK);
    CHECK(code == UINT64_C(0x0000345623451234));
    CHECK(ErrCode_Pack(&code, UINT32_C(0x4567)) == ERR_PUSH_OK);
    CHECK(code == UINT64_C(0x4567345623451234));

    full_code = code;
    CHECK(ErrCode_Pack(&code, UINT32_C(0x5678)) == ERR_PUSH_FAIL);
    CHECK(code == full_code);
    return 1;
}

static int test_payload_boundaries(void)
{
    uint64_t code = UINT64_C(0);

    CHECK(ErrCode_Pack(&code, UINT32_C(1)) == ERR_PUSH_OK);
    CHECK(ErrCode_Pack(&code, UINT32_C(0xFFFE)) == ERR_PUSH_OK);
    CHECK(code == UINT64_C(0x00000000FFFE0001));
    return 1;
}

static int test_invalid_values_write_marker(void)
{
    const uint32_t invalid_values[] = {
        UINT32_C(0),
        UINT32_C(0xFFFF),
        UINT32_C(0x10000),
        UINT32_C(0xFFFFFFFF)
    };
    unsigned int index = 0U;

    while (index < (unsigned int)(sizeof(invalid_values) /
                                  sizeof(invalid_values[0]))) {
        uint64_t code = UINT64_C(0);
        uint16_t first = UINT16_C(0);

        CHECK(ErrCode_Pack(&code, invalid_values[index]) == ERR_PUSH_FAIL);
        CHECK(code == UINT64_C(0xFFFF));
        ErrCode_Unpack(code, &first, (uint16_t *)0,
                       (uint16_t *)0, (uint16_t *)0);
        CHECK(first == TEST_OVERFLOW_MARKER);
        ++index;
    }

    return 1;
}

static int test_failed_invalid_push_consumes_slot(void)
{
    uint64_t code = UINT64_C(0);

    CHECK(ErrCode_Pack(&code, UINT32_C(1)) == ERR_PUSH_OK);
    CHECK(ErrCode_Pack(&code, UINT32_C(0)) == ERR_PUSH_FAIL);
    CHECK(code == UINT64_C(0x00000000FFFF0001));
    CHECK(ErrCode_Pack(&code, UINT32_C(2)) == ERR_PUSH_OK);
    CHECK(code == UINT64_C(0x00000002FFFF0001));
    return 1;
}

static int test_unpack_layout_and_null_outputs(void)
{
    uint16_t e1 = UINT16_C(0);
    uint16_t e2 = UINT16_C(0xAAAA);
    uint16_t e3 = UINT16_C(0xBBBB);
    uint16_t e4 = UINT16_C(0);

    ErrCode_Unpack(UINT64_C(0x4444333322221111),
                   &e1, (uint16_t *)0, (uint16_t *)0, &e4);
    CHECK(e1 == UINT16_C(0x1111));
    CHECK(e2 == UINT16_C(0xAAAA));
    CHECK(e3 == UINT16_C(0xBBBB));
    CHECK(e4 == UINT16_C(0x4444));

    ErrCode_Unpack(UINT64_C(0x4444333322221111), &e1, &e2, &e3, &e4);
    CHECK(e1 == UINT16_C(0x1111));
    CHECK(e2 == UINT16_C(0x2222));
    CHECK(e3 == UINT16_C(0x3333));
    CHECK(e4 == UINT16_C(0x4444));

    ErrCode_Unpack(UINT64_C(0x4444333322221111),
                   (uint16_t *)0, (uint16_t *)0,
                   (uint16_t *)0, (uint16_t *)0);
    return 1;
}

static int test_null_pack_and_sparse_code(void)
{
    uint64_t code = UINT64_C(0x4444000022221111);

    CHECK(ErrCode_Pack((uint64_t *)0, UINT32_C(1)) == ERR_PUSH_FAIL);
    CHECK(ErrCode_Pack(&code, UINT32_C(0x3333)) == ERR_PUSH_OK);
    CHECK(code == UINT64_C(0x4444333322221111));
    return 1;
}

static int test_nested_call_chain(void)
{
    nested_context_t context = {
        UINT64_C(0), {UINT16_C(0), UINT16_C(0),
                      UINT16_C(0), UINT16_C(0)}, 0U
    };
    uint16_t actual[4] = {0U, 0U, 0U, 0U};
    unsigned int index = 0U;

    CHECK(nested_level_4(&context));
    CHECK(context.count == 4U);
    ErrCode_Unpack(context.code, &actual[0], &actual[1],
                   &actual[2], &actual[3]);

    while (index < 4U) {
        CHECK(actual[index] == context.expected[index]);
        ++index;
    }
    return 1;
}

static int test_all_valid_values_round_trip(void)
{
    uint32_t value = UINT32_C(1);

    while (value <= UINT32_C(0xFFFE)) {
        uint64_t code = UINT64_C(0);
        uint16_t e1 = UINT16_C(0);
        uint16_t e2 = UINT16_C(0xAAAA);
        uint16_t e3 = UINT16_C(0xBBBB);
        uint16_t e4 = UINT16_C(0xCCCC);

        CHECK(ErrCode_Pack(&code, value) == ERR_PUSH_OK);
        ErrCode_Unpack(code, &e1, &e2, &e3, &e4);
        CHECK(e1 == (uint16_t)value);
        CHECK(e2 == UINT16_C(0));
        CHECK(e3 == UINT16_C(0));
        CHECK(e4 == UINT16_C(0));
        ++value;
    }
    return 1;
}

static void run_test(const char *name, test_function_t function)
{
    unsigned int checks_before = g_check_count;

    ++g_test_count;
    if (function()) {
        printf("[PASS] %s (%u checks)\n", name,
               g_check_count - checks_before);
    } else {
        ++g_failure_count;
        printf("[FAIL] %s\n", name);
    }
}

int main(void)
{
    run_test("empty code", test_empty_code);
    run_test("pack layout and capacity", test_pack_layout_and_capacity);
    run_test("payload boundaries", test_payload_boundaries);
    run_test("invalid values write marker", test_invalid_values_write_marker);
    run_test("invalid push consumes slot",
             test_failed_invalid_push_consumes_slot);
    run_test("unpack layout and null outputs",
             test_unpack_layout_and_null_outputs);
    run_test("null pack and sparse code", test_null_pack_and_sparse_code);
    run_test("nested call chain", test_nested_call_chain);
    run_test("all valid values round trip", test_all_valid_values_round_trip);

    printf("\n%u test(s), %u check(s), %u failure(s)\n",
           g_test_count, g_check_count, g_failure_count);
    return g_failure_count == 0U ? 0 : 1;
}
