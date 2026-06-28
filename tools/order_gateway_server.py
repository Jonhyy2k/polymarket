#!/usr/bin/env python3
"""
Order-gateway HTTP service — the "connector" the C++ HFT bot drives.

Wraps py-clob-client-v2 (sigType 3, funder = deposit wallet, EOA-bound L2 creds)
behind a tiny local HTTP API so the C++ strategy can place/cancel orders without
implementing ERC-7739 TypedDataSign itself. Localhost-only, bearer-auth, with hard
caps and cancel-all on shutdown.

Endpoints (all except /health require  Authorization: Bearer <token>):
  GET  /health                         -> {ok, signer, funder, dry, open}
  GET  /open_orders                    -> [ {id, asset_id, side, price, size}, ... ]
  POST /place  {token_id,side,price,size,neg_risk?,tick_size?,post_only?} -> {success,order_id}
  POST /cancel {order_id}               -> {success}
  POST /cancel_all                      -> {success, canceled:[...]}

Run:
  ORDER_GW_TOKEN=<secret> PYTHONPATH=./.pmlibs python3 tools/order_gateway_server.py [--dry] [--port 8765]
Token: from $ORDER_GW_TOKEN, else a random one is generated and printed once.
Key/creds read from files; never logged.
"""
import argparse, json, os, secrets, signal, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".pmlibs"))
from py_clob_client_v2 import (  # noqa: E402
    ClobClient, ApiCreds, OrderArgsV2, OrderType, PartialCreateOrderOptions,
)

HOST = "https://clob.polymarket.com"
CHAIN = 137
DEPOSIT_WALLET = "0x832317706479bb6762741B9b9ba568bb86fFfFF0"
CREDS_FILE = "/home/ubuntu/.pm_creds.env"
KEY_FILE = "/home/ubuntu/.pm_signer_key"

# hard caps (a single order may not exceed these)
MAX_ORDER_NOTIONAL = float(os.getenv("GW_MAX_ORDER_NOTIONAL", "10"))
MAX_ORDER_SIZE = float(os.getenv("GW_MAX_ORDER_SIZE", "200"))
MIN_PRICE, MAX_PRICE = 0.01, 0.99


def read_env(path):
    out = {}
    for ln in open(path):
        ln = ln.strip()
        if ln.startswith("export "):
            ln = ln[7:]
        if "=" in ln and not ln.startswith("#"):
            k, v = ln.split("=", 1)
            out[k.strip()] = v.strip().strip('"').strip("'")
    return out


def log(m):
    print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)


class GW:
    def __init__(self, dry):
        env = read_env(CREDS_FILE)
        self.dry = dry
        self.key = open(KEY_FILE).read().strip()
        self.client = ClobClient(host=HOST, chain_id=CHAIN, key=self.key,
                                 creds=ApiCreds(env["PM_API_KEY"], env["PM_API_SECRET"], env["PM_API_PASSPHRASE"]),
                                 signature_type=3, funder=DEPOSIT_WALLET)
        self.signer = self.client.get_address()
        self._lock = threading.Lock()      # serialize SDK calls (single sender)

    def open_orders(self):
        with self._lock:
            oo = self.client.get_open_orders() or []
        return [{"id": o["id"], "asset_id": o["asset_id"], "side": o["side"],
                 "price": o["price"], "size": o["original_size"],
                 "matched": o.get("size_matched", "0")} for o in oo]

    def place(self, req):
        token = str(req["token_id"]); side = str(req["side"]).upper()
        price = float(req["price"]); size = float(req["size"])
        if side not in ("BUY", "SELL"):
            return 400, {"error": "side must be BUY or SELL"}
        if not (MIN_PRICE <= price <= MAX_PRICE):
            return 400, {"error": f"price out of [{MIN_PRICE},{MAX_PRICE}]"}
        if size <= 0 or size > MAX_ORDER_SIZE:
            return 400, {"error": f"size out of (0,{MAX_ORDER_SIZE}]"}
        if price * size > MAX_ORDER_NOTIONAL:
            return 400, {"error": f"notional {price*size:.2f} > cap {MAX_ORDER_NOTIONAL}"}
        post_only = bool(req.get("post_only", True))
        if self.dry:
            log(f"DRY place {side} {size}@{price} tok…{token[-6:]} post_only={post_only}")
            return 200, {"success": True, "order_id": "DRY", "dry": True}
        with self._lock:
            neg = req.get("neg_risk"); tick = req.get("tick_size")
            if neg is None: neg = self.client.get_neg_risk(token)
            if tick is None: tick = self.client.get_tick_size(token)
            resp = self.client.create_and_post_order(
                OrderArgsV2(token_id=token, price=price, size=size, side=side),
                options=PartialCreateOrderOptions(tick_size=tick, neg_risk=bool(neg)),
                order_type=OrderType.GTC, post_only=post_only)
        ok = isinstance(resp, dict) and resp.get("success")
        oid = resp.get("orderID") if isinstance(resp, dict) else None
        log(f"place {side} {size}@{price} tok…{token[-6:]} -> {'OK '+str(oid) if ok else resp}")
        return (200 if ok else 400), {"success": bool(ok), "order_id": oid, "raw": resp}

    def cancel(self, req):
        from types import SimpleNamespace
        oid = req["order_id"]
        if self.dry:
            return 200, {"success": True, "dry": True}
        with self._lock:
            # SDK cancel_order wants an object exposing .orderID (passing a str crashes).
            r = self.client.cancel_order(SimpleNamespace(orderID=oid))
        return 200, {"success": True, "raw": r}

    def cancel_all(self):
        if self.dry:
            return 200, {"success": True, "dry": True}
        with self._lock:
            r = self.client.cancel_all()
        log(f"cancel_all -> {r}")
        return 200, {"success": True, "canceled": (r or {}).get("canceled", [])}


def make_handler(gw, token):
    class H(BaseHTTPRequestHandler):
        def _send(self, code, obj):
            body = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _auth(self):
            return self.headers.get("Authorization") == f"Bearer {token}"

        def _body(self):
            n = int(self.headers.get("Content-Length", "0") or "0")
            return json.loads(self.rfile.read(n) or b"{}")

        def log_message(self, *a):  # quiet default logging
            pass

        def do_GET(self):
            if self.path == "/health":
                return self._send(200, {"ok": True, "signer": gw.signer, "funder": DEPOSIT_WALLET,
                                        "dry": gw.dry, "open": len(gw.open_orders())})
            if not self._auth():
                return self._send(401, {"error": "unauthorized"})
            if self.path == "/open_orders":
                return self._send(200, gw.open_orders())
            self._send(404, {"error": "not found"})

        def do_POST(self):
            if not self._auth():
                return self._send(401, {"error": "unauthorized"})
            try:
                if self.path == "/place":
                    code, r = gw.place(self._body())
                elif self.path == "/cancel":
                    code, r = gw.cancel(self._body())
                elif self.path == "/cancel_all":
                    code, r = gw.cancel_all()
                else:
                    code, r = 404, {"error": "not found"}
            except Exception as e:
                code, r = 500, {"error": repr(e)}
            self._send(code, r)
    return H


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--dry", action="store_true")
    a = ap.parse_args()

    token = os.getenv("ORDER_GW_TOKEN") or secrets.token_hex(16)
    gw = GW(a.dry)
    server = ThreadingHTTPServer(("127.0.0.1", a.port), make_handler(gw, token))

    def shutdown(*_):
        log("shutting down — cancel_all")
        try: gw.cancel_all()
        except Exception as e: log(f"shutdown cancel_all err {e!r}")
        server.shutdown()
    for s in (signal.SIGINT, signal.SIGTERM):
        signal.signal(s, lambda *_: threading.Thread(target=shutdown).start())

    log(f"order-gateway {'DRY' if a.dry else 'LIVE'} on 127.0.0.1:{a.port} | signer={gw.signer} | funder={DEPOSIT_WALLET}")
    if not os.getenv("ORDER_GW_TOKEN"):
        log(f"AUTH TOKEN (set ORDER_GW_TOKEN to fix it): {token}")
    server.serve_forever()


if __name__ == "__main__":
    main()
