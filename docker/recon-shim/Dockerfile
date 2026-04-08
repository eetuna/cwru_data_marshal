FROM python:3.11-slim

ENV DEBIAN_FRONTEND=noninteractive \
    PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

# curl for the healthcheck; h5py/ismrmrd wheels bundle their own native libs.
RUN apt-get update && apt-get install -y --no-install-recommends curl \
    && rm -rf /var/lib/apt/lists/*

# Python deps for both python-ismrmrd-server and the shim.
RUN apt-get update && apt-get install -y --no-install-recommends git \
    && rm -rf /var/lib/apt/lists/*

RUN pip install --no-cache-dir \
        ismrmrd \
        numpy \
        h5py \
        pydicom \
        matplotlib \
        requests \
        "git+https://github.com/ismrmrd/ismrmrd-python-tools.git"

# Vendored python-ismrmrd-server
COPY third_party/python-ismrmrd-server /opt/python-ismrmrd-server

# The shim
COPY docker/recon-shim/shim.py /opt/recon-shim/shim.py
COPY docker/recon-shim/entrypoint.sh /opt/recon-shim/entrypoint.sh
RUN chmod +x /opt/recon-shim/entrypoint.sh

EXPOSE 9003

HEALTHCHECK --interval=10s --timeout=3s --retries=3 \
    CMD curl -sf http://127.0.0.1:9003/health || exit 1

ENTRYPOINT ["/opt/recon-shim/entrypoint.sh"]
