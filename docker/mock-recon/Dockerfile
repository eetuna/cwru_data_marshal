FROM python:3.11-slim
RUN pip install --no-cache-dir numpy ismrmrd
COPY mock_recon.py /app/mock_recon.py
WORKDIR /app
EXPOSE 9002
ENTRYPOINT ["python3", "mock_recon.py"]
