
# WebSocket Realtime Ingest Protocol

Two entry modes:
1. Control message: send text "INGEST series=NAME"
2. Path: connect to /ws/realtime/ingest?series=NAME

Binary header (little-endian):
  "MRD1"|ver(2)|flags(2)|series_len(2)|reserved(2)|frame_idx(8)|ts_ns(8)|payload_len(8)|series[...]|payload[...]

Ack: server responds {"ack":<frame_idx>} per frame.
