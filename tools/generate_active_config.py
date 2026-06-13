#!/usr/bin/env python3

import argparse
import json
import sys
import urllib.parse
import urllib.request
from collections import defaultdict
from pathlib import Path


GAMMA_BASE = "https://gamma-api.polymarket.com"


def fetch_json(path: str, params: dict[str, object]) -> object:
    query = urllib.parse.urlencode(params, doseq=True)
    url = f"{GAMMA_BASE}{path}?{query}"
    request = urllib.request.Request(
        url,
        headers={
            "Accept": "application/json",
            "User-Agent": "polymarket-arb-config-generator/1.0",
        },
    )
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)


def as_float(value: object) -> float:
    if value in (None, ""):
        return 0.0
    try:
        return float(value)
    except (TypeError, ValueError):
        return 0.0


def parse_token_ids(value: object) -> list[str]:
    if isinstance(value, list):
        return [str(item) for item in value]
    if isinstance(value, str):
        text = value.strip()
        if not text:
            return []
        try:
            parsed = json.loads(text)
        except json.JSONDecodeError:
            return [text]
        if isinstance(parsed, list):
            return [str(item) for item in parsed]
    return []


def event_info(market: dict) -> tuple[str, str]:
    events = market.get("events")
    if isinstance(events, list) and events:
        event = events[0]
        if isinstance(event, dict):
            return str(event.get("slug", "")), str(event.get("title", ""))
    return "", ""


def normalize_market(market: dict) -> dict | None:
    token_ids = parse_token_ids(market.get("clobTokenIds"))
    if len(token_ids) < 2:
        return None

    condition_id = str(market.get("conditionId") or market.get("condition_id") or "").strip()
    if not condition_id:
        return None

    event_slug, event_title = event_info(market)
    name = str(market.get("question") or market.get("title") or market.get("slug") or condition_id)

    return {
        "name": name,
        "condition_id": condition_id,
        "token_id_yes": token_ids[0],
        "token_id_no": token_ids[1],
        "volume24hr": as_float(market.get("volume24hr")),
        "liquidity": as_float(market.get("liquidity")),
        "active": bool(market.get("active", True)),
        "closed": bool(market.get("closed", False)),
        "accepting_orders": bool(market.get("acceptingOrders", True)),
        "enable_order_book": bool(market.get("enableOrderBook", True)),
        "neg_risk": bool(market.get("negRisk", False)),
        "event_slug": event_slug,
        "event_title": event_title,
    }


def market_passes(record: dict, args: argparse.Namespace) -> bool:
    if record is None:
        return False
    if record["closed"] or not record["active"]:
        return False
    if not record["accepting_orders"] or not record["enable_order_book"]:
        return False
    if record["volume24hr"] < args.min_volume24hr:
        return False
    if record["liquidity"] < args.min_liquidity:
        return False
    return True


def fetch_top_markets(args: argparse.Namespace) -> list[dict]:
    markets: list[dict] = []
    for page in range(args.pages):
        payload = fetch_json(
            "/markets",
            {
                "closed": "false",
                "order": "volume24hr",
                "ascending": "false",
                "limit": args.page_size,
                "offset": page * args.page_size,
            },
        )
        if not isinstance(payload, list) or not payload:
            break
        markets.extend(item for item in payload if isinstance(item, dict))
    return markets


def fetch_event_markets(event_slug: str) -> list[dict]:
    payload = fetch_json("/events", {"slug": event_slug})
    if not isinstance(payload, list) or not payload:
        return []
    event = payload[0]
    if not isinstance(event, dict):
        return []
    markets = event.get("markets")
    if not isinstance(markets, list):
        return []
    return [item for item in markets if isinstance(item, dict)]


def select_markets(args: argparse.Namespace) -> tuple[list[dict], list[dict], list[dict]]:
    top_markets = [normalize_market(m) for m in fetch_top_markets(args)]
    candidates = [m for m in top_markets if market_passes(m, args)]
    selected: list[dict] = []
    selected_condition_ids: set[str] = set()
    selected_groups: list[dict] = []

    event_scores: dict[str, float] = defaultdict(float)
    for market in candidates:
        if market["neg_risk"] and market["event_slug"]:
            event_scores[market["event_slug"]] += market["volume24hr"]

    if args.group_mode != "none":
        for event_slug, _score in sorted(event_scores.items(), key=lambda item: item[1], reverse=True):
            if len(selected_groups) >= args.max_groups or len(selected) >= args.max_markets:
                break
            event_markets = [normalize_market(m) for m in fetch_event_markets(event_slug)]
            filtered = [m for m in event_markets if market_passes(m, args)]
            if len(filtered) < 2 or len(filtered) > args.max_group_size:
                continue
            if len(filtered) != len(event_markets):
                continue

            group_title = filtered[0]["event_title"] or event_slug
            added_group: list[dict] = []
            for market in filtered:
                if market["condition_id"] in selected_condition_ids:
                    continue
                selected_condition_ids.add(market["condition_id"])
                selected.append(market)
                added_group.append(market)
            if added_group:
                selected_groups.append(
                    {
                        "key": event_slug,
                        "display_name": group_title,
                        "markets": added_group,
                    }
                )

            if args.group_mode == "neg-risk-only" and len(selected) >= args.max_markets:
                break

    if args.group_mode != "neg-risk-only":
        for market in candidates:
            if len(selected) >= args.max_markets:
                break
            if market["condition_id"] in selected_condition_ids:
                continue
            selected_condition_ids.add(market["condition_id"])
            selected.append(market)

    return selected[: args.max_markets], selected_groups, candidates


def make_config(
    args: argparse.Namespace,
    output_path: Path,
    contracts: list[dict],
    selected_groups: list[dict],
) -> dict:
    stem = output_path.stem
    groups = [
        {
            "key": group["key"],
            "display_name": group["display_name"],
            "exhaustive": True,
            "condition_ids": [market["condition_id"] for market in group["markets"]],
        }
        for group in selected_groups
    ]
    enable_group_arbitrage = args.enable_group_arbitrage and bool(groups)
    return {
        "min_edge_threshold_bps": 0,
        "taker_fee_bps": 100,
        "summary_interval_seconds": args.summary_interval_seconds,
        "warmup_seconds": args.warmup_seconds,
        "log_file": f"logs/arb-{stem}.csv",
        "near_miss_log_file": f"logs/near-miss-{stem}.csv",
        "replay_log_file": f"logs/replay-{stem}.csv",
        "ping_interval_seconds": 15,
        "stale_feed_timeout_seconds": 30,
        "reconnect_max_delay_seconds": 30,
        "message_queue_capacity": args.message_queue_capacity,
        "metrics_queue_capacity": args.metrics_queue_capacity,
        "opportunity_queue_capacity": args.opportunity_queue_capacity,
        "replay_queue_capacity": args.replay_queue_capacity,
        "active_market_report_limit": args.active_market_report_limit,
        "custom_feature_enabled": True,
        "initial_dump": True,
        "metrics_enabled": True,
        "edge_telemetry_enabled": args.edge_telemetry_enabled,
        "replay_logging_enabled": args.replay_logging_enabled,
        "hot_path_logging": False,
        "flush_csv_each_write": False,
        "fetch_market_metadata": args.fetch_market_metadata,
        "enable_group_arbitrage": enable_group_arbitrage,
        "auto_detect_exhaustive_groups": bool(args.fetch_market_metadata and enable_group_arbitrage),
        "maker_arb_enabled": args.maker_arb_enabled,
        "pin_thread_cpu": -1,
        "receiver_cpu": 2,
        "parser_cpu": 4,
        "logger_cpu": 6,
        "lock_memory": False,
        "prefault_stack_kb": 64,
        "realtime_priority": 0,
        "receiver_priority": 0,
        "parser_priority": 0,
        "logger_priority": 0,
        "groups": groups,
        "contracts": [
            {
                "name": contract["name"],
                "condition_id": contract["condition_id"],
                "token_id_yes": contract["token_id_yes"],
                "token_id_no": contract["token_id_no"],
            }
            for contract in contracts
        ],
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate an active Polymarket benchmark config")
    parser.add_argument("--output", required=True, help="Output config path")
    parser.add_argument("--max-markets", type=int, default=100)
    parser.add_argument("--pages", type=int, default=5)
    parser.add_argument("--page-size", type=int, default=100)
    parser.add_argument("--min-volume24hr", type=float, default=25000.0)
    parser.add_argument("--min-liquidity", type=float, default=2500.0)
    parser.add_argument("--max-groups", type=int, default=8)
    parser.add_argument("--max-group-size", type=int, default=6)
    parser.add_argument(
        "--group-mode",
        choices=("mixed", "neg-risk-only", "none"),
        default="mixed",
    )
    parser.add_argument("--summary-interval-seconds", type=int, default=60)
    parser.add_argument("--warmup-seconds", type=int, default=20)
    parser.add_argument("--message-queue-capacity", type=int, default=256)
    parser.add_argument("--metrics-queue-capacity", type=int, default=32768)
    parser.add_argument("--opportunity-queue-capacity", type=int, default=4096)
    parser.add_argument("--replay-queue-capacity", type=int, default=8192)
    parser.add_argument("--active-market-report-limit", type=int, default=10)
    parser.add_argument("--enable-group-arbitrage", action="store_true", default=True)
    parser.add_argument("--disable-group-arbitrage", action="store_false", dest="enable_group_arbitrage")
    parser.add_argument("--fetch-market-metadata", action="store_true", default=False)
    parser.add_argument("--maker-arb-enabled", action="store_true", default=False)
    parser.add_argument("--edge-telemetry-enabled", action="store_true", default=False)
    parser.add_argument("--replay-logging-enabled", action="store_true", default=False)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    selected, selected_groups, candidates = select_markets(args)
    if not selected:
        print("No active markets matched the requested filters", file=sys.stderr)
        return 1

    config = make_config(args, output_path, selected, selected_groups)
    with output_path.open("w", encoding="utf-8") as fh:
        json.dump(config, fh, indent=4)
        fh.write("\n")

    print(
        f"[config] wrote {output_path} with {len(selected)} markets "
        f"from {len(candidates)} filtered candidates",
        file=sys.stderr,
    )
    for group in selected_groups:
        print(
            f"[config] group {group['display_name']} | legs={len(group['markets'])}",
            file=sys.stderr,
        )
    for market in selected[: min(10, len(selected))]:
        print(
            f"[config] market {market['name']} | vol24h={market['volume24hr']:.0f} "
            f"| liq={market['liquidity']:.0f}",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
