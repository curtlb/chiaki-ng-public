#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Cloud Streaming billing UDP server for 4cloud.pro + chiaki-ng.

JSON over UDP (request/response share the same "id" field).
Default bind: 0.0.0.0:13750

Actions:
  ping, quote, start, heartbeat, renew, end_stream, status

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
from datetime import datetime, timedelta
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


def verify_schema(conn):
    """Fail fast at startup when the catalog migration was not applied."""
    required = [
        ("CloudStreaming_Catalog", "ID"),
        ("CloudStreaming_Games", "CatalogID"),
        ("CloudStreaming_AccountOwnedGames", "CatalogID"),
        ("CloudStreaming_PaymentMethods", "StartPaymentID"),
    ]
    missing = []
    with conn.cursor() as cur:
        for table, column in required:
            cur.execute(
                "SELECT COUNT(*) AS n FROM information_schema.COLUMNS "
                "WHERE TABLE_SCHEMA = %s AND TABLE_NAME = %s AND COLUMN_NAME = %s",
                (DB_NAME, table, column),
            )
            if int(cur.fetchone()["n"]) == 0:
                missing.append("%s.%s" % (table, column))
    if missing:
        raise RuntimeError(
            "Cloud billing schema is outdated (missing: %s). Run migrate_catalog.sql "
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
            return row
        catalog = find_catalog_match(conn, st, gid)
        code = re.sub(r"[^a-zA-Z0-9_]+", "_", (game_name or (catalog or {}).get("Name") or gid))[:60].lower()
        access = "ps_plus" if st == "psnow" else "owned_only"
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


def find_active_lease(conn, user_id, game):
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


def end_other_active_sessions(conn, user_id, except_session_id=None):
    with conn.cursor() as cur:
        cur.execute(
            "SELECT ID, SessionToken FROM CloudStreaming_Sessions "
            "WHERE UserID = %s AND Status IN ('active','grace_no_stream','renewal_pending')",
            (user_id,),
        )
        rows = cur.fetchall()
    for row in rows:
        if except_session_id and row["ID"] == except_session_id:
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
            "l.RetentionUntil "
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
    now = datetime.now()
    paid_until = row["PaidUntil"]
    if isinstance(paid_until, str):
        paid_until = datetime.strptime(paid_until[:19], DATE_FMT)
    minutes_left = max(0, int((paid_until - now).total_seconds() // 60))
    renew_at = row["RenewAt"]
    if isinstance(renew_at, str):
        renew_at = datetime.strptime(renew_at[:19], DATE_FMT)
    return {
        "session_token": row["SessionToken"],
        "npsso": row["NPSSO"],
        "account_label": row.get("AccountLabel"),
        "game_name": row.get("GameName"),
        "hourly_price": float(row.get("HourlyPrice") or DEFAULT_HOURLY),
        "paid_until": fmt_dt(paid_until),
        "minutes_left": minutes_left,
        "renew_at": fmt_dt(renew_at),
        "renew_soon": minutes_left <= int(RENEW_LEAD.total_seconds() // 60),
        "retention_until": str(row.get("RetentionUntil") or ""),
        "status": row["Status"],
        "stream_active": bool(row.get("StreamActive")),
        "ui_message": row.get("UiMessage") or "",
    }


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
    lease = find_active_lease(conn, user["ID"], game)
    reuse = lease is not None

    if reuse:
        msg = (
            "Будет использован ваш сохранённый аккаунт PS (%s). "
            "Сейчас спишется %s ₽ за 1 час игры «%s». "
            "Неиспользованное время с прошлой сессии не переносится."
            % (lease.get("AccountLabel") or "аренда", int(price), game["Name"])
        )
    else:
        msg = (
            "Сейчас спишется %s ₽ за 1 час игры «%s». "
            "Вам будет выделен PS-аккаунт с PS Plus. "
            "Сохранения останутся на этом аккаунте %s дней."
            % (int(price), game["Name"], RETENTION_DAYS)
        )

    return reply(
        req_id,
        True,
        ui_message=msg,
        hourly_price=price,
        currency=game.get("Currency") or "RUB",
        reuse_account=reuse,
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

    # Switching game forfeits remaining paid time on other sessions
    end_other_active_sessions(conn, user["ID"])

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
    renew_at = paid_until - RENE_LEAD
    ui_msg = (
        "Оплата за 1 час игры «%s». Списание %s ₽ с привязанной карты…"
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

    idem = "cs-%s-b1" % token
    ok_pay, pay_msg = charge_hour(
        conn, email, session_id, user["ID"], 1, price, game["Name"], idem
    )
    if not ok_pay:
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE CloudStreaming_Sessions SET Status='failed', EndedAt=NOW(3), "
                "EndReason='payment_failed', UiMessage=%s WHERE ID=%s",
                (pay_msg, session_id),
            )
        conn.commit()
        return reply(req_id, False, error=pay_msg, ui_message=pay_msg)

    with conn.cursor() as cur:
        cur.execute(
            "UPDATE CloudStreaming_Sessions SET Status='active', StreamActive=1, "
            "LastHeartbeatAt=NOW(3), UiMessage=%s WHERE ID=%s",
            ("Оплата прошла. Запускаем облачный стрим…", session_id),
        )
        cur.execute("SELECT * FROM CloudStreaming_Sessions WHERE ID=%s", (session_id,))
        sess = cur.fetchone()

    touch_lease(conn, lease["ID"])
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
            "SELECT * FROM CloudStreaming_Sessions WHERE SessionToken=%s LIMIT 1 FOR UPDATE",
            (token,),
        )
        sess = cur.fetchone()
    if not sess:
        conn.rollback()
        return reply(req_id, False, error="Сессия не найдена")

    if sess["Status"] not in ("active", "grace_no_stream", "renewal_pending"):
        conn.rollback()
        return reply(req_id, False, error="Сессия завершена", status=sess["Status"])

    now = datetime.now()
    paid_until = sess["PaidUntil"]
    if isinstance(paid_until, str):
        paid_until = datetime.strptime(paid_until[:19], DATE_FMT)

    if now >= paid_until:
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
    paid_until = now + timedelta(hours=1)
    renew_at = paid_until - RENE_LEAD
    with conn.cursor() as cur:
        cur.execute(
            "UPDATE CloudStreaming_Sessions SET BlockNo=%s, BlockStartedAt=%s, "
            "PaidUntil=%s, RenewAt=%s, Status='active', UiMessage=%s WHERE ID=%s",
            (
                block_no,
                fmt_dt(now),
                fmt_dt(paid_until),
                fmt_dt(renew_at),
                "Продлено ещё на 1 час (%s ₽)" % int(price),
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

    with conn.cursor() as cur:
        cur.execute(
            "UPDATE CloudStreaming_Sessions SET StreamActive=0, Status='grace_no_stream', "
            "UiMessage=%s WHERE ID=%s",
            (
                "Стрим остановлен. Оплаченное время продолжает идти до %s."
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
        ui_message="Стрим завершён. Оставшееся оплаченное время не переносится на другую игру.",
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
