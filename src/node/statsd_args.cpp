// Copyright (c) 2025-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/statsd_args.h>

#include <common/args.h>
#include <statsd_client.h>
#include <util/log.h>

#include <cstdint>
#include <optional>
#include <string>

std::optional<std::string> ConfigureStatsClient(statsd::StatsdClient& client, const ArgsManager& args)
{
    if (!args.GetBoolArg("-stats", statsd::DEFAULT_STATS)) {
        client.SetEnabled(false);
        LogInfo("Sending statsd metrics disabled (-nostats)");
        return std::nullopt;
    }

    const std::string host{args.GetArg("-statshost", statsd::DEFAULT_STATSD_HOST)};
    if (host.empty()) {
        return "Invalid -statshost: may not be empty";
    }

    const int64_t port_arg{args.GetIntArg("-statsport", statsd::DEFAULT_STATSD_PORT)};
    if (port_arg <= 0 || port_arg > 65535) {
        return strprintf("Invalid -statsport: %d is not a valid UDP port (1-65535)", static_cast<int>(port_arg));
    }

    const std::string ns{args.GetArg("-statsnamespace", "")};
    const std::string nodename{args.GetArg("-statshostname", "")};

    client.config(host, static_cast<int>(port_arg), ns);
    // The node name partitions every metric key, so it must be set after
    // config() (which resets everything else) and before the first emission.
    client.setNodeName(nodename);
    client.SetEnabled(true);

    LogInfo("Sending statsd metrics to %s:%d (namespace: '%s', node name: '%s')",
            host, static_cast<int>(port_arg), ns, nodename);
    return std::nullopt;
}
