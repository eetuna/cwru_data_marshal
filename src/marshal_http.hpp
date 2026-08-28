/*
 * File: src/marshal_http.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: HTTP routing — query/control endpoints only
 *
 * Scanner data arrives via MRD TCP (mrd_tcp_listener.hpp), NOT HTTP.
 * Recon images arrive via MRD TCP (recon_forwarder.hpp reader thread), NOT HTTP.
 *
 * HTTP endpoints (for non-scanner clients: viz, pose, tracker):
 *   GET  /image/latest, /transform, /pose, /health, /dump/scanner, /dump/recon
 *   PUT  /transform
 *   POST /pose
 */

#pragma once

#undef LOG_COMPONENT
#define LOG_COMPONENT "marshal_http"
#include "logging.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <sstream>
#include <vector>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <ismrmrd/ismrmrd.h>
#include <ismrmrd/waveform.h>

#include "marshal_state.hpp"
#include "live_image_store.hpp"
#include "mrd_io.hpp"
#include "mrd_sink.hpp"
#include "mrd_stream_tags.hpp"
#include "slice_math.hpp"

namespace beast = boost::beast;
namespace http  = beast::http;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

template <class Body>
static auto json_response(const http::request<Body>& req,
                          http::status status,
                          const nlohmann::json& j)
{
    http::response<http::string_body> res{status, req.version()};
    res.set(http::field::content_type, "application/json");
    res.keep_alive(req.keep_alive());
    res.body() = j.dump();
    res.prepare_payload();
    return res;
}

template <class Body>
static auto text_response(const http::request<Body>& req,
                          http::status status,
                          const std::string& text)
{
    http::response<http::string_body> res{status, req.version()};
    res.set(http::field::content_type, "text/plain");
    res.keep_alive(req.keep_alive());
    res.body() = text;
    res.prepare_payload();
    return res;
}

// ---------------------------------------------------------------------------
// handle_recon_image: archive recon image + update latest-file side channel.
// Called from ReconForwarder callback (MRD TCP path). Scanner return is handled
// by the callback in marshal_main.cpp and must not wait on latest-file HDF5 I/O.
// ---------------------------------------------------------------------------
// require_epoch (see live_image_store.hpp): the recon session's epoch; the
// image is dropped if a newer scan owns the state.
inline void handle_recon_image(MarshalState& state, const void* data, size_t size,
                               uint64_t require_epoch = mrd::kAnyEpoch)
{
    // Track slice geometry in every mode (recon lane) — the slice-translation
    // command pushed to the scanner embeds it.
    mrd::update_slice_geometry(state, static_cast<const uint8_t*>(data), size);

    // Dump mode is exclusive of the live snapshot/history pipeline. Gate
    // purely on the mode flag so a missing dump_recorder does not silently
    // fall back to writing live output.
    if (state.dump_enabled) {
        if (require_epoch != mrd::kAnyEpoch && state.scan_epoch.load() != require_epoch)
            return;
        if (state.dump_recorder) {
            const auto* bytes = static_cast<const uint8_t*>(data);
            state.dump_recorder->append_recon_image(
                std::vector<uint8_t>(bytes, bytes + size));
        }
        return;
    }

    mrd::append_live_image(state, mrd::LiveLane::Recon,
                           static_cast<const uint8_t*>(data), size, require_epoch);
}

inline void handle_recon_waveform(MarshalState& state, const void* data, size_t size,
                                  uint64_t require_epoch = mrd::kAnyEpoch)
{
    if (size < mrd::WAVEFORM_HEADER_BYTES) return;
    const auto* whdr = static_cast<const ISMRMRD::WaveformHeader*>(data);
    size_t data_bytes = size_t(whdr->number_of_samples) * whdr->channels * sizeof(uint32_t);
    if (size < mrd::WAVEFORM_HEADER_BYTES + data_bytes) return;
    if (state.dump_enabled) {
        if (require_epoch != mrd::kAnyEpoch && state.scan_epoch.load() != require_epoch)
            return;
        if (state.dump_recorder) {
            const auto* bytes = static_cast<const uint8_t*>(data);
            state.dump_recorder->append_recon_waveform(
                std::vector<uint8_t>(bytes, bytes + size));
        }
        return;
    }
    // Live mode: persist recon-side waveforms via the per-lane history
    // recorder so default-mode runs capture ECG too (not just images).
    mrd::append_live_waveform(state, mrd::LiveLane::Recon,
                              static_cast<const uint8_t*>(data), size, require_epoch);
}

// ---------------------------------------------------------------------------
// GET /image/latest
// ---------------------------------------------------------------------------
template <class Body>
static auto handle_get_image_latest(const http::request<Body>& req, MarshalState& state)
    -> http::response<http::string_body>
{
    if (state.dump_enabled) {
        return json_response(req, http::status::not_found,
                             {{"error", "dump mode; no live snapshot"}});
    }
    std::lock_guard<std::mutex> lk(state.latest_image_mtx);
    if (state.latest_image_path.empty()) {
        http::response<http::string_body> res{http::status::no_content, req.version()};
        res.keep_alive(req.keep_alive());
        return res;
    }
    nlohmann::json j;
    j["path"] = state.latest_image_path;
    j["error"] = state.latest_image_error;
    // Monotonic publish generation, bumped on every successful snapshot
    // publish. Clients remember the last value and skip re-reading the
    // snapshot when it hasn't changed.
    j["generation"] = state.latest_image_generation.load();
    return json_response(req, http::status::ok, j);
}

// ---------------------------------------------------------------------------
// GET /image/latest.h5 — the snapshot BYTES over HTTP.
//
// The JSON endpoint above returns a filesystem path, which only works for
// readers that share the marshal's volume. This endpoint serves the same
// closed, atomically-renamed HDF5 snapshot as the response body, so clients
// on OTHER machines can fetch the latest image with a plain HTTP GET and
// open it in-memory (e.g. h5py.File(io.BytesIO(body))).
//
// ETag carries the publish generation: send If-None-Match with the previous
// ETag and the marshal answers 304 Not Modified when nothing new was
// published — pollers only download actual new images.
// ---------------------------------------------------------------------------
template <class Body>
static auto handle_get_image_latest_h5(const http::request<Body>& req, MarshalState& state)
    -> http::response<http::string_body>
{
    if (state.dump_enabled) {
        return json_response(req, http::status::not_found,
                             {{"error", "dump mode; no live snapshot"}});
    }

    std::string path;
    bool error = false;
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lk(state.latest_image_mtx);
        path = state.latest_image_path;
        error = state.latest_image_error;
        generation = state.latest_image_generation.load();
    }
    if (path.empty()) {
        http::response<http::string_body> res{http::status::no_content, req.version()};
        res.keep_alive(req.keep_alive());
        return res;
    }
    if (error) {
        return json_response(req, http::status::service_unavailable,
                             {{"error", "reconstruction failing"}, {"path", path}});
    }

    const std::string etag = "\"" + std::to_string(generation) + "\"";
    auto inm = req.find(http::field::if_none_match);
    if (inm != req.end() && inm->value() == etag) {
        http::response<http::string_body> res{http::status::not_modified, req.version()};
        res.set(http::field::etag, etag);
        res.keep_alive(req.keep_alive());
        return res;
    }

    // The snapshot is a closed file replaced by atomic rename; an open FD
    // keeps whichever complete version we opened.
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return json_response(req, http::status::not_found,
                             {{"error", "snapshot unreadable"}, {"path", path}});
    }
    std::string body((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());

    http::response<http::string_body> res{http::status::ok, req.version()};
    res.set(http::field::content_type, "application/x-hdf5");
    res.set(http::field::etag, etag);
    res.set(http::field::cache_control, "no-store");
    res.keep_alive(req.keep_alive());
    res.body() = std::move(body);
    res.prepare_payload();
    return res;
}

// ---------------------------------------------------------------------------
// GET /transform — returns and zeros
// ---------------------------------------------------------------------------
template <class Body>
static auto handle_get_transform(const http::request<Body>& req, MarshalState& state)
{
    std::lock_guard<std::mutex> lk(state.transform_mtx);
    nlohmann::json j;
    j["through_plane_mm"] = state.transform.through_plane_mm;
    j["readout_mm"] = state.transform.readout_mm;
    j["phase_mm"] = state.transform.phase_mm;
    j["rotation_rad"] = state.transform.rotation_rad;
    // Atomically zero after returning
    state.transform.clear();
    return json_response(req, http::status::ok, j);
}

// ---------------------------------------------------------------------------
// PUT /transform
// ---------------------------------------------------------------------------
template <class Body>
static auto handle_put_transform(const http::request<Body>& req, MarshalState& state)
{
    try {
        auto j = nlohmann::json::parse(req.body());
        std::lock_guard<std::mutex> lk(state.transform_mtx);
        state.transform.through_plane_mm = j.value("through_plane_mm", 0.0);
        state.transform.readout_mm = j.value("readout_mm", 0.0);
        state.transform.phase_mm = j.value("phase_mm", 0.0);
        state.transform.rotation_rad = j.value("rotation_rad", 0.0);
    } catch (const std::exception& e) {
        return json_response(req, http::status::bad_request,
                             {{"error", std::string("bad JSON: ") + e.what()}});
    }
    return json_response(req, http::status::ok, {{"status", "ok"}});
}

// ---------------------------------------------------------------------------
// Slice command channel (Ridaa's WebGL -> marshal -> Andrew's slice_agent)
//
// The marshal does what Andrew's slice_control keyboard tool does: it keeps
// six absolute numbers — tx ty tz (mm), rx ry rz (deg) — starting at zero,
// adds each UI move to them with his arithmetic (slice_math::apply_step) and
// sends the totals to `slice_agent --listen` as a 56-byte SliceCommand.
//
//   POST /write/slice_delta   { "translation_mm": [..], "rotation_rad": [..] }
//       translation_mm[k]: along row k of buildRotMatrix (0 read, 1 phase,
//       2 slice normal) — his arrows / PgUp / PgDn; rotation_rad[k]: added
//       to the k-th angle — his W/S, A/D, Q/E.
//   POST /write/file_slice_translation { "values": [ ±1 ] }   the ± buttons:
//       1 mm along the slice normal (PgUp / PgDn).
//   POST /write/slice_target  { "position", "read_dir", "phase_dir", "slice_dir" }
//       Ridaa's "Send Absolute Position": header pose -> the six numbers.
//
// Responses: {"file", "delivered" (written to a connected agent), "enabled",
// "state" (the six numbers)}. Requests are cached at GET /read/... (204 until
// the first POST). The six numbers go back to zero at scan start.
// ---------------------------------------------------------------------------

// JSON snapshot of the geometry cache. {"latest_slice":-1,"slices":{}} when
// nothing has been observed yet.
inline nlohmann::json slice_geometry_json(MarshalState& state)
{
    nlohmann::json j;
    nlohmann::json slices = nlohmann::json::object();
    std::lock_guard<std::mutex> lk(state.slice_geom_mtx);
    for (const auto& [idx, g] : state.slice_geom) {
        slices[std::to_string(idx)] = {
            {"slice", g.slice},
            {"position", {g.position[0], g.position[1], g.position[2]}},
            {"read_dir", {g.read_dir[0], g.read_dir[1], g.read_dir[2]}},
            {"phase_dir", {g.phase_dir[0], g.phase_dir[1], g.phase_dir[2]}},
            {"slice_dir", {g.slice_dir[0], g.slice_dir[1], g.slice_dir[2]}},
            {"ts", g.ts},
        };
    }
    j["latest_slice"] = state.latest_slice;
    j["slices"] = std::move(slices);
    return j;
}

inline nlohmann::json state_to_json(const slice_math::SliceState& s)
{
    return {{"tx", s.t[0] + 0.0}, {"ty", s.t[1] + 0.0}, {"tz", s.t[2] + 0.0},
            {"rx_deg", s.r_deg[0] + 0.0}, {"ry_deg", s.r_deg[1] + 0.0}, {"rz_deg", s.r_deg[2] + 0.0}};
}

// Record the new six numbers and queue them for the agent. Called with
// state.slice_state_mtx HELD (keeps commands in order across HTTP threads);
// only the non-blocking post happens under the lock.
inline uint64_t commit_locked(MarshalState& state, const slice_math::SliceState& s,
                              const std::string& ts)
{
    state.slice_state = s;
    state.slice_state_ts = ts;
    ++state.slice_state_count;
    return state.slice_agent_post(slice_math::command_from_state(s));
}

// Wait for the agent client's verdict (lock NOT held) and build the response.
template <class Body>
static auto send_response(const http::request<Body>& req, MarshalState& state,
                          const char* file, const slice_math::SliceState& s, uint64_t gen,
                          nlohmann::json extra)
    -> http::response<http::string_body>
{
    nlohmann::json out = extra;
    out["file"] = file;
    const auto verdict = state.slice_agent_wait(gen);
    out["delivered"] = (verdict == slice_math::Delivery::Delivered);
    // A newer command was posted before this one was sent; the newer one
    // went out instead and these values were never applied.
    out["superseded"] = (verdict == slice_math::Delivery::Superseded);
    out["enabled"] = state.slice_agent_cfg.enabled;
    out["state"] = state_to_json(s);
    return json_response(req, http::status::ok, out);
}

// One relative move: accumulate under the lock, then send.
template <class Body>
static auto apply_step_and_send(const http::request<Body>& req, MarshalState& state,
                                const char* file,
                                const slice_math::Vec3& t_mm,
                                const slice_math::Vec3& r_deg,
                                const std::string& ts,
                                nlohmann::json extra)
    -> http::response<http::string_body>
{
    slice_math::SliceState next;
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lk(state.slice_state_mtx);
        next = state.slice_state.value_or(slice_math::SliceState{});
        slice_math::apply_step(next, t_mm, r_deg);
        gen = commit_locked(state, next, ts);
    }
    return send_response(req, state, file, next, gen, std::move(extra));
}

template <class Body>
static auto handle_post_slice_translation(const http::request<Body>& req, MarshalState& state)
    -> http::response<http::string_body>
{
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body());
    } catch (const std::exception& e) {
        return json_response(req, http::status::bad_request,
                             {{"error", std::string("bad JSON: ") + e.what()}});
    }

    if (!body.contains("values") || !body["values"].is_array() || body["values"].size() != 1) {
        return json_response(req, http::status::bad_request,
                             {{"error", "missing or invalid values"}, {"required", "values[1]"}});
    }
    const auto& direction_value = body["values"][0];
    if (!direction_value.is_number()) {
        return json_response(req, http::status::bad_request,
                             {{"error", "values[0] must be numeric"}});
    }
    const double direction = direction_value.get<double>();
    if (direction != 1.0 && direction != -1.0) {
        return json_response(req, http::status::bad_request,
                             {{"error", "values[0] must be +1 or -1"}, {"got", direction}});
    }

    const std::string ts = mrd::iso8601_now_ms();
    body["ts"] = ts;
    {
        std::lock_guard<std::mutex> lk(state.slice_translation_mtx);
        state.latest_slice_translation_json = body.dump();
    }

    // ±1 = one millimetre along the slice normal (PgUp / PgDn).
    return apply_step_and_send(req, state, "file_slice_translation",
                               {0.0, 0.0, direction}, {0.0, 0.0, 0.0}, ts,
                               {{"direction", direction}});
}

// GET /read/slice_geometry — position/orientation per slice as observed in
// this scan's image headers (both lanes). 204 until the first image.
template <class Body>
static auto handle_get_slice_geometry(const http::request<Body>& req, MarshalState& state)
    -> http::response<http::string_body>
{
    auto j = slice_geometry_json(state);
    if (j["latest_slice"].get<int>() < 0) {
        http::response<http::string_body> res{http::status::no_content, req.version()};
        res.keep_alive(req.keep_alive());
        res.prepare_payload();
        return res;
    }
    return json_response(req, http::status::ok, j);
}

// ---------------------------------------------------------------------------
// POST /write/slice_target and GET /read/slice_target
//
// Absolute slice prescription from the UI: "put the slice exactly here,
// facing this way". Body (JSON):
//   { "position": [x,y,z],                       // slice center, mm (PCS)
//     "read_dir": [..], "phase_dir": [..], "slice_dir": [..] }  // unit vectors
// Sent to the scanner-side slice_agent as an absolute geometry. The latest
// prescription is cached; GET returns it without clearing.
// ---------------------------------------------------------------------------

// Parse a 3-vector field. Returns false (and sets err) on absence/shape/type.
inline bool parse_vec3(const nlohmann::json& body, const char* key,
                       double out[3], std::string& err)
{
    if (!body.contains(key) || !body[key].is_array() || body[key].size() != 3) {
        err = std::string(key) + " must be a 3-element array";
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (!body[key][i].is_number()) {
            err = std::string(key) + "[" + std::to_string(i) + "] must be numeric";
            return false;
        }
        out[i] = body[key][i].get<double>();
    }
    return true;
}

template <class Body>
static auto handle_post_slice_target(const http::request<Body>& req, MarshalState& state)
    -> http::response<http::string_body>
{
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body());
    } catch (const std::exception& e) {
        return json_response(req, http::status::bad_request,
                             {{"error", std::string("bad JSON: ") + e.what()}});
    }

    slice_math::Geometry geom;
    std::string err;
    if (!parse_vec3(body, "position", geom.position.data(), err) ||
        !parse_vec3(body, "read_dir", geom.read_dir.data(), err) ||
        !parse_vec3(body, "phase_dir", geom.phase_dir.data(), err) ||
        !parse_vec3(body, "slice_dir", geom.slice_dir.data(), err)) {
        return json_response(req, http::status::bad_request, {{"error", err}});
    }

    // The three direction vectors must be unit length, mutually orthogonal
    // and right-handed — otherwise there is no rotation to send.
    constexpr double kTol = 1e-3;
    const slice_math::Vec3* dirs[3] = {&geom.read_dir, &geom.phase_dir, &geom.slice_dir};
    const char* names[3] = {"read_dir", "phase_dir", "slice_dir"};
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(slice_math::dot(*dirs[i], *dirs[i]) - 1.0) > kTol) {
            return json_response(req, http::status::bad_request,
                {{"error", std::string(names[i]) + " must be unit length"}});
        }
        for (int k = i + 1; k < 3; ++k) {
            if (std::fabs(slice_math::dot(*dirs[i], *dirs[k])) > kTol) {
                return json_response(req, http::status::bad_request,
                    {{"error", std::string(names[i]) + " and " + names[k]
                               + " must be orthogonal"}});
            }
        }
    }
    if (!slice_math::is_right_handed(geom)) {
        return json_response(req, http::status::bad_request,
            {{"error", "left-handed geometry: read_dir x phase_dir must equal slice_dir"}});
    }

    const slice_math::SliceState next = slice_math::state_from_geometry(geom);
    const std::string ts = mrd::iso8601_now_ms();
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lk(state.slice_state_mtx);
        gen = commit_locked(state, next, ts);
        body["ts"] = ts;
        std::lock_guard<std::mutex> tlk(state.slice_target_mtx);
        state.latest_slice_target_json = body.dump();
    }
    return send_response(req, state, "slice_target", next, gen, nlohmann::json::object());
}

template <class Body>
static auto handle_get_slice_target(const http::request<Body>& req, MarshalState& state)
    -> http::response<http::string_body>
{
    std::string cached;
    {
        std::lock_guard<std::mutex> lk(state.slice_target_mtx);
        cached = state.latest_slice_target_json;
    }
    if (cached.empty()) {
        http::response<http::string_body> res{http::status::no_content, req.version()};
        res.keep_alive(req.keep_alive());
        res.prepare_payload();
        return res;
    }
    try {
        return json_response(req, http::status::ok, nlohmann::json::parse(cached));
    } catch (...) {
        return json_response(req, http::status::internal_server_error,
                             {{"error", "failed to parse slice target cache"}});
    }
}

// ---------------------------------------------------------------------------
// POST /write/slice_delta and GET /read/slice_delta
//
// Relative move from the WebGL ± button and rotation sliders. Body (JSON),
// at least one field required, the other defaults to zero:
//   { "translation_mm": [d_read, d_phase, d_normal],   // along rows 0/1/2 of
//                                                      // buildRotMatrix(rx,ry,rz)
//     "rotation_rad":   [d_rx, d_ry, d_rz] }           // added to the angles
// slice_control.cpp's key arithmetic verbatim.
// ---------------------------------------------------------------------------
template <class Body>
static auto handle_post_slice_delta(const http::request<Body>& req, MarshalState& state)
    -> http::response<http::string_body>
{
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body());
    } catch (const std::exception& e) {
        return json_response(req, http::status::bad_request,
                             {{"error", std::string("bad JSON: ") + e.what()}});
    }

    const bool has_t = body.contains("translation_mm");
    const bool has_r = body.contains("rotation_rad");
    if (!has_t && !has_r) {
        return json_response(req, http::status::bad_request,
            {{"error", "need translation_mm and/or rotation_rad"}});
    }
    double t[3] = {0, 0, 0}, r[3] = {0, 0, 0};
    std::string err;
    if (has_t && !parse_vec3(body, "translation_mm", t, err))
        return json_response(req, http::status::bad_request, {{"error", err}});
    if (has_r && !parse_vec3(body, "rotation_rad", r, err))
        return json_response(req, http::status::bad_request, {{"error", err}});

    const std::string ts = mrd::iso8601_now_ms();
    body["ts"] = ts;
    {
        std::lock_guard<std::mutex> lk(state.slice_delta_mtx);
        state.latest_slice_delta_json = body.dump();
    }
    return apply_step_and_send(req, state, "slice_delta",
                               {t[0], t[1], t[2]},
                               {r[0] * slice_math::kRad2Deg, r[1] * slice_math::kRad2Deg, r[2] * slice_math::kRad2Deg},
                               ts, nlohmann::json::object());
}

template <class Body>
static auto handle_get_slice_delta(const http::request<Body>& req, MarshalState& state)
    -> http::response<http::string_body>
{
    std::string cached;
    {
        std::lock_guard<std::mutex> lk(state.slice_delta_mtx);
        cached = state.latest_slice_delta_json;
    }
    if (cached.empty()) {
        http::response<http::string_body> res{http::status::no_content, req.version()};
        res.keep_alive(req.keep_alive());
        res.prepare_payload();
        return res;
    }
    try {
        return json_response(req, http::status::ok, nlohmann::json::parse(cached));
    } catch (...) {
        return json_response(req, http::status::internal_server_error,
                             {{"error", "failed to parse slice delta cache"}});
    }
}

template <class Body>
static auto handle_get_slice_translation(const http::request<Body>& req, MarshalState& state)
    -> http::response<http::string_body>
{
    std::string cached;
    {
        std::lock_guard<std::mutex> lk(state.slice_translation_mtx);
        cached = state.latest_slice_translation_json;
    }
    if (cached.empty()) {
        // 204 must be body-less; json_response would set a body and
        // prepare_payload() would throw.
        http::response<http::string_body> res{http::status::no_content, req.version()};
        res.keep_alive(req.keep_alive());
        res.prepare_payload();
        return res;
    }
    try {
        return json_response(req, http::status::ok, nlohmann::json::parse(cached));
    } catch (...) {
        return json_response(req, http::status::internal_server_error,
                             {{"error", "failed to parse slice translation cache"}});
    }
}

// ---------------------------------------------------------------------------
// POST /pose and GET /pose
// ---------------------------------------------------------------------------
template <class Body>
static auto handle_post_pose(const http::request<Body>& req, MarshalState& state)
    -> http::response<http::string_body>
{
    try {
        auto j = nlohmann::json::parse(req.body());
        Pose pose;
        pose.t = std::chrono::system_clock::now();
        if (j.contains("position")) {
            auto& pos = j["position"];
            pose.p[0] = pos[0].template get<double>();
            pose.p[1] = pos[1].template get<double>();
            pose.p[2] = pos[2].template get<double>();
        }
        if (j.contains("orientation")) {
            auto& ori = j["orientation"];
            for (size_t i = 0; i < 9 && i < ori.size(); ++i)
                pose.R[i] = ori[i].template get<double>();
        }
        state.poses.set(pose);
    } catch (const std::exception& e) {
        return json_response(req, http::status::bad_request,
                             {{"error", std::string("bad JSON: ") + e.what()}});
    }
    return json_response(req, http::status::ok, {{"status", "ok"}});
}

template <class Body>
static auto handle_get_pose(const http::request<Body>& req, MarshalState& state)
{
    auto p = state.poses.get();
    return json_response(req, http::status::ok, pose_to_json(p));
}

// ---------------------------------------------------------------------------
// GET /dump/scanner and GET /dump/recon
// ---------------------------------------------------------------------------
static nlohmann::json list_dump_files(const std::filesystem::path& dir)
{
    namespace fs = std::filesystem;
    nlohmann::json arr = nlohmann::json::array();
    if (!fs::exists(dir)) return arr;
    for (auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() == ".h5" && entry.path().filename().string().rfind("scan_", 0) == 0) {
            nlohmann::json item;
            item["path"] = entry.path().string();
            std::error_code ec;
            item["size"] = fs::file_size(entry.path(), ec);
            auto ftime = fs::last_write_time(entry.path(), ec);
            if (!ec) {
                auto sys_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                item["modified"] = std::chrono::duration_cast<std::chrono::seconds>(
                    sys_time.time_since_epoch()).count();
            }
            arr.push_back(item);
        }
    }
    return arr;
}

template <class Body>
static auto handle_get_dump_scanner(const http::request<Body>& req, MarshalState& state)
{
    return json_response(req, http::status::ok,
                         list_dump_files(mrd::dump_scanner_dir(state.dump_dir)));
}

template <class Body>
static auto handle_get_dump_recon(const http::request<Body>& req, MarshalState& state)
{
    return json_response(req, http::status::ok,
                         list_dump_files(mrd::dump_recon_dir(state.dump_dir)));
}

// ---------------------------------------------------------------------------
// GET /health
// ---------------------------------------------------------------------------
template <class Body>
static auto handle_get_health(const http::request<Body>& req, MarshalState& state)
{
    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - state.start).count();
    return json_response(req, http::status::ok,
                         {{"status", "ok"}, {"uptime_s", uptime}});
}

// ---------------------------------------------------------------------------
// GET /status — one-glance operational summary for experiment operators:
// mode, scanner/recon connectivity, current scan, last-publish age, disk.
// Read-only; aggregates state that already exists in-process.
// ---------------------------------------------------------------------------
template <class Body>
static auto handle_get_status(const http::request<Body>& req, MarshalState& state)
{
    nlohmann::json j;
    j["mode"] = state.dump_enabled ? "dump" : "live";
    auto now = std::chrono::steady_clock::now();
    j["uptime_s"] = std::chrono::duration_cast<std::chrono::seconds>(now - state.start).count();

    j["scanner_connected"] = state.mrd_scanner_connected();

    j["recon"]["configured"] = !state.recon_url.empty();
    j["recon"]["target"] = state.recon_url;
    j["recon"]["connected"] = state.recon_connected();

    {
        std::lock_guard<std::mutex> lk(state.scan_mtx);
        j["scan"]["active"] = state.header_received.load();
        j["scan"]["file"] = state.current_scan_filename;
    }
    j["images"]["from_scanner_total"] = state.perf_scanner_images_received.load();
    j["images"]["from_recon_total"] = state.perf_recon_images_received.load();

    const int64_t lp = state.last_publish_ms.load();
    if (lp > 0) {
        j["last_image_age_s"] =
            static_cast<double>(static_cast<int64_t>(mrd::now_ms_epoch()) - lp) / 1000.0;
    } else {
        j["last_image_age_s"] = nullptr;
    }

    std::error_code ec;
    const auto sp = std::filesystem::space(state.dump_dir, ec);
    if (!ec) {
        j["disk_free_gb"] =
            std::round(static_cast<double>(sp.available) / 1e8) / 10.0;
    }

    return json_response(req, http::status::ok, j);
}

// ---------------------------------------------------------------------------
// GET /debug/sinks — per-pipeline message counters for retention testing.
// Returns sink-level acq/img/wf counters (only those incremented on
// successful HDF5 writes) plus dump drop totals. Use this — not viz_client
// FPS — to measure dump or live-history retention.
// ---------------------------------------------------------------------------
template <class Body>
static auto handle_get_debug_sinks(const http::request<Body>& req, MarshalState& state)
{
    nlohmann::json j;
    j["mode"] = state.dump_enabled ? "dump" : "live";

    if (state.dump_enabled && state.dump_recorder) {
        auto c = state.dump_recorder->counters();
        auto lane_json = [](const mrd::DumpRecorder::LaneSnapshot& l) {
            nlohmann::json o;
            o["spool_records"] = l.spool_records;
            o["spool_bytes"]   = l.spool_bytes;
            o["spool_open"]    = l.spool_open;
            o["write_error"]   = l.write_error;
            o["converted_acq"] = l.converted_acq;
            o["converted_img"] = l.converted_img;
            o["converted_wf"]  = l.converted_wf;
            o["conversion_ok"] = l.conversion_ok;
            return o;
        };
        j["dump"]["from_scanner"]        = lane_json(c.scanner);
        j["dump"]["from_reconstruction"] = lane_json(c.recon);
        j["dump"]["dropped_records"] = c.dropped_records;
        j["dump"]["dropped_bytes"]   = c.dropped_bytes;
        j["dump"]["had_overflow"]    = c.had_overflow;
        j["dump"]["conversion_status"] = mrd::DumpRecorder::status_name(c.status);
    }
    if (!state.dump_enabled) {
        auto live_lane_json = [](const mrd::LiveImageRecorder::CounterSnapshot& c,
                                 uint64_t dropped) {
            nlohmann::json o;
            o["acq"] = c.acq;
            o["img"] = c.img;
            o["wf"]  = c.wf;
            o["open"] = c.sink_open;
            // Post-fix: dropped should stay 0 under the lossless
            // contract. Retained for visibility.
            o["dropped"] = dropped;
            // Live queue depth and high-watermark surface: new with
            // the lossless-live change (no more drop-oldest). If the
            // watermark fires, the HDF5 worker is behind; memory
            // grows until it catches up.
            o["queued_jobs"] = c.queued_jobs;
            o["high_watermark_hit"] = c.high_watermark_hit;
            return o;
        };
        if (state.scanner_live.recorder) {
            j["live"]["from_scanner"] = live_lane_json(
                state.scanner_live.recorder->counters(),
                state.scanner_live.recorder->dropped_count());
        }
        if (state.recon_live.recorder) {
            j["live"]["from_reconstruction"] = live_lane_json(
                state.recon_live.recorder->counters(),
                state.recon_live.recorder->dropped_count());
        }
    }
    return json_response(req, http::status::ok, j);
}

// ---------------------------------------------------------------------------
// GET /debug/perf — FPS-regression instrumentation. Free-running totals
// since process start. Deltas between two polls give rates.
// ---------------------------------------------------------------------------
template <class Body>
static auto handle_get_debug_perf(const http::request<Body>& req, MarshalState& state)
{
    nlohmann::json j;

    auto now = std::chrono::steady_clock::now();
    j["uptime_s"] = std::chrono::duration_cast<std::chrono::seconds>(now - state.start).count();

    j["recv"]["scanner_images"]    = state.perf_scanner_images_received.load();
    j["recv"]["recon_images"]      = state.perf_recon_images_received.load();
    j["recv"]["scanner_waveforms"] = state.perf_scanner_waveforms_received.load();
    j["publish_attempts"]["scanner"] = state.perf_publish_attempts_scanner.load();
    j["publish_attempts"]["recon"]   = state.perf_publish_attempts_recon.load();
    j["publish_lost"]                = state.perf_publish_lost.load();

    if (state.latest_writer) {
        auto p = state.latest_writer->perf();
        j["latest_writer"]["enqueued"]          = p.enqueued;
        j["latest_writer"]["coalesced"]         = p.coalesced;
        j["latest_writer"]["dropped_oldest"]    = p.dropped_oldest;
        j["latest_writer"]["completed"]         = p.completed;
        j["latest_writer"]["failed"]            = p.failed;
        j["latest_writer"]["retried"]           = p.retried;
        j["latest_writer"]["lost"]              = p.lost;
        j["latest_writer"]["max_queue_depth"]   = p.max_queue_depth;
        j["latest_writer"]["last_write_us"]     = p.last_write_us;
        j["latest_writer"]["max_write_us"]      = p.max_write_us;
        j["latest_writer"]["last_drain_lag_us"] = p.last_drain_lag_us;
        j["latest_writer"]["max_drain_lag_us"]  = p.max_drain_lag_us;
    }

    return json_response(req, http::status::ok, j);
}

// ---------------------------------------------------------------------------
// Main dispatcher
// ---------------------------------------------------------------------------
template <class Body, class Send>
void handle_http_request(http::request<Body>&& req, MarshalState& state, Send&& send)
{
    auto target = req.target();
    auto method = req.method();

    // Body size check
    if (req.body().size() > state.max_body_bytes) {
        send(json_response(req, http::status::payload_too_large,
                           {{"error", "body too large"}}));
        return;
    }

    // Query/control endpoints (scanner data arrives via MRD TCP, not HTTP)
    if (method == http::verb::get && target == "/image/latest") {
        send(handle_get_image_latest(req, state));
        return;
    }
    if (method == http::verb::get && target == "/image/latest.h5") {
        send(handle_get_image_latest_h5(req, state));
        return;
    }
    if (method == http::verb::get && target == "/transform") {
        send(handle_get_transform(req, state));
        return;
    }
    if (method == http::verb::put && target == "/transform") {
        send(handle_put_transform(req, state));
        return;
    }
    if (method == http::verb::post &&
        (target == "/write/file_slice_translation" || target == "/write/file_slice_translation.json")) {
        send(handle_post_slice_translation(req, state));
        return;
    }
    if (method == http::verb::get &&
        (target == "/read/file_slice_translation" || target == "/read/file_slice_translation.json")) {
        send(handle_get_slice_translation(req, state));
        return;
    }
    if (method == http::verb::get && target == "/read/slice_geometry") {
        send(handle_get_slice_geometry(req, state));
        return;
    }
    if (method == http::verb::post && target == "/write/slice_target") {
        send(handle_post_slice_target(req, state));
        return;
    }
    if (method == http::verb::get && target == "/read/slice_target") {
        send(handle_get_slice_target(req, state));
        return;
    }
    if (method == http::verb::post && target == "/write/slice_delta") {
        send(handle_post_slice_delta(req, state));
        return;
    }
    if (method == http::verb::get && target == "/read/slice_delta") {
        send(handle_get_slice_delta(req, state));
        return;
    }
    if (method == http::verb::post && target == "/pose") {
        send(handle_post_pose(req, state));
        return;
    }
    if (method == http::verb::get && target == "/pose") {
        send(handle_get_pose(req, state));
        return;
    }
    if (method == http::verb::get && target == "/dump/scanner") {
        send(handle_get_dump_scanner(req, state));
        return;
    }
    if (method == http::verb::get && target == "/dump/recon") {
        send(handle_get_dump_recon(req, state));
        return;
    }
    if (method == http::verb::get && target == "/health") {
        send(handle_get_health(req, state));
        return;
    }
    if (method == http::verb::get && target == "/status") {
        send(handle_get_status(req, state));
        return;
    }
    if (method == http::verb::get && target == "/debug/sinks") {
        send(handle_get_debug_sinks(req, state));
        return;
    }
    if (method == http::verb::get && target == "/debug/perf") {
        send(handle_get_debug_perf(req, state));
        return;
    }

    // 404
    send(json_response(req, http::status::not_found,
                       {{"error", "not found"},
                        {"path", std::string(target)}}));
}
