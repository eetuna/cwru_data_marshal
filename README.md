# cwru_data_marshal
# Data Marshal (Boost.Beast)


## Quickstart
```bash
docker compose build --build-arg BUILDKIT_INLINE_CACHE=1
docker compose up -d
# health
curl -s http://localhost:8080/health
# current pose
curl -s http://localhost:8080/v1/pose/current | jq
# latest MRD
curl -s http://localhost:8080/v1/mrd/latest | jq
# entries since timestamp
curl -s "http://localhost:8080/v1/mrd/since?ts=2025-09-10T12:30:00Z&limit=5" | jq