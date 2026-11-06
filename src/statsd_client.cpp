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
#include <cinttypes>
#include <cstdint>
#include "statsd_client.h"
#include <fcntl.h>
#include <mutex>

using namespace std;
namespace statsd {

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
    if ( fequal(sample_rate, 1.0) )
    {
        return true;
    }

    float p = ((float)random() / RAND_MAX);
    return sample_rate > p;
}

struct _StatsdClientData {
    int     sock;
    struct  sockaddr_in server;

    string  ns;
    string  host;
    string  nodename;
    int     port;
    bool    init;

    // Metrics are emitted from many threads (net, validation, RPC, and the
    // scheduler's heartbeat). The socket is created lazily on first send, so
    // that creation must be serialized.
    std::mutex init_mutex;

    char    errmsg[1024];
};

StatsdClient::StatsdClient(const string& host, int port, const string& ns)
{
    d = new _StatsdClientData;
    d->sock = -1;
    config(host, port, ns);
    srandom(time(NULL));
}

StatsdClient::~StatsdClient()
{
    // close socket
    if (d->sock >= 0) {
        close(d->sock);
        d->sock = -1;
        delete d;
        d = NULL;
    }
}

void StatsdClient::config(const string& host, int port, const string& ns)
{
    d->ns = ns;
    d->host = host;
    d->port = port;
    d->init = false;
    if ( d->sock >= 0 ) {
        close(d->sock);
    }
    d->sock = -1;
}

void StatsdClient::setNodeName(const string& name)
{
    d->nodename = name;
}

int StatsdClient::init()
{
    std::lock_guard<std::mutex> lock(d->init_mutex);
    if ( d->init ) return 0;

    d->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if ( d->sock == -1 ) {
        snprintf(d->errmsg, sizeof(d->errmsg), "could not create socket, err=%m");
        return -1;
    }

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
            return -2;
        }
        struct sockaddr_in* host_addr = (struct sockaddr_in*)result->ai_addr;
        memcpy(&d->server.sin_addr, &host_addr->sin_addr, sizeof(struct in_addr));
        freeaddrinfo(result);
    }

    d->init = true;
    return 0;
}

/* will change the original string */
void StatsdClient::cleanup(string& key)
{
    size_t pos = key.find_first_of(":|@");
    while ( pos != string::npos )
    {
        key[pos] = '_';
        pos = key.find_first_of(":|@");
    }
}

int StatsdClient::dec(const string& key, float sample_rate)
{
    return count(key, -1, sample_rate);
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
    string message;
    if (!formatSigned(message, std::move(key), value, "c", sample_rate)) {
        return 0;
    }
    return send(message);
}

int StatsdClient::gauge(const string& key, size_t value, float sample_rate)
{
    return send(key, value, "g", sample_rate);
}

int StatsdClient::gaugeSigned(const string& key, int64_t value, float sample_rate)
{
    string message;
    if (!formatSigned(message, std::move(key), value, "g", sample_rate)) {
        return 0;
    }
    return send(message);
}

int StatsdClient::gaugeDouble(const string& key, double value, float sample_rate)
{
    return sendDouble(key, value, "g", sample_rate);
}

int StatsdClient::timing(const string& key, size_t ms, float sample_rate)
{
    return send(key, ms, "ms", sample_rate);
}

bool StatsdClient::formatSigned(string& out, string key, int64_t value, const string &type, float sample_rate)
{
    // Gauges are absolute values, not events: sampling them would make the
    // backend drop updates and leave the gauge stale. Always send them.
    if (type == "g") {
        sample_rate = 1.0;
    }

    if (!should_send(sample_rate)) {
        return false;
    }

    // partition stats by node name if set
    if (!d->nodename.empty())
        key = key + "." + d->nodename;

    cleanup(key);

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

bool StatsdClient::format(string& out, string key, size_t value, const string &type, float sample_rate)
{
    // Gauges are absolute values, not events: sampling them would make the
    // backend drop updates and leave the gauge stale. Always send them.
    if (type == "g") {
        sample_rate = 1.0;
    }

    if (!should_send(sample_rate)) {
        return false;
    }

    // partition stats by node name if set
    if (!d->nodename.empty())
        key = key + "." + d->nodename;

    cleanup(key);

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

bool StatsdClient::formatDouble(string& out, string key, double value, const string &type, float sample_rate)
{
    // Gauges are absolute values, not events: sampling them would make the
    // backend drop updates and leave the gauge stale. Always send them.
    if (type == "g") {
        sample_rate = 1.0;
    }

    if (!should_send(sample_rate)) {
        return false;
    }

    // partition stats by node name if set
    if (!d->nodename.empty())
        key = key + "." + d->nodename;

    cleanup(key);

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

int StatsdClient::send(string key, size_t value, const string &type, float sample_rate)
{
    string message;
    if (!format(message, std::move(key), value, type, sample_rate)) {
        return 0;
    }
    return send(message);
}

int StatsdClient::sendDouble(string key, double value, const string &type, float sample_rate)
{
    string message;
    if (!formatDouble(message, std::move(key), value, type, sample_rate)) {
        return 0;
    }
    return send(message);
}

int StatsdClient::send(const string &message)
{
    int ret = init();
    if ( ret )
    {
        return ret;
    }
    ret = sendto(d->sock, message.data(), message.size(), 0, (struct sockaddr *) &d->server, sizeof(d->server));
    if ( ret == -1) {
        snprintf(d->errmsg, sizeof(d->errmsg),
                "sendto server fail, host=%s:%d, err=%m", d->host.c_str(), d->port);
        return -1;
    }
    return 0;
}

const char* StatsdClient::errmsg()
{
    return d->errmsg;
}

} // namespace statsd

statsd::StatsdClient statsClient{statsd::DEFAULT_STATSD_HOST, statsd::DEFAULT_STATSD_PORT, ""};

std::optional<std::string> ConfigureStatsClient(statsd::StatsdClient& client, const ArgsManager& args)
{
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

    LogInfo("Sending statsd metrics to %s:%d (namespace: '%s', node name: '%s')",
            host, static_cast<int>(port_arg), ns, nodename);
    return std::nullopt;
}

