/*
 * api_resolver.c
 *
 * Resolves exports from an already mapped PE image without loading that image.
 * User-mode forwarders query already-loaded modules through GetModuleHandleW;
 * kernel-mode forwarders use the appropriate loader lists.  The resolver
 * deliberately treats image metadata, PEB/LDR lists and API Set maps as
 * untrusted: every variable-length region with a
 * trustworthy size field is checked before it is dereferenced, and kernel
 * callers are protected by SEH.
 * Forwarders are followed with a depth limit so a malformed image cannot
 * turn resolution into an infinite loop.  Module/PEB operations must run in
 * the target process context at an IRQL where the corresponding loader data
 * can be inspected (normally PASSIVE_LEVEL).  The PEB and PE header layout
 * follows the compilation architecture; a 64-bit driver should use a
 * separate 32-bit adapter for WOW64 images.
 */

#include "api_resolver.h"

/* The resolver is built as part of the team's CRT-enabled codebase.  Only
 * bounded operations on local or already-validated buffers are routed through
 * libc; malformed PE/loader strings continue to use guarded routines. */
#include "libc.h"
#include "def.h"

/* The same translation unit is intentionally usable in both build worlds.
 * Driver projects define _KERNEL_MODE; user-mode projects use the SDK path. */
#ifdef _KERNEL_MODE
#include <ntifs.h>
#include <ntimage.h>
#else
#include <windows.h>

/* Keep the small NT string/loader declarations local so the public header
 * remains independent of SDK and WDK internals. */
typedef struct _UNICODE_STRING {
	USHORT Length;
	USHORT MaximumLength;
	PWSTR Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef const UNICODE_STRING *PCUNICODE_STRING;
typedef struct _LDR_DATA_TABLE_ENTRY LDR_DATA_TABLE_ENTRY, *PLDR_DATA_TABLE_ENTRY;
typedef PLDR_DATA_TABLE_ENTRY PKLDR_DATA_TABLE_ENTRY;
typedef struct _PEB PEB, *PPEB;
typedef LONG NTSTATUS;
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#endif
#ifndef STATUS_NOT_FOUND
#define STATUS_NOT_FOUND ((NTSTATUS)0xC0000225L)
#endif
#ifndef STATUS_DLL_NOT_FOUND
#define STATUS_DLL_NOT_FOUND ((NTSTATUS)0xC0000135L)
#endif
#ifndef STATUS_NOT_SUPPORTED
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BBL)
#endif
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) ((NTSTATUS)(Status) >= 0)
#endif
#endif

#ifdef _KERNEL_MODE
/*
 * The WDK intentionally leaves PEB/LDR internals opaque.  Define
 * API_RESOLVER_EXTERNAL_LOADER_TYPES in a project that already supplies
 * compatible declarations; otherwise the small prefix layouts below are
 * enough for this file and match the NT loader layouts on x86/x64 Windows.
 */
#ifndef API_RESOLVER_EXTERNAL_LOADER_TYPES
typedef struct _AR_PEB_LDR_DATA {
	ULONG Length;
	BOOLEAN Initialized;
	UCHAR Reserved[3];
	PVOID SsHandle;
	LIST_ENTRY InLoadOrderModuleList;
	LIST_ENTRY InMemoryOrderModuleList;
	LIST_ENTRY InInitializationOrderModuleList;
} AR_PEB_LDR_DATA, *PAR_PEB_LDR_DATA;

typedef struct _AR_LDR_DATA_TABLE_ENTRY {
	LIST_ENTRY InLoadOrderLinks;
	LIST_ENTRY InMemoryOrderLinks;
	LIST_ENTRY InInitializationOrderLinks;
	PVOID DllBase;
	PVOID EntryPoint;
	ULONG SizeOfImage;
	UNICODE_STRING FullDllName;
	UNICODE_STRING BaseDllName;
} AR_LDR_DATA_TABLE_ENTRY, *PAR_LDR_DATA_TABLE_ENTRY;

typedef AR_LDR_DATA_TABLE_ENTRY LDR_DATA_TABLE_ENTRY;
typedef AR_LDR_DATA_TABLE_ENTRY KLDR_DATA_TABLE_ENTRY;
typedef PAR_LDR_DATA_TABLE_ENTRY PLDR_DATA_TABLE_ENTRY;
typedef PAR_LDR_DATA_TABLE_ENTRY PKLDR_DATA_TABLE_ENTRY;

typedef struct _PEB PEB;
struct _PEB {
	UCHAR InheritedAddressSpace;
	UCHAR ReadImageFileExecOptions;
	UCHAR BeingDebugged;
	UCHAR BitField;
	PVOID Mutant;
	PVOID ImageBaseAddress;
	PAR_PEB_LDR_DATA Ldr;
	PVOID ProcessParameters;
	PVOID SubSystemData;
	PVOID ProcessHeap;
	PVOID FastPebLock;
	PVOID AtlThunkSListPtr;
	PVOID IFEOKey;
	ULONG CrossProcessFlags;
	PVOID UserSharedInfoPtr;
	ULONG SystemReserved;
	ULONG AtlThunkSListPtr32;
	PVOID ApiSetMap;
};

extern PLIST_ENTRY PsLoadedModuleList;
extern PERESOURCE PsLoadedModuleResource;
#endif
extern PPEB NTAPI PsGetProcessPeb(PEPROCESS Process);
extern PIMAGE_NT_HEADERS NTAPI RtlImageNtHeader(PVOID Base);
#endif

#ifndef IN
#define IN
#endif
#ifndef OUT
#define OUT
#endif
#ifndef OPTIONAL
#define OPTIONAL
#endif
#ifndef UNREFERENCED_PARAMETER
#define UNREFERENCED_PARAMETER(P) (void)(P)
#endif

/*
 * These adapters make the intended CRT use explicit at the call sites.
 * libc_memcpy is overlap-safe in this library, but the resolver only uses it
 * for non-overlapping copies into local buffers.
 */
#ifdef _KERNEL_MODE
static PVOID
ArMemoryCopy(
	IN PVOID Destination,
	IN const void *Source,
	IN SIZE_T Length
	)
{
	return libc_memcpy(Destination, Source, Length);
}
#endif

static PVOID
ArMemorySet(
	IN PVOID Destination,
	IN UCHAR Value,
	IN SIZE_T Length
	)
{
	return libc_memset(Destination, Value, Length);
}

#ifdef _KERNEL_MODE
#define AR_TRY __try
#define AR_EXCEPT __except(EXCEPTION_EXECUTE_HANDLER)
#else
/* User-mode builds cannot use the kernel SEH spelling. */
#define AR_TRY if (1)
#define AR_EXCEPT else if (0)
#endif

#ifndef _KERNEL_MODE
static BOOLEAN
ArUserMemoryIsReadable(
	IN PVOID Address,
	IN SIZE_T Length
	)
{
	PUCHAR Current = (PUCHAR)Address;
	SIZE_T Remaining = Length;

	if (!Address) {
		return FALSE;
	}
	while (Remaining) {
		MEMORY_BASIC_INFORMATION Information = {0};
		ULONG Protection = 0;
		ULONG_PTR RegionStart = 0;
		ULONG_PTR RegionEnd = 0;
		SIZE_T Available = 0;

		if (VirtualQuery(Current, &Information, sizeof(Information)) !=
			sizeof(Information) || Information.State != MEM_COMMIT ||
			(Information.Protect & PAGE_GUARD)) {
			return FALSE;
		}
		Protection = Information.Protect & 0xffU;
		if (Protection != PAGE_READONLY && Protection != PAGE_READWRITE &&
			Protection != PAGE_WRITECOPY && Protection != PAGE_EXECUTE_READ &&
			Protection != PAGE_EXECUTE_READWRITE &&
			Protection != PAGE_EXECUTE_WRITECOPY) {
			return FALSE;
		}

		RegionStart = (ULONG_PTR)Information.BaseAddress;
		if ((ULONG_PTR)Current < RegionStart ||
			Information.RegionSize > (SIZE_T)-1 - RegionStart) {
			return FALSE;
		}
		RegionEnd = RegionStart + Information.RegionSize;
		if ((ULONG_PTR)Current >= RegionEnd) {
			return FALSE;
		}
		Available = (SIZE_T)(RegionEnd - (ULONG_PTR)Current);
		if (Remaining <= Available) {
			return TRUE;
		}
		Current += Available;
		Remaining -= Available;
	}
	return TRUE;
}
#endif

#define API_RESOLVER_MAX_FORWARDER_LENGTH 0x10000UL
#define API_RESOLVER_MAX_FORWARD_DEPTH 16UL
#define API_RESOLVER_MAX_MODULE_NAME 260UL
#define API_RESOLVER_MAX_APISET_ENTRIES 0x10000UL
#define API_RESOLVER_MAX_MODULE_ENTRIES 0x10000UL
#define API_RESOLVER_MAX_EXPORT_ENTRIES 0x100000UL

/* Windows 7 used a different API Set layout than current Windows versions. */
typedef struct _AR_API_SET_DATA_ENTRY {
	ULONG NameOffset;
	ULONG NameLength;
	ULONG ImportNameOffset;
	ULONG ImportNameLength;
} AR_API_SET_DATA_ENTRY, *PAR_API_SET_DATA_ENTRY;

typedef struct _AR_API_SET_DATA {
	ULONG Count;
	AR_API_SET_DATA_ENTRY DataEntry[1];
} AR_API_SET_DATA, *PAR_API_SET_DATA;

typedef struct _AR_API_SET_ENTRY {
	ULONG NameOffset;
	ULONG NameLength;
	ULONG DataOffset;
} AR_API_SET_ENTRY, *PAR_API_SET_ENTRY;

typedef struct _AR_API_SET_INFO {
	ULONG Version;
	ULONG Count;
	AR_API_SET_ENTRY ApiSetEntry[1];
} AR_API_SET_INFO, *PAR_API_SET_INFO;

typedef struct _AR_API_SET_NAMESPACE {
	ULONG Version;
	ULONG Size;
	ULONG Flags;
	ULONG Count;
	ULONG EntryOffset;
	ULONG HashOffset;
	ULONG HashFactor;
} AR_API_SET_NAMESPACE, *PAR_API_SET_NAMESPACE;

typedef struct _AR_API_SET_HASH_ENTRY {
	ULONG Hash;
	ULONG Index;
} AR_API_SET_HASH_ENTRY, *PAR_API_SET_HASH_ENTRY;

typedef struct _AR_API_SET_NAMESPACE_ENTRY {
	ULONG Flags;
	ULONG NameOffset;
	ULONG NameLength;
	ULONG HashedLength;
	ULONG ValueOffset;
	ULONG ValueCount;
} AR_API_SET_NAMESPACE_ENTRY, *PAR_API_SET_NAMESPACE_ENTRY;

typedef struct _AR_API_SET_VALUE_ENTRY {
	ULONG Flags;
	ULONG NameOffset;
	ULONG NameLength;
	ULONG ValueOffset;
	ULONG ValueLength;
} AR_API_SET_VALUE_ENTRY, *PAR_API_SET_VALUE_ENTRY;

/* Keep the original public type spellings for callers that include this file.
 * Define API_RESOLVER_EXTERNAL_API_SET_TYPES when the host project already
 * provides these private declarations. */
#ifndef API_RESOLVER_EXTERNAL_API_SET_TYPES
typedef AR_API_SET_DATA_ENTRY API_SET_DATA_ENTRY, *PAPI_SET_DATA_ENTRY;
typedef AR_API_SET_DATA API_SET_DATA, *PAPI_SET_DATA;
typedef AR_API_SET_ENTRY API_SET_ENTRY, *PAPI_SET_ENTRY;
typedef AR_API_SET_INFO API_SET_INFO, *PAPI_SET_INFO;
typedef AR_API_SET_NAMESPACE API_SET_NAMESPACE, *PAPI_SET_NAMESPACE;
typedef AR_API_SET_HASH_ENTRY API_SET_HASH_ENTRY, *PAPI_SET_HASH_ENTRY;
typedef AR_API_SET_NAMESPACE_ENTRY API_SET_NAMESPACE_ENTRY,
	*PAPI_SET_NAMESPACE_ENTRY;
typedef AR_API_SET_VALUE_ENTRY API_SET_VALUE_ENTRY, *PAPI_SET_VALUE_ENTRY;
#endif

/* ----------------------------- PE primitives ---------------------------- */

static BOOLEAN
ArRangeInImage(
	IN ULONG SizeOfImage,
	IN ULONG Rva,
	IN SIZE_T Length
	)
{
	/* The subtraction form avoids Rva + Length wrapping around ULONG. */
	if (!SizeOfImage || Rva > SizeOfImage) {
		return FALSE;
	}

	return Length <= (SIZE_T)(SizeOfImage - Rva);
}

static PVOID
ArAddressFromRva(
	IN PVOID Base,
	IN ULONG SizeOfImage,
	IN ULONG Rva,
	IN SIZE_T Length
	)
{
	PVOID Address = NULL;

	if (!Base || !ArRangeInImage(SizeOfImage, Rva, Length) ||
		(ULONG_PTR)Base > (ULONG_PTR)-1 - (ULONG_PTR)Rva ||
		(ULONG_PTR)Base + Rva > (ULONG_PTR)-1 - Length) {
		return NULL;
	}

	Address = (PVOID)((PUCHAR)Base + Rva);
#ifndef _KERNEL_MODE
	if (!ArUserMemoryIsReadable(Address, Length)) {
		return NULL;
	}
#endif
	return Address;
}

static PVOID
ArArrayFromRva(
	IN PVOID Base,
	IN ULONG SizeOfImage,
	IN ULONG Rva,
	IN ULONG Count,
	IN SIZE_T ElementSize
	)
{
	SIZE_T Length = 0;

	if (!Count || !ElementSize ||
		(SIZE_T)Count > (SIZE_T)-1 / ElementSize) {
		return NULL;
	}

	Length = (SIZE_T)Count * ElementSize;
	return ArAddressFromRva(Base, SizeOfImage, Rva, Length);
}

static BOOLEAN
ArAsciiEqualsExact(
	IN PCSTR Left,
	IN SIZE_T LeftLimit,
	IN PCSTR Right
	)
{
	SIZE_T Index = 0;

	/* PE export names follow GetProcAddress semantics and are case-sensitive. */
	if (!Left || !Right) {
		return FALSE;
	}
	while (Index < LeftLimit) {
#ifndef _KERNEL_MODE
		/* Stop safely if a malformed name crosses into an inaccessible page. */
		if (!ArUserMemoryIsReadable((PVOID)(Left + Index), 1)) {
			return FALSE;
		}
#endif
		if (!Left[Index]) {
			return Right[Index] == '\0';
		}
		if (!Right[Index] || Left[Index] != Right[Index]) {
			return FALSE;
		}
		++Index;
	}

	/* A name without a terminator is malformed, even if its prefix matches. */
	return FALSE;
}

static BOOLEAN
ArParseOrdinal(
	IN PCSTR Name,
	OUT PULONG_PTR Ordinal
	)
{
	ULONG_PTR Value = 0;
	ULONG Index = 1;
	ULONG Digit = 0;
	CHAR Character = 0;

	if (!Name || !Ordinal || Name[0] != '#') {
		return FALSE;
	}

	/* An export ordinal is a 32-bit value; reject signs and overflow. */
	for ( ; ; ++Index) {
		Character = Name[Index];
		if (!Character) {
			break;
		}
		/* Ten decimal digits are enough for every ULONG ordinal. */
		if (Index > 10) {
			return FALSE;
		}
		if (Character < '0' || Character > '9') {
			return FALSE;
		}
		Digit = (ULONG)(Character - '0');
		if (Value > ((ULONG_PTR)(ULONG)-1 - Digit) / 10) {
			return FALSE;
		}
		Value = Value * 10 + Digit;
	}

	if (Index == 1) {
		return FALSE;
	}

	*Ordinal = Value;
	return TRUE;
}

static LONG
ArFoldUnicode(
	IN WCHAR Character
	)
{
	if (Character >= L'A' && Character <= L'Z') {
		return (LONG)(Character + (L'a' - L'A'));
	}

	return (LONG)Character;
}

static LONG
ArUnicodeCompare(
	IN PCUNICODE_STRING Left,
	IN PCUNICODE_STRING Right
	)
{
	USHORT LeftCount = 0;
	USHORT RightCount = 0;
	USHORT Count = 0;
	USHORT Index = 0;
	LONG Difference = 0;

	if (!Left || !Right) {
		return -1;
	}
	if (!Left->Buffer || !Right->Buffer ||
		(Left->Length & (sizeof(WCHAR) - 1)) ||
		(Right->Length & (sizeof(WCHAR) - 1))) {
		return -1;
	}

	LeftCount = (USHORT)(Left->Length / sizeof(WCHAR));
	RightCount = (USHORT)(Right->Length / sizeof(WCHAR));
	Count = LeftCount < RightCount ? LeftCount : RightCount;
	for (Index = 0; Index < Count; ++Index) {
		Difference = ArFoldUnicode(Left->Buffer[Index]) -
			ArFoldUnicode(Right->Buffer[Index]);
		if (Difference) {
			return Difference;
		}
	}

	return (LONG)LeftCount - (LONG)RightCount;
}

static BOOLEAN
ArUnicodeEquals(
	IN PCUNICODE_STRING Left,
	IN PCUNICODE_STRING Right
	)
{
	return ArUnicodeCompare(Left, Right) == 0;
}

#ifdef _KERNEL_MODE
static BOOLEAN
ArModuleNameMatches(
	IN PCUNICODE_STRING Requested,
	IN PCUNICODE_STRING Candidate,
	IN BOOLEAN AllowImportSuffix
	)
{
	UNICODE_STRING Trimmed = {0};

	if (!Requested || !Requested->Buffer || !Requested->Length ||
		(Requested->Length & (sizeof(WCHAR) - 1)) || !Candidate ||
		!Candidate->Buffer || (Candidate->Length & (sizeof(WCHAR) - 1))) {
		return FALSE;
	}
	if (ArUnicodeEquals(Requested, Candidate)) {
		return TRUE;
	}

	/* Import forwarders use "module." while loader names contain .dll/.sys. */
	if (AllowImportSuffix && Requested->Length >= sizeof(WCHAR) &&
		Requested->Length <= (USHORT)-1 - 3 * sizeof(WCHAR) &&
		Requested->Buffer[Requested->Length / sizeof(WCHAR) - 1] == L'.' &&
		Candidate->Length == Requested->Length + 3 * sizeof(WCHAR)) {
		Trimmed = *Candidate;
		Trimmed.Length = (USHORT)(Candidate->Length - 3 * sizeof(WCHAR));
		return ArUnicodeEquals(Requested, &Trimmed);
	}

	return FALSE;
}
#endif

#ifdef _KERNEL_MODE
/* PEB memory is user supplied from the resolver's point of view. */
static BOOLEAN
ArUserRangeIsReadable(
	IN PVOID Address,
	IN SIZE_T Length
	)
{
	ULONG_PTR Start = (ULONG_PTR)Address;
	ULONG_PTR End = 0;

	if (!Address || Start >= (ULONG_PTR)MmUserProbeAddress ||
		Length > (SIZE_T)-1 - Start) {
		return FALSE;
	}
	End = Start + Length;
	return End <= (ULONG_PTR)MmUserProbeAddress;
}
#endif

/* --------------------------- Module enumeration ------------------------- */

static NTSTATUS
ArFindSystemModuleByName(
	IN PUNICODE_STRING ModuleName,
	OUT PKLDR_DATA_TABLE_ENTRY *ModuleEntry
	)
{
#ifndef _KERNEL_MODE
	UNREFERENCED_PARAMETER(ModuleName);
	UNREFERENCED_PARAMETER(ModuleEntry);
	return STATUS_NOT_SUPPORTED;
#else
	NTSTATUS Status = STATUS_DLL_NOT_FOUND;
	PLIST_ENTRY Link = NULL;
	PKLDR_DATA_TABLE_ENTRY Entry = NULL;
	UNICODE_STRING Candidate = {0};
	BOOLEAN Locked = FALSE;
	ULONG Visits = 0;

	if (!ModuleName || !ModuleName->Buffer || !ModuleName->Length ||
		(ModuleName->Length & (sizeof(WCHAR) - 1)) || !ModuleEntry) {
		return STATUS_INVALID_PARAMETER;
	}
	*ModuleEntry = NULL;
	if (!PsLoadedModuleList || !PsLoadedModuleResource) {
		return STATUS_DLL_NOT_FOUND;
	}

	KeEnterCriticalRegion();
	ExAcquireResourceSharedLite(PsLoadedModuleResource, TRUE);
	Locked = TRUE;
	AR_TRY {
		/* The loader list is shared with the system loader, so hold its
		 * resource while returning a stable entry pointer. */
		for (Visits = 0, Link = PsLoadedModuleList->Flink;
			Link != PsLoadedModuleList && Visits < API_RESOLVER_MAX_MODULE_ENTRIES;
			++Visits,
			Link = Link->Flink) {
			Entry = CONTAINING_RECORD(Link, KLDR_DATA_TABLE_ENTRY,
				InLoadOrderLinks);
			Candidate = (ModuleName->Buffer[0] == L'\\') ?
				Entry->FullDllName : Entry->BaseDllName;
			if (ArModuleNameMatches(ModuleName, &Candidate, TRUE)) {
				*ModuleEntry = Entry;
				Status = STATUS_SUCCESS;
				break;
			}
		}
	} AR_EXCEPT {
		Status = STATUS_INVALID_PARAMETER;
	}
	if (Locked) {
		ExReleaseResourceLite(PsLoadedModuleResource);
		KeLeaveCriticalRegion();
	}
	return Status;
#endif
}

/* Returned loader entries are borrowed pointers; callers must not retain them
 * across module unload or process teardown. */
NTSTATUS
GetSystemModuleDataEntryByName2(
	IN PUNICODE_STRING ModuleName,
	OUT PKLDR_DATA_TABLE_ENTRY *OutDataEntry
	)
{
	return ArFindSystemModuleByName(ModuleName, OutDataEntry);
}

static NTSTATUS
ArFindProcessModuleByName(
	IN PUNICODE_STRING ModuleName,
	OUT PLDR_DATA_TABLE_ENTRY *ModuleEntry,
	IN BOOLEAN AllowImportSuffix
	)
{
#ifndef _KERNEL_MODE
	UNREFERENCED_PARAMETER(ModuleName);
	UNREFERENCED_PARAMETER(ModuleEntry);
	UNREFERENCED_PARAMETER(AllowImportSuffix);
	return STATUS_NOT_SUPPORTED;
#else
	NTSTATUS Status = STATUS_DLL_NOT_FOUND;
	PPEB Peb = NULL;
	PLDR_DATA_TABLE_ENTRY Entry = NULL;
	UNICODE_STRING Candidate = {0};
	ULONG Visits = 0;

	if (!ModuleName || !ModuleName->Buffer || !ModuleName->Length ||
		(ModuleName->Length & (sizeof(WCHAR) - 1)) || !ModuleEntry) {
		return STATUS_INVALID_PARAMETER;
	}
	*ModuleEntry = NULL;
	Peb = PsGetProcessPeb(PsGetCurrentProcess());
	if (!Peb || !ArUserRangeIsReadable(Peb, sizeof(PEB))) {
		return STATUS_INVALID_PARAMETER;
	}

	AR_TRY {
		if (!Peb->Ldr || !Peb->Ldr->Initialized ||
			!ArUserRangeIsReadable(Peb->Ldr, sizeof(*Peb->Ldr))) {
			return STATUS_INVALID_PARAMETER;
		}
		for (Visits = 0, Entry = CONTAINING_RECORD(Peb->Ldr->InLoadOrderModuleList.Flink,
			LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
			Entry != CONTAINING_RECORD(&Peb->Ldr->InLoadOrderModuleList,
			LDR_DATA_TABLE_ENTRY, InLoadOrderLinks) &&
			Visits < API_RESOLVER_MAX_MODULE_ENTRIES;
			++Visits,
			Entry = CONTAINING_RECORD(Entry->InLoadOrderLinks.Flink,
			LDR_DATA_TABLE_ENTRY, InLoadOrderLinks)) {
			/* A forwarder supplies a module prefix (for example "ntdll.");
			 * the loader normally stores the concrete .dll/.sys name. */
			Candidate = (ModuleName->Buffer[0] == L'\\') ?
				Entry->FullDllName : Entry->BaseDllName;
			if (ArModuleNameMatches(ModuleName, &Candidate, AllowImportSuffix)) {
				*ModuleEntry = Entry;
				Status = STATUS_SUCCESS;
				break;
			}
		}
	} AR_EXCEPT {
		Status = STATUS_INVALID_PARAMETER;
	}
	return Status;
#endif
}

/* The process must be attached to the target address space before calling;
 * returned entries are borrowed pointers owned by the loader. */
NTSTATUS
GetProcessModuleDataEntryByName(
	IN PUNICODE_STRING ModuleName,
	OUT PLDR_DATA_TABLE_ENTRY *ModuleEntry
	)
{
	return ArFindProcessModuleByName(ModuleName, ModuleEntry, FALSE);
}

NTSTATUS
GetProcessModuleDataEntryByName2(
	IN PUNICODE_STRING ModuleName,
	OUT PLDR_DATA_TABLE_ENTRY *ModuleEntry
	)
{
	return ArFindProcessModuleByName(ModuleName, ModuleEntry, TRUE);
}

NTSTATUS
GetProcessModuleDataEntry(
	IN PVOID VirtualAddress,
	OUT PLDR_DATA_TABLE_ENTRY *ModuleEntry
	)
{
#ifndef _KERNEL_MODE
	UNREFERENCED_PARAMETER(VirtualAddress);
	UNREFERENCED_PARAMETER(ModuleEntry);
	return STATUS_NOT_SUPPORTED;
#else
	NTSTATUS Status = STATUS_DLL_NOT_FOUND;
	PPEB Peb = NULL;
	PLDR_DATA_TABLE_ENTRY Entry = NULL;
	ULONG Visits = 0;

	if (!VirtualAddress || !ModuleEntry) {
		return STATUS_INVALID_PARAMETER;
	}
	*ModuleEntry = NULL;
	Peb = PsGetProcessPeb(PsGetCurrentProcess());
	if (!Peb || !ArUserRangeIsReadable(Peb, sizeof(PEB))) {
		return STATUS_INVALID_PARAMETER;
	}

	AR_TRY {
		if (!Peb->Ldr || !Peb->Ldr->Initialized ||
			!ArUserRangeIsReadable(Peb->Ldr, sizeof(*Peb->Ldr))) {
			return STATUS_INVALID_PARAMETER;
		}
		for (Visits = 0, Entry = CONTAINING_RECORD(Peb->Ldr->InLoadOrderModuleList.Flink,
			LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
			Entry != CONTAINING_RECORD(&Peb->Ldr->InLoadOrderModuleList,
			LDR_DATA_TABLE_ENTRY, InLoadOrderLinks) &&
			Visits < API_RESOLVER_MAX_MODULE_ENTRIES;
			++Visits,
			Entry = CONTAINING_RECORD(Entry->InLoadOrderLinks.Flink,
			LDR_DATA_TABLE_ENTRY, InLoadOrderLinks)) {
			if (Entry->DllBase && Entry->SizeOfImage &&
				(ULONG_PTR)Entry->DllBase <= (ULONG_PTR)-1 - Entry->SizeOfImage &&
				(ULONG_PTR)VirtualAddress >= (ULONG_PTR)Entry->DllBase &&
				(ULONG_PTR)VirtualAddress <
				(ULONG_PTR)Entry->DllBase + Entry->SizeOfImage) {
				*ModuleEntry = Entry;
				Status = STATUS_SUCCESS;
				break;
			}
		}
	} AR_EXCEPT {
		Status = STATUS_INVALID_PARAMETER;
	}
	return Status;
#endif
}

/* ---------------------------- API Set parsing ---------------------------- */

static PVOID
ArApiSetOffset(
	IN PVOID Map,
	IN ULONG MapSize,
	IN ULONG Offset,
	IN SIZE_T Length
	)
{
	PVOID Address = NULL;

	/* Version-2 maps do not carry a total size; MapSize == 0 still protects
	 * pointer arithmetic, while the caller's image/range checks bound access. */
	if (!Map || (MapSize && !ArRangeInImage(MapSize, Offset, Length)) ||
		(ULONG_PTR)Map > (ULONG_PTR)-1 - (ULONG_PTR)Offset ||
		(ULONG_PTR)Map + Offset > (ULONG_PTR)-1 - Length) {
		return NULL;
	}
	Address = (PVOID)((PUCHAR)Map + Offset);
#ifndef _KERNEL_MODE
	if (!ArUserMemoryIsReadable(Address, Length)) {
		return NULL;
	}
#endif
	return Address;
}

static BOOLEAN
ArApiSetIndexedOffset(
	IN ULONG BaseOffset,
	IN ULONG Index,
	IN ULONG ElementSize,
	OUT PULONG Offset
	)
{
	if (!ElementSize || Index > ((ULONG)-1 - BaseOffset) / ElementSize || !Offset) {
		return FALSE;
	}
	*Offset = BaseOffset + Index * ElementSize;
	return TRUE;
}

static ULONG
ArApiSetHash(
	IN PCUNICODE_STRING Name,
	IN ULONG HashFactor
	)
{
	ULONG Hash = 0;
	USHORT Index = 0;
	USHORT Count = 0;
	WCHAR Character = 0;

	if (!Name || !Name->Buffer) {
		return 0;
	}
	Count = (USHORT)(Name->Length / sizeof(WCHAR));
	for (Index = 0; Index < Count; ++Index) {
		Character = Name->Buffer[Index];
		if (Character >= L'A' && Character <= L'Z') {
			Character = (WCHAR)(Character + (L'a' - L'A'));
		}
		Hash = Character + HashFactor * Hash;
	}
	return Hash;
}

PAPI_SET_NAMESPACE_ENTRY
ApiSetpSearchForApiSet(
	IN PAPI_SET_NAMESPACE ApiNamespace,
	IN PUNICODE_STRING ApiNameToResolve
	)
{
	/* The public type names are kept as aliases for source compatibility. */
	PAR_API_SET_NAMESPACE Namespace = (PAR_API_SET_NAMESPACE)ApiNamespace;
	PAR_API_SET_NAMESPACE_ENTRY Found = NULL;
	PAR_API_SET_HASH_ENTRY HashEntry = NULL;
	UNICODE_STRING Candidate = {0};
	ULONG Hash = 0;
	ULONG Low = 0;
	ULONG High = 0;
	ULONG Middle = 0;
	ULONG Probe = 0;
	ULONG Offset = 0;
	
	AR_TRY {
	if (!Namespace || !ApiNameToResolve || !ApiNameToResolve->Buffer ||
		(ApiNameToResolve->Length & (sizeof(WCHAR) - 1)) ||
		Namespace->Size < sizeof(*Namespace) || !Namespace->Count ||
		Namespace->Count > API_RESOLVER_MAX_APISET_ENTRIES ||
		!Namespace->EntryOffset || !Namespace->HashOffset) {
		return NULL;
	}
	Hash = ArApiSetHash(ApiNameToResolve, Namespace->HashFactor);
	Low = 0;
	High = Namespace->Count - 1;
	while (Low <= High) {
		Middle = Low + (High - Low) / 2;
		if (!ArApiSetIndexedOffset(Namespace->HashOffset, Middle,
			sizeof(*HashEntry), &Offset)) {
			return NULL;
		}
		HashEntry = (PAR_API_SET_HASH_ENTRY)ArApiSetOffset(Namespace,
			Namespace->Size, Offset, sizeof(*HashEntry));
		if (!HashEntry) {
			return NULL;
		}
		if (Hash < HashEntry->Hash) {
			if (!Middle) {
				break;
			}
			High = Middle - 1;
		}
		else if (Hash > HashEntry->Hash) {
			Low = Middle + 1;
		}
		else {
			/* Hash collisions are possible.  Check every adjacent entry with
			 * the same hash instead of assuming the first one is unique. */
			for (Probe = Middle; Probe > 0; --Probe) {
				if (!ArApiSetIndexedOffset(Namespace->HashOffset, Probe - 1,
					sizeof(*HashEntry), &Offset)) {
					return NULL;
				}
				HashEntry = (PAR_API_SET_HASH_ENTRY)ArApiSetOffset(Namespace,
					Namespace->Size, Offset, sizeof(*HashEntry));
				if (!HashEntry || HashEntry->Hash != Hash) {
					break;
				}
			}
			for ( ; Probe < Namespace->Count; ++Probe) {
				if (!ArApiSetIndexedOffset(Namespace->HashOffset, Probe,
					sizeof(*HashEntry), &Offset)) {
					return NULL;
				}
				HashEntry = (PAR_API_SET_HASH_ENTRY)ArApiSetOffset(Namespace,
					Namespace->Size, Offset, sizeof(*HashEntry));
				if (!HashEntry || HashEntry->Hash != Hash) {
					break;
				}
				if (HashEntry->Index >= Namespace->Count ||
					!ArApiSetIndexedOffset(Namespace->EntryOffset, HashEntry->Index,
						sizeof(*Found), &Offset)) {
					continue;
				}
				Found = (PAR_API_SET_NAMESPACE_ENTRY)ArApiSetOffset(Namespace,
					Namespace->Size, Offset, sizeof(*Found));
				if (!Found || !Found->NameOffset || !Found->NameLength ||
					Found->NameLength > 0xFFFEUL ||
					Found->HashedLength > Found->NameLength ||
					Found->HashedLength > 0xFFFEUL) {
					continue;
				}
				Candidate.Buffer = (PWSTR)ArApiSetOffset(Namespace, Namespace->Size,
					Found->NameOffset, Found->HashedLength);
				Candidate.Length = (USHORT)Found->HashedLength;
				Candidate.MaximumLength = Candidate.Length;
				if (Candidate.Buffer && ArUnicodeEquals(ApiNameToResolve, &Candidate)) {
					return Found;
				}
			}
			return NULL;
		}
	}
	return NULL;
	} AR_EXCEPT {
		return NULL;
	}
}

PAPI_SET_DATA_ENTRY
ApiSetpSearchForApiSetHost(
	IN PAPI_SET_DATA DataEntry,
	IN PCUNICODE_STRING ParentName,
	IN PAPI_SET_INFO ApiSetMap
	)
{
	PAR_API_SET_DATA Data = (PAR_API_SET_DATA)DataEntry;
	PAR_API_SET_DATA_ENTRY Entries = NULL;
	PAR_API_SET_DATA_ENTRY Entry = NULL;
	UNICODE_STRING Candidate = {0};
	ULONG Length = 0;
	ULONG Index = 0;

	AR_TRY {
	if (!Data || !ParentName || !ParentName->Buffer || !ApiSetMap ||
		Data->Count <= 1 || Data->Count > API_RESOLVER_MAX_APISET_ENTRIES) {
		return NULL;
	}
	if (!ArApiSetIndexedOffset(FIELD_OFFSET(AR_API_SET_DATA, DataEntry),
		Data->Count, sizeof(*Entries), &Length)) {
		return NULL;
	}
	Entries = (PAR_API_SET_DATA_ENTRY)ArApiSetOffset(Data, 0,
		FIELD_OFFSET(AR_API_SET_DATA, DataEntry), Length -
		FIELD_OFFSET(AR_API_SET_DATA, DataEntry));
	if (!Entries) {
		return NULL;
	}
	for (Index = 1; Index < Data->Count; ++Index) {
		Entry = &Entries[Index];
		if (!Entry->ImportNameOffset || Entry->ImportNameLength > 0xFFFEUL ||
			(Entry->ImportNameLength & (sizeof(WCHAR) - 1))) {
			continue;
		}
		Candidate.Buffer = (PWSTR)ArApiSetOffset(ApiSetMap, 0,
			Entry->ImportNameOffset, Entry->ImportNameLength);
		if (!Candidate.Buffer) {
			continue;
		}
		Candidate.Length = (USHORT)Entry->ImportNameLength;
		Candidate.MaximumLength = Candidate.Length;
		if (ArUnicodeEquals(ParentName, &Candidate)) {
			return Entry;
		}
	}
	return NULL;
	} AR_EXCEPT {
		return NULL;
	}
}

PAPI_SET_VALUE_ENTRY
ApiSetpSearchForApiSetHostEx(
	IN PAPI_SET_NAMESPACE_ENTRY ApiNamespaceEntry,
	IN PUNICODE_STRING ParentName,
	IN PAPI_SET_NAMESPACE ApiNamespace
	)
{
	PAR_API_SET_NAMESPACE_ENTRY NamespaceEntry =
		(PAR_API_SET_NAMESPACE_ENTRY)ApiNamespaceEntry;
	PAR_API_SET_NAMESPACE Namespace = (PAR_API_SET_NAMESPACE)ApiNamespace;
	PAR_API_SET_VALUE_ENTRY Entries = NULL;
	PAR_API_SET_VALUE_ENTRY Entry = NULL;
	PAR_API_SET_VALUE_ENTRY DefaultEntry = NULL;
	UNICODE_STRING Candidate = {0};
	ULONG Index = 0;
	ULONG Length = 0;

	AR_TRY {
	if (!NamespaceEntry || !Namespace || Namespace->Size < sizeof(*Namespace) ||
		!NamespaceEntry->ValueCount ||
		NamespaceEntry->ValueCount > API_RESOLVER_MAX_APISET_ENTRIES ||
		(ParentName && !ParentName->Buffer)) {
		return NULL;
	}
	if (!ArApiSetIndexedOffset(0, NamespaceEntry->ValueCount,
		sizeof(*Entries), &Length)) {
		return NULL;
	}
	Entries = (PAR_API_SET_VALUE_ENTRY)ArApiSetOffset(Namespace,
		Namespace->Size, NamespaceEntry->ValueOffset, Length);
	if (!Entries) {
		return NULL;
	}
	for (Index = 0; Index < NamespaceEntry->ValueCount; ++Index) {
		Entry = &Entries[Index];
		if (!Entry->NameLength) {
			/* An empty alias is the default host when no parent-specific
			 * implementation is present. */
			if (!DefaultEntry) {
				DefaultEntry = Entry;
			}
			continue;
		}
		if (Entry->NameLength > 0xFFFEUL ||
			(Entry->NameLength & (sizeof(WCHAR) - 1)) || !Entry->NameOffset) {
			continue;
		}
		Candidate.Buffer = (PWSTR)ArApiSetOffset(Namespace, Namespace->Size,
			Entry->NameOffset, Entry->NameLength);
		Candidate.Length = (USHORT)Entry->NameLength;
		Candidate.MaximumLength = Candidate.Length;
		if (ParentName && Candidate.Buffer &&
			ArUnicodeEquals(ParentName, &Candidate)) {
			return Entry;
		}
	}
	return DefaultEntry;
	} AR_EXCEPT {
		return NULL;
	}
}

static BOOLEAN
ArBuildApiSetContractName(
	IN PCUNICODE_STRING Input,
	OUT PUNICODE_STRING Contract
	)
{
	USHORT Count = 0;

	if (!Input || !Input->Buffer || !Contract || Input->Length < 8 ||
		(Input->Length & (sizeof(WCHAR) - 1))) {
		return FALSE;
	}
	Count = (USHORT)(Input->Length / sizeof(WCHAR));
	/* Forwarder syntax supplies a trailing dot ("contract.").  The API Set
	 * namespace stores the contract without that separator. */
	if (Count && Input->Buffer[Count - 1] == L'.') {
		--Count;
	}
	if (Count < 8) {
		return FALSE;
	}
	if (!(((Input->Buffer[0] == L'a' || Input->Buffer[0] == L'A') &&
		(Input->Buffer[1] == L'p' || Input->Buffer[1] == L'P') &&
		(Input->Buffer[2] == L'i' || Input->Buffer[2] == L'I') &&
		Input->Buffer[3] == L'-') ||
		((Input->Buffer[0] == L'e' || Input->Buffer[0] == L'E') &&
		(Input->Buffer[1] == L'x' || Input->Buffer[1] == L'X') &&
		(Input->Buffer[2] == L't' || Input->Buffer[2] == L'T') &&
		Input->Buffer[3] == L'-'))) {
		return FALSE;
	}
	Contract->Buffer = Input->Buffer;
	Contract->Length = (USHORT)(Count * sizeof(WCHAR));
	Contract->MaximumLength = Contract->Length;
	return TRUE;
}

NTSTATUS
ApiSetResolveToHostEx(
	IN PVOID ApiSetMap,
	IN PUNICODE_STRING ApiToResolve,
	IN PUNICODE_STRING ParentName OPTIONAL,
	OUT PUNICODE_STRING OutputName
	)
{
	PAR_API_SET_NAMESPACE Namespace = (PAR_API_SET_NAMESPACE)ApiSetMap;
	PAR_API_SET_NAMESPACE_ENTRY NamespaceEntry = NULL;
	PAR_API_SET_VALUE_ENTRY ValueEntry = NULL;
	UNICODE_STRING Contract = {0};

	AR_TRY {
	if (!ApiSetMap || !ApiToResolve || !ApiToResolve->Buffer || !OutputName) {
		return STATUS_INVALID_PARAMETER;
	}
	OutputName->Buffer = NULL;
	OutputName->Length = 0;
	OutputName->MaximumLength = 0;
	if (Namespace->Version < 4 || Namespace->Size < sizeof(*Namespace)) {
		return STATUS_NOT_FOUND;
	}
	if (!ArBuildApiSetContractName(ApiToResolve, &Contract)) {
		return STATUS_NOT_FOUND;
	}
	NamespaceEntry = ApiSetpSearchForApiSet(Namespace, &Contract);
	if (!NamespaceEntry || !NamespaceEntry->ValueCount) {
		return STATUS_NOT_FOUND;
	}
	ValueEntry = ApiSetpSearchForApiSetHostEx(NamespaceEntry, ParentName,
		Namespace);
	if (!ValueEntry) {
		return STATUS_NOT_FOUND;
	}
	if (!ValueEntry->ValueLength ||
		ValueEntry->ValueLength > 0xFFFEUL ||
		(ValueEntry->ValueLength & (sizeof(WCHAR) - 1))) {
		return STATUS_NOT_FOUND;
	}
	OutputName->Buffer = (PWSTR)ArApiSetOffset(Namespace, Namespace->Size,
		ValueEntry->ValueOffset, ValueEntry->ValueLength);
	if (!OutputName->Buffer) {
		return STATUS_NOT_FOUND;
	}
	OutputName->Length = (USHORT)ValueEntry->ValueLength;
	OutputName->MaximumLength = OutputName->Length;
	return STATUS_SUCCESS;
	} AR_EXCEPT {
		return STATUS_INVALID_PARAMETER;
	}
}

NTSTATUS
ApiSetResolveToHost(
	IN PVOID ApiSetMap,
	IN PUNICODE_STRING ApiToResolve,
	IN PUNICODE_STRING ParentName OPTIONAL,
	OUT PUNICODE_STRING OutputName
	)
{
	PAR_API_SET_INFO Map = (PAR_API_SET_INFO)ApiSetMap;
	PAR_API_SET_ENTRY Entry = NULL;
	PAR_API_SET_DATA Data = NULL;
	PAR_API_SET_DATA_ENTRY Host = NULL;
	UNICODE_STRING Contract = {0};
	UNICODE_STRING LegacyContract = {0};
	UNICODE_STRING Candidate = {0};
	ULONG Offset = 0;
	ULONG EntriesLength = 0;
	ULONG Index = 0;

	AR_TRY {
	if (!Map || !ApiToResolve || !ApiToResolve->Buffer || !OutputName) {
		return STATUS_INVALID_PARAMETER;
	}
	OutputName->Buffer = NULL;
	OutputName->Length = 0;
	OutputName->MaximumLength = 0;
	if (Map->Version >= 4) {
		return STATUS_NOT_FOUND;
	}
	if (!ArBuildApiSetContractName(ApiToResolve, &Contract) || !Map->Count) {
		return STATUS_NOT_FOUND;
	}
	if (Contract.Length <= 8) {
		return STATUS_NOT_FOUND;
	}
	/* Windows 7 stores the same key without the leading "API-" prefix. */
	LegacyContract = Contract;
	LegacyContract.Buffer += 4;
	LegacyContract.Length = (USHORT)(Contract.Length - 8);
	LegacyContract.MaximumLength = LegacyContract.Length;

	/* The legacy table is sorted by contract name; a linear scan is safer for
	 * malformed maps and the table is small in practice. */
	if (Map->Count > API_RESOLVER_MAX_APISET_ENTRIES ||
		!ArApiSetIndexedOffset(FIELD_OFFSET(AR_API_SET_INFO, ApiSetEntry),
		Map->Count, sizeof(*Entry), &EntriesLength)) {
		return STATUS_NOT_FOUND;
	}
	for (Index = 0; Index < Map->Count; ++Index) {
		if (!ArApiSetIndexedOffset(FIELD_OFFSET(AR_API_SET_INFO, ApiSetEntry),
			Index, sizeof(*Entry), &Offset)) {
			return STATUS_NOT_FOUND;
		}
		Entry = (PAR_API_SET_ENTRY)ArApiSetOffset(Map, 0, Offset, sizeof(*Entry));
		if (!Entry || !Entry->NameOffset || !Entry->NameLength ||
			Entry->NameLength > 0xFFFEUL ||
			(Entry->NameLength & (sizeof(WCHAR) - 1))) {
			continue;
		}
		Candidate.Buffer = (PWSTR)ArApiSetOffset(Map, 0, Entry->NameOffset,
			Entry->NameLength);
		if (!Candidate.Buffer) {
			continue;
		}
		Candidate.Length = (USHORT)Entry->NameLength;
		Candidate.MaximumLength = Candidate.Length;
		if (ArUnicodeEquals(&LegacyContract, &Candidate) ||
			ArUnicodeEquals(&Contract, &Candidate)) {
			Data = (PAR_API_SET_DATA)ArApiSetOffset(Map, 0,
				Entry->DataOffset, sizeof(ULONG));
			break;
		}
	}
	if (!Data || !Data->Count) {
		return STATUS_NOT_FOUND;
	}
	if (!ArApiSetOffset(Data, 0, FIELD_OFFSET(AR_API_SET_DATA, DataEntry),
		sizeof(*Host))) {
		return STATUS_NOT_FOUND;
	}
	Host = (PAR_API_SET_DATA_ENTRY)ArApiSetOffset(Data, 0,
		FIELD_OFFSET(AR_API_SET_DATA, DataEntry), sizeof(*Host));
	if (ParentName && Data->Count > 1) {
		PAR_API_SET_DATA_ENTRY Named = ApiSetpSearchForApiSetHost(Data,
			ParentName, Map);
		if (Named) {
			Host = Named;
		}
	}
	if (!Host->NameLength || Host->NameLength > 0xFFFEUL) {
		return STATUS_NOT_FOUND;
	}
	if (!Host->NameOffset || (Host->NameLength & (sizeof(WCHAR) - 1))) {
		return STATUS_NOT_FOUND;
	}
	OutputName->Buffer = (PWSTR)ArApiSetOffset(Map, 0, Host->NameOffset,
		Host->NameLength);
	if (!OutputName->Buffer) {
		return STATUS_NOT_FOUND;
	}
	OutputName->Length = (USHORT)Host->NameLength;
	OutputName->MaximumLength = OutputName->Length;
	return STATUS_SUCCESS;
	} AR_EXCEPT {
		return STATUS_INVALID_PARAMETER;
	}
}

static ULONG_PTR
ArResolveForwarder(
	IN PIMAGE_NT_HEADERS NtHeaders,
	IN PCSTR ForwarderName,
	IN ULONG ForwardDepth
	);

/* --------------------------- Export table walk -------------------------- */

static ULONG_PTR
ArGetFunAddressInternal(
	IN PVOID Base,
	IN PCSTR FunctionName,
	IN ULONG ForwardDepth
	);

static PIMAGE_NT_HEADERS
ArGetNtHeaders(
	IN PVOID Base
	)
{
#ifdef _KERNEL_MODE
	/* The WDK helper also handles the architecture-specific header layout. */
	return Base ? RtlImageNtHeader(Base) : NULL;
#else
	PIMAGE_DOS_HEADER DosHeader = NULL;
	PIMAGE_NT_HEADERS NtHeaders = NULL;

	if (!Base) {
		return NULL;
	}
	DosHeader = (PIMAGE_DOS_HEADER)Base;
#ifndef _KERNEL_MODE
	if (!ArUserMemoryIsReadable(DosHeader, sizeof(*DosHeader))) {
		return NULL;
	}
#endif
	if (DosHeader->e_magic != IMAGE_DOS_SIGNATURE || DosHeader->e_lfanew < 0 ||
		(ULONG_PTR)Base > (ULONG_PTR)-1 - (ULONG_PTR)DosHeader->e_lfanew) {
		return NULL;
	}
	NtHeaders = (PIMAGE_NT_HEADERS)((PUCHAR)Base + DosHeader->e_lfanew);
#ifndef _KERNEL_MODE
	if (!ArUserMemoryIsReadable(NtHeaders, sizeof(*NtHeaders))) {
		return NULL;
	}
#endif
	return NtHeaders->Signature == IMAGE_NT_SIGNATURE ? NtHeaders : NULL;
#endif
}

static BOOLEAN
ArGetExportDirectory(
	IN PVOID Base,
	IN PIMAGE_NT_HEADERS NtHeaders,
	OUT PIMAGE_EXPORT_DIRECTORY *Export,
	OUT PULONG ExportRva,
	OUT PULONG ExportSize
	)
{
	IMAGE_DATA_DIRECTORY Directory = {0};
	ULONG RequiredHeaderSize = 0;
	ULONG SizeOfImage = 0;

	if (!Base || !NtHeaders || !Export || !ExportRva || !ExportSize ||
		NtHeaders->Signature != IMAGE_NT_SIGNATURE ||
		NtHeaders->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC) {
		return FALSE;
	}
	*Export = NULL;
	*ExportRva = 0;
	*ExportSize = 0;
	RequiredHeaderSize = FIELD_OFFSET(IMAGE_OPTIONAL_HEADER, DataDirectory) +
		(IMAGE_DIRECTORY_ENTRY_EXPORT + 1) * sizeof(IMAGE_DATA_DIRECTORY);
	if (NtHeaders->FileHeader.SizeOfOptionalHeader < RequiredHeaderSize ||
		NtHeaders->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) {
		return FALSE;
	}
	SizeOfImage = NtHeaders->OptionalHeader.SizeOfImage;
	Directory = NtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
	if (!Directory.VirtualAddress || Directory.Size < sizeof(IMAGE_EXPORT_DIRECTORY) ||
		!ArRangeInImage(SizeOfImage, Directory.VirtualAddress, Directory.Size)) {
		return FALSE;
	}
	*Export = (PIMAGE_EXPORT_DIRECTORY)ArAddressFromRva(Base, SizeOfImage,
		Directory.VirtualAddress, Directory.Size);
	if (!*Export) {
		return FALSE;
	}
	*ExportRva = Directory.VirtualAddress;
	*ExportSize = Directory.Size;
	return TRUE;
}

static ULONG_PTR
ArResolveExportRva(
	IN PVOID Base,
	IN PIMAGE_NT_HEADERS NtHeaders,
	IN PIMAGE_EXPORT_DIRECTORY Export,
	IN ULONG ExportRva,
	IN ULONG ExportSize,
	IN ULONG FunctionRva,
	IN ULONG ForwardDepth
	)
{
	ULONG SizeOfImage = 0;
	ULONG Offset = 0;
	LPSTR Forwarder = NULL;

	if (!Base || !NtHeaders || !Export || !ExportRva || !ExportSize ||
		!FunctionRva) {
		return 0;
	}
	SizeOfImage = NtHeaders->OptionalHeader.SizeOfImage;
	if (!ArAddressFromRva(Base, SizeOfImage, FunctionRva, 1)) {
		return 0;
	}
	if (FunctionRva >= ExportRva && FunctionRva - ExportRva < ExportSize) {
		/* A function RVA inside the export directory is the PE forwarder
		 * string "module.symbol" (or "module.#ordinal"), not executable code. */
		if (ForwardDepth >= API_RESOLVER_MAX_FORWARD_DEPTH) {
			return 0;
		}
		Offset = FunctionRva - ExportRva;
		Forwarder = (LPSTR)ArAddressFromRva(Base, SizeOfImage, FunctionRva,
			ExportSize - Offset);
		if (!Forwarder) {
			return 0;
		}
		while (Offset < ExportSize) {
			if (!Forwarder[Offset - (FunctionRva - ExportRva)]) {
				return ArResolveForwarder(NtHeaders, Forwarder, ForwardDepth + 1);
			}
			++Offset;
		}
		return 0;
	}
	return (ULONG_PTR)ArAddressFromRva(Base, SizeOfImage, FunctionRva, 1);
}

static ULONG_PTR
ArResolveOrdinalInternal(
	IN PVOID Base,
	IN ULONG_PTR Ordinal,
	IN ULONG ForwardDepth
	)
{
	PIMAGE_NT_HEADERS NtHeaders = NULL;
	PIMAGE_EXPORT_DIRECTORY Export = NULL;
	PULONG Functions = NULL;
	ULONG ExportRva = 0;
	ULONG ExportSize = 0;
	ULONG Index = 0;

	AR_TRY {
		if (!Base || ForwardDepth > API_RESOLVER_MAX_FORWARD_DEPTH) {
			return 0;
		}
		NtHeaders = ArGetNtHeaders(Base);
		if (!NtHeaders || !ArGetExportDirectory(Base, NtHeaders, &Export,
			&ExportRva, &ExportSize) ||
			Export->NumberOfFunctions > API_RESOLVER_MAX_EXPORT_ENTRIES ||
			!Export->AddressOfFunctions ||
			Ordinal < Export->Base ||
			Ordinal - Export->Base >= Export->NumberOfFunctions) {
			return 0;
		}
		Functions = (PULONG)ArArrayFromRva(Base, NtHeaders->OptionalHeader.SizeOfImage,
			Export->AddressOfFunctions, Export->NumberOfFunctions, sizeof(ULONG));
		if (!Functions) {
			return 0;
		}
		Index = (ULONG)(Ordinal - Export->Base);
		/* AddressOfFunctions is indexed by ordinal minus the export base. */
		return ArResolveExportRva(Base, NtHeaders, Export, ExportRva, ExportSize,
			Functions[Index], ForwardDepth);
	} AR_EXCEPT {
		return 0;
	}
}

/* Resolve an export by its ordinal value (not a zero-based function index). */
ULONG_PTR
GetFunAddressByOriginal(
	IN PVOID Base,
	IN ULONG_PTR Original
	)
{
	return ArResolveOrdinalInternal(Base, Original, 0);
}

static ULONG_PTR
ArResolveForwarder(
	IN PIMAGE_NT_HEADERS NtHeaders,
	IN PCSTR ForwarderName,
	IN ULONG ForwardDepth
	)
{
#ifndef _KERNEL_MODE
	HMODULE Module = NULL;
	WCHAR ModuleBuffer[API_RESOLVER_MAX_MODULE_NAME] = {0};
	ULONG Length = 0;
	ULONG DotOffset = 0;
	ULONG ModuleLength = 0;
	ULONG Index = 0;
	(void)ArMemorySet(ModuleBuffer, 0, sizeof(ModuleBuffer));

	/* User mode cannot safely walk another process's loader lists here.  The
	 * documented loader query is sufficient for already-loaded forwarder
	 * targets and does not add a reference or trigger DLL initialization. */
	if (!NtHeaders || !ForwarderName ||
		ForwardDepth > API_RESOLVER_MAX_FORWARD_DEPTH) {
		return 0;
	}
	AR_TRY {
		while (Length < API_RESOLVER_MAX_FORWARDER_LENGTH &&
			ForwarderName[Length]) {
			++Length;
		}
		if (!Length || Length == API_RESOLVER_MAX_FORWARDER_LENGTH) {
			return 0;
		}

		/* A forwarder is "module.export".  Module names in PE forwarders
		 * conventionally omit the .dll suffix; the first dot is the separator. */
		for (Index = 0; Index < Length; ++Index) {
			if (ForwarderName[Index] == '.') {
				DotOffset = Index;
				break;
			}
		}
		if (!DotOffset || DotOffset + 1 >= Length ||
			DotOffset >= API_RESOLVER_MAX_MODULE_NAME - 1) {
			return 0;
		}

		ModuleLength = DotOffset;
		for (Index = 0; Index < ModuleLength; ++Index) {
			/* PE export names are ASCII.  Reject high bytes instead of silently
			 * producing a different Unicode module name. */
			if ((UCHAR)ForwarderName[Index] >= 0x80U) {
				return 0;
			}
			ModuleBuffer[Index] = (WCHAR)(UCHAR)ForwarderName[Index];
		}
		ModuleBuffer[ModuleLength] = L'\0';

		/* The Windows loader also applies API Set contract redirection here. */
		Module = GetModuleHandleW(ModuleBuffer);
		if (!Module) {
			/* GetModuleHandleW normally supplies the .dll default, but retrying
			 * explicitly also covers hosts that require an extension. */
			if (ModuleLength + 4 >= API_RESOLVER_MAX_MODULE_NAME) {
				return 0;
			}
			ModuleBuffer[ModuleLength++] = L'.';
			ModuleBuffer[ModuleLength++] = L'd';
			ModuleBuffer[ModuleLength++] = L'l';
			ModuleBuffer[ModuleLength++] = L'l';
			ModuleBuffer[ModuleLength] = L'\0';
			Module = GetModuleHandleW(ModuleBuffer);
		}
		if (!Module) {
			return 0;
		}

		return ArGetFunAddressInternal((PVOID)Module,
			ForwarderName + DotOffset + 1, ForwardDepth);
	} AR_EXCEPT {
		return 0;
	}
#else
	ULONG Length = 0;
	ULONG DotOffset = 0;
	ULONG ModuleLength = 0;
	ULONG Index = 0;
	CHAR ModuleBuffer[API_RESOLVER_MAX_MODULE_NAME] = {0};
	WCHAR UnicodeBuffer[API_RESOLVER_MAX_MODULE_NAME] = {0};
	UNICODE_STRING ModuleName = {0};
	UNICODE_STRING HostName = {0};
	PVOID ApiSetMap = NULL;
	PKLDR_DATA_TABLE_ENTRY SystemEntry = NULL;
	PLDR_DATA_TABLE_ENTRY ProcessEntry = NULL;
	PPEB Peb = NULL;
	NTSTATUS Status = STATUS_NOT_FOUND;
	ULONG_PTR Result = 0;
	(void)ArMemorySet(ModuleBuffer, 0, sizeof(ModuleBuffer));
	(void)ArMemorySet(UnicodeBuffer, 0, sizeof(UnicodeBuffer));

	if (!NtHeaders || !ForwarderName || ForwardDepth > API_RESOLVER_MAX_FORWARD_DEPTH) {
		return 0;
	}
	AR_TRY {
		while (Length < API_RESOLVER_MAX_FORWARDER_LENGTH && ForwarderName[Length]) {
			++Length;
		}
		if (!Length || Length == API_RESOLVER_MAX_FORWARDER_LENGTH) {
			return 0;
		}
		for (Index = 0; Index < Length; ++Index) {
			if (ForwarderName[Index] == '.') {
				DotOffset = Index;
				break;
			}
		}
		if (!DotOffset || DotOffset + 1 >= Length ||
			DotOffset + 1 >= API_RESOLVER_MAX_MODULE_NAME) {
			return 0;
		}
		ModuleLength = DotOffset + 1;
		(void)ArMemoryCopy(ModuleBuffer, ForwarderName, ModuleLength);
		ModuleBuffer[ModuleLength] = '\0';
		for (Index = 0; Index < ModuleLength; ++Index) {
			if ((UCHAR)ModuleBuffer[Index] >= 0x80U) {
				return 0;
			}
			UnicodeBuffer[Index] = (WCHAR)(UCHAR)ModuleBuffer[Index];
		}
		ModuleName.Buffer = UnicodeBuffer;
		ModuleName.Length = (USHORT)(ModuleLength * sizeof(WCHAR));
		ModuleName.MaximumLength = sizeof(UnicodeBuffer);

		if (NtHeaders->OptionalHeader.Subsystem == IMAGE_SUBSYSTEM_NATIVE) {
			/* Drivers can forward to ntoskrnl or another loaded system image.
			 * Resolve both through the loader list so no project-specific global
			 * (such as a cached ntoskrnl entry) is required. */
			Status = ArFindSystemModuleByName(&ModuleName, &SystemEntry);
			if (NT_SUCCESS(Status) && SystemEntry) {
				Result = ArGetFunAddressInternal(SystemEntry->DllBase,
					ForwarderName + DotOffset + 1, ForwardDepth);
			}
		}
		else {
			Peb = PsGetProcessPeb(PsGetCurrentProcess());
			if (Peb) {
				ApiSetMap = Peb->ApiSetMap;
			}
			Status = GetProcessModuleDataEntry((PVOID)NtHeaders, &ProcessEntry);
			if (NT_SUCCESS(Status) && ProcessEntry) {
				Status = ApiSetResolveToHostEx(ApiSetMap, &ModuleName,
					&ProcessEntry->BaseDllName, &HostName);
				/* Windows 7/8 use the legacy API Set map.  Try it when the
				 * modern namespace lookup is unavailable or did not find a host. */
				if (!NT_SUCCESS(Status)) {
					Status = ApiSetResolveToHost(ApiSetMap, &ModuleName,
						&ProcessEntry->BaseDllName, &HostName);
				}
				if (NT_SUCCESS(Status)) {
					ProcessEntry = NULL;
					Status = GetProcessModuleDataEntryByName(&HostName, &ProcessEntry);
				}
				if (!NT_SUCCESS(Status)) {
					ProcessEntry = NULL;
					Status = GetProcessModuleDataEntryByName2(&ModuleName, &ProcessEntry);
				}
				if (NT_SUCCESS(Status) && ProcessEntry) {
					Result = ArGetFunAddressInternal(ProcessEntry->DllBase,
						ForwarderName + DotOffset + 1, ForwardDepth);
				}
			}
		}
	} AR_EXCEPT {
		Result = 0;
	}
	return Result;
#endif
}

static ULONG_PTR
ArGetFunAddressInternal(
	IN PVOID Base,
	IN PCSTR FunctionName,
	IN ULONG ForwardDepth
	)
{
	PIMAGE_NT_HEADERS NtHeaders = NULL;
	PIMAGE_EXPORT_DIRECTORY Export = NULL;
	PULONG Functions = NULL;
	PULONG Names = NULL;
	PUSHORT NameOrdinals = NULL;
	LPSTR ExportName = NULL;
	ULONG ExportRva = 0;
	ULONG ExportSize = 0;
	ULONG Index = 0;
	ULONG_PTR Ordinal = 0;

	if (!Base || !FunctionName || ForwardDepth > API_RESOLVER_MAX_FORWARD_DEPTH) {
		return 0;
	}
#ifndef _KERNEL_MODE
	if (!ArUserMemoryIsReadable((PVOID)FunctionName, 1)) {
		return 0;
	}
#endif
	AR_TRY {
		if (FunctionName[0] == '#') {
			if (!ArParseOrdinal(FunctionName, &Ordinal)) {
				return 0;
			}
			return ArResolveOrdinalInternal(Base, Ordinal, ForwardDepth);
		}
		NtHeaders = ArGetNtHeaders(Base);
		if (!NtHeaders || !ArGetExportDirectory(Base, NtHeaders, &Export,
			&ExportRva, &ExportSize) || !Export->NumberOfFunctions ||
			!Export->NumberOfNames ||
			Export->NumberOfFunctions > API_RESOLVER_MAX_EXPORT_ENTRIES ||
			Export->NumberOfNames > API_RESOLVER_MAX_EXPORT_ENTRIES ||
			!Export->AddressOfFunctions || !Export->AddressOfNames ||
			!Export->AddressOfNameOrdinals) {
			return 0;
		}
		Functions = (PULONG)ArArrayFromRva(Base, NtHeaders->OptionalHeader.SizeOfImage,
			Export->AddressOfFunctions, Export->NumberOfFunctions, sizeof(ULONG));
		Names = (PULONG)ArArrayFromRva(Base, NtHeaders->OptionalHeader.SizeOfImage,
			Export->AddressOfNames, Export->NumberOfNames, sizeof(ULONG));
		NameOrdinals = (PUSHORT)ArArrayFromRva(Base, NtHeaders->OptionalHeader.SizeOfImage,
			Export->AddressOfNameOrdinals, Export->NumberOfNames, sizeof(USHORT));
		if (!Functions || !Names || !NameOrdinals) {
			return 0;
		}
		for (Index = 0; Index < Export->NumberOfNames; ++Index) {
			if (Names[Index] >= NtHeaders->OptionalHeader.SizeOfImage) {
				continue;
			}
			ExportName = (LPSTR)ArAddressFromRva(Base,
				NtHeaders->OptionalHeader.SizeOfImage, Names[Index], 1);
			if (!ExportName || !ArAsciiEqualsExact(ExportName,
				NtHeaders->OptionalHeader.SizeOfImage - Names[Index], FunctionName)) {
				continue;
			}
			/* NameOrdinals maps a name to the function table; malformed PE files
			 * occasionally contain an out-of-range index here. */
			if (NameOrdinals[Index] >= Export->NumberOfFunctions) {
				return 0;
			}
			return ArResolveExportRva(Base, NtHeaders, Export, ExportRva, ExportSize,
				Functions[NameOrdinals[Index]], ForwardDepth);
		}
	} AR_EXCEPT {
		return 0;
	}
	return 0;
}

/* Resolve a forwarder string using the module that owns NtHeaders. */
ULONG_PTR
GetFunAddressReAnalysis(
	IN PIMAGE_NT_HEADERS NtHeaders,
	IN LPSTR lpszFunName
	)
{
	return ArResolveForwarder(NtHeaders, lpszFunName, 0);
}

/* Public entry point: equivalent to GetProcAddress for a mapped PE image. */
API_RESOLVER_API API_RESOLVER_ADDRESS API_RESOLVER_CALL
ApiResolver_GetFuncAddress(
	IN PVOID Base,
	IN PCSTR lpszFunName
	)
{
	return ArGetFunAddressInternal(Base, lpszFunName, 0);
}
