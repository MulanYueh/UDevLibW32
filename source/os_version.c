#include "os_version.h"

NTOS_VERSION OSVersion_ParseOsInfo2Version(OSVERSIONINFOEXW NtosInfo)
{
    NTOS_VERSION WinVer = WINDOWS_NONE;

    if (NtosInfo.dwMajorVersion == 5 && NtosInfo.dwMinorVersion == 1 && NtosInfo.dwBuildNumber == 2600)
    {
        WinVer = WINDOWS_XP_SP3;
        return WinVer;
    }

    if (NtosInfo.dwMajorVersion == 5 && NtosInfo.dwMinorVersion == 2 && NtosInfo.dwBuildNumber == 3790)
    {
        WinVer = WINDOWS_2K3_SP2;
        return WinVer;
    }

    if (NtosInfo.dwMajorVersion == 6 && NtosInfo.dwMinorVersion == 0)
    {
        switch (NtosInfo.dwBuildNumber)
        {
        case 6000:
            WinVer = WINDOWS_VISTA_2008_RTM;
            break;
        case 6001:
            WinVer = WINDOWS_VISTA_2008_SP1;
            break;
        case 6002:
            WinVer = WINDOWS_VISTA_2008_SP2;
            break;
        default:
            break;
        }
        return WinVer;
    }

    if (NtosInfo.dwMajorVersion == 6 && NtosInfo.dwMinorVersion == 1)
    {
        switch (NtosInfo.dwBuildNumber)
        {
        case 7600:
            WinVer = WINDOWS_7_2008R2_RTM;
            break;
        case 7601:
            WinVer = WINDOWS_7_2008R2_SP1;
            break;
        default:
            break;
        }
        return WinVer;
    }

    if (NtosInfo.dwMajorVersion == 6 && NtosInfo.dwMinorVersion == 2 && NtosInfo.dwBuildNumber == 9200)
    {
        WinVer = WINDOWS_8_2012_RTM;
        return WinVer;
    }

    if (NtosInfo.dwMajorVersion == 6 && NtosInfo.dwMinorVersion == 3 && NtosInfo.dwBuildNumber == 9600)
    {
        switch (NtosInfo.wServicePackMajor)
        {
        case 0:
            WinVer = WINDOWS_81_2012R2;
            break;
        case 1:
            WinVer = WINDOWS_81_2012R2_SP1;
            break;
        default:
            break;
        }
        return WinVer;
    }

    if (NtosInfo.dwMajorVersion == 10 && NtosInfo.dwMinorVersion == 0)
    {
        switch (NtosInfo.dwBuildNumber)
        {
        case 10240:
            WinVer = WINDOWS_10_TH1;
            break;
        case 10586:
            WinVer = WINDOWS_10_TH2;
            break;
        case 14393:
            WinVer = WINDOWS_10_RS1;
            break;
        case 15063:
            WinVer = WINDOWS_10_RS2;
            break;
        case 16299:
            WinVer = WINDOWS_10_RS3;
            break;
        case 17134:
            WinVer = WINDOWS_10_RS4;
            break;
        case 17763:
            WinVer = WINDOWS_10_RS5;
            break;
        case 18362:
            WinVer = WINDOWS_10_19H1;
            break;
        case 18363:
            WinVer = WINDOWS_10_19H2;
            break;
        case 19041:
            WinVer = WINDOWS_10_20H1;
            break;
        case 19042:
            WinVer = WINDOWS_10_20H2;
            break;
        case 19043:
            WinVer = WINDOWS_10_21H1;
            break;
        case 19044:
            WinVer = WINDOWS_10_21H2;
            break;
        case 19045:
            WinVer = WINDOWS_10_22H2;
            break;
        case 22000:
            WinVer = WINDOWS_11_21H2;
            break;
        case 22621:
            WinVer = WINDOWS_11_22H2;
            break;
        case 22631:
            WinVer = WINDOWS_11_23H2;
            break;
        case 26100:
            WinVer = WINDOWS_11_24H2;
            break;
        case 26200:
            WinVer = WINDOWS_11_25H2;
            break;
        default:
            break;
        }
    }

    return WinVer;
}
