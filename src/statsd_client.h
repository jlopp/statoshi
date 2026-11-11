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

#ifndef BITCOIN_STATSD_CLIENT_H
#define BITCOIN_STATSD_CLIENT_H

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace statsd {

struct _StatsdClientData;

/**
 * Append @p value to @p out as a decimal integer.
 *
 * std::to_string follows the C locale's LC_NUMERIC, so under a process locale
 * that enables digit grouping it can emit thousands separators, which makes the
 * value unparseable by statsd. std::to_chars is locale-independent by
 * definition, and this lives in the header because gaugeBatch() needs it.
 */
inline void AppendIndex(std::string& out, size_t value)
{
    char buf[32];
    const auto res{std::to_chars(buf, buf + sizeof(buf), value)};
    out.append(buf, static_cast<size_t>(res.ptr - buf));
}

//! Default host metrics are sent to when -statshost is not set.
inline constexpr const char* DEFAULT_STATSD_HOST{"127.0.0.1"};
//! Default UDP port metrics are sent to when -statsport is not set.
inline constexpr int DEFAULT_STATSD_PORT{8125};
//! Whether metrics are emitted unless -nostats is given.
inline constexpr bool DEFAULT_STATS{true};
//! Payload size at which a metric batch is flushed, so that one datagram stays
//! within a typical 1500-byte MTU instead of being IP-fragmented.
inline constexpr size_t BATCH_FLUSH_BYTES{1400};
//! How often buffered metric text is handed to the kernel. Independent of the
//! statsd aggregation interval downstream, which is measured in seconds.
inline constexpr std::chrono::milliseconds FLUSH_INTERVAL{100};
//! Upper bound on buffered metric text. Producers drop (and count) once this is
//! reached, so an unhealthy emitter can never grow without bound.
inline constexpr size_t MAX_QUEUED_BYTES{4 * 1024 * 1024};
//! Requested socket send buffer, so a burst between flushes is absorbed by the
//! kernel instead of being dropped in userspace.
inline constexpr int SNDBUF_BYTES{4 * 1024 * 1024};
//! How long to wait before retrying a failed host resolution, so that a bad
//! -statshost does not cost a DNS lookup per flush.
inline constexpr std::chrono::seconds INIT_RETRY_INTERVAL{60};
//! Minimum spacing between failure log lines; a dead backend must not flood the log.
inline constexpr std::chrono::seconds FAILURE_LOG_INTERVAL{60};

/**
 * Health of the emitter itself.
 *
 * Emitting these as metrics is the only way to notice a pipeline that is
 * silently dropping or failing to transmit: without them the graphs keep
 * rendering, just flat.
 */
struct ClientStats {
    //! Metric bytes accepted into the queue since startup.
    uint64_t bytes_queued{0};
    //! Metric bytes rejected because the queue was full.
    uint64_t bytes_dropped{0};
    //! UDP datagrams handed to the kernel successfully.
    uint64_t datagrams_sent{0};
    //! Datagrams the kernel refused (buffer full, network unreachable, ...).
    uint64_t send_failures{0};
    //! Metric bytes currently buffered, not yet transmitted.
    size_t queue_bytes{0};
};

/**
 * Make @p in safe to use as one component of a metric key.
 *
 * Metric keys are embedded in a newline-delimited datagram, so a value
 * containing a newline, space or statisticsd delimiter would corrupt the
 * frame or split one metric into two. Only [A-Za-z0-9_] are preserved; every
 * other character becomes '_'. Use this for any key component that is not a
 * compile-time constant (reject reasons, RPC methods, database names).
 */
std::string SanitizeKeyPart(const std::string& in);

class StatsdClient {
    public:
        StatsdClient(const std::string& host=DEFAULT_STATSD_HOST, int port=DEFAULT_STATSD_PORT, const std::string& ns = "");
        ~StatsdClient();

    public:
        // you can config at anytime; client will use new address (useful for Singleton)
        void config(const std::string& host, int port, const std::string& ns = "");
        //! Append @p name to every metric key, so that several nodes can report
        //! into a single backend without their metrics colliding.
        void setNodeName(const std::string& name);
        //! Turn emission off entirely (-nostats). Queued data is discarded.
        void SetEnabled(bool enabled);
        //! Whether emission is currently on. Callers that must do real work to
        //! compute a metric (e.g. the fee estimator's per-target loop) should
        //! check this first, so no statistics cost is paid when turned off.
        bool IsEnabled() const;
        const char* errmsg();

        /**
         * Start the background thread that turns buffered metric text into
         * datagrams. Until this is called, metrics accumulate in a bounded
         * queue and nothing is transmitted. Idempotent.
         */
        void Start();
        /**
         * Flush the remaining backlog and join the flusher. Idempotent, and
         * safe to call on paths where Start() never ran.
         */
        void Stop();
        //! Snapshot of the emitter's own counters.
        ClientStats GetStats() const;

    public:
        int inc(const std::string& key, float sample_rate = 1.0);
        int dec(const std::string& key, float sample_rate = 1.0);
        int count(const std::string& key, size_t value, float sample_rate = 1.0);
        //! Signed counters, for values that can legitimately be negative
        //! (e.g. a fee delta from prioritiseTransaction).
        int countSigned(const std::string& key, int64_t value, float sample_rate = 1.0);
        //! Gauges are absolute values, so they are never sampled. The
        //! sample_rate argument is accepted for source compatibility only.
        int gauge(const std::string& key, size_t value, float sample_rate = 1.0);
        //! Signed gauge, for values that can legitimately be negative.
        int gaugeSigned(const std::string& key, int64_t value, float sample_rate = 1.0);
        //! Gauges are absolute values, so they are never sampled. The
        //! sample_rate argument is accepted for source compatibility only.
        int gaugeDouble(const std::string& key, double value, float sample_rate = 1.0);
        int timing(const std::string& key, size_t ms, float sample_rate = 1.0);

    public:
        /**
         * Emit many gauges in a single datagram.
         *
         * StatsD allows one UDP payload to carry several newline-separated
         * metric lines. When a caller has a batch of related values (e.g. a
         * whole table of fee estimates), this avoids building the key prefix
         * repeatedly and keeps related samples together.
         *
         * @param prefix prepended to the numeric suffix of each key
         * @param values one gauge per element; element i is emitted as
         *               @p prefix + the decimal index (first_index + i)
         */
        template <typename Container>
        int gaugeBatch(const std::string& prefix, const Container& values, size_t first_index = 0)
        {
            std::string payload;
            std::string line;
            std::string key;
            size_t index{first_index};
            int ret{0};
            for (const auto& value : values) {
                line.clear();
                key = prefix;
                AppendIndex(key, index);
                if constexpr (std::is_floating_point_v<typename Container::value_type>) {
                    formatDouble(line, key, value, "g", 1.0);
                } else {
                    format(line, key, value, "g", 1.0);
                }
                // Measure the line *before* appending. Checking size() >= limit
                // after the append (as this used to) lets a long line push the
                // payload past the MTU, so one batch relies on IP fragmentation.
                if (!payload.empty() && payload.size() + line.size() > BATCH_FLUSH_BYTES) {
                    ret |= sendBatch(payload);
                    payload.clear();
                }
                payload += line;
                ++index;
            }
            ret |= sendBatch(payload);
            return ret;
        }

        /// Queue an already-formatted batch (newline-separated metric lines).
        /// The payload is transmitted verbatim.
        int sendBatch(const std::string& payload)
        {
            if (payload.empty()) return 0;
            return enqueue(payload);
        }

        /**
         * Emit several counters in one datagram.
         *
         * Use this where the caller would otherwise perform many enqueue calls
         * in a hot, lock-holding path (e.g. removing a transaction from the
         * mempool, which happens for every tx of every block).
         */
        int countBatch(const std::vector<std::pair<std::string, int64_t>>& metrics)
        {
            std::string payload;
            std::string line;
            int ret{0};
            for (const auto& [key, value] : metrics) {
                line.clear();
                formatSigned(line, key, value, "c", 1.0f);
                if (!payload.empty() && payload.size() + line.size() > BATCH_FLUSH_BYTES) {
                    ret |= sendBatch(payload);
                    payload.clear();
                }
                payload += line;
            }
            ret |= sendBatch(payload);
            return ret;
        }

    public:
        /**
         * (Low Level Api) manually send a message
         * which might be composed of several lines.
         *
         * This only queues the message; the flusher transmits it. Returns 0 if
         * the message was queued, -1 if it was dropped, and 0 if emission is
         * disabled.
         */
        int send(const std::string& message);

        /* (Low Level Api) manually send a message
         * type = "c", "g" or "ms"
         */
        int send(const std::string& key, size_t value,
                const std::string& type, float sample_rate);
        int sendDouble(const std::string& key, double value,
                const std::string& type, float sample_rate);

    protected:
        int init();
        //! Append one formatted metric line to @p out, honouring sampling.
        //! Returns false when the sample was dropped.
        bool format(std::string& out, const std::string& key, size_t value, const std::string& type, float sample_rate);
        bool formatDouble(std::string& out, const std::string& key, double value, const std::string& type, float sample_rate);
        bool formatSigned(std::string& out, const std::string& key, int64_t value, const std::string& type, float sample_rate);

    protected:
        //! Append metric text to the outgoing queue. Never performs I/O.
        int enqueue(std::string_view payload);
        //! Split @p payload into MTU-safe datagrams, cutting only on newlines.
        void transmit(std::string_view payload);
        //! Hand one datagram to the kernel. Returns 0 on success.
        int sendDatagram(const char* data, size_t len);
        //! Record and rate-limit a transmission failure.
        void recordFailure(int error);
        void flushLoop();

    protected:
        struct _StatsdClientData* d;
};

}; // end namespace

/**
 * The single StatsD client shared by every metric emission site.
 *
 * Using one instance (rather than one per translation unit) means the
 * connection is configured once from the -stats* options and all metrics are
 * partitioned by the same -statshostname.
 */
extern statsd::StatsdClient statsClient;

#endif // BITCOIN_STATSD_CLIENT_H
