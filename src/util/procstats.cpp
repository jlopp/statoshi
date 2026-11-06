// Copyright (c) 2025-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <util/procstats.h>

#include <cstdio>
#include <cstring>

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
    unsigned long total{0};
    unsigned long resident{0};
    const int matched{std::fscanf(f, "%lu %lu", &total, &resident)};
    std::fclose(f);
    if (matched != 2) return 0;
    const long page_size{::sysconf(_SC_PAGESIZE)};
    if (page_size <= 0) return 0;
    return static_cast<size_t>(resident) * static_cast<size_t>(page_size);
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
