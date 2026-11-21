#pragma once
// Fake iPhone companion (D-21): speaks the phone side of the wire protocol over plain TCP so the
// Windows side can be built and tested without a device. Grows with the protocol run by run.

#include "net.h"

#include <functional>
#include <string>

namespace cabled::fakephone {

struct Options {
    std::string model = "FakePhone1,1";
    std::string protocolVersion;  // empty = the real version; set to test mismatches
    double throttleMbps = 0;      // > 0 limits phone->PC throughput, to imitate a slow link
    std::function<void(const std::string&)> log;
};

// Serves one connected PC until it says BYE or disconnects.
void serveClient(net::Socket client, const Options& options);

}  // namespace cabled::fakephone
