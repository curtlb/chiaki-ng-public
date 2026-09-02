#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Fetch PlayStation cloud catalog (PSNOW + PS Cloud) and upsert into MySQL.

Sources (same as chiaki-ng libchiaki):
  - imagic public PS5 lists
  - public Apollo PSNOW container walk (US / GB stores)
  - optional owned entitlements via NPSSO (CS_CATALOG_NPSSO)
"""

from __future__ import print_function

import json
import logging
import os
import re
import sys
import time
import urllib.parse
from datetime import datetime
from pathlib import Path

import pymysql
import requests

BASE_DIR = Path(__file__).resolve().parent
ENV_FILE = Path(os.environ.get("CS_ENV_FILE", str(BASE_DIR / ".env")))

GENERIC_UA = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
)
KAMAJI_UA = (
    "Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "playstation-now/0.0.0 Chrome/83.0.4103.104 Electron/9.0.4 Safari/537.36 gkApollo"
)
IMAGIC_LISTS = (
    "plus-games-list",
    "ubisoft-classics-list",
    "plus-classics-list",
    "plus-monthly-games-list",
    "free-to-play-list",
    "all-ps5-list",
)
AMERICAS = frozenset(
    "US CA MX BR AR CL CO PE EC BO PY UY CR GT HN NI PA SV DO".split()
)
OWNED_CLIENT_ID = "dc523cc2-b51b-4190-bff0-3397c06871b3"
OWNED_SCOPES = "kamaji:get_internal_entitlements user:account.attributes.validate"
KAMAJI_REDIRECT = (
    "https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/grc-response.html"
)
DATE_FMT = "%Y-%m-%d %H:%M:%S"

log = logging.getLogger("catalog_sync")


def load_env_file():
    if not ENV_FILE.is_file():
        log.warning("Env file not found: %s", ENV_FILE)
        return
    try:
        from dotenv import load_dotenv
        load_dotenv(ENV_FILE, override=False)
    except ImportError:
        with open(ENV_FILE, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, _, val = line.partition("=")
                key, val = key.strip(), val.strip().strip("'\"")
                if key and key not in os.environ:
                    os.environ[key] = val


load_env_file()

DB_HOST = os.environ.get("CS_DB_HOST", "127.0.0.1")
DB_NAME = os.environ.get("CS_DB_NAME", "")
DB_USER = os.environ.get("CS_DB_USER", "")
DB_PASS = os.environ.get("CS_DB_PASS", "")
CATALOG_LOCALE = os.environ.get("CS_CATALOG_LOCALE", "en-PL")
CATALOG_REGION = os.environ.get("CS_CATALOG_REGION", "PL").upper()
CATALOG_NPSSO = os.environ.get("CS_CATALOG_NPSSO", "").strip()
HTTP_TIMEOUT = int(os.environ.get("CS_CATALOG_HTTP_TIMEOUT_SEC", "60"))


def fmt_dt(dt):
    return dt.strftime(DATE_FMT)


def db_connect():
    return pymysql.connect(
        host=DB_HOST,
        user=DB_USER,
        password=DB_PASS,
        database=DB_NAME,
        charset="utf8mb4",
        cursorclass=pymysql.cursors.DictCursor,
        autocommit=False,
        connect_timeout=30,
        read_timeout=600,
        write_timeout=600,
    )


def classics_store_country(account_country):
    return "US" if account_country in AMERICAS else "GB"


def apollo_root_id(account_country):
    return (
        "STORE-MSF192018-APOLLOROOT"
        if account_country in AMERICAS
        else "STORE-MSF192014-APOLLOROOT"
    )


def locale_chain(stored_locale):
    canonical = (stored_locale or "en-US").strip() or "en-US"
    if "-" not in canonical:
        canonical = canonical + "-US"
    parts = canonical.split("-", 1)
    lang = parts[0].lower()
    country = parts[1].upper() if len(parts) > 1 else "US"
    canonical = "%s-%s" % (lang, country)
    en_country = "en-%s" % country
    out = []
    for item in (canonical, en_country, "en-US"):
        if item not in out:
            out.append(item)
    return out


def platform_from_devices(devices):
    if not devices:
        return "unknown"
    joined = " ".join(str(d).lower() for d in devices)
    if "ps5" in joined:
        return "ps5"
    if "ps4" in joined:
        return "ps4"
    if "ps3" in joined:
        return "ps3"
    return "unknown"


def platform_from_product_id(product_id):
    if not product_id:
        return "unknown"
    if product_id.startswith("PPSA") or product_id.startswith("UP"):
        return "ps5"
    if product_id.startswith("CUSA") or product_id.startswith("EP"):
        return "ps4"
    if product_id.startswith("NPUA") or product_id.startswith("NPEA"):
        return "ps3"
    return "unknown"


def make_catalog_key(service_type, stream_identifier, product_id):
    sid = (stream_identifier or product_id or "").strip()
    return "%s:%s" % (service_type, sid)


def extract_image_url(row):
    for key in ("imageUrl", "landscapeImageUrl"):
        val = row.get(key)
        if val:
            return val
    images = row.get("images") or []
    for img in images:
        if not isinstance(img, dict):
            continue
        url = img.get("url")
        if url:
            return url
    return None


def as_str(val, max_len=None):
    if val is None:
        return ""
    s = str(val).strip()
    if max_len is not None and len(s) > max_len:
        return s[:max_len]
    return s


def normalize_row(
    name,
    service_type,
    platform,
    product_id,
    entitlement_id,
    stream_identifier,
    concept_id=None,
    image_url=None,
    category="streamable",
    source_list=None,
    locale=None,
):
    name = as_str(name)
    product_id = as_str(product_id) or None
    entitlement_id = as_str(entitlement_id) or None
    stream_identifier = as_str(stream_identifier or entitlement_id or product_id)
    if not name or not stream_identifier:
        return None
    if not platform or platform == "unknown":
        platform = platform_from_product_id(product_id or stream_identifier)
    catalog_key = make_catalog_key(service_type, stream_identifier, product_id)
    return {
        "CatalogKey": catalog_key,
        "Name": name[:512],
        "ServiceType": service_type,
        "Platform": platform if platform in ("ps3", "ps4", "ps5") else "unknown",
        "ProductId": product_id,
        "EntitlementId": entitlement_id,
        "StreamIdentifier": stream_identifier[:128],
        "ConceptId": as_str(concept_id, 64) or None,
        "ImageUrl": as_str(image_url, 1024) or None,
        "Category": category,
        "SourceList": source_list,
        "Locale": locale,
    }


def http_get(url, headers=None, cookies=None, allow_redirects=True):
    hdrs = {"User-Agent": GENERIC_UA}
    if headers:
        hdrs.update(headers)
    resp = requests.get(
        url,
        headers=hdrs,
        cookies=cookies,
        timeout=HTTP_TIMEOUT,
        allow_redirects=allow_redirects,
    )
    return resp


PLUS_CATALOG_LISTS = frozenset(
    (
        "plus-games-list",
        "ubisoft-classics-list",
        "plus-classics-list",
        "plus-monthly-games-list",
    )
)


def is_cloud_device_game(item):
    devices = item.get("device") or []
    return any(str(d).upper() in ("PS5", "PS4") for d in devices)


def iter_imagic_games(list_doc):
    """imagic returns [{catalogKey, count, games:[...]}, ...] not a flat game list."""
    if not isinstance(list_doc, list):
        return
    for cat in list_doc:
        if not isinstance(cat, dict):
            continue
        games = cat.get("games")
        if not isinstance(games, list):
            continue
        for game in games:
            if isinstance(game, dict):
                yield game


def imagic_category(list_name, game):
    """Mirror chiaki-ng: PS+ streamable vs must-buy-to-stream PS5 cloud."""
    streaming = bool(game.get("streamingSupported"))
    if list_name in PLUS_CATALOG_LISTS:
        return "streamable" if streaming else "purchaseable"
    if list_name == "free-to-play-list":
        return "streamable" if streaming else "purchaseable"
    # all-ps5-list: full PS5 cloud universe
    return "streamable" if streaming else "purchaseable"


def merge_imagic_game(batch, list_name, item, locale):
    if not is_cloud_device_game(item):
        return
    product_id = (item.get("productId") or item.get("id") or "").strip()
    name = (item.get("name") or item.get("title") or "").strip()
    if not product_id or not name:
        return
    category = imagic_category(list_name, item)
    platform = platform_from_devices(item.get("device")) or "ps5"
    row = normalize_row(
        name=name,
        service_type="pscloud",
        platform=platform,
        product_id=product_id,
        entitlement_id=None,
        stream_identifier=product_id,
        concept_id=item.get("conceptId"),
        image_url=extract_image_url(item),
        category=category,
        source_list=list_name,
        locale=locale,
    )
    if not row:
        return
    key = row["CatalogKey"]
    prev = batch.get(key)
    if not prev:
        batch[key] = row
        return
    # Prefer streamable over purchaseable when same product appears in multiple lists
    rank = {"streamable": 2, "owned": 3, "purchaseable": 1}
    if rank.get(row["Category"], 0) > rank.get(prev["Category"], 0):
        batch[key] = row


def fetch_imagic(locale):
    rows = {}
    settled = None
    stats = {"streamable": 0, "purchaseable": 0}
    for loc in locale_chain(locale):
        imagic_locale = loc.lower()
        batch = {}
        succeeded = 0
        for list_name in IMAGIC_LISTS:
            url = (
                "https://www.playstation.com/bin/imagic/gameslist"
                "?locale=%s&categoryList=%s" % (imagic_locale, list_name)
            )
            try:
                resp = http_get(
                    url,
                    headers={
                        "Accept": "application/json",
                        "Content-Type": "application/json",
                    },
                )
                if resp.status_code != 200:
                    continue
                data = resp.json()
                games_seen = 0
                for item in iter_imagic_games(data):
                    games_seen += 1
                    merge_imagic_game(batch, list_name, item, loc)
                if games_seen <= 0:
                    continue
                succeeded += 1
            except Exception as exc:
                log.warning("imagic %s/%s failed: %s", imagic_locale, list_name, exc)
        if succeeded > 0:
            rows.update(batch)
            for row in batch.values():
                stats[row["Category"]] = stats.get(row["Category"], 0) + 1
            settled = loc
            log.info(
                "imagic locale %s: %d titles (streamable=%d purchaseable=%d)",
                loc,
                len(batch),
                stats.get("streamable", 0),
                stats.get("purchaseable", 0),
            )
            break
    return rows, settled


def apollo_walk_container(store_country, container_id, seen, page_budget):
    base = (
        "https://psnow.playstation.com/store/api/pcnow/00_09_000/container/"
        "%s/en/19/%s" % (store_country, container_id)
    )
    games = []
    child_ids = []
    children_dropped = False
    start = 0
    total = -1
    while page_budget[0] > 0:
        page_budget[0] -= 1
        url = "%s?useOffers=true&gkb=1&gkb2=1&start=%d&size=100" % (base, start)
        try:
            resp = http_get(url, headers={"Accept": "application/json", "User-Agent": KAMAJI_UA})
            if resp.status_code != 200:
                return games, child_ids, False
            obj = resp.json()
        except Exception as exc:
            log.warning("apollo %s start=%d failed: %s", container_id, start, exc)
            return games, child_ids, False
        if total < 0:
            total = int(obj.get("total_results") or -1)
        links = obj.get("links") or []
        link_count = 0
        for g in links:
            if not isinstance(g, dict):
                continue
            link_count += 1
            gid = (g.get("id") or "").strip()
            ctype = (g.get("container_type") or "").strip().lower()
            if ctype == "product" and gid:
                if gid in seen:
                    continue
                seen.add(gid)
                name = (g.get("name") or "").strip()
                platform = platform_from_devices(g.get("device"))
                if platform == "unknown":
                    platform = platform_from_product_id(gid)
                row = normalize_row(
                    name=name,
                    service_type="psnow",
                    platform=platform,
                    product_id=gid,
                    entitlement_id=gid,
                    stream_identifier=gid,
                    image_url=extract_image_url(g),
                    category="streamable",
                    source_list="apollo:%s" % store_country,
                    locale=None,
                )
                if row:
                    games.append(row)
            elif ctype == "container" and gid:
                if gid not in child_ids:
                    if len(child_ids) >= 40:
                        children_dropped = True
                    else:
                        child_ids.append(gid)
        start += 100
        if total >= 0 and start >= total:
            return games, child_ids, not children_dropped
        if link_count <= 0 or start >= 10000:
            return games, child_ids, False
    return games, child_ids, False


def fetch_apollo(account_country):
    store = classics_store_country(account_country)
    root = apollo_root_id(account_country)
    seen = set()
    page_budget = [200]
    rows = {}
    games, child_ids, complete = apollo_walk_container(store, root, seen, page_budget)
    for row in games:
        rows[row["CatalogKey"]] = row
    for child in child_ids:
        sub_games, _, _ = apollo_walk_container(store, child, seen, page_budget)
        for row in sub_games:
            rows[row["CatalogKey"]] = row
    log.info(
        "apollo %s (%s): %d titles%s",
        account_country,
        store,
        len(rows),
        "" if complete else " (partial)",
    )
    return rows


def owned_service_type(ent):
    for attr in ent.get("entitlement_attributes") or []:
        if not isinstance(attr, dict):
            continue
        pid = (attr.get("platform_id") or "").lower()
        if pid == "ps5":
            return "pscloud"
        if pid in ("ps4", "ps3"):
            return "psnow"
    return "pscloud"


def fetch_owned_entitlements(npsso):
    if not npsso:
        return {}
    auth_url = (
        "https://ca.account.sony.com/api/v1/oauth/authorize"
        "?response_type=token&scope=%s&client_id=%s&redirect_uri=%s"
        "&service_entity=urn%%3Aservice-entity%%3Apsn&prompt=none"
        % (
            urllib.parse.quote(OWNED_SCOPES, safe=""),
            OWNED_CLIENT_ID,
            urllib.parse.quote(KAMAJI_REDIRECT, safe=""),
        )
    )
    try:
        resp = http_get(auth_url, cookies={"npsso": npsso}, allow_redirects=False)
    except Exception as exc:
        log.warning("owned oauth failed: %s", exc)
        return {}
    token = None
    if resp.status_code == 302:
        loc = resp.headers.get("Location") or ""
        m = re.search(r"[?&#]access_token=([^&]+)", loc)
        if m:
            token = urllib.parse.unquote(m.group(1))
    if not token:
        log.warning("owned oauth: no access_token (status %s)", resp.status_code)
        return {}

    rows = {}
    start = 0
    page_size = 300
    for _ in range(200):
        url = (
            "https://commerce.api.np.km.playstation.net/commerce/api/v1/users/me/"
            "internal_entitlements?fields=game_meta&entitlement_type=5"
            "&start=%d&size=%d" % (start, page_size)
        )
        try:
            resp = http_get(
                url,
                headers={
                    "Authorization": "Bearer %s" % token,
                    "Accept": "application/json",
                },
            )
            if resp.status_code in (401, 403):
                log.warning("owned entitlements auth error %s", resp.status_code)
                break
            if resp.status_code != 200:
                log.warning("owned entitlements HTTP %s", resp.status_code)
                break
            data = resp.json()
        except Exception as exc:
            log.warning("owned entitlements page failed: %s", exc)
            break
        page = data.get("entitlements") or []
        for ent in page:
            if not isinstance(ent, dict):
                continue
            if not ent.get("active_flag"):
                continue
            if int(ent.get("feature_type") or 0) == 0:
                continue
            product_id = (ent.get("product_id") or "").strip()
            if product_id.startswith(("IP", "SUB")):
                continue
            entitlement_id = (ent.get("id") or "").strip()
            meta = ent.get("game_meta") or {}
            name = (meta.get("name") or entitlement_id or product_id).strip()
            service_type = owned_service_type(ent)
            stream_id = entitlement_id or product_id
            platform = "ps5" if service_type == "pscloud" else platform_from_product_id(product_id)
            row = normalize_row(
                name=name,
                service_type=service_type,
                platform=platform,
                product_id=product_id or None,
                entitlement_id=entitlement_id or None,
                stream_identifier=stream_id,
                concept_id=meta.get("concept_id"),
                image_url=meta.get("icon_url"),
                category="owned",
                source_list="owned_entitlements",
                locale=None,
            )
            if row:
                rows[row["CatalogKey"]] = row
        if len(page) < page_size:
            break
        start += len(page)
        time.sleep(0.1)
    log.info("owned entitlements: %d titles", len(rows))
    return rows


def fetch_all_catalog_rows():
    merged = {}
    imagic_rows, settled = fetch_imagic(CATALOG_LOCALE)
    merged.update(imagic_rows)
    apollo_rows = fetch_apollo(CATALOG_REGION)
    merged.update(apollo_rows)
    # Also walk US apollo when region is EU — more SKU coverage for admin reference
    if classics_store_country(CATALOG_REGION) == "GB":
        merged.update(fetch_apollo("US"))
    if CATALOG_NPSSO:
        merged.update(fetch_owned_entitlements(CATALOG_NPSSO))
    else:
        log.info("CS_CATALOG_NPSSO not set — skipping owned entitlements sync")
    log.info(
        "catalog fetch total: %d (imagic locale=%s, apollo region=%s)",
        len(merged),
        settled or CATALOG_LOCALE,
        CATALOG_REGION,
    )
    return merged


UPSERT_SQL = (
    "INSERT INTO CloudStreaming_Catalog "
    "(CatalogKey, Name, ServiceType, Platform, ProductId, EntitlementId, "
    "StreamIdentifier, ConceptId, ImageUrl, Category, SourceList, Locale, "
    "IsVisible, FirstSeenAt, LastSyncedAt) "
    "VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,1,%s,%s) "
    "ON DUPLICATE KEY UPDATE "
    "Name=VALUES(Name), Platform=VALUES(Platform), ProductId=VALUES(ProductId), "
    "EntitlementId=COALESCE(VALUES(EntitlementId), EntitlementId), "
    "StreamIdentifier=VALUES(StreamIdentifier), ConceptId=VALUES(ConceptId), "
    "ImageUrl=COALESCE(VALUES(ImageUrl), ImageUrl), Category=VALUES(Category), "
    "SourceList=VALUES(SourceList), Locale=COALESCE(VALUES(Locale), Locale), "
    "IsVisible=1, LastSyncedAt=VALUES(LastSyncedAt)"
)
UPSERT_BATCH = int(os.environ.get("CS_CATALOG_UPSERT_BATCH", "250"))


def row_to_params(row, now):
    return (
        row["CatalogKey"],
        row["Name"],
        row["ServiceType"],
        row["Platform"],
        row["ProductId"],
        row["EntitlementId"],
        row["StreamIdentifier"],
        row["ConceptId"],
        row["ImageUrl"],
        row["Category"],
        row["SourceList"],
        row["Locale"],
        now,
        now,
    )


def upsert_catalog(conn, rows):
    now = fmt_dt(datetime.now())
    inserted = updated = errors = 0
    items = list(rows.values())
    total = len(items)
    log.info("DB upsert starting: %d rows -> %s@%s/%s", total, DB_USER, DB_HOST, DB_NAME)

    for start in range(0, total, UPSERT_BATCH):
        batch = items[start : start + UPSERT_BATCH]
        with conn.cursor() as cur:
            for row in batch:
                try:
                    cur.execute(UPSERT_SQL, row_to_params(row, now))
                    if cur.rowcount == 1:
                        inserted += 1
                    elif cur.rowcount == 2:
                        updated += 1
                except Exception as exc:
                    errors += 1
                    if errors <= 5:
                        log.error(
                            "upsert failed for %s: %s",
                            row.get("CatalogKey"),
                            exc,
                        )
        conn.commit()
        done = min(start + len(batch), total)
        if done % 1000 < UPSERT_BATCH or done == total:
            log.info("DB upsert progress: %d / %d", done, total)

    with conn.cursor() as cur:
        cur.execute("SELECT COUNT(*) AS cnt FROM CloudStreaming_Catalog")
        db_count = cur.fetchone()["cnt"]
    log.info(
        "DB upsert done: inserted=%d updated=%d errors=%d table_count=%d",
        inserted,
        updated,
        errors,
        db_count,
    )
    return inserted, updated


def run_sync_once():
    if not DB_NAME or not DB_USER:
        print("Set CS_DB_NAME, CS_DB_USER, CS_DB_PASS in .env", file=sys.stderr)
        return 2
    rows = fetch_all_catalog_rows()
    if not rows:
        log.error("No catalog rows fetched — check network and locale/region settings")
        return 1
    conn = db_connect()
    try:
        ins, upd = upsert_catalog(conn, rows)
    except Exception:
        log.exception("DB upsert crashed")
        return 1
    finally:
        conn.close()
    return 0


if __name__ == "__main__":
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )
    sys.exit(run_sync_once())
