#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Hourly catalog sync worker for pm2.

  python3 catalog_sync_service.py          # loop every CS_CATALOG_SYNC_INTERVAL_SEC
  python3 catalog_sync_service.py --once   # single run (cron / manual)
"""

from __future__ import print_function

import argparse
import logging
import os
import sys
import time

from catalog_sync import run_sync_once

INTERVAL_SEC = int(os.environ.get("CS_CATALOG_SYNC_INTERVAL_SEC", "3600"))


def main():
    parser = argparse.ArgumentParser(description="Cloud catalog sync worker")
    parser.add_argument(
        "--once",
        action="store_true",
        help="Run one sync and exit",
    )
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )
    log = logging.getLogger("catalog_sync_service")

    if args.once:
        return run_sync_once()

    log.info("Catalog sync worker started (interval %ds)", INTERVAL_SEC)
    while True:
        try:
            code = run_sync_once()
            if code != 0:
                log.warning("Sync finished with code %s", code)
        except Exception:
            log.exception("Sync crashed")
        time.sleep(INTERVAL_SEC)


if __name__ == "__main__":
    sys.exit(main() or 0)
