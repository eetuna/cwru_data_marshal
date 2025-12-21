import asyncio
import json
import sys
from datetime import datetime

try:
    import websockets
except ImportError:
    print("Error: 'websockets' library not found. Run 'pip install websockets' to use this mock client.")
    sys.exit(1)

async def main():
    uri = "ws://127.0.0.1:8090/ws"
    try:
        async with websockets.connect(uri) as websocket:
            # Subscribe to bio and pose
            await websocket.send(json.dumps({"subscribe": "bio"}))
            await websocket.send(json.dumps({"subscribe": "pose"}))
            
            print(f"Mock Surface Tracker: Subscribed to 'bio' and 'pose' at {uri}")
            print("Monitoring latency (ms)... Press Ctrl+C to stop.")
            
            while True:
                msg = await websocket.recv()
                data = json.loads(msg)
                
                m_type = data.get("type")
                if m_type in ["bio", "pose"]:
                    ts_str = data.get("ts")
                    if ts_str:
                        try:
                            # Parse ISO8601 with Z (UTC)
                            ts = datetime.fromisoformat(ts_str.replace('Z', '+00:00'))
                            now = datetime.now(ts.tzinfo)
                            latency = (now - ts).total_seconds() * 1000
                            source = data.get("source", "api")
                            print(f"[{m_type}] latency: {latency:7.2f}ms | source: {source:10} | ts: {ts_str}")
                        except Exception as e:
                            print(f"[{m_type}] Could not parse timestamp {ts_str}: {e}")
                elif "ok" in data and "subscribed" in data:
                    print(f"Subscribed to topic: {data['subscribed']}")
    except ConnectionRefusedError:
        print(f"Error: Could not connect to marshal at {uri}. Is it running?")
    except Exception as e:
        print(f"WS Error: {e}")

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nExiting.")
