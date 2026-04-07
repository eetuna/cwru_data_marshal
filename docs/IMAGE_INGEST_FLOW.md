# Image Ingest & Reconstruction Flow

The scanner POSTs every frame to **`POST /v1/mrd/frame`**. Marshal sniffs the bytes via `detect_mrd_type()` and routes them:

| Detected type | Action |
|---|---|
| `ACQUISITION` (raw k-space) | Async-forward to recon service. Return `202 Accepted` immediately. Recon later POSTs the reconstructed image to `/v1/mrd/callback`. |
| `IMAGE` (already reconstructed) | Store directly via `mrd_sink->append_frame`. |
| `HDF5_FILE` (full `.h5`) | Hand off to `mrd::ingest_payload`. |
| `UNKNOWN` | `400 Bad Request`. |

Both the IMAGE path and the recon callback call the **same** `mrd_sink->append_frame`, so reconstructed and direct images are indistinguishable in the store. The only way clients can tell them apart is if the scanner uses a different `X-MRD-Stream` ID for each input.

Clients consume frames by polling **`GET /v1/mrd/latest`** over HTTP. WebSockets are bound at startup but unused by image clients in the demo / docker compose setup.

## Headers on `POST /v1/mrd/frame`

| Header | Required | Meaning |
|---|---|---|
| `X-MRD-Stream` | yes | Stream key. Stored with the frame and forwarded to recon. |
| `X-MRD-Session` | no | Session id. Stored with the frame and forwarded to recon. |
| `X-MRD-Reply-To` | no | Scanner URL to receive the reconstructed image back. ACQUISITION-only. Stored in an in-memory map keyed by `job_id`, erased on first use. |

When forwarding to recon, marshal also sets `X-MRD-Callback` (hardcoded to `http://mri-marshal:8080/v1/mrd/callback`) and `X-MRD-Job-Id` (`<stream>_<iso8601_now>`).

If marshal was started without `--recon-endpoint`, ACQUISITION frames return `501 Not Implemented`.

## Run folder

Docker compose creates `/session-data/run_<timestamp>/mrd/` and passes it to marshal via `--data` ([docker-compose.demo.yml:13-21](docker-compose.demo.yml#L13-L21)). Inside `mrd/`:

- MRD HDF5 file(s) — written by `append_frame` for both direct images and recon callbacks.
- `bio.jsonl` — bio/ECG samples appended by the writer thread.
- `poses.jsonl` — pose updates appended by the writer thread.

## Diagram

```
                      ┌─────────────┐
                      │   Scanner   │
                      └──────┬──────┘
                             │ POST /v1/mrd/frame
                             ▼
                ┌──────────────────────────┐
                │   detect_mrd_type()      │
                └──┬─────────┬──────────┬──┘
          ACQUISITION    IMAGE     HDF5_FILE
                   │         │          │
                   ▼         │          ▼
        ┌────────────────┐   │  ┌──────────────┐
        │ async → recon  │   │  │ ingest_      │
        │ + 202 to scnr  │   │  │ payload      │
        └───────┬────────┘   │  └──────┬───────┘
                │            │         │
                ▼            │         │
        ┌────────────────┐   │         │
        │ Recon Service  │   │         │
        └───────┬────────┘   │         │
                │ POST       │         │
                │ /v1/mrd/   │         │
                │  callback  │         │
                ▼            ▼         │
        ┌──────────────────────────┐   │
        │ mrd_sink->append_frame   │◄──┘
        └────────────┬─────────────┘
                     ▼
            ┌────────────────┐
            │ MRD HDF5 store │
            │ in run folder  │
            └────────┬───────┘
                     │ GET /v1/mrd/latest
                     ▼
                  Clients

   Side channel: if X-MRD-Reply-To was set, the recon
   callback also async-POSTs the image back to that URL.
```

## Code references

- Route on detected type: [marshal_http.hpp:562-720](.worktrees/mri_data_marshal/src/marshal_http.hpp#L562-L720)
- ACQUISITION → recon (async, 202): [marshal_http.hpp:608-695](.worktrees/mri_data_marshal/src/marshal_http.hpp#L608-L695)
- Recon callback handler: [marshal_http.hpp:367-543](.worktrees/mri_data_marshal/src/marshal_http.hpp#L367-L543)
- Shared `append_frame` call: [marshal_http.hpp:425](.worktrees/mri_data_marshal/src/marshal_http.hpp#L425)
- Reply-to lookup/erase/forward: [marshal_http.hpp:457-475](.worktrees/mri_data_marshal/src/marshal_http.hpp#L457-L475)
- WS bind (optional, unused by image clients): [marshal_main.cpp:297](.worktrees/mri_data_marshal/src/marshal_main.cpp#L297)
