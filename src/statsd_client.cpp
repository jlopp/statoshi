/**
Copyright (c) 2014, Rex
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of the {organization} nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**/

#include <math.h>
#include <netdb.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <netinet/in.h>

#include <common/args.h>
#include <logging.h>
#include <util/thread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdint>
#include <fcntl.h>
#include <mutex>
#include <thread>
#include <utility>

#include "statsd_client.h"

using namespace std;
using namespace std::chrono_literals;

namespace statsd {

//! How often the emitter reports its own counters as metrics.
static constexpr auto SELF_REPORT_INTERVAL{10s};

std::string SanitizeKeyPart(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        const bool safe{(c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_'};
        out.push_back(safe ? c : '_');
    }
    return out;
}

inline bool fequal(float a, float b)
{
    const float epsilon = 0.0001;
    return ( fabs(a - b) < epsilon );
}

inline bool should_send(float sample_rate)
{
    // The overwhelmingly common case: no sampling, decided without touching an
    // RNG at all.
    if ( fequal(sample_rate, 1.0) )
    {
        return true;
    }

    // random() serialises on a global lock, which costs ~9x more once several
    // threads sample concurrently. A per-thread xorshift keeps sampling local
    // to the emitting thread. Sampling decisions do not need a global stream.
    thread_local uint64_t state{[] {
        // splitmix64 of a per-thread address and a clock read, so two threads
        // started in the same tick do not get identical streams.
        uint64_t x{static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&state)) ^
                   static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }()};

    // xorshift64*
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    const uint64_t r{state * 0x2545f4914f6cdd1dULL};

    // Map to [0, 1) and compare, so behaviour matches the documented meaning of
    // a sample rate: the fraction of events that survive.
    const double p{static_cast<double>(r >> 11) * (1.0 / 9007199254740992.0)};
    return sample_rate > p;
}

/**
 * Emitter state.
 *
 * Producers (every thread in the node) only ever append text to @p queue under
 * @p mutex; they never touch the socket. A single flusher thread owns the
 * socket, splits the queued text into MTU-safe datagrams and transmits them.
 *
 * This split matters because the syscall dominates the cost of a metric: a
 * sendto() is roughly 4us here against ~90ns for formatting the line. Doing
 * one sendto per metric therefore costs far more than the metrics themselves,
 * and it happens while the caller may hold cs_main, m_nodes_mutex or
 * cs_vRecv -- so a slow backend becomes consensus-path latency.
 */
struct _StatsdClientData {
    //! Socket used by the flusher. -1 until the first successful init.
    int sock{-1};
    //! Cached destination; the socket is connect()ed, so send() is enough.
    struct sockaddr_in server{};
    bool connected{false};
    //! When host resolution last failed, so a bad host is not re-resolved on
    //! every flush.
    std::chrono::steady_clock::time_point last_init_failure{};

    //! Written once by config() during startup, then read by every producer.
    string ns;
    string host;
    string nodename;
    int port{DEFAULT_STATSD_PORT};
    bool enabled{DEFAULT_STATS};

    char errmsg[1024]{};

    mutable std::mutex mutex;
    //! Metric text waiting to be transmitted (producers append here).
    string queue;
    //! Buffer handed to the flusher; swapped with queue so the producers are
    //! only blocked for a pointer swap, not for the duration of the send.
    string out;
    std::condition_variable cv;
    bool running{false};
    //! Set once the queue is large enough that waiting for the tick is wasteful.
    bool threshold_reached{false};
    std::thread flusher;

    std::atomic<uint64_t> bytes_queued{0};
    std::atomic<uint64_t> bytes_dropped{0};
    std::atomic<uint64_t> datagrams_sent{0};
    std::atomic<uint64_t> send_failures{0};
    mutable std::atomic<size_t> queue_bytes{0};

    //! Rate limiting for failure logging, so a dead backend cannot flood the log.
    std::chrono::steady_clock::time_point last_failure_log{};
    uint64_t consecutive_failures{0};
};

StatsdClient::StatsdClient(const string& host, int port, const string& ns)
{
    d = new _StatsdClientData;
    config(host, port, ns);
    srandom(time(NULL));
}

StatsdClient::~StatsdClient()
{
    Stop();
    // Stop() flushes through the socket but deliberately leaves it open, since
    // it is also called on restartable paths. Close it here.
    if (d->sock >= 0) {
        close(d->sock);
        d->sock = -1;
    }
    delete d;
    d = nullptr;
}

void StatsdClient::config(const string& host, int port, const string& ns)
{
    // Must be called before Start(); producers read these without a lock, which
    // is safe because configuration happens once during startup.
    d->ns = ns;
    d->host = host;
    d->port = port;
    if (d->sock >= 0) {
        close(d->sock);
        d->sock = -1;
        d->connected = false;
    }
}

void StatsdClient::setNodeName(const string& name)
{
    d->nodename = name;
}

void StatsdClient::SetEnabled(bool enabled)
{
    d->enabled = enabled;
    if (!enabled) {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->queue.clear();
        d->queue_bytes = 0;
    }
}

void StatsdClient::Start()
{
    std::lock_guard<std::mutex> lock(d->mutex);
    if (d->running) return;
    // Nothing to flush when emission is off, so do not spend a thread on it.
    if (!d->enabled) return;
    d->running = true;
    d->flusher = std::thread(util::TraceThread, "statsd", [this] { flushLoop(); });
}

void StatsdClient::Stop()
{
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        if (!d->running) {
            // Either never started, or already stopped. Still drop anything
            // queued so a later restart does not emit stale samples.
            d->queue.clear();
            d->queue_bytes = 0;
            return;
        }
        d->running = false;
    }
    d->cv.notify_all();
    if (d->flusher.joinable()) d->flusher.join();

    // Final drain: emit whatever was produced between the last tick and now.
    string payload;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        payload.swap(d->queue);
        d->queue_bytes = 0;
    }
    if (d->sock >= 0 && !payload.empty()) transmit(payload);
}

ClientStats StatsdClient::GetStats() const
{
    ClientStats stats;
    stats.bytes_queued = d->bytes_queued.load(std::memory_order_relaxed);
    stats.bytes_dropped = d->bytes_dropped.load(std::memory_order_relaxed);
    stats.datagrams_sent = d->datagrams_sent.load(std::memory_order_relaxed);
    stats.send_failures = d->send_failures.load(std::memory_order_relaxed);
    stats.queue_bytes = d->queue_bytes.load(std::memory_order_relaxed);
    return stats;
}

int StatsdClient::init()
{
    if (d->sock >= 0) return 0;

    // A failing host must not cost a DNS lookup on every flush.
    const auto now{std::chrono::steady_clock::now()};
    if (d->last_init_failure.time_since_epoch().count() != 0 &&
        now - d->last_init_failure < INIT_RETRY_INTERVAL) {
        return -2;
    }

    d->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if ( d->sock == -1 ) {
        snprintf(d->errmsg, sizeof(d->errmsg), "could not create socket, err=%s", strerror(errno));
        d->last_init_failure = now;
        LogWarning("statsd: could not create socket: %s", strerror(errno));
        return -1;
    }

    // A large send buffer lets a burst between flushes be absorbed by the
    // kernel instead of being dropped in userspace. Both ends of a loopback
    // transfer are cheap to enlarge; the request is best-effort.
    int sndbuf{SNDBUF_BYTES};
    if (setsockopt(d->sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) == -1) {
        LogDebug(BCLog::NET, "statsd: could not raise SO_SNDBUF to %d: %s", SNDBUF_BYTES, strerror(errno));
    }

    // Non-blocking, so an overflowing socket buffer turns into a dropped sample
    // instead of a stalled flusher.
#ifndef WIN32
    const int flags{fcntl(d->sock, F_GETFL, 0)};
    if (flags != -1) {
        if (fcntl(d->sock, F_SETFL, flags | O_NONBLOCK) == -1) {
            LogDebug(BCLog::NET, "statsd: could not set O_NONBLOCK: %s", strerror(errno));
        }
    }
#endif

    memset(&d->server, 0, sizeof(d->server));
    d->server.sin_family = AF_INET;
    d->server.sin_port = htons(d->port);

    int ret = inet_aton(d->host.c_str(), &d->server.sin_addr);
    if ( ret == 0 )
    {
        // host must be a domain, get it from internet
        struct addrinfo hints, *result = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;

        ret = getaddrinfo(d->host.c_str(), NULL, &hints, &result);
        if ( ret ) {
            snprintf(d->errmsg, sizeof(d->errmsg),
                    "getaddrinfo fail, error=%d, msg=%s", ret, gai_strerror(ret) );
            close(d->sock);
            d->sock = -1;
            d->last_init_failure = now;
            return -2;
        }
        struct sockaddr_in* host_addr = (struct sockaddr_in*)result->ai_addr;
        memcpy(&d->server.sin_addr, &host_addr->sin_addr, sizeof(struct in_addr));
        freeaddrinfo(result);
    }

    // Connecting a UDP socket caches the route and, more importantly, makes the
    // kernel surface ICMP errors: without it a dead backend looks like a
    // perfectly healthy sendto().
    if (connect(d->sock, reinterpret_cast<struct sockaddr*>(&d->server), sizeof(d->server)) == -1) {
        snprintf(d->errmsg, sizeof(d->errmsg),
                "connect fail, host=%s:%d, err=%s", d->host.c_str(), d->port, strerror(errno));
        close(d->sock);
        d->sock = -1;
        d->last_init_failure = now;
        return -3;
    }
    d->connected = true;

    LogDebug(BCLog::NET, "statsd: connected to %s:%d", d->host.c_str(), d->port);
    return 0;
}

int StatsdClient::enqueue(std::string_view payload)
{
    if (!d->enabled || payload.empty()) return 0;

    const size_t len{payload.size()};
    bool wake{false};
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        if (d->queue.size() + len > MAX_QUEUED_BYTES) {
            // Drop rather than block: a stalled producer inside cs_vRecv or
            // cs_main is far worse than a lost sample. The dropped byte count
            // is reported so the gap is visible.
            d->bytes_dropped.fetch_add(len, std::memory_order_relaxed);
            return -1;
        }
        const bool was_below{d->queue.size() < BATCH_FLUSH_BYTES};
        d->queue.append(payload.data(), len);
        d->bytes_queued.fetch_add(len, std::memory_order_relaxed);
        d->queue_bytes.store(d->queue.size(), std::memory_order_relaxed);
        if (was_below && d->queue.size() >= BATCH_FLUSH_BYTES) {
            d->threshold_reached = true;
            wake = true;
        }
    }
    if (wake) d->cv.notify_one();
    return 0;
}

int StatsdClient::sendDatagram(const char* data, size_t len)
{
    if (d->sock < 0) {
        if (init() != 0) return -1;
    }

    while (true) {
        const ssize_t ret{::send(d->sock, data, len, 0)};
        if (ret == static_cast<ssize_t>(len)) {
            d->datagrams_sent.fetch_add(1, std::memory_order_relaxed);
            if (d->consecutive_failures > 0) {
                LogInfo("statsd: transmission to %s:%d recovered after %" PRIu64 " failures",
                        d->host, d->port, d->consecutive_failures);
                d->consecutive_failures = 0;
            }
            return 0;
        }
        if (ret < 0 && errno == EINTR) continue;

        recordFailure(errno);
        // A socket that is gone (e.g. the interface disappeared) must be
        // recreated on the next flush rather than reused forever.
        if (errno == EBADF || errno == ENOTSOCK || errno == ECONNREFUSED || errno == ENETUNREACH) {
            if (d->sock >= 0) {
                close(d->sock);
                d->sock = -1;
                d->connected = false;
                d->last_init_failure = std::chrono::steady_clock::now();
            }
        }
        return -1;
    }
}

void StatsdClient::recordFailure(int error)
{
    d->send_failures.fetch_add(1, std::memory_order_relaxed);
    ++d->consecutive_failures;
    snprintf(d->errmsg, sizeof(d->errmsg),
            "sendto server fail, host=%s:%d, err=%s", d->host.c_str(), d->port, strerror(error));

    const auto now{std::chrono::steady_clock::now()};
    if (d->last_failure_log.time_since_epoch().count() == 0 ||
        now - d->last_failure_log >= FAILURE_LOG_INTERVAL) {
        d->last_failure_log = now;
        LogWarning("statsd: could not transmit to %s:%d (%" PRIu64 " consecutive failures): %s",
                   d->host, d->port, d->consecutive_failures, strerror(error));
    }
}

void StatsdClient::transmit(std::string_view payload)
{
    size_t pos{0};
    while (pos < payload.size()) {
        size_t end{std::min(payload.size(), pos + BATCH_FLUSH_BYTES)};
        if (end < payload.size()) {
            // Cut on a line boundary so no metric is ever split across packets.
            const size_t nl{payload.rfind('\n', end)};
            if (nl != std::string_view::npos && nl >= pos) {
                end = nl + 1;
            }
        }
        sendDatagram(payload.data() + pos, end - pos);
        pos = end;
    }
}

void StatsdClient::flushLoop()
{
    uint64_t reported_queued{0};
    uint64_t reported_sent{0};
    uint64_t reported_failed{0};
    auto last_report{std::chrono::steady_clock::now()};

    while (true) {
        string payload;
        {
            std::unique_lock<std::mutex> lock(d->mutex);
            if (!d->running) break;
            d->cv.wait_for(lock, FLUSH_INTERVAL, [this] {
                return !d->running || d->threshold_reached;
            });
            if (!d->running) break;
            d->threshold_reached = false;
            // Swap so producers are blocked only for the pointer exchange,
            // never for the duration of the send itself.
            payload.swap(d->queue);
            d->queue_bytes.store(d->queue.size(), std::memory_order_relaxed);
        }

        if (!payload.empty()) {
            transmit(payload);
            payload.clear();
        } else if (d->sock < 0) {
            // Nothing to send, but still let init() make progress so a boot-time
            // DNS failure is retried.
            init();
        }

        // Report the emitter's own health. Without this a pipeline that is
        // dropping or failing looks exactly like an idle one.
        const auto now{std::chrono::steady_clock::now()};
        if (now - last_report >= SELF_REPORT_INTERVAL) {
            last_report = now;
            const uint64_t queued{d->bytes_queued.load(std::memory_order_relaxed)};
            const uint64_t sent{d->datagrams_sent.load(std::memory_order_relaxed)};
            const uint64_t failed{d->send_failures.load(std::memory_order_relaxed)};
            const uint64_t dropped{d->bytes_dropped.load(std::memory_order_relaxed)};

            string self;
            self += d->ns + "self.bytesQueued:" + std::to_string(queued - reported_queued) + "|c\n";
            self += d->ns + "self.datagramsSent:" + std::to_string(sent - reported_sent) + "|c\n";
            self += d->ns + "self.sendFailures:" + std::to_string(failed - reported_failed) + "|c\n";
            self += d->ns + "self.bytesDropped:" + std::to_string(dropped) + "|g\n";
            self += d->ns + "self.queueBytes:" + std::to_string(d->queue_bytes.load(std::memory_order_relaxed)) + "|g\n";
            reported_queued = queued;
            reported_sent = sent;
            reported_failed = failed;

            if (d->sock >= 0) transmit(self);
        }
    }

    // Drain anything left by producers that ran between the loop exit and here.
    string trailing;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        trailing.swap(d->queue);
        d->queue_bytes = 0;
    }
    if (!trailing.empty()) transmit(trailing);
}

int StatsdClient::dec(const string& key, float sample_rate)
{
    return countSigned(key, -1, sample_rate);
}

int StatsdClient::inc(const string& key, float sample_rate)
{
    return count(key, 1, sample_rate);
}

int StatsdClient::count(const string& key, size_t value, float sample_rate)
{
    return send(key, value, "c", sample_rate);
}

int StatsdClient::countSigned(const string& key, int64_t value, float sample_rate)
{
    if (!d->enabled) return 0;
    // Reused across calls on this thread: the formatted line is longer than the
    // small-string buffer, so a fresh std::string would allocate per metric.
    thread_local string message;
    message.clear();
    if (!formatSigned(message, key, value, "c", sample_rate)) {
        return 0;
    }
    return enqueue(message);
}

int StatsdClient::gauge(const string& key, size_t value, float sample_rate)
{
    return send(key, value, "g", sample_rate);
}

int StatsdClient::gaugeSigned(const string& key, int64_t value, float sample_rate)
{
    if (!d->enabled) return 0;
    thread_local string message;
    message.clear();
    if (!formatSigned(message, key, value, "g", sample_rate)) {
        return 0;
    }
    return enqueue(message);
}

int StatsdClient::gaugeDouble(const string& key, double value, float sample_rate)
{
    return sendDouble(key, value, "g", sample_rate);
}

int StatsdClient::timing(const string& key, size_t ms, float sample_rate)
{
    return send(key, ms, "ms", sample_rate);
}

//! Apply the -statshostname suffix and escape the StatsD delimiters.
//!
//! Both are no-ops in the common case (no node name, literals at the call site
//! with no ':' '|' '@'), so the copy is skipped and the caller's string is
//! returned unchanged. @p storage backs the result only when a copy is needed;
//! it is thread-local because formatting happens on the emitting thread.
static const std::string& PrepareKey(const std::string& nodename, const std::string& key_in, std::string& storage)
{
    const bool needs_suffix{!nodename.empty()};
    const bool needs_escape{key_in.find_first_of(":|@") != std::string::npos};
    if (!needs_suffix && !needs_escape) return key_in;

    if (needs_suffix) {
        storage.assign(key_in);
        storage += '.';
        storage += nodename;
    } else {
        storage.assign(key_in);
    }
    for (char& c : storage) {
        if (c == ':' || c == '|' || c == '@') c = '_';
    }
    return storage;
}

bool StatsdClient::formatSigned(string& out, const string& key_in, int64_t value, const string &type, float sample_rate)
{
    // Gauges are absolute values, not events: sampling them would make the
    // backend drop updates and leave the gauge stale. Always send them.
    if (type == "g") {
        sample_rate = 1.0;
    }

    if (!should_send(sample_rate)) {
        return false;
    }

    thread_local string storage;
    const string& key{PrepareKey(d->nodename, key_in, storage)};

    char buf[256];
    if ( fequal( sample_rate, 1.0 ) )
    {
        snprintf(buf, sizeof(buf), "%s%s:%" PRId64 "|%s",
                d->ns.c_str(), key.c_str(), value, type.c_str());
    }
    else
    {
        snprintf(buf, sizeof(buf), "%s%s:%" PRId64 "|%s|@%.2f",
                d->ns.c_str(), key.c_str(), value, type.c_str(), sample_rate);
    }

    out += buf;
    out += '\n';
    return true;
}

bool StatsdClient::format(string& out, const string& key_in, size_t value, const string &type, float sample_rate)
{
    // Gauges are absolute values, not events: sampling them would make the
    // backend drop updates and leave the gauge stale. Always send them.
    if (type == "g") {
        sample_rate = 1.0;
    }

    if (!should_send(sample_rate)) {
        return false;
    }

    thread_local string storage;
    const string& key{PrepareKey(d->nodename, key_in, storage)};

    char buf[256];
    if ( fequal( sample_rate, 1.0 ) )
    {
        snprintf(buf, sizeof(buf), "%s%s:%zd|%s",
                d->ns.c_str(), key.c_str(), value, type.c_str());
    }
    else
    {
        snprintf(buf, sizeof(buf), "%s%s:%zd|%s|@%.2f",
                d->ns.c_str(), key.c_str(), value, type.c_str(), sample_rate);
    }

    out += buf;
    out += '\n';
    return true;
}

bool StatsdClient::formatDouble(string& out, const string& key_in, double value, const string &type, float sample_rate)
{
    // Gauges are absolute values, not events: sampling them would make the
    // backend drop updates and leave the gauge stale. Always send them.
    if (type == "g") {
        sample_rate = 1.0;
    }

    if (!should_send(sample_rate)) {
        return false;
    }

    thread_local string storage;
    const string& key{PrepareKey(d->nodename, key_in, storage)};

    char buf[320];
    // Use %.17g so small values (e.g. regtest difficulty ~4.7e-10) are not
    // rounded down to zero, while integers stay free of trailing decimals.
    if ( fequal( sample_rate, 1.0 ) )
    {
        snprintf(buf, sizeof(buf), "%s%s:%.17g|%s",
                d->ns.c_str(), key.c_str(), value, type.c_str());
    }
    else
    {
        snprintf(buf, sizeof(buf), "%s%s:%.17g|%s|@%.2f",
                d->ns.c_str(), key.c_str(), value, type.c_str(), sample_rate);
    }

    out += buf;
    out += '\n';
    return true;
}

int StatsdClient::send(const string& key, size_t value, const string &type, float sample_rate)
{
    if (!d->enabled) return 0;
    // Reused across calls on this thread: the formatted line is longer than the
    // small-string buffer, so a fresh std::string would allocate per metric.
    thread_local string message;
    message.clear();
    if (!format(message, key, value, type, sample_rate)) {
        return 0;
    }
    return enqueue(message);
}

int StatsdClient::sendDouble(const string& key, double value, const string &type, float sample_rate)
{
    if (!d->enabled) return 0;
    thread_local string message;
    message.clear();
    if (!formatDouble(message, key, value, type, sample_rate)) {
        return 0;
    }
    return enqueue(message);
}

int StatsdClient::send(const string &message)
{
    return enqueue(message);
}

const char* StatsdClient::errmsg()
{
    return d->errmsg;
}

} // namespace statsd

statsd::StatsdClient statsClient{statsd::DEFAULT_STATSD_HOST, statsd::DEFAULT_STATSD_PORT, ""};

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
