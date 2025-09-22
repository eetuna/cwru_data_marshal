# CWRU Data Marshal (Generic Server)

**What it is.** A lightweight http-based (httplib.h) server with multiple clients that uses JSON format (json.hpp) for file read/writes. The server-client system is containerized using Docker. The clients wait 4 seconds on startup to allow the server to be up.
File access is synchronized using mutexes (unique lock for writes, shared lock for reads). A JSON file (files.json) specifies the files used; this is used to create file mutexes.
File routing (i.e. which files each clients read/writes to) is specified in the file file_routes.json.

## Build and Run with Docker Compose:
docker-compose up --build --force-recreate --remove-orphans

