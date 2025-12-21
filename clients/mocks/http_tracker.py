import requests
import time
import sys

MRI_HTTP_BASE = "http://127.0.0.1:8080"

def main():
    print(f"[*] Starting HTTP-only Tracker polling {MRI_HTTP_BASE}")
    last_ts = None
    
    try:
        while True:
            # 1. Poll for latest MRI/Frame metadata
            try:
                resp = requests.get(f"{MRI_HTTP_BASE}/v1/mrd/latest", timeout=0.5)
                if resp.status_code == 200:
                    data = resp.json()
                    current_ts = data.get("ts")
                    if current_ts != last_ts:
                        print(f"[HTTP-MRD] New Data: {data.get('path')} | Frame: {data.get('frame_index')}")
                        last_ts = current_ts
            except Exception:
                pass

            # 2. Poll for latest Pose
            try:
                p_resp = requests.get(f"{MRI_HTTP_BASE}/v1/pose/current", timeout=0.5)
                if p_resp.status_code == 200:
                    p_data = p_resp.json()
                    pos = p_data.get("pose", {}).get("p")
                    # (Optional: print pose if changed)
            except Exception:
                pass

            time.sleep(0.2) # 5Hz Polling
    except KeyboardInterrupt:
        print("\nExiting HTTP Tracker.")

if __name__ == "__main__":
    main()

