import asyncio
import json
import time
import sys
import requests
from datetime import datetime

try:
    import websockets
except ImportError:
    print("Error: 'websockets' library not found. Run 'pip install websockets'")
    sys.exit(1)

import os

# Configuration (Overridable via environment variables)
MRI_WS_URI = os.getenv("MRI_WS_URI", "ws://127.0.0.1:8090/ws")
MRI_HTTP_BASE = os.getenv("MRI_HTTP_BASE", "http://127.0.0.1:8080")
ROBOT_HTTP_BASE = os.getenv("ROBOT_HTTP_BASE", "http://127.0.0.1:8081")

class Coordinator:
    def __init__(self):
        self.last_robot_state = None
        self.last_tool_id = None
        self.last_mri_ts = None
        self.polling_hz = 20
        self.is_halted = False # Prevents duplicate halt commands

    async def mri_http_safety_poller(self):
        """High-frequency HTTP polling for MRI safety events."""
        print(f"[*] Starting MRI HTTP Safety Poller ({self.polling_hz}Hz) at {MRI_HTTP_BASE}")
        while True:
            try:
                # Poll for latest metadata
                resp = requests.get(f"{MRI_HTTP_BASE}/v1/mrd/latest", timeout=0.1)
                if resp.status_code == 200:
                    data = resp.json()
                    
                    # Detect Fault via HTTP
                    if "error" in data and not self.is_halted:
                        print(f"\033[91m[CRITICAL]\033[0m HTTP Poll detected MRI Error: {data['error']}. HALTING.")
                        self.send_robot_halt(data['error'])
                        self.is_halted = True
                    
                    # Detect Reset (if error cleared)
                    if "error" not in data and self.is_halted:
                        print("\033[92m[INFO]\033[0m MRI Error cleared. System ready for reset.")
                        self.is_halted = False
            except Exception:
                pass
            
            await asyncio.sleep(1.0 / self.polling_hz)

    async def robot_poller(self):
        """Polls the Robot Marshal for state changes."""
        print(f"[*] Starting Robot Marshal poller at {ROBOT_HTTP_BASE}")
        while True:
            try:
                # Poll the 'robot_status' file
                resp = requests.get(f"{ROBOT_HTTP_BASE}/read/robot_status?last=1", timeout=1)
                if resp.status_code == 200:
                    data = resp.json()
                    
                    # Requirement 2: Scan Sync
                    vals = data.get("values", [{}])
                    if isinstance(vals, list) and len(vals) > 0:
                        status_obj = vals[0]
                        current_pos = status_obj.get("position")
                        
                        if current_pos == "isocenter" and self.last_robot_state != "isocenter":
                            print("\033[92m[ACTION]\033[0m Robot reached isocenter. Triggering MRI Scan Start...")
                            # Triggering MRI scan via MRI Marshal HTTP
                            trigger_payload = {"command": "START_SCAN", "ts": datetime.now().isoformat()}
                            requests.post(f"{MRI_HTTP_BASE}/v1/mrd/ingest", json=trigger_payload, timeout=0.5)
                        
                        self.last_robot_state = current_pos

                        # Requirement 3: Data Tagging
                        current_tool = status_obj.get("tool_id")
                        if current_tool and current_tool != self.last_tool_id:
                            print(f"\033[94m[ACTION]\033[0m Tool changed to {current_tool}. Notifying MRI Marshal...")
                            tag_payload = {"type": "metadata", "tool_id": current_tool, "ts": datetime.now().isoformat()}
                            # Post metadata change to MRI Marshal
                            requests.post(f"{MRI_HTTP_BASE}/v1/mrd/ingest", json=tag_payload, timeout=0.5)
                            self.last_tool_id = current_tool
                
            except Exception:
                # Silently wait if robot marshal is offline or file not yet created
                pass
            
            await asyncio.sleep(0.5) # 2Hz polling

    async def mri_listener(self):
        """Listens to MRI Marshal for safety and status events."""
        print(f"[*] Connecting to MRI Marshal WebSocket at {MRI_WS_URI}")
        while True:
            try:
                async with websockets.connect(MRI_WS_URI) as ws:
                    # Subscribe to mrd for frame updates
                    await ws.send(json.dumps({"subscribe": "mrd"}))
                    print("[+] Subscribed to MRI 'mrd' topic.")

                    while True:
                        msg = await ws.recv()
                        data = json.loads(msg)

                        # Requirement 1: Safety Stop
                        # Triggered if MRI Marshal reports an error field
                        if "error" in data and not self.is_halted:
                            err_msg = data.get("error", "Unknown MRI error")
                            print(f"\033[91m[CRITICAL]\033[0m WS Listener detected MRI Error: {err_msg}. HALTING.")
                            self.send_robot_halt(err_msg)
                            self.is_halted = True

            except (websockets.ConnectionClosed, ConnectionRefusedError):
                # Silently retry to handle marshal restarts
                await asyncio.sleep(2)
            except Exception as e:
                print(f"[!] MRI Listener Error: {e}")
                await asyncio.sleep(2)

    def send_robot_halt(self, reason):
        """Sends a high-priority halt command to the Robot Marshal."""
        try:
            payload = {
                "sent_at": int(time.time() * 1e9),
                "client_id": "coordinator_bridge",
                "values": [{"command": "HALT", "reason": reason}]
            }
            # Post to Robot Marshal native RPC endpoint
            requests.post(f"{ROBOT_HTTP_BASE}/write/robot_commands", json=payload, timeout=0.5)
            print("[✓] HALT command successfully posted to Robot Marshal.")
        except Exception as e:
            print(f"[×] Failed to send HALT to Robot: {e}")

    async def run(self):
        await asyncio.gather(
            self.mri_listener(),
            self.mri_http_safety_poller(),
            self.robot_poller()
        )

if __name__ == "__main__":
    print("============================================")
    print("   CWRU Data Marshal Coordinator Bridge     ")
    print("============================================")
    coordinator = Coordinator()
    try:
        asyncio.run(coordinator.run())
    except KeyboardInterrupt:
        print("\n[*] Shutting down bridge.")
