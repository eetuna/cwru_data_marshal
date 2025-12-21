import asyncio
import json
import sys

try:
    import websockets
except ImportError:
    print("Error: 'websockets' library not found. Run 'pip install websockets' to use this mock client.")
    sys.exit(1)

async def main():
    uri = "ws://127.0.0.1:8090/ws"
    last_index = -1
    try:
        async with websockets.connect(uri) as websocket:
            # Subscribe to mrd topic (frames)
            await websocket.send(json.dumps({"subscribe": "mrd"}))
            print(f"Mock Planner: Subscribed to 'mrd' at {uri}")
            print("Verifying frame continuity... Press Ctrl+C to stop.")
            
            while True:
                msg = await websocket.recv()
                data = json.loads(msg)
                
                # Filter for mrd frame notifications
                if data.get("type") == "mrd":
                    idx = data.get("frame_index")
                    stream = data.get("stream", "unknown")
                    
                    if idx is not None:
                        if last_index != -1:
                            diff = idx - last_index
                            if diff != 1:
                                print(f"[\033[91mGAP DETECTED\033[0m] Stream '{stream}': Frame index jumped from {last_index} to {idx}!")
                            else:
                                if idx % 10 == 0:
                                    print(f"[OK] Stream '{stream}': Received frame {idx}")
                        else:
                            print(f"[START] Stream '{stream}': Initial frame {idx}")
                        last_index = idx
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
