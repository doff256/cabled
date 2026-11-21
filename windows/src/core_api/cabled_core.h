#pragma once
/* C ABI of the Cabled engine (cabled_core.dll), used by the WinUI app through P/Invoke (D-16).
 * Narrow on purpose: create/start/stop, a JSON status snapshot, and the latest preview frame.
 * All functions are thread-safe. Strings are UTF-8. */

#include <stdint.h>

#ifdef CABLED_CORE_BUILD
#define CABLED_API __declspec(dllexport)
#else
#define CABLED_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cabled_engine cabled_engine;

/* config_json: optional (NULL = defaults). Keys: tcpTarget, udid, devicePort, width, height, fps,
 * bitrate, hardwareDecode, virtualCamera, previewMaxWidth. Returns NULL on bad JSON. */
CABLED_API cabled_engine* cabled_engine_create(const char* config_json);
CABLED_API void cabled_engine_start(cabled_engine* engine);
CABLED_API void cabled_engine_stop(cabled_engine* engine);
CABLED_API void cabled_engine_destroy(cabled_engine* engine);

/* Writes the status as JSON (NUL-terminated). Returns the bytes needed including the NUL; if that's
 * more than capacity, nothing is written and the caller retries with a bigger buffer. */
CABLED_API int32_t cabled_engine_status_json(cabled_engine* engine, char* buffer, int32_t capacity);

/* Copies the newest preview frame (BGRA, top-down, stride = width*4) if it's newer than *seq.
 * Returns 1 and updates *width, *height, *seq when a frame was copied; 0 if there's nothing new;
 * -1 if capacity is too small (then *width/*height say what's needed). */
CABLED_API int32_t cabled_engine_copy_preview(cabled_engine* engine, uint8_t* buffer, int32_t capacity,
                                              int32_t* width, int32_t* height, uint64_t* seq);

CABLED_API void cabled_engine_set_preview_enabled(cabled_engine* engine, int32_t enabled);

/* R6. camera_id from status "camera.list[].id" (NULL/"" = phone default). Zoom in display units
 * ("1" = main lens, "0.5" = ultra wide); smooth = ramp instead of jump. */
CABLED_API void cabled_engine_set_camera(cabled_engine* engine, const char* camera_id);
CABLED_API void cabled_engine_set_zoom(cabled_engine* engine, double zoom, int32_t smooth);

/* R7: auto-framing on/off (config keys: "framing", "debugFaces"). */
CABLED_API void cabled_engine_set_framing(cabled_engine* engine, int32_t enabled);

/* Engine log lines since the last call, newline-separated (same size contract as status_json). */
CABLED_API int32_t cabled_engine_take_log(cabled_engine* engine, char* buffer, int32_t capacity);

CABLED_API const char* cabled_version(void);

#ifdef __cplusplus
}
#endif
