// Copyright (c) 2025-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_UTIL_PROCSTATS_H
#define BITCOIN_UTIL_PROCSTATS_H

#include <cstddef>

/**
 * Lightweight process introspection used for periodically emitted metrics.
 *
 * These read kernel-provided files on Linux and fall back to 0 on platforms
 * without an equivalent, so callers never need their own #ifdef.
 */

/** Resident set size of this process in bytes, or 0 if unavailable. */
size_t GetProcessMemoryUsed();

/** Number of open file descriptors, or 0 if unavailable. */
size_t GetNumFDs();

#endif // BITCOIN_UTIL_PROCSTATS_H
