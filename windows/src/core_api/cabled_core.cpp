#define CABLED_CORE_BUILD
#include "cabled_core.h"

#include "engine.h"
#include "net.h"
#include "product.h"

#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

using namespace cabled;

struct cabled_engine {
    net::WinsockSession winsock;
    std::unique_ptr<engine::Engine> engine;
    std::mutex logMutex;
    std::deque<std::string> log;
    std::mutex previewMutex;  // copyPreview's scratch buffer
    std::vector<uint8_t> scratch;
    uint32_t scratchWidth = 0, scratchHeight = 0;
    uint64_t scratchSeq = 0;
};

namespace {

int32_t writeString(const std::string& s, char* buffer, int32_t capacity) {
    const int32_t needed = int32_t(s.size() + 1);
    if (buffer && capacity >= needed) std::memcpy(buffer, s.c_str(), size_t(needed));
    return needed;
}

}  // namespace

extern "C" {

cabled_engine* cabled_engine_create(const char* configJson) {
    engine::EngineConfig c;
    if (configJson && *configJson) {
        auto j = nlohmann::json::parse(configJson, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return nullptr;
        c.tcpTarget = j.value("tcpTarget", c.tcpTarget);
        c.udid = j.value("udid", c.udid);
        c.devicePort = j.value("devicePort", c.devicePort);
        c.width = j.value("width", c.width);
        c.height = j.value("height", c.height);
        c.fps = j.value("fps", c.fps);
        c.bitrate = j.value("bitrate", c.bitrate);
        c.hardwareDecode = j.value("hardwareDecode", c.hardwareDecode);
        c.virtualCamera = j.value("virtualCamera", c.virtualCamera);
        c.previewMaxWidth = j.value("previewMaxWidth", c.previewMaxWidth);
    }
    auto* handle = new cabled_engine();
    c.log = [handle](const std::string& line) {
        std::lock_guard<std::mutex> lock(handle->logMutex);
        handle->log.push_back(line);
        if (handle->log.size() > 500) handle->log.pop_front();
    };
    handle->engine = std::make_unique<engine::Engine>(std::move(c));
    return handle;
}

void cabled_engine_start(cabled_engine* e) {
    if (e) e->engine->start();
}

void cabled_engine_stop(cabled_engine* e) {
    if (e) e->engine->stop();
}

void cabled_engine_destroy(cabled_engine* e) {
    if (!e) return;
    e->engine->stop();
    e->engine.reset();
    delete e;
}

int32_t cabled_engine_status_json(cabled_engine* e, char* buffer, int32_t capacity) {
    if (!e) return 0;
    return writeString(e->engine->status().toJson().dump(), buffer, capacity);
}

int32_t cabled_engine_copy_preview(cabled_engine* e, uint8_t* buffer, int32_t capacity, int32_t* width,
                                   int32_t* height, uint64_t* seq) {
    if (!e || !width || !height || !seq) return 0;
    std::lock_guard<std::mutex> lock(e->previewMutex);
    uint64_t s = *seq;
    if (!e->engine->copyPreview(e->scratch, e->scratchWidth, e->scratchHeight, s)) return 0;
    *width = int32_t(e->scratchWidth);
    *height = int32_t(e->scratchHeight);
    if (!buffer || capacity < int32_t(e->scratch.size())) return -1;
    std::memcpy(buffer, e->scratch.data(), e->scratch.size());
    *seq = s;
    return 1;
}

void cabled_engine_set_preview_enabled(cabled_engine* e, int32_t enabled) {
    if (e) e->engine->setPreviewEnabled(enabled != 0);
}

int32_t cabled_engine_take_log(cabled_engine* e, char* buffer, int32_t capacity) {
    if (!e) return 0;
    std::lock_guard<std::mutex> lock(e->logMutex);
    std::string text;
    for (const auto& line : e->log) text += line + "\n";
    const int32_t needed = writeString(text, buffer, capacity);
    if (buffer && capacity >= needed) e->log.clear();
    return needed;
}

const char* cabled_version(void) { return CABLED_VERSION; }

}  // extern "C"
