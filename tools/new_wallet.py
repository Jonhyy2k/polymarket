#!/usr/bin/env python3
"""
Generate a FRESH dedicated EOA for live trading.

Writes the private key to a 0600 file (never stdout/scrollback) and prints only
the address. Use a wallet created here ONLY for the bot's trading capital — never
a primary wallet.

Usage:
    python3 tools/new_wallet.py /home/ubuntu/.pm_signer_key
    chmod 600 is applied automatically.

Then load it for the bot (key never transits anything but this file + env):
    export PM_SIGNER_KEY="$(cat /home/ubuntu/.pm_signer_key)"

Fund the printed address on POLYGON with: USDC.e (collateral) + a little POL (gas).
"""
import os
import sys
import stat

from eth_account import Account


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: python3 tools/new_wallet.py <keyfile-path>", file=sys.stderr)
        return 2
    path = sys.argv[1]
    if os.path.exists(path):
        print(f"refusing to overwrite existing file: {path}", file=sys.stderr)
        return 1

    acct = Account.create()
    # Write key with restrictive perms BEFORE writing content.
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write("0x" + acct.key.hex() + "\n")
    os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)  # 0600

    print("Fresh EOA created.")
    print(f"  address : {acct.address}")
    print(f"  keyfile : {path}  (0600, private key only)")
    print()
    print("Next:")
    print(f'  export PM_SIGNER_KEY="$(cat {path})"')
    print(f"  # then fund {acct.address} on POLYGON with USDC.e + a little POL (gas)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
