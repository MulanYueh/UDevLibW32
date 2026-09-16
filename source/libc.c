/* Define either option as 0 to select the generic, smaller-feature path. */
#ifndef LIBC_WANT_SMALL_STRING_ROUTINES
#define LIBC_WANT_SMALL_STRING_ROUTINES 1
#endif
#ifndef LIBC_WANT_SMALL_WSTRING_ROUTINES
#define LIBC_WANT_SMALL_WSTRING_ROUTINES 1
#endif

#if defined(_MSC_VER)
#define LIBC_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define LIBC_NOINLINE __attribute__((noinline))
#else
#define LIBC_NOINLINE
#endif

/* Allow an explicit ABI choice, then fall back to the compiler's wchar_t. */
#if defined(LIBC_WCHAR_IS_UTF16) && defined(LIBC_WCHAR_IS_UTF32)
#error "LIBC_WCHAR_IS_UTF16 and LIBC_WCHAR_IS_UTF32 are mutually exclusive"
#elif !defined(LIBC_WCHAR_IS_UTF16) && !defined(LIBC_WCHAR_IS_UTF32) && \
      defined(__SIZEOF_WCHAR_T__) && __SIZEOF_WCHAR_T__ == 2
#define LIBC_WCHAR_IS_UTF16 1
#elif !defined(LIBC_WCHAR_IS_UTF16) && !defined(LIBC_WCHAR_IS_UTF32) && \
      defined(__SIZEOF_WCHAR_T__) && __SIZEOF_WCHAR_T__ == 4
#define LIBC_WCHAR_IS_UTF32 1
#elif !defined(LIBC_WCHAR_IS_UTF16) && !defined(LIBC_WCHAR_IS_UTF32) && \
      (defined(_WIN32) || defined(_WIN64))
#define LIBC_WCHAR_IS_UTF16 1
#endif

#include "libc.h"
#include <limits.h>
#include <stdarg.h>

LIBC_NOINLINE void* LIBC_CALL libc_memmove(void* dest, const void* src,
                                           size_t n);
LIBC_NOINLINE void* LIBC_CALL libc_memset(void* dest, int value, size_t count);
LIBC_NOINLINE wchar_t* LIBC_CALL libc_wmemmove(wchar_t* dest,
                                               const wchar_t* src, size_t n);
LIBC_NOINLINE wchar_t* LIBC_CALL libc_wmemset(wchar_t* dest, wchar_t value,
                                              size_t n);

#if !defined(LIBC_BUILD_KERNEL) && !defined(LIBC_NO_ERRNO) && \
    !defined(LIBC_SET_ERRNO)
#include <errno.h>
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifndef ERANGE
#define ERANGE 34
#endif

#ifndef EINVAL
#define EINVAL 22
#endif

#ifndef EILSEQ
#ifdef _WIN32
#define EILSEQ 42
#else
#define EILSEQ 84
#endif
#endif

#ifdef LIBC_SET_ERRNO
#define SET_ERRNO(err) LIBC_SET_ERRNO(err)
#elif defined(LIBC_BUILD_KERNEL) || defined(LIBC_NO_ERRNO)
#define SET_ERRNO(err) ((void)(err))
#else
#define SET_ERRNO(err) \
    do                 \
    {                  \
        errno = (err); \
    } while (0)
#endif

#define RETURN_ERROR(err, ret) \
    do                         \
    {                          \
        SET_ERRNO(err);        \
        return (ret);          \
    } while (0)

#define CHECK_NULL(ptr, ret)           \
    do                                 \
    {                                  \
        if (!(ptr))                    \
            RETURN_ERROR(EINVAL, ret); \
    } while (0)

static int libc_strlen_bounded(const char* str, size_t capacity,
                               size_t* length);
static int libc_wcslen_bounded(const wchar_t* str, size_t capacity,
                               size_t* length);
static int libc_ranges_overlap(const void* left, size_t left_size,
                               const void* right, size_t right_size);
static int libc_ranges_overlap_units(const void* left, size_t left_count,
                                     size_t left_unit_size,
                                     const void* right, size_t right_count,
                                     size_t right_unit_size);
static int libc_wranges_overlap(const void* left, size_t left_count,
                                const void* right, size_t right_count);
static int libc_string_constraint(char* dest, size_t dest_size, int error);
static int libc_wstring_constraint(wchar_t* dest, size_t dest_size, int error);

int LIBC_CALL libc_isspace(int ch)
{
    return ((ch == ' ') || (ch == '\f') || (ch == '\t') || (ch == '\v') || (ch == '\r') || (ch == '\n'));
}

int LIBC_CALL libc_iswspace(wchar_t ch)
{
    return ((ch == L' ') || (ch == L'\f') || (ch == L'\t') || (ch == L'\v') || (ch == L'\r') || (ch == L'\n'));
}

int LIBC_CALL libc_iswblank(wchar_t c)
{
    return c == L' ' || c == L'\t';
}

int LIBC_CALL libc_isdigit(int c)
{
    return (c >= '0' && c <= '9');
}

int LIBC_CALL libc_iswdigit(wchar_t c)
{
    return (c >= L'0' && c <= L'9');
}

int LIBC_CALL libc_isalpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

int LIBC_CALL libc_iswalpha(wchar_t c)
{
    return ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'));
}

int LIBC_CALL libc_iswalnum(wchar_t c)
{
    return (libc_iswalpha(c) || libc_iswdigit(c));
}

int LIBC_CALL libc_iswcntrl(wchar_t c)
{
    /* The unsigned cast also makes negative wchar_t values non-control. */
    uintmax_t value = (uintmax_t)c;
    return (value <= 31U || value == 127U);
}

int LIBC_CALL libc_iswgraph(wchar_t c)
{
    return (c >= 33 && c <= 126);
}

int LIBC_CALL libc_iswprint(wchar_t c)
{
    return (c >= 32 && c <= 126);
}

int LIBC_CALL libc_iswpunct(wchar_t c)
{
    return (libc_iswgraph(c) && !libc_iswalnum(c));
}

int LIBC_CALL libc_iswxdigit(wchar_t c)
{
    return (libc_iswdigit(c) || (c >= L'A' && c <= L'F') || (c >= L'a' && c <= L'f'));
}

int LIBC_CALL libc_isupper(int c)
{
    return (c >= 'A' && c <= 'Z');
}

int LIBC_CALL libc_islower(int c)
{
    return (c >= 'a' && c <= 'z');
}

int LIBC_CALL libc_iscntrl(int c)
{
    return ((c >= 0 && c <= 31) || c == 127);
}

int LIBC_CALL libc_isgraph(int c)
{
    return (c >= 33 && c <= 126);
}

int LIBC_CALL libc_isprint(int c)
{
    return (c >= 32 && c <= 126);
}

int LIBC_CALL libc_ispunct(int c)
{
    return (libc_isgraph(c) && !libc_isalnum(c));
}

int LIBC_CALL libc_toupper(int c)
{
    if (c >= 'a' && c <= 'z')
        return c - ('a' - 'A');
    return c;
}

int LIBC_CALL libc_tolower(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c + ('a' - 'A');
    return c;
}

wchar_t LIBC_CALL libc_towupper(wchar_t c)
{
    if (c >= L'a' && c <= L'z')
        return c - (L'a' - L'A');
    return c;
}

/* ASCII */
int LIBC_CALL libc_isascii(int c)
{
    return (c >= 0 && c <= 127);
}

int LIBC_CALL libc_isxdigit(int c)
{
    return libc_isdigit(c) ||
           (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

int LIBC_CALL libc_isblank(int c)
{
    return c == ' ' || c == '\t';
}

int LIBC_CALL libc_iswspace_ext(wchar_t c)
{
    // ASCII
    if (c == L' ' || c == L'\f' || c == L'\t' || c == L'\v' || c == L'\r' || c == L'\n')
        return 1;

    // Unicode
    if (c == 0x00A0 || c == 0x1680 || c == 0x2000 || c == 0x2001 || c == 0x2002 || c == 0x2003 || c == 0x2004 ||
        c == 0x2005 || c == 0x2006 || c == 0x2007 || c == 0x2008 || c == 0x2009 || c == 0x200A || c == 0x2028 ||
        c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000)
        return 1;

    return 0;
}

wchar_t LIBC_CALL libc_towlower(wchar_t c)
{
    if (c >= L'A' && c <= L'Z')
        return c + (L'a' - L'A');

    return c;
}

int LIBC_CALL libc_isalnum(int c)
{
    return libc_isalpha(c) || libc_isdigit(c);
}

int LIBC_CALL libc_iswupper(wchar_t c)
{
    return (c >= L'A' && c <= L'Z');
}

int LIBC_CALL libc_iswlower(wchar_t c)
{
    return (c >= L'a' && c <= L'z');
}

static int libc_wchar_compare(wchar_t left, wchar_t right)
{
    if (left < right)
        return -1;
    if (left > right)
        return 1;
    return 0;
}

size_t LIBC_CALL libc_strlen(const char* s)
{
    if (!s)
        return 0;

#if LIBC_WANT_SMALL_STRING_ROUTINES
    size_t i = 0;
    while (*s)
    {
        ++i;
        ++s;
    }
    return i;
#else
    size_t length = 0;
    while (s[length] != 0)
        ++length;
    return length;
#endif
}

size_t LIBC_CALL libc_strnlen_s(const char* s, size_t max_count)
{
    size_t length = 0;

    if (!s)
        return 0;
    while (length < max_count && s[length] != 0)
        ++length;
    return length;
}

char* LIBC_CALL libc_strcat(char* s, const char* t)
{
    CHECK_NULL(s, NULL);
    CHECK_NULL(t, s);

    char* dest = s;
    while (*s) s++;  // 

    do
    {
        *s++ = *t;
    } while (*t++);

    return dest;
}

char* LIBC_CALL libc_strchr(const char* t, int c)
{
    unsigned char ch;

    if (!t)
    {
        SET_ERRNO(EINVAL);
        return NULL;
    }

    /* strchr compares after conversion to unsigned char. */
    ch = (unsigned char)c;
    for (;; ++t)
    {
        if ((unsigned char)*t == ch)
            return (char*)t;
        if (*t == 0)
            return NULL;
    }
}

char* LIBC_CALL libc_strrchr(const char* t, int c)
{
    unsigned char ch;
    const char* last = NULL;

    if (!t)
    {
        SET_ERRNO(EINVAL);
        return NULL;
    }

    ch = (unsigned char)c;
    for (;; ++t)
    {
        if ((unsigned char)*t == ch)
            last = t;
        if (*t == 0)
            return (char*)last;
    }
}

int LIBC_CALL libc_strcmp(const char* s, const char* t)
{
    char x;

    for (;;)
    {
        x = *s;
        if (x != *t)
            break;
        if (!x)
            break;
        ++s;
        ++t;
#if !LIBC_WANT_SMALL_STRING_ROUTINES
        x = *s;
        if (x != *t)
            break;
        if (!x)
            break;
        ++s;
        ++t;
        x = *s;
        if (x != *t)
            break;
        if (!x)
            break;
        ++s;
        ++t;
        x = *s;
        if (x != *t)
            break;
        if (!x)
            break;
        ++s;
        ++t;
#endif
    }
    return ((int)(unsigned int)(unsigned char)x) - ((int)(unsigned int)(unsigned char)*t);
}

char* LIBC_CALL libc_strcpy(char* s, const char* t)
{
    char* dest = s;

    if (!s || !t)
        return s;

    for (;;)
    {
        char ch = *t++;
        *s++ = ch;
        if (!ch)
            break;
    }
    return dest;
}

char* LIBC_CALL libc_strncpy(char* s, const char* t, size_t n)
{
    char* dest = s;
    while (n != 0)
    {
        char ch = *t++;
        *s++ = ch;
        --n;
        if (!ch)
            break;
    }
    libc_memset(s, 0, n);
    return dest;
}

int LIBC_CALL libc_strncmp(const char* s, const char* t, size_t n)
{
    if (n == 0)
        return 0;

    char x;
    for (;;)
    {
        x = *s;
        if (x != *t)
            break;
        if (!x || --n == 0)
            break;
        ++s;
        ++t;
#if !LIBC_WANT_SMALL_STRING_ROUTINES
        x = *s;
        if (x != *t)
            break;
        if (!x || --n == 0)
            break;
        ++s;
        ++t;
        x = *s;
        if (x != *t)
            break;
        if (!x || --n == 0)
            break;
        ++s;
        ++t;
        x = *s;
        if (x != *t)
            break;
        if (!x || --n == 0)
            break;
        ++s;
        ++t;
#endif
    }
    return ((int)(unsigned int)(unsigned char)x) - ((int)(unsigned int)(unsigned char)*t);
}

char* LIBC_CALL libc_strncat(char* s, const char* t, size_t n)
{
    char* dest = s;

    if (!s || !t)
        return s;

    s += libc_strlen(s);
    while (n != 0 && *t)
    {
        *s++ = *t++;
        --n;
    }
    *s = 0;
    return dest;
}

char* LIBC_CALL libc_strstr(const char* haystack, const char* needle)
{
    if (!haystack || !needle)
    {
        SET_ERRNO(EINVAL);
        return NULL;
    }

    if (!*needle)
        return (char*)haystack;

    while (*haystack)
    {
        const char* h = haystack;
        const char* n = needle;

        while (*h && *n && *h == *n)
        {
            ++h;
            ++n;
        }

        if (!*n)
            return (char*)haystack;

        ++haystack;
    }

    return 0;
}

char* LIBC_CALL libc_strtok(char* str, const char* delim, char** saveptr)
{
    char* token;

    if (!delim || !saveptr)
    {
        SET_ERRNO(EINVAL);
        return NULL;
    }

    if (str == 0)
        str = *saveptr;
    if (!str)
        return NULL;

    while (*str && libc_strchr(delim, *str)) ++str;

    if (!*str)
    {
        *saveptr = str;
        return 0;
    }

    token = str;

    while (*str && !libc_strchr(delim, *str)) ++str;

    if (*str)
    {
        *str = 0;
        *saveptr = str + 1;
    }
    else
    {
        *saveptr = str;
    }

    return token;
}

char* LIBC_CALL libc_strtok_s(char* str, const char* delim, char** context)
{
    char* token;

    if (!delim || !context)
    {
        SET_ERRNO(EINVAL);
        return NULL;
    }

    if (!str)
        str = *context;
    if (!str)
        return NULL;

    while (*str && libc_strchr(delim, *str))
        ++str;
    if (!*str)
    {
        *context = str;
        return NULL;
    }

    token = str;
    while (*str && !libc_strchr(delim, *str))
        ++str;
    if (*str)
    {
        *str = 0;
        *context = str + 1;
    }
    else
    {
        *context = str;
    }
    return token;
}

char* LIBC_CALL libc_strrev(char* s)
{
    if (!s || !*s)
        return s;

    char* start = s;
    char* end = s + libc_strlen(s) - 1;

    while (start < end)
    {
        char temp = *start;
        *start = *end;
        *end = temp;
        ++start;
        --end;
    }

    return s;
}

int LIBC_CALL libc_stricmp(const char* s, const char* t)
{
    char x, y;

    for (;;)
    {
        x = *s;
        y = *t;

        if (x >= 'A' && x <= 'Z')
            x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z')
            y += 'a' - 'A';

        if (x != y)
            break;
        if (!x)
            break;
        ++s;
        ++t;
#if !LIBC_WANT_SMALL_STRING_ROUTINES
        x = *s;
        y = *t;
        if (x >= 'A' && x <= 'Z')
            x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z')
            y += 'a' - 'A';
        if (x != y)
            break;
        if (!x)
            break;
        ++s;
        ++t;
        x = *s;
        y = *t;
        if (x >= 'A' && x <= 'Z')
            x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z')
            y += 'a' - 'A';
        if (x != y)
            break;
        if (!x)
            break;
        ++s;
        ++t;
        x = *s;
        y = *t;
        if (x >= 'A' && x <= 'Z')
            x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z')
            y += 'a' - 'A';
        if (x != y)
            break;
        if (!x)
            break;
        ++s;
        ++t;
#endif
    }
    return ((int)(unsigned char)x) - ((int)(unsigned char)y);
}

size_t LIBC_CALL libc_strcount(const char* s, char ch)
{
    size_t count = 0;

    if (!s)
        return 0;

    while (*s)
    {
        if (*s == ch)
            ++count;
        ++s;
    }

    return count;
}

size_t LIBC_CALL libc_strlcpy(char* dest, const char* src, size_t size)
{
    size_t src_len;

    if (!src)
    {
        if (dest && size != 0)
            dest[0] = 0;
        SET_ERRNO(EINVAL);
        return 0;
    }

    src_len = libc_strlen(src);

    if (size == 0)
        return src_len;
    if (!dest)
    {
        SET_ERRNO(EINVAL);
        return 0;
    }

    size_t n = src_len < size - 1 ? src_len : size - 1;
    libc_memcpy(dest, src, n);
    dest[n] = 0;

    return src_len;
}

size_t LIBC_CALL libc_strlcat(char* dest, const char* src, size_t size)
{
    size_t dest_len;
    size_t src_len;
    size_t result;

    if (!src)
    {
        if (dest && size != 0)
            dest[0] = 0;
        SET_ERRNO(EINVAL);
        return 0;
    }

    src_len = libc_strlen(src);

    if (size == 0)
        return src_len;
    if (!dest)
    {
        SET_ERRNO(EINVAL);
        return 0;
    }

    /* Unlike strlen, strlcat must never inspect beyond the supplied size. */
    dest_len = libc_strnlen_s(dest, size);

    if (dest_len >= size)
    {
        if (src_len > SIZE_MAX - size)
            return SIZE_MAX;
        return size + src_len;
    }

    size_t n = size - dest_len - 1;
    if (src_len < n)
        n = src_len;

    libc_memcpy(dest + dest_len, src, n);
    dest[dest_len + n] = 0;

    if (dest_len > SIZE_MAX - src_len)
        result = SIZE_MAX;
    else
        result = dest_len + src_len;
    return result;
}

char* LIBC_CALL libc_strlwr(char* s)
{
    char* p = s;

    if (!p)
        return NULL;
    while (*p)
    {
        *p = (char)libc_tolower(*p);
        ++p;
    }
    return s;
}

char* LIBC_CALL libc_strupr(char* s)
{
    char* p = s;

    if (!p)
        return NULL;
    while (*p)
    {
        *p = (char)libc_toupper(*p);
        ++p;
    }
    return s;
}

int LIBC_CALL libc_strlwr_s(char* s, size_t dest_size)
{
    size_t length;
    size_t i;

    if (!s || dest_size == 0)
        return libc_string_constraint(s, dest_size, EINVAL);
    if (!libc_strlen_bounded(s, dest_size, &length))
        return libc_string_constraint(s, dest_size, ERANGE);

    for (i = 0; i < length; ++i)
        s[i] = (char)libc_tolower((unsigned char)s[i]);
    return 0;
}

int LIBC_CALL libc_strupr_s(char* s, size_t dest_size)
{
    size_t length;
    size_t i;

    if (!s || dest_size == 0)
        return libc_string_constraint(s, dest_size, EINVAL);
    if (!libc_strlen_bounded(s, dest_size, &length))
        return libc_string_constraint(s, dest_size, ERANGE);

    for (i = 0; i < length; ++i)
        s[i] = (char)libc_toupper((unsigned char)s[i]);
    return 0;
}

/*  - 32wchar_t */
int LIBC_CALL libc_strrev_s(char* s, size_t dest_size)
{
    size_t length;
    size_t i;

    if (!s || dest_size == 0)
        return libc_string_constraint(s, dest_size, EINVAL);
    if (!libc_strlen_bounded(s, dest_size, &length))
        return libc_string_constraint(s, dest_size, ERANGE);

    for (i = 0; i < length / 2; ++i)
    {
        char value = s[i];
        s[i] = s[length - i - 1];
        s[length - i - 1] = value;
    }
    return 0;
}

size_t LIBC_CALL libc_strspn(const char* s, const char* accept)
{
    size_t length = 0;

    if (!s || !accept)
    {
        SET_ERRNO(EINVAL);
        return 0;
    }

    while (s[length] != 0 && libc_strchr(accept, (unsigned char)s[length]))
        ++length;
    return length;
}

size_t LIBC_CALL libc_strcspn(const char* s, const char* reject)
{
    size_t length = 0;

    if (!s || !reject)
    {
        SET_ERRNO(EINVAL);
        return 0;
    }

    while (s[length] != 0 &&
           !libc_strchr(reject, (unsigned char)s[length]))
        ++length;
    return length;
}

char* LIBC_CALL libc_strpbrk(const char* s, const char* accept)
{
    if (!s || !accept)
    {
        SET_ERRNO(EINVAL);
        return NULL;
    }

    while (*s != 0)
    {
        if (libc_strchr(accept, (unsigned char)*s) != NULL)
            return (char*)s;
        ++s;
    }
    return NULL;
}

#if LIBC_WANT_SMALL_WSTRING_ROUTINES
/*  -  */
static size_t libc_wcslen_simple(const wchar_t* s)
{
    size_t i;
    if (!s)
        return 0;
    for (i = 0; *s; ++s) ++i;
    return i;
}
size_t LIBC_CALL libc_wcslen(const wchar_t* s)
{
    return libc_wcslen_simple(s);
}
#else
size_t LIBC_CALL libc_wcslen(const wchar_t* s)
{
    size_t length = 0;

    if (!s)
        return 0;

    /* Byte-oriented zero scans are invalid for both 16-bit and 32-bit wchar_t. */
    while (s[length] != L'\0')
        ++length;
    return length;

#if 0
    static const unsigned long magic = 0x01010101;
    const wchar_t* t = s;
    unsigned long word;

    if (!s)
        return 0;

    for (; (((uintptr_t)(void*)t) & (sizeof(unsigned long) - 1)); t++)
        if (!*t)
            return t - s;

    do
    {
        word = *((unsigned long*)t);
        t += sizeof(unsigned long) / sizeof(wchar_t);
        word = (word - magic) & ~word;
        word &= (magic << 7);
    } while (word == 0);

#if BYTE_ORDER == LITTLE_ENDIAN
    word = (word - 1) & (magic << 10);
    word += (word << 8) + (word << 16);
    t += word >> 26;
#else
    if ((word & 0x80800000) == 0)
    {
        word <<= 16;
        t += 2;
    }
    if ((word & 0x80000000) == 0)
        t += 1;
#endif
    return ((const wchar_t*)t) - 4 - s;
#endif
}
#endif

size_t LIBC_CALL libc_wcsnlen_s(const wchar_t* s, size_t max_count)
{
    size_t length = 0;

    if (!s)
        return 0;
    while (length < max_count && s[length] != L'\0')
        ++length;
    return length;
}

wchar_t* LIBC_CALL libc_wcscat(wchar_t* s, const wchar_t* t)
{
    wchar_t* dest = s;

    if (!s || !t)
        return s;

    s += libc_wcslen(s);
    while (*t)
    {
        *s++ = *t++;
    }
    *s = L'\0';
    return dest;
}

wchar_t* LIBC_CALL libc_wcschr(const wchar_t* t, wchar_t c)
{
    wchar_t ch;

    if (!t)
    {
        SET_ERRNO(EINVAL);
        return NULL;
    }

    ch = (wchar_t)c;
    for (;;)
    {
        if (*t == ch)
            break;
        if (!*t)
            return 0;
        ++t;
#if !LIBC_WANT_SMALL_WSTRING_ROUTINES
        if (*t == ch)
            break;
        if (!*t)
            return 0;
        ++t;
        if (*t == ch)
            break;
        if (!*t)
            return 0;
        ++t;
        if (*t == ch)
            break;
        if (!*t)
            return 0;
        ++t;
#endif
    }
    return (wchar_t*)t;
}

wchar_t* LIBC_CALL libc_wcsrchr(const wchar_t* t, wchar_t c)
{
    wchar_t ch;
    const wchar_t* l = 0;

    if (!t)
    {
        SET_ERRNO(EINVAL);
        return NULL;
    }

    ch = (wchar_t)c;
    for (;;)
    {
        if (*t == ch)
            l = t;
        if (!*t)
            return (wchar_t*)l;
        ++t;
#if !LIBC_WANT_SMALL_WSTRING_ROUTINES
        if (*t == ch)
            l = t;
        if (!*t)
            return (wchar_t*)l;
        ++t;
        if (*t == ch)
            l = t;
        if (!*t)
            return (wchar_t*)l;
        ++t;
        if (*t == ch)
            l = t;
        if (!*t)
            return (wchar_t*)l;
        ++t;
#endif
    }
}

int LIBC_CALL libc_wcscmp(const wchar_t* s, const wchar_t* t)
{
    wchar_t x;

    for (;;)
    {
        x = *s;
        if (x != *t)
            break;
        if (!x)
            break;
        ++s;
        ++t;
#if !LIBC_WANT_SMALL_WSTRING_ROUTINES
        x = *s;
        if (x != *t)
            break;
        if (!x)
            break;
        ++s;
        ++t;
        x = *s;
        if (x != *t)
            break;
        if (!x)
            break;
        ++s;
        ++t;
        x = *s;
        if (x != *t)
            break;
        if (!x)
            break;
        ++s;
        ++t;
#endif
    }
    return libc_wchar_compare(x, *t);
}

wchar_t* LIBC_CALL libc_wcscpy(wchar_t* s, const wchar_t* t)
{
    wchar_t* dest = s;

    if (!s || !t)
        return s;

    for (;;)
    {
        wchar_t ch = *t++;
        *s++ = ch;
        if (!ch)
            break;
    }
    return dest;
}

/* n */
wchar_t* LIBC_CALL libc_wcsncpy(wchar_t* s, const wchar_t* t, size_t n)
{
    wchar_t* dest = s;
    while (n != 0)
    {
        wchar_t ch = *t++;
        *s++ = ch;
        --n;
        if (!ch)
            break;
    }
    libc_wmemset(s, L'\0', n);
    return dest;
}

/* n */
int LIBC_CALL libc_wcsncmp(const wchar_t* s, const wchar_t* t, size_t n)
{
    if (n == 0)
        return 0;

    wchar_t x;
    for (;;)
    {
        x = *s;
        if (x != *t)
            break;
        if (!x || --n == 0)
            break;
        ++s;
        ++t;
#if !LIBC_WANT_SMALL_WSTRING_ROUTINES
        x = *s;
        if (x != *t)
            break;
        if (!x || --n == 0)
            break;
        ++s;
        ++t;
        x = *s;
        if (x != *t)
            break;
        if (!x || --n == 0)
            break;
        ++s;
        ++t;
        x = *s;
        if (x != *t)
            break;
        if (!x || --n == 0)
            break;
        ++s;
        ++t;
#endif
    }
    return libc_wchar_compare(x, *t);
}

/* n */
wchar_t* LIBC_CALL libc_wcsncat(wchar_t* s, const wchar_t* t, size_t n)
{
    wchar_t* dest = s;

    if (!s || !t)
        return s;

    s += libc_wcslen(s);
    while (n != 0 && *t)
    {
        *s++ = *t++;
        --n;
    }
    *s = 0;
    return dest;
}

wchar_t* LIBC_CALL libc_wcsstr(const wchar_t* haystack, const wchar_t* needle)
{
    if (!haystack || !needle)
    {
        SET_ERRNO(EINVAL);
        return NULL;
    }

    if (!*needle)
        return (wchar_t*)haystack;

    while (*haystack)
    {
        const wchar_t* h = haystack;
        const wchar_t* n = needle;

        while (*h && *n && *h == *n)
        {
            ++h;
            ++n;
        }

        if (!*n)
            return (wchar_t*)haystack;

        ++haystack;
    }

    return 0;
}

wchar_t* LIBC_CALL libc_wcstok(wchar_t* str, const wchar_t* delim, wchar_t** saveptr)
{
    wchar_t* token;

    if (!delim || !saveptr)
    {
        SET_ERRNO(EINVAL);
        return NULL;
    }

    if (str == 0)
        str = *saveptr;
    if (!str)
        return NULL;

    while (*str && libc_wcschr(delim, *str)) ++str;

    if (!*str)
    {
        *saveptr = str;
        return 0;
    }

    token = str;

    while (*str && !libc_wcschr(delim, *str)) ++str;

    if (*str)
    {
        *str = 0;
        *saveptr = str + 1;
    }
    else
    {
        *saveptr = str;
    }

    return token;
}

wchar_t* LIBC_CALL libc_wcstok_s(wchar_t* str, const wchar_t* delim,
                       wchar_t** context)
{
    wchar_t* token;

    if (!delim || !context)
    {
        SET_ERRNO(EINVAL);
        return NULL;
    }

    if (!str)
        str = *context;
    if (!str)
        return NULL;

    while (*str && libc_wcschr(delim, *str))
        ++str;
    if (!*str)
    {
        *context = str;
        return NULL;
    }

    token = str;
    while (*str && !libc_wcschr(delim, *str))
        ++str;
    if (*str)
    {
        *str = L'\0';
        *context = str + 1;
    }
    else
    {
        *context = str;
    }
    return token;
}

wchar_t* LIBC_CALL libc_wcsrev(wchar_t* s)
{
    size_t length;

    if (!s || !*s)
        return s;

    length = libc_wcslen(s);
    wchar_t* start = s;
    wchar_t* end = s + length - 1;

    while (start < end)
    {
        wchar_t temp = *start;
        *start = *end;
        *end = temp;
        ++start;
        --end;
    }

    return s;
}

int LIBC_CALL libc_wcsicmp(const wchar_t* s, const wchar_t* t)
{
    wchar_t x, y;

    for (;;)
    {
        x = *s;
        y = *t;

        if (x >= L'A' && x <= L'Z')
            x += L'a' - L'A';
        if (y >= L'A' && y <= L'Z')
            y += L'a' - L'A';

        if (x != y)
            break;
        if (!x)
            break;
        ++s;
        ++t;
#if !LIBC_WANT_SMALL_WSTRING_ROUTINES
        x = *s;
        y = *t;
        if (x >= L'A' && x <= L'Z')
            x += L'a' - L'A';
        if (y >= L'A' && y <= L'Z')
            y += L'a' - L'A';
        if (x != y)
            break;
        if (!x)
            break;
        ++s;
        ++t;
        x = *s;
        y = *t;
        if (x >= L'A' && x <= L'Z')
            x += L'a' - L'A';
        if (y >= L'A' && y <= L'Z')
            y += L'a' - L'A';
        if (x != y)
            break;
        if (!x)
            break;
        ++s;
        ++t;
        x = *s;
        y = *t;
        if (x >= L'A' && x <= L'Z')
            x += L'a' - L'A';
        if (y >= L'A' && y <= L'Z')
            y += L'a' - L'A';
        if (x != y)
            break;
        if (!x)
            break;
        ++s;
        ++t;
#endif
    }
    return libc_wchar_compare(x, y);
}

size_t LIBC_CALL libc_wcscount(const wchar_t* s, wchar_t ch)
{
    size_t count = 0;

    if (!s)
        return 0;

    while (*s)
    {
        if (*s == ch)
            ++count;
        ++s;
    }

    return count;
}

size_t LIBC_CALL libc_wcslcpy(wchar_t* dest, const wchar_t* src, size_t size)
{
    size_t src_len;

    if (!src)
    {
        if (dest && size != 0)
            dest[0] = L'\0';
        SET_ERRNO(EINVAL);
        return 0;
    }

    src_len = libc_wcslen(src);

    if (size == 0)
        return src_len;
    if (!dest)
    {
        SET_ERRNO(EINVAL);
        return 0;
    }

    size_t n = src_len < size - 1 ? src_len : size - 1;
    libc_wmemcpy(dest, src, n);
    dest[n] = 0;

    return src_len;
}

size_t LIBC_CALL libc_wcslcat(wchar_t* dest, const wchar_t* src, size_t size)
{
    size_t dest_len;
    size_t src_len;
    size_t result;

    if (!src)
    {
        if (dest && size != 0)
            dest[0] = L'\0';
        SET_ERRNO(EINVAL);
        return 0;
    }

    src_len = libc_wcslen(src);

    if (size == 0)
        return src_len;
    if (!dest)
    {
        SET_ERRNO(EINVAL);
        return 0;
    }

    /* Bound the destination scan just like the byte-oriented variant. */
    dest_len = libc_wcsnlen_s(dest, size);

    if (dest_len >= size)
    {
        if (src_len > SIZE_MAX - size)
            return SIZE_MAX;
        return size + src_len;
    }

    size_t n = size - dest_len - 1;
    if (src_len < n)
        n = src_len;

    libc_wmemcpy(dest + dest_len, src, n);
    dest[dest_len + n] = 0;

    if (dest_len > SIZE_MAX - src_len)
        result = SIZE_MAX;
    else
        result = dest_len + src_len;
    return result;
}

wchar_t* LIBC_CALL libc_wcslwr(wchar_t* s)
{
    wchar_t* p = s;

    if (!p)
        return NULL;
    while (*p)
    {
        *p = libc_towlower(*p);
        ++p;
    }
    return s;
}

wchar_t* LIBC_CALL libc_wcsupr(wchar_t* s)
{
    wchar_t* p = s;

    if (!p)
        return NULL;
    while (*p)
    {
        *p = libc_towupper(*p);
        ++p;
    }
    return s;
}

int LIBC_CALL libc_wcslwr_s(wchar_t* s, size_t dest_size)
{
    size_t length;
    size_t i;

    if (!s || dest_size == 0)
        return libc_wstring_constraint(s, dest_size, EINVAL);
    if (!libc_wcslen_bounded(s, dest_size, &length))
        return libc_wstring_constraint(s, dest_size, ERANGE);

    for (i = 0; i < length; ++i)
        s[i] = libc_towlower(s[i]);
    return 0;
}

int LIBC_CALL libc_wcsupr_s(wchar_t* s, size_t dest_size)
{
    size_t length;
    size_t i;

    if (!s || dest_size == 0)
        return libc_wstring_constraint(s, dest_size, EINVAL);
    if (!libc_wcslen_bounded(s, dest_size, &length))
        return libc_wstring_constraint(s, dest_size, ERANGE);

    for (i = 0; i < length; ++i)
        s[i] = libc_towupper(s[i]);
    return 0;
}

int LIBC_CALL libc_wcsrev_s(wchar_t* s, size_t dest_size)
{
    size_t length;
    size_t i;

    if (!s || dest_size == 0)
        return libc_wstring_constraint(s, dest_size, EINVAL);
    if (!libc_wcslen_bounded(s, dest_size, &length))
        return libc_wstring_constraint(s, dest_size, ERANGE);

    for (i = 0; i < length / 2; ++i)
    {
        wchar_t value = s[i];
        s[i] = s[length - i - 1];
        s[length - i - 1] = value;
    }
    return 0;
}

size_t LIBC_CALL libc_wcsspn(const wchar_t* s, const wchar_t* accept)
{
    size_t length = 0;

    if (!s || !accept)
    {
        SET_ERRNO(EINVAL);
        return 0;
    }

    while (s[length] != L'\0' &&
           libc_wcschr(accept, s[length]) != NULL)
        ++length;
    return length;
}

size_t LIBC_CALL libc_wcscspn(const wchar_t* s, const wchar_t* reject)
{
    size_t length = 0;

    if (!s || !reject)
    {
        SET_ERRNO(EINVAL);
        return 0;
    }

    while (s[length] != L'\0' &&
           libc_wcschr(reject, s[length]) == NULL)
        ++length;
    return length;
}

wchar_t* LIBC_CALL libc_wcspbrk(const wchar_t* s, const wchar_t* accept)
{
    if (!s || !accept)
    {
        SET_ERRNO(EINVAL);
        return NULL;
    }

    while (*s != L'\0')
    {
        if (libc_wcschr(accept, *s) != NULL)
            return (wchar_t*)s;
        ++s;
    }
    return NULL;
}

wchar_t* LIBC_CALL libc_wmemcpy(wchar_t* dest, const wchar_t* src, size_t n)
{
    return libc_wmemmove(dest, src, n);
}

LIBC_NOINLINE wchar_t* LIBC_CALL libc_wmemset(wchar_t* s, wchar_t c, size_t n)
{
    volatile wchar_t* p = s;

    while (n-- > 0) *p++ = c;

    return s;
}

int LIBC_CALL libc_wmemcmp(const wchar_t* s1, const wchar_t* s2, size_t n)
{
    if (n == 0)
        return 0;

    while (n-- > 0)
    {
        if (*s1 != *s2)
            return libc_wchar_compare(*s1, *s2);
        ++s1;
        ++s2;
    }

    return 0;
}

wchar_t* LIBC_CALL libc_wmemchr(const wchar_t* s, wchar_t c, size_t n)
{
    while (n-- > 0)
    {
        if (*s == c)
            return (wchar_t*)s;
        ++s;
    }
    return 0;
}

LIBC_NOINLINE void* LIBC_CALL libc_memmove(void* dest, const void* src, size_t n)
{
    if (dest == NULL || src == NULL || n == 0)
        return dest;

    char* d = (char*)dest;
    const char* s = (const char*)src;
    uintptr_t d_addr = (uintptr_t)d;
    uintptr_t s_addr = (uintptr_t)s;

    if (d_addr < s_addr)
    {
        while (n > 0)
        {
            *d++ = *s++;
            --n;
        }
    }
    else if (d_addr > s_addr)
    {
        d += n;
        s += n;
        while (n > 0)
        {
            *--d = *--s;
            --n;
        }
    }

    return dest;
}

void* LIBC_CALL libc_memcpy(void* dst, const void* src, size_t count)
{
    if (!dst || !src || count == 0)
        return dst;

    /* This implementation deliberately keeps memcpy overlap-safe. */

    return libc_memmove(dst, src, count);
}

void* LIBC_CALL libc_memrcpy(void* dst, const void* src, size_t count)
{
    if (!dst || !src || count == 0)
        return dst;

    /* Reverse traversal is safe only when the destination starts after src. */
    if ((uintptr_t)dst > (uintptr_t)src)
    {
        unsigned char* d = (unsigned char*)dst + count;
        const unsigned char* s = (const unsigned char*)src + count;

        while (count > 0)
        {
            *--d = *--s;
            --count;
        }
    }
    else
    {
        unsigned char* d = (unsigned char*)dst;
        const unsigned char* s = (const unsigned char*)src;

        while (count > 0)
        {
            *d++ = *s++;
            --count;
        }
    }

    return dst;
}

LIBC_NOINLINE void* LIBC_CALL libc_memset(void* dst, int s, size_t count)
{
    volatile unsigned char* a = (volatile unsigned char*)dst;
    unsigned char c = (unsigned char)s;

    if (!dst || count == 0)
        return dst;

    while (count > 0)
    {
        *a++ = c;
        --count;
    }

    return dst;
}

int LIBC_CALL libc_memcmp(const void* dst, const void* src, size_t count)
{
    const unsigned char* d = (const unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (count > 0)
    {
        if (*d != *s)
            return (int)*d - (int)*s;
        ++d;
        ++s;
        --count;
    }
    return 0;
}

LIBC_NOINLINE wchar_t* LIBC_CALL libc_wmemmove(wchar_t* dest, const wchar_t* src, size_t n)
{
    if (dest == NULL || src == NULL || n == 0)
    {
        return dest;
    }

    if (n > SIZE_MAX / sizeof(wchar_t))
        return dest;

    volatile uint8_t* d = (volatile uint8_t*)dest;
    const volatile uint8_t* s = (const volatile uint8_t*)src;
    size_t byte_count = n * sizeof(wchar_t);

    if ((uintptr_t)d < (uintptr_t)s)
    {
        for (size_t i = 0; i < byte_count; i++)
        {
            d[i] = s[i];
        }
    }
    else if ((uintptr_t)d > (uintptr_t)s)
    {
        for (size_t i = byte_count; i > 0; i--)
        {
            d[i - 1] = s[i - 1];
        }
    }

    return dest;
}

void* LIBC_CALL libc_memchr(const void* s, int c, size_t n)
{
    const unsigned char* p = (const unsigned char*)s;
    const unsigned char value = (unsigned char)c;

    while (n > 0)
    {
        if (*p == value)
            return (void*)p;
        ++p;
        --n;
    }
    return 0;
}

static void libc_sort_swap(unsigned char* left, unsigned char* right,
                            size_t size)
{
    while (size != 0)
    {
        unsigned char value = *left;
        *left++ = *right;
        *right++ = value;
        --size;
    }
}

static void libc_qsort_sift_down(unsigned char* base, size_t root,
                                 size_t count, size_t size,
                                 libc_compare_fn compare)
{
    while (root < count / 2)
    {
        size_t child = root * 2 + 1;
        unsigned char* root_element = base + root * size;
        unsigned char* child_element = base + child * size;

        if (child + 1 < count &&
            compare(child_element, base + (child + 1) * size) < 0)
        {
            ++child;
            child_element = base + child * size;
        }

        if (compare(root_element, child_element) >= 0)
            break;
        libc_sort_swap(root_element, child_element, size);
        root = child;
    }
}

static void libc_qsort_sift_down_s(unsigned char* base, size_t root,
                                   size_t count, size_t size,
                                   libc_compare_s_fn compare, void* context)
{
    while (root < count / 2)
    {
        size_t child = root * 2 + 1;
        unsigned char* root_element = base + root * size;
        unsigned char* child_element = base + child * size;

        if (child + 1 < count &&
            compare(context, child_element,
                    base + (child + 1) * size) < 0)
        {
            ++child;
            child_element = base + child * size;
        }

        if (compare(context, root_element, child_element) >= 0)
            break;
        libc_sort_swap(root_element, child_element, size);
        root = child;
    }
}

void LIBC_CALL libc_qsort(void* base, size_t count, size_t size,
                          libc_compare_fn compare)
{
    unsigned char* bytes = (unsigned char*)base;
    size_t start;
    size_t end;

    /* Heapsort keeps sorting deterministic and bounded in kernel stacks. */
    if (count < 2 || size == 0)
        return;
    if (!bytes || !compare || count > SIZE_MAX / size)
        return;

    start = count / 2;
    while (start != 0)
    {
        --start;
        libc_qsort_sift_down(bytes, start, count, size, compare);
    }

    end = count;
    while (end > 1)
    {
        --end;
        libc_sort_swap(bytes, bytes + end * size, size);
        libc_qsort_sift_down(bytes, 0, end, size, compare);
    }
}

void* LIBC_CALL libc_bsearch(const void* key, const void* base,
                             size_t count, size_t size,
                             libc_compare_fn compare)
{
    size_t first = 0;
    size_t last = count;
    const unsigned char* bytes = (const unsigned char*)base;

    if (count == 0)
        return NULL;
    if (!key || !bytes || !compare || size == 0 ||
        count > SIZE_MAX / size)
        return NULL;

    while (first < last)
    {
        size_t middle = first + (last - first) / 2;
        const unsigned char* element = bytes + middle * size;
        int order = compare(key, element);

        if (order < 0)
        {
            last = middle;
        }
        else if (order > 0)
        {
            first = middle + 1;
        }
        else
        {
            return (void*)element;
        }
    }
    return NULL;
}

void LIBC_CALL libc_qsort_s(void* base, size_t count, size_t size,
                            libc_compare_s_fn compare, void* context)
{
    unsigned char* bytes = (unsigned char*)base;
    size_t start;
    size_t end;

    if (count == 0)
        return;
    if (size == 0 || !bytes || !compare)
    {
        SET_ERRNO(EINVAL);
        return;
    }
    if (count > SIZE_MAX / size)
    {
        SET_ERRNO(ERANGE);
        return;
    }

    start = count / 2;
    while (start != 0)
    {
        --start;
        libc_qsort_sift_down_s(bytes, start, count, size, compare, context);
    }

    end = count;
    while (end > 1)
    {
        --end;
        libc_sort_swap(bytes, bytes + end * size, size);
        libc_qsort_sift_down_s(bytes, 0, end, size, compare, context);
    }
}

void* LIBC_CALL libc_bsearch_s(const void* key, const void* base,
                               size_t count, size_t size,
                               libc_compare_s_fn compare, void* context)
{
    size_t first = 0;
    size_t last = count;
    const unsigned char* bytes = (const unsigned char*)base;

    if (count == 0)
        return NULL;
    if (!key || !bytes || !compare || size == 0 ||
        count > SIZE_MAX / size)
    {
        SET_ERRNO(EINVAL);
        return NULL;
    }

    while (first < last)
    {
        size_t middle = first + (last - first) / 2;
        const unsigned char* element = bytes + middle * size;
        int order = compare(context, key, element);

        if (order < 0)
        {
            last = middle;
        }
        else if (order > 0)
        {
            first = middle + 1;
        }
        else
        {
            return (void*)element;
        }
    }
    return NULL;
}

static int libc_strlen_bounded(const char* str, size_t capacity,
                               size_t* length)
{
    size_t i;

    if (!str || !length)
        return 0;
    for (i = 0; i < capacity; ++i)
    {
        if (str[i] == 0)
        {
            *length = i;
            return 1;
        }
    }
    return 0;
}

static int libc_wcslen_bounded(const wchar_t* str, size_t capacity,
                               size_t* length)
{
    size_t i;

    if (!str || !length)
        return 0;
    for (i = 0; i < capacity; ++i)
    {
        if (str[i] == L'\0')
        {
            *length = i;
            return 1;
        }
    }
    return 0;
}

static int libc_ranges_overlap(const void* left, size_t left_size,
                               const void* right, size_t right_size)
{
    uintptr_t left_address;
    uintptr_t right_address;

    if (left_size == 0 || right_size == 0)
        return 0;

    left_address = (uintptr_t)left;
    right_address = (uintptr_t)right;
    if (left_address < right_address)
        return right_address - left_address < left_size;
    return left_address - right_address < right_size;
}

static int libc_wranges_overlap(const void* left, size_t left_count,
                                const void* right, size_t right_count)
{
    return libc_ranges_overlap_units(left, left_count, sizeof(wchar_t),
                                     right, right_count, sizeof(wchar_t));
}

static int libc_ranges_overlap_units(const void* left, size_t left_count,
                                     size_t left_unit_size,
                                     const void* right, size_t right_count,
                                     size_t right_unit_size)
{
    if (left_unit_size == 0 || right_unit_size == 0 ||
        left_count > SIZE_MAX / left_unit_size ||
        right_count > SIZE_MAX / right_unit_size)
        return 1;

    return libc_ranges_overlap(left, left_count * left_unit_size,
                               right, right_count * right_unit_size);
}

static int libc_string_constraint(char* dest, size_t dest_size, int error)
{
    if (dest && dest_size != 0)
        dest[0] = 0;
    SET_ERRNO(error);
    return error;
}

static int libc_wstring_constraint(wchar_t* dest, size_t dest_size, int error)
{
    if (dest && dest_size != 0)
        dest[0] = L'\0';
    SET_ERRNO(error);
    return error;
}

static void libc_secure_zero(void* dest, size_t dest_size)
{
    volatile unsigned char* bytes = (volatile unsigned char*)dest;

    while (dest_size != 0)
    {
        *bytes++ = 0;
        --dest_size;
    }
}

static int libc_memory_constraint(void* dest, size_t dest_size, int error)
{
    if (dest && dest_size != 0)
        libc_secure_zero(dest, dest_size);
    SET_ERRNO(error);
    return error;
}

int LIBC_CALL libc_memcpy_s(void* dest, size_t dest_size,
                  const void* src, size_t count)
{
    if (!dest || dest_size == 0)
        return libc_memory_constraint(dest, dest_size, EINVAL);
    if (count > dest_size)
        return libc_memory_constraint(dest, dest_size, ERANGE);
    if (count != 0 && !src)
        return libc_memory_constraint(dest, dest_size, EINVAL);
    if (libc_ranges_overlap(dest, count, src, count))
        return libc_memory_constraint(dest, dest_size, EINVAL);

    libc_memcpy(dest, src, count);
    return 0;
}

int LIBC_CALL libc_memmove_s(void* dest, size_t dest_size,
                   const void* src, size_t count)
{
    if (!dest || dest_size == 0)
        return libc_memory_constraint(dest, dest_size, EINVAL);
    if (count > dest_size)
        return libc_memory_constraint(dest, dest_size, ERANGE);
    if (count != 0 && !src)
        return libc_memory_constraint(dest, dest_size, EINVAL);

    libc_memmove(dest, src, count);
    return 0;
}

int LIBC_CALL libc_memset_s(void* dest, size_t dest_size, int value, size_t count)
{
    volatile unsigned char* bytes;

    if (!dest || dest_size == 0)
        return libc_memory_constraint(dest, dest_size, EINVAL);
    if (count > dest_size)
        return libc_memory_constraint(dest, dest_size, ERANGE);

    /* Volatile stores preserve the required observable erase operation. */
    bytes = (volatile unsigned char*)dest;
    while (count != 0)
    {
        *bytes++ = (unsigned char)value;
        --count;
    }
    return 0;
}

int LIBC_CALL libc_wmemcpy_s(wchar_t* dest, size_t dest_size,
                   const wchar_t* src, size_t count)
{
    if (!dest || dest_size == 0)
        return libc_wstring_constraint(dest, dest_size, EINVAL);
    if (count > dest_size)
        return libc_wstring_constraint(dest, dest_size, ERANGE);
    if (count != 0 && !src)
        return libc_wstring_constraint(dest, dest_size, EINVAL);
    if (libc_wranges_overlap(dest, count, src, count))
        return libc_wstring_constraint(dest, dest_size, EINVAL);

    libc_wmemcpy(dest, src, count);
    return 0;
}

int LIBC_CALL libc_wmemmove_s(wchar_t* dest, size_t dest_size,
                    const wchar_t* src, size_t count)
{
    if (!dest || dest_size == 0)
        return libc_wstring_constraint(dest, dest_size, EINVAL);
    if (count > dest_size)
        return libc_wstring_constraint(dest, dest_size, ERANGE);
    if (count != 0 && !src)
        return libc_wstring_constraint(dest, dest_size, EINVAL);

    libc_wmemmove(dest, src, count);
    return 0;
}

int LIBC_CALL libc_wmemset_s(wchar_t* dest, size_t dest_size,
                   wchar_t value, size_t count)
{
    if (!dest || dest_size == 0)
        return libc_wstring_constraint(dest, dest_size, EINVAL);
    if (count > dest_size)
        return libc_wstring_constraint(dest, dest_size, ERANGE);

    libc_wmemset(dest, value, count);
    return 0;
}

int LIBC_CALL libc_strcpy_s(char* dest, size_t dest_size, const char* src)
{
    size_t length;

    if (!dest || dest_size == 0)
        return libc_string_constraint(dest, dest_size, EINVAL);
    if (!src)
        return libc_string_constraint(dest, dest_size, EINVAL);

    length = libc_strlen(src);
    if (length >= dest_size)
        return libc_string_constraint(dest, dest_size, ERANGE);
    if (libc_ranges_overlap(dest, length + 1, src, length + 1))
        return libc_string_constraint(dest, dest_size, EINVAL);

    libc_memmove(dest, src, length + 1);
    return 0;
}

int LIBC_CALL libc_strcat_s(char* dest, size_t dest_size, const char* src)
{
    size_t dest_length;
    size_t src_length;

    if (!dest || dest_size == 0)
        return libc_string_constraint(dest, dest_size, EINVAL);
    if (!src)
        return libc_string_constraint(dest, dest_size, EINVAL);
    if (libc_strlen_bounded(dest, dest_size, &dest_length) == 0)
        return libc_string_constraint(dest, dest_size, ERANGE);

    if (dest_length > SIZE_MAX - 1)
        return libc_string_constraint(dest, dest_size, ERANGE);
    src_length = libc_strlen(src);
    if (src_length > dest_size - dest_length - 1)
        return libc_string_constraint(dest, dest_size, ERANGE);
    if (libc_ranges_overlap(dest + dest_length, src_length + 1,
                            src, src_length + 1))
        return libc_string_constraint(dest, dest_size, EINVAL);

    libc_memmove(dest + dest_length, src, src_length + 1);
    return 0;
}

int LIBC_CALL libc_strncpy_s(char* dest, size_t dest_size,
                   const char* src, size_t count)
{
    size_t src_length = 0;
    size_t copy_length;
    size_t source_size;

    if (!dest || dest_size == 0)
        return libc_string_constraint(dest, dest_size, EINVAL);
    if (!src)
        return libc_string_constraint(dest, dest_size, EINVAL);

    if (count == _TRUNCATE)
    {
        src_length = libc_strlen(src);
        copy_length = src_length < dest_size - 1 ? src_length : dest_size - 1;
        if (libc_ranges_overlap(dest, copy_length + 1, src, src_length + 1))
            return libc_string_constraint(dest, dest_size, EINVAL);
        libc_memmove(dest, src, copy_length);
        dest[copy_length] = 0;
        if (copy_length != src_length)
        {
            SET_ERRNO(STRUNCATE);
            return STRUNCATE;
        }
        return 0;
    }

    if (count == 0)
    {
        dest[0] = 0;
        return 0;
    }

    /* strncpy_s copies count characters when the source is at least that long. */
    while (src_length < count && src[src_length] != 0)
        ++src_length;
    copy_length = src_length == count ? count : src_length;
    if (copy_length >= dest_size)
        return libc_string_constraint(dest, dest_size, ERANGE);
    source_size = src_length == count ? count : copy_length + 1;
    if (libc_ranges_overlap(dest, copy_length + 1, src, source_size))
        return libc_string_constraint(dest, dest_size, EINVAL);

    libc_memmove(dest, src, copy_length);
    dest[copy_length] = 0;
    return 0;
}

int LIBC_CALL libc_strncat_s(char* dest, size_t dest_size,
                   const char* src, size_t count)
{
    size_t dest_length;
    size_t src_length;
    size_t copy_length;
    size_t source_size;

    if (!dest || dest_size == 0)
        return libc_string_constraint(dest, dest_size, EINVAL);
    if (!src)
        return libc_string_constraint(dest, dest_size, EINVAL);
    if (libc_strlen_bounded(dest, dest_size, &dest_length) == 0)
        return libc_string_constraint(dest, dest_size, ERANGE);

    if (dest_length > SIZE_MAX - 1)
        return libc_string_constraint(dest, dest_size, ERANGE);
    if (count == _TRUNCATE)
    {
        src_length = libc_strlen(src);
        copy_length = src_length;
        if (copy_length > dest_size - dest_length - 1)
            copy_length = dest_size - dest_length - 1;
    }
    else
    {
        src_length = libc_strnlen_s(src, count);
        copy_length = src_length;
    }

    if (copy_length > dest_size - dest_length - 1)
        return libc_string_constraint(dest, dest_size, ERANGE);

    source_size = (src_length == count && count != _TRUNCATE) ?
        count : copy_length + 1;
    if (libc_ranges_overlap(dest + dest_length, copy_length + 1,
                            src, source_size))
        return libc_string_constraint(dest, dest_size, EINVAL);

    libc_memmove(dest + dest_length, src, copy_length);
    dest[dest_length + copy_length] = 0;
    if (count == _TRUNCATE && copy_length != src_length)
    {
        SET_ERRNO(STRUNCATE);
        return STRUNCATE;
    }
    return 0;
}

int LIBC_CALL libc_wcscpy_s(wchar_t* dest, size_t dest_size, const wchar_t* src)
{
    size_t length;

    if (!dest || dest_size == 0)
        return libc_wstring_constraint(dest, dest_size, EINVAL);
    if (!src)
        return libc_wstring_constraint(dest, dest_size, EINVAL);

    length = libc_wcslen(src);
    if (length >= dest_size)
        return libc_wstring_constraint(dest, dest_size, ERANGE);
    if (libc_wranges_overlap(dest, length + 1, src, length + 1))
        return libc_wstring_constraint(dest, dest_size, EINVAL);

    libc_wmemmove(dest, src, length + 1);
    return 0;
}

int LIBC_CALL libc_wcscat_s(wchar_t* dest, size_t dest_size, const wchar_t* src)
{
    size_t dest_length;
    size_t src_length;

    if (!dest || dest_size == 0)
        return libc_wstring_constraint(dest, dest_size, EINVAL);
    if (!src)
        return libc_wstring_constraint(dest, dest_size, EINVAL);
    if (libc_wcslen_bounded(dest, dest_size, &dest_length) == 0)
        return libc_wstring_constraint(dest, dest_size, ERANGE);

    if (dest_length > SIZE_MAX - 1)
        return libc_wstring_constraint(dest, dest_size, ERANGE);
    src_length = libc_wcslen(src);
    if (src_length > dest_size - dest_length - 1)
        return libc_wstring_constraint(dest, dest_size, ERANGE);
    if (libc_wranges_overlap(dest + dest_length, src_length + 1,
                             src, src_length + 1))
        return libc_wstring_constraint(dest, dest_size, EINVAL);

    libc_wmemmove(dest + dest_length, src, src_length + 1);
    return 0;
}

int LIBC_CALL libc_wcsncpy_s(wchar_t* dest, size_t dest_size,
                   const wchar_t* src, size_t count)
{
    size_t src_length = 0;
    size_t copy_length;
    size_t source_size;

    if (!dest || dest_size == 0)
        return libc_wstring_constraint(dest, dest_size, EINVAL);
    if (!src)
        return libc_wstring_constraint(dest, dest_size, EINVAL);

    if (count == _TRUNCATE)
    {
        src_length = libc_wcslen(src);
        copy_length = src_length < dest_size - 1 ? src_length : dest_size - 1;
        if (libc_wranges_overlap(dest, copy_length + 1, src,
                                 src_length + 1))
            return libc_wstring_constraint(dest, dest_size, EINVAL);
        libc_wmemmove(dest, src, copy_length);
        dest[copy_length] = L'\0';
        if (copy_length != src_length)
        {
            SET_ERRNO(STRUNCATE);
            return STRUNCATE;
        }
        return 0;
    }

    if (count == 0)
    {
        dest[0] = L'\0';
        return 0;
    }

    /* The bounded form still terminates the destination after count characters. */
    while (src_length < count && src[src_length] != L'\0')
        ++src_length;
    copy_length = src_length == count ? count : src_length;
    if (copy_length >= dest_size)
        return libc_wstring_constraint(dest, dest_size, ERANGE);
    source_size = src_length == count ? count : copy_length + 1;
    if (libc_wranges_overlap(dest, copy_length + 1, src, source_size))
        return libc_wstring_constraint(dest, dest_size, EINVAL);

    libc_wmemmove(dest, src, copy_length);
    dest[copy_length] = L'\0';
    return 0;
}

int LIBC_CALL libc_wcsncat_s(wchar_t* dest, size_t dest_size,
                   const wchar_t* src, size_t count)
{
    size_t dest_length;
    size_t src_length;
    size_t copy_length;
    size_t source_size;

    if (!dest || dest_size == 0)
        return libc_wstring_constraint(dest, dest_size, EINVAL);
    if (!src)
        return libc_wstring_constraint(dest, dest_size, EINVAL);
    if (libc_wcslen_bounded(dest, dest_size, &dest_length) == 0)
        return libc_wstring_constraint(dest, dest_size, ERANGE);

    if (dest_length > SIZE_MAX - 1)
        return libc_wstring_constraint(dest, dest_size, ERANGE);
    if (count == _TRUNCATE)
    {
        src_length = libc_wcslen(src);
        copy_length = src_length;
        if (copy_length > dest_size - dest_length - 1)
            copy_length = dest_size - dest_length - 1;
    }
    else
    {
        src_length = libc_wcsnlen_s(src, count);
        copy_length = src_length;
    }

    if (copy_length > dest_size - dest_length - 1)
        return libc_wstring_constraint(dest, dest_size, ERANGE);

    source_size = (src_length == count && count != _TRUNCATE) ?
        count : copy_length + 1;
    if (libc_wranges_overlap(dest + dest_length, copy_length + 1,
                             src, source_size))
        return libc_wstring_constraint(dest, dest_size, EINVAL);

    libc_wmemmove(dest + dest_length, src, copy_length);
    dest[dest_length + copy_length] = L'\0';
    if (count == _TRUNCATE && copy_length != src_length)
    {
        SET_ERRNO(STRUNCATE);
        return STRUNCATE;
    }
    return 0;
}

/* Keep small-radix 64-bit arithmetic freestanding on 32-bit targets. */
#if defined(_M_IX86) || defined(__i386__)
static void libc_ull_get_words(unsigned long long value,
                               uint32_t* high, uint32_t* low)
{
    union
    {
        unsigned long long value;
        uint32_t words[2];
    } parts;

    parts.value = value;
    *low = parts.words[0];
    *high = parts.words[1];
}

static unsigned long long libc_ull_make_words(uint32_t high, uint32_t low)
{
    union
    {
        unsigned long long value;
        uint32_t words[2];
    } parts;

    parts.words[0] = low;
    parts.words[1] = high;
    return parts.value;
}
#endif

static unsigned long long libc_ull_divmod_u32(
    unsigned long long value, unsigned int divisor, unsigned int* remainder)
{
#if defined(_M_IX86) || defined(__i386__)
    uint32_t high;
    uint32_t low;
    uint32_t quotient_high = 0;
    uint32_t quotient_low = 0;
    uint32_t remainder_high = 0;
    uint32_t remainder_low = 0;
    unsigned int bit_index;

    libc_ull_get_words(value, &high, &low);

    if (divisor == 0)
    {
        if (remainder)
            *remainder = 0;
        return 0;
    }

    if (high == 0)
    {
        if (remainder)
            *remainder = low % divisor;
        return libc_ull_make_words(0, low / divisor);
    }

    for (bit_index = 64U; bit_index != 0; --bit_index)
    {
        uint32_t bit;

        if (bit_index > 32U)
            bit = (high >> (bit_index - 33U)) & 1U;
        else
            bit = (low >> (bit_index - 1U)) & 1U;

        remainder_high = remainder_low >> 31;
        remainder_low = (remainder_low << 1) | bit;
        if (remainder_high != 0 || remainder_low >= divisor)
        {
            remainder_low -= divisor;
            remainder_high = 0;
            if (bit_index > 32U)
                quotient_high |= 1U << (bit_index - 33U);
            else
                quotient_low |= 1U << (bit_index - 1U);
        }
    }

    if (remainder)
        *remainder = remainder_low;
    return libc_ull_make_words(quotient_high, quotient_low);
#else
    unsigned long long quotient;

    if (divisor == 0)
    {
        if (remainder)
            *remainder = 0;
        return 0;
    }

    quotient = value / (unsigned long long)divisor;
    if (remainder)
        *remainder = (unsigned int)(value % (unsigned long long)divisor);
    return quotient;
#endif
}

/* The multiplier is a numeric base (2..36), so 16-bit limbs are sufficient. */
static unsigned long long libc_ull_mul_add_u32(
    unsigned long long value, unsigned int multiplier, unsigned int addend)
{
#if defined(_M_IX86) || defined(__i386__)
    uint32_t high;
    uint32_t low;
    uint32_t limb;
    uint32_t carry = addend;
    uint32_t result0;
    uint32_t result1;
    uint32_t result2;
    uint32_t result3;

    libc_ull_get_words(value, &high, &low);

    limb = (low & 0xFFFFU) * multiplier + carry;
    result0 = limb & 0xFFFFU;
    carry = limb >> 16;
    limb = (low >> 16) * multiplier + carry;
    result1 = limb & 0xFFFFU;
    carry = limb >> 16;
    limb = (high & 0xFFFFU) * multiplier + carry;
    result2 = limb & 0xFFFFU;
    carry = limb >> 16;
    limb = (high >> 16) * multiplier + carry;
    result3 = limb & 0xFFFFU;

    return libc_ull_make_words((result2 | (result3 << 16)),
                               (result0 | (result1 << 16)));
#else
    return value * (unsigned long long)multiplier +
           (unsigned long long)addend;
#endif
}

int LIBC_CALL libc_ltowstr(wchar_t* s, int size, long i, int base, char UpCase)
{
    wchar_t* tmp;
    int j = 0;
    int negative = 0;

    static const wchar_t wnum[] = L"0123456789abcdefghijklmnopqrstuvwxyz";

    if (s == NULL || size <= 0)
    {
        return 0;
    }

    unsigned long value;
    if (i < 0)
    {
        negative = 1;
        /* Convert before negating so LONG_MIN is handled without UB. */
        value = 0UL - (unsigned long)i;
    }
    else
    {
        value = (unsigned long)i;
    }

    if (base < 2 || base > 36)
    {
        base = 10;
    }

    if (size < 2)
    {
        s[0] = L'\0';
        return 0;
    }

    s[--size] = L'\0';  // size>=2
    tmp = s + size;

    // 0
    if (value == 0)
    {
        *(--tmp) = L'0';
        j = 1;
    }
    else
    {
        while ((tmp > s) && (value > 0))
        {
            unsigned int remainder;
            tmp--;
            value = (unsigned long)libc_ull_divmod_u32(
                (unsigned long long)value, (unsigned int)base, &remainder);
            wchar_t digit = wnum[remainder];

            // Unicode
            if (UpCase && digit >= L'a' && digit <= L'z')
            {
                digit = (wchar_t)(digit - L'a' + L'A');
            }

            *tmp = digit;
            j++;

            if (negative && (tmp == s))
            {
                *tmp = L'\0';
                return 0;
            }
        }
    }

    if (value != 0)
    {
        s[0] = L'\0';
        return 0;
    }

    if (negative)
    {
        if (tmp > s)
        {
            *(--tmp) = L'-';
            j++;
        }
        else
        {
            s[0] = L'\0';
            return 0;
        }
    }

    if (tmp != s)
    {
        libc_memmove(s, tmp, ((size_t)j + 1) * sizeof(*s));
    }

    return j;
}

int LIBC_CALL libc_ltostr(char* s, int size, long i, int base, char UpCase)
{
    if (!s || size <= 0)
        return 0;

    char* tmp;
    int j = 0;
    int negative = 0;

    /* Keep this table static so freestanding Clang does not emit a startup
     * memcpy for a local aggregate initializer. */
    static const char num[] = "0123456789abcdefghijklmnopqrstuvwxyz";

    unsigned long value;
    if (i < 0)
    {
        negative = 1;
        // LONG_MIN
        if (i == LONG_MIN)
        {
            value = (unsigned long)LONG_MAX + 1;
        }
        else
        {
            value = (unsigned long)(-i);
        }
    }
    else
    {
        value = (unsigned long)i;
    }

    if (base < 2 || base > 36)
    {
        base = 10;
    }

    if (size < 2)
    {
        s[0] = '\0';
        return 0;
    }

    s[--size] = '\0';
    tmp = s + size;

    // 0
    if (value == 0)
    {
        *(--tmp) = '0';
        j = 1;
    }
    else
    {
        while ((tmp > s) && (value > 0))
        {
            unsigned int remainder;
            tmp--;
            value = (unsigned long)libc_ull_divmod_u32(
                (unsigned long long)value, (unsigned int)base, &remainder);
            char digit = num[remainder];

            if (UpCase && digit >= 'a' && digit <= 'z')
            {
                digit = (char)(digit - 'a' + 'A');
            }

            *tmp = digit;
            j++;

            if (negative && (tmp == s))
            {
                s[0] = '\0';
                return 0;
            }
        }
    }

    if (value != 0)
    {
        s[0] = '\0';
        return 0;
    }

    if (negative)
    {
        if (tmp > s)
        {
            *(--tmp) = '-';
            j++;
        }
        else
        {
            s[0] = '\0';
            return 0;
        }
    }

    if (tmp != s)
    {
        libc_memmove(s, tmp, ((size_t)j + 1) * sizeof(*s));
    }

    return j;
}

int LIBC_CALL libc_lltowstr(wchar_t* s, int size, long long i, int base, char UpCase)
{
    wchar_t* tmp;
    int j = 0;
    int negative = 0;

    static const wchar_t wnum[] = L"0123456789abcdefghijklmnopqrstuvwxyz";

    if (s == NULL || size <= 0)
    {
        return 0;
    }

    unsigned long long value;
    if (i < 0)
    {
        negative = 1;
        if (i == LLONG_MIN)
        {
            //  -LLONG_MIN 
            value = 0ULL - (unsigned long long)i;
        }
        else
        {
            value = (unsigned long long)(-i);
        }
    }
    else
    {
        value = (unsigned long long)i;
    }

    if (base < 2 || base > 36)
    {
        base = 10;
    }

    if (size < 2)
    {
        s[0] = L'\0';
        return 0;
    }

    s[--size] = L'\0';  // size>=2
    tmp = s + size;

    // 0
    if (value == 0)
    {
        *(--tmp) = L'0';
        j = 1;
    }
    else
    {
        while ((tmp > s) && (value > 0))
        {
            unsigned int remainder;
            tmp--;
            value = libc_ull_divmod_u32(value, (unsigned int)base,
                                        &remainder);
            wchar_t digit = wnum[remainder];

            if (UpCase && digit >= L'a' && digit <= L'z')
            {
                digit = (wchar_t)(digit - L'a' + L'A');
            }

            *tmp = digit;
            j++;

            if (negative && (tmp == s))
            {
                *tmp = L'\0';
                return 0;
            }
        }
    }

    if (value != 0)
    {
        s[0] = L'\0';
        return 0;
    }

    if (negative)
    {
        if (tmp > s)
        {
            *(--tmp) = L'-';
            j++;
        }
        else
        {
            s[0] = L'\0';
            return 0;
        }
    }

    if (tmp != s)
    {
        libc_memmove(s, tmp, ((size_t)j + 1) * sizeof(*s));
    }

    return j;
}

int LIBC_CALL libc_ultostr(char* s, int size, unsigned long i, int base, char UpCase)
{
    char* tmp;
    int j = 0;

    static const char num[] = "0123456789abcdefghijklmnopqrstuvwxyz";

    if (s == NULL || size <= 0)
    {
        return 0;
    }

    if (base < 2 || base > 36)
    {
        base = 10;
    }

    if (size < 2)
    {
        s[0] = '\0';
        return 0;
    }

    s[--size] = '\0';  // size>=2
    tmp = s + size;

    // 0
    if (i == 0)
    {
        *(--tmp) = '0';
        j = 1;
    }
    else
    {
        while ((tmp > s) && (i > 0))
        {
            unsigned int remainder;
            tmp--;
            i = (unsigned long)libc_ull_divmod_u32(
                (unsigned long long)i, (unsigned int)base, &remainder);
            char digit = num[remainder];

            if (UpCase && digit >= 'a' && digit <= 'z')
            {
                digit = (char)(digit - 'a' + 'A');
            }

            *tmp = digit;
            j++;
        }
    }

    if (i != 0)
    {
        s[0] = '\0';
        return 0;
    }

    if (tmp != s)
    {
        libc_memmove(s, tmp, ((size_t)j + 1) * sizeof(*s));
    }

    return j;
}

int LIBC_CALL libc_ultowstr(wchar_t* s, int size, unsigned long i, int base, char UpCase)
{
    wchar_t* tmp;
    int j = 0;

    static const wchar_t wnum[] = L"0123456789abcdefghijklmnopqrstuvwxyz";

    if (s == NULL || size <= 0)
    {
        return 0;
    }

    if (base < 2 || base > 36)
    {
        base = 10;
    }

    if (size < 2)
    {
        s[0] = L'\0';
        return 0;
    }

    s[--size] = L'\0';  // size>=2
    tmp = s + size;

    // 0
    if (i == 0)
    {
        *(--tmp) = L'0';
        j = 1;
    }
    else
    {
        while ((tmp > s) && (i > 0))
        {
            unsigned int remainder;
            tmp--;
            i = (unsigned long)libc_ull_divmod_u32(
                (unsigned long long)i, (unsigned int)base, &remainder);
            wchar_t digit = wnum[remainder];

            // Unicode
            if (UpCase && digit >= L'a' && digit <= L'z')
            {
                digit = (wchar_t)(digit - L'a' + L'A');
            }

            *tmp = digit;
            j++;
        }
    }

    if (i != 0)
    {
        s[0] = '\0';
        return 0;
    }

    if (tmp != s)
    {
        libc_memmove(s, tmp, ((size_t)j + 1) * sizeof(*s));
    }

    return j;
}

int LIBC_CALL libc_ulltostr(char* s, int size, unsigned long long i, int base, char UpCase)
{
    char* tmp;
    int j = 0;

    static const char num[] = "0123456789abcdefghijklmnopqrstuvwxyz";

    if (s == NULL || size <= 0)
    {
        return 0;
    }

    if (base < 2 || base > 36)
    {
        base = 10;
    }

    if (size < 2)
    {
        s[0] = '\0';
        return 0;
    }

    s[--size] = '\0';  // size>=2
    tmp = s + size;

    // 0
    if (i == 0)
    {
        *(--tmp) = '0';
        j = 1;
    }
    else
    {
        while ((tmp > s) && (i > 0))
        {
            unsigned int remainder;
            tmp--;
            i = libc_ull_divmod_u32(i, (unsigned int)base, &remainder);
            char digit = num[remainder];

            if (UpCase && digit >= 'a' && digit <= 'z')
            {
                digit = (char)(digit - 'a' + 'A');
            }

            *tmp = digit;
            j++;
        }
    }

    if (i != 0)
    {
        s[0] = L'\0';
        return 0;
    }

    if (tmp != s)
    {
        libc_memmove(s, tmp, ((size_t)j + 1) * sizeof(*s));
    }

    return j;
}

int LIBC_CALL libc_ulltowstr(wchar_t* s, int size, unsigned long long i, int base, char UpCase)
{
    wchar_t* tmp;
    int j = 0;

    static const wchar_t wnum[] = L"0123456789abcdefghijklmnopqrstuvwxyz";

    if (s == NULL || size <= 0)
    {
        return 0;
    }

    if (base < 2 || base > 36)
    {
        base = 10;
    }

    if (size < 2)
    {
        s[0] = L'\0';
        return 0;
    }

    s[--size] = L'\0';  // size>=2
    tmp = s + size;

    // 0
    if (i == 0)
    {
        *(--tmp) = L'0';
        j = 1;
    }
    else
    {
        while ((tmp > s) && (i > 0))
        {
            unsigned int remainder;
            tmp--;
            i = libc_ull_divmod_u32(i, (unsigned int)base, &remainder);
            wchar_t digit = wnum[remainder];

            // Unicode
            if (UpCase && digit >= L'a' && digit <= L'z')
            {
                digit = (wchar_t)(digit - L'a' + L'A');
            }

            *tmp = digit;
            j++;
        }
    }

    if (i != 0)
    {
        s[0] = L'\0';
        return 0;
    }

    if (tmp != s)
    {
        libc_memmove(s, tmp, ((size_t)j + 1) * sizeof(*s));
    }

    return j;
}

int LIBC_CALL libc_lltostr(char* s, int size, long long i, int base, char UpCase)
{
    char* tmp;
    int j = 0;
    int negative = 0;

    static const char num[] = "0123456789abcdefghijklmnopqrstuvwxyz";

    if (s == NULL || size <= 0)
    {
        return 0;
    }

    unsigned long long value;
    if (i < 0)
    {
        negative = 1;
        if (i == LLONG_MIN)
        {
            //  -LLONG_MIN 
            value = 0ULL - (unsigned long long)i;
        }
        else
        {
            value = (unsigned long long)(-i);
        }
    }
    else
    {
        value = (unsigned long long)i;
    }

    if (base < 2 || base > 36)
    {
        base = 10;
    }

    if (size < 2)
    {
        s[0] = '\0';
        return 0;
    }

    s[--size] = '\0';  // size>=2
    tmp = s + size;

    // 0
    if (value == 0)
    {
        *(--tmp) = '0';
        j = 1;
    }
    else
    {
        while ((tmp > s) && (value > 0))
        {
            unsigned int remainder;
            tmp--;
            value = libc_ull_divmod_u32(value, (unsigned int)base,
                                        &remainder);
            char digit = num[remainder];

            if (UpCase && digit >= 'a' && digit <= 'z')
            {
                digit = (char)(digit - 'a' + 'A');
            }

            *tmp = digit;
            j++;

            if (negative && (tmp == s))
            {
                *tmp = '\0';
                return 0;
            }
        }
    }

    if (value != 0)
    {
        s[0] = '\0';
        return 0;
    }

    if (negative)
    {
        if (tmp > s)
        {
            *(--tmp) = '-';
            j++;
        }
        else
        {
            s[0] = '\0';
            return 0;
        }
    }

    if (tmp != s)
    {
        libc_memmove(s, tmp, ((size_t)j + 1) * sizeof(*s));
    }

    return j;
}

// 0-35
#if 0
static inline int get_digit_value(char c, int base)
{
    int value = -1;

    if (c >= '0' && c <= '9')
        value = c - '0';
    else if (c >= 'a' && c <= 'z')
        value = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z')
        value = c - 'A' + 10;

    return (value >= 0 && value < base) ? value : -1;
}

unsigned long LIBC_CALL libc_strtoul(const char* nptr, char** endptr, int base)
{
    unsigned long v = 0;
    int digit;
    bool overflow = false;
    bool negative = false;
    const char* start = nptr;
    const char* base_start;

    if (nptr == NULL)
    {
        if (endptr)
            *endptr = (char*)nptr;
        SET_ERRNO(EINVAL);
        return 0;
    }

    while (libc_isspace((unsigned char)*nptr)) nptr++;

    base_start = nptr;

    if (*nptr == '+' || *nptr == '-')
    {
        negative = *nptr == '-';
        if (0 && *nptr == '-')
        {
            if (endptr)
                *endptr = (char*)nptr;
            SET_ERRNO(EINVAL);
            return 0;
        }
        nptr++;
    }

    if (base == 0)
    {
        if (*nptr == '0')
        {
            if ((nptr[1] == 'x' || nptr[1] == 'X') && get_digit_value(nptr[2], 16) >= 0)
            {
                base = 16;
                nptr += 2;  // "0x"
            }
            else
            {
                base = 8;
                nptr++;  // "0"
            }
        }
        else
        {
            base = 10;
        }
    }
    else if (base == 16)
    {
        // 16"0x"
        if (nptr[0] == '0' && (nptr[1] == 'x' || nptr[1] == 'X'))
        {
            nptr += 2;
        }
    }

    if (*base_start == '+' || *base_start == '-')
        ++base_start;

    if (base == 8 && nptr == base_start + 1)
        --nptr;
    if (base == 16 && nptr == base_start + 2 &&
        get_digit_value(base_start[2], 16) < 0)
        nptr = base_start;

    if (base < 2 || base > 36)
    {
        if (endptr)
            *endptr = (char*)nptr;
        SET_ERRNO(EINVAL);
        return 0;
    }

    if (!*nptr || get_digit_value(*nptr, base) < 0)
    {
        if (endptr)
            *endptr = (char*)start;
        SET_ERRNO(EINVAL);
        return 0;
    }

    while (*nptr)
    {
        digit = get_digit_value(*nptr, base);
        if (digit < 0 || digit >= base)
        {
            break;  // 
        }

        if (v > (ULONG_MAX - (unsigned long)digit) / (unsigned int)base)
        {
            overflow = true;
            v = ULONG_MAX;
            SET_ERRNO(ERANGE);
        }
        else
        {
            v = v * (unsigned int)base + (unsigned int)digit;
        }

        nptr++;
    }

    // endptr
    if (endptr)
    {
        *endptr = (char*)nptr;
    }

    if (negative && !overflow)
        v = 0UL - v;

    return v;
}

long LIBC_CALL libc_strtol(const char* nptr, char** endptr, int base)
{
    int neg = 0;
    unsigned long v;
    char* parse_end;
    const char* start = nptr;

    if (nptr == NULL)
    {
        if (endptr)
            *endptr = (char*)nptr;
        return 0;
    }

    while (libc_isspace(*nptr)) nptr++;

    if (*nptr == '+')
    {
        nptr++;
    }
    else if (*nptr == '-')
    {
        neg = 1;
        nptr++;
    }

    v = libc_strtoul(nptr, &parse_end, base);
    if (parse_end == nptr)
    {
        if (endptr)
            *endptr = (char*)start;
        return 0;
    }
    nptr = parse_end;

    // endptr0
    if (neg)
    {
        if (v > (unsigned long)LONG_MAX + 1)
        {
#ifdef ERANGE
            SET_ERRNO(ERANGE);
#endif
            v = (unsigned long)LONG_MIN;
        }
        else
        {
            v = -v;
        }
    }
    else
    {
        if (v > LONG_MAX)
        {
#ifdef ERANGE
            SET_ERRNO(ERANGE);
#endif
            v = LONG_MAX;
        }
    }

    // endptr
    if (endptr)
    {
        *endptr = (char*)nptr;
    }

    return (long)v;
}

long long LIBC_CALL libc_strtoll(const char* nptr, char** endptr, int base)
{
    int neg = 0;
    unsigned long long v = 0;
    int digit;
    int overflow = 0;
    const char* start = nptr;

    if (nptr == NULL)
    {
        if (endptr)
            *endptr = (char*)nptr;
        return 0;
    }

    while (libc_isspace(*nptr)) nptr++;

    if (*nptr == '+')
    {
        nptr++;
    }
    else if (*nptr == '-')
    {
        neg = 1;
        nptr++;
    }

    if (base == 0)
    {
        if (*nptr == '0')
        {
            if (nptr[1] == 'x' || nptr[1] == 'X')
            {
                base = 16;
                nptr += 2;
            }
            else
            {
                base = 8;
                nptr++;
            }
        }
        else
        {
            base = 10;
        }
    }
    else if (base == 16)
    {
        if (nptr[0] == '0' && (nptr[1] == 'x' || nptr[1] == 'X'))
        {
            nptr += 2;
        }
    }

    if (base < 2 || base > 36)
    {
        if (endptr)
            *endptr = (char*)start;
        return 0;
    }

    while (*nptr)
    {
        digit = get_digit_value(*nptr, base);
        if (digit < 0 || digit >= base)
        {
            break;
        }

        if (v > (ULLONG_MAX - (unsigned long long)digit) / (unsigned int)base)
        {
            overflow = 1;
            v = ULLONG_MAX;
        }
        else
        {
            v = v * (unsigned int)base + (unsigned int)digit;
        }

        nptr++;
    }

    if (nptr == start)
    {
        if (endptr)
            *endptr = (char*)start;
        return 0;
    }

    if (neg)
    {
        if (overflow || v > (unsigned long long)LLONG_MAX + 1)
        {
#ifdef ERANGE
            SET_ERRNO(ERANGE);
#endif
            v = (unsigned long long)LLONG_MIN;
        }
        else
        {
            v = -v;
        }
    }
    else
    {
        if (overflow || v > LLONG_MAX)
        {
#ifdef ERANGE
            SET_ERRNO(ERANGE);
#endif
            v = LLONG_MAX;
        }
    }

    // endptr
    if (endptr)
    {
        *endptr = (char*)nptr;
    }

    return (long long)v;
}

unsigned long long LIBC_CALL libc_strtoull(const char* nptr, char** endptr, int base)
{
    unsigned long long v = 0;
    int digit;
    int overflow = 0;

    if (nptr == NULL)
    {
        if (endptr)
            *endptr = (char*)nptr;
        return 0;
    }

    while (libc_isspace(*nptr)) nptr++;

    if (*nptr == '+' || *nptr == '-')
    {
        if (*nptr == '-')
        {
            if (endptr)
                *endptr = (char*)nptr;
            return 0;
        }
        nptr++;
    }

    if (base == 0)
    {
        if (*nptr == '0')
        {
            if (nptr[1] == 'x' || nptr[1] == 'X')
            {
                base = 16;
                nptr += 2;
            }
            else
            {
                base = 8;
                nptr++;
            }
        }
        else
        {
            base = 10;
        }
    }
    else if (base == 16)
    {
        if (nptr[0] == '0' && (nptr[1] == 'x' || nptr[1] == 'X'))
        {
            nptr += 2;
        }
    }

    if (base < 2 || base > 36)
    {
        if (endptr)
            *endptr = (char*)nptr;
        return 0;
    }

    while (*nptr)
    {
        digit = get_digit_value(*nptr, base);
        if (digit < 0 || digit >= base)
        {
            break;
        }

        if (v > (ULLONG_MAX - digit) / base)
        {
            overflow = 1;
            v = ULLONG_MAX;
        }
        else
        {
            v = v * base + digit;
        }

        nptr++;
    }

    // endptr
    if (endptr)
    {
        *endptr = (char*)nptr;
    }

    if (overflow)
    {
#ifdef ERANGE
        SET_ERRNO(ERANGE);
#endif
    }

    return v;
}

// 0-35
static int get_wdigit_value(wchar_t c, int base)
{
    if (c >= L'0' && c <= L'9')
        return c - L'0';
    else if (c >= L'a' && c <= L'z')
        return c - L'a' + 10;
    else if (c >= L'A' && c <= L'Z')
        return c - L'A' + 10;
    return -1;  // 
}

unsigned long LIBC_CALL libc_wcstoul(const wchar_t* nptr, wchar_t** endptr, int base)
{
    unsigned long v = 0;
    int digit;
    int overflow = 0;
    const wchar_t* start = nptr;

    if (nptr == NULL)
    {
        if (endptr)
            *endptr = (wchar_t*)nptr;
        return 0;
    }

    while (libc_iswspace(*nptr)) nptr++;

    if (*nptr == L'+' || *nptr == L'-')
    {
        if (*nptr == L'-')
        {
            if (endptr)
                *endptr = (wchar_t*)nptr;
            return 0;
        }
        nptr++;
    }

    if (base == 0)
    {
        if (*nptr == L'0')
        {
            if (nptr[1] == L'x' || nptr[1] == L'X')
            {
                base = 16;
                nptr += 2;
            }
            else
            {
                base = 8;
                nptr++;
            }
        }
        else
        {
            base = 10;
        }
    }
    else if (base == 16)
    {
        if (nptr[0] == L'0' && (nptr[1] == L'x' || nptr[1] == L'X'))
        {
            nptr += 2;
        }
    }

    if (base < 2 || base > 36)
    {
        if (endptr)
            *endptr = (wchar_t*)start;
        return 0;
    }

    while (*nptr)
    {
        digit = get_wdigit_value(*nptr, base);
        if (digit < 0 || digit >= base)
        {
            break;
        }

        if (v > (ULONG_MAX - digit) / base)
        {
            overflow = 1;
            v = ULONG_MAX;
        }
        else
        {
            v = v * base + digit;
        }

        nptr++;
    }

    if (nptr == start)
    {
        if (endptr)
            *endptr = (wchar_t*)start;
        return 0;
    }

    // endptr
    if (endptr)
    {
        *endptr = (wchar_t*)nptr;
    }

    if (overflow)
    {
#ifdef ERANGE
        SET_ERRNO(ERANGE);
#endif
    }

    return v;
}

long LIBC_CALL libc_wcstol(const wchar_t* nptr, wchar_t** endptr, int base)
{
    int neg = 0;
    unsigned long v;
    const wchar_t* start = nptr;

    if (nptr == NULL)
    {
        if (endptr)
            *endptr = (wchar_t*)nptr;
        return 0;
    }

    while (libc_iswspace(*nptr)) nptr++;

    if (*nptr == L'+')
    {
        nptr++;
    }
    else if (*nptr == L'-')
    {
        neg = 1;
        nptr++;
    }

    v = libc_wcstoul(nptr, (wchar_t**)&nptr, base);

    if (nptr == start)
    {
        if (endptr)
            *endptr = (wchar_t*)start;
        return 0;
    }

    if (neg)
    {
        if (v > (unsigned long)LONG_MAX + 1)
        {
#ifdef ERANGE
            SET_ERRNO(ERANGE);
#endif
            v = (unsigned long)LONG_MIN;
        }
        else
        {
            v = -v;
        }
    }
    else
    {
        if (v > LONG_MAX)
        {
#ifdef ERANGE
            SET_ERRNO(ERANGE);
#endif
            v = LONG_MAX;
        }
    }

    // endptr
    if (endptr)
    {
        *endptr = (wchar_t*)nptr;
    }

    return (long)v;
}

long long LIBC_CALL libc_wcstoll(const wchar_t* nptr, wchar_t** endptr, int base)
{
    int neg = 0;
    unsigned long long v = 0;
    int digit;
    int overflow = 0;
    const wchar_t* start = nptr;

    if (nptr == NULL)
    {
        if (endptr)
            *endptr = (wchar_t*)nptr;
        return 0;
    }

    while (libc_iswspace(*nptr)) nptr++;

    if (*nptr == L'+')
    {
        nptr++;
    }
    else if (*nptr == L'-')
    {
        neg = 1;
        nptr++;
    }

    if (base == 0)
    {
        if (*nptr == L'0')
        {
            if (nptr[1] == L'x' || nptr[1] == L'X')
            {
                base = 16;
                nptr += 2;
            }
            else
            {
                base = 8;
                nptr++;
            }
        }
        else
        {
            base = 10;
        }
    }
    else if (base == 16)
    {
        if (nptr[0] == L'0' && (nptr[1] == L'x' || nptr[1] == L'X'))
        {
            nptr += 2;
        }
    }

    if (base < 2 || base > 36)
    {
        if (endptr)
            *endptr = (wchar_t*)start;
        return 0;
    }

    while (*nptr)
    {
        digit = get_wdigit_value(*nptr, base);
        if (digit < 0 || digit >= base)
        {
            break;
        }

        if (v > (ULLONG_MAX - digit) / base)
        {
            overflow = 1;
            v = ULLONG_MAX;
        }
        else
        {
            v = v * base + digit;
        }

        nptr++;
    }

    if (nptr == start)
    {
        if (endptr)
            *endptr = (wchar_t*)start;
        return 0;
    }

    if (neg)
    {
        if (overflow || v > (unsigned long long)LLONG_MAX + 1)
        {
#ifdef ERANGE
            SET_ERRNO(ERANGE);
#endif
            v = (unsigned long long)LLONG_MIN;
        }
        else
        {
            v = -v;
        }
    }
    else
    {
        if (overflow || v > LLONG_MAX)
        {
#ifdef ERANGE
            SET_ERRNO(ERANGE);
#endif
            v = LLONG_MAX;
        }
    }

    // endptr
    if (endptr)
    {
        *endptr = (wchar_t*)nptr;
    }

    return (long long)v;
}

unsigned long long LIBC_CALL libc_wcstoull(const wchar_t* nptr, wchar_t** endptr, int base)
{
    unsigned long long v = 0;
    int digit;
    int overflow = 0;
    const wchar_t* start = nptr;

    if (nptr == NULL)
    {
        if (endptr)
            *endptr = (wchar_t*)nptr;
        return 0;
    }

    while (libc_iswspace(*nptr)) nptr++;

    if (*nptr == L'+' || *nptr == L'-')
    {
        if (*nptr == L'-')
        {
            if (endptr)
                *endptr = (wchar_t*)nptr;
            return 0;
        }
        nptr++;
    }

    if (base == 0)
    {
        if (*nptr == L'0')
        {
            if (nptr[1] == L'x' || nptr[1] == L'X')
            {
                base = 16;
                nptr += 2;
            }
            else
            {
                base = 8;
                nptr++;
            }
        }
        else
        {
            base = 10;
        }
    }
    else if (base == 16)
    {
        if (nptr[0] == L'0' && (nptr[1] == L'x' || nptr[1] == L'X'))
        {
            nptr += 2;
        }
    }

    if (base < 2 || base > 36)
    {
        if (endptr)
            *endptr = (wchar_t*)start;
        return 0;
    }

    while (*nptr)
    {
        digit = get_wdigit_value(*nptr, base);
        if (digit < 0 || digit >= base)
        {
            break;
        }

        if (v > (ULLONG_MAX - digit) / base)
        {
            overflow = 1;
            v = ULLONG_MAX;
        }
        else
        {
            v = v * base + digit;
        }

        nptr++;
    }

    if (nptr == start)
    {
        if (endptr)
            *endptr = (wchar_t*)start;
        return 0;
    }

    // endptr
    if (endptr)
    {
        *endptr = (wchar_t*)nptr;
    }

    if (overflow)
    {
#ifdef ERANGE
        SET_ERRNO(ERANGE);
#endif
    }

    return v;
}

#endif

static int libc_digit_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    return -1;
}

static int libc_wdigit_value(wchar_t c)
{
    if (c >= L'0' && c <= L'9')
        return (int)(c - L'0');
    if (c >= L'a' && c <= L'z')
        return (int)(c - L'a' + 10);
    if (c >= L'A' && c <= L'Z')
        return (int)(c - L'A' + 10);
    return -1;
}

/* Parse a bounded unsigned magnitude and keep the end pointer exact. */
static int libc_parse_integer(const char* input, int base,
                              unsigned long long positive_limit,
                              unsigned long long negative_limit,
                              unsigned long long* value, int* negative,
                              const char** end)
{
    const char* p = input;
    unsigned long long result = 0;
    unsigned long long limit;
    unsigned int radix;
    int digit;
    int overflow = 0;

    /* Keep every output defined, including the no-conversion path. */
    *value = 0;
    *negative = 0;
    *end = input;

    while (libc_isspace((unsigned char)*p))
        ++p;

    *negative = 0;
    if (*p == '+' || *p == '-')
    {
        *negative = *p == '-';
        ++p;
    }

    if (base != 0 && (base < 2 || base > 36))
    {
        *end = input;
        SET_ERRNO(EINVAL);
        return -1;
    }

    if ((base == 0 || base == 16) && p[0] == '0' &&
        (p[1] == 'x' || p[1] == 'X'))
    {
        digit = libc_digit_value(p[2]);
        if (digit >= 0 && digit < 16)
        {
            base = 16;
            p += 2;
        }
        else
        {
            /* The 0x prefix requires at least one hexadecimal digit. */
            *end = input;
            return 0;
        }
    }
    else if (base == 0)
    {
        base = p[0] == '0' ? 8 : 10;
    }

    radix = (unsigned int)base;
    digit = libc_digit_value(*p);
    if (digit < 0 || (unsigned int)digit >= radix)
    {
        *end = input;
        return 0;
    }

    limit = *negative ? negative_limit : positive_limit;
    do
    {
        digit = libc_digit_value(*p);
        if (digit < 0 || (unsigned int)digit >= radix)
            break;

        if (result > libc_ull_divmod_u32(
                limit - (unsigned int)digit, radix, NULL))
        {
            result = limit;
            overflow = 1;
        }
        else if (!overflow)
        {
            result = libc_ull_mul_add_u32(
                result, radix, (unsigned int)digit);
        }
        ++p;
    } while (*p);

    *value = result;
    *end = p;
    return !overflow;
}

static int libc_parse_winteger(const wchar_t* input, int base,
                               unsigned long long positive_limit,
                               unsigned long long negative_limit,
                               unsigned long long* value, int* negative,
                               const wchar_t** end)
{
    const wchar_t* p = input;
    unsigned long long result = 0;
    unsigned long long limit;
    unsigned int radix;
    int digit;
    int overflow = 0;

    /* Keep every output defined, including the no-conversion path. */
    *value = 0;
    *negative = 0;
    *end = input;

    while (libc_iswspace(*p))
        ++p;

    *negative = 0;
    if (*p == L'+' || *p == L'-')
    {
        *negative = *p == L'-';
        ++p;
    }

    if (base != 0 && (base < 2 || base > 36))
    {
        *end = input;
        SET_ERRNO(EINVAL);
        return -1;
    }

    if ((base == 0 || base == 16) && p[0] == L'0' &&
        (p[1] == L'x' || p[1] == L'X'))
    {
        digit = libc_wdigit_value(p[2]);
        if (digit >= 0 && digit < 16)
        {
            base = 16;
            p += 2;
        }
        else
        {
            /* The 0x prefix requires at least one hexadecimal digit. */
            *end = input;
            return 0;
        }
    }
    else if (base == 0)
    {
        base = p[0] == L'0' ? 8 : 10;
    }

    radix = (unsigned int)base;
    digit = libc_wdigit_value(*p);
    if (digit < 0 || (unsigned int)digit >= radix)
    {
        *end = input;
        return 0;
    }

    limit = *negative ? negative_limit : positive_limit;
    do
    {
        digit = libc_wdigit_value(*p);
        if (digit < 0 || (unsigned int)digit >= radix)
            break;

        if (result > libc_ull_divmod_u32(
                limit - (unsigned int)digit, radix, NULL))
        {
            result = limit;
            overflow = 1;
        }
        else if (!overflow)
        {
            result = libc_ull_mul_add_u32(
                result, radix, (unsigned int)digit);
        }
        ++p;
    } while (*p);

    *value = result;
    *end = p;
    return !overflow;
}

unsigned long LIBC_CALL libc_strtoul(const char* nptr, char** endptr, int base)
{
    unsigned long long value;
    const char* end;
    int negative;
    int valid;

    if (!nptr)
    {
        if (endptr)
            *endptr = NULL;
        SET_ERRNO(EINVAL);
        return 0;
    }

    valid = libc_parse_integer(nptr, base,
                               (unsigned long long)ULONG_MAX,
                               (unsigned long long)ULONG_MAX,
                               &value, &negative, &end);
    if (endptr)
        *endptr = (char*)end;
    if (!valid)
    {
        if (valid < 0)
        {
            SET_ERRNO(EINVAL);
            return 0;
        }
        if (end != nptr)
            SET_ERRNO(ERANGE);
        return end != nptr ? ULONG_MAX : 0;
    }

    return negative ? 0UL - (unsigned long)value : (unsigned long)value;
}

long LIBC_CALL libc_strtol(const char* nptr, char** endptr, int base)
{
    unsigned long long value;
    const char* end;
    int negative;
    int valid;

    if (!nptr)
    {
        if (endptr)
            *endptr = NULL;
        SET_ERRNO(EINVAL);
        return 0;
    }

    valid = libc_parse_integer(nptr, base,
                               (unsigned long long)LONG_MAX,
                               (unsigned long long)LONG_MAX + 1ULL,
                               &value, &negative, &end);
    if (endptr)
        *endptr = (char*)end;
    if (!valid)
    {
        if (valid < 0)
        {
            SET_ERRNO(EINVAL);
            return 0;
        }
        if (end != nptr)
            SET_ERRNO(ERANGE);
        return end != nptr ? (negative ? LONG_MIN : LONG_MAX) : 0;
    }

    if (negative)
    {
        if (value == (unsigned long long)LONG_MAX + 1ULL)
            return LONG_MIN;
        return -(long)value;
    }
    return (long)value;
}

long long LIBC_CALL libc_strtoll(const char* nptr, char** endptr, int base)
{
    unsigned long long value;
    const char* end;
    int negative;
    int valid;

    if (!nptr)
    {
        if (endptr)
            *endptr = NULL;
        SET_ERRNO(EINVAL);
        return 0;
    }

    valid = libc_parse_integer(nptr, base, (unsigned long long)LLONG_MAX,
                               (unsigned long long)LLONG_MAX + 1ULL,
                               &value, &negative, &end);
    if (endptr)
        *endptr = (char*)end;
    if (!valid)
    {
        if (valid < 0)
        {
            SET_ERRNO(EINVAL);
            return 0;
        }
        if (end != nptr)
            SET_ERRNO(ERANGE);
        return end != nptr ? (negative ? LLONG_MIN : LLONG_MAX) : 0;
    }

    if (negative)
    {
        if (value == (unsigned long long)LLONG_MAX + 1ULL)
            return LLONG_MIN;
        return -(long long)value;
    }
    return (long long)value;
}

unsigned long long LIBC_CALL libc_strtoull(const char* nptr, char** endptr, int base)
{
    unsigned long long value;
    const char* end;
    int negative;
    int valid;

    if (!nptr)
    {
        if (endptr)
            *endptr = NULL;
        SET_ERRNO(EINVAL);
        return 0;
    }

    valid = libc_parse_integer(nptr, base, ULLONG_MAX, ULLONG_MAX,
                               &value, &negative, &end);
    if (endptr)
        *endptr = (char*)end;
    if (!valid)
    {
        if (valid < 0)
        {
            SET_ERRNO(EINVAL);
            return 0;
        }
        if (end != nptr)
            SET_ERRNO(ERANGE);
        return end != nptr ? ULLONG_MAX : 0;
    }

    return negative ? 0ULL - value : value;
}

unsigned long LIBC_CALL libc_wcstoul(const wchar_t* nptr, wchar_t** endptr, int base)
{
    unsigned long long value;
    const wchar_t* end;
    int negative;
    int valid;

    if (!nptr)
    {
        if (endptr)
            *endptr = NULL;
        SET_ERRNO(EINVAL);
        return 0;
    }

    valid = libc_parse_winteger(nptr, base,
                                (unsigned long long)ULONG_MAX,
                                (unsigned long long)ULONG_MAX,
                                &value, &negative, &end);
    if (endptr)
        *endptr = (wchar_t*)end;
    if (!valid)
    {
        if (valid < 0)
        {
            SET_ERRNO(EINVAL);
            return 0;
        }
        if (end != nptr)
            SET_ERRNO(ERANGE);
        return end != nptr ? ULONG_MAX : 0;
    }

    return negative ? 0UL - (unsigned long)value : (unsigned long)value;
}

long LIBC_CALL libc_wcstol(const wchar_t* nptr, wchar_t** endptr, int base)
{
    unsigned long long value;
    const wchar_t* end;
    int negative;
    int valid;

    if (!nptr)
    {
        if (endptr)
            *endptr = NULL;
        SET_ERRNO(EINVAL);
        return 0;
    }

    valid = libc_parse_winteger(nptr, base,
                                (unsigned long long)LONG_MAX,
                                (unsigned long long)LONG_MAX + 1ULL,
                                &value, &negative, &end);
    if (endptr)
        *endptr = (wchar_t*)end;
    if (!valid)
    {
        if (valid < 0)
        {
            SET_ERRNO(EINVAL);
            return 0;
        }
        if (end != nptr)
            SET_ERRNO(ERANGE);
        return end != nptr ? (negative ? LONG_MIN : LONG_MAX) : 0;
    }

    if (negative)
    {
        if (value == (unsigned long long)LONG_MAX + 1ULL)
            return LONG_MIN;
        return -(long)value;
    }
    return (long)value;
}

long long LIBC_CALL libc_wcstoll(const wchar_t* nptr, wchar_t** endptr, int base)
{
    unsigned long long value;
    const wchar_t* end;
    int negative;
    int valid;

    if (!nptr)
    {
        if (endptr)
            *endptr = NULL;
        SET_ERRNO(EINVAL);
        return 0;
    }

    valid = libc_parse_winteger(nptr, base, (unsigned long long)LLONG_MAX,
                                (unsigned long long)LLONG_MAX + 1ULL,
                                &value, &negative, &end);
    if (endptr)
        *endptr = (wchar_t*)end;
    if (!valid)
    {
        if (valid < 0)
        {
            SET_ERRNO(EINVAL);
            return 0;
        }
        if (end != nptr)
            SET_ERRNO(ERANGE);
        return end != nptr ? (negative ? LLONG_MIN : LLONG_MAX) : 0;
    }

    if (negative)
    {
        if (value == (unsigned long long)LLONG_MAX + 1ULL)
            return LLONG_MIN;
        return -(long long)value;
    }
    return (long long)value;
}

unsigned long long LIBC_CALL libc_wcstoull(const wchar_t* nptr, wchar_t** endptr, int base)
{
    unsigned long long value;
    const wchar_t* end;
    int negative;
    int valid;

    if (!nptr)
    {
        if (endptr)
            *endptr = NULL;
        SET_ERRNO(EINVAL);
        return 0;
    }

    valid = libc_parse_winteger(nptr, base, ULLONG_MAX, ULLONG_MAX,
                                &value, &negative, &end);
    if (endptr)
        *endptr = (wchar_t*)end;
    if (!valid)
    {
        if (valid < 0)
        {
            SET_ERRNO(EINVAL);
            return 0;
        }
        if (end != nptr)
            SET_ERRNO(ERANGE);
        return end != nptr ? ULLONG_MAX : 0;
    }

    return negative ? 0ULL - value : value;
}

static uint32_t libc_rotr32(uint32_t value, unsigned int bits)
{
    bits &= 31U;
    if (bits == 0)
        return value;
    return (value >> bits) | (value << (32U - bits));
}

uint32_t LIBC_CALL libc_strhash(const char* str)
{
    uint32_t hash = 0;

    if (!str)
        return 0;

    while (*str)
    {
        hash = libc_rotr32(hash, 13) + (uint32_t)(unsigned char)*str++;
    }

    return hash;
}

uint32_t LIBC_CALL libc_strnhash(const char* str, size_t length)
{
    uint32_t hash = 0;

    if (!str)
        return 0;

    for (; length != 0 && *str != 0; --length, ++str)
    {
        unsigned char c = (unsigned char)*str;
        hash = libc_rotr32(hash, 13) + (uint32_t)c;
    }

    return hash;
}

#if 0
static size_t libc_vsnprintf_legacy(char* str, size_t size, const char* format, va_list arg_ptr)
{
    size_t apos, i;
    char ch, buf[1024];
    char* pb;
    char flag_in_sign;
    char flag_hash, flag_zero, flag_left, flag_space, flag_sign, flag_dot, flag_long, flag_sizet;
    size_t number, width, preci, buf_len, pad;
    char padwith;

    size--;

    apos = 0;
    while (apos < size)
    {
        ch = *format++;
        switch (ch)
        {
        case '%':
            flag_hash = 0;
            flag_zero = 0;
            flag_left = 0;
            flag_space = 0;
            flag_sign = 0;
            flag_dot = 0;
            flag_in_sign = 0;
            flag_long = 0;
            flag_sizet = 0;

            width = 0;
            padwith = ' ';

        inn_vsnprintf:
            if (apos >= size)
                continue; /* ARGL !!! */

            ch = *format++;
            switch (ch)
            {
                /* Format end ?!? */
            case 0:
                return -1;
                break;

                /* Format flag chars */
            case '#':
                flag_hash = 1;
                goto inn_vsnprintf;

            case 'l':
                ++flag_long;
                goto inn_vsnprintf;

            case '0':
                padwith = '0';
                goto inn_vsnprintf;

            case '-':
                flag_left = 1;
                goto inn_vsnprintf;

            case ' ':
                flag_space = 1;
                goto inn_vsnprintf;

            case '+':
                flag_sign = 1;
                goto inn_vsnprintf;

            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
                if (flag_dot)
                    return -1;
                width = libc_strtol(--format, &pb, 10);
                format = pb;
                goto inn_vsnprintf;

            case '.':
                flag_dot = 1;
                preci = libc_strtol(format, &pb, 10);
                format = pb;
                goto inn_vsnprintf;

                /* Format conversion chars */
            case 'c':
                ch = (char)va_arg(arg_ptr, int);
            case '%':
                if (str)
                    str[apos] = ch;
                ++apos;
                break;

            case 's':
                pb = va_arg(arg_ptr, char*);
#ifdef WANT_NULL_PRINTF
                if (!pb)
                    pb = "(null)";
#endif
                buf_len = libc_strlen(pb);

            print_out:
                if (str)
                {
                    if (width && (!flag_left))
                    {
                        for (pad = width - buf_len; pad > 0; --pad) str[apos++] = padwith;
                    }
                    for (i = 0; (pb[i]) && (apos < size); i++)
                    {
                        str[apos++] = pb[i];
                    } /* strncpy */
                    if (width && (flag_left))
                    {
                        for (pad = width - buf_len; pad > 0; --pad) str[apos++] = padwith;
                    }
                }
                else
                {
                    if (width)
                        apos += width;
                    else
                    {
                        size_t a = libc_strlen(pb);
                        if (a > size)
                            apos += size;
                        else
                            apos += a;
                    }
                }

                break;

                /* Numbers */
            case 'b':
                i = 2;
                goto num_vsnprintf;
            case 'p':
                flag_hash = 1;
                flag_sizet = 1;
                width = sizeof(void*) << 1;
                padwith = '0';
                ch = 'x';
            case 'X':
            case 'x':
                i = 16;
                if (flag_hash)
                {
                    if (str)
                    {
                        str[apos++] = '0';
                        str[apos++] = ch;
                    }
                    else
                        apos += 2;
                }
                goto num_vsnprintf;
            case 'd':
            case 'i':
                flag_in_sign = 1;
            case 'u':
                i = 10;
                goto num_vsnprintf;
            case 'o':
                i = 8;
                if (flag_hash)
                {
                    if (str)
                        str[apos] = '0';
                    ++apos;
                }

            num_vsnprintf:
                if (apos >= size)
                    continue; /* ARGL !!! */

                if (flag_long)
                    number = va_arg(arg_ptr, long);
                else if (flag_sizet)
                    number = va_arg(arg_ptr, size_t);
                else
                    number = va_arg(arg_ptr, int);

                if (flag_in_sign && /*(number<0)*/ 0)
                {
                    number *= -1;
                    flag_in_sign = 2;
                }

                if (flag_sizet)
                    buf_len = libc_lltostr(buf + 1, sizeof(buf) - 1, number, (int)i, 0);
                else
                    buf_len = libc_ltostr(buf + 1, sizeof(buf) - 1, (unsigned long)number, (int)i, 0);

                pb = buf + 1;

                if (flag_in_sign == 2)
                {
                    *(--pb) = '-';
                    buf_len++;
                }
                else if ((flag_in_sign) && (flag_sign || flag_space))
                {
                    *(--pb) = (flag_sign) ? '+' : ' ';
                    buf_len++;
                }
                goto print_out;

#ifndef LIBC_NO_FLOATING_POINT
            case 'g':
            {
                double d = va_arg(arg_ptr, double);
                buf_len = libc_dtostr(d, buf, sizeof(buf), 6);
                pb = buf;
                goto print_out;
            }
#endif
            default:
                break;
            }
            break;
        case 0:
            if (str)
                str[apos] = 0;
            return apos;
        default:
            if (str)
                str[apos] = ch;
            apos++;
            break;
        }
    }
    if (str)
        str[apos] = 0;
    return apos;
}

#endif

typedef struct libc_printf_state
{
    char* buffer;
    size_t capacity;
    size_t length;
    int error;
} libc_printf_state;

static void libc_printf_add_length(libc_printf_state* state, size_t count)
{
    if (count > SIZE_MAX - state->length)
        state->length = SIZE_MAX;
    else
        state->length += count;
}

static void libc_printf_putc(libc_printf_state* state, char ch)
{
    if (state->length < state->capacity)
        state->buffer[state->length] = ch;

    libc_printf_add_length(state, 1);
}

static void libc_printf_putn(libc_printf_state* state, const char* text, size_t length)
{
    size_t writable = 0;

    if (state->length < state->capacity)
    {
        writable = state->capacity - state->length;
        if (writable > length)
            writable = length;
    }

    if (writable != 0)
        libc_memmove(state->buffer + state->length, text, writable);

    libc_printf_add_length(state, length);
}

static void libc_printf_repeat(libc_printf_state* state, char ch, size_t count)
{
    size_t writable = 0;
    volatile unsigned char* output;

    if (state->length < state->capacity)
    {
        writable = state->capacity - state->length;
        if (writable > count)
            writable = count;
    }

    if (writable != 0)
    {
        output = (volatile unsigned char*)(state->buffer + state->length);
        while (writable != 0)
        {
            *output++ = (unsigned char)ch;
            --writable;
        }
    }

    libc_printf_add_length(state, count);
}

static size_t libc_printf_number_digits(char* digits, unsigned long long value,
                                        unsigned int base, int upper)
{
    static const char lower_digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    static const char upper_digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    const char* table = upper ? upper_digits : lower_digits;
    size_t length = 0;
    size_t i;

    do
    {
        unsigned int remainder;
        value = libc_ull_divmod_u32(value, base, &remainder);
        digits[length++] = table[remainder];
    } while (value != 0);

    for (i = 0; i < length / 2; ++i)
    {
        char ch = digits[i];
        digits[i] = digits[length - i - 1];
        digits[length - i - 1] = ch;
    }

    return length;
}

static void libc_printf_number(libc_printf_state* state,
                               unsigned long long value,
                               char sign,
                               unsigned int base,
                               int upper,
                               size_t width,
                               int precision,
                               int left,
                               int zero,
                               int alternate,
                               int pointer_value)
{
    char digits[65];
    size_t digits_length;
    size_t padded_digits;
    size_t prefix_length = 0;
    size_t content_length;
    size_t padding;
    int emit_prefix = 0;

    digits_length = libc_printf_number_digits(digits, value, base, upper);
    if (precision == 0 && value == 0)
        digits_length = 0;

    padded_digits = digits_length;
    if (precision > 0 && padded_digits < (size_t)precision)
        padded_digits = (size_t)precision;

    if (alternate || pointer_value)
    {
        if (base == 16 && (value != 0 || pointer_value))
        {
            prefix_length = 2;
            emit_prefix = 1;
        }
        else if (base == 8 && precision >= 0 && value != 0 &&
                 padded_digits == digits_length)
        {
            ++padded_digits;
        }
        else if (base == 8 && precision >= 0 && value == 0 &&
                 digits_length == 0)
        {
            prefix_length = 1;
            emit_prefix = 1;
        }
        else if (base == 8 && precision < 0 && value != 0)
        {
            prefix_length = 1;
            emit_prefix = 1;
        }
        else if (base == 2 && value != 0)
        {
            prefix_length = 2;
            emit_prefix = 2;
        }
    }

    content_length = padded_digits;
    if (prefix_length > SIZE_MAX - content_length)
        content_length = SIZE_MAX;
    else
        content_length += prefix_length;
    if (sign != 0)
    {
        if (content_length == SIZE_MAX)
            content_length = SIZE_MAX;
        else
            ++content_length;
    }

    padding = width > content_length ? width - content_length : 0;

    if (!left && !(zero && precision < 0))
        libc_printf_repeat(state, ' ', padding);

    if (sign != 0)
        libc_printf_putc(state, sign);

    if (emit_prefix == 1)
    {
        libc_printf_putc(state, '0');
        if (base == 16)
            libc_printf_putc(state, upper ? 'X' : 'x');
    }
    else if (emit_prefix == 2)
    {
        libc_printf_putc(state, '0');
        libc_printf_putc(state, 'b');
    }

    if (!left && zero && precision < 0)
        libc_printf_repeat(state, '0', padding);
    else if (padded_digits > digits_length)
        libc_printf_repeat(state, '0', padded_digits - digits_length);

    libc_printf_putn(state, digits, digits_length);

    if (left)
        libc_printf_repeat(state, ' ', padding);
}

static void libc_printf_string(libc_printf_state* state, const char* text,
                               size_t width, int precision, int left)
{
    size_t length = 0;
    size_t padding;

    if (!text)
    {
        state->error = EINVAL;
        return;
    }

    /* A precision must bound the read as well as the number of bytes written. */
    if (precision >= 0)
    {
        while (length < (size_t)precision && text[length] != 0)
            ++length;
    }
    else
    {
        length = libc_strlen(text);
    }

    padding = width > length ? width - length : 0;
    if (!left)
        libc_printf_repeat(state, ' ', padding);
    libc_printf_putn(state, text, length);
    if (left)
        libc_printf_repeat(state, ' ', padding);
}

static int libc_vsnprintf_s_impl(char* dest, size_t dest_size, size_t count,
                                 int truncate, const char* format,
                                 va_list args)
{
    libc_printf_state state;
    size_t capacity;

    if (!dest || dest_size == 0 || !format)
    {
        if (dest && dest_size != 0)
            dest[0] = 0;
        SET_ERRNO(EINVAL);
        return -1;
    }

    capacity = dest_size - 1;
    if (!truncate && capacity > count)
        capacity = count;

    state.buffer = dest;
    state.capacity = capacity;
    state.length = 0;
    state.error = 0;

    while (*format)
    {
        char conversion;
        size_t width = 0;
        int precision = -1;
        int alternate = 0;
        int zero = 0;
        int left = 0;
        int plus = 0;
        int space = 0;
        int length_modifier = 0;

        if (*format != '%')
        {
            libc_printf_putc(&state, *format++);
            continue;
        }

        ++format;
        if (*format == '%')
        {
            libc_printf_putc(&state, '%');
            ++format;
            continue;
        }

        for (;;)
        {
            if (*format == '#')
                alternate = 1;
            else if (*format == '0')
                zero = 1;
            else if (*format == '-')
                left = 1;
            else if (*format == '+')
                plus = 1;
            else if (*format == ' ')
                space = 1;
            else
                break;
            ++format;
        }

        if (*format == '*')
        {
            int width_value = va_arg(args, int);
            if (width_value < 0)
            {
                left = 1;
                width = (size_t)(-(width_value + 1)) + 1;
            }
            else
            {
                width = (size_t)width_value;
            }
            ++format;
        }
        else
        {
            while (libc_isdigit((unsigned char)*format))
            {
                size_t digit = (size_t)(*format - '0');
                if (width > (SIZE_MAX - digit) / 10)
                    width = SIZE_MAX;
                else
                    width = width * 10 + digit;
                ++format;
            }
        }

        if (*format == '.')
        {
            precision = 0;
            ++format;
            if (*format == '*')
            {
                precision = va_arg(args, int);
                if (precision < 0)
                    precision = -1;
                ++format;
            }
            else
            {
                while (libc_isdigit((unsigned char)*format))
                {
                    int digit = *format - '0';
                    if (precision > (INT_MAX - digit) / 10)
                        precision = INT_MAX;
                    else
                        precision = precision * 10 + digit;
                    ++format;
                }
            }
        }

        if (*format == 'h')
        {
            length_modifier = 1;
            ++format;
            if (*format == 'h')
            {
                length_modifier = 5;
                ++format;
            }
        }
        else if (*format == 'l')
        {
            length_modifier = 2;
            ++format;
            if (*format == 'l')
            {
                length_modifier = 3;
                ++format;
            }
        }
        else if (*format == 'z')
        {
            length_modifier = 4;
            ++format;
        }
        else if (*format == 'j')
        {
            length_modifier = 6;
            ++format;
        }
        else if (*format == 't')
        {
            length_modifier = 7;
            ++format;
        }
        else if (*format == 'I')
        {
            length_modifier = 4;
            ++format;
            if (format[0] == '3' && format[1] == '2')
            {
                length_modifier = 0;
                format += 2;
            }
            else if (format[0] == '6' && format[1] == '4')
            {
                length_modifier = 3;
                format += 2;
            }
        }

        conversion = *format;
        if (conversion != 0)
            ++format;

        switch (conversion)
        {
        case 'c':
            if (length_modifier != 0)
            {
                state.error = EINVAL;
                break;
            }
            if (!left)
            {
                if (width > 1)
                    libc_printf_repeat(&state, ' ', width - 1);
            }
            libc_printf_putc(&state, (char)va_arg(args, int));
            if (left && width > 1)
                libc_printf_repeat(&state, ' ', width - 1);
            break;

        case 's':
        {
            const char* text;
            if (length_modifier != 0)
            {
                state.error = EINVAL;
                break;
            }
            text = va_arg(args, const char*);
            libc_printf_string(&state, text, width, precision, left);
            break;
        }

        case 'd':
        case 'i':
        {
            long long signed_value;
            unsigned long long magnitude;
            char sign = 0;

            if (length_modifier == 3)
                signed_value = va_arg(args, long long);
            else if (length_modifier == 2)
                signed_value = (long long)va_arg(args, long);
            else if (length_modifier == 4 || length_modifier == 7)
                signed_value = (long long)va_arg(args, ptrdiff_t);
            else if (length_modifier == 6)
                signed_value = (long long)va_arg(args, intmax_t);
            else
                signed_value = (long long)va_arg(args, int);

            if (length_modifier == 1)
                signed_value = (short)signed_value;
            else if (length_modifier == 5)
                signed_value = (signed char)signed_value;

            if (signed_value < 0)
            {
                magnitude = 0ULL - (unsigned long long)signed_value;
                sign = '-';
            }
            else
            {
                magnitude = (unsigned long long)signed_value;
                if (plus)
                    sign = '+';
                else if (space)
                    sign = ' ';
            }

            libc_printf_number(&state, magnitude, sign, 10, 0, width,
                               precision, left, zero, 0, 0);
            break;
        }

        case 'u':
        case 'o':
        case 'b':
        case 'x':
        case 'X':
        {
            unsigned long long value;
            unsigned int base = conversion == 'o' ? 8 :
                                 (conversion == 'b' ? 2 :
                                  (conversion == 'u' ? 10 : 16));
            int upper = conversion == 'X';

            if (length_modifier == 3)
                value = va_arg(args, unsigned long long);
            else if (length_modifier == 2)
                value = (unsigned long long)va_arg(args, unsigned long);
            else if (length_modifier == 4 || length_modifier == 7)
                value = (unsigned long long)va_arg(args, size_t);
            else if (length_modifier == 6)
                value = (unsigned long long)va_arg(args, uintmax_t);
            else if (length_modifier == 1 || length_modifier == 5)
                value = (unsigned long long)(unsigned int)va_arg(args, int);
            else
                value = (unsigned long long)va_arg(args, unsigned int);

            if (length_modifier == 1)
                value = (unsigned short)value;
            else if (length_modifier == 5)
                value = (unsigned char)value;

            libc_printf_number(&state, value, 0, base, upper, width,
                               precision, left, zero, alternate, 0);
            break;
        }

        case 'p':
        {
            void* pointer = va_arg(args, void*);
            if (width == 0)
                width = sizeof(void*) * 2 + 2;
            libc_printf_number(&state,
                               (unsigned long long)(uintptr_t)pointer,
                               0, 16, 0, width, -1, 0, 1, 1, 1);
            break;
        }

#ifndef LIBC_NO_FLOATING_POINT
        case 'g':
        {
            char float_buffer[128];
            int float_length = libc_dtostr(va_arg(args, double),
                                           float_buffer,
                                           (int)sizeof(float_buffer),
                                           precision >= 0 ? precision : 6);
            if (float_length > 0)
                libc_printf_string(&state, float_buffer, width, -1, left);
            break;
        }
#endif

        case 0:
            state.error = EINVAL;
            break;

        default:
            state.error = EINVAL;
            break;
        }

        if (state.error)
            break;
    }

    if (state.error || state.length > (size_t)INT_MAX)
    {
        dest[0] = 0;
        SET_ERRNO(EINVAL);
        return -1;
    }

    if (state.length >= dest_size || (!truncate && state.length > count))
    {
        if (truncate && state.length >= dest_size)
        {
            dest[dest_size - 1] = 0;
            SET_ERRNO(ERANGE);
            return -1;
        }

        dest[0] = 0;
        SET_ERRNO(ERANGE);
        return -1;
    }

    dest[state.length] = 0;
    return (int)state.length;
}

int LIBC_CALL libc_vsnprintf_s(char* dest, size_t dest_size, size_t count,
                     const char* format, va_list args)
{
    return libc_vsnprintf_s_impl(dest, dest_size, count,
                                  count == _TRUNCATE, format, args);
}

int LIBC_CALL libc_snprintf_s(char* dest, size_t dest_size, size_t count,
                    const char* format, ...)
{
    int result;
    va_list args;

    va_start(args, format);
    result = libc_vsnprintf_s(dest, dest_size, count, format, args);
    va_end(args);
    return result;
}

int LIBC_CALL libc_vsprintf_s(char* dest, size_t dest_size,
                    const char* format, va_list args)
{
    return libc_vsnprintf_s_impl(dest, dest_size, SIZE_MAX, 0, format, args);
}

int LIBC_CALL libc_sprintf_s(char* dest, size_t dest_size, const char* format, ...)
{
    int result;
    va_list args;

    va_start(args, format);
    result = libc_vsprintf_s(dest, dest_size, format, args);
    va_end(args);
    return result;
}

// error LNK2001:  _fltused
#if !defined(LIBC_NO_FLOATING_POINT) && defined(_MSC_VER) && _MSC_VER >= 1930
int _fltused = 0;
#endif

#ifndef LIBC_NO_FLOATING_POINT
static int libc_ascii_equal_n(const char* left, const char* right,
                              size_t count)
{
    size_t i;

    for (i = 0; i < count; ++i)
    {
        char left_value = left[i];
        char right_value = right[i];

        if (left_value == 0 || right_value == 0)
            return 0;
        if (left_value >= 'A' && left_value <= 'Z')
            left_value = (char)(left_value + ('a' - 'A'));
        if (right_value >= 'A' && right_value <= 'Z')
            right_value = (char)(right_value + ('a' - 'A'));
        if (left_value != right_value)
            return 0;
    }
    return 1;
}

double LIBC_CALL libc_strtod(const char* nptr, char** endptr)
{
    double value = 0.0;
    const char* start = nptr;
    const char* c;
    int negative = 0;
    int has_digits = 0;

    if (!nptr)
    {
        if (endptr)
            *endptr = (char*)nptr;
        return 0.0;
    }

    c = nptr;
    while (libc_isspace((unsigned char)*c))
        ++c;

    if (*c == '+' || *c == '-')
    {
        negative = *c == '-';
        ++c;
    }

    if (libc_ascii_equal_n(c, "infinity", 8))
    {
        unsigned long long special_bits = 0x7ff0000000000000ULL;
        if (negative)
            special_bits |= 1ULL << 63;
        libc_memcpy(&value, &special_bits, sizeof(value));
        c += 8;
        if (endptr)
            *endptr = (char*)c;
        return value;
    }
    if (libc_ascii_equal_n(c, "inf", 3))
    {
        unsigned long long special_bits = 0x7ff0000000000000ULL;
        if (negative)
            special_bits |= 1ULL << 63;
        libc_memcpy(&value, &special_bits, sizeof(value));
        c += 3;
        if (endptr)
            *endptr = (char*)c;
        return value;
    }
    if (libc_ascii_equal_n(c, "nan", 3))
    {
        unsigned long long special_bits = 0x7ff8000000000000ULL;
        if (negative)
            special_bits |= 1ULL << 63;
        libc_memcpy(&value, &special_bits, sizeof(value));
        c += 3;
        if (endptr)
            *endptr = (char*)c;
        return value;
    }

    while (libc_isdigit((unsigned char)*c))
    {
        value = value * 10.0 + (double)(*c - '0');
        ++c;
        has_digits = 1;
    }

    if (*c == '.')
    {
        double factor = 0.1;
        ++c;
        while (libc_isdigit((unsigned char)*c))
        {
            value += factor * (double)(*c - '0');
            factor /= 10.0;
            ++c;
            has_digits = 1;
        }
    }

    if (!has_digits)
    {
        if (endptr)
            *endptr = (char*)start;
        return 0.0;
    }

    if (*c == 'e' || *c == 'E')
    {
        const char* exponent_start = c;
        int exponent_negative = 0;
        int exponent = 0;
        int exponent_digits = 0;

        ++c;
        if (*c == '+' || *c == '-')
        {
            exponent_negative = *c == '-';
            ++c;
        }

        while (libc_isdigit((unsigned char)*c))
        {
            if (exponent < 4096)
            {
                exponent = exponent * 10 + (*c - '0');
                if (exponent > 4096)
                    exponent = 4096;
            }
            ++c;
            exponent_digits = 1;
        }

        if (!exponent_digits)
        {
            c = exponent_start;
        }
        else
        {
            {
                double factor = 10.0;
                int remaining = exponent;

                /* Exponentiation by squaring keeps extreme exponents bounded. */
                while (remaining > 0)
                {
                    if (remaining & 1)
                    {
                        if (exponent_negative)
                            value /= factor;
                        else
                            value *= factor;
                    }
                    remaining >>= 1;
                    if (remaining > 0)
                        factor *= factor;
                }
            }
        }
    }

    if (negative)
        value = -value;

    if (endptr)
        *endptr = (char*)c;
    return value;
}
#endif /* LIBC_NO_FLOATING_POINT */

#if 0
int LIBC_CALL libc_vsscanf(const char* str, const char* format, va_list arg_ptr)
{
    int n = 0, div;
    char ch;

    char flag_discard, flag_malloc, flag_half, flag_long, flag_longlong;
    char flag_width;

    unsigned long width;

    /* arg_ptr tmps */
    // double d, *pd;
    // float *pf;

    long l = 0, * pl;
    short* ph;
    int* pi;
    char* s = 0;

    while ((*str) && (*format))
    {
        const char* prevfmt = format;
        format = __skip_ws(format);
        ch = *format++;
        if (!ch)
            continue;

        switch (ch)
        {
        case '%':
            div = 0;
            flag_discard = 0;
            flag_malloc = 0;
            flag_half = 0;
            flag_long = 0;
            flag_longlong = 0;

            flag_width = 0;
            width = -1;

        inn_vsscanf:
            ch = *format++;

            switch (ch)
            {
            case 0:
                return 0;

            case '%':
                if (*(str++) != ch)
                    return n;
                break;

                /* flags */
            case '*':
                flag_discard = 1;
                goto inn_vsscanf;

            case 'a':
                flag_malloc = 1;
                goto inn_vsscanf;

            case 'h':
                flag_half = 1;
                goto inn_vsscanf;

            case 'l':
                if (flag_long)
                    flag_longlong = 1;
                flag_long = 1;
                goto inn_vsscanf;

                /* longlong ? NOT YET ! */
            case 'q':
            case 'L':
                flag_longlong = 1;
                goto inn_vsscanf;

            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
                width = libc_strtol(format - 1, &s, 10);
                format = s;
                flag_width = 1;
                goto inn_vsscanf;

                /* conversion */

            case 'n':
                while (width && *str)
                {
                    *(s++) = *(str++);
                    --width;
                    l++;
                }
                if (!flag_discard)
                {
                    pl = (long*)va_arg(arg_ptr, long*);
                    *pl = l;
                    ++n;
                }
                break;

            case 'p':
            case 'X':
            case 'x':
                div += 6;
            case 'd':
                div += 2;
            case 'o':
                div += 8;
            case 'u':
            case 'i':
                if (*(str = __skip_ws(str)))
                {
                    l = libc_strtol(str, &s, div);
                    if (str != s)
                    {
                        if (!flag_discard)
                        {
                            if (flag_long)
                            {
                                pl = (long*)va_arg(arg_ptr, long*);
                                *pl = l;
                            }
                            else if (flag_half)
                            {
                                ph = (short*)va_arg(arg_ptr, short*);
                                *ph = (short)l;
                            }
                            else
                            {
                                pi = (int*)va_arg(arg_ptr, int*);
                                *pi = l;
                            }
                            ++n;
                        }
                        str = s;
                    }
                    else
                        return n;
                }
                break;

#ifndef LIBC_NO_FLOATING_POINT
            case 'e':
            case 'E':
            case 'f':
            case 'g':
                if (*(str = __skip_ws(str)))
                {
                    d = libc_strtod(str, &s);
                    if (str != s)
                    {
                        if (!flag_discard)
                        {
                            if (flag_long)
                            {
                                pd = (double*)va_arg(arg_ptr, double*);
                                *pd = d;
                            }
                            else
                            {
                                pf = (float*)va_arg(arg_ptr, float*);
                                *pf = (float)d;
                            }
                            ++n;
                        }
                        str = s;
                    }
                    else
                        return n;
                }
                break;
#endif

            case 'c':
                if (!flag_discard)
                {
                    s = (char*)va_arg(arg_ptr, char*);
                    ++n;
                }
                if (!flag_width)
                    width = 1;
                while (width && *str)
                {
                    if (!flag_discard)
                        *(s++) = *(str);
                    ++str;
                    --width;
                }
                break;

            case 's':
                if (!flag_discard)
                {
                    s = (char*)va_arg(arg_ptr, char*);
                    ++n;
                }
                if (*(str = __skip_ws(str)))
                {
                    while (width && (!libc_isspace(*str)))
                    {
                        if (!flag_discard)
                            *(s++) = *(str);
                        if (!*str)
                            break;
                        ++str;
                        --width;
                    }
                }
                break;
            }
            break;

        default:
            if (prevfmt < format)
            {
                while (prevfmt < format)
                {
                    if (*str != *prevfmt)
                        return n;
                    ++str;
                    ++prevfmt;
                }
            }
            else if (*(str++) != ch)
                return n;
            break;
        }
    }
    return n;
}

#endif

static int libc_scan_digit(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'z')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'Z')
        return ch - 'A' + 10;
    return -1;
}

static int libc_scan_unsigned(const char* input, size_t width, int base,
                              unsigned long long* value, int* negative,
                              size_t* consumed)
{
    size_t pos = 0;
    int digit;
    int any = 0;
    int overflow = 0;
    unsigned long long result = 0;

    *value = 0;
    *negative = 0;
    *consumed = 0;
    if (width == 0)
    {
        return 0;
    }

    if (pos < width && (input[pos] == '+' || input[pos] == '-'))
    {
        *negative = input[pos] == '-';
        ++pos;
    }

    if (base == 0)
    {
        if (width - pos >= 3 && input[pos] == '0' &&
            (input[pos + 1] == 'x' || input[pos + 1] == 'X') &&
            libc_scan_digit(input[pos + 2]) >= 0 &&
            libc_scan_digit(input[pos + 2]) < 16)
        {
            base = 16;
            pos += 2;
        }
        else if (pos < width && input[pos] == '0')
        {
            base = 8;
        }
        else
        {
            base = 10;
        }
    }
    else if (base == 16 && width - pos >= 3 && input[pos] == '0' &&
             (input[pos + 1] == 'x' || input[pos + 1] == 'X') &&
             libc_scan_digit(input[pos + 2]) >= 0 &&
             libc_scan_digit(input[pos + 2]) < 16)
    {
        pos += 2;
    }

    if (base < 2 || base > 36)
    {
        return 0;
    }

    while (pos < width && input[pos])
    {
        digit = libc_scan_digit(input[pos]);
        if (digit < 0 || digit >= base)
            break;

        any = 1;
        if (result > libc_ull_divmod_u32(
                ULLONG_MAX - (unsigned int)digit,
                (unsigned int)base, NULL))
        {
            result = ULLONG_MAX;
            overflow = 1;
        }
        else if (!overflow)
        {
            result = libc_ull_mul_add_u32(
                result, (unsigned int)base, (unsigned int)digit);
        }
        ++pos;
    }

    if (!any)
    {
        return 0;
    }

    *value = result;
    *consumed = pos;
    return !overflow;
}

#ifndef LIBC_NO_FLOATING_POINT
static int libc_scan_double(const char* input, size_t width, double* value,
                            size_t* consumed)
{
    size_t pos = 0;
    double result = 0.0;
    int negative = 0;
    int any_digits = 0;
    int exponent_negative = 0;
    unsigned int exponent = 0;

    *value = 0.0;
    *consumed = 0;
    if (width == 0)
        return 0;

    if (pos < width && (input[pos] == '+' || input[pos] == '-'))
    {
        negative = input[pos] == '-';
        ++pos;
    }

    while (pos < width && libc_isdigit((unsigned char)input[pos]))
    {
        result = result * 10.0 + (double)(input[pos] - '0');
        ++pos;
        any_digits = 1;
    }

    if (pos < width && input[pos] == '.')
    {
        double factor = 0.1;
        ++pos;
        while (pos < width && libc_isdigit((unsigned char)input[pos]))
        {
            result += factor * (double)(input[pos] - '0');
            factor *= 0.1;
            ++pos;
            any_digits = 1;
        }
    }

    if (!any_digits)
        return 0;

    if (pos < width && (input[pos] == 'e' || input[pos] == 'E'))
    {
        size_t exponent_start = pos;
        unsigned int exponent_digits = 0;

        ++pos;
        if (pos < width && (input[pos] == '+' || input[pos] == '-'))
        {
            exponent_negative = input[pos] == '-';
            ++pos;
        }
        while (pos < width && libc_isdigit((unsigned char)input[pos]))
        {
            if (exponent < 4096U)
            {
                exponent = exponent * 10U +
                           (unsigned int)(input[pos] - '0');
                if (exponent > 4096U)
                    exponent = 4096U;
            }
            ++pos;
            exponent_digits = 1;
        }

        if (!exponent_digits)
            pos = exponent_start;
    }

    if (exponent != 0)
    {
        double factor = 10.0;
        unsigned int remaining = exponent;

        /* Exponentiation by squaring avoids thousands of floating operations. */
        while (remaining != 0)
        {
            if (remaining & 1U)
            {
                if (exponent_negative)
                    result /= factor;
                else
                    result *= factor;
            }
            remaining >>= 1;
            if (remaining != 0)
                factor *= factor;
        }
    }

    *value = negative ? -result : result;
    *consumed = pos;
    return 1;
}
#endif

static int libc_scan_set_contains(const char* set_start, const char* set_end,
                                  int inverted, unsigned char value)
{
    const char* p = set_start;
    int found = 0;

    while (p < set_end)
    {
        unsigned char first = (unsigned char)*p++;

        if ((size_t)(set_end - p) >= 2 && p[0] == '-')
        {
            unsigned char last = (unsigned char)p[1];
            if (first <= last && value >= first && value <= last)
                found = 1;
            else if (value == first || value == '-' || value == last)
                found = 1;
            p += 2;
        }
        else if (value == first)
        {
            found = 1;
        }
    }

    return inverted ? !found : found;
}

static const char* libc_scan_skip_space(const char* input)
{
    while (*input && libc_isspace((unsigned char)*input))
        ++input;
    return input;
}

static int libc_scan_constraint(int error);

static int libc_scan_destination_valid(const void* destination)
{
    if (destination)
        return 1;
    libc_scan_constraint(EINVAL);
    return 0;
}

static int libc_size_exceeds_ull(size_t value, unsigned long long limit)
{
    return (uintmax_t)value > (uintmax_t)limit;
}

static int libc_pointer_value_fits(unsigned long long value)
{
#if UINTPTR_MAX < ULLONG_MAX
    return value <= (unsigned long long)UINTPTR_MAX;
#else
    (void)value;
    return 1;
#endif
}

static int libc_scan_signed_fits(unsigned long long magnitude,
                                 int negative, int length_modifier)
{
    unsigned long long positive_limit;

    switch (length_modifier)
    {
    case 1:
        positive_limit = (unsigned long long)SHRT_MAX;
        break;
    case 5:
        positive_limit = (unsigned long long)SCHAR_MAX;
        break;
    case 2:
        positive_limit = (unsigned long long)LONG_MAX;
        break;
    case 3:
        positive_limit = (unsigned long long)LLONG_MAX;
        break;
    case 4:
    case 7:
        positive_limit = (unsigned long long)PTRDIFF_MAX;
        break;
    case 6:
        positive_limit = (unsigned long long)INTMAX_MAX;
        break;
    default:
        positive_limit = (unsigned long long)INT_MAX;
        break;
    }

    if (!negative)
        return magnitude <= positive_limit;
    if (positive_limit == ULLONG_MAX)
        return magnitude <= positive_limit;
    return magnitude <= positive_limit + 1ULL;
}

static int libc_scan_unsigned_fits(unsigned long long magnitude,
                                   int negative, int length_modifier)
{
    unsigned long long limit;

    switch (length_modifier)
    {
    case 1:
        limit = (unsigned long long)USHRT_MAX;
        break;
    case 5:
        limit = (unsigned long long)UCHAR_MAX;
        break;
    case 2:
        limit = (unsigned long long)ULONG_MAX;
        break;
    case 3:
        limit = (unsigned long long)ULLONG_MAX;
        break;
    case 4:
        limit = (unsigned long long)SIZE_MAX;
        break;
    case 6:
        limit = (unsigned long long)UINTMAX_MAX;
        break;
    case 7:
        limit = (unsigned long long)SIZE_MAX;
        break;
    default:
        limit = (unsigned long long)UINT_MAX;
        break;
    }

    if (!negative || limit == ULLONG_MAX)
        return magnitude <= limit;
    return magnitude <= limit + 1ULL;
}

static int libc_vsscanf_impl(const char* str, const char* format,
                             va_list arg_ptr, int secure)
{
    const char* input = str;
    const char* input_start = str;
    int assigned = 0;

    if (!str || !format)
    {
        if (secure)
        {
            SET_ERRNO(EINVAL);
            return -1;
        }
        return 0;
    }

    while (*format)
    {
        if (libc_isspace((unsigned char)*format))
        {
            do
            {
                ++format;
            } while (libc_isspace((unsigned char)*format));
            input = libc_scan_skip_space(input);
            continue;
        }

        if (*format != '%')
        {
            if (*input != *format)
                break;
            ++input;
            ++format;
            continue;
        }

        ++format;
        if (*format == '%')
        {
            if (*input != '%')
                break;
            ++input;
            ++format;
            continue;
        }

        {
            int discard = 0;
            int length_modifier = 0;
            size_t width = SIZE_MAX;
            char conversion;

            if (*format == '*')
            {
                discard = 1;
                ++format;
            }

            if (libc_isdigit((unsigned char)*format))
            {
                width = 0;
                while (libc_isdigit((unsigned char)*format))
                {
                    size_t digit = (size_t)(*format - '0');
                    if (width > (SIZE_MAX - digit) / 10)
                        width = SIZE_MAX;
                    else
                        width = width * 10 + digit;
                    ++format;
                }
            }

            if (*format == 'h')
            {
                length_modifier = 1;
                ++format;
                if (*format == 'h')
                {
                    length_modifier = 5;
                    ++format;
                }
            }
            else if (*format == 'l')
            {
                length_modifier = 2;
                ++format;
                if (*format == 'l')
                {
                    length_modifier = 3;
                    ++format;
                }
            }
            else if (*format == 'z')
            {
                length_modifier = 4;
                ++format;
            }
            else if (*format == 'j')
            {
                length_modifier = 6;
                ++format;
            }
            else if (*format == 't')
            {
                length_modifier = 7;
                ++format;
            }
            else if (*format == 'I')
            {
                length_modifier = 4;
                ++format;
                if (format[0] == '3' && format[1] == '2')
                {
                    length_modifier = 0;
                    format += 2;
                }
                else if (format[0] == '6' && format[1] == '4')
                {
                    length_modifier = 3;
                    format += 2;
                }
            }

            conversion = *format++;
            switch (conversion)
            {
            case 'c':
            {
                char* destination = NULL;
                size_t count = width == SIZE_MAX ? 1 : width;
                size_t destination_size = 0;
                size_t i;

                if (length_modifier != 0)
                    return secure ? libc_scan_constraint(EINVAL) : assigned;
                if (count == 0)
                    break;
                for (i = 0; i < count && input[i]; ++i)
                    ;
                if (i != count)
                    return assigned;
                if (!discard)
                {
                    destination = va_arg(arg_ptr, char*);
                    if (secure)
                        destination_size = (size_t)va_arg(arg_ptr, unsigned);
                }
                if (!discard && !libc_scan_destination_valid(destination))
                    return -1;
                if (!discard && secure && count > destination_size)
                {
                    if (destination_size != 0)
                        destination[0] = 0;
                    return libc_scan_constraint(ERANGE);
                }
                if (!discard)
                {
                    libc_memmove(destination, input, count);
                    ++assigned;
                }
                input += count;
                break;
            }

            case 's':
            {
                const char* token_start;
                size_t count = 0;
                char* destination = NULL;
                size_t destination_size = 0;

                if (length_modifier != 0)
                    return secure ? libc_scan_constraint(EINVAL) : assigned;
                input = libc_scan_skip_space(input);
                token_start = input;
                while (count < width && input[count] &&
                       !libc_isspace((unsigned char)input[count]))
                    ++count;
                if (count == 0)
                    return assigned;
                if (!discard)
                {
                    destination = va_arg(arg_ptr, char*);
                    if (secure)
                        destination_size = (size_t)va_arg(arg_ptr, unsigned);
                }
                if (!discard && !libc_scan_destination_valid(destination))
                    return -1;
                if (!discard && secure && count >= destination_size)
                {
                    if (destination_size != 0)
                        destination[0] = 0;
                    return libc_scan_constraint(ERANGE);
                }
                if (!discard)
                {
                    libc_memmove(destination, token_start, count);
                    destination[count] = 0;
                    ++assigned;
                }
                input += count;
                break;
            }

            case '[':
            {
                const char* set_start = format;
                const char* set_end;
                int inverted = 0;
                size_t count = 0;
                char* destination = NULL;
                size_t destination_size = 0;

                if (length_modifier != 0)
                    return secure ? libc_scan_constraint(EINVAL) : assigned;
                if (*set_start == '^')
                {
                    inverted = 1;
                    ++set_start;
                }
                format = set_start;
                if (*format == ']')
                    ++format;
                while (*format && *format != ']')
                    ++format;
                if (*format != ']')
                    return secure ? libc_scan_constraint(EINVAL) : assigned;
                set_end = format;
                ++format;

                while (count < width && input[count] != 0 &&
                       libc_scan_set_contains(set_start, set_end, inverted,
                                              (unsigned char)input[count]))
                    ++count;
                if (count == 0)
                    return assigned;
                if (!discard)
                {
                    destination = va_arg(arg_ptr, char*);
                    if (secure)
                        destination_size = (size_t)va_arg(arg_ptr, unsigned);
                }
                if (!discard && !libc_scan_destination_valid(destination))
                    return -1;
                if (!discard && secure && count >= destination_size)
                {
                    if (destination_size != 0)
                        destination[0] = 0;
                    return libc_scan_constraint(ERANGE);
                }
                if (!discard)
                {
                    libc_memmove(destination, input, count);
                    destination[count] = 0;
                    ++assigned;
                }
                input += count;
                break;
            }

#ifndef LIBC_NO_FLOATING_POINT
            case 'e':
            case 'E':
            case 'f':
            case 'F':
            case 'g':
            case 'G':
            {
                double value;
                size_t count;

                if (length_modifier != 0 && length_modifier != 2)
                    return secure ? libc_scan_constraint(EINVAL) : assigned;
                input = libc_scan_skip_space(input);
                if (!libc_scan_double(input, width, &value, &count))
                    return assigned;
                input += count;

                if (!discard)
                {
                    if (length_modifier == 0)
                    {
                        float* destination = va_arg(arg_ptr, float*);
                        if (!libc_scan_destination_valid(destination))
                            return -1;
                        *destination = (float)value;
                    }
                    else if (length_modifier == 2)
                    {
                        double* destination = va_arg(arg_ptr, double*);
                        if (!libc_scan_destination_valid(destination))
                            return -1;
                        *destination = value;
                    }
                    ++assigned;
                }
                break;
            }
#endif

            case 'n':
            {
                size_t count = (size_t)(input - input_start);
                if (!discard)
                {
                    if (length_modifier == 1)
                    {
                        short* destination = va_arg(arg_ptr, short*);
                        if (!libc_scan_destination_valid(destination))
                            return -1;
                        if (secure && count > (size_t)SHRT_MAX)
                            return libc_scan_constraint(ERANGE);
                        *destination = (short)count;
                    }
                    else if (length_modifier == 5)
                    {
                        signed char* destination = va_arg(arg_ptr, signed char*);
                        if (!libc_scan_destination_valid(destination))
                            return -1;
                        if (secure && count > (size_t)SCHAR_MAX)
                            return libc_scan_constraint(ERANGE);
                        *destination = (signed char)count;
                    }
                    else if (length_modifier == 2)
                    {
                        long* destination = va_arg(arg_ptr, long*);
                        if (!libc_scan_destination_valid(destination))
                            return -1;
                        if (secure && count > (size_t)LONG_MAX)
                            return libc_scan_constraint(ERANGE);
                        *destination = (long)count;
                    }
                    else if (length_modifier == 3)
                    {
                        long long* destination = va_arg(arg_ptr, long long*);
                        if (!libc_scan_destination_valid(destination))
                            return -1;
                        if (secure && libc_size_exceeds_ull(
                                count, (unsigned long long)LLONG_MAX))
                            return libc_scan_constraint(ERANGE);
                        *destination = (long long)count;
                    }
                    else if (length_modifier == 4)
                    {
                        size_t* destination = va_arg(arg_ptr, size_t*);
                        if (!libc_scan_destination_valid(destination))
                            return -1;
                        *destination = count;
                    }
                    else if (length_modifier == 6)
                    {
                        intmax_t* destination = va_arg(arg_ptr, intmax_t*);
                        if (!libc_scan_destination_valid(destination))
                            return -1;
                        if (secure && libc_size_exceeds_ull(
                                count, (unsigned long long)INTMAX_MAX))
                            return libc_scan_constraint(ERANGE);
                        *destination = (intmax_t)count;
                    }
                    else if (length_modifier == 7)
                    {
                        ptrdiff_t* destination = va_arg(arg_ptr, ptrdiff_t*);
                        if (!libc_scan_destination_valid(destination))
                            return -1;
                        if (secure && count > (size_t)PTRDIFF_MAX)
                            return libc_scan_constraint(ERANGE);
                        *destination = (ptrdiff_t)count;
                    }
                    else
                    {
                        int* destination = va_arg(arg_ptr, int*);
                        if (!libc_scan_destination_valid(destination))
                            return -1;
                        if (secure && count > (size_t)INT_MAX)
                            return libc_scan_constraint(ERANGE);
                        *destination = (int)count;
                    }
                }
                break;
            }

            case 'p':
            case 'd':
            case 'i':
            case 'u':
            case 'o':
            case 'x':
            case 'X':
            {
                unsigned long long value = 0;
                size_t count = 0;
                int negative = 0;
                int base;
                int signed_conversion = conversion == 'd' || conversion == 'i';
                int parsed_without_overflow;

                input = libc_scan_skip_space(input);
                if (conversion == 'i')
                    base = 0;
                else if (conversion == 'o')
                    base = 8;
                else if (conversion == 'x' || conversion == 'X' || conversion == 'p')
                    base = 16;
                else
                    base = 10;

                parsed_without_overflow = libc_scan_unsigned(
                    input, width, base, &value, &negative, &count);
                if (count == 0)
                    return assigned;
                input += count;

                if (conversion == 'p')
                {
                    if (!discard)
                    {
                        void** destination = va_arg(arg_ptr, void**);
                        if (!libc_scan_destination_valid(destination))
                            return -1;
                        if (secure && (!parsed_without_overflow ||
                                       !libc_pointer_value_fits(value)))
                            return libc_scan_constraint(ERANGE);
                        *destination = (void*)(uintptr_t)value;
                        ++assigned;
                    }
                    break;
                }

                if (signed_conversion)
                {
                    long long signed_value;
                    unsigned long long limit = negative ?
                        (unsigned long long)LLONG_MAX + 1ULL :
                        (unsigned long long)LLONG_MAX;
                    if (!discard && secure &&
                        (!parsed_without_overflow ||
                         !libc_scan_signed_fits(value, negative,
                                                length_modifier)))
                        return libc_scan_constraint(ERANGE);
                    if (!parsed_without_overflow || value > limit)
                        signed_value = negative ? LLONG_MIN : LLONG_MAX;
                    else if (negative)
                        signed_value = value == (unsigned long long)LLONG_MAX + 1ULL ?
                            LLONG_MIN : -(long long)value;
                    else
                        signed_value = (long long)value;

                    if (!discard)
                    {
                        if (length_modifier == 1)
                        {
                            short* destination = va_arg(arg_ptr, short*);
                            if (!libc_scan_destination_valid(destination))
                                return -1;
                            *destination = (short)signed_value;
                        }
                        else if (length_modifier == 5)
                        {
                            signed char* destination = va_arg(arg_ptr, signed char*);
                            if (!libc_scan_destination_valid(destination))
                                return -1;
                            *destination = (signed char)signed_value;
                        }
                        else if (length_modifier == 2)
                        {
                            long* destination = va_arg(arg_ptr, long*);
                            if (!libc_scan_destination_valid(destination))
                                return -1;
                            *destination = (long)signed_value;
                        }
                        else if (length_modifier == 3)
                        {
                            long long* destination = va_arg(arg_ptr, long long*);
                            if (!libc_scan_destination_valid(destination))
                                return -1;
                            *destination = signed_value;
                        }
                        else if (length_modifier == 4)
                        {
                            ptrdiff_t* destination = va_arg(arg_ptr, ptrdiff_t*);
                            if (!libc_scan_destination_valid(destination))
                                return -1;
                            *destination = (ptrdiff_t)signed_value;
                        }
                        else if (length_modifier == 6)
                        {
                            intmax_t* destination = va_arg(arg_ptr, intmax_t*);
                            if (!libc_scan_destination_valid(destination))
                                return -1;
                            *destination = (intmax_t)signed_value;
                        }
                        else if (length_modifier == 7)
                        {
                            ptrdiff_t* destination = va_arg(arg_ptr, ptrdiff_t*);
                            if (!libc_scan_destination_valid(destination))
                                return -1;
                            *destination = (ptrdiff_t)signed_value;
                        }
                        else
                        {
                            int* destination = va_arg(arg_ptr, int*);
                            if (!libc_scan_destination_valid(destination))
                                return -1;
                            *destination = (int)signed_value;
                        }
                        ++assigned;
                    }
                }
                else if (!discard)
                {
                    if (secure &&
                        (!parsed_without_overflow ||
                         !libc_scan_unsigned_fits(value, negative,
                                                  length_modifier)))
                        return libc_scan_constraint(ERANGE);
                    if (negative)
                        value = 0ULL - value;
                    if (length_modifier == 1)
                    {
                        unsigned short* destination = va_arg(arg_ptr, unsigned short*);
                        if (!libc_scan_destination_valid(destination))
                            return -1;
                        *destination = (unsigned short)value;
                    }
                    else if (length_modifier == 5)
                    {
                        unsigned char* destination = va_arg(arg_ptr, unsigned char*);
                        if (!libc_scan_destination_valid(destination))
                            return -1;
                        *destination = (unsigned char)value;
                    }
                    else if (length_modifier == 2)
                    {
                        unsigned long* destination = va_arg(arg_ptr, unsigned long*);
                        if (!libc_scan_destination_valid(destination))
                            return -1;
                        *destination = (unsigned long)value;
                    }
                    else if (length_modifier == 3)
                    {
                        unsigned long long* destination = va_arg(arg_ptr, unsigned long long*);
                        if (!libc_scan_destination_valid(destination))
                            return -1;
                        *destination = value;
                    }
                    else if (length_modifier == 4)
                    {
                        size_t* destination = va_arg(arg_ptr, size_t*);
                        if (!libc_scan_destination_valid(destination))
                            return -1;
                        *destination = (size_t)value;
                    }
                    else if (length_modifier == 6)
                    {
                        uintmax_t* destination = va_arg(arg_ptr, uintmax_t*);
                        if (!libc_scan_destination_valid(destination))
                            return -1;
                        *destination = (uintmax_t)value;
                    }
                    else if (length_modifier == 7)
                    {
                        size_t* destination = va_arg(arg_ptr, size_t*);
                        if (!libc_scan_destination_valid(destination))
                            return -1;
                        *destination = (size_t)value;
                    }
                    else
                    {
                        unsigned int* destination = va_arg(arg_ptr, unsigned int*);
                        if (!libc_scan_destination_valid(destination))
                            return -1;
                        *destination = (unsigned int)value;
                    }
                    ++assigned;
                }
                break;
            }

            default:
                return secure ? libc_scan_constraint(EINVAL) : assigned;
            }
        }
    }

    return assigned;
}

static int libc_scan_constraint(int error)
{
    SET_ERRNO(error);
    return -1;
}

int LIBC_CALL libc_vsscanf(const char* str, const char* format, va_list arg_ptr)
{
    return libc_vsscanf_impl(str, format, arg_ptr, 0);
}

int LIBC_CALL libc_vsscanf_s(const char* str, const char* format, va_list arg_ptr)
{
    return libc_vsscanf_impl(str, format, arg_ptr, 1);
}

int LIBC_CALL libc_sscanf(const char* str, const char* format, ...)
{
    int n;
    va_list arg_ptr;
    va_start(arg_ptr, format);
    n = libc_vsscanf(str, format, arg_ptr);
    va_end(arg_ptr);
    return n;
}

int LIBC_CALL libc_sscanf_s(const char* str, const char* format, ...)
{
    int n;
    va_list arg_ptr;
    va_start(arg_ptr, format);
    n = libc_vsscanf_s(str, format, arg_ptr);
    va_end(arg_ptr);
    return n;
}

/* convert double to string.  Helper for sprintf. */
#ifndef LIBC_NO_FLOATING_POINT
#if 0
int LIBC_CALL libc_dtostr(double d, char* buf, int maxlen, int prec)
{
    unsigned long long bits;
    unsigned long long exponent;
    unsigned long long fraction;
    const char* special = NULL;

    if (!buf || maxlen <= 0)
        return 0;

    libc_memcpy(&bits, &d, sizeof(bits));
    exponent = (bits >> 52) & 0x7ffULL;
    fraction = bits & ((1ULL << 52) - 1);

    if (exponent == 0x7ffULL)
    {
        if (fraction != 0)
            special = "nan";
        else if ((bits >> 63) != 0)
            special = "-inf";
        else
            special = "inf";
    }
    else if (d == 0.0)
    {
        special = (bits >> 63) != 0 ? "-0" : "0";
    }

    if (special)
    {
        size_t special_length = libc_strlen(special);
        size_t i;
        if (special_length >= (size_t)maxlen)
            return 0;
        for (i = 0; i < special_length; ++i)
            buf[i] = special[i];
        buf[special_length] = 0;
        return (int)special_length;
    }

    /* step 1: extract sign, mantissa and exponent */
    signed int s = (int)(bits >> 63);
    signed long e = (signed long)(((bits >> 52) & 0x7ffULL) - 1023);
    /*  unsigned long long m=*x & ((1ull<<52)-1); */
    /* step 2: exponent is base 2, compute exponent for base 10 */
    signed long e10 = 1 + (long)(e * 0.30102999566398119802); /* log10(2) */
    /* step 3: calculate 10^e10 */
    int i;
    double tmp = 10.0;
    char* oldbuf = buf;
    int initial = 1;

    if (s)
    {
        d = -d;
        *buf = '-';
        --maxlen;
        buf++;
    }
    if ((i = e10) >= 0)
    {
        while (i > 10)
        {
            tmp = tmp * 1e10;
            i -= 10;
        }
        while (i > 1)
        {
            tmp = tmp * 10;
            --i;
        }
    }
    else
    {
        i = (e10 = -e10);
        while (i > 10)
        {
            tmp = tmp * 1e-10;
            i -= 10;
        }
        while (i > 1)
        {
            tmp = tmp / 10;
            --i;
        }
    }
    while (d / tmp < 1)
    {
        --e10;
        tmp /= 10.0;
    }
    /* step 4: see if precision is sufficient to display all digits */
    if (e10 > prec)
    {
        /* use scientific notation */
        int len = libc_dtostr(d / tmp, buf, maxlen, prec);
        if (len == 0)
            return 0;
        maxlen -= len;
        buf += len;
        if (--maxlen >= 0)
        {
            *buf = 'e';
            ++buf;
        }
        for (len = 1000; len > 0; len /= 10)
        {
            if (e10 >= len || !initial)
            {
                if (--maxlen >= 0)
                {
                    *buf = (char)((e10 / len) + '0');
                    ++buf;
                }
                initial = 0;
                e10 = e10 % len;
            }
        }
        if (maxlen >= 0)
            return (int)(buf - oldbuf);
        return 0;
    }
    /* step 5: loop through the digits, inserting the decimal point when
     * appropriate */
    for (; prec > 0;)
    {
        double tmp2 = d / tmp;
        int c;
        d -= ((int)tmp2 * tmp);
        c = (int)tmp2;
        if ((!initial) || c)
        {
            if (--maxlen >= 0)
            {
                initial = 0;
                *buf = (char)(c + '0');
                ++buf;
            }
            else
                return 0;
            --prec;
        }
        if (tmp > 0.5 && tmp < 1.5)
        {
            tmp = 1e-1;
            initial = 0;
            if (--maxlen >= 0)
            {
                *buf = '.';
                ++buf;
            }
            else
                return 0;
        }
        else
            tmp /= 10.0;
    }
    *buf = 0;
    return (int)(buf - oldbuf);
}

#endif

int LIBC_CALL libc_dtostr(double d, char* buf, int maxlen, int prec)
{
    unsigned long long bits;
    unsigned long long exponent_bits;
    unsigned long long fraction_bits;
    const char* special = NULL;
    double value;
    char digits[768];
    char output[768];
    int decimal_exponent = 0;
    int negative = 0;
    int scientific;
    int leading_zeroes = 0;
    int requested_digits;
    int generated_digits;
    size_t output_length = 0;
    size_t content_start;
    size_t i;

    if (!buf || maxlen <= 0)
        return 0;
    buf[0] = 0;

    if (prec < 0)
        prec = 0;
    if (prec > 300)
        return 0;

    libc_memcpy(&bits, &d, sizeof(bits));
    exponent_bits = (bits >> 52) & 0x7ffULL;
    fraction_bits = bits & ((1ULL << 52) - 1);
    if (exponent_bits == 0x7ffULL)
    {
        if (fraction_bits != 0)
            special = "nan";
        else if ((bits >> 63) != 0)
            special = "-inf";
        else
            special = "inf";
    }
    else if (d == 0.0)
    {
        special = (bits >> 63) != 0 ? "-0" : "0";
    }

    if (special)
    {
        size_t special_length = libc_strlen(special);
        if (special_length >= (size_t)maxlen)
            return 0;
        libc_memcpy(buf, special, special_length + 1);
        return (int)special_length;
    }

    negative = (int)(bits >> 63);
    value = negative ? -d : d;

    /* Normalize into [1, 10) and track the decimal exponent. */
    while (value >= 10.0)
    {
        value /= 10.0;
        ++decimal_exponent;
    }
    while (value < 1.0)
    {
        value *= 10.0;
        --decimal_exponent;
    }

    scientific = decimal_exponent > prec;
    if (scientific)
    {
        requested_digits = 1 + prec;
    }
    else if (decimal_exponent >= 0)
    {
        requested_digits = decimal_exponent + 1 + prec;
    }
    else
    {
        leading_zeroes = -decimal_exponent - 1;
        requested_digits = prec - leading_zeroes;
        if (requested_digits < 0)
            requested_digits = 0;
    }

    generated_digits = requested_digits + 1;
    if (generated_digits >= (int)sizeof(digits))
        return 0;

    for (i = 0; i < (size_t)generated_digits; ++i)
    {
        int digit = (int)value;
        if (digit < 0 || digit > 9)
            return 0;
        digits[i] = (char)digit;
        value = (value - (double)digit) * 10.0;
    }

    if (negative)
        output[output_length++] = '-';
    content_start = output_length;

    if (scientific)
    {
        output[output_length++] = (char)('0' + digits[0]);
        if (prec > 0)
        {
            output[output_length++] = '.';
            for (i = 0; i < (size_t)prec; ++i)
                output[output_length++] = (char)('0' + digits[i + 1]);
        }
    }
    else if (decimal_exponent >= 0)
    {
        for (i = 0; i < (size_t)(decimal_exponent + 1); ++i)
            output[output_length++] = (char)('0' + digits[i]);
        if (prec > 0)
        {
            output[output_length++] = '.';
            for (i = 0; i < (size_t)prec; ++i)
                output[output_length++] =
                    (char)('0' + digits[(size_t)(decimal_exponent + 1) + i]);
        }
    }
    else
    {
        output[output_length++] = '0';
        if (prec > 0)
        {
            output[output_length++] = '.';
            for (i = 0; i < (size_t)leading_zeroes && i < (size_t)prec; ++i)
                output[output_length++] = '0';
            for (; i < (size_t)prec; ++i)
            {
                size_t digit_index = i - (size_t)leading_zeroes;
                output[output_length++] = digit_index < (size_t)requested_digits ?
                    (char)('0' + digits[digit_index]) : '0';
            }
        }
    }

    if (digits[requested_digits] >= 5)
    {
        size_t position = output_length;
        int carry = 1;
        while (position > content_start)
        {
            char ch;
            --position;
            ch = output[position];
            if (ch == '.')
                continue;
            if (ch < '9')
            {
                output[position] = (char)(ch + 1);
                carry = 0;
                break;
            }
            output[position] = '0';
        }
        if (carry)
        {
            for (position = output_length; position > content_start; --position)
                output[position] = output[position - 1];
            output[content_start] = '1';
            ++output_length;
            if (scientific)
            {
                for (position = content_start + 1;
                     position + 1 < output_length; ++position)
                    output[position] = output[position + 1];
                --output_length;
                ++decimal_exponent;
            }
        }
    }

    if (scientific)
    {
        unsigned int exponent_value;
        char exponent_digits[12];
        size_t exponent_length = 0;

        output[output_length++] = 'e';
        if (decimal_exponent < 0)
        {
            output[output_length++] = '-';
            exponent_value = (unsigned int)(-(decimal_exponent + 1)) + 1U;
        }
        else
        {
            output[output_length++] = '+';
            exponent_value = (unsigned int)decimal_exponent;
        }
        do
        {
            exponent_digits[exponent_length++] =
                (char)('0' + exponent_value % 10U);
            exponent_value /= 10U;
        } while (exponent_value != 0);
        while (exponent_length != 0)
            output[output_length++] = exponent_digits[--exponent_length];
    }

    if (output_length >= (size_t)maxlen)
        return 0;
    libc_memcpy(buf, output, output_length);
    buf[output_length] = 0;
    return (int)output_length;
}
#endif /* LIBC_NO_FLOATING_POINT */

static int libc_utf8_is_continuation(unsigned char value)
{
    return value >= 0x80U && value <= 0xBFU;
}

static int libc_utf8_decode(const unsigned char* input,
                            uint32_t* codepoint, size_t* consumed)
{
    unsigned char first = input[0];
    unsigned char second;
    unsigned char third;
    unsigned char fourth;

    if (first <= 0x7FU)
    {
        *codepoint = first;
        *consumed = 1;
        return 1;
    }

    if (first >= 0xC2U && first <= 0xDFU)
    {
        second = input[1];
        if (!libc_utf8_is_continuation(second))
            return 0;
        *codepoint = ((uint32_t)first & 0x1FU) << 6 |
                     ((uint32_t)second & 0x3FU);
        *consumed = 2;
        return 1;
    }

    if (first >= 0xE0U && first <= 0xEFU)
    {
        second = input[1];
        if (!libc_utf8_is_continuation(second) ||
            (first == 0xE0U && second < 0xA0U) ||
            (first == 0xEDU && second > 0x9FU))
            return 0;
        third = input[2];
        if (!libc_utf8_is_continuation(third))
            return 0;
        *codepoint = ((uint32_t)first & 0x0FU) << 12 |
                     ((uint32_t)second & 0x3FU) << 6 |
                     ((uint32_t)third & 0x3FU);
        *consumed = 3;
        return 1;
    }

    if (first >= 0xF0U && first <= 0xF4U)
    {
        second = input[1];
        if (!libc_utf8_is_continuation(second) ||
            (first == 0xF0U && second < 0x90U) ||
            (first == 0xF4U && second > 0x8FU))
            return 0;
        third = input[2];
        if (!libc_utf8_is_continuation(third))
            return 0;
        fourth = input[3];
        if (!libc_utf8_is_continuation(fourth))
            return 0;
        *codepoint = ((uint32_t)first & 0x07U) << 18 |
                     ((uint32_t)second & 0x3FU) << 12 |
                     ((uint32_t)third & 0x3FU) << 6 |
                     ((uint32_t)fourth & 0x3FU);
        *consumed = 4;
        return 1;
    }

    return 0;
}

static size_t libc_utf8_encode(uint32_t codepoint, unsigned char output[4])
{
    if (codepoint <= 0x7FU)
    {
        output[0] = (unsigned char)codepoint;
        return 1;
    }
    if (codepoint <= 0x7FFU)
    {
        output[0] = (unsigned char)(0xC0U | (codepoint >> 6));
        output[1] = (unsigned char)(0x80U | (codepoint & 0x3FU));
        return 2;
    }
    if (codepoint >= 0xD800U && codepoint <= 0xDFFFU)
        return 0;
    if (codepoint <= 0xFFFFU)
    {
        output[0] = (unsigned char)(0xE0U | (codepoint >> 12));
        output[1] = (unsigned char)(0x80U | ((codepoint >> 6) & 0x3FU));
        output[2] = (unsigned char)(0x80U | (codepoint & 0x3FU));
        return 3;
    }
    if (codepoint <= 0x10FFFFU)
    {
        output[0] = (unsigned char)(0xF0U | (codepoint >> 18));
        output[1] = (unsigned char)(0x80U | ((codepoint >> 12) & 0x3FU));
        output[2] = (unsigned char)(0x80U | ((codepoint >> 6) & 0x3FU));
        output[3] = (unsigned char)(0x80U | (codepoint & 0x3FU));
        return 4;
    }
    return 0;
}

static int libc_wchar_size_is(size_t expected)
{
    return sizeof(wchar_t) == expected;
}

static int libc_utf16_constraint(uint16_t* dest, size_t dest_size, int error)
{
    if (dest && dest_size != 0)
        dest[0] = 0;
    SET_ERRNO(error);
    return error;
}

int LIBC_CALL libc_utf8_to_utf16_s(uint16_t* dest, size_t dest_size,
                         const char* src, size_t* written)
{
    const unsigned char* input = (const unsigned char*)src;
    size_t output_length = 0;
    size_t source_length;

    if (written)
        *written = 0;
    if (!dest || dest_size == 0 || !src)
        return libc_utf16_constraint(dest, dest_size, EINVAL);
    source_length = libc_strlen(src);
    if (source_length == SIZE_MAX ||
        libc_ranges_overlap_units(dest, dest_size, sizeof(uint16_t),
                                  src, source_length + 1, sizeof(char)))
        return libc_utf16_constraint(dest, dest_size, EINVAL);

    while (*input != 0)
    {
        uint32_t codepoint;
        size_t consumed;
        size_t required = 1;

        if (!libc_utf8_decode(input, &codepoint, &consumed))
            return libc_utf16_constraint(dest, dest_size, EILSEQ);
        if (codepoint > 0xFFFFU)
            required = 2;

        if (output_length > dest_size - 1 ||
            required > dest_size - 1 - output_length)
            return libc_utf16_constraint(dest, dest_size, ERANGE);

        if (required == 1)
        {
            dest[output_length++] = (uint16_t)codepoint;
        }
        else
        {
            codepoint -= 0x10000U;
            dest[output_length++] = (uint16_t)(0xD800U + (codepoint >> 10));
            dest[output_length++] = (uint16_t)(0xDC00U + (codepoint & 0x3FFU));
        }
        input += consumed;
    }

    dest[output_length] = 0;
    if (written)
        *written = output_length;
    return 0;
}

int LIBC_CALL libc_utf16_to_utf8_s(char* dest, size_t dest_size,
                         const uint16_t* src, size_t* written)
{
    unsigned char* output = (unsigned char*)dest;
    size_t input_length = 0;
    size_t output_length = 0;
    size_t source_count;

    if (written)
        *written = 0;
    if (!dest || dest_size == 0 || !src)
        return libc_string_constraint(dest, dest_size, EINVAL);
    while (src[input_length] != 0)
        ++input_length;
    if (input_length == SIZE_MAX)
        return libc_string_constraint(dest, dest_size, EINVAL);
    source_count = input_length + 1;
    if (libc_ranges_overlap_units(dest, dest_size, sizeof(char),
                                  src, source_count, sizeof(uint16_t)))
        return libc_string_constraint(dest, dest_size, EINVAL);
    input_length = 0;

    while (src[input_length] != 0)
    {
        uint32_t codepoint = src[input_length++];
        unsigned char encoded[4];
        size_t required;

        if (codepoint >= 0xD800U && codepoint <= 0xDBFFU)
        {
            uint32_t low = src[input_length];
            if (low < 0xDC00U || low > 0xDFFFU)
                return libc_string_constraint(dest, dest_size, EILSEQ);
            codepoint = 0x10000U +
                        ((codepoint - 0xD800U) << 10) +
                        (low - 0xDC00U);
            ++input_length;
        }
        else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU)
        {
            return libc_string_constraint(dest, dest_size, EILSEQ);
        }

        required = libc_utf8_encode(codepoint, encoded);
        if (required == 0)
            return libc_string_constraint(dest, dest_size, EILSEQ);
        if (output_length > dest_size - 1 ||
            required > dest_size - 1 - output_length)
            return libc_string_constraint(dest, dest_size, ERANGE);

        libc_memcpy(output + output_length, encoded, required);
        output_length += required;
    }

    dest[output_length] = 0;
    if (written)
        *written = output_length;
    return 0;
}

int LIBC_CALL libc_utf8_to_wchar_s(wchar_t* dest, size_t dest_size,
                         const char* src, size_t* written)
{
    if (written)
        *written = 0;
    if (!dest || dest_size == 0 || !src)
        return libc_wstring_constraint(dest, dest_size, EINVAL);

#if defined(LIBC_WCHAR_IS_UTF16)
    if (!libc_wchar_size_is(sizeof(uint16_t)))
        return libc_wstring_constraint(dest, dest_size, EINVAL);
    return libc_utf8_to_utf16_s((uint16_t*)dest, dest_size, src, written);
#else
    if (!libc_wchar_size_is(sizeof(uint32_t)))
        return libc_wstring_constraint(dest, dest_size, EINVAL);

    {
        const unsigned char* input = (const unsigned char*)src;
        size_t output_length = 0;
        size_t source_length = libc_strlen(src);

        if (source_length == SIZE_MAX ||
            libc_ranges_overlap_units(dest, dest_size, sizeof(wchar_t),
                                      src, source_length + 1, sizeof(char)))
            return libc_wstring_constraint(dest, dest_size, EINVAL);

    while (*input != 0)
    {
        uint32_t codepoint;
        size_t consumed;

        if (!libc_utf8_decode(input, &codepoint, &consumed))
            return libc_wstring_constraint(dest, dest_size, EILSEQ);
        if (output_length >= dest_size - 1)
            return libc_wstring_constraint(dest, dest_size, ERANGE);
        dest[output_length++] = (wchar_t)codepoint;
        input += consumed;
    }

    dest[output_length] = L'\0';
    if (written)
        *written = output_length;
    return 0;
    }
#endif
}

int LIBC_CALL libc_wchar_to_utf8_s(char* dest, size_t dest_size,
                         const wchar_t* src, size_t* written)
{
    if (written)
        *written = 0;
    if (!dest || dest_size == 0 || !src)
        return libc_string_constraint(dest, dest_size, EINVAL);

#if defined(LIBC_WCHAR_IS_UTF16)
    if (!libc_wchar_size_is(sizeof(uint16_t)))
        return libc_string_constraint(dest, dest_size, EINVAL);
    return libc_utf16_to_utf8_s(dest, dest_size,
                                (const uint16_t*)src, written);
#else
    if (!libc_wchar_size_is(sizeof(uint32_t)))
        return libc_string_constraint(dest, dest_size, EINVAL);

    {
    unsigned char* output = (unsigned char*)dest;
    size_t input_length = 0;
    size_t output_length = 0;
    size_t source_length;

    source_length = libc_wcslen(src);
    if (source_length == SIZE_MAX ||
        libc_ranges_overlap_units(dest, dest_size, sizeof(char),
                                  src, source_length + 1, sizeof(wchar_t)))
        return libc_string_constraint(dest, dest_size, EINVAL);

    while (src[input_length] != L'\0')
    {
        uint32_t codepoint = (uint32_t)src[input_length++];
        unsigned char encoded[4];
        size_t required = libc_utf8_encode(codepoint, encoded);

        if (required == 0)
            return libc_string_constraint(dest, dest_size, EILSEQ);
        if (output_length > dest_size - 1 ||
            required > dest_size - 1 - output_length)
            return libc_string_constraint(dest, dest_size, ERANGE);
        libc_memcpy(output + output_length, encoded, required);
        output_length += required;
    }

    output[output_length] = 0;
    if (written)
        *written = output_length;
    return 0;
    }
#endif
}
