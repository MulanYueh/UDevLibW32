/*
 * Catch2 regression tests for the public PE export resolver.
 *
 * Build the resolver and the private CRT as C, then link this file as C++:
 *
 *   gcc -std=c11 -I. -c api_resolver.c libc.c
 *   g++ -std=c++17 -I. api_resolver_unit_test.cpp api_resolver.o libc.o \
 *       -o api_resolver_unit_test.exe
 *
 * The synthetic image below is laid out like a mapped PE image.  It does not
 * contain executable code; the resolver only needs valid headers, export
 * tables and readable RVAs to validate address selection and forwarders.
 */

#define CATCH_CONFIG_MAIN
#include "catch2/catch.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "api_resolver.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{

constexpr DWORD kNtHeadersRva = 0x100U;
constexpr DWORD kExportRva = 0x1000U;
constexpr DWORD kExportSize = 0x800U;
constexpr DWORD kFunctionsRva = 0x1100U;
constexpr DWORD kNamesRva = 0x1200U;
constexpr DWORD kNameOrdinalsRva = 0x1300U;
constexpr DWORD kNameAlphaRva = 0x1400U;
constexpr DWORD kNameForwardedRva = 0x1410U;
constexpr DWORD kForwarderRva = 0x1500U;
constexpr DWORD kFunctionAlphaRva = 0x2000U;
constexpr DWORD kFunctionOrdinalOnlyRva = 0x2100U;
constexpr DWORD kImageSize = 0x4000U;
constexpr DWORD kExportBase = 100U;

template <typename T>
static T* image_at(std::vector<unsigned char>& image, DWORD rva)
{
    return reinterpret_cast<T*>(image.data() + rva);
}

template <typename T>
static const T* image_at(const std::vector<unsigned char>& image, DWORD rva)
{
    return reinterpret_cast<const T*>(image.data() + rva);
}

template <typename T>
static API_RESOLVER_ADDRESS address_value(T* address)
{
    return static_cast<API_RESOLVER_ADDRESS>(
        reinterpret_cast<std::uintptr_t>(address));
}

class SyntheticImage
{
public:
    SyntheticImage()
        : storage_(kImageSize, static_cast<unsigned char>(0))
    {
        PIMAGE_DOS_HEADER dos = image_at<IMAGE_DOS_HEADER>(storage_, 0U);
        PIMAGE_NT_HEADERS nt = image_at<IMAGE_NT_HEADERS>(storage_,
                                                           kNtHeadersRva);

        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = static_cast<LONG>(kNtHeadersRva);

        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
        nt->FileHeader.NumberOfSections = 0U;
        nt->FileHeader.SizeOfOptionalHeader =
            static_cast<WORD>(sizeof(nt->OptionalHeader));
        nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR_MAGIC;
        nt->OptionalHeader.SizeOfImage = kImageSize;
        nt->OptionalHeader.SizeOfHeaders = kNtHeadersRva;
        nt->OptionalHeader.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_CUI;
        nt->OptionalHeader.NumberOfRvaAndSizes =
            IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT]
            .VirtualAddress = kExportRva;
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size =
            kExportSize;

        configure_exports("kernel32.GetProcAddress");
    }

    void* base()
    {
        return static_cast<void*>(storage_.data());
    }

    const void* base() const
    {
        return static_cast<const void*>(storage_.data());
    }

    PIMAGE_EXPORT_DIRECTORY exports()
    {
        return image_at<IMAGE_EXPORT_DIRECTORY>(storage_, kExportRva);
    }

    PIMAGE_DOS_HEADER dos_header()
    {
        return image_at<IMAGE_DOS_HEADER>(storage_, 0U);
    }

    PULONG functions()
    {
        return image_at<ULONG>(storage_, kFunctionsRva);
    }

    void configure_exports(const char* forwarder)
    {
        PIMAGE_EXPORT_DIRECTORY export_directory = exports();
        PULONG function_table = functions();
        PULONG name_table = image_at<ULONG>(storage_, kNamesRva);
        PUSHORT ordinal_table = image_at<USHORT>(storage_,
                                                 kNameOrdinalsRva);

        export_directory->Name = kNameAlphaRva;
        export_directory->Base = kExportBase;
        export_directory->NumberOfFunctions = 3U;
        export_directory->NumberOfNames = 2U;
        export_directory->AddressOfFunctions = kFunctionsRva;
        export_directory->AddressOfNames = kNamesRva;
        export_directory->AddressOfNameOrdinals = kNameOrdinalsRva;

        function_table[0] = kFunctionAlphaRva;
        function_table[1] = kForwarderRva;
        function_table[2] = kFunctionOrdinalOnlyRva;

        name_table[0] = kNameAlphaRva;
        name_table[1] = kNameForwardedRva;
        ordinal_table[0] = 0U;
        ordinal_table[1] = 1U;

        write_ascii(kNameAlphaRva, "Alpha");
        write_ascii(kNameForwardedRva, "Forwarded");
        write_ascii(kForwarderRva, forwarder);
    }

    void write_ascii(DWORD rva, const char* text)
    {
        const std::size_t length = std::strlen(text) + 1U;
        std::memcpy(storage_.data() + rva, text, length);
    }

private:
    std::vector<unsigned char> storage_;
};

static PIMAGE_EXPORT_DIRECTORY loaded_export_directory(HMODULE module)
{
    if (module == nullptr) {
        return nullptr;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) {
        return nullptr;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const unsigned char*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes <=
            IMAGE_DIRECTORY_ENTRY_EXPORT) {
        return nullptr;
    }

    const IMAGE_DATA_DIRECTORY& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (directory.VirtualAddress == 0U || directory.Size == 0U) {
        return nullptr;
    }

    return reinterpret_cast<PIMAGE_EXPORT_DIRECTORY>(
        reinterpret_cast<unsigned char*>(module) + directory.VirtualAddress);
}

static DWORD loaded_export_ordinal(HMODULE module, const char* name)
{
    PIMAGE_EXPORT_DIRECTORY export_directory =
        loaded_export_directory(module);
    if (export_directory == nullptr || name == nullptr) {
        return 0U;
    }

    const auto* base = reinterpret_cast<const unsigned char*>(module);
    const auto* names = reinterpret_cast<const DWORD*>(
        base + export_directory->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(
        base + export_directory->AddressOfNameOrdinals);

    for (DWORD index = 0U; index < export_directory->NumberOfNames; ++index) {
        const char* candidate = reinterpret_cast<const char*>(
            base + names[index]);
        if (std::strcmp(candidate, name) == 0) {
            return export_directory->Base + ordinals[index];
        }
    }

    return 0U;
}

static std::string ordinal_name(DWORD ordinal)
{
    return std::string("#") + std::to_string(ordinal);
}

} /* namespace */

TEST_CASE("resolves named and ordinal exports from a loaded module",
          "[api_resolver][loaded]")
{
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    REQUIRE(kernel32 != nullptr);

    FARPROC expected = GetProcAddress(kernel32, "GetProcAddress");
    REQUIRE(expected != nullptr);

    CHECK(ApiResolver_GetFuncAddress(kernel32, "GetProcAddress") ==
          address_value(expected));
    CHECK(ApiResolver_GetFuncAddress(kernel32, "getprocaddress") == 0U);
    CHECK(ApiResolver_GetFuncAddress(kernel32, "does_not_exist") == 0U);

    const DWORD ordinal = loaded_export_ordinal(kernel32, "GetProcAddress");
    REQUIRE(ordinal != 0U);
    const std::string requested_ordinal = ordinal_name(ordinal);
    CHECK(ApiResolver_GetFuncAddress(kernel32, requested_ordinal.c_str()) ==
          address_value(expected));
}

TEST_CASE("resolves synthetic PE exports by name and ordinal",
          "[api_resolver][pe]")
{
    SyntheticImage image;

    CHECK(ApiResolver_GetFuncAddress(image.base(), "Alpha") ==
          address_value(static_cast<unsigned char*>(image.base()) +
                        kFunctionAlphaRva));
    CHECK(ApiResolver_GetFuncAddress(image.base(), "#100") ==
          address_value(static_cast<unsigned char*>(image.base()) +
                        kFunctionAlphaRva));
    CHECK(ApiResolver_GetFuncAddress(image.base(), "#102") ==
          address_value(static_cast<unsigned char*>(image.base()) +
                        kFunctionOrdinalOnlyRva));
    CHECK(ApiResolver_GetFuncAddress(image.base(), "alpha") == 0U);
    CHECK(ApiResolver_GetFuncAddress(image.base(), "missing") == 0U);
}

TEST_CASE("follows ordinary and API Set forwarders",
          "[api_resolver][forwarder]")
{
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    REQUIRE(kernel32 != nullptr);
    FARPROC expected = GetProcAddress(kernel32, "GetProcAddress");
    REQUIRE(expected != nullptr);

    SyntheticImage ordinary_forwarder;
    CHECK(ApiResolver_GetFuncAddress(ordinary_forwarder.base(), "Forwarded") ==
          address_value(expected));
    CHECK(ApiResolver_GetFuncAddress(ordinary_forwarder.base(), "#101") ==
          address_value(expected));

    SyntheticImage api_set_forwarder;
    HMODULE api_set = GetModuleHandleW(
        L"api-ms-win-core-libraryloader-l1-2-0.dll");
    if (api_set == nullptr) {
        SUCCEED("API Set contract is unavailable on this Windows runtime");
        return;
    }
    FARPROC api_set_expected = GetProcAddress(api_set, "GetProcAddress");
    REQUIRE(api_set_expected != nullptr);

    api_set_forwarder.configure_exports(
        "api-ms-win-core-libraryloader-l1-2-0.GetProcAddress");
    CHECK(ApiResolver_GetFuncAddress(api_set_forwarder.base(), "Forwarded") ==
          address_value(api_set_expected));
}

TEST_CASE("rejects invalid PE metadata and export names",
          "[api_resolver][validation]")
{
    SyntheticImage image;

    CHECK(ApiResolver_GetFuncAddress(nullptr, "Alpha") == 0U);
    CHECK(ApiResolver_GetFuncAddress(
              reinterpret_cast<void*>(static_cast<std::uintptr_t>(1U)),
              "Alpha") ==
          0U);
    CHECK(ApiResolver_GetFuncAddress(image.base(), nullptr) == 0U);
    CHECK(ApiResolver_GetFuncAddress(
              image.base(), reinterpret_cast<const char*>(
                                static_cast<std::uintptr_t>(1U))) == 0U);

    SECTION("invalid ordinal syntax")
    {
        CHECK(ApiResolver_GetFuncAddress(image.base(), "#") == 0U);
        CHECK(ApiResolver_GetFuncAddress(image.base(), "#-1") == 0U);
        CHECK(ApiResolver_GetFuncAddress(image.base(), "#+100") == 0U);
        CHECK(ApiResolver_GetFuncAddress(image.base(), "#100x") == 0U);
        CHECK(ApiResolver_GetFuncAddress(image.base(), "#4294967296") == 0U);
    }

    SECTION("invalid DOS header")
    {
        image.dos_header()->e_magic = 0U;
        CHECK(ApiResolver_GetFuncAddress(image.base(), "Alpha") == 0U);
    }

    SECTION("export arrays outside the mapped image")
    {
        image.exports()->AddressOfFunctions = kImageSize - 2U;
        CHECK(ApiResolver_GetFuncAddress(image.base(), "Alpha") == 0U);
    }

    SECTION("unterminated forwarder inside the export range")
    {
        image.functions()[1] = kExportRva + kExportSize - 1U;
        static_cast<unsigned char*>(image.base())[kExportRva + kExportSize -
                                                   1U] = 'x';
        CHECK(ApiResolver_GetFuncAddress(image.base(), "Forwarded") == 0U);
    }
}

TEST_CASE("rejects malformed PE signatures without dereferencing them",
          "[api_resolver][validation]")
{
    std::vector<unsigned char> malformed(kImageSize,
                                         static_cast<unsigned char>(0));
    PIMAGE_DOS_HEADER dos = image_at<IMAGE_DOS_HEADER>(malformed, 0U);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = -1;

    CHECK(ApiResolver_GetFuncAddress(malformed.data(), "Alpha") == 0U);
    CHECK(ApiResolver_GetFuncAddress(static_cast<void*>(malformed.data()),
                                     "Alpha") == 0U);
}
