// Copyright (c) 2025-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <util/procstats.h>

#include <util/strencodings.h>

#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>

#if defined(__linux__)
#include <dirent.h>
#include <unistd.h>
#endif

size_t GetProcessMemoryUsed()
{
#if defined(__linux__)
    // /proc/self/statm field 2 is the resident set size in pages.
    FILE* f{std::fopen("/proc/self/statm", "r")};
    if (f == nullptr) return 0;
    char buf[256];
    const size_t n_read{std::fread(buf, 1, sizeof(buf) - 1, f)};
    std::fclose(f);
    // Both fields are plain decimal integers, so parse them by hand instead of
    // with a locale-dependent scanf conversion.
    const std::string_view text{buf, n_read};
    const size_t first_sep{text.find(' ')};
    if (first_sep == std::string_view::npos) return 0;
    const std::string_view rest{text.substr(first_sep + 1)};
    const auto resident{ToIntegral<unsigned long>(rest.substr(0, rest.find(' ')))};
    if (!resident) return 0;
    const long page_size{::sysconf(_SC_PAGESIZE)};
    if (page_size <= 0) return 0;
    return static_cast<size_t>(*resident) * static_cast<size_t>(page_size);
#else
    return 0;
#endif
}

size_t GetNumFDs()
{
#if defined(__linux__)
    DIR* dir{::opendir("/proc/self/fd")};
    if (dir == nullptr) return 0;
    size_t count{0};
    while (const dirent* entry{::readdir(dir)}) {
        // Skip "." and ".."; the directory handle itself is already open and
        // will show up as an entry, so the count is off by one either way.
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
        ++count;
    }
    ::closedir(dir);
    return count;
#else
    return 0;
#endif
}
