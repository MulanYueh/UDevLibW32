#ifndef OXDRV_KLIBC_H_INCLUDED
#define OXDRV_KLIBC_H_INCLUDED

#include <stddef.h>
#include <stdarg.h>
#include <wchar.h>

#include "stdint2.h"

#ifndef LIBC_CALL
#if defined(_MSC_VER)
#define LIBC_CALL __cdecl
#elif defined(__i386__) && defined(_WIN32) && \
      (defined(__GNUC__) || defined(__clang__))
#define LIBC_CALL __attribute__((__cdecl__))
#else
#define LIBC_CALL
#endif
#endif

/* Comparator types use the library ABI so callbacks remain valid on x86. */
typedef int (LIBC_CALL *libc_compare_fn)(const void* left, const void* right);
typedef int (LIBC_CALL *libc_compare_s_fn)(void* context,
                                           const void* left,
                                           const void* right);

#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif

#ifndef STRUNCATE
#define STRUNCATE 80
#endif

/* Build options:
 *   LIBC_KERNEL_MODE      Build without the user-mode errno dependency.
 *   LIBC_NO_ERRNO         Also disable errno for freestanding user builds.
 *   LIBC_SET_ERRNO(err)   Route error updates to a caller-provided hook.
 *   LIBC_ENABLE_FLOATING_POINT
 *                         Keep the optional floating-point conversions in a
 *                         kernel build; they are disabled there by default.
 * LIBC_WCHAR_IS_UTF16 / LIBC_WCHAR_IS_UTF32
 *                         Override wchar_t's encoding when the compiler does
 *                         not expose a reliable width macro.
 * LIBC_KERNEL_MODE avoids defining Microsoft's reserved _KERNEL_MODE macro. */
#if defined(_KERNEL_MODE) || defined(LIBC_KERNEL_MODE)
#define LIBC_BUILD_KERNEL
#endif

#if defined(LIBC_BUILD_KERNEL) && !defined(LIBC_ENABLE_FLOATING_POINT) && \
    !defined(LIBC_NO_FLOATING_POINT)
#define LIBC_NO_FLOATING_POINT
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    /*==========================================================================*/
    /* Character Classification Functions                                       */
    /* These functions classify characters based on their properties            */
    /*==========================================================================*/

    /**
     * @brief Check if character is a whitespace character
     * @param ch Character to check
     * @return Non-zero if character is whitespace, 0 otherwise
     */
    int    LIBC_CALL libc_isspace(int ch);

    /**
     * @brief Check if character is a decimal digit (0-9)
     * @param c Character to check
     * @return Non-zero if character is a digit, 0 otherwise
     */
    int    LIBC_CALL libc_isdigit(int c);

    /**
     * @brief Check if character is an alphabetic letter (a-z, A-Z)
     * @param c Character to check
     * @return Non-zero if character is alphabetic, 0 otherwise
     */
    int    LIBC_CALL libc_isalpha(int c);

    /**
     * @brief Check if character is an uppercase letter (A-Z)
     * @param c Character to check
     * @return Non-zero if character is uppercase, 0 otherwise
     */
    int    LIBC_CALL libc_isupper(int c);

    /**
     * @brief Check if character is a lowercase letter (a-z)
     * @param c Character to check
     * @return Non-zero if character is lowercase, 0 otherwise
     */
    int    LIBC_CALL libc_islower(int c);

    /**
     * @brief Check if character is a control character (0x00-0x1F, 0x7F)
     * @param c Character to check
     * @return Non-zero if character is a control character, 0 otherwise
     */
    int    LIBC_CALL libc_iscntrl(int c);

    /**
     * @brief Check if character is a printable character excluding space (0x21-0x7E)
     * @param c Character to check
     * @return Non-zero if character is a graphic character, 0 otherwise
     */
    int    LIBC_CALL libc_isgraph(int c);

    /**
     * @brief Check if character is a printable character including space (0x20-0x7E)
     * @param c Character to check
     * @return Non-zero if character is printable, 0 otherwise
     */
    int    LIBC_CALL libc_isprint(int c);

    /**
     * @brief Check if character is a punctuation character
     * @param c Character to check
     * @return Non-zero if character is punctuation, 0 otherwise
     */
    int    LIBC_CALL libc_ispunct(int c);

    /**
     * @brief Convert character to uppercase
     * @param c Character to convert
     * @return Uppercase version of character, or original if not lowercase
     */
    int    LIBC_CALL libc_toupper(int c);

    /**
     * @brief Convert character to lowercase
     * @param c Character to convert
     * @return Lowercase version of character, or original if not uppercase
     */
    int    LIBC_CALL libc_tolower(int c);

    /**
     * @brief Check if character is alphanumeric (a-z, A-Z, 0-9)
     * @param c Character to check
     * @return Non-zero if character is alphanumeric, 0 otherwise
     */
    int    LIBC_CALL libc_isalnum(int c);

    /**
     * @brief Check if character is an ASCII character (0-127)
     * @param c Character to check
     * @return Non-zero if character is ASCII, 0 otherwise
     */
    int    LIBC_CALL libc_isascii(int c);

    /**
     * @brief Check if character is a hexadecimal digit
     * @param c Character to check
     * @return Non-zero for 0-9, a-f, or A-F
     */
    int    LIBC_CALL libc_isxdigit(int c);

    /**
     * @brief Check if character is a horizontal whitespace character
     * @param c Character to check
     * @return Non-zero for space or tab
     */
    int    LIBC_CALL libc_isblank(int c);

    /*==========================================================================*/
    /* Wide Character Classification Functions                                  */
    /* These functions classify wide characters based on their properties       */
    /*==========================================================================*/

    /**
     * @brief Check if wide character is uppercase
     * @param c Wide character to check
     * @return Non-zero if character is uppercase, 0 otherwise
     */
    int    LIBC_CALL libc_iswupper(wchar_t c);

    /**
     * @brief Check if wide character is lowercase
     * @param c Wide character to check
     * @return Non-zero if character is lowercase, 0 otherwise
     */
    int    LIBC_CALL libc_iswlower(wchar_t c);

    /**
     * @brief Check if wide character is alphabetic
     * @param c Wide character to check
     * @return Non-zero if character is alphabetic, 0 otherwise
     */
    int    LIBC_CALL libc_iswalpha(wchar_t c);

    /**
     * @brief Check if wide character is alphanumeric
     * @param c Wide character to check
     * @return Non-zero if character is alphanumeric, 0 otherwise
     */
    int    LIBC_CALL libc_iswalnum(wchar_t c);

    /**
     * @brief Check if wide character is a control character
     * @param c Wide character to check
     * @return Non-zero if character is a control character, 0 otherwise
     */
    int    LIBC_CALL libc_iswcntrl(wchar_t c);

    /**
     * @brief Check if wide character is a graphic character
     * @param c Wide character to check
     * @return Non-zero if character is graphic, 0 otherwise
     */
    int    LIBC_CALL libc_iswgraph(wchar_t c);

    /**
     * @brief Check if wide character is printable
     * @param c Wide character to check
     * @return Non-zero if character is printable, 0 otherwise
     */
    int    LIBC_CALL libc_iswprint(wchar_t c);

    /**
     * @brief Check if wide character is punctuation
     * @param c Wide character to check
     * @return Non-zero if character is punctuation, 0 otherwise
     */
    int    LIBC_CALL libc_iswpunct(wchar_t c);

    /**
     * @brief Check if wide character is a hexadecimal digit
     * @param c Wide character to check
     * @return Non-zero if character is a hex digit, 0 otherwise
     */
    int    LIBC_CALL libc_iswxdigit(wchar_t c);

    /**
     * @brief Check if wide character is whitespace
     * @param ch Wide character to check
     * @return Non-zero if character is whitespace, 0 otherwise
     */
    int    LIBC_CALL libc_iswspace(wchar_t ch);

    /**
     * @brief Check if wide character is a horizontal whitespace character
     * @param c Wide character to check
     * @return Non-zero for space or tab
     */
    int    LIBC_CALL libc_iswblank(wchar_t c);

    /**
     * @brief Check if wide character is a decimal digit
     * @param c Wide character to check
     * @return Non-zero if character is a digit, 0 otherwise
     */
    int    LIBC_CALL libc_iswdigit(wchar_t c);

    /**
     * @brief Convert wide character to lowercase
     * @param c Wide character to convert
     * @return Lowercase version of character, or original if not uppercase
     */
    wchar_t LIBC_CALL libc_towlower(wchar_t c);

    /**
     * @brief Convert wide character to uppercase
     * @param c Wide character to convert
     * @return Uppercase version of character, or original if not lowercase
     */
    wchar_t LIBC_CALL libc_towupper(wchar_t c);

    /**
     * @brief Extended wide character space check
     * @param c Wide character to check
     * @return Non-zero if character is space, 0 otherwise
     */
    int LIBC_CALL libc_iswspace_ext(wchar_t c);

    /*==========================================================================*/
    /* String Manipulation Functions                                            */
    /* These functions operate on null-terminated character strings             */
    /*==========================================================================*/

    /**
     * @brief Calculate the length of a string
     * @param s Pointer to the null-terminated string
     * @return Length of the string (excluding null terminator)
     */
    size_t   LIBC_CALL libc_strlen(const char* s);

    /**
     * @brief Concatenate two strings
     * @param s Destination string
     * @param t Source string to append
     * @return Pointer to the destination string
     */
    char* LIBC_CALL libc_strcat(char* s, const char* t);

    /**
     * @brief Safely copy a null-terminated string
     * @param dest Destination buffer
     * @param dest_size Destination capacity in bytes
     * @param src Source string
     * @return 0 on success, or an error code on failure
     */
    int LIBC_CALL libc_strcpy_s(char* dest, size_t dest_size, const char* src);

    /**
     * @brief Safely append a null-terminated string
     * @param dest Destination buffer containing a valid string
     * @param dest_size Destination capacity in bytes
     * @param src Source string
     * @return 0 on success, or an error code on failure
     */
    int LIBC_CALL libc_strcat_s(char* dest, size_t dest_size, const char* src);

    /**
     * @brief Find a string length up to a specified bound
     * @param s String to inspect; NULL is accepted
     * @param max_count Maximum number of characters to inspect
     * @return String length, or 0 for NULL/empty input
     */
    size_t LIBC_CALL libc_strnlen_s(const char* s, size_t max_count);

    /**
     * @brief Find first occurrence of character in string
     * @param t String to search
     * @param c Character to find
     * @return Pointer to first occurrence, or NULL if not found
     */
    char* LIBC_CALL libc_strchr(const char* t, int c);

    /**
     * @brief Find last occurrence of character in string
     * @param t String to search
     * @param c Character to find
     * @return Pointer to last occurrence, or NULL if not found
     */
    char* LIBC_CALL libc_strrchr(const char* t, int c);

    /**
     * @brief Compare two strings lexicographically
     * @param s First string
     * @param t Second string
     * @return 0 if equal, <0 if s<t, >0 if s>t
     */
    int      LIBC_CALL libc_strcmp(const char* s, const char* t);

    /**
     * @brief Copy a string
     * @param s Destination buffer
     * @param t Source string
     * @return Pointer to the destination string
     */
    char* LIBC_CALL libc_strcpy(char* s, const char* t);

    /**
     * @brief Copy a string with length limit
     * @param s Destination buffer
     * @param t Source string
     * @param n Maximum number of characters to copy
     * @return Pointer to the destination string
     */
    char* LIBC_CALL libc_strncpy(char* s, const char* t, size_t n);

    /**
     * @brief Compare two strings with length limit
     * @param s First string
     * @param t Second string
     * @param n Maximum number of characters to compare
     * @return 0 if equal, <0 if s<t, >0 if s>t
     */
    int   LIBC_CALL libc_strncmp(const char* s, const char* t, size_t n);

    /**
     * @brief Concatenate two strings with length limit
     * @param s Destination string
     * @param t Source string to append
     * @param n Maximum number of characters to append
     * @return Pointer to the destination string
     */
    char* LIBC_CALL libc_strncat(char* s, const char* t, size_t n);

    /**
     * @brief Safely copy at most count characters and always terminate
     * @param dest Destination buffer
     * @param dest_size Destination capacity in bytes
     * @param src Source string
     * @param count Maximum source characters to copy, or _TRUNCATE
     * @return 0 on success, or an error code on failure
     */
    int LIBC_CALL libc_strncpy_s(char* dest, size_t dest_size,
                       const char* src, size_t count);

    /**
     * @brief Safely append at most count characters and always terminate
     * @param dest Destination buffer containing a valid string
     * @param dest_size Destination capacity in bytes
     * @param src Source string
     * @param count Maximum source characters to append, or _TRUNCATE
     * @return 0 on success, or an error code on failure
     */
    int LIBC_CALL libc_strncat_s(char* dest, size_t dest_size,
                       const char* src, size_t count);

    /**
     * @brief Find first occurrence of substring
     * @param haystack String to search in
     * @param needle Substring to find
     * @return Pointer to first occurrence, or NULL if not found
     */
    char* LIBC_CALL libc_strstr(const char* haystack, const char* needle);

    /**
     * @brief Split string into tokens
     * @param str String to tokenize (NULL to continue with previous string)
     * @param delim Delimiter characters
     * @param saveptr Pointer to char* for internal state
     * @return Pointer to next token, or NULL if no more tokens
     */
    char* LIBC_CALL libc_strtok(char* str, const char* delim, char** saveptr);

    /**
     * @brief Context-based string tokenization using the Windows signature
     * @param str String to tokenize, or NULL to continue
     * @param delim Delimiter characters
     * @param context Tokenizer state
     * @return Pointer to the next token, or NULL
     */
    char* LIBC_CALL libc_strtok_s(char* str, const char* delim, char** context);

    /**
     * @brief Reverse a string in place
     * @param s String to reverse
     * @return Pointer to the reversed string
     */
    char* LIBC_CALL libc_strrev(char* s);

    /**
     * @brief Compare two strings case-insensitively
     * @param s First string
     * @param t Second string
     * @return 0 if equal, <0 if s<t, >0 if s>t
     */
    int      LIBC_CALL libc_stricmp(const char* s, const char* t);

    /**
     * @brief Count occurrences of a character in a string
     * @param s String to search
     * @param ch Character to count
     * @return Number of occurrences
     */
    size_t   LIBC_CALL libc_strcount(const char* s, char ch);

    /**
     * @brief Copy string with size limit (safer than strncpy)
     * @param dest Destination buffer
     * @param src Source string
     * @param size Size of destination buffer
     * @return Length of source string
     */
    size_t   LIBC_CALL libc_strlcpy(char* dest, const char* src, size_t size);

    /**
     * @brief Concatenate strings with size limit (safer than strncat)
     * @param dest Destination string
     * @param src Source string to append
     * @param size Size of destination buffer
     * @return Length of resulting string
     */
    size_t   LIBC_CALL libc_strlcat(char* dest, const char* src, size_t size);

    /**
     * @brief Convert string to lowercase
     * @param s String to convert (modified in place)
     * @return Pointer to the converted string
     */
    char* LIBC_CALL libc_strlwr(char* s);

    /**
     * @brief Convert string to uppercase
     * @param s String to convert (modified in place)
     * @return Pointer to the converted string
     */
    char* LIBC_CALL libc_strupr(char* s);

    /**
     * @brief Safely convert a string to lowercase in place
     * @param s Destination string
     * @param dest_size Destination capacity in bytes
     * @return 0 on success, or an error code on failure
     */
    int LIBC_CALL libc_strlwr_s(char* s, size_t dest_size);

    /**
     * @brief Safely convert a string to uppercase in place
     * @param s Destination string
     * @param dest_size Destination capacity in bytes
     * @return 0 on success, or an error code on failure
     */
    int LIBC_CALL libc_strupr_s(char* s, size_t dest_size);
    /**
     * @brief Safely reverse a string in place
     * @param s Destination string
     * @param dest_size Destination capacity in bytes
     * @return 0 on success, or an error code on failure
     */
    int LIBC_CALL libc_strrev_s(char* s, size_t dest_size);

    /**
     * @brief Count the initial characters belonging to a set
     * @param s String to inspect
     * @param accept Accepted character set
     * @return Length of the initial matching span
     */
    size_t LIBC_CALL libc_strspn(const char* s, const char* accept);

    /**
     * @brief Count the initial characters not belonging to a set
     * @param s String to inspect
     * @param reject Rejected character set
     * @return Length of the initial non-matching span
     */
    size_t LIBC_CALL libc_strcspn(const char* s, const char* reject);

    /**
     * @brief Find the first character from a set
     * @param s String to inspect
     * @param accept Character set to search for
     * @return Pointer to the matching character, or NULL
     */
    char* LIBC_CALL libc_strpbrk(const char* s, const char* accept);

    /*==========================================================================*/
    /* Wide Character String Functions                                          */
    /* These functions operate on null-terminated wide character strings        */
    /*==========================================================================*/

    /**
     * @brief Calculate length of wide character string
     * @param s Wide character string
     * @return Length of the string (excluding null terminator)
     */
    size_t   LIBC_CALL libc_wcslen(const wchar_t* s);

    /**
     * @brief Concatenate wide character strings
     * @param s Destination wide string
     * @param t Source wide string to append
     * @return Pointer to the destination string
     */
    wchar_t* LIBC_CALL libc_wcscat(wchar_t* s, const wchar_t* t);

    /**
     * @brief Safely copy a null-terminated wide string
     * @param dest Destination buffer
     * @param dest_size Destination capacity in wchar_t elements
     * @param src Source wide string
     * @return 0 on success, or an error code on failure
     */
    int LIBC_CALL libc_wcscpy_s(wchar_t* dest, size_t dest_size, const wchar_t* src);

    /**
     * @brief Safely append a null-terminated wide string
     * @param dest Destination buffer containing a valid string
     * @param dest_size Destination capacity in wchar_t elements
     * @param src Source wide string
     * @return 0 on success, or an error code on failure
     */
    int LIBC_CALL libc_wcscat_s(wchar_t* dest, size_t dest_size, const wchar_t* src);

    /**
     * @brief Find a wide string length up to a specified bound
     * @param s Wide string to inspect; NULL is accepted
     * @param max_count Maximum number of wide characters to inspect
     * @return Wide string length, or 0 for NULL/empty input
     */
    size_t LIBC_CALL libc_wcsnlen_s(const wchar_t* s, size_t max_count);

    /**
     * @brief Find first occurrence of wide character in wide string
     * @param t Wide string to search
     * @param c Wide character to find
     * @return Pointer to first occurrence, or NULL if not found
     */
    wchar_t* LIBC_CALL libc_wcschr(const wchar_t* t, wchar_t c);

    /**
     * @brief Find last occurrence of wide character in wide string
     * @param t Wide string to search
     * @param c Wide character to find
     * @return Pointer to last occurrence, or NULL if not found
     */
    wchar_t* LIBC_CALL libc_wcsrchr(const wchar_t* t, wchar_t c);

    /**
     * @brief Compare wide character strings lexicographically
     * @param s First wide string
     * @param t Second wide string
     * @return 0 if equal, <0 if s<t, >0 if s>t
     */
    int      LIBC_CALL libc_wcscmp(const wchar_t* s, const wchar_t* t);

    /**
     * @brief Copy wide character string
     * @param s Destination buffer
     * @param t Source wide string
     * @return Pointer to the destination string
     */
    wchar_t* LIBC_CALL libc_wcscpy(wchar_t* s, const wchar_t* t);

    /**
     * @brief Copy wide character string with length limit
     * @param s Destination buffer
     * @param t Source wide string
     * @param n Maximum number of characters to copy
     * @return Pointer to the destination string
     */
    wchar_t* LIBC_CALL libc_wcsncpy(wchar_t* s, const wchar_t* t, size_t n);

    /**
     * @brief Compare wide character strings with length limit
     * @param s First wide string
     * @param t Second wide string
     * @param n Maximum number of characters to compare
     * @return 0 if equal, <0 if s<t, >0 if s>t
     */
    int      LIBC_CALL libc_wcsncmp(const wchar_t* s, const wchar_t* t, size_t n);

    /**
     * @brief Concatenate wide character strings with length limit
     * @param s Destination wide string
     * @param t Source wide string to append
     * @param n Maximum number of characters to append
     * @return Pointer to the destination string
     */
    wchar_t* LIBC_CALL libc_wcsncat(wchar_t* s, const wchar_t* t, size_t n);

    /**
     * @brief Safely copy at most count wide characters and always terminate
     * @param dest Destination buffer
     * @param dest_size Destination capacity in wchar_t elements
     * @param src Source wide string
     * @param count Maximum source characters to copy, or _TRUNCATE
     * @return 0 on success, or an error code on failure
     */
    int LIBC_CALL libc_wcsncpy_s(wchar_t* dest, size_t dest_size,
                       const wchar_t* src, size_t count);

    /**
     * @brief Safely append at most count wide characters and always terminate
     * @param dest Destination buffer containing a valid string
     * @param dest_size Destination capacity in wchar_t elements
     * @param src Source wide string
     * @param count Maximum source characters to append, or _TRUNCATE
     * @return 0 on success, or an error code on failure
     */
    int LIBC_CALL libc_wcsncat_s(wchar_t* dest, size_t dest_size,
                       const wchar_t* src, size_t count);

    /**
     * @brief Find first occurrence of wide substring
     * @param haystack Wide string to search in
     * @param needle Wide substring to find
     * @return Pointer to first occurrence, or NULL if not found
     */
    wchar_t* LIBC_CALL libc_wcsstr(const wchar_t* haystack, const wchar_t* needle);

    /**
     * @brief Split wide character string into tokens
     * @param str Wide string to tokenize (NULL to continue)
     * @param delim Delimiter wide characters
     * @param saveptr Pointer to wchar_t* for internal state
     * @return Pointer to next token, or NULL if no more tokens
     */
    wchar_t* LIBC_CALL libc_wcstok(wchar_t* str, const wchar_t* delim, wchar_t** saveptr);

    /**
     * @brief Context-based wide string tokenization using the Windows signature
     * @param str Wide string to tokenize, or NULL to continue
     * @param delim Delimiter characters
     * @param context Tokenizer state
     * @return Pointer to the next token, or NULL
     */
    wchar_t* LIBC_CALL libc_wcstok_s(wchar_t* str, const wchar_t* delim,
                           wchar_t** context);

    /**
     * @brief Reverse wide character string in place
     * @param s Wide string to reverse
     * @return Pointer to the reversed string
     */
    wchar_t* LIBC_CALL libc_wcsrev(wchar_t* s);

    /**
     * @brief Compare wide character strings case-insensitively
     * @param s First wide string
     * @param t Second wide string
     * @return 0 if equal, <0 if s<t, >0 if s>t
     */
    int      LIBC_CALL libc_wcsicmp(const wchar_t* s, const wchar_t* t);

    /**
     * @brief Count occurrences of wide character in wide string
     * @param s Wide string to search
     * @param ch Wide character to count
     * @return Number of occurrences
     */
    size_t   LIBC_CALL libc_wcscount(const wchar_t* s, wchar_t ch);

    /**
     * @brief Copy wide character string with size limit
     * @param dest Destination buffer
     * @param src Source wide string
     * @param size Size of destination buffer
     * @return Length of source string
     */
    size_t   LIBC_CALL libc_wcslcpy(wchar_t* dest, const wchar_t* src, size_t size);

    /**
     * @brief Concatenate wide character strings with size limit
     * @param dest Destination wide string
     * @param src Source wide string to append
     * @param size Size of destination buffer
     * @return Length of resulting string
     */
    size_t   LIBC_CALL libc_wcslcat(wchar_t* dest, const wchar_t* src, size_t size);

    /**
     * @brief Convert wide character string to lowercase
     * @param s Wide string to convert (modified in place)
     * @return Pointer to the converted string
     */
    wchar_t* LIBC_CALL libc_wcslwr(wchar_t* s);

    /**
     * @brief Convert wide character string to uppercase
     * @param s Wide string to convert (modified in place)
     * @return Pointer to the converted string
     */
    wchar_t* LIBC_CALL libc_wcsupr(wchar_t* s);

    /**
     * @brief Safely convert a wide string to lowercase in place
     * @param s Destination wide string
     * @param dest_size Destination capacity in wchar_t elements
     * @return 0 on success, or an error code on failure
     */
    int LIBC_CALL libc_wcslwr_s(wchar_t* s, size_t dest_size);

    /**
     * @brief Safely convert a wide string to uppercase in place
     * @param s Destination wide string
     * @param dest_size Destination capacity in wchar_t elements
     * @return 0 on success, or an error code on failure
     */
    int LIBC_CALL libc_wcsupr_s(wchar_t* s, size_t dest_size);
    /**
     * @brief Safely reverse a wide string in place
     * @param s Destination wide string
     * @param dest_size Destination capacity in wchar_t elements
     * @return 0 on success, or an error code on failure
     */
    int LIBC_CALL libc_wcsrev_s(wchar_t* s, size_t dest_size);

    /**
     * @brief Count the initial wide characters belonging to a set
     * @param s Wide string to inspect
     * @param accept Accepted wide character set
     * @return Length of the initial matching span
     */
    size_t LIBC_CALL libc_wcsspn(const wchar_t* s, const wchar_t* accept);

    /**
     * @brief Count the initial wide characters not belonging to a set
     * @param s Wide string to inspect
     * @param reject Rejected wide character set
     * @return Length of the initial non-matching span
     */
    size_t LIBC_CALL libc_wcscspn(const wchar_t* s, const wchar_t* reject);

    /**
     * @brief Find the first wide character from a set
     * @param s Wide string to inspect
     * @param accept Wide character set to search for
     * @return Pointer to the matching character, or NULL
     */
    wchar_t* LIBC_CALL libc_wcspbrk(const wchar_t* s, const wchar_t* accept);

    /*==========================================================================*/
    /* Memory Manipulation Functions                                            */
    /* These functions operate on raw memory buffers                            */
    /*==========================================================================*/

    /**
     * @brief Copy memory area
     * @param dst Destination memory area
     * @param src Source memory area
     * @param count Number of bytes to copy
     * @return Pointer to destination
     */
    void* LIBC_CALL libc_memcpy(void* dst, const void* src, size_t count);

    /**
     * @brief Safely copy bytes into a bounded destination
     * @param dest Destination buffer
     * @param dest_size Destination capacity in bytes
     * @param src Source buffer
     * @param count Number of bytes to copy
     * @return 0 on success, or an error code on failure
     *
     * Overlapping source and destination ranges are rejected with EINVAL.
     */
    int LIBC_CALL libc_memcpy_s(void* dest, size_t dest_size,
                      const void* src, size_t count);

    /**
     * @brief Copy memory area, handling overlapping regions
     * @param dest Destination memory area
     * @param src Source memory area
     * @param n Number of bytes to copy
     * @return Pointer to destination
     */
    void* LIBC_CALL libc_memmove(void* dest, const void* src, size_t n);

    /**
     * @brief Safely move bytes into a bounded destination
     * @param dest Destination buffer
     * @param dest_size Destination capacity in bytes
     * @param src Source buffer
     * @param count Number of bytes to move
     * @return 0 on success, or an error code on failure
     */
    int LIBC_CALL libc_memmove_s(void* dest, size_t dest_size,
                       const void* src, size_t count);

    /**
     * @brief Copy memory area while preserving reverse-copy compatibility
     * @param dst Destination memory area
     * @param src Source memory area
     * @param count Number of bytes to copy
     * @return Pointer to destination
     *
     * Overlapping source and destination ranges are handled like memmove.
     */
    void* LIBC_CALL libc_memrcpy(void* dst, const void* src, size_t count);

    /**
     * @brief Fill memory with a constant byte
     * @param dst Destination memory area
     * @param s Byte value to fill with
     * @param count Number of bytes to fill
     * @return Pointer to destination
     */
    void* LIBC_CALL libc_memset(void* dst, int s, size_t count);

    /**
     * @brief Safely fill a bounded memory area
     * @param dest Destination memory area
     * @param dest_size Destination capacity in bytes
     * @param value Byte value to write
     * @param count Number of bytes to write
     * @return 0 on success, or an error code on failure
     */
    int LIBC_CALL libc_memset_s(void* dest, size_t dest_size, int value, size_t count);

    /**
     * @brief Compare memory areas
     * @param dst First memory area
     * @param src Second memory area
     * @param count Number of bytes to compare
     * @return 0 if equal, <0 if dst<src, >0 if dst>src
     */
    int      LIBC_CALL libc_memcmp(const void* dst, const void* src, size_t count);

    /**
     * @brief Search memory for a byte
     * @param s Memory area to search
     * @param c Byte value to find
     * @param n Number of bytes to search
     * @return Pointer to first occurrence, or NULL if not found
     */
    void* LIBC_CALL libc_memchr(const void* s, int c, size_t n);

    /*==========================================================================*/
    /* Wide Character Memory Functions                                          */
    /* These functions operate on wide character memory buffers                 */
    /*==========================================================================*/

    /**
     * @brief Copy wide character memory area
     * @param dest Destination memory area
     * @param src Source memory area
     * @param n Number of wide characters to copy
     * @return Pointer to destination
     */
    wchar_t* LIBC_CALL libc_wmemcpy(wchar_t* dest, const wchar_t* src, size_t n);

    /**
     * @brief Safely copy wide character memory
     * @param dest Destination buffer
     * @param dest_size Destination capacity in wchar_t elements
     * @param src Source buffer
     * @param count Number of wide characters to copy
     * @return 0 on success, or an error code on failure
     *
     * Overlapping source and destination ranges are rejected.
     */
    int LIBC_CALL libc_wmemcpy_s(wchar_t* dest, size_t dest_size,
                       const wchar_t* src, size_t count);

    /**
     * @brief Fill wide character memory with a constant wide character
     * @param s Memory area to fill
     * @param c Wide character to fill with
     * @param n Number of wide characters to fill
     * @return Pointer to destination
     */
    wchar_t* LIBC_CALL libc_wmemset(wchar_t* s, wchar_t c, size_t n);

    /**
     * @brief Compare wide character memory areas
     * @param s1 First memory area
     * @param s2 Second memory area
     * @param n Number of wide characters to compare
     * @return 0 if equal, <0 if s1<s2, >0 if s1>s2
     */
    int      LIBC_CALL libc_wmemcmp(const wchar_t* s1, const wchar_t* s2, size_t n);

    /**
     * @brief Search wide character memory for a wide character
     * @param s Memory area to search
     * @param c Wide character to find
     * @param n Number of wide characters to search
     * @return Pointer to first occurrence, or NULL if not found
     */
    wchar_t* LIBC_CALL libc_wmemchr(const wchar_t* s, wchar_t c, size_t n);

    /**
     * @brief Copy wide character memory area, handling overlapping regions
     * @param dest Destination memory area
     * @param src Source memory area
     * @param n Number of wide characters to copy
     * @return Pointer to destination
     */
    wchar_t* LIBC_CALL libc_wmemmove(wchar_t* dest, const wchar_t* src, size_t n);

    /**
     * @brief Safely move wide character memory
     * @param dest Destination buffer
     * @param dest_size Destination capacity in wchar_t elements
     * @param src Source buffer
     * @param count Number of wide characters to move
     * @return 0 on success, or an error code on failure
     */
    int LIBC_CALL libc_wmemmove_s(wchar_t* dest, size_t dest_size,
                        const wchar_t* src, size_t count);

    /**
     * @brief Safely fill wide character memory
     * @param dest Destination buffer
     * @param dest_size Destination capacity in wchar_t elements
     * @param value Wide character value
     * @param count Number of wide characters to fill
     * @return 0 on success, or an error code on failure
     */
    int LIBC_CALL libc_wmemset_s(wchar_t* dest, size_t dest_size,
                       wchar_t value, size_t count);

    /*==========================================================================*/
    /* Sorting and Searching Functions                                          */
    /* These implementations do not allocate memory and are kernel-safe.       */
    /*==========================================================================*/

    /**
     * @brief Sort an array in place using an iterative heapsort
     * @param base Array storage
     * @param count Number of elements
     * @param size Size of each element in bytes
     * @param compare Three-way element comparator
     */
    void LIBC_CALL libc_qsort(void* base, size_t count, size_t size,
                              libc_compare_fn compare);

    /**
     * @brief Search a sorted array using binary search
     * @param key Search key
     * @param base Array storage
     * @param count Number of elements
     * @param size Size of each element in bytes
     * @param compare Key/element comparator
     * @return Matching element, or NULL
     */
    void* LIBC_CALL libc_bsearch(const void* key, const void* base,
                                 size_t count, size_t size,
                                 libc_compare_fn compare);

    /**
     * @brief Secure-context array sort using the Windows signature
     * @param base Array storage
     * @param count Number of elements
     * @param size Size of each element in bytes
     * @param compare Comparator receiving context first
     * @param context Opaque comparator context
     * Invalid arguments cause a safe no-op and set the configured error state.
     */
    void LIBC_CALL libc_qsort_s(void* base, size_t count, size_t size,
                                libc_compare_s_fn compare, void* context);

    /**
     * @brief Secure-context binary search using the Windows comparator signature
     * @param key Search key
     * @param base Array storage
     * @param count Number of elements
     * @param size Size of each element in bytes
     * @param compare Comparator receiving context first
     * @param context Opaque comparator context
     * @return Matching element, or NULL
     */
    void* LIBC_CALL libc_bsearch_s(const void* key, const void* base,
                                   size_t count, size_t size,
                                   libc_compare_s_fn compare, void* context);

    /*==========================================================================*/
    /* Numeric Conversion Functions                                             */
    /* These functions convert between strings and numeric types                */
    /*==========================================================================*/

    /**
     * @brief Convert long integer to string
     * @param s Buffer to store result
     * @param size Size of buffer
     * @param i Value to convert
     * @param base Numeric base (2-36)
     * @param UpCase Use uppercase for hex digits (non-zero for uppercase)
     * @return Number of characters written, or 0 on error
     */
    int      LIBC_CALL libc_ltostr(char* s, int size, long i, int base, char UpCase);

    /**
     * @brief Convert long integer to wide character string
     * @param s Buffer to store result
     * @param size Size of buffer (in wide characters)
     * @param i Value to convert
     * @param base Numeric base (2-36)
     * @param UpCase Use uppercase for hex digits (non-zero for uppercase)
     * @return Number of characters written, or 0 on error
     */
    int      LIBC_CALL libc_ltowstr(wchar_t* s, int size, long i, int base, char UpCase);

    /**
     * @brief Convert long long integer to string
     * @param s Buffer to store result
     * @param size Size of buffer
     * @param i Value to convert
     * @param base Numeric base (2-36)
     * @param UpCase Use uppercase for hex digits (non-zero for uppercase)
     * @return Number of characters written, or 0 on error
     */
    int      LIBC_CALL libc_lltostr(char* s, int size, long long i, int base, char UpCase);

    /**
     * @brief Convert long long integer to wide character string
     * @param s Buffer to store result
     * @param size Size of buffer (in wide characters)
     * @param i Value to convert
     * @param base Numeric base (2-36)
     * @param UpCase Use uppercase for hex digits (non-zero for uppercase)
     * @return Number of characters written, or 0 on error
     */
    int      LIBC_CALL libc_lltowstr(wchar_t* s, int size, long long i, int base, char UpCase);

    /**
     * @brief Convert unsigned long integer to string
     * @param s Buffer to store result
     * @param size Size of buffer
     * @param i Value to convert
     * @param base Numeric base (2-36)
     * @param UpCase Use uppercase for hex digits (non-zero for uppercase)
     * @return Number of characters written, or 0 on error
     */
    int      LIBC_CALL libc_ultostr(char* s, int size, unsigned long i, int base, char UpCase);

    /**
     * @brief Convert unsigned long integer to wide character string
     * @param s Buffer to store result
     * @param size Size of buffer (in wide characters)
     * @param i Value to convert
     * @param base Numeric base (2-36)
     * @param UpCase Use uppercase for hex digits (non-zero for uppercase)
     * @return Number of characters written, or 0 on error
     */
    int      LIBC_CALL libc_ultowstr(wchar_t* s, int size, unsigned long i, int base, char UpCase);

    /**
     * @brief Convert unsigned long long integer to string
     * @param s Buffer to store result
     * @param size Size of buffer
     * @param i Value to convert
     * @param base Numeric base (2-36)
     * @param UpCase Use uppercase for hex digits (non-zero for uppercase)
     * @return Number of characters written, or 0 on error
     */
    int      LIBC_CALL libc_ulltostr(char* s, int size, unsigned long long i, int base, char UpCase);

    /**
     * @brief Convert unsigned long long integer to wide character string
     * @param s Buffer to store result
     * @param size Size of buffer (in wide characters)
     * @param i Value to convert
     * @param base Numeric base (2-36)
     * @param UpCase Use uppercase for hex digits (non-zero for uppercase)
     * @return Number of characters written, or 0 on error
     */
    int      LIBC_CALL libc_ulltowstr(wchar_t* s, int size, unsigned long long i, int base, char UpCase);

    /**
     * @brief Convert string to unsigned long integer
     * @param nptr String to convert
     * @param endptr Pointer to store address of first invalid character
     * @param base Numeric base (0, 2-36)
     * @return Converted value
     */
    unsigned long LIBC_CALL libc_strtoul(const char* nptr, char** endptr, int base);

    /**
     * @brief Convert string to long integer
     * @param nptr String to convert
     * @param endptr Pointer to store address of first invalid character
     * @param base Numeric base (0, 2-36)
     * @return Converted value
     */
    long     LIBC_CALL libc_strtol(const char* nptr, char** endptr, int base);

    /**
     * @brief Convert string to long long integer
     * @param nptr String to convert
     * @param endptr Pointer to store address of first invalid character
     * @param base Numeric base (0, 2-36)
     * @return Converted value
     */
    long long LIBC_CALL libc_strtoll(const char* nptr, char** endptr, int base);

    /**
     * @brief Convert string to unsigned long long integer
     * @param nptr String to convert
     * @param endptr Pointer to store address of first invalid character
     * @param base Numeric base (0, 2-36)
     * @return Converted value
     */
    unsigned long long LIBC_CALL libc_strtoull(const char* nptr, char** endptr, int base);

    /**
     * @brief Convert wide character string to unsigned long integer
     * @param nptr Wide string to convert
     * @param endptr Pointer to store address of first invalid character
     * @param base Numeric base (0, 2-36)
     * @return Converted value
     */
    unsigned long LIBC_CALL libc_wcstoul(const wchar_t* nptr, wchar_t** endptr, int base);

    /**
     * @brief Convert wide character string to long integer
     * @param nptr Wide string to convert
     * @param endptr Pointer to store address of first invalid character
     * @param base Numeric base (0, 2-36)
     * @return Converted value
     */
    long LIBC_CALL libc_wcstol(const wchar_t* nptr, wchar_t** endptr, int base);

    /**
     * @brief Convert wide character string to long long integer
     * @param nptr Wide string to convert
     * @param endptr Pointer to store address of first invalid character
     * @param base Numeric base (0, 2-36)
     * @return Converted value
     */
    long long LIBC_CALL libc_wcstoll(const wchar_t* nptr, wchar_t** endptr, int base);

    /**
     * @brief Convert wide character string to unsigned long long integer
     * @param nptr Wide string to convert
     * @param endptr Pointer to store address of first invalid character
     * @param base Numeric base (0, 2-36)
     * @return Converted value
     */
    unsigned long long LIBC_CALL libc_wcstoull(const wchar_t* nptr, wchar_t** endptr, int base);

    /*==========================================================================*/
    /* Hash Functions                                                           */
    /* These functions compute hash values for strings                          */
    /*==========================================================================*/

    /**
     * @brief Compute a case-sensitive hash value for a string
     * @param str String to hash
     * @return 32-bit hash value
     */
    uint32_t LIBC_CALL libc_strhash(const char* str);

    /**
     * @brief Compute a case-sensitive hash value for a string prefix
     * @param str String to hash
     * @param length Maximum number of characters to hash
     * @return 32-bit hash value
     *
     * Hashing stops at the first null character and uses the same
     * case-sensitive algorithm as libc_strhash.
     */
    uint32_t LIBC_CALL libc_strnhash(const char* str, size_t length);

    /*==========================================================================*/
    /* Formatting Functions                                                     */
    /* These functions provide formatted input/output capabilities              */
    /*==========================================================================*/

    /**
     * @brief Safely format a string with variable arguments using Windows semantics
     * @param dest Buffer to store result
     * @param dest_size Size of the destination buffer in bytes
     * @param count Maximum characters to write, or _TRUNCATE
     * @param format Format string
     * @param args Variable argument list
     * @return Number of characters written (excluding null terminator), or -1 on failure
     *
     * For _TRUNCATE, a truncated result is kept and -1 is returned.
     * Otherwise, the destination buffer is cleared when it is valid and an error occurs.
     */
    int LIBC_CALL libc_vsnprintf_s(char* dest, size_t dest_size,
                         size_t count, const char* format, va_list args);

    /**
     * @brief Safely format a string with a character-count limit
     * @param dest Buffer to store result
     * @param dest_size Size of the destination buffer in bytes
     * @param count Maximum characters to write, or _TRUNCATE
     * @param format Format string
     * @param ... Variable arguments
     * @return Number of characters written (excluding null terminator), or -1 on failure
     */
    int LIBC_CALL libc_snprintf_s(char* dest, size_t dest_size,
                        size_t count, const char* format, ...);

    /**
     * @brief Safely format a string with variable arguments without a count limit
     * @param dest Buffer to store result
     * @param dest_size Size of the destination buffer in bytes
     * @param format Format string
     * @param args Variable argument list
     * @return Number of characters written (excluding null terminator), or -1 on failure
     */
    int LIBC_CALL libc_vsprintf_s(char* dest, size_t dest_size,
                        const char* format, va_list args);

    /**
     * @brief Safely format a string
     * @param dest Buffer to store result
     * @param dest_size Size of the destination buffer in bytes
     * @param format Format string
     * @param ... Variable arguments
     * @return Number of characters written (excluding null terminator), or -1 on failure
     *
     * The destination buffer is cleared when it is valid and an error occurs.
     * Formatting fails instead of truncating when the result does not fit.
     */
    int LIBC_CALL libc_sprintf_s(char* dest, size_t dest_size, const char* format, ...);

    /**
     * @brief Parse formatted input from string
     * @param str String to parse
     * @param format Format string
     * @param arg_ptr Variable argument list
     * @return Number of successfully parsed items
     */
    int LIBC_CALL libc_vsscanf(const char* str, const char* format, va_list arg_ptr);

    /**
     * @brief Safely parse formatted input
     * @param str Input string
     * @param format Format string
     * @param arg_ptr Variable argument list; %s, %c, and %[ require an unsigned size after the destination
     * @return Number of assigned items, 0 on input failure, or -1 on constraint failure
     *
     * The size argument follows the Windows scanf_s convention. Cast a size_t
     * buffer length to unsigned before passing it through the variadic list.
     */
    int LIBC_CALL libc_vsscanf_s(const char* str, const char* format, va_list arg_ptr);

    /**
     * @brief Parse formatted input from string
     * @param str String to parse
     * @param format Format string
     * @param ... Variable arguments
     * @return Number of successfully parsed items
     */
    int LIBC_CALL libc_sscanf(const char* str, const char* format, ...);

    /**
     * @brief Safely parse formatted input
     * @param str Input string
     * @param format Format string
     * @param ... Variable arguments; %s, %c, and %[ require an unsigned size after the destination
     * @return Number of assigned items, 0 on input failure, or -1 on constraint failure
     *
     * The size argument follows the Windows scanf_s convention. Cast a size_t
     * buffer length to unsigned before passing it as a variadic argument.
     */
    int LIBC_CALL libc_sscanf_s(const char* str, const char* format, ...);

    /*==========================================================================*/
    /* Floating Point Functions                                                 */
    /* These functions handle floating point number conversions                 */
    /*==========================================================================*/

#ifndef LIBC_NO_FLOATING_POINT
    /**
     * @brief Convert string to double precision floating point
     * @param nptr String to convert
     * @param endptr Pointer to store address of first invalid character
     * @return Converted floating point value
     */
    double LIBC_CALL libc_strtod(const char* nptr, char** endptr);

    /**
     * @brief Convert double to string with precision control
     * @param d Double value to convert
     * @param buf Buffer to store result
     * @param maxlen Maximum buffer size
     * @param prec Number of digits after decimal point
     * @return Number of characters written
     */
    int LIBC_CALL libc_dtostr(double d, char* buf, int maxlen, int prec);
#endif

    /*==========================================================================*/
    /* Explicit Unicode Conversion Functions                                   */
    /* These APIs do not depend on the platform wchar_t width.                 */
    /*==========================================================================*/

    /**
     * @brief Convert UTF-8 to null-terminated UTF-16
     * @param dest Destination UTF-16 buffer
     * @param dest_size Destination capacity in uint16_t elements
     * @param src Null-terminated UTF-8 string
     * @param written Optional number of UTF-16 elements written, excluding null
     * @return 0 on success, or an error code on failure
     *
     * Overlapping source and destination ranges are rejected.
     */
    int LIBC_CALL libc_utf8_to_utf16_s(uint16_t* dest, size_t dest_size,
                             const char* src, size_t* written);

    /**
     * @brief Convert null-terminated UTF-16 to UTF-8
     * @param dest Destination UTF-8 buffer
     * @param dest_size Destination capacity in bytes
     * @param src Null-terminated UTF-16 string
     * @param written Optional number of UTF-8 bytes written, excluding null
     * @return 0 on success, or an error code on failure
     *
     * Overlapping source and destination ranges are rejected.
     */
    int LIBC_CALL libc_utf16_to_utf8_s(char* dest, size_t dest_size,
                             const uint16_t* src, size_t* written);

    /**
     * @brief Convert UTF-8 to the platform-native wchar_t string
     * @param dest Destination wide-character buffer
     * @param dest_size Destination capacity in wchar_t elements
     * @param src Null-terminated UTF-8 string
     * @param written Optional number of wchar_t elements written, excluding null
     * @return 0 on success, or an error code on failure
     */
    int LIBC_CALL libc_utf8_to_wchar_s(wchar_t* dest, size_t dest_size,
                             const char* src, size_t* written);

    /**
     * @brief Convert a platform-native wchar_t string to UTF-8
     * @param dest Destination UTF-8 buffer
     * @param dest_size Destination capacity in bytes
     * @param src Null-terminated platform-native wide string
     * @param written Optional number of UTF-8 bytes written, excluding null
     * @return 0 on success, or an error code on failure
     */
    int LIBC_CALL libc_wchar_to_utf8_s(char* dest, size_t dest_size,
                             const wchar_t* src, size_t* written);

     // clang-format on

#ifdef __cplusplus
}

/* char16_t is distinct from uint16_t in C++, although both are 16-bit units.
 * Keep these wrappers under distinct names because the C ABI declarations
 * above cannot be overloaded reliably by MSVC. */
#if __cplusplus >= 201103L || \
    (defined(_MSVC_LANG) && _MSVC_LANG >= 201103L)
static_assert(sizeof(char16_t) == sizeof(uint16_t),
              "char16_t must use 16-bit UTF-16 code units");

inline int libc_utf8_to_char16_s(char16_t* dest, size_t dest_size,
                                 const char* src, size_t* written)
{
    return libc_utf8_to_utf16_s(reinterpret_cast<uint16_t*>(dest),
                                dest_size, src, written);
}

inline int libc_char16_to_utf8_s(char* dest, size_t dest_size,
                                 const char16_t* src, size_t* written)
{
    return libc_utf16_to_utf8_s(dest, dest_size,
                                reinterpret_cast<const uint16_t*>(src),
                                written);
}

#if defined(__cpp_char8_t) && __cpp_char8_t >= 201811L
inline int libc_utf8_to_char16_s(char16_t* dest, size_t dest_size,
                                 const char8_t* src, size_t* written)
{
    return libc_utf8_to_char16_s(dest, dest_size,
                                 reinterpret_cast<const char*>(src),
                                 written);
}

inline int libc_char16_to_utf8_s(char8_t* dest, size_t dest_size,
                                 const char16_t* src, size_t* written)
{
    return libc_char16_to_utf8_s(reinterpret_cast<char*>(dest), dest_size,
                                 src, written);
}
#endif
#endif
#endif

#endif /* OXDRV_KLIBC_H_INCLUDED */
