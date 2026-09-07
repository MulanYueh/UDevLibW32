#include "libc.h"

#include <limits.h>

#if !defined(LIBC_BUILD_KERNEL) && !defined(LIBC_NO_ERRNO) && \
    !defined(LIBC_SET_ERRNO)
#include <errno.h>
#define LIBC_UNIT_CHECK_ERRNO
#endif

#if defined(__has_include)
#if __has_include(<catch2/catch_all.hpp>)
#define LIBC_UNIT_CATCH2_V3
#include <catch2/catch_all.hpp>
#elif __has_include(<catch2/catch.hpp>)
#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#elif __has_include(<catch.hpp>)
#define CATCH_CONFIG_MAIN
#include <catch.hpp>
#else
#error "Catch2 headers were not found"
#endif
#else
#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#endif

/* Catch2 v2 uses __COUNTER__ in TEST_CASE and SECTION registrations. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc2y-extensions"
#endif

#if defined(LIBC_UNIT_CATCH2_V3)
int main(int argc, char* argv[])
{
    return Catch::Session().run(argc, argv);
}
#endif

namespace
{
bool same(const char* left, const char* right)
{
    return libc_strcmp(left, right) == 0;
}

union UtfOverlapBuffer
{
    uint16_t wide[8];
    char bytes[16];
};
}

TEST_CASE("printf integer formatting", "[libc][printf]")
{
    char buffer[32] = {};

    SECTION("zero-padded hexadecimal")
    {
        REQUIRE(libc_snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                                "%08x", 0x1aU) == 8);
        CHECK(same(buffer, "0000001a"));
    }

    SECTION("Windows I64 length modifier")
    {
        REQUIRE(libc_snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                                "%I64X", 0x1234ULL) == 4);
        CHECK(same(buffer, "1234"));
    }

    SECTION("alternate hexadecimal form with padding")
    {
        REQUIRE(libc_snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                                "%#08x", 0x1aU) == 8);
        CHECK(same(buffer, "0x00001a"));
    }

    SECTION("alternate octal forms")
    {
        REQUIRE(libc_snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                                "%#o", 8U) == 3);
        CHECK(same(buffer, "010"));

        REQUIRE(libc_snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                                "%#.5o", 8U) == 5);
        CHECK(same(buffer, "00010"));

        REQUIRE(libc_snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                                "%#.0o", 0U) == 1);
        CHECK(same(buffer, "0"));
    }

    SECTION("sign and alignment padding")
    {
        REQUIRE(libc_snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                                "%+08d", -12) == 8);
        CHECK(same(buffer, "-0000012"));

        REQUIRE(libc_snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                                "%-08d", 12) == 8);
        CHECK(same(buffer, "12      "));
    }

    SECTION("character field width")
    {
        REQUIRE(libc_snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                                "%4c", 'x') == 4);
        CHECK(same(buffer, "   x"));

        REQUIRE(libc_snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                                "%-4c", 'x') == 4);
        CHECK(same(buffer, "x   "));
    }

    SECTION("portable size and pointer-difference modifiers")
    {
        REQUIRE(libc_snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                                "%zu/%td", static_cast<size_t>(12),
                                static_cast<ptrdiff_t>(-3)) == 5);
        CHECK(same(buffer, "12/-3"));
    }

    SECTION("promoted short integer arguments")
    {
        REQUIRE(libc_snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                                "%hu/%hhu",
                                static_cast<unsigned short>(65535),
                                static_cast<unsigned char>(255)) == 9);
        CHECK(same(buffer, "65535/255"));
    }
}

TEST_CASE("printf string formatting and secure truncation", "[libc][printf][secure]")
{
    char buffer[32] = {};

    SECTION("dynamic string precision")
    {
        REQUIRE(libc_snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                                "%.*s", 3, "abcdef") == 3);
        CHECK(same(buffer, "abc"));
    }

    SECTION("finite count failure clears the destination")
    {
        buffer[0] = 'x';
        CHECK(libc_snprintf_s(buffer, sizeof(buffer), 3,
                              "%s", "abcdef") == -1);
        CHECK(buffer[0] == '\0');
    }

    SECTION("TRUNCATE keeps the largest terminated prefix")
    {
        CHECK(libc_snprintf_s(buffer, 8, _TRUNCATE,
                              "%s", "abcdefghi") == -1);
        CHECK(same(buffer, "abcdefg"));
    }

    SECTION("count exhaustion clears partially formatted output")
    {
        buffer[0] = 'x';
        CHECK(libc_snprintf_s(buffer, sizeof(buffer), 1,
                              "%s%-2s", "x", "y") == -1);
        CHECK(buffer[0] == '\0');
    }

    SECTION("unsupported wide-string conversion fails safely")
    {
        const wchar_t wide_source[] = L"x";
        buffer[0] = 'x';
        CHECK(libc_snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                              "%ls", wide_source) == -1);
        CHECK(buffer[0] == '\0');
    }
}

TEST_CASE("integer parsing and overflow", "[libc][strto]")
{
    SECTION("a bare hexadecimal prefix consumes no input")
    {
        const char input[] = "0x";
        char* end = nullptr;

        CHECK(libc_strtol(input, &end, 0) == 0);
        CHECK(end == input);

        end = nullptr;
        CHECK(libc_strtoul(input, &end, 16) == 0);
        CHECK(end == input);
    }

    SECTION("wide hexadecimal prefix handling")
    {
        const wchar_t bare_prefix[] = L"0x";
        const wchar_t value[] = L"0x10";
        wchar_t* end = nullptr;

        CHECK(libc_wcstoul(bare_prefix, &end, 0) == 0);
        CHECK(end == bare_prefix);

        end = nullptr;
        CHECK(libc_wcstoul(value, &end, 0) == 16);
        CHECK(end == value + 4);
    }

    SECTION("negative input wraps for unsigned conversion")
    {
        char* end = nullptr;
        CHECK(libc_strtoul("-1", &end, 10) == ULONG_MAX);
        REQUIRE(end != nullptr);
        CHECK(*end == '\0');
    }

    SECTION("maximum unsigned long long parses exactly")
    {
        char* end = nullptr;
        CHECK(libc_strtoull("18446744073709551615", &end, 10) ==
              ULLONG_MAX);
        REQUIRE(end != nullptr);
        CHECK(*end == '\0');
    }

    SECTION("no digits leaves errno unchanged")
    {
        const char input[] = "  +x";
        char* end = nullptr;
#ifdef LIBC_UNIT_CHECK_ERRNO
        errno = 0;
#endif
        CHECK(libc_strtoull(input, &end, 10) == 0);
        CHECK(end == input);
#ifdef LIBC_UNIT_CHECK_ERRNO
        CHECK(errno == 0);
#endif
    }

    SECTION("invalid base reports EINVAL")
    {
        const char input[] = "10";
        char* end = nullptr;
#ifdef LIBC_UNIT_CHECK_ERRNO
        errno = 0;
#endif
        CHECK(libc_strtoull(input, &end, 1) == 0);
        CHECK(end == input);
#ifdef LIBC_UNIT_CHECK_ERRNO
        CHECK(errno == EINVAL);
#endif
    }

    SECTION("signed overflow saturates and consumes all digits")
    {
        const char input[] = "9223372036854775808!";
        char* end = nullptr;
#ifdef LIBC_UNIT_CHECK_ERRNO
        errno = 0;
#endif
        CHECK(libc_strtoll(input, &end, 10) == LLONG_MAX);
        CHECK(end == input + 19);
#ifdef LIBC_UNIT_CHECK_ERRNO
        CHECK(errno == ERANGE);
#endif
    }

    SECTION("wide signed overflow saturates and consumes all digits")
    {
        const wchar_t input[] = L"9223372036854775808!";
        wchar_t* end = nullptr;
#ifdef LIBC_UNIT_CHECK_ERRNO
        errno = 0;
#endif
        CHECK(libc_wcstoll(input, &end, 10) == LLONG_MAX);
        CHECK(end == input + 19);
#ifdef LIBC_UNIT_CHECK_ERRNO
        CHECK(errno == ERANGE);
#endif
    }
}

TEST_CASE("integer to string conversion", "[libc][conversion]")
{
    char buffer[32] = {};
    REQUIRE(libc_ulltostr(buffer, static_cast<int>(sizeof(buffer)),
                          ULLONG_MAX, 16, 1) == 16);
    CHECK(same(buffer, "FFFFFFFFFFFFFFFF"));
}

TEST_CASE("secure scanf integer and string conversions", "[libc][scanf][secure]")
{
    SECTION("bounded string and decimal integer")
    {
        char text[8] = {};
        int value = 0;
        REQUIRE(libc_sscanf_s("abc 42", "%3s %d", text,
                              static_cast<unsigned>(sizeof(text)),
                              &value) == 2);
        CHECK(same(text, "abc"));
        CHECK(value == 42);
    }

    SECTION("maximum unsigned long long")
    {
        unsigned long long value = 0;
        REQUIRE(libc_sscanf_s("18446744073709551615", "%llu", &value) == 1);
        CHECK(value == ULLONG_MAX);
    }

    SECTION("small destination fails and is cleared")
    {
        char text[8] = {'x'};
        CHECK(libc_sscanf_s("abcdef", "%s", text, 4U) == -1);
        CHECK(text[0] == '\0');
    }

    SECTION("hh and h integer length modifiers")
    {
        signed char small_value = 0;
        unsigned short unsigned_value = 0;
        int signed_value = 0;

        REQUIRE(libc_sscanf_s("-12 65535 -7", "%hhd %hu %d",
                              &small_value, &unsigned_value,
                              &signed_value) == 3);
        CHECK(small_value == -12);
        CHECK(unsigned_value == USHRT_MAX);
        CHECK(signed_value == -7);
    }

    SECTION("positive scan set followed by integer")
    {
        char text[8] = {};
        int value = 0;
        REQUIRE(libc_sscanf_s("abc123", "%[a-z]%d", text,
                              static_cast<unsigned>(sizeof(text)),
                              &value) == 2);
        CHECK(same(text, "abc"));
        CHECK(value == 123);
    }

    SECTION("negated scan set")
    {
        char text[8] = {};
        REQUIRE(libc_sscanf_s("ABC,tail", "%[^,]", text,
                              static_cast<unsigned>(sizeof(text))) == 1);
        CHECK(same(text, "ABC"));
    }

    SECTION("closing bracket as first scan-set member")
    {
        char text[8] = {};
        REQUIRE(libc_sscanf_s("]a", "%[]a]", text,
                              static_cast<unsigned>(sizeof(text))) == 1);
        CHECK(same(text, "]a"));
    }

    SECTION("unsupported wide destination leaves it unchanged")
    {
        wchar_t text[8] = {L'!'};
        CHECK(libc_sscanf_s("x", "%ls", text,
                            static_cast<unsigned>(sizeof(text) /
                                                  sizeof(text[0]))) == -1);
        CHECK(text[0] == L'!');
    }
}

TEST_CASE("secure narrow string operations", "[libc][string][secure]")
{
    SECTION("overlapping strcpy fails and clears the destination")
    {
        char text[8] = "abcdef";
        CHECK(libc_strcpy_s(text + 1, sizeof(text) - 1, text) != 0);
        CHECK(text[1] == '\0');
    }

    SECTION("bounded append accepts a non-terminated source")
    {
        const char source[3] = {'x', 'y', 'z'};
        char destination[8] = "a";
        REQUIRE(libc_strncat_s(destination, sizeof(destination), source,
                               sizeof(source)) == 0);
        CHECK(same(destination, "axyz"));
    }

    SECTION("bounded copy accepts a non-terminated source")
    {
        const char source[3] = {'x', 'y', 'z'};
        char destination[8] = "a";
        REQUIRE(libc_strncpy_s(destination, sizeof(destination), source,
                               sizeof(source)) == 0);
        CHECK(same(destination, "xyz"));
    }

    SECTION("bounded length handles limits and null")
    {
        CHECK(libc_strnlen_s("abcdef", 3) == 3);
        CHECK(libc_strnlen_s(nullptr, 3) == 0);
    }

    SECTION("case conversion supports empty and populated strings")
    {
        char text[8] = {};
        CHECK(libc_strlwr_s(text, sizeof(text)) == 0);

        REQUIRE(libc_strcpy_s(text, sizeof(text), "AbC") == 0);
        REQUIRE(libc_strlwr_s(text, sizeof(text)) == 0);
        CHECK(same(text, "abc"));

        REQUIRE(libc_strcpy_s(text, sizeof(text), "aBc") == 0);
        REQUIRE(libc_strupr_s(text, sizeof(text)) == 0);
        CHECK(same(text, "ABC"));
    }

    SECTION("in-place reverse")
    {
        char text[8] = "abcd";
        REQUIRE(libc_strrev_s(text, sizeof(text)) == 0);
        CHECK(same(text, "dcba"));
    }
}

TEST_CASE("secure memory operations", "[libc][memory][secure]")
{
    SECTION("overlapping memcpy fails and clears the destination")
    {
        char bytes[8] = "abcdef";
        CHECK(libc_memcpy_s(bytes + 1, sizeof(bytes) - 1, bytes, 3) != 0);
        CHECK(bytes[1] == '\0');
    }

    SECTION("secure memset writes the requested byte range")
    {
        char bytes[32] = {};
        REQUIRE(libc_memset_s(bytes, sizeof(bytes), 0xA5, 4) == 0);
        CHECK(static_cast<unsigned char>(bytes[0]) == 0xA5U);
        CHECK(static_cast<unsigned char>(bytes[3]) == 0xA5U);
    }
}

TEST_CASE("wide secure memory and string operations", "[libc][wide][secure]")
{
    SECTION("in-place wide reverse")
    {
        wchar_t text[8] = L"abcd";
        REQUIRE(libc_wcsrev_s(text, sizeof(text) / sizeof(text[0])) == 0);
        CHECK(libc_wcscmp(text, L"dcba") == 0);
    }

    SECTION("wide copy set and overlapping move")
    {
        const wchar_t source[] = L"xyz";
        wchar_t destination[8] = {};
        const size_t capacity = sizeof(destination) / sizeof(destination[0]);

        REQUIRE(libc_wmemcpy_s(destination, capacity, source, 4) == 0);
        CHECK(libc_wcscmp(destination, L"xyz") == 0);

        REQUIRE(libc_wmemset_s(destination, capacity, L'!', 2) == 0);
        CHECK(destination[0] == L'!');
        CHECK(destination[1] == L'!');

        REQUIRE(libc_wmemmove_s(destination + 1, capacity - 1,
                                destination, 2) == 0);
        CHECK(destination[1] == L'!');
        CHECK(destination[2] == L'!');
    }
}

TEST_CASE("bounded concatenation preserves full destination buffers",
          "[libc][string][wide]")
{
    SECTION("narrow string")
    {
        char bytes[4] = {'a', 'b', 'c', 'd'};
        CHECK(libc_strlcat(bytes, "x", sizeof(bytes)) == 5);
        CHECK(bytes[0] == 'a');
        CHECK(bytes[3] == 'd');
    }

    SECTION("wide string")
    {
        wchar_t bytes[4] = {L'a', L'b', L'c', L'd'};
        CHECK(libc_wcslcat(bytes, L"x", sizeof(bytes) / sizeof(bytes[0])) == 5);
        CHECK(bytes[0] == L'a');
        CHECK(bytes[3] == L'd');
    }
}

TEST_CASE("overlap-safe reverse copy", "[libc][memory]")
{
    SECTION("destination starts before source")
    {
        char bytes[8] = "abcdefg";
        REQUIRE(libc_memrcpy(bytes, bytes + 1, 5) == bytes);
        CHECK(bytes[0] == 'b');
        CHECK(bytes[1] == 'c');
        CHECK(bytes[2] == 'd');
        CHECK(bytes[3] == 'e');
        CHECK(bytes[4] == 'f');
    }

    SECTION("destination starts inside source")
    {
        char bytes[8] = "abcdefg";
        REQUIRE(libc_memrcpy(bytes + 1, bytes, 5) == bytes + 1);
        CHECK(bytes[1] == 'a');
        CHECK(bytes[2] == 'b');
        CHECK(bytes[3] == 'c');
        CHECK(bytes[4] == 'd');
        CHECK(bytes[5] == 'e');
    }
}

TEST_CASE("string hashing honors the requested bound", "[libc][hash]")
{
    CHECK(libc_strhash("abc") == libc_strnhash("abc", 3));
    CHECK(libc_strhash("abc") == libc_strnhash("abc", 8));
    CHECK(libc_strnhash("abc", 3) != libc_strnhash("ABC", 3));
}

TEST_CASE("secure tokenization maintains caller-owned context",
          "[libc][token][secure]")
{
    SECTION("narrow string")
    {
        char text[] = "a,,b";
        char* context = nullptr;

        REQUIRE(libc_strtok_s(text, ",", &context) != nullptr);
        CHECK(same(text, "a"));
        REQUIRE(libc_strtok_s(nullptr, ",", &context) != nullptr);
        CHECK(same(context - 1, "b"));
    }

    SECTION("wide string")
    {
        wchar_t text[] = L"x:y";
        wchar_t* context = nullptr;

        REQUIRE(libc_wcstok_s(text, L":", &context) != nullptr);
        CHECK(text[1] == L'\0');
        REQUIRE(libc_wcstok_s(nullptr, L":", &context) != nullptr);
        CHECK(context[-1] == L'y');
    }
}

TEST_CASE("UTF-8 and UTF-16 conversion", "[libc][unicode][utf16]")
{
    const char emoji[] = "\xF0\x9F\x98\x80";

    SECTION("supplementary code point round trip")
    {
        uint16_t utf16[4] = {};
        char utf8[8] = {};
        size_t written = 0;

        REQUIRE(libc_utf8_to_utf16_s(utf16, 4, emoji, &written) == 0);
        CHECK(written == 2);
        CHECK(utf16[0] == 0xD83D);
        CHECK(utf16[1] == 0xDE00);

        REQUIRE(libc_utf16_to_utf8_s(utf8, sizeof(utf8), utf16,
                                     &written) == 0);
        CHECK(written == 4);
        CHECK(same(utf8, emoji));
    }

    SECTION("UTF-8 encoded surrogate is rejected and cleared")
    {
        uint16_t utf16[4] = {1};
        size_t written = 0;
        CHECK(libc_utf8_to_utf16_s(utf16, 4, "\xED\xA0\x80",
                                   &written) != 0);
        CHECK(utf16[0] == 0);
    }

    SECTION("overlapping UTF-8 to UTF-16 buffers are rejected")
    {
        UtfOverlapBuffer overlap = {};
        overlap.bytes[0] = 'x';
        overlap.bytes[1] = '\0';

        CHECK(libc_utf8_to_utf16_s(overlap.wide, 8, overlap.bytes,
                                   nullptr) != 0);
        CHECK(overlap.wide[0] == 0);
    }

    SECTION("overlapping UTF-16 to UTF-8 buffers are rejected")
    {
        UtfOverlapBuffer overlap = {};
        overlap.wide[0] = static_cast<uint16_t>('x');
        overlap.wide[1] = 0;

        CHECK(libc_utf16_to_utf8_s(overlap.bytes, sizeof(overlap.bytes),
                                   overlap.wide, nullptr) != 0);
        CHECK(overlap.bytes[0] == '\0');
    }
}

TEST_CASE("UTF-8 and native wchar conversion", "[libc][unicode][wchar]")
{
    const char emoji[] = "\xF0\x9F\x98\x80";
    wchar_t wide[4] = {};
    char utf8[8] = {};
    size_t written = 0;

    REQUIRE((sizeof(wchar_t) == 2 || sizeof(wchar_t) == 4));
    REQUIRE(libc_utf8_to_wchar_s(wide, 4, emoji, &written) == 0);
    if (sizeof(wchar_t) == 2)
        CHECK(written == 2);
    else
        CHECK(written == 1);

    REQUIRE(libc_wchar_to_utf8_s(utf8, sizeof(utf8), wide, &written) == 0);
    CHECK(written == 4);
    CHECK(same(utf8, emoji));
}

TEST_CASE("C++ UTF character wrappers and C ABI", "[libc][unicode][cpp]")
{
    char text[8] = {};
    char16_t utf16[8] = {};
    char roundtrip[8] = {};
    size_t written = 0;

    REQUIRE(libc_snprintf_s(text, sizeof(text), _TRUNCATE,
                            "%s", "ok") == 2);
    CHECK(same(text, "ok"));

    REQUIRE(libc_utf8_to_char16_s(utf16, 8, text, &written) == 0);
    CHECK(written == 2);
    REQUIRE(libc_char16_to_utf8_s(roundtrip, sizeof(roundtrip), utf16,
                                  &written) == 0);
    CHECK(written == 2);
    CHECK(same(roundtrip, "ok"));

#if defined(__cpp_char8_t) && __cpp_char8_t >= 201811L
    SECTION("C++20 char8_t overloads")
    {
        char8_t utf8_cxx[8] = {};
        const char8_t emoji[] = u8"\U0001F600";

        REQUIRE(libc_utf8_to_char16_s(utf16, 8, emoji, &written) == 0);
        CHECK(written == 2);
        REQUIRE(libc_char16_to_utf8_s(utf8_cxx, sizeof(utf8_cxx), utf16,
                                      &written) == 0);
        CHECK(written == 4);
    }
#endif
}

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
