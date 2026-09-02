#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Cloud Streaming billing UDP server for 4cloud.pro + chiaki-ng.

JSON over UDP (request/response share the same "id" field).
Default bind: 0.0.0.0:13750

Actions:
  ping, catalog, quote, start, confirm_stream, heartbeat, renew, end_stream, status

Deploy:
  cp .env.example .env   # fill in secrets locally (never commit .env)
  pip install -r requirements.txt
  pm2 start ecosystem.config.js
"""

from __future__ import print_function

import hashlib
import json
import logging
import os
import random
import re
import socket
import sys
import threading
import time
import urllib.parse
import uuid
from datetime import datetime, timedelta, date
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent
ENV_FILE = Path(os.environ.get("CS_ENV_FILE", str(BASE_DIR / ".env")))


def load_env_file():
    """Load .env from the script directory (pm2 env_file is not reliable)."""
    if not ENV_FILE.is_file():
        log_pre = logging.getLogger("cloud_billing_udp")
        log_pre.warning("Env file not found: %s", ENV_FILE)
        return False
    try:
        from dotenv import load_dotenv
        load_dotenv(ENV_FILE, override=False)
        return True
    except ImportError:
        # Minimal parser if python-dotenv is missing
        with open(ENV_FILE, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, _, val = line.partition("=")
                key, val = key.strip(), val.strip().strip("'\"")
                if key and key not in os.environ:
                    os.environ[key] = val
        return True


load_env_file()

try:
    import pymysql
except ImportError:
    print("pip install pymysql requests", file=sys.stderr)
    sys.exit(1)

try:
    import requests
except ImportError:
    print("pip install pymysql requests", file=sys.stderr)
    sys.exit(1)


def require_env(name):
    value = os.environ.get(name, "").strip()
    if not value:
        print(
            "Missing required environment variable: %s (check %s)"
            % (name, ENV_FILE),
            file=sys.stderr,
        )
        sys.exit(2)
    return value


# --- DB / Robokassa: no defaults for secrets (set via environment or .env) ---
DB_HOST = os.environ.get("CS_DB_HOST", "127.0.0.1")
DB_NAME = os.environ.get("CS_DB_NAME", "")
DB_USER = os.environ.get("CS_DB_USER", "")
DB_PASS = os.environ.get("CS_DB_PASS", "")

MRH_LOGIN = os.environ.get("CS_MRH_LOGIN", "")
PASS1 = os.environ.get("CS_ROBOKASSA_PASS1", "")
PASS2 = os.environ.get("CS_ROBOKASSA_PASS2", "")
RECURRING_URL = "https://auth.robokassa.ru/Merchant/Recurring"
OPSTATE_URL = "https://auth.robokassa.ru/Merchant/WebService/Service.asmx/OpStateExt"

UDP_HOST = os.environ.get("CS_BILLING_UDP_HOST", "0.0.0.0")
UDP_PORT = int(os.environ.get("CS_BILLING_UDP_PORT", "13750"))
RETENTION_DAYS = int(os.environ.get("CS_RETENTION_DAYS", "3"))
RENEW_LEAD = timedelta(minutes=int(os.environ.get("CS_RENEW_LEAD_MINUTES", "10")))
HEARTBEAT_TIMEOUT = int(os.environ.get("CS_HEARTBEAT_TIMEOUT_SEC", "180"))
DEFAULT_HOURLY = float(os.environ.get("CS_DEFAULT_HOURLY_PRICE", "110"))
OPSTATE_POLL_SEC = 5
OPSTATE_MAX_WAIT_SEC = 180
DATE_FMT = "%Y-%m-%d %H:%M:%S"

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger("cloud_billing_udp")


def php_urlencode(s):
    if isinstance(s, bytes):
        s = s.decode("utf-8")
    return urllib.parse.quote(s, safe="").replace("%20", "+")


def fmt_dt(dt):
    return dt.strftime(DATE_FMT)


def parse_db_datetime(value):
    if value is None:
        return None
    if isinstance(value, datetime):
        return value.replace(tzinfo=None)
    if isinstance(value, date):
        return datetime.combine(value, datetime.min.time())
    text = str(value).strip()
    if not text:
        return None
    return datetime.strptime(text[:19], DATE_FMT)


def sql_minutes_left_expr():
    """Remaining paid minutes using the same MySQL clock as PaidUntil / NOW(3)."""
    return (
        "GREATEST(0, CEILING(TIMESTAMPDIFF(SECOND, NOW(3), s.PaidUntil) / 60))"
    )


def fetch_minutes_left(conn, session_id):
    with conn.cursor() as cur:
        cur.execute(
            "SELECT "
            + sql_minutes_left_expr()
            + " AS MinutesLeft FROM CloudStreaming_Sessions s WHERE s.ID = %s LIMIT 1",
            (session_id,),
        )
        row = cur.fetchone()
    if not row:
        return 0
    return int(row.get("MinutesLeft") or 0)


def db_connect():
    return pymysql.connect(
        host=DB_HOST,
        user=DB_USER,
        password=DB_PASS,
        database=DB_NAME,
        charset="utf8mb4",
        cursorclass=pymysql.cursors.DictCursor,
        autocommit=False,
        connect_timeout=20,
        read_timeout=60,
        write_timeout=60,
    )


def reply(req_id, ok, **kwargs):
    out = {"id": req_id, "ok": bool(ok)}
    out.update(kwargs)
    return out


def get_payment_method(conn, email):
    """Cloud gaming card — NOT the console rental autobilling table."""
    with conn.cursor() as cur:
        cur.execute(
            "SELECT pm.ID, pm.UserID, pm.Email, pm.StartPaymentID, pm.Status "
            "FROM CloudStreaming_PaymentMethods pm "
            "WHERE pm.Email = %s AND pm.Status = 'active' LIMIT 1",
            (email.strip().lower(),),
        )
        return cur.fetchone()


PAYMENT_SETUP_MSG = (
    "Для облачного гейминга нужна отдельная привязка карты. "
    "Оформите автоплатёж для cloud gaming в личном кабинете 4cloud.pro "
    "(не путать с арендой консоли)."
)

SCHEMA_MIGRATION_MSG = (
    "База данных не обновлена: отсутствует колонка CatalogID. "
    "На сервере выполните migrate_catalog.sql (см. scripts/cloud_billing/DEPLOY.md) "
    "и перезапустите: pm2 restart cloud-billing-udp"
)


def friendly_db_error(exc):
    msg = str(exc)
    if "1054" in msg and "CatalogID" in msg:
        return SCHEMA_MIGRATION_MSG
    if "1146" in msg and "CloudStreaming_Catalog" in msg:
        return (
            "Таблица CloudStreaming_Catalog не найдена. "
            "Выполните migrate_catalog.sql и запустите catalog sync (scripts/cloud_catalog_sync/)."
        )
    if "1146" in msg and "CloudStreaming_PaymentMethods" in msg:
        return PAYMENT_SETUP_MSG
    return msg


def _table_exists(conn, table):
    with conn.cursor() as cur:
        cur.execute("SHOW TABLES LIKE %s", (table,))
        return cur.fetchone() is not None


def _column_exists(conn, table, column):
    with conn.cursor() as cur:
        cur.execute("SHOW COLUMNS FROM `%s` LIKE %%s" % table.replace("`", ""), (column,))
        return cur.fetchone() is not None


def verify_schema(conn):
    """Fail fast at startup when the catalog migration was not applied."""
    required = [
        ("CloudStreaming_Catalog", "ID"),
        ("CloudStreaming_Games", "CatalogID"),
        ("CloudStreaming_AccountOwnedGames", "CatalogID"),
        ("CloudStreaming_PaymentMethods", "StartPaymentID"),
    ]
    missing = []
    for table, column in required:
        if not _table_exists(conn, table) or not _column_exists(conn, table, column):
            missing.append("%s.%s" % (table, column))
    if missing:
        raise RuntimeError(
            "Cloud billing schema is outdated (missing: %s). Run migrate_catalog_v2.sql "
            "and migrate_payment_methods.sql, then pm2 restart cloud-billing-udp."
            % ", ".join(missing)
        )


def ensure_user(conn, email):
    with conn.cursor() as cur:
        cur.execute("SELECT * FROM CloudStreaming_Users WHERE User = %s LIMIT 1", (email,))
        row = cur.fetchone()
        if row:
            return row
        cur.execute(
            "INSERT INTO CloudStreaming_Users (User) VALUES (%s)",
            (email,),
        )
        cur.execute("SELECT * FROM CloudStreaming_Users WHERE User = %s LIMIT 1", (email,))
        return cur.fetchone()


def find_catalog_match(conn, service_type, game_identifier):
    st = service_type.lower().strip()
    gid = game_identifier.strip()
    with conn.cursor() as cur:
        cur.execute(
            "SELECT * FROM CloudStreaming_Catalog "
            "WHERE ServiceType = %s AND IsVisible = 1 AND ("
            "StreamIdentifier = %s OR ProductId = %s OR EntitlementId = %s"
            ") LIMIT 1",
            (st, gid, gid, gid),
        )
        return cur.fetchone()


def access_type_for_catalog(service_type, category=None):
    st = (service_type or "").lower()
    cat = (category or "").lower()
    if st == "psnow" or cat == "streamable":
        return "ps_plus"
    if cat == "owned":
        return "owned_only"
    # Hourly rental: PS+ account can add / stream catalog titles without store purchase.
    return "both"


def catalog_for_game(conn, game):
    if game.get("CatalogID"):
        with conn.cursor() as cur:
            cur.execute(
                "SELECT * FROM CloudStreaming_Catalog WHERE ID = %s LIMIT 1",
                (game["CatalogID"],),
            )
            row = cur.fetchone()
            if row:
                return row
    return find_catalog_match(
        conn, game.get("ServiceType") or "", game.get("GameIdentifier") or ""
    )


def refresh_game_access_type(conn, game):
    catalog = catalog_for_game(conn, game)
    expected = access_type_for_catalog(
        game.get("ServiceType"),
        catalog.get("Category") if catalog else None,
    )
    if game.get("AccessType") != expected:
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE CloudStreaming_Games SET AccessType = %s WHERE ID = %s",
                (expected, game["ID"]),
            )
        game = dict(game)
        game["AccessType"] = expected
    return game


def ensure_game(conn, service_type, game_identifier, game_name=None):
    st = service_type.lower().strip()
    gid = game_identifier.strip()
    with conn.cursor() as cur:
        cur.execute(
            "SELECT * FROM CloudStreaming_Games "
            "WHERE ServiceType = %s AND GameIdentifier = %s LIMIT 1",
            (st, gid),
        )
        row = cur.fetchone()
        if row:
            return refresh_game_access_type(conn, row)
        catalog = find_catalog_match(conn, st, gid)
        code = re.sub(r"[^a-zA-Z0-9_]+", "_", (game_name or (catalog or {}).get("Name") or gid))[:60].lower()
        access = access_type_for_catalog(st, (catalog or {}).get("Category"))
        display_name = game_name or (catalog or {}).get("Name") or gid
        catalog_id = catalog["ID"] if catalog else None
        cur.execute(
            "INSERT INTO CloudStreaming_Games "
            "(CatalogID, Code, Name, ServiceType, GameIdentifier, AccessType, HourlyPrice) "
            "VALUES (%s, %s, %s, %s, %s, %s, %s)",
            (catalog_id, code, display_name, st, gid, access, DEFAULT_HOURLY),
        )
        cur.execute(
            "SELECT * FROM CloudStreaming_Games "
            "WHERE ServiceType = %s AND GameIdentifier = %s LIMIT 1",
            (st, gid),
        )
        return cur.fetchone()


def account_owns_game(conn, account_id, service_type, game_identifier, game_id=None):
    st = service_type.lower().strip()
    gid = game_identifier.strip()
    with conn.cursor() as cur:
        cur.execute(
            "SELECT 1 FROM CloudStreaming_AccountOwnedGames aog "
            "JOIN CloudStreaming_Catalog c ON c.ID = aog.CatalogID "
            "WHERE aog.AccountID = %s AND c.ServiceType = %s AND ("
            "c.StreamIdentifier = %s OR c.ProductId = %s OR c.EntitlementId = %s"
            ") LIMIT 1",
            (account_id, st, gid, gid, gid),
        )
        if cur.fetchone():
            return True
        if game_id:
            cur.execute(
                "SELECT 1 FROM CloudStreaming_AccountOwnedGames "
                "WHERE AccountID = %s AND GameID = %s LIMIT 1",
                (account_id, game_id),
            )
            if cur.fetchone():
                return True
    return False


def account_can_play_game(conn, account, game):
    game = refresh_game_access_type(conn, game) if game.get("ID") else game
    access = game.get("AccessType")
    if game["ServiceType"] == "psnow" or access == "ps_plus":
        if int(account.get("HasPsPlus") or 0):
            return True
    if access in ("owned_only", "both"):
        if account_owns_game(
            conn,
            account["ID"],
            game["ServiceType"],
            game["GameIdentifier"],
            game.get("ID"),
        ):
            return True
    if int(account.get("HasPsPlus") or 0) and game.get("AccessType") in ("ps_plus", "both"):
        return True
    return False


def find_any_user_lease(conn, user_id):
    with conn.cursor() as cur:
        cur.execute(
            "SELECT l.*, a.NPSSO, a.Label AS AccountLabel, a.HasPsPlus "
            "FROM CloudStreaming_Leases l "
            "JOIN CloudStreaming_Accounts a ON a.ID = l.AccountID "
            "WHERE l.UserID = %s AND l.Status IN ('active','retention') "
            "AND l.RetentionUntil > NOW(3) "
            "ORDER BY l.LastActivityAt DESC LIMIT 1",
            (user_id,),
        )
        return cur.fetchone()


def find_active_lease(conn, user_id, game):
    game = refresh_game_access_type(conn, game) if game.get("ID") else game
    with conn.cursor() as cur:
        cur.execute(
            "SELECT l.*, a.NPSSO, a.Label AS AccountLabel, a.HasPsPlus "
            "FROM CloudStreaming_Leases l "
            "JOIN CloudStreaming_Accounts a ON a.ID = l.AccountID "
            "WHERE l.UserID = %s AND l.Status IN ('active','retention') "
            "AND l.RetentionUntil > NOW(3) "
            "ORDER BY l.LastActivityAt DESC",
            (user_id,),
        )
        leases = cur.fetchall()
    for lease in leases:
        acc = {
            "ID": lease["AccountID"],
            "NPSSO": lease["NPSSO"],
            "HasPsPlus": lease["HasPsPlus"],
            "Label": lease.get("AccountLabel"),
        }
        if account_can_play_game(conn, acc, game):
            return lease
    # Hourly rental: one PS account per player — reuse it for any catalog title.
    return find_any_user_lease(conn, user_id)


def collect_game_identity(conn, service_type, game_identifier, game=None):
    """Aliases for the same catalog title (productId, entitlementId, streamIdentifier)."""
    st = service_type.lower().strip()
    gid = game_identifier.strip()
    identifiers = {gid}
    catalog_ids = set()
    catalog = find_catalog_match(conn, st, gid)
    if catalog:
        catalog_ids.add(catalog["ID"])
        for field in ("StreamIdentifier", "ProductId", "EntitlementId"):
            val = catalog.get(field)
            if val:
                identifiers.add(str(val).strip())
    if game:
        if game.get("CatalogID"):
            catalog_ids.add(game["CatalogID"])
        if game.get("GameIdentifier"):
            identifiers.add(str(game["GameIdentifier"]).strip())
    identifiers = {x for x in identifiers if x}
    catalog_ids = {x for x in catalog_ids if x}
    return st, identifiers, catalog_ids


def _session_same_game(row, st, identifiers, catalog_ids, game_id=None):
    if game_id and row.get("GameID") == game_id:
        return True
    if row.get("ServiceType") == st:
        sid = (row.get("GameIdentifier") or "").strip()
        if sid and sid in identifiers:
            return True
    row_catalog = row.get("CatalogID")
    if row_catalog and catalog_ids and row_catalog in catalog_ids:
        return True
    return False


def find_resumable_session(conn, user_id, service_type, game_identifier, game_id=None, game=None):
    """Paid session for the same game that still has time left (after stream stop or app close)."""
    st, identifiers, catalog_ids = collect_game_identity(
        conn, service_type, game_identifier, game
    )
    active_statuses = ("active", "grace_no_stream", "renewal_pending")
    status_ph = ",".join(["%s"] * len(active_statuses))

    with conn.cursor() as cur:
        cur.execute(
            "SELECT s.*, g.CatalogID FROM CloudStreaming_Sessions s "
            "JOIN CloudStreaming_Games g ON g.ID = s.GameID "
            "WHERE s.UserID = %s AND s.Status IN (" + status_ph + ") AND s.PaidUntil > NOW(3) "
            "ORDER BY s.PaidUntil DESC",
            (user_id,) + active_statuses,
        )
        rows = cur.fetchall()

    for row in rows:
        if _session_same_game(row, st, identifiers, catalog_ids, game_id):
            return row

    # Recover session wrongly ended (e.g. old billing ended same game on re-entry).
    recover_reasons = ("game_switch", "stream_stopped", "user_exit")
    reason_ph = ",".join(["%s"] * len(recover_reasons))
    with conn.cursor() as cur:
        cur.execute(
            "SELECT s.*, g.CatalogID FROM CloudStreaming_Sessions s "
            "JOIN CloudStreaming_Games g ON g.ID = s.GameID "
            "WHERE s.UserID = %s AND s.Status = 'ended' AND s.PaidUntil > NOW(3) "
            "AND s.EndReason IN (" + reason_ph + ") "
            "ORDER BY s.PaidUntil DESC",
            (user_id,) + recover_reasons,
        )
        rows = cur.fetchall()

    for row in rows:
        if _session_same_game(row, st, identifiers, catalog_ids, game_id):
            with conn.cursor() as cur:
                cur.execute(
                    "UPDATE CloudStreaming_Sessions SET Status='grace_no_stream', "
                    "StreamActive=0, EndedAt=NULL, EndReason=NULL, "
                    "UiMessage=%s WHERE ID=%s",
                    ("Восстановлена оплаченная сессия…", row["ID"]),
                )
                cur.execute(
                    "SELECT s.*, g.CatalogID FROM CloudStreaming_Sessions s "
                    "JOIN CloudStreaming_Games g ON g.ID = s.GameID "
                    "WHERE s.ID = %s LIMIT 1",
                    (row["ID"],),
                )
                return cur.fetchone()
    return None


def find_other_active_session(conn, user_id, service_type, game_identifier, game_id=None, game=None):
    """Another game with remaining paid time (switching forfeits that time)."""
    st, identifiers, catalog_ids = collect_game_identity(
        conn, service_type, game_identifier, game
    )
    with conn.cursor() as cur:
        cur.execute(
            "SELECT s.*, g.CatalogID FROM CloudStreaming_Sessions s "
            "JOIN CloudStreaming_Games g ON g.ID = s.GameID "
            "WHERE s.UserID = %s AND s.Status IN ('active','grace_no_stream','renewal_pending') "
            "AND s.PaidUntil > NOW(3) "
            "ORDER BY s.PaidUntil DESC",
            (user_id,),
        )
        rows = cur.fetchall()

    for row in rows:
        if _session_same_game(row, st, identifiers, catalog_ids, game_id):
            continue
        return row
    return None


def allocate_account(conn, game):
    with conn.cursor() as cur:
        cur.execute(
            "SELECT * FROM CloudStreaming_Accounts "
            "WHERE Status = 'available' ORDER BY LastUsedAt IS NULL DESC, LastUsedAt ASC "
            "FOR UPDATE"
        )
        accounts = cur.fetchall()
    for acc in accounts:
        if account_can_play_game(conn, acc, game):
            return acc
    return None


def touch_lease(conn, lease_id):
    now = datetime.now()
    retention = now + timedelta(days=RETENTION_DAYS)
    with conn.cursor() as cur:
        cur.execute(
            "UPDATE CloudStreaming_Leases SET "
            "LastActivityAt = %s, RetentionUntil = %s, Status = 'active', UpdatedAt = NOW(3) "
            "WHERE ID = %s",
            (fmt_dt(now), fmt_dt(retention), lease_id),
        )


def build_receipt(out_sum, game_name):
    arr = {
        "sno": "usn_income",
        "items": [
            {
                "name": "Облачный гейминг: %s (1 час)" % (game_name[:80]),
                "quantity": 1,
                "sum": float(out_sum),
                "payment_method": "full_payment",
                "tax": "none",
            }
        ],
    }
    raw = json.dumps(arr, ensure_ascii=True)
    arrrr = php_urlencode(raw)
    arrrrrr = php_urlencode(php_urlencode(arrrr))
    return arrrr, arrrrrr


def robokassa_charge(email, prev_id, out_sum, game_name):
    invoice_id = str(random.randint(1, 2147483647))
    out_sum_str = str(int(out_sum))
    arrrr, arrrrrr = build_receipt(out_sum, game_name)
    sign_src = (
        "%s:%s:%s:%s:%s:shp_days=%s:shp_plan=%s:shp_promocode=%s:shp_site=%s:shp_user=%s"
        % (MRH_LOGIN, out_sum_str, invoice_id, arrrr, PASS1, "1", "cloud_hour", "", "fourcloud", email)
    )
    crc = hashlib.md5(sign_src.encode("utf-8")).hexdigest().upper()
    body = (
        "MerchantLogin=" + php_urlencode(MRH_LOGIN)
        + "&OutSum=" + php_urlencode(out_sum_str)
        + "&InvoiceID=" + php_urlencode(invoice_id)
        + "&PreviousInvoiceID=" + php_urlencode(str(prev_id))
        + "&Description=" + php_urlencode("Cloud gaming 1h")
        + "&SignatureValue=" + php_urlencode(crc)
        + "&Receipt=" + arrrrrr
        + "&shp_days=" + php_urlencode("1")
        + "&shp_plan=" + php_urlencode("cloud_hour")
        + "&shp_promocode=" + php_urlencode("")
        + "&shp_site=" + php_urlencode("fourcloud")
        + "&shp_user=" + php_urlencode(email)
    )
    resp = requests.post(
        RECURRING_URL,
        data=body.encode("ascii"),
        headers={"Content-Type": "application/x-www-form-urlencoded"},
        timeout=45,
    )
    text = (resp.text or "").strip()
    return {
        "created": text.upper().startswith("OK"),
        "invoice_id": invoice_id,
        "response": text[:500],
        "http_status": resp.status_code,
        "sum": out_sum_str,
    }


def opstate(invoice_id):
    crc = hashlib.md5(
        ("%s:%s:%s" % (MRH_LOGIN, invoice_id, PASS2)).encode("utf-8")
    ).hexdigest().upper()
    r = requests.get(
        OPSTATE_URL,
        params={"MerchantLogin": MRH_LOGIN, "InvoiceID": invoice_id, "Signature": crc},
        timeout=20,
    )
    raw = r.text or ""
    m = re.search(r"<State>.*?<Code>(\d+)</Code>", raw, re.S | re.I)
    if not m:
        m = re.search(r"<Code>(\d+)</Code>", raw)
    return int(m.group(1)) if m else None


def wait_paid(invoice_id, email=""):
    deadline = time.time() + OPSTATE_MAX_WAIT_SEC
    while time.time() < deadline:
        state = opstate(invoice_id)
        log.info("[%s] OpState invoice=%s state=%s", email, invoice_id, state)
        if state == 100:
            return True
        if state in (10, 60):
            return False
        time.sleep(OPSTATE_POLL_SEC)
    return False


def charge_hour(conn, email, session_id, user_id, block_no, amount, game_name, idem_key):
    pm = get_payment_method(conn, email)
    if not pm or not pm.get("StartPaymentID"):
        return False, PAYMENT_SETUP_MSG

    with conn.cursor() as cur:
        cur.execute(
            "SELECT ID FROM CloudStreaming_Charges WHERE IdempotencyKey = %s LIMIT 1",
            (idem_key,),
        )
        existing = cur.fetchone()
        if existing:
            cur.execute(
                "SELECT Status, ErrorMessage FROM CloudStreaming_Charges WHERE ID = %s",
                (existing["ID"],),
            )
            ch = cur.fetchone()
            if ch and ch["Status"] == "succeeded":
                return True, "Уже оплачено"
            if ch and ch["Status"] == "failed":
                return False, ch.get("ErrorMessage") or "Предыдущая оплата не прошла"

    result = robokassa_charge(email, pm["StartPaymentID"], amount, game_name)
    with conn.cursor() as cur:
        cur.execute(
            "INSERT INTO CloudStreaming_Charges "
            "(SessionID, UserID, BlockNo, Amount, Status, ProviderInvoiceID, IdempotencyKey) "
            "VALUES (%s, %s, %s, %s, 'pending', %s, %s)",
            (session_id, user_id, block_no, amount, result["invoice_id"], idem_key),
        )

    if not result["created"]:
        err = "Не удалось создать платёж (HTTP %s): %s" % (
            result["http_status"],
            result["response"][:200],
        )
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE CloudStreaming_Charges SET Status='failed', ErrorMessage=%s "
                "WHERE IdempotencyKey=%s",
                (err, idem_key),
            )
        return False, err

    if not wait_paid(result["invoice_id"], email):
        err = "Оплата не подтверждена банком. Проверьте карту и попробуйте снова."
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE CloudStreaming_Charges SET Status='failed', ErrorMessage=%s "
                "WHERE IdempotencyKey=%s",
                (err, idem_key),
            )
        return False, err

    with conn.cursor() as cur:
        cur.execute(
            "UPDATE CloudStreaming_Charges SET Status='succeeded' WHERE IdempotencyKey=%s",
            (idem_key,),
        )
    return True, "Оплата прошла успешно"


def end_other_active_sessions(
    conn,
    user_id,
    except_session_id=None,
    service_type=None,
    game_identifier=None,
    game_id=None,
    game=None,
):
    st, identifiers, catalog_ids = (None, set(), set())
    if service_type and game_identifier:
        st, identifiers, catalog_ids = collect_game_identity(
            conn, service_type, game_identifier, game
        )
    with conn.cursor() as cur:
        cur.execute(
            "SELECT s.ID, s.SessionToken, s.ServiceType, s.GameIdentifier, s.GameID, "
            "g.CatalogID FROM CloudStreaming_Sessions s "
            "JOIN CloudStreaming_Games g ON g.ID = s.GameID "
            "WHERE s.UserID = %s AND s.Status IN ('active','grace_no_stream','renewal_pending')",
            (user_id,),
        )
        rows = cur.fetchall()
    for row in rows:
        if except_session_id and row["ID"] == except_session_id:
            continue
        if st and _session_same_game(row, st, identifiers, catalog_ids, game_id):
            continue
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE CloudStreaming_Sessions SET Status='ended', EndedAt=NOW(3), "
                "EndReason='game_switch', StreamActive=0 WHERE ID=%s",
                (row["ID"],),
            )


def session_payload(conn, sess):
    with conn.cursor() as cur:
        cur.execute(
            "SELECT s.*, g.Name AS GameName, g.HourlyPrice, a.NPSSO, a.Label AS AccountLabel, "
            "l.RetentionUntil, "
            + sql_minutes_left_expr()
            + " AS MinutesLeft "
            "FROM CloudStreaming_Sessions s "
            "JOIN CloudStreaming_Games g ON g.ID = s.GameID "
            "JOIN CloudStreaming_Accounts a ON a.ID = s.AccountID "
            "JOIN CloudStreaming_Leases l ON l.ID = s.LeaseID "
            "WHERE s.ID = %s LIMIT 1",
            (sess["ID"],),
        )
        row = cur.fetchone()
    if not row:
        return {}
    paid_until = parse_db_datetime(row["PaidUntil"])
    minutes_left = int(row.get("MinutesLeft") or 0)
    renew_at = parse_db_datetime(row["RenewAt"])
    return {
        "session_token": row["SessionToken"],
        "npsso": row["NPSSO"],
        "account_label": row.get("AccountLabel"),
        "game_name": row.get("GameName"),
        "hourly_price": float(row.get("HourlyPrice") or DEFAULT_HOURLY),
        "paid_until": fmt_dt(paid_until) if paid_until else "",
        "minutes_left": minutes_left,
        "renew_at": fmt_dt(renew_at) if renew_at else "",
        "renew_soon": minutes_left <= int(RENEW_LEAD.total_seconds() // 60),
        "retention_until": str(row.get("RetentionUntil") or ""),
        "status": row["Status"],
        "stream_active": bool(row.get("StreamActive")),
        "ui_message": row.get("UiMessage") or "",
    }


def catalog_title_key(name):
    if not name:
        return ""
    name = name.lower().replace("(playstation plus)", "")
    return "".join(ch for ch in name if ch.isalnum())


def fallback_store_image_url(product_id):
    """Chihiro cover URL when CloudStreaming_Catalog.ImageUrl was never synced."""
    pid = (product_id or "").strip()
    if not pid:
        return ""
    # EU SKUs use EP/EE/EC; US-style UP / bare CUSA/PPSA → US store.
    if pid.upper().startswith(("EP", "EE", "EC")):
        country, lang = "GB", "en"
    else:
        country, lang = "US", "en"
    return (
        "https://store.playstation.com/store/api/chihiro/00_09_000/container/"
        "%s/%s/999/%s/image" % (country, lang, pid)
    )


def catalog_row_to_game(row):
    """Compact game object — must fit many rows in one UDP datagram (<= ~48KB)."""
    st = row["ServiceType"]
    stream_id = row["StreamIdentifier"]
    product_id = (row.get("ProductId") or stream_id or "").strip()
    platform = (row.get("Platform") or "unknown").lower()
    if platform == "unknown":
        platform = "ps4" if st == "psnow" else "ps5"
    category = (row.get("Category") or "streamable").lower()
    image = (row.get("ImageUrl") or "").strip()
    if not image:
        image = fallback_store_image_url(product_id)
    return {
        "productId": product_id,
        "name": row["Name"],
        "imageUrl": image,
        "category": category,
        "serviceType": st,
        "platform": platform,
        "isOwned": False,
        "streamServiceType": st,
        "streamIdentifier": stream_id,
        "entitlementId": (row.get("EntitlementId") or ""),
    }


def dedupe_catalog_games(rows):
    """Prefer psnow over pscloud when the normalized title matches (hourly rental)."""
    picked = {}
    extras = []
    for row in rows:
        game = catalog_row_to_game(row)
        key = catalog_title_key(game.get("name"))
        if not key:
            extras.append(game)
            continue
        prev = picked.get(key)
        if not prev:
            picked[key] = game
        elif prev.get("serviceType") == "pscloud" and game.get("serviceType") == "psnow":
            picked[key] = game
    games = list(picked.values()) + extras
    games.sort(key=lambda g: (g.get("name") or "").lower())
    return games


# Full catalog list is too large for one UDP datagram (EMSGSIZE / 64KB cap).
# Clients page with offset+limit; server caches the assembled list briefly.
_CATALOG_MEM = {"key": None, "ts": 0.0, "games": None}
CATALOG_MEM_TTL_SEC = int(os.environ.get("CS_CATALOG_CACHE_SEC", "300"))
CATALOG_DEFAULT_LIMIT = 80
CATALOG_MAX_LIMIT = 120
CATALOG_MAX_UDP_BYTES = 48000


def load_catalog_games(conn, service_type, platform, only_billable):
    key = (service_type or "*", platform or "*", bool(only_billable))
    now = time.time()
    cached = _CATALOG_MEM
    if (
        cached["games"] is not None
        and cached["key"] == key
        and (now - cached["ts"]) < CATALOG_MEM_TTL_SEC
    ):
        return cached["games"]

    sql = "SELECT c.* FROM CloudStreaming_Catalog c "
    if only_billable:
        sql += (
            "INNER JOIN CloudStreaming_Games g "
            "ON g.CatalogID = c.ID AND g.IsActive = 1 "
        )
    sql += "WHERE c.IsVisible = 1 "
    params = []
    if service_type in ("psnow", "pscloud"):
        sql += "AND c.ServiceType = %s "
        params.append(service_type)
    if platform in ("ps3", "ps4", "ps5", "unknown"):
        sql += "AND c.Platform = %s "
        params.append(platform)
    sql += "ORDER BY c.Name ASC"

    with conn.cursor() as cur:
        cur.execute(sql, params or None)
        rows = cur.fetchall()

    games = dedupe_catalog_games(rows)
    _CATALOG_MEM["key"] = key
    _CATALOG_MEM["ts"] = now
    _CATALOG_MEM["games"] = games
    return games


def handle_catalog(conn, req):
    """Paged catalog from MySQL (no player NPSSO). UDP cannot carry the full list."""
    req_id = req.get("id")
    service_type = (req.get("service_type") or "").strip().lower()
    platform = (req.get("platform") or "").strip().lower()
    only_billable = bool(req.get("only_billable"))
    try:
        offset = max(0, int(req.get("offset") or 0))
    except (TypeError, ValueError):
        offset = 0
    try:
        limit = int(req.get("limit") or CATALOG_DEFAULT_LIMIT)
    except (TypeError, ValueError):
        limit = CATALOG_DEFAULT_LIMIT
    limit = max(1, min(limit, CATALOG_MAX_LIMIT))

    games = load_catalog_games(conn, service_type, platform, only_billable)
    total = len(games)

    # Shrink page until the JSON fits a safe UDP payload size.
    page_limit = limit
    while True:
        page = games[offset : offset + page_limit]
        out = reply(
            req_id,
            True,
            games=page,
            totalGames=total,
            offset=offset,
            limit=page_limit,
            has_more=(offset + len(page)) < total,
            catalog_source="billing_db",
        )
        raw_len = len(json.dumps(out, ensure_ascii=False).encode("utf-8"))
        if raw_len <= CATALOG_MAX_UDP_BYTES or page_limit <= 10 or len(page) <= 10:
            break
        page_limit = max(10, page_limit // 2)

    log.info(
        "catalog: page offset=%s limit=%s size=%s/%s "
        "(filters: service=%s platform=%s only_billable=%s bytes=%s)",
        offset,
        page_limit,
        len(out.get("games") or []),
        total,
        service_type or "*",
        platform or "*",
        only_billable,
        raw_len,
    )
    return out


def handle_quote(conn, req):
    email = (req.get("email") or "").strip().lower()
    service_type = (req.get("service_type") or "").strip().lower()
    game_identifier = (req.get("game_identifier") or "").strip()
    game_name = (req.get("game_name") or game_identifier).strip()
    req_id = req.get("id")

    if not email:
        return reply(req_id, False, error="Укажите email (войдите в аккаунт 4cloud.pro)")
    if service_type not in ("psnow", "pscloud"):
        return reply(req_id, False, error="Неверный service_type")
    if not game_identifier:
        return reply(req_id, False, error="Не указана игра")

    ab = get_payment_method(conn, email)
    if not ab or not ab.get("StartPaymentID"):
        return reply(
            req_id,
            False,
            error=PAYMENT_SETUP_MSG,
            ui_message="Сначала привяжите карту для почасовой оплаты облачных игр.",
        )

    user = ensure_user(conn, email)
    game = ensure_game(conn, service_type, game_identifier, game_name)
    conn.commit()

    price = float(game["HourlyPrice"])
    resumable = find_resumable_session(
        conn, user["ID"], service_type, game_identifier, game["ID"], game
    )
    if resumable:
        payload = session_payload(conn, resumable)
        mins = payload.get("minutes_left", 0)
        return reply(
            req_id,
            True,
            ui_message=(
                "У вас осталось %s мин оплаченного времени в «%s». "
                "Дополнительное списание не требуется — можно продолжить игру."
                % (mins, game["Name"])
            ),
            hourly_price=0,
            currency=game.get("Currency") or "RUB",
            resume_session=True,
            no_charge=True,
            minutes_left=mins,
            game_name=game["Name"],
        )

    lease = find_active_lease(conn, user["ID"], game)
    reuse = lease is not None
    other_active = find_other_active_session(
        conn, user["ID"], service_type, game_identifier, game["ID"], game
    )

    if reuse:
        msg = (
            "Будет использован ваш сохранённый аккаунт PS (%s). "
            "Сейчас спишется %s ₽ за 1 час игры «%s»."
            % (lease.get("AccountLabel") or "аренда", int(price), game["Name"])
        )
        if other_active:
            msg += " Оставшееся оплаченное время на другой игре не переносится."
    else:
        msg = (
            "Сейчас спишется %s ₽ за 1 час игры «%s». "
            "Вам будет выделен PS-аккаунт с PS Plus. "
            "Сохранения останутся на этом аккаунте %s дней."
            % (int(price), game["Name"], RETENTION_DAYS)
        )
        if other_active:
            msg += " Оставшееся оплаченное время на другой игре не переносится."

    return reply(
        req_id,
        True,
        ui_message=msg,
        hourly_price=price,
        currency=game.get("Currency") or "RUB",
        reuse_account=reuse,
        resume_session=False,
        no_charge=False,
        game_name=game["Name"],
    )


def handle_start(conn, req):
    email = (req.get("email") or "").strip().lower()
    service_type = (req.get("service_type") or "").strip().lower()
    game_identifier = (req.get("game_identifier") or "").strip()
    game_name = (req.get("game_name") or game_identifier).strip()
    req_id = req.get("id")
    confirm = bool(req.get("confirm"))

    if not confirm:
        return reply(
            req_id,
            False,
            error="Требуется подтверждение (confirm=true)",
            ui_message="Подтвердите списание перед запуском игры.",
        )

    q = handle_quote(conn, {**req, "confirm": False})
    if not q.get("ok"):
        conn.rollback()
        return q

    user = ensure_user(conn, email)
    game = ensure_game(conn, service_type, game_identifier, game_name)
    price = float(game["HourlyPrice"])

    resumable = find_resumable_session(
        conn, user["ID"], service_type, game_identifier, game["ID"], game
    )
    if resumable:
        end_other_active_sessions(
            conn,
            user["ID"],
            except_session_id=resumable["ID"],
            service_type=service_type,
            game_identifier=game_identifier,
            game_id=game["ID"],
            game=game,
        )
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE CloudStreaming_Sessions SET StreamActive=1, Status='active', "
                "LastHeartbeatAt=NOW(3) WHERE ID=%s",
                (resumable["ID"],),
            )
        touch_lease(conn, resumable["LeaseID"])
        conn.commit()
        payload = session_payload(conn, resumable)
        mins = payload.get("minutes_left", 0)
        ui_msg = (
            "Продолжение сессии «%s». Осталось %s мин оплаченного времени."
            % (game["Name"], mins)
        )
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE CloudStreaming_Sessions SET UiMessage=%s WHERE ID=%s",
                (ui_msg, resumable["ID"]),
            )
        conn.commit()
        payload["resumed"] = True
        payload["ui_message"] = ui_msg
        return reply(req_id, True, **payload)

    # Switching game forfeits remaining paid time on other sessions only.
    end_other_active_sessions(
        conn,
        user["ID"],
        service_type=service_type,
        game_identifier=game_identifier,
        game_id=game["ID"],
        game=game,
    )

    lease = find_active_lease(conn, user["ID"], game)
    account = None
    if lease:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT * FROM CloudStreaming_Accounts WHERE ID = %s FOR UPDATE",
                (lease["AccountID"],),
            )
            account = cur.fetchone()
        touch_lease(conn, lease["ID"])
    else:
        account = allocate_account(conn, game)
        if not account:
            conn.rollback()
            return reply(
                req_id,
                False,
                error="Нет свободных PS-аккаунтов для этой игры. Попробуйте позже.",
                ui_message="Все аккаунты заняты. Ожидайте освобождения или выберите другую игру.",
            )
        now = datetime.now()
        retention = now + timedelta(days=RETENTION_DAYS)
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO CloudStreaming_Leases "
                "(UserID, AccountID, Status, FirstAssignedAt, LastActivityAt, RetentionUntil) "
                "VALUES (%s, %s, 'active', %s, %s, %s)",
                (user["ID"], account["ID"], fmt_dt(now), fmt_dt(now), fmt_dt(retention)),
            )
            lease_id = cur.lastrowid
            cur.execute(
                "UPDATE CloudStreaming_Accounts SET Status='leased', CurrentLeaseID=%s, "
                "LastUsedAt=NOW(3) WHERE ID=%s",
                (lease_id, account["ID"]),
            )
            cur.execute("SELECT * FROM CloudStreaming_Leases WHERE ID=%s", (lease_id,))
            lease = cur.fetchone()

    token = str(uuid.uuid4())
    now = datetime.now()
    paid_until = now + timedelta(hours=1)
    renew_at = paid_until - RENEW_LEAD
    ui_msg = (
        "Подготовка к запуску «%s». Списание %s ₽ произойдёт только после успешного "
        "подключения к игре."
        % (game["Name"], int(price))
    )

    with conn.cursor() as cur:
        cur.execute(
            "INSERT INTO CloudStreaming_Sessions "
            "(SessionToken, UserID, LeaseID, AccountID, GameID, ServiceType, GameIdentifier, "
            "Status, BlockNo, BlockStartedAt, PaidUntil, RenewAt, StreamActive, UiMessage) "
            "VALUES (%s,%s,%s,%s,%s,%s,%s,'pending_payment',1,%s,%s,%s,0,%s)",
            (
                token,
                user["ID"],
                lease["ID"],
                account["ID"],
                game["ID"],
                service_type,
                game_identifier,
                fmt_dt(now),
                fmt_dt(paid_until),
                fmt_dt(renew_at),
                ui_msg,
            ),
        )
        session_id = cur.lastrowid
        cur.execute("SELECT * FROM CloudStreaming_Sessions WHERE ID=%s", (session_id,))
        sess = cur.fetchone()

    touch_lease(conn, lease["ID"])
    conn.commit()

    payload = session_payload(conn, sess)
    payload["payment_pending"] = True
    payload["hourly_price"] = price
    payload["ui_message"] = ui_msg
    return reply(req_id, True, **payload)


def handle_confirm_stream(conn, req):
    """Charge the reserved hour after Gaikai allocation succeeded."""
    token = (req.get("session_token") or "").strip()
    email = (req.get("email") or "").strip().lower()
    req_id = req.get("id")

    with conn.cursor() as cur:
        cur.execute(
            "SELECT s.*, g.Name AS GameName, g.HourlyPrice "
            "FROM CloudStreaming_Sessions s "
            "JOIN CloudStreaming_Games g ON g.ID = s.GameID "
            "WHERE s.SessionToken=%s LIMIT 1 FOR UPDATE",
            (token,),
        )
        sess = cur.fetchone()
    if not sess:
        conn.rollback()
        return reply(req_id, False, error="Сессия не найдена")

    if sess["Status"] in ("active", "grace_no_stream", "renewal_pending"):
        conn.commit()
        payload = session_payload(conn, sess)
        payload["already_active"] = True
        return reply(req_id, True, **payload)

    if sess["Status"] != "pending_payment":
        conn.rollback()
        return reply(req_id, False, error="Сессия не ожидает подтверждения", status=sess["Status"])

    price = float(sess["HourlyPrice"])
    block_no = int(sess["BlockNo"])
    idem = "cs-%s-b%s" % (token, block_no)
    ok_pay, pay_msg = charge_hour(
        conn, email, sess["ID"], sess["UserID"], block_no, price, sess["GameName"], idem
    )
    if not ok_pay:
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE CloudStreaming_Sessions SET Status='failed', EndedAt=NOW(3), "
                "EndReason='payment_failed', UiMessage=%s WHERE ID=%s",
                (pay_msg, sess["ID"]),
            )
        conn.commit()
        return reply(req_id, False, error=pay_msg, ui_message=pay_msg)

    with conn.cursor() as cur:
        cur.execute(
            "UPDATE CloudStreaming_Sessions SET Status='active', StreamActive=1, "
            "LastHeartbeatAt=NOW(3), UiMessage=%s WHERE ID=%s",
            ("Оплата прошла. Игра запущена.", sess["ID"]),
        )
        cur.execute("SELECT * FROM CloudStreaming_Sessions WHERE ID=%s", (sess["ID"],))
        sess = cur.fetchone()
    touch_lease(conn, sess["LeaseID"])
    conn.commit()

    payload = session_payload(conn, sess)
    payload["ui_message"] = (
        "Списано %s ₽. Играйте до %s. За 10 минут до конца при активном стриме "
        "спишется следующий час автоматически."
        % (int(price), payload.get("paid_until", ""))
    )
    return reply(req_id, True, **payload)


def handle_heartbeat(conn, req):
    token = (req.get("session_token") or "").strip()
    streaming = bool(req.get("streaming", True))
    req_id = req.get("id")
    if not token:
        return reply(req_id, False, error="session_token required")

    with conn.cursor() as cur:
        cur.execute(
            "SELECT *, TIMESTAMPDIFF(SECOND, NOW(3), PaidUntil) AS SecsLeft "
            "FROM CloudStreaming_Sessions WHERE SessionToken=%s LIMIT 1 FOR UPDATE",
            (token,),
        )
        sess = cur.fetchone()
    if not sess:
        conn.rollback()
        return reply(req_id, False, error="Сессия не найдена")

    if sess["Status"] not in ("active", "grace_no_stream", "renewal_pending"):
        conn.rollback()
        if sess["Status"] == "pending_payment":
            return reply(
                req_id,
                False,
                error="Ожидается подтверждение запуска",
                ui_message="Списание ещё не выполнено — дождитесь подключения к игре.",
                status=sess["Status"],
            )
        return reply(req_id, False, error="Сессия завершена", status=sess["Status"])

    if int(sess.get("SecsLeft") or 0) <= 0:
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE CloudStreaming_Sessions SET Status='ended', EndedAt=NOW(3), "
                "EndReason='time_expired', StreamActive=0 WHERE ID=%s",
                (sess["ID"],),
            )
        conn.commit()
        return reply(
            req_id,
            False,
            error="Оплаченный час истёк",
            ui_message="Время сессии закончилось. Запустите игру снова для новой оплаты.",
            minutes_left=0,
        )

    new_status = "active" if streaming else "grace_no_stream"
    with conn.cursor() as cur:
        cur.execute(
            "UPDATE CloudStreaming_Sessions SET StreamActive=%s, LastHeartbeatAt=NOW(3), "
            "Status=%s WHERE ID=%s",
            (1 if streaming else 0, new_status, sess["ID"]),
        )
        cur.execute("SELECT * FROM CloudStreaming_Sessions WHERE ID=%s", (sess["ID"],))
        sess = cur.fetchone()

    touch_lease(conn, sess["LeaseID"])
    conn.commit()

    payload = session_payload(conn, sess)
    if payload.get("renew_soon") and streaming:
        payload["ui_message"] = (
            "Осталось %s мин. При активном стриме скоро спишется следующий час."
            % payload.get("minutes_left", 0)
        )
        payload["should_renew"] = True
    else:
        payload["should_renew"] = False
    return reply(req_id, True, **payload)


def handle_renew(conn, req):
    token = (req.get("session_token") or "").strip()
    email = (req.get("email") or "").strip().lower()
    req_id = req.get("id")

    with conn.cursor() as cur:
        cur.execute(
            "SELECT s.*, g.Name AS GameName, g.HourlyPrice "
            "FROM CloudStreaming_Sessions s "
            "JOIN CloudStreaming_Games g ON g.ID = s.GameID "
            "WHERE s.SessionToken=%s LIMIT 1 FOR UPDATE",
            (token,),
        )
        sess = cur.fetchone()
    if not sess:
        conn.rollback()
        return reply(req_id, False, error="Сессия не найдена")
    if not sess.get("StreamActive"):
        conn.rollback()
        return reply(
            req_id,
            False,
            error="Продление только при активном стриме",
            ui_message="Запустите стрим снова, чтобы продлить сессию.",
        )

    block_no = int(sess["BlockNo"]) + 1
    price = float(sess["HourlyPrice"])
    idem = "cs-%s-b%s" % (token, block_no)

    ok_pay, pay_msg = charge_hour(
        conn, email, sess["ID"], sess["UserID"], block_no, price, sess["GameName"], idem
    )
    if not ok_pay:
        conn.commit()
        return reply(req_id, False, error=pay_msg, ui_message=pay_msg)

    now = datetime.now()
    current_paid_until = parse_db_datetime(sess["PaidUntil"])
    # Stack the new hour on any remaining paid time (e.g. 8 min left + 60 min = 68 min).
    base = current_paid_until if current_paid_until and current_paid_until > now else now
    paid_until = base + timedelta(hours=1)
    renew_at = paid_until - RENEW_LEAD
    minutes_left = max(0, int((paid_until - now).total_seconds() + 59) // 60)
    with conn.cursor() as cur:
        cur.execute(
            "UPDATE CloudStreaming_Sessions SET BlockNo=%s, BlockStartedAt=%s, "
            "PaidUntil=%s, RenewAt=%s, Status='active', UiMessage=%s WHERE ID=%s",
            (
                block_no,
                fmt_dt(now),
                fmt_dt(paid_until),
                fmt_dt(renew_at),
                "Продлено ещё на 1 час (%s ₽). Всего осталось %s мин."
                % (int(price), minutes_left),
                sess["ID"],
            ),
        )
    conn.commit()
    with conn.cursor() as cur:
        cur.execute("SELECT * FROM CloudStreaming_Sessions WHERE ID=%s", (sess["ID"],))
        sess = cur.fetchone()
    payload = session_payload(conn, sess)
    return reply(req_id, True, **payload)


def handle_end_stream(conn, req):
    token = (req.get("session_token") or "").strip()
    req_id = req.get("id")
    with conn.cursor() as cur:
        cur.execute(
            "SELECT * FROM CloudStreaming_Sessions WHERE SessionToken=%s LIMIT 1 FOR UPDATE",
            (token,),
        )
        sess = cur.fetchone()
    if not sess:
        conn.rollback()
        return reply(req_id, False, error="Сессия не найдена")

    if sess["Status"] == "pending_payment":
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE CloudStreaming_Sessions SET Status='ended', EndedAt=NOW(3), "
                "EndReason='payment_aborted', StreamActive=0, UiMessage=%s WHERE ID=%s",
                ("Запуск не удался — списание не выполнялось.", sess["ID"]),
            )
        conn.commit()
        return reply(
            req_id,
            True,
            ui_message="Сессия отменена без списания.",
            payment_aborted=True,
        )

    with conn.cursor() as cur:
        cur.execute(
            "UPDATE CloudStreaming_Sessions SET StreamActive=0, Status='grace_no_stream', "
            "UiMessage=%s WHERE ID=%s",
            (
                "Стрим приостановлен. Можно вернуться в эту же игру до %s."
                % str(sess["PaidUntil"]),
                sess["ID"],
            ),
        )
        cur.execute(
            "UPDATE CloudStreaming_Leases SET Status='retention' WHERE ID=%s",
            (sess["LeaseID"],),
        )
    conn.commit()
    return reply(
        req_id,
        True,
        ui_message=(
            "Стрим остановлен. Оплаченное время действует до %s — "
            "запустите ту же игру снова, чтобы продолжить без новой оплаты."
            % str(sess["PaidUntil"])
        ),
    )


def handle_status(conn, req):
    token = (req.get("session_token") or "").strip()
    req_id = req.get("id")
    with conn.cursor() as cur:
        cur.execute(
            "SELECT * FROM CloudStreaming_Sessions WHERE SessionToken=%s LIMIT 1",
            (token,),
        )
        sess = cur.fetchone()
    if not sess:
        return reply(req_id, False, error="Сессия не найдена")
    return reply(req_id, True, **session_payload(conn, sess))


def dispatch(payload):
    req_id = payload.get("id")
    action = (payload.get("action") or "").strip().lower()
    if action == "ping":
        return reply(req_id, True, message="pong", server_time=fmt_dt(datetime.now()))

    conn = db_connect()
    try:
        if action == "quote":
            out = handle_quote(conn, payload)
        elif action == "catalog":
            out = handle_catalog(conn, payload)
        elif action == "confirm_stream":
            out = handle_confirm_stream(conn, payload)
        elif action == "start":
            out = handle_start(conn, payload)
        elif action == "heartbeat":
            out = handle_heartbeat(conn, payload)
        elif action == "renew":
            out = handle_renew(conn, payload)
        elif action == "end_stream":
            out = handle_end_stream(conn, payload)
        elif action == "status":
            out = handle_status(conn, payload)
        else:
            out = reply(req_id, False, error="unknown action: %s" % action)
        if not conn.get_autocommit():
            conn.commit()
        return out
    except Exception as e:
        log.exception("dispatch error action=%s", action)
        try:
            conn.rollback()
        except Exception:
            pass
        return reply(req_id, False, error=friendly_db_error(e))
    finally:
        conn.close()


def expire_leases_job():
    while True:
        try:
            conn = db_connect()
            with conn.cursor() as cur:
                cur.execute(
                    "UPDATE CloudStreaming_Leases SET Status='expired', ReleasedAt=NOW(3), "
                    "ReleaseReason='inactivity_3d' "
                    "WHERE Status IN ('active','retention') AND RetentionUntil < NOW(3)"
                )
                cur.execute(
                    "UPDATE CloudStreaming_Accounts a "
                    "JOIN CloudStreaming_Leases l ON l.ID = a.CurrentLeaseID "
                    "SET a.Status='available', a.CurrentLeaseID=NULL "
                    "WHERE l.Status='expired' AND a.Status='leased'"
                )
                cur.execute(
                    "UPDATE CloudStreaming_Sessions SET Status='ended', EndedAt=NOW(3), "
                    "EndReason='lease_expired', StreamActive=0 "
                    "WHERE Status IN ('active','grace_no_stream','renewal_pending') "
                    "AND LeaseID IN (SELECT ID FROM CloudStreaming_Leases WHERE Status='expired')"
                )
                cur.execute(
                    "UPDATE CloudStreaming_Sessions SET Status='ended', EndedAt=NOW(3), "
                    "EndReason='payment_aborted', StreamActive=0 "
                    "WHERE Status='pending_payment' AND BlockStartedAt < DATE_SUB(NOW(3), INTERVAL 20 MINUTE)"
                )
            conn.commit()
            conn.close()
        except Exception as e:
            log.exception("expire_leases_job: %s", e)
        time.sleep(60)


def auto_renew_job():
    while True:
        try:
            conn = db_connect()
            with conn.cursor() as cur:
                cur.execute(
                    "SELECT s.SessionToken, u.User AS Email "
                    "FROM CloudStreaming_Sessions s "
                    "JOIN CloudStreaming_Users u ON u.ID = s.UserID "
                    "WHERE s.Status='active' AND s.StreamActive=1 AND s.RenewAt <= NOW(3)"
                )
                rows = cur.fetchall()
            conn.close()
            for row in rows:
                dispatch(
                    {
                        "id": str(uuid.uuid4()),
                        "action": "renew",
                        "session_token": row["SessionToken"],
                        "email": row["Email"],
                    }
                )
        except Exception as e:
            log.exception("auto_renew_job: %s", e)
        time.sleep(30)


def init_config():
    """Load required secrets from environment (or .env next to this script)."""
    global DB_HOST, DB_NAME, DB_USER, DB_PASS, MRH_LOGIN, PASS1, PASS2
    load_env_file()
    DB_HOST = os.environ.get("CS_DB_HOST", "127.0.0.1").strip() or "127.0.0.1"
    DB_NAME = require_env("CS_DB_NAME")
    DB_USER = require_env("CS_DB_USER")
    DB_PASS = require_env("CS_DB_PASS")
    MRH_LOGIN = require_env("CS_MRH_LOGIN")
    PASS1 = require_env("CS_ROBOKASSA_PASS1")
    PASS2 = require_env("CS_ROBOKASSA_PASS2")


def main():
    init_config()
    try:
        conn = db_connect()
        verify_schema(conn)
        conn.close()
        log.info("Database schema OK")
    except Exception as e:
        log.error("Schema check failed: %s", e)
        raise SystemExit(1) from e
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((UDP_HOST, UDP_PORT))
    log.info("Cloud billing UDP listening on %s:%s", UDP_HOST, UDP_PORT)

    threading.Thread(target=expire_leases_job, daemon=True).start()
    threading.Thread(target=auto_renew_job, daemon=True).start()

    while True:
        data, addr = sock.recvfrom(65535)
        try:
            payload = json.loads(data.decode("utf-8"))
        except Exception as e:
            log.warning("bad json from %s: %s", addr, e)
            continue
        log.info("REQ %s from %s action=%s", payload.get("id"), addr, payload.get("action"))
        out = dispatch(payload)
        try:
            sock.sendto(json.dumps(out, ensure_ascii=False).encode("utf-8"), addr)
        except Exception as e:
            log.error("sendto %s: %s", addr, e)


if __name__ == "__main__":
    main()
