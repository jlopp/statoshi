// Copyright (c) 2025-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_NODE_STATSD_ARGS_H
#define BITCOIN_NODE_STATSD_ARGS_H

#include <optional>
#include <string>

class ArgsManager;
namespace statsd {
class StatsdClient;
}

/**
 * Apply the -stats, -statshost, -statsport, -statsnamespace and -statshostname
 * options to @p client. Call once during startup, before any metrics are
 * emitted, and call Start() on the client afterwards.
 *
 * This lives outside statsd_client.cpp so that library can be linked into
 * libbitcoinkernel, which must not depend on the args parsing machinery.
 *
 * @return std::nullopt on success, otherwise a description of the invalid
 *         option value.
 */
std::optional<std::string> ConfigureStatsClient(statsd::StatsdClient& client, const ArgsManager& args);

#endif // BITCOIN_NODE_STATSD_ARGS_H
