#define CATCH_CONFIG_MAIN

#include "catch2/catch.hpp"

#include "logger.h"

#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <string>

namespace
{
struct CapturedOutput
{
    std::wstring text;
    ULONG length = 0;
    unsigned int calls = 0;
    bool null_terminated = false;

    void clear()
    {
        text.clear();
        length = 0;
        calls = 0;
        null_terminated = false;
    }
};

struct LegacyCapture
{
    std::wstring text;
    unsigned int calls = 0;
};

LegacyCapture* g_legacy_capture = nullptr;

void LOGGER_CALL capture_output(void* context, LPCWSTR text, ULONG length)
{
    CapturedOutput* capture = static_cast<CapturedOutput*>(context);

    if (!capture) {
        return;
    }
    ++capture->calls;
    capture->length = length;
    capture->null_terminated = text != nullptr && text[length] == L'\0';
    if (text) {
        capture->text.assign(text, text + length);
    } else {
        capture->text.clear();
    }
}

void LOGGER_CALL capture_legacy_output(LPCWSTR text)
{
    if (!g_legacy_capture) {
        return;
    }
    ++g_legacy_capture->calls;
    g_legacy_capture->text = text ? text : L"";
}

LOGGER_RESULT logger_print_v_for_test(PLOGGER_SESSION session,
                                      LPCSTR function,
                                      ULONG line,
                                      DBGLEVEL level,
                                      LPCWSTR format,
                                      ...)
{
    LOGGER_RESULT result;
    va_list args;

    va_start(args, format);
    result = Logger_PrintV(session, function, line, level, format, args);
    va_end(args);
    return result;
}

PLOGGER_SESSION create_logger(CapturedOutput* capture,
                              DBGLEVEL level = LEVEL_DEBUG,
                              ULONG max_message_chars = 0)
{
    LOGGER_CONFIG config = {};

    config.OutputLevel = level;
    config.pfnOutput = capture_output;
    config.Context = capture;
    config.MaxMessageChars = max_message_chars;
    return Logger_CreateEx(&config);
}

std::wstring logger_body(const CapturedOutput& capture)
{
    const std::wstring marker = L" -- ";
    const std::wstring::size_type position = capture.text.find(marker);

    if (position == std::wstring::npos) {
        return std::wstring();
    }
    return capture.text.substr(position + marker.size());
}

std::wstring expected_pointer(std::uintptr_t value)
{
    static const wchar_t digits[] = L"0123456789abcdef";
    std::wstring result = L"0x";
    std::size_t index = sizeof(void*) * 2U;

    while (index != 0U) {
        const std::size_t shift = (index - 1U) * 4U;
        result += digits[(value >> shift) & 0x0fU];
        --index;
    }
    return result;
}
}

TEST_CASE("logger creates sessions and filters by level", "[logger]")
{
    CapturedOutput capture;
    PLOGGER_SESSION logger;

    CHECK(Logger_GetLevel(nullptr) == LEVEL_MAX);
    CHECK(Logger_IsEnabled(nullptr, LEVEL_INFO) == 0);
    CHECK(Logger_IsEnabled(nullptr, LEVEL_MAX) == 0);
    Logger_SetLevel(nullptr, LEVEL_INFO);
    Logger_Destroy(nullptr);

    logger = create_logger(&capture, LEVEL_DEBUG, 256);
    REQUIRE(logger != nullptr);
    CHECK(Logger_GetLevel(logger) == LEVEL_DEBUG);
    CHECK(Logger_IsEnabled(logger, LEVEL_FATAL) != 0);
    CHECK(Logger_IsEnabled(logger, LEVEL_DEBUG) != 0);
    CHECK(Logger_IsEnabled(logger, LEVEL_MAX) == 0);

    Logger_SetLevel(logger, LEVEL_WARNING);
    CHECK(Logger_GetLevel(logger) == LEVEL_WARNING);
    CHECK(Logger_IsEnabled(logger, LEVEL_INFO) == 0);
    CHECK(Logger_IsEnabled(logger, LEVEL_WARNING) != 0);
    Logger_SetLevel(logger, LEVEL_MAX);
    CHECK(Logger_GetLevel(logger) == LEVEL_WARNING);

    capture.clear();
    CHECK(Logger_PrintEx(logger, "level_test", 10, LEVEL_INFO,
                         L"filtered") == LOGGER_RESULT_FILTERED);
    CHECK(capture.calls == 0U);

    CHECK(Logger_PrintEx(logger, "level_test", 11, LEVEL_WARNING,
                         L"warning") == LOGGER_RESULT_WRITTEN);
    CHECK(capture.calls == 1U);
    CHECK(logger_body(capture) == L"warning");

    capture.clear();
    CHECK(Logger_PrintEx(logger, "level_test", 12,
                         static_cast<DBGLEVEL>(0), L"invalid") ==
          LOGGER_RESULT_INVALID_LEVEL);
    CHECK(capture.calls == 0U);
    CHECK(Logger_PrintEx(logger, "level_test", 13, LEVEL_INFO,
                         static_cast<LPCWSTR>(nullptr)) ==
          LOGGER_RESULT_INVALID_PARAMETER);
    CHECK(capture.calls == 0U);

    Logger_Destroy(logger);
}

TEST_CASE("logger formats integers, flags, widths and pointers", "[logger]")
{
    CapturedOutput capture;
    PLOGGER_SESSION logger = create_logger(&capture, LEVEL_DEBUG, 512);
    void* pointer = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1234U));

    REQUIRE(logger != nullptr);

    CHECK(Logger_PrintEx(
              logger, "format_test", 20, LEVEL_INFO,
              L"%d|%i|%u|%o|%#o|%x|%#x|%X|%b|%+d|% d|%08d|%-6d|%.5d|%c|%C|%%",
              -42, -7, 42U, 42U, 42U, 42U, 42U, 42U, 5U, 7, 7, 7, 7,
              7, 'A', 'B') == LOGGER_RESULT_WRITTEN);
    CHECK(logger_body(capture) ==
          L"-42|-7|42|52|052|2a|0x2a|2A|101|+7| 7|00000007|7     |00007|A|B|%");

    capture.clear();
    CHECK(Logger_PrintEx(logger, "format_test", 21, LEVEL_INFO, L"ptr=%p",
                         pointer) == LOGGER_RESULT_WRITTEN);
    CHECK(logger_body(capture) ==
          L"ptr=" + expected_pointer(static_cast<std::uintptr_t>(0x1234U)));

    capture.clear();
    CHECK(Logger_PrintEx(
              logger, "format_test", 22, LEVEL_INFO,
              L"%hd/%hhu/%ld/%lld/%I32u/%I64d/%I64u/%zu/%td/%ju/%tu",
              static_cast<int>(-12), static_cast<unsigned int>(250),
              static_cast<long>(-12345), static_cast<long long>(-1234567890123LL),
              static_cast<unsigned int>(4000000000U),
              static_cast<long long>(-1234567890123LL),
              static_cast<unsigned long long>(123456789012345ULL),
              static_cast<std::size_t>(12345U),
              static_cast<std::ptrdiff_t>(-77),
              static_cast<unsigned long long>(123456789012345ULL),
              static_cast<std::size_t>(12345U)) == LOGGER_RESULT_WRITTEN);
    CHECK(logger_body(capture) ==
          L"-12/250/-12345/-1234567890123/4000000000/-1234567890123/"
          L"123456789012345/12345/-77/123456789012345/12345");

    Logger_Destroy(logger);
}

TEST_CASE("logger formats wide and narrow text", "[logger]")
{
    CapturedOutput capture;
    PLOGGER_SESSION logger = create_logger(&capture, LEVEL_DEBUG, 512);
    const char narrow[] = "ansi";
    const LPCWSTR wide = L"wide";

    REQUIRE(logger != nullptr);
    CHECK(Logger_PrintEx(
              logger, "text_test", 30, LEVEL_INFO,
              L"%s|%ws|%S|%hs|%8s|%-8S|%.3s|%.*S|%c|%C|%%", wide, wide,
              narrow, narrow, wide, narrow, wide, 3, narrow, 'Q', 'R') ==
          LOGGER_RESULT_WRITTEN);
    CHECK(logger_body(capture) ==
          L"wide|wide|ansi|ansi|    wide|ansi    |wid|ans|Q|R|%");

    capture.clear();
    CHECK(Logger_PrintEx(logger, "text_test", 31, LEVEL_INFO, L"%s/%S",
                         static_cast<LPCWSTR>(nullptr),
                         static_cast<const char*>(nullptr)) ==
          LOGGER_RESULT_WRITTEN);
    CHECK(logger_body(capture) == L"(null)/(null)");

    Logger_Destroy(logger);
}

TEST_CASE("logger forwards va_list and supports the void wrapper", "[logger]")
{
    CapturedOutput capture;
    PLOGGER_SESSION logger = create_logger(&capture, LEVEL_DEBUG, 256);

    REQUIRE(logger != nullptr);
    CHECK(logger_print_v_for_test(logger, "v_test", 40, LEVEL_INFO,
                                  L"value=%d hex=%#x text=%ws", -42, 42U,
                                  L"hello") == LOGGER_RESULT_WRITTEN);
    CHECK(capture.calls == 1U);
    CHECK(capture.null_terminated);
    CHECK(logger_body(capture) == L"value=-42 hex=0x2a text=hello");

    capture.clear();
    Logger_Print(logger, "void_test", 41, LEVEL_INFO, L"void=%u", 9U);
    CHECK(capture.calls == 1U);
    CHECK(logger_body(capture) == L"void=9");

    Logger_Destroy(logger);
}

TEST_CASE("logger rejects unsupported formats", "[logger]")
{
    CapturedOutput capture;
    PLOGGER_SESSION logger = create_logger(&capture, LEVEL_DEBUG, 256);

    REQUIRE(logger != nullptr);
    CHECK(Logger_PrintEx(logger, "error_test", 50, LEVEL_INFO, L"bad=%f") ==
          LOGGER_RESULT_FORMAT_ERROR);
    CHECK(Logger_PrintEx(logger, "error_test", 51, LEVEL_INFO, L"bad=%n") ==
          LOGGER_RESULT_FORMAT_ERROR);
    CHECK(Logger_PrintEx(logger, "error_test", 52, LEVEL_INFO, L"bad=%q") ==
          LOGGER_RESULT_FORMAT_ERROR);
    CHECK(Logger_PrintEx(logger, "error_test", 53, LEVEL_INFO, L"bad=%") ==
          LOGGER_RESULT_FORMAT_ERROR);
    CHECK(Logger_PrintEx(logger, "error_test", 54, LEVEL_INFO,
                         L"bad=%I128u") == LOGGER_RESULT_FORMAT_ERROR);
    CHECK(capture.calls == 0U);

    CHECK(Logger_PrintEx(logger, "error_test", 55, LEVEL_INFO,
                         L"recovered=%d", 1) == LOGGER_RESULT_WRITTEN);
    CHECK(capture.calls == 1U);
    CHECK(logger_body(capture) == L"recovered=1");

    Logger_Destroy(logger);
}

TEST_CASE("logger reports bounded output and missing output", "[logger]")
{
    CapturedOutput capture;
    LOGGER_CONFIG config = {};
    PLOGGER_SESSION logger;

    logger = create_logger(&capture, LEVEL_INFO, 32);
    REQUIRE(logger != nullptr);
    CHECK(Logger_PrintEx(logger, "bound_test", 60, LEVEL_INFO,
                         L"this message is intentionally long") ==
          LOGGER_RESULT_TRUNCATED);
    CHECK(capture.calls == 1U);
    CHECK(capture.length == 31U);
    CHECK(capture.text.size() == 31U);
    CHECK(capture.null_terminated);
    Logger_Destroy(logger);

    capture.clear();
    logger = create_logger(&capture, LEVEL_INFO, 2);
    REQUIRE(logger != nullptr);
    CHECK(Logger_PrintEx(logger, "bound_test", 61, LEVEL_INFO, L"x") ==
          LOGGER_RESULT_TRUNCATED);
    CHECK(capture.calls == 1U);
    CHECK(capture.length == 1U);
    Logger_Destroy(logger);

    config.OutputLevel = LEVEL_INFO;
    config.pfnOutput = nullptr;
    config.Context = &capture;
    config.MaxMessageChars = 32;
    logger = Logger_CreateEx(&config);
    REQUIRE(logger != nullptr);
    CHECK(Logger_PrintEx(logger, "no_output", 62, LEVEL_INFO,
                         L"not delivered") == LOGGER_RESULT_NO_OUTPUT);
    Logger_Destroy(logger);
}

TEST_CASE("logger validates configuration and preserves legacy ABI", "[logger]")
{
    CapturedOutput capture;
    LOGGER_CONFIG config = {};
    LOGOPT option = {};
    LegacyCapture legacy;
    PLOGGER_SESSION logger;

    option.OutputLevel = LEVEL_MAX;
    option.pfnLoggerPrintImpl = capture_legacy_output;
    CHECK(Logger_Create(option) == nullptr);

    config.OutputLevel = LEVEL_INFO;
    config.pfnOutput = capture_output;
    config.Context = &capture;
    config.MaxMessageChars = 1;
    CHECK(Logger_CreateEx(&config) == nullptr);
    config.MaxMessageChars = LOGGER_MAX_MESSAGE_CHARS + 1U;
    CHECK(Logger_CreateEx(&config) == nullptr);
    CHECK(Logger_CreateEx(nullptr) == nullptr);

    option.OutputLevel = LEVEL_INFO;
    g_legacy_capture = &legacy;
    logger = Logger_Create(option);
    REQUIRE(logger != nullptr);
    CHECK(Logger_GetLevel(logger) == LEVEL_INFO);
    Logger_Print(logger, "legacy_test", 70, LEVEL_INFO, L"legacy=%d", 7);
    CHECK(legacy.calls == 1U);
    CHECK(legacy.text.find(L"legacy=%d") == std::wstring::npos);
    CHECK(legacy.text.find(L"legacy=7") != std::wstring::npos);
    CHECK(Logger_PrintEx(logger, "legacy_test", 71, LEVEL_INFO,
                         L"legacy=%u", 8U) == LOGGER_RESULT_WRITTEN);
    CHECK(legacy.calls == 2U);
    CHECK(legacy.text.find(L"legacy=8") != std::wstring::npos);
    Logger_Destroy(logger);
    g_legacy_capture = nullptr;
}

