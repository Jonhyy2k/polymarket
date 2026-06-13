#!/usr/bin/env python3
"""Phase 0: capture raw V2 market-channel frames to confirm the schema the
C++ parser depends on. Dumps every frame verbatim, then prints a summary of
event_type strings and the fields present on each event kind.
"""
import asyncio, json, sys, time, collections
import websockets

WS_URL = "wss://ws-subscriptions-clob.polymarket.com/ws/market"

# Live June-14 BTC markets (two-sided book on the $64k strike).
ASSETS = [
    "6023564916909538396287403142815490796285739586329855294484240466650852817010",  # $64k YES
    "18485469840862733377815375691536799288741919334754043355477488848197934906536", # $64k NO
    "24263007049311544381149874427246491010362852457986556661717075902445320044187",  # $62k YES
    "41864523372202600045662802580468115995435401217414770171466940334945492803241", # $62k NO
]

DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 35.0

async def main():
    raw_path = "tools/ws_raw_frames.jsonl"
    raw = open(raw_path, "w")
    sub = {"assets_ids": ASSETS, "type": "market",
           "initial_dump": True, "custom_feature_enabled": True}
    event_fields = collections.defaultdict(set)
    event_counts = collections.Counter()
    examples = {}
    nested_fields = collections.defaultdict(set)
    n_frames = 0
    async with websockets.connect(WS_URL, max_size=None, ping_interval=10) as ws:
        await ws.send(json.dumps(sub))
        print(f"[sent subscribe] {json.dumps(sub)[:120]}...")
        t_end = time.time() + DURATION
        while time.time() < t_end:
            try:
                msg = await asyncio.wait_for(ws.recv(), timeout=t_end - time.time())
            except asyncio.TimeoutError:
                break
            n_frames += 1
            raw.write(msg if isinstance(msg, str) else msg.decode())
            raw.write("\n")
            try:
                doc = json.loads(msg)
            except Exception:
                print(f"[non-json frame] {msg[:200]!r}")
                continue
            events = doc if isinstance(doc, list) else [doc]
            for ev in events:
                if not isinstance(ev, dict):
                    continue
                et = ev.get("event_type", "<none>")
                event_counts[et] += 1
                for k, v in ev.items():
                    event_fields[et].add(k)
                    # capture nested object/array shapes
                    if isinstance(v, list) and v and isinstance(v[0], dict):
                        for kk in v[0]:
                            nested_fields[f"{et}.{k}[]"].add(kk)
                if et not in examples:
                    examples[et] = ev
    raw.close()

    print(f"\n===== {n_frames} frames, {sum(event_counts.values())} events in {DURATION}s =====")
    print("\n--- event_type counts ---")
    for et, c in event_counts.most_common():
        print(f"  {et:24s} {c}")
    print("\n--- fields per event_type ---")
    for et in event_counts:
        print(f"  {et}: {sorted(event_fields[et])}")
    print("\n--- nested array element fields ---")
    for k, fs in sorted(nested_fields.items()):
        print(f"  {k}: {sorted(fs)}")
    print("\n--- one example of each event_type ---")
    for et, ex in examples.items():
        s = json.dumps(ex)
        print(f"  [{et}] {s[:600]}")
    print(f"\nRaw frames -> {raw_path}")

if __name__ == "__main__":
    asyncio.run(main())
