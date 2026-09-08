/*
 * Complete user-mode logger example.
 *
 * Build this file together with logger.c and allocator.c.  The same wrapper
 * shape can be used by a kernel driver; only the callback and build headers
 * need to be changed for the target environment.
 */
#define WIN32_LEAN_AND_MEAN
#include "logger.h"

#include <stdio.h>
#include <stdarg.h>

static PLOGGER_SESSION g_logger_session = NULL;

static const char *logger_result_name(LOGGER_RESULT result)
{
    switch (result) {
    case LOGGER_RESULT_WRITTEN:
        return "WRITTEN";
    case LOGGER_RESULT_FILTERED:
        return "FILTERED";
    case LOGGER_RESULT_TRUNCATED:
        return "TRUNCATED";
    case LOGGER_RESULT_NO_OUTPUT:
        return "NO_OUTPUT";
    case LOGGER_RESULT_INVALID_PARAMETER:
        return "INVALID_PARAMETER";
    case LOGGER_RESULT_NO_MEMORY:
        return "NO_MEMORY";
    case LOGGER_RESULT_INVALID_LEVEL:
        return "INVALID_LEVEL";
    case LOGGER_RESULT_FORMAT_ERROR:
        return "FORMAT_ERROR";
    case LOGGER_RESULT_IRQL_NOT_SUPPORTED:
        return "IRQL_NOT_SUPPORTED";
    default:
        return "UNKNOWN";
    }
}

static void LOGGER_CALL logger_demo_output(void *context,
                                           LPCWSTR text,
                                           ULONG length)
{
    FILE *stream = (FILE *)context;

    if (!stream) {
        stream = stdout;
    }
    if (!text) {
        fputs("logger callback received null text\n", stderr);
        return;
    }

    /* The callback is synchronous; text is valid only during this call. */
    (void)fwprintf(stream, L"%.*ls\n", (int)length, text);
    (void)fflush(stream);
}

static LOGGER_RESULT KLogPrintV(LPCSTR function,
                                ULONG line,
                                DBGLEVEL level,
                                LPCWSTR format,
                                va_list args)
{
    if (!g_logger_session || !format) {
        return LOGGER_RESULT_INVALID_PARAMETER;
    }

    /* Logger_PrintV consumes args.  The caller owns va_end(args). */
    return Logger_PrintV(g_logger_session, function, line, level, format,
                         args);
}

static void KLogPrint(LPCSTR function,
                      ULONG line,
                      DBGLEVEL level,
                      LPCWSTR format,
                      ...)
{
    va_list args;

    va_start(args, format);
    (void)KLogPrintV(function, line, level, format, args);
    va_end(args);
}

/*
 * Keep the format string inside __VA_ARGS__.  This is valid for a call with
 * no formatting arguments, so LOGGER_DEMO_INFO(L"message") has no dangling
 * comma and does not rely on the non-standard `##__VA_ARGS__` extension.
 */
#define LOGGER_DEMO_LOG(level, ...) \
    KLogPrint(__FUNCTION__, (ULONG)__LINE__, (level), __VA_ARGS__)
#define LOGGER_DEMO_FATAL(...) LOGGER_DEMO_LOG(LEVEL_FATAL, __VA_ARGS__)
#define LOGGER_DEMO_ERROR(...) LOGGER_DEMO_LOG(LEVEL_ERROR, __VA_ARGS__)
#define LOGGER_DEMO_WARNING(...) LOGGER_DEMO_LOG(LEVEL_WARNING, __VA_ARGS__)
#define LOGGER_DEMO_IMPORTANT(...) LOGGER_DEMO_LOG(LEVEL_IMPORTANT, __VA_ARGS__)
#define LOGGER_DEMO_NOTIFY(...) LOGGER_DEMO_LOG(LEVEL_NOTIFY, __VA_ARGS__)
#define LOGGER_DEMO_INFO(...) LOGGER_DEMO_LOG(LEVEL_INFO, __VA_ARGS__)
#define LOGGER_DEMO_DEBUG(...) LOGGER_DEMO_LOG(LEVEL_DEBUG, __VA_ARGS__)

static int logger_demo_initialize(void)
{
    LOGGER_CONFIG config = {0};

    config.OutputLevel = LEVEL_DEBUG;
    config.pfnOutput = logger_demo_output;
    config.Context = stdout;
    config.MaxMessageChars = 0;

    g_logger_session = Logger_CreateEx(&config);
    if (!g_logger_session) {
        fputs("Logger_CreateEx failed\n", stderr);
        return 0;
    }
    return 1;
}

static void logger_demo_shutdown(void)
{
    if (g_logger_session) {
        /* Stop all logging callers before destroying the shared session. */
        Logger_Destroy(g_logger_session);
        g_logger_session = NULL;
    }
}

static int logger_demo_check_result(const char *operation,
                                    LOGGER_RESULT actual,
                                    LOGGER_RESULT expected)
{
    if (actual == expected) {
        return 1;
    }

    fprintf(stderr, "%s: expected %s (%d), got %s (%d)\n",
            operation, logger_result_name(expected), (int)expected,
            logger_result_name(actual), (int)actual);
    return 0;
}

static int logger_demo_print_values(LPCWSTR format, ...)
{
    LOGGER_RESULT result;
    va_list args;

    va_start(args, format);
    result = KLogPrintV("logger_demo_print_values", (ULONG)__LINE__,
                        LEVEL_INFO, format, args);
    va_end(args);
    return logger_demo_check_result("KLogPrintV", result,
                                   LOGGER_RESULT_WRITTEN);
}

static void LOGGER_CALL logger_demo_legacy_output(LPCWSTR text)
{
    if (text) {
        (void)fwprintf(stdout, L"[legacy callback] %ls\n", text);
    }
}

static int logger_demo_legacy_api(void)
{
    LOGOPT option = {0};
    PLOGGER_SESSION legacy_logger;

    option.OutputLevel = LEVEL_INFO;
    option.pfnLoggerPrintImpl = logger_demo_legacy_output;
    legacy_logger = Logger_Create(option);
    if (!legacy_logger) {
        fputs("Logger_Create failed\n", stderr);
        return 0;
    }

    Logger_Print(legacy_logger, "legacy_demo", (ULONG)__LINE__, LEVEL_INFO,
                 L"legacy API still works: value=%d", 7);
    Logger_Destroy(legacy_logger);
    return 1;
}

int main(void)
{
    LOGGER_RESULT result;
    int success = 1;

    if (!logger_demo_initialize()) {
        return 1;
    }

    LOGGER_DEMO_INFO(L"logger demo started");
    LOGGER_DEMO_DEBUG(L"debug: signed=%d unsigned=%u hex=%#x binary=%b",
                      -42, 42U, 42U, 42U);
    LOGGER_DEMO_NOTIFY(L"notify: a normal event was observed");
    LOGGER_DEMO_IMPORTANT(L"important: configuration has been loaded");
    LOGGER_DEMO_WARNING(L"warning: pointer=%p text=%ws narrow=%S",
                        (void *)g_logger_session, L"wide text", "narrow text");
    LOGGER_DEMO_FATAL(L"fatal sample: this log entry does not terminate the process");

    if (!Logger_IsEnabled(g_logger_session, LEVEL_DEBUG)) {
        fputs("debug logging should be enabled initially\n", stderr);
        success = 0;
    }

    result = Logger_PrintEx(g_logger_session, "direct_call", (ULONG)__LINE__,
                            LEVEL_INFO, L"direct Logger_PrintEx: width=%08d",
                            123);
    success = logger_demo_check_result("Logger_PrintEx", result,
                                       LOGGER_RESULT_WRITTEN) && success;

    success = logger_demo_print_values(L"forwarded va_list: value=%d text=%ws",
                                       99, L"from helper") && success;

    Logger_SetLevel(g_logger_session, LEVEL_WARNING);
    LOGGER_DEMO_INFO(L"this INFO message is filtered after SetLevel");
    result = Logger_PrintEx(g_logger_session, "filter_demo", (ULONG)__LINE__,
                            LEVEL_INFO, L"filtered result is returned");
    success = logger_demo_check_result("filtered Logger_PrintEx", result,
                                       LOGGER_RESULT_FILTERED) && success;
    LOGGER_DEMO_ERROR(L"ERROR remains enabled after SetLevel(LEVEL_WARNING)");

    if (!logger_demo_legacy_api()) {
        success = 0;
    }

    logger_demo_shutdown();
    return success ? 0 : 1;
}
