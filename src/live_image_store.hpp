/*
 * File: src/live_image_store.hpp
 * Project: CWRU Data Marshal - MRI Marshal
 * Purpose: Small helpers shared by the scanner and recon image paths.
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

    // Root of the viewer snapshot (latest_image.h5). By default next to the
    // archive under dump_dir; --latest-dir points it at a separate root
    // (typically a RAM-backed tmpfs) so snapshot writes never touch the
    // archive disk.
    inline std::filesystem::path latest_base_dir(const MarshalState &state)
    {
        return state.latest_dir.empty() ? state.dump_dir : state.latest_dir;
    }

    // Record the slice geometry (position + orientation) carried by an IMAGE
    // header so the slice-translation command pushed to the scanner can name
    // the current slice location. Called from both image paths (scanner
    // images in mrd_tcp_listener.hpp, recon images in handle_recon_image).
    // Cleared at scan start (METADATA_XML handler).
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

} // namespace mrd
