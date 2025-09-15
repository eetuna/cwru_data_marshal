# Troubleshooting

- Segfault after /health: health handler must not dereference shared state.
- WS connect refused: ensure ports exposed 8080/8090, route `/ws` matches.
- latest.json missing: verify ingest path writes index & latest atomically.
