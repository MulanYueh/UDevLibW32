#include "logger.h"

#include <stddef.h>
#include <stdarg.h>

#include "def.h"
#include "allocator.h"

#ifndef NULL
#define NULL (void*)0
#endif

typedef enum _LOGGER_OUTPUT_KIND {
    LOGGER_OUTPUT_LEGACY = 0,
    LOGGER_OUTPUT_CONTEXT = 1
} LOGGER_OUTPUT_KIND;

typedef struct _LOGGER_SESSION {
    volatile LONG output_level;
    ULONG max_message_chars;
    LOGGER_OUTPUT_KIND output_kind;
    union {
        typedef_LoggerPrintImpl legacy;
        LOGGER_OUTPUT_FN context;
    } output;
    void *context;
} LOGGER_SESSION, *PLOGGER_SESSION;

static const WCHAR *const g_logger_level_names[LEVEL_MAX] = {
    NULL,
    L"FATAL",
    L"ERROR",
    L"WARNING",
    L"IMPORTANT",
    L"NOTIFY",
    L"INFO",
    L"DEBUG"
};

static int logger_valid_level(DBGLEVEL level)
{
    return level >= LEVEL_FATAL && level <= LEVEL_DEBUG;
}

static void *logger_alloc(size_t size)
{
#if defined(_KERNEL_MODE)
    return Allocator_Malloc((BOOLEAN)1, size, LOGGER_POOL_TAG);
#else
    return Allocator_Malloc(size);
#endif
}

static void logger_free(void *memory)
{
#if defined(_KERNEL_MODE)
    Allocator_Free(memory, LOGGER_POOL_TAG);
#else
    Allocator_Free(memory);
#endif
}

static LONG logger_level_load(const PLOGGER_SESSION session)
{
    return InterlockedCompareExchange(
        (volatile LONG *)&session->output_level, 0, 0);
}

static PLOGGER_SESSION logger_session_create(DBGLEVEL level,
                                             ULONG max_message_chars)
{
    PLOGGER_SESSION session = NULL;

    session = (PLOGGER_SESSION)logger_alloc(sizeof(*session));
    if (!session) {
        return NULL;
    }
    session->output_level = (LONG)level;
    session->max_message_chars = max_message_chars;
    session->output_kind = LOGGER_OUTPUT_LEGACY;
    session->output.legacy = NULL;
    session->context = NULL;
    return session;
}

PLOGGER_SESSION Logger_Create(LOGOPT option)
{
    PLOGGER_SESSION session = NULL;

    if (!logger_valid_level(option.OutputLevel)) {
        return NULL;
    }

    session = logger_session_create(
        option.OutputLevel, (ULONG)LOGGER_MAX_MESSAGE_CHARS);
    if (session) {
        session->output.legacy = option.pfnLoggerPrintImpl;
    }
    return session;
}

PLOGGER_SESSION Logger_CreateEx(const LOGGER_CONFIG *config)
{
    ULONG max_message_chars = 0;
    PLOGGER_SESSION session = NULL;

    if (!config || !logger_valid_level(config->OutputLevel)) {
        return NULL;
    }

    max_message_chars = config->MaxMessageChars;
    if (max_message_chars == 0) {
        max_message_chars = (ULONG)LOGGER_MAX_MESSAGE_CHARS;
    }
    if (max_message_chars < 2 ||
        max_message_chars > (ULONG)LOGGER_MAX_MESSAGE_CHARS) {
        return NULL;
    }

    session = logger_session_create(config->OutputLevel, max_message_chars);
    if (session) {
        session->output_kind = LOGGER_OUTPUT_CONTEXT;
        session->output.context = config->pfnOutput;
        session->context = config->Context;
    }
    return session;
}

void Logger_Destroy(PLOGGER_SESSION session)
{
    if (session) {
        logger_free(session);
    }
}

void Logger_SetLevel(PLOGGER_SESSION session, DBGLEVEL level)
{
    if (session && logger_valid_level(level)) {
        (void)InterlockedExchange(
            (volatile LONG *)&session->output_level, (LONG)level);
    }
}

DBGLEVEL Logger_GetLevel(PLOGGER_SESSION session)
{
    LONG level = 0;

    if (!session) {
        return LEVEL_MAX;
    }
    level = logger_level_load(session);
    return logger_valid_level((DBGLEVEL)level)
        ? (DBGLEVEL)level : LEVEL_MAX;
}

int Logger_IsEnabled(PLOGGER_SESSION session, DBGLEVEL level)
{
    LONG output_level = 0;

    if (!session || !logger_valid_level(level)) {
        return 0;
    }
    output_level = logger_level_load(session);
    return output_level >= (LONG)level;
}

typedef struct _LOGGER_WRITER {
    WCHAR *buffer;
    ULONG capacity;
    ULONG length;
    int truncated;
} LOGGER_WRITER;

static ULONG logger_writer_space(const LOGGER_WRITER *writer)
{
    if (!writer || writer->capacity == 0 ||
        writer->length >= writer->capacity - 1) {
        return 0;
    }
    return writer->capacity - 1 - writer->length;
}

static void logger_writer_putc(LOGGER_WRITER *writer, WCHAR value)
{
    if (logger_writer_space(writer) == 0) {
        if (writer) {
            writer->truncated = 1;
        }
        return;
    }
    writer->buffer[writer->length++] = value;
}

static void logger_writer_put_wide_n(LOGGER_WRITER *writer,
                                    const WCHAR *text, ULONG count)
{
    ULONG available = 0;
    ULONG written = 0;
    ULONG index = 0;

    if (!writer || !text || count == 0) {
        return;
    }
    available = logger_writer_space(writer);
    written = count < available ? count : available;
    for (index = 0; index < written; ++index) {
        writer->buffer[writer->length++] = text[index];
    }
    if (written != count) {
        writer->truncated = 1;
    }
}

static void logger_writer_put_narrow_n(LOGGER_WRITER *writer,
                                      const char *text, ULONG count)
{
    ULONG available = 0;
    ULONG written = 0;
    ULONG index = 0;

    if (!writer || !text || count == 0) {
        return;
    }
    available = logger_writer_space(writer);
    written = count < available ? count : available;
    for (index = 0; index < written; ++index) {
        /* %S/%hs are byte strings; preserve each byte in one WCHAR unit. */
        writer->buffer[writer->length++] = (WCHAR)(unsigned char)text[index];
    }
    if (written != count) {
        writer->truncated = 1;
    }
}

static void logger_writer_repeat(LOGGER_WRITER *writer, WCHAR value,
                                 ULONG count)
{
    ULONG available = 0;
    ULONG written = 0;

    if (!writer || count == 0) {
        return;
    }
    available = logger_writer_space(writer);
    written = count < available ? count : available;
    while (written != 0) {
        writer->buffer[writer->length++] = value;
        --written;
    }
    if (count > available) {
        writer->truncated = 1;
    }
}

static ULONG logger_wide_length(const WCHAR *text, int precision)
{
    ULONG length = 0;

    if (!text) {
        return 6;
    }
    length = 0;
    while (*text && (precision < 0 || length < (ULONG)precision)) {
        ++text;
        if (length == (ULONG)-1) {
            return (ULONG)-1;
        }
        ++length;
    }
    return length;
}

static ULONG logger_narrow_length(const char *text, int precision)
{
    ULONG length = 0;

    if (!text) {
        return 6;
    }
    length = 0;
    while (*text && (precision < 0 || length < (ULONG)precision)) {
        ++text;
        if (length == (ULONG)-1) {
            return (ULONG)-1;
        }
        ++length;
    }
    return length;
}

static void logger_writer_put_wide(LOGGER_WRITER *writer, const WCHAR *text,
                                   ULONG width, int precision, int left)
{
    ULONG length = 0;
    const WCHAR *value = (const WCHAR *)0;

    value = text ? text : L"(null)";
    if (!writer || logger_writer_space(writer) == 0) {
        if (writer) {
            writer->truncated = 1;
        }
        return;
    }
    length = logger_wide_length(value, precision);
    if (!left && width > length) {
        logger_writer_repeat(writer, L' ', width - length);
    }
    logger_writer_put_wide_n(writer, value, length);
    if (left && width > length) {
        logger_writer_repeat(writer, L' ', width - length);
    }
}

static void logger_writer_put_narrow(LOGGER_WRITER *writer, const char *text,
                                     ULONG width, int precision, int left)
{
    ULONG length = 0;
    const char *value = (const char *)0;

    value = text ? text : "(null)";
    if (!writer || logger_writer_space(writer) == 0) {
        if (writer) {
            writer->truncated = 1;
        }
        return;
    }
    length = logger_narrow_length(value, precision);
    if (!left && width > length) {
        logger_writer_repeat(writer, L' ', width - length);
    }
    logger_writer_put_narrow_n(writer, value, length);
    if (left && width > length) {
        logger_writer_repeat(writer, L' ', width - length);
    }
}

typedef struct _LOGGER_FORMAT_SPEC {
    ULONG width;
    int precision;
    int alternate;
    int zero;
    int left;
    int plus;
    int space;
    int length;
} LOGGER_FORMAT_SPEC;

enum {
    LOGGER_LENGTH_NONE = 0,
    LOGGER_LENGTH_H = 1,
    LOGGER_LENGTH_HH = 2,
    LOGGER_LENGTH_L = 3,
    LOGGER_LENGTH_LL = 4,
    LOGGER_LENGTH_Z = 5,
    LOGGER_LENGTH_J = 6,
    LOGGER_LENGTH_T = 7,
    LOGGER_LENGTH_I32 = 8,
    LOGGER_LENGTH_I64 = 9,
    LOGGER_LENGTH_W = 10
};

static ULONG logger_saturating_add(ULONG left, ULONG right)
{
    if ((ULONG)-1 - left < right) {
        return (ULONG)-1;
    }
    return left + right;
}

static ULONG logger_parse_width(const WCHAR **format)
{
    ULONG width = 0;
    ULONG digit = 0;

    width = 0;
    while (**format >= L'0' && **format <= L'9') {
        digit = (ULONG)(**format - L'0');
        if (width > ((ULONG)-1 - digit) / 10U) {
            width = (ULONG)-1;
        } else {
            width = width * 10U + digit;
        }
        ++*format;
    }
    return width;
}

typedef union _LOGGER_U64_PARTS {
    unsigned long long value;
    struct {
        ULONG low;
        ULONG high;
    } parts;
} LOGGER_U64_PARTS;

/* Divide a 64-bit value by a small base without compiler runtime helpers. */
static unsigned int logger_u64_divide_small(unsigned long long *value,
                                            unsigned int divisor)
{
    LOGGER_U64_PARTS input = {0};
    LOGGER_U64_PARTS output = {0};
    ULONG high = 0;
    ULONG low = 0;
    ULONG quotient_high = 0;
    ULONG quotient_low = 0;
    ULONG remainder = 0;
    ULONG bit = 0;

    if (value == NULL || divisor == 0U) {
        return 0U;
    }

    input.value = *value;
    high = input.parts.high;
    low = input.parts.low;
    quotient_high = high / (ULONG)divisor;
    remainder = high - quotient_high * (ULONG)divisor;
    if (high == 0) {
        quotient_low = low / (ULONG)divisor;
        remainder = low - quotient_low * (ULONG)divisor;
        output.parts.high = 0;
        output.parts.low = quotient_low;
        *value = output.value;
        return (unsigned int)remainder;
    }

    quotient_low = 0;
    for (bit = 32; bit != 0; --bit) {
        ULONG shift = bit - 1;
        remainder = remainder * 2U + ((low >> shift) & 1U);
        if (remainder >= (ULONG)divisor) {
            remainder -= (ULONG)divisor;
            quotient_low |= (ULONG)1U << shift;
        }
    }
    output.parts.high = quotient_high;
    output.parts.low = quotient_low;
    *value = output.value;
    return (unsigned int)remainder;
}

static void logger_writer_number(LOGGER_WRITER *writer,
                                 unsigned long long value, int negative,
                                 unsigned int base, int uppercase,
                                 const LOGGER_FORMAT_SPEC *spec,
                                 int pointer_value)
{
    WCHAR digits[65] = {0};
    const WCHAR *prefix = (const WCHAR *)0;
    ULONG digit_count = 0;
    ULONG precision_zeroes = 0;
    ULONG prefix_length = 0;
    ULONG sign_length = 0;
    ULONG body_length = 0;
    ULONG padding = 0;
    ULONG index = 0;
    WCHAR sign = 0;
    int precision = -1;
    int value_was_nonzero = 0;

    if (!writer || !spec || (base != 2U && base != 8U &&
                             base != 10U && base != 16U)) {
        return;
    }

    value_was_nonzero = value != 0;
    digit_count = 0;
    if (value != 0 || spec->precision != 0) {
        do {
            unsigned int digit = logger_u64_divide_small(&value, base);
            digits[digit_count++] = (WCHAR)(
                digit < 10U ? L'0' + digit :
                (uppercase ? L'A' : L'a') + (digit - 10U));
        } while (value != 0);
    }

    precision = spec->precision;
    prefix = L"";
    prefix_length = 0;
    if (pointer_value) {
        prefix = L"0x";
        prefix_length = 2;
    } else if (spec->alternate && value_was_nonzero) {
        if (base == 16U) {
            prefix = uppercase ? L"0X" : L"0x";
            prefix_length = 2;
        } else if (base == 2U) {
            prefix = L"0b";
            prefix_length = 2;
        }
    }
    if (!pointer_value && spec->alternate && base == 8U &&
        (value_was_nonzero || digit_count == 0)) {
        if (precision < (int)digit_count + 1) {
            precision = (int)digit_count + 1;
        }
    }
    if (precision < 0) {
        precision_zeroes = 0;
    } else if ((ULONG)precision > digit_count) {
        precision_zeroes = (ULONG)precision - digit_count;
    } else {
        precision_zeroes = 0;
    }

    sign = 0;
    if (negative) {
        sign = L'-';
    } else if (spec->plus) {
        sign = L'+';
    } else if (spec->space) {
        sign = L' ';
    }
    sign_length = sign ? 1U : 0U;
    body_length = logger_saturating_add(sign_length, prefix_length);
    body_length = logger_saturating_add(body_length, precision_zeroes);
    body_length = logger_saturating_add(body_length, digit_count);

    padding = spec->width > body_length ? spec->width - body_length : 0;
    if (!spec->left && !(spec->zero && precision < 0)) {
        logger_writer_repeat(writer, L' ', padding);
        padding = 0;
    }
    if (sign) {
        logger_writer_putc(writer, sign);
    }
    if (prefix_length) {
        logger_writer_put_wide_n(writer, prefix, prefix_length);
    }
    if (!spec->left && spec->zero && precision < 0) {
        logger_writer_repeat(writer, L'0', padding);
        padding = 0;
    }
    logger_writer_repeat(writer, L'0', precision_zeroes);
    index = digit_count;
    while (index != 0) {
        --index;
        logger_writer_putc(writer, digits[index]);
    }
    if (spec->left) {
        logger_writer_repeat(writer, L' ', padding);
    }
}

static int logger_format_wide(LOGGER_WRITER *writer, LPCWSTR format,
                              va_list args)
{
    const WCHAR *cursor = (const WCHAR *)0;

    if (!writer || !format) {
        return 0;
    }

    cursor = format;
    while (*cursor) {
        LOGGER_FORMAT_SPEC spec = {0};
        WCHAR conversion = 0;

        if (*cursor != L'%') {
            logger_writer_putc(writer, *cursor++);
            continue;
        }
        ++cursor;
        if (*cursor == L'%') {
            logger_writer_putc(writer, L'%');
            ++cursor;
            continue;
        }

        spec.width = 0;
        spec.precision = -1;
        spec.alternate = 0;
        spec.zero = 0;
        spec.left = 0;
        spec.plus = 0;
        spec.space = 0;
        spec.length = LOGGER_LENGTH_NONE;

        for (;;) {
            if (*cursor == L'#') {
                spec.alternate = 1;
            } else if (*cursor == L'0') {
                spec.zero = 1;
            } else if (*cursor == L'-') {
                spec.left = 1;
            } else if (*cursor == L'+') {
                spec.plus = 1;
            } else if (*cursor == L' ') {
                spec.space = 1;
            } else {
                break;
            }
            ++cursor;
        }

        if (*cursor == L'*') {
            int width_value = va_arg(args, int);
            if (width_value < 0) {
                spec.left = 1;
                spec.width = (ULONG)(-(width_value + 1)) + 1U;
            } else {
                spec.width = (ULONG)width_value;
            }
            ++cursor;
        } else {
            spec.width = logger_parse_width(&cursor);
        }

        if (*cursor == L'.') {
            ++cursor;
            if (*cursor == L'*') {
                spec.precision = va_arg(args, int);
                ++cursor;
            } else {
                spec.precision = 0;
                while (*cursor >= L'0' && *cursor <= L'9') {
                    int digit = (int)(*cursor - L'0');
                    if (spec.precision > (2147483647 - digit) / 10) {
                        spec.precision = 2147483647;
                    } else {
                        spec.precision = spec.precision * 10 + digit;
                    }
                    ++cursor;
                }
            }
        }

        if (*cursor == L'h') {
            spec.length = LOGGER_LENGTH_H;
            ++cursor;
            if (*cursor == L'h') {
                spec.length = LOGGER_LENGTH_HH;
                ++cursor;
            }
        } else if (*cursor == L'l') {
            spec.length = LOGGER_LENGTH_L;
            ++cursor;
            if (*cursor == L'l') {
                spec.length = LOGGER_LENGTH_LL;
                ++cursor;
            }
        } else if (*cursor == L'z') {
            spec.length = LOGGER_LENGTH_Z;
            ++cursor;
        } else if (*cursor == L'j') {
            spec.length = LOGGER_LENGTH_J;
            ++cursor;
        } else if (*cursor == L't') {
            spec.length = LOGGER_LENGTH_T;
            ++cursor;
        } else if (*cursor == L'w') {
            spec.length = LOGGER_LENGTH_W;
            ++cursor;
        } else if (*cursor == L'I') {
            if (cursor[1] == L'3' && cursor[2] == L'2') {
                spec.length = LOGGER_LENGTH_I32;
                cursor += 3;
            } else if (cursor[1] == L'6' && cursor[2] == L'4') {
                spec.length = LOGGER_LENGTH_I64;
                cursor += 3;
            } else {
                return 0;
            }
        }

        conversion = *cursor;
        if (conversion) {
            ++cursor;
        } else {
            return 0;
        }

        switch (conversion) {
        case L'c':
        case L'C':
        {
            WCHAR value = 0;
            int narrow = conversion == L'C' ||
                         spec.length == LOGGER_LENGTH_H ||
                         spec.length == LOGGER_LENGTH_HH;
            int character = va_arg(args, int);

            value = narrow ? (WCHAR)(unsigned char)character
                           : (WCHAR)character;
            if (!spec.left && spec.width > 1) {
                logger_writer_repeat(writer, L' ', spec.width - 1);
            }
            logger_writer_putc(writer, value);
            if (spec.left && spec.width > 1) {
                logger_writer_repeat(writer, L' ', spec.width - 1);
            }
            break;
        }

        case L's':
        case L'S':
        {
            int narrow = conversion == L'S' ||
                         spec.length == LOGGER_LENGTH_H ||
                         spec.length == LOGGER_LENGTH_HH;
            if (spec.length == LOGGER_LENGTH_W ||
                spec.length == LOGGER_LENGTH_L) {
                narrow = 0;
            }
            if (narrow) {
                logger_writer_put_narrow(
                    writer, va_arg(args, const char *), spec.width,
                    spec.precision, spec.left);
            } else {
                logger_writer_put_wide(
                    writer, va_arg(args, LPCWSTR), spec.width,
                    spec.precision, spec.left);
            }
            break;
        }

        case L'd':
        case L'i':
        {
            long long value = 0;
            LOGGER_FORMAT_SPEC number_spec = spec;

            if (spec.length == LOGGER_LENGTH_LL ||
                spec.length == LOGGER_LENGTH_I64 ||
                spec.length == LOGGER_LENGTH_J) {
                value = va_arg(args, long long);
            } else if (spec.length == LOGGER_LENGTH_L) {
                value = (long long)va_arg(args, long);
            } else if (spec.length == LOGGER_LENGTH_Z ||
                       spec.length == LOGGER_LENGTH_T) {
                value = (long long)va_arg(args, ptrdiff_t);
            } else {
                value = (long long)va_arg(args, int);
            }
            if (spec.length == LOGGER_LENGTH_H) {
                value = (short)value;
            } else if (spec.length == LOGGER_LENGTH_HH) {
                value = (signed char)value;
            }
            if (value < 0) {
                logger_writer_number(
                    writer, 0ULL - (unsigned long long)value, 1, 10U, 0,
                    &number_spec, 0);
            } else {
                logger_writer_number(
                    writer, (unsigned long long)value, 0, 10U, 0,
                    &number_spec, 0);
            }
            break;
        }

        case L'u':
        case L'o':
        case L'x':
        case L'X':
        case L'b':
        {
            unsigned long long value = 0;
            unsigned int base = 0;
            int uppercase = 0;
            LOGGER_FORMAT_SPEC number_spec = spec;

            base = conversion == L'o' ? 8U :
                   (conversion == L'b' ? 2U :
                    (conversion == L'u' ? 10U : 16U));
            uppercase = conversion == L'X';
            if (spec.length == LOGGER_LENGTH_LL ||
                spec.length == LOGGER_LENGTH_I64 ||
                spec.length == LOGGER_LENGTH_J) {
                value = va_arg(args, unsigned long long);
            } else if (spec.length == LOGGER_LENGTH_L) {
                value = (unsigned long long)va_arg(args, unsigned long);
            } else if (spec.length == LOGGER_LENGTH_Z) {
                value = (unsigned long long)va_arg(args, size_t);
            } else if (spec.length == LOGGER_LENGTH_T) {
                value = (unsigned long long)va_arg(args, size_t);
            } else {
                value = (unsigned long long)va_arg(args, unsigned int);
            }
            if (spec.length == LOGGER_LENGTH_H) {
                value = (unsigned short)value;
            } else if (spec.length == LOGGER_LENGTH_HH) {
                value = (unsigned char)value;
            }
            logger_writer_number(writer, value, 0, base, uppercase,
                                 &number_spec, 0);
            break;
        }

        case L'p':
        {
            void *pointer = (void *)0;
            LOGGER_FORMAT_SPEC number_spec = spec;

            if (spec.length != LOGGER_LENGTH_NONE) {
                return 0;
            }
            pointer = va_arg(args, void *);
            if (number_spec.width == 0) {
                number_spec.width = (ULONG)(sizeof(void *) * 2U + 2U);
            }
            number_spec.zero = 1;
            logger_writer_number(
                writer, (unsigned long long)(ULONG_PTR)pointer, 0, 16U, 0,
                &number_spec, 1);
            break;
        }

        case L'Z':
#if defined(_KERNEL_MODE)
        {
            PUNICODE_STRING value = NULL;
            ULONG count = 0;

            if (spec.length != LOGGER_LENGTH_W) {
                return 0;
            }
            value = va_arg(args, PUNICODE_STRING);
            if (!value || !value->Buffer) {
                logger_writer_put_wide(writer, L"(null)", spec.width,
                                       spec.precision, spec.left);
                break;
            }
            count = value->Length / (ULONG)sizeof(WCHAR);
            if (spec.precision >= 0 && count > (ULONG)spec.precision) {
                count = (ULONG)spec.precision;
            }
            if (!spec.left && spec.width > count) {
                logger_writer_repeat(writer, L' ', spec.width - count);
            }
            logger_writer_put_wide_n(writer, value->Buffer, count);
            if (spec.left && spec.width > count) {
                logger_writer_repeat(writer, L' ', spec.width - count);
            }
            break;
        }
#else
            return 0;
#endif

        /* Floating point and %n are deliberately rejected in all builds. */
        case L'a':
        case L'A':
        case L'e':
        case L'E':
        case L'f':
        case L'F':
        case L'g':
        case L'G':
        case L'n':
        default:
            return 0;
        }
    }
    return 1;
}

static void logger_write_prefix(LOGGER_WRITER *writer, ULONG thread_id,
                                LPCSTR func, ULONG line, DBGLEVEL level)
{
    LOGGER_FORMAT_SPEC spec = {0};

    spec.width = 8;
    spec.precision = -1;
    spec.alternate = 0;
    spec.zero = 1;
    spec.left = 0;
    spec.plus = 0;
    spec.space = 0;
    spec.length = LOGGER_LENGTH_NONE;

    logger_writer_put_wide_n(writer, L"[TID:", 5);
    logger_writer_number(writer, (unsigned long long)thread_id, 0, 16U, 1,
                         &spec, 0);
    logger_writer_put_wide_n(writer, L"] ", 2);
    logger_writer_put_narrow(writer, func ? func : "(unknown)", 0, -1, 0);
    logger_writer_put_wide_n(writer, L" - LINE:", 8);

    spec.width = 0;
    spec.zero = 0;
    logger_writer_number(writer, (unsigned long long)line, 0, 10U, 0,
                         &spec, 0);
    logger_writer_put_wide_n(writer, L" - ", 3);
    logger_writer_put_wide_n(writer, g_logger_level_names[level],
                             logger_wide_length(g_logger_level_names[level],
                                                -1));
    logger_writer_put_wide_n(writer, L" -- ", 4);
}

static ULONG logger_current_thread_id(void)
{
#if defined(_KERNEL_MODE)
    return (ULONG)(ULONG_PTR)PsGetCurrentThreadId();
#else
    return (ULONG)GetCurrentThreadId();
#endif
}

LOGGER_RESULT Logger_PrintV(PLOGGER_SESSION session, LPCSTR func,
                            ULONG line, DBGLEVEL level, LPCWSTR format,
                            va_list args)
{
    WCHAR buffer[LOGGER_MAX_MESSAGE_CHARS] = {0};
    LOGGER_WRITER writer = {0};
    LONG output_level = 0;
    int formatted = 0;

    if (!session || !format) {
        return LOGGER_RESULT_INVALID_PARAMETER;
    }
    if (!logger_valid_level(level)) {
        return LOGGER_RESULT_INVALID_LEVEL;
    }
    output_level = logger_level_load(session);
    if (output_level < (LONG)level) {
        return LOGGER_RESULT_FILTERED;
    }

#if defined(_KERNEL_MODE)
    /* Formatting is stack-only, but arbitrary callbacks are not DISPATCH-safe. */
    if (KeGetCurrentIrql() > APC_LEVEL) {
        return LOGGER_RESULT_IRQL_NOT_SUPPORTED;
    }
#endif

    if (session->output_kind == LOGGER_OUTPUT_LEGACY) {
        if (!session->output.legacy) {
            return LOGGER_RESULT_NO_OUTPUT;
        }
    } else if (!session->output.context) {
        return LOGGER_RESULT_NO_OUTPUT;
    }

    writer.buffer = buffer;
    writer.capacity = session->max_message_chars;
    writer.length = 0;
    writer.truncated = 0;
    buffer[0] = 0;

    logger_write_prefix(&writer, logger_current_thread_id(), func, line, level);
    formatted = logger_format_wide(&writer, format, args);
    if (!formatted) {
        buffer[0] = 0;
        return LOGGER_RESULT_FORMAT_ERROR;
    }
    buffer[writer.length] = 0;

    if (session->output_kind == LOGGER_OUTPUT_LEGACY) {
        session->output.legacy(buffer);
    } else {
        session->output.context(session->context, buffer, writer.length);
    }
    return writer.truncated ? LOGGER_RESULT_TRUNCATED
                            : LOGGER_RESULT_WRITTEN;
}

LOGGER_RESULT Logger_PrintEx(PLOGGER_SESSION session, LPCSTR func,
                             ULONG line, DBGLEVEL level, LPCWSTR format, ...)
{
    LOGGER_RESULT result = LOGGER_RESULT_INVALID_PARAMETER;
    va_list args = {0};

    /* va_start establishes the ABI-specific traversal state before use. */
    va_start(args, format);
    result = Logger_PrintV(session, func, line, level, format, args);
    va_end(args);
    return result;
}

void Logger_Print(PLOGGER_SESSION session, LPCSTR func, ULONG line,
                  DBGLEVEL level, LPCWSTR format, ...)
{
    va_list args = {0};

    /* va_start initializes args before its first read. */
    va_start(args, format);
    (void)Logger_PrintV(session, func, line, level, format, args);
    va_end(args);
}
