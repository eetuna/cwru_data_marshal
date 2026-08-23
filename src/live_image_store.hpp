/*
 * File: src/live_image_store.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Shared helpers for live per-scan image history and closed snapshots.
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <ismrmrd/ismrmrd.h>

#include "logging.hpp"
#include "marshal_state.hpp"
#include "mrd_io.hpp"
#include "mrd_stream_tags.hpp"

namespace mrd
{

    enum class LiveLane
    {
        Scanner,
        Recon,
    };

    inline LiveImageLaneState &lane_state(MarshalState &state, LiveLane lane)
    {
        return lane == LiveLane::Scanner ? state.scanner_live : state.recon_live;
    }

    inline const char *lane_name(LiveLane lane)
    {
        return lane == LiveLane::Scanner ? "scanner" : "reconstruction";
    }

    inline std::filesystem::path lane_live_dir(const std::filesystem::path &dump_dir, LiveLane lane)
    {
        return lane == LiveLane::Scanner ? live_scanner_dir(dump_dir) : live_recon_dir(dump_dir);
    }

    // Root under which the transient latest-snapshot artifacts live
    // (latest_image.h5 per lane + latest_error.png). By default they sit
    // next to the archives under dump_dir; --latest-dir points them at a
    // separate root (typically a RAM-backed tmpfs) so per-volume snapshot
    // I/O never touches — and can never be stalled by — the archive disk.
    // The snapshot is disposable by design (rewritten per volume, deleted
    // at scan start), so a volatile filesystem loses nothing durable.
    inline std::filesystem::path latest_base_dir(const MarshalState &state)
    {
        return state.latest_dir.empty() ? state.dump_dir : state.latest_dir;
    }

    inline std::filesystem::path lane_latest_path(const MarshalState &state, LiveLane lane)
    {
        return lane_live_dir(latest_base_dir(state), lane) / "latest_image.h5";
    }

    struct ParsedWireImage
    {
        ISMRMRD::ImageHeader header{};
        const char *attr{nullptr};
        size_t attr_len{0};
        const uint8_t *pixels{nullptr};
        size_t pixel_bytes{0};
    };

    inline bool parse_wire_image(const uint8_t *data, size_t size, ParsedWireImage &out)
    {
        if (size < IMAGE_HEADER_BYTES + sizeof(uint64_t))
            return false;
        std::memcpy(&out.header, data, sizeof(ISMRMRD::ImageHeader));
        uint64_t attr_len = 0;
        std::memcpy(&attr_len, data + IMAGE_HEADER_BYTES, sizeof(uint64_t));
        const size_t attr_off = IMAGE_HEADER_BYTES + sizeof(uint64_t);
        // MEDIUM #13: `attr_off + attr_len` could wrap on malicious input. Use
        // the overflow-safe form: size must be >= attr_off, and attr_len must
        // not exceed the remaining bytes.
        if (size < attr_off)
            return false;
        if (attr_len > size - attr_off)
            return false;
        const size_t attr_len_s = static_cast<size_t>(attr_len);
        out.attr = reinterpret_cast<const char *>(data + attr_off);
        out.attr_len = attr_len_s;
        out.pixels = data + attr_off + attr_len_s;
        out.pixel_bytes = size - attr_off - attr_len_s;
        return true;
    }

    // Record the slice geometry (position + orientation) carried by an IMAGE
    // header so the slice-translation command pushed to the scanner can name
    // the current slice location. Called from BOTH image paths (scanner lane
    // in mrd_tcp_listener.hpp, recon lane in handle_recon_image) before any
    // live/dump gating — geometry is tracked in every mode. Cleared at scan
    // start (METADATA_XML handler).
    inline void update_slice_geometry(MarshalState &state, const uint8_t *data, size_t size)
    {
        if (size < IMAGE_HEADER_BYTES)
            return;
        ISMRMRD::ImageHeader hdr{};
        std::memcpy(&hdr, data, sizeof(ISMRMRD::ImageHeader));

        MarshalState::SliceGeometry g;
        g.slice = hdr.slice;
        for (int i = 0; i < 3; ++i)
        {
            g.position[i] = hdr.position[i];
            g.read_dir[i] = hdr.read_dir[i];
            g.phase_dir[i] = hdr.phase_dir[i];
            g.slice_dir[i] = hdr.slice_dir[i];
        }
        g.ts = iso8601_now_ms();

        std::lock_guard<std::mutex> lk(state.slice_geom_mtx);
        state.slice_geom[hdr.slice] = std::move(g);
        state.latest_slice = hdr.slice;
    }

    inline void publish_latest_snapshot(MarshalState &state,
                                        LiveLane lane,
                                        std::string xml,
                                        std::vector<std::vector<uint8_t>> images)
    {
        if (images.empty())
            return;

        if (lane == LiveLane::Scanner)
        {
            state.perf_publish_attempts_scanner.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            state.perf_publish_attempts_recon.fetch_add(1, std::memory_order_relaxed);
        }

        auto dest = lane_latest_path(state, lane);
        // Stale-publish guard: capture the scan epoch at enqueue time. If a
        // new scan's METADATA arrives while this publish is still in the
        // writer queue, reset_live_outputs_for_new_scan has already deleted
        // the snapshot the publish would advertise — discard it instead of
        // resurrecting a dead scan's path.
        const auto epoch = state.scan_epoch.load();
        auto on_complete = [&state, epoch](const std::filesystem::path &path)
        {
            if (state.scan_epoch.load() != epoch)
                return;
            state.last_publish_ms.store(static_cast<int64_t>(now_ms_epoch()));
            std::lock_guard<std::mutex> img_lk(state.latest_image_mtx);
            state.latest_image_path = path.string();
            state.latest_image_error = false;
            // Path and generation move together under latest_image_mtx so
            // /image/latest readers never see a new generation with a stale
            // path (or vice versa).
            state.latest_image_generation.fetch_add(1);
        };

        if (state.latest_writer)
        {
            state.latest_writer->enqueue(dest, std::move(xml), std::move(images), std::move(on_complete));
            return;
        }

        try
        {
            write_latest_image_h5_file(dest, xml, images);
            on_complete(dest);
        }
        catch (const std::exception &e)
        {
            LOG_WARN("Latest H5 write failed for " << lane_name(lane) << ": " << e.what());
        }
    }

    // Publish a pending, non-empty recon group and reset it. Caller holds
    // scan_mtx. Used when a recon stream ends (normal EOF or abnormal-EOF
    // finalize): a group still buffering at that point will never complete,
    // so surface the partial result on /image/latest instead of silently
    // dropping it. publish_latest_snapshot only enqueues to the async writer
    // (no lock conflict), and its scan_epoch guard discards the publish if a
    // new scan has already taken over.
    inline void flush_pending_recon_group_locked(MarshalState &state)
    {
        auto &group = state.recon_latest_group;
        if (group.active && !group.published && !group.images.empty())
        {
            publish_latest_snapshot(state, LiveLane::Recon,
                                    state.current_xml_header,
                                    std::move(group.images));
        }
        group.reset();
    }

    inline bool recon_group_is_complete(const MarshalState &state,
                                        const ReconLatestGroupState &group)
    {
        // HIGH #9: gate on header_received. If a recon image somehow arrives
        // before the XML header has been parsed (reconnect, malformed stream),
        // recon_expected_slices is still its default 0 and the old `<= 1` check
        // would publish a single slice as a "complete" multislice result.
        if (!state.header_received.load(std::memory_order_acquire))
        {
            // No header yet: we cannot know the expected slice count. Do not
            // publish prematurely; treat as incomplete so the group buffers.
            return false;
        }
        if (state.recon_expected_slices <= 1)
            return true;
        return group.seen_slices.size() >= state.recon_expected_slices;
    }

    inline void append_live_image(MarshalState &state, LiveLane lane, const uint8_t *data, size_t size)
    {
        ParsedWireImage parsed;
        if (!parse_wire_image(data, size, parsed))
            return;

        if (lane == LiveLane::Scanner)
        {
            state.perf_scanner_images_received.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            state.perf_recon_images_received.fetch_add(1, std::memory_order_relaxed);
        }

        std::string xml;
        std::string filename;
        std::vector<uint8_t> image_bytes(data, data + size);
        std::vector<std::vector<uint8_t>> publish_images;

        {
            std::lock_guard<std::mutex> lk(state.scan_mtx);
            xml = state.current_xml_header;

            if (state.current_scan_filename.empty())
            {
                state.current_scan_filename = scan_filename();
            }
            filename = state.current_scan_filename;

            auto &lane_store = lane_state(state, lane);
            if (!lane_store.recorder)
            {
                lane_store.recorder = std::make_unique<LiveImageRecorder>(
                    lane_live_dir(state.dump_dir, lane));
            }

            lane_store.recorder->append_image(filename, xml, image_bytes);

            if (lane != LiveLane::Recon)
            {
                publish_images.push_back(std::move(image_bytes));
            }
            else if (parsed.header.matrix_size[2] > 1)
            {
                state.recon_latest_group.reset();
                publish_images.push_back(std::move(image_bytes));
            }
            else
            {
                auto &recon_group = state.recon_latest_group;
                if (recon_group.active && recon_group.image_series_index != parsed.header.image_series_index)
                {
                    if (!recon_group.published && !recon_group.images.empty())
                    {
                        publish_images = recon_group.images;
                    }
                    recon_group.reset();
                }
                else if (recon_group.active &&
                         std::find(recon_group.seen_slices.begin(),
                                   recon_group.seen_slices.end(),
                                   parsed.header.slice) != recon_group.seen_slices.end())
                {
                    // Repeat of a slice already in the buffered group: the
                    // recon has started a new pass over the same prescription
                    // without bumping the series index. Flush the buffered
                    // group so it reaches /image/latest instead of buffering
                    // forever (the "frozen during scan, burst after" failure
                    // when same-orientation slices repeat), and start a new
                    // group with this image. Cannot collide with the
                    // group-complete publish below: with expected slices
                    // <= 1 every image completes instantly and no group ever
                    // buffers, and with expected > 1 the single image pushed
                    // after this reset cannot complete the fresh group.
                    if (!recon_group.published && !recon_group.images.empty())
                    {
                        publish_images = recon_group.images;
                    }
                    recon_group.reset();
                }

                if (!recon_group.active)
                {
                    recon_group.reset();
                    recon_group.active = true;
                    recon_group.image_series_index = parsed.header.image_series_index;
                }

                recon_group.images.push_back(std::move(image_bytes));
                if (std::find(recon_group.seen_slices.begin(),
                              recon_group.seen_slices.end(),
                              parsed.header.slice) == recon_group.seen_slices.end())
                {
                    recon_group.seen_slices.push_back(parsed.header.slice);
                }

                if (recon_group_is_complete(state, recon_group))
                {
                    // Volume complete: publish it and START A NEW GROUP. Without
                    // the reset, a recon that keeps image_series_index constant
                    // for the whole scan (e.g. python-ismrmrd-server
                    // invertcontrast) appends every subsequent image to this same
                    // still-"complete" group, so each new image republishes the
                    // ENTIRE accumulated history — latest_image.h5 grows without
                    // bound and per-publish write cost grows linearly with scan
                    // length. Recons that DO bump the series index per volume are
                    // unaffected (the series-change path above still flushes
                    // partial groups).
                    publish_images = recon_group.images;
                    recon_group.reset();
                }
            }
        }

        if (!publish_images.empty())
        {
            publish_latest_snapshot(state, lane, std::move(xml), std::move(publish_images));
        }
    }

    inline void append_live_waveform(MarshalState &state, LiveLane lane,
                                     const uint8_t *data, size_t size)
    {
        if (size < WAVEFORM_HEADER_BYTES)
            return;

        if (lane == LiveLane::Scanner)
        {
            state.perf_scanner_waveforms_received.fetch_add(1, std::memory_order_relaxed);
        }

        std::string xml;
        std::string filename;
        std::vector<uint8_t> wf_bytes(data, data + size);

        {
            std::lock_guard<std::mutex> lk(state.scan_mtx);
            xml = state.current_xml_header;

            if (state.current_scan_filename.empty())
            {
                state.current_scan_filename = scan_filename();
            }
            filename = state.current_scan_filename;

            auto &lane_store = lane_state(state, lane);
            if (!lane_store.recorder)
            {
                lane_store.recorder = std::make_unique<LiveImageRecorder>(
                    lane_live_dir(state.dump_dir, lane));
            }

            lane_store.recorder->append_waveform(filename, xml, std::move(wf_bytes));
        }
    }

    inline void flush_live_lane(MarshalState &state, LiveLane lane)
    {
        std::lock_guard<std::mutex> lk(state.scan_mtx);
        auto &lane_store = lane_state(state, lane);
        if (lane_store.recorder)
        {
            lane_store.recorder->close_scan();
        }
        if (lane == LiveLane::Recon)
        {
            flush_pending_recon_group_locked(state);
        }
    }

    inline void flush_all_live_lanes(MarshalState &state)
    {
        flush_live_lane(state, LiveLane::Scanner);
        flush_live_lane(state, LiveLane::Recon);
    }

    // Epoch-guarded variant for the abnormal-EOF finalizer. With the
    // scanner slot released BEFORE finalization (non-blocking finalize),
    // a new session can start while the old session's thread is still
    // flushing. If a new scan's METADATA has bumped scan_epoch, the new
    // session's reset_live_outputs_for_new_scan already closed/converted
    // the dead scan's spool — flushing again here would convert the NEW
    // scan's partial spool. Skip instead. The epoch check and the flush
    // are atomic under scan_mtx.
    inline void flush_live_lane_at_epoch(MarshalState &state, LiveLane lane,
                                         uint64_t epoch)
    {
        std::lock_guard<std::mutex> lk(state.scan_mtx);
        if (state.scan_epoch.load() != epoch)
            return;
        auto &lane_store = lane_state(state, lane);
        if (lane_store.recorder)
        {
            lane_store.recorder->close_scan();
        }
        if (lane == LiveLane::Recon)
        {
            flush_pending_recon_group_locked(state);
        }
    }

    // Core of the finalized-flag bookkeeping. Caller holds scan_mtx.
    inline void mark_lane_finalized_after_eof_locked(MarshalState &state, LiveLane lane)
    {
        if (lane == LiveLane::Scanner)
        {
            state.scanner_lane_finalized = true;
        }
        else
        {
            state.recon_lane_finalized = true;
            flush_pending_recon_group_locked(state);
        }

        if (state.scanner_lane_finalized && state.recon_lane_finalized)
        {
            state.current_xml_header.clear();
            state.current_config.clear();
            state.current_scan_filename.clear();
            state.recon_expected_slices = 0;
            state.header_received.store(false);
            state.config_received.store(false);
            state.recon_failure_reported.store(false);
            state.scanner_lane_finalized = false;
            state.recon_lane_finalized = false;
        }
    }

    inline void mark_lane_finalized_after_eof(MarshalState &state, LiveLane lane)
    {
        std::lock_guard<std::mutex> lk(state.scan_mtx);
        mark_lane_finalized_after_eof_locked(state, lane);
    }

    // Epoch-guarded companion to flush_live_lane_at_epoch: skip the
    // finalized-flag bookkeeping (and the combined per-scan state reset it
    // can trigger) when a new scan has taken ownership — otherwise the old
    // session's thread would clear the NEW scan's xml/filename mid-scan.
    inline void mark_lane_finalized_after_eof_at_epoch(MarshalState &state,
                                                       LiveLane lane,
                                                       uint64_t epoch)
    {
        std::lock_guard<std::mutex> lk(state.scan_mtx);
        if (state.scan_epoch.load() != epoch)
            return;
        mark_lane_finalized_after_eof_locked(state, lane);
    }

    inline void reset_live_outputs_for_new_scan(MarshalState &state)
    {
        state.latest_image_generation.fetch_add(1);

        {
            std::lock_guard<std::mutex> lk(state.scan_mtx);
            state.scanner_live.close();
            state.recon_live.close();
            state.scanner_lane_finalized = false;
            state.recon_lane_finalized = false;
            state.recon_failure_reported.store(false);
            state.recon_latest_group.reset();
        }

        std::error_code ec;
        std::filesystem::remove(lane_latest_path(state, LiveLane::Scanner), ec);
        ec.clear();
        std::filesystem::remove(lane_latest_path(state, LiveLane::Recon), ec);
        ec.clear();
        std::filesystem::remove(live_recon_dir(latest_base_dir(state)) / "latest_error.png", ec);

        std::lock_guard<std::mutex> img_lk(state.latest_image_mtx);
        state.latest_image_path.clear();
        state.latest_image_error = false;
    }

} // namespace mrd
