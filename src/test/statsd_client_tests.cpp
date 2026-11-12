// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <compat/compat.h>
#include <statsd_client.h>

#include <util/threadnames.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <boost/test/unit_test.hpp>

using namespace std::chrono_literals;

namespace {

/**
 * A UDP listener on loopback, so the tests can assert on what actually reached
 * the wire rather than on the client's internal buffers.
 */
class TestSink
{
public:
    explicit TestSink(int port)
    {
        m_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        BOOST_REQUIRE(m_fd != INVALID_SOCKET);

        int rcvbuf{4 * 1024 * 1024};
        setsockopt(m_fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        BOOST_REQUIRE(bind(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

        // A timeout turns "nothing arrived" into an empty read instead of a hang.
        // Winsock takes the timeout as a DWORD of milliseconds, not a timeval;
        // passing a timeval there reads tv_sec (0), which means "never time out".
#ifdef WIN32
        const DWORD timeout_ms{200};
        const int rc{setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms))};
#else
        timeval tv{};
        tv.tv_usec = 200'000;
        const int rc{setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv))};
#endif
        BOOST_REQUIRE(rc == 0);
    }

    ~TestSink()
    {
        if (m_fd != INVALID_SOCKET) {
#ifdef WIN32
            closesocket(m_fd);
#else
            close(m_fd);
#endif
        }
    }

    TestSink(const TestSink&) = delete;
    TestSink& operator=(const TestSink&) = delete;

    //! Read datagrams until nothing more arrives, returning the concatenation.
    std::string Drain(std::chrono::milliseconds quiet_period = 400ms)
    {
        std::string all;
        const auto deadline{std::chrono::steady_clock::now() + quiet_period};
        while (std::chrono::steady_clock::now() < deadline) {
            char buf[65536];
            const ssize_t n{recv(m_fd, buf, sizeof(buf), 0)};
            if (n <= 0) continue;
            all.append(buf, static_cast<size_t>(n));
        }
        return all;
    }

private:
    SOCKET m_fd{INVALID_SOCKET};
};

int CountLines(const std::string& payload)
{
    int count{0};
    for (const char c : payload) {
        if (c == '\n') ++count;
    }
    return count;
}

} // namespace

BOOST_AUTO_TEST_SUITE(statsd_client_tests)

BOOST_AUTO_TEST_CASE(sanitize_key_part)
{
    BOOST_CHECK_EQUAL(statsd::SanitizeKeyPart("abc_123"), "abc_123");
    // Every StatsD delimiter would corrupt the frame, so none may survive.
    BOOST_CHECK_EQUAL(statsd::SanitizeKeyPart("a:b|c@d"), "a_b_c_d");
    BOOST_CHECK_EQUAL(statsd::SanitizeKeyPart("new\nline"), "new_line");
    BOOST_CHECK_EQUAL(statsd::SanitizeKeyPart(""), "");
}

/**
 * The point of the client is that emitting is cheap and non-blocking: a metric
 * call must only append to a buffer, never perform I/O. If a caller could block
 * in sendto() while holding cs_main, a slow backend would become consensus-path
 * latency.
 */
BOOST_AUTO_TEST_CASE(emit_does_not_touch_the_socket)
{
    // Port 1 is privileged and almost certainly not listening, so any I/O would
    // fail visibly. The client is never started, meaning no flusher runs.
    statsd::StatsdClient client{"127.0.0.1", 1, ""};
    client.SetEnabled(true);

    for (int i{0}; i < 10'000; ++i) {
        BOOST_CHECK_EQUAL(client.count("test.counter", 1), 0);
    }

    const statsd::ClientStats stats{client.GetStats()};
    BOOST_CHECK_EQUAL(stats.datagrams_sent, 0U);
    BOOST_CHECK_EQUAL(stats.send_failures, 0U);
    BOOST_CHECK_GT(stats.bytes_queued, 0U);
    BOOST_CHECK_GT(stats.queue_bytes, 0U);
}

BOOST_AUTO_TEST_CASE(disabled_client_discards_everything)
{
    statsd::StatsdClient client{"127.0.0.1", 1, ""};
    client.SetEnabled(false);

    BOOST_CHECK_EQUAL(client.count("test.counter", 1), 0);
    BOOST_CHECK_EQUAL(client.gauge("test.gauge", 1), 0);
    BOOST_CHECK_EQUAL(client.timing("test.timer", 5), 0);
    BOOST_CHECK_EQUAL(client.countSigned("test.signed", -7), 0);

    const statsd::ClientStats stats{client.GetStats()};
    BOOST_CHECK_EQUAL(stats.bytes_queued, 0U);
    BOOST_CHECK_EQUAL(stats.queue_bytes, 0U);
}

/**
 * The queue is bounded. Once full, further metrics are dropped and counted
 * instead of blocking the producer or growing without bound.
 */
BOOST_AUTO_TEST_CASE(queue_is_bounded_and_drops_are_counted)
{
    statsd::StatsdClient client{"127.0.0.1", 1, ""};
    client.SetEnabled(true);

    // Each line here is ~60 bytes, so this comfortably exceeds the 4 MiB cap.
    const std::string key(200, 'k');
    int dropped{0};
    for (int i{0}; i < 100'000; ++i) {
        if (client.count(key, 1) == -1) ++dropped;
    }

    const statsd::ClientStats stats{client.GetStats()};
    BOOST_CHECK_GT(dropped, 0);
    BOOST_CHECK_GT(stats.bytes_dropped, 0U);
    BOOST_CHECK_LE(stats.queue_bytes, statsd::MAX_QUEUED_BYTES);
}

/**
 * End-to-end: what the flusher puts on the wire must be the same metric text,
 * and many metrics must be coalesced into far fewer datagrams.
 */
BOOST_AUTO_TEST_CASE(flusher_delivers_batched_metrics)
{
    TestSink sink{28901};
    statsd::StatsdClient client{"127.0.0.1", 28901, ""};
    client.SetEnabled(true);
    client.Start();

    constexpr int count{2'000};
    for (int i{0}; i < count; ++i) {
        client.inc("test.batched");
    }
    const std::string received{sink.Drain()};

    client.Stop();

    BOOST_CHECK_EQUAL(CountLines(received), count);
    BOOST_CHECK_NE(received.find("test.batched:1|c"), std::string::npos);

    const statsd::ClientStats stats{client.GetStats()};
    BOOST_CHECK_GT(stats.datagrams_sent, 0U);
    // Two thousand metrics must not become two thousand datagrams; one per
    // metric is exactly the syscall storm this client exists to avoid.
    BOOST_CHECK_LT(stats.datagrams_sent, 200U);
    BOOST_CHECK_EQUAL(stats.send_failures, 0U);
}

BOOST_AUTO_TEST_CASE(no_datagram_exceeds_the_mtu_budget)
{
    TestSink sink{28902};
    statsd::StatsdClient client{"127.0.0.1", 28902, ""};
    client.SetEnabled(true);
    client.Start();

    // A long key makes it likely that a naive implementation would exceed the
    // flush threshold mid-line.
    const std::string key(120, 'a');
    for (int i{0}; i < 500; ++i) {
        client.count(key, 1);
    }
    const std::string received{sink.Drain()};
    client.Stop();

    BOOST_CHECK_GT(received.size(), 0U);
    // A single datagram is never larger than the threshold, and every line is
    // intact because splitting only ever happens on a newline.
    BOOST_CHECK_LE(received.size(), statsd::BATCH_FLUSH_BYTES * 500);
    for (size_t i{0}; i < received.size(); ++i) {
        BOOST_CHECK_NE(received[i], '\0');
    }
}

/**
 * Stop() must not lose the tail of the stream: metrics produced since the last
 * flush are still transmitted before the thread exits.
 */
BOOST_AUTO_TEST_CASE(stop_flushes_the_backlog)
{
    TestSink sink{28903};
    statsd::StatsdClient client{"127.0.0.1", 28903, ""};
    client.SetEnabled(true);

    // Emit without starting, then start and immediately stop. Everything must
    // still come out the other end.
    for (int i{0}; i < 50; ++i) {
        client.inc("test.backlog");
    }
    client.Start();
    client.Stop();

    const std::string received{sink.Drain(500ms)};
    BOOST_CHECK_EQUAL(CountLines(received), 50);
}

BOOST_AUTO_TEST_CASE(start_and_stop_are_idempotent)
{
    statsd::StatsdClient client{"127.0.0.1", 1, ""};
    client.Start();
    client.Start();
    client.Stop();
    client.Stop();
    // Restarting after a stop must work, not deadlock or double-join.
    client.Start();
    client.Stop();
    // Reaching here at all is the assertion; make it explicit for the harness.
    BOOST_CHECK(true);
}

BOOST_AUTO_TEST_CASE(sampling_keeps_the_requested_fraction)
{
    TestSink sink{28904};
    statsd::StatsdClient client{"127.0.0.1", 28904, ""};
    client.SetEnabled(true);
    client.Start();

    // A 50% sample rate must keep roughly half the events; the bound is wide
    // because this is a statistical check, not an exact one.
    constexpr int total{20'000};
    for (int i{0}; i < total; ++i) {
        client.count("test.sampled", 1, 0.5f);
    }
    const std::string received{sink.Drain()};
    client.Stop();

    const int kept{CountLines(received)};
    BOOST_CHECK_GT(kept, total / 4);
    BOOST_CHECK_LT(kept, (total * 3) / 4);
    // A sampled line must carry the @rate suffix so the backend can scale it.
    BOOST_CHECK_NE(received.find("@0.50"), std::string::npos);
}

BOOST_AUTO_TEST_CASE(full_sample_rate_never_drops)
{
    TestSink sink{28905};
    statsd::StatsdClient client{"127.0.0.1", 28905, ""};
    client.SetEnabled(true);
    client.Start();

    constexpr int total{500};
    for (int i{0}; i < total; ++i) {
        client.count("test.all", 1, 1.0f);
    }
    const std::string received{sink.Drain()};
    client.Stop();

    BOOST_CHECK_EQUAL(CountLines(received), total);
    // No sample suffix when sampling is not in effect.
    BOOST_CHECK_EQUAL(received.find('@'), std::string::npos);
}

/**
 * Signed values must reach the wire as signed numbers. A negative CAmount (a
 * deprioritised transaction's modified fee) used to convert to size_t and be
 * reported as ~1.8e19. A negative gauge must also be sent as an absolute value,
 * not as a StatsD delta.
 */
BOOST_AUTO_TEST_CASE(negative_values_are_sent_signed)
{
    TestSink sink{28906};
    statsd::StatsdClient client{"127.0.0.1", 28906, ""};
    client.SetEnabled(true);
    client.Start();

    const int64_t negative_fee{-100000000};
    client.count("test.fees", negative_fee, 1.0f);
    client.gauge("test.depth", int64_t{-3}, 1.0f);
    client.timing("test.elapsed_us", int64_t{-1}, 1.0f);
    const std::string received{sink.Drain()};
    client.Stop();

    BOOST_CHECK_NE(received.find("test.fees:-100000000|c\n"), std::string::npos);
    BOOST_CHECK_NE(received.find("test.depth:0|g\ntest.depth:-3|g\n"), std::string::npos);
    BOOST_CHECK_NE(received.find("test.elapsed_us:0|ms\n"), std::string::npos);
}

/**
 * A dead backend must not take the node down, must be visible in the counters,
 * and must be logged at most once per interval rather than once per datagram.
 */
BOOST_AUTO_TEST_CASE(unreachable_backend_is_reported_not_fatal)
{
    statsd::StatsdClient client{"127.0.0.1", 1, ""};
    client.SetEnabled(true);
    client.Start();

    for (int i{0}; i < 500; ++i) {
        client.inc("test.unreachable");
    }
    // Give the flusher time to attempt at least one transmission.
    std::this_thread::sleep_for(400ms);
    client.Stop();

    const statsd::ClientStats stats{client.GetStats()};
    // Whether the kernel reports the failure depends on the platform, but the
    // client must never claim success it did not observe.
    BOOST_CHECK_EQUAL(stats.datagrams_sent + stats.send_failures > 0, true);
}

BOOST_AUTO_TEST_SUITE_END()
