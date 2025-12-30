# CWRU Data Marshal (Generic Server)

**What it is.** A lightweight http-based (httplib.h) server with multiple clients that uses JSON format (json.hpp) for file read/writes. The server-client system is containerized using Docker. The clients wait 8 seconds on startup to allow the server to be up.
File access is synchronized using mutexes (unique lock for writes, shared lock for reads). A JSON file (files.json) specifies the files used; this is used to create file mutexes.
File routing (i.e. which files each clients read/writes to) is specified in the file file_routes.json.
For archival purposes, data written to files is saved in the files directory.

## Build and Run with Docker Compose:
<br>Copy the data files into the files directory (every time - delete/replace exisiting files in the files director)
<br>In the cwru_data_marshal run
<br>docker-compose up --build --force-recreate --remove-orphans

### Notes about System Comfiguration
The docker container can be run on both Windows and Ubuntu host systems. Make sure the line endings are appropriate for the host system you are using.
#### File Path Changes
Because container directories are mounted to the host system (for logging/archival purposes), the following file paths should be modified for your system:
<br>file: docker-compose.yml
<br>lines: 9, 10
<br>change: D:\rza3\cwru_data_marshal to the filepath for cwru_data_marshal (this repository) on your system.
