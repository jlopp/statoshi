#ifndef STATSD_CLIENT_H
#define STATSD_CLIENT_H

#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <string>
#include <type_traits>
#include <optional>
#include <utility>
#include <vector>

class ArgsManager;

namespace statsd {

struct _StatsdClientData;

//! Default host metrics are sent to when -statshost is not set.
inline constexpr const char* DEFAULT_STATSD_HOST{"127.0.0.1"};
//! Default UDP port metrics are sent to when -statsport is not set.
inline constexpr int DEFAULT_STATSD_PORT{8125};
//! Payload size at which a metric batch is flushed, so that one datagram stays
//! within a typical 1500-byte MTU instead of being IP-fragmented.
inline constexpr size_t BATCH_FLUSH_BYTES{1400};

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
        const char* errmsg();

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
         * whole table of fee estimates), this is dramatically cheaper than one
         * send() per value: the syscall dominates the per-metric cost.
         *
         * @param prefix prepended to the numeric suffix of each key
         * @param values one gauge per element; element i is emitted as
         *               @p prefix + std::to_string(first_index + i)
         */
        template <typename Container>
        int gaugeBatch(const std::string& prefix, const Container& values, size_t first_index = 0)
        {
            std::string payload;
            size_t index{first_index};
            int ret{0};
            for (const auto& value : values) {
                if constexpr (std::is_floating_point_v<typename Container::value_type>) {
                    formatDouble(payload, prefix + std::to_string(index), value, "g", 1.0);
                } else {
                    format(payload, prefix + std::to_string(index), value, "g", 1.0);
                }
                ++index;
                // Flush before the payload grows past a typical MTU, so one
                // batch never relies on IP fragmentation.
                if (payload.size() >= BATCH_FLUSH_BYTES) {
                    ret |= sendBatch(payload);
                    payload.clear();
                }
            }
            ret |= sendBatch(payload);
            return ret;
        }

        /// Emit an already-formatted batch (newline-separated metric lines) as
        /// one datagram. The payload is sent verbatim.
        int sendBatch(const std::string& payload)
        {
            if (payload.empty()) return 0;
            return send(payload);
        }

        /**
         * Emit several counters in one datagram.
         *
         * Use this where the caller would otherwise perform many send() calls
         * in a hot, lock-holding path (e.g. removing a transaction from the
         * mempool, which happens for every tx of every block).
         */
        int countBatch(const std::vector<std::pair<std::string, int64_t>>& metrics)
        {
            std::string payload;
            int ret{0};
            for (const auto& [key, value] : metrics) {
                formatSigned(payload, key, value, "c", 1.0f);
                if (payload.size() >= BATCH_FLUSH_BYTES) {
                    ret |= sendBatch(payload);
                    payload.clear();
                }
            }
            ret |= sendBatch(payload);
            return ret;
        }

    public:
        /**
         * (Low Level Api) manually send a message
         * which might be composed of several lines.
         */
        int send(const std::string& message);

        /* (Low Level Api) manually send a message
         * type = "c", "g" or "ms"
         */
        int send(std::string key, size_t value,
                const std::string& type, float sample_rate);
        int sendDouble(std::string key, double value,
                const std::string& type, float sample_rate);

    protected:
        int init();
        void cleanup(std::string& key);
        //! Append one formatted metric line to @p out, honouring sampling.
        //! Returns false when the sample was dropped.
        bool format(std::string& out, std::string key, size_t value, const std::string& type, float sample_rate);
        bool formatDouble(std::string& out, std::string key, double value, const std::string& type, float sample_rate);
        bool formatSigned(std::string& out, std::string key, int64_t value, const std::string& type, float sample_rate);

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

/**
 * Apply the -statshost, -statsport, -statsnamespace and -statshostname options
 * to @p client. Call once during startup, before any metrics are emitted.
 *
 * @return std::nullopt on success, otherwise a description of the invalid
 *         option value.
 */
std::optional<std::string> ConfigureStatsClient(statsd::StatsdClient& client, const ArgsManager& args);

#endif
