// Copyright (c) 2020-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_RPC_MINING_H
#define BITCOIN_RPC_MINING_H

#include <cstdint>

class CChain;

/** Default max iterations to try in RPC generatetodescriptor, generatetoaddress, and generateblock. */
inline constexpr uint64_t DEFAULT_MAX_TRIES{1'000'000};

/**
 * Estimate the network hashrate over the last 120 blocks, using the same
 * computation as the getnetworkhashps RPC (default arguments).
 *
 * Must be called with ::cs_main held.
 */
double GetNetworkHashPS(const CChain& active_chain);

#endif // BITCOIN_RPC_MINING_H
