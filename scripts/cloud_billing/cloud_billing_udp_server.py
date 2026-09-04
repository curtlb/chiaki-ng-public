#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Cloud Streaming billing UDP server for 4cloud.pro + chiaki-ng.

JSON over UDP (request/response share the same "id" field).
Default bind: 0.0.0.0:13750

Actions:
  ping, catalog, catalog_npsso, quote, start, confirm_stream, heartbeat, renew,
  end_stream, status, whoami

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
import struct
import sys
import threading
import time
import base64
import zlib
import urllib.parse
import uuid
from datetime import datetime, timedelta, date
from pathlib import Path

try:
    from zoneinfo import ZoneInfo
except ImportError:
    ZoneInfo = None

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
TCP_PORT = int(os.environ.get("CS_BILLING_TCP_PORT", str(UDP_PORT + 1)))
RETENTION_DAYS = int(os.environ.get("CS_RETENTION_DAYS", "3"))
SAVE_FREEZE_THRESHOLD_MIN = int(os.environ.get("CS_SAVE_FREEZE_MINUTES", "61"))
SAVE_INITIAL_RETENTION_HOURS = int(os.environ.get("CS_SAVE_INITIAL_HOURS", "48"))
SAVE_EXTEND_THRESHOLD_MIN = int(os.environ.get("CS_SAVE_EXTEND_MINUTES", "90"))
MSK_TZ = ZoneInfo("Europe/Moscow") if ZoneInfo else None
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


def msk_now():
    """Current time in Europe/Moscow (naive datetime for display/storage)."""
    if MSK_TZ:
        return datetime.now(MSK_TZ).replace(tzinfo=None)
    return datetime.now()


def msk_today():
    return msk_now().date()


def fmt_dt_msk(dt):
    if not dt:
        return ""
    return fmt_dt(dt) + " (МСК)"


def lease_valid_sql():
    """Lease row still usable for streaming (freeze optional)."""
    return (
        "l.Status IN ('active','retention') "
        "AND (l.SaveFreezeActive = 0 OR l.RetentionUntil > NOW(3))"
    )


def ensure_daily_play_row(conn, user_id, play_date):
    with conn.cursor() as cur:
        cur.execute(
            "INSERT IGNORE INTO CloudStreaming_DailyPlay "
            "(UserID, PlayDateMSK, StreamSeconds, ExtensionGranted) VALUES (%s, %s, 0, 0)",
            (user_id, play_date),
        )
        cur.execute(
            "SELECT * FROM CloudStreaming_DailyPlay "
            "WHERE UserID=%s AND PlayDateMSK=%s FOR UPDATE",
            (user_id, play_date),
        )
        return cur.fetchone()


def fetch_lease_row(conn, lease_id, for_update=False):
    lock = " FOR UPDATE" if for_update else ""
    with conn.cursor() as cur:
        cur.execute(
            "SELECT * FROM CloudStreaming_Leases WHERE ID=%s" + lock,
            (lease_id,),
        )
        return cur.fetchone()


def is_lease_first_msk_day(lease):
    """True on the MSK calendar day when the PS account was first assigned."""
    first = parse_db_datetime((lease or {}).get("FirstAssignedAt"))
    if not first:
        return False
    return first.date() == msk_today()


def apply_save_retention_rules(conn, user_id, lease_id, stream_seconds_today, daily_row):
    """First MSK day: 48h freeze at 61 min. Later days: +1 day at 90 min if freeze already active."""
    lease = fetch_lease_row(conn, lease_id, for_update=True)
    if not lease:
        return lease
    played_min = int(stream_seconds_today) // 60
    now = datetime.now()
    freeze_active = bool(int(lease.get("SaveFreezeActive") or 0))
    retention = parse_db_datetime(lease.get("RetentionUntil"))
    ext_granted = bool(int(daily_row.get("ExtensionGranted") or 0))
    first_day = is_lease_first_msk_day(lease)
    new_freeze = freeze_active
    new_retention = retention

    if not freeze_active and first_day and played_min >= SAVE_FREEZE_THRESHOLD_MIN:
        new_freeze = True
        new_retention = now + timedelta(hours=SAVE_INITIAL_RETENTION_HOURS)

    if new_freeze and played_min >= SAVE_EXTEND_THRESHOLD_MIN and not ext_granted:
        base = new_retention if new_retention and new_retention > now else now
        new_retention = base + timedelta(days=1)
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE CloudStreaming_DailyPlay SET ExtensionGranted=1 "
                "WHERE UserID=%s AND PlayDateMSK=%s",
                (user_id, daily_row["PlayDateMSK"]),
            )
            daily_row["ExtensionGranted"] = 1

    if new_freeze != freeze_active or (new_retention and new_retention != retention):
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE CloudStreaming_Leases SET SaveFreezeActive=%s, RetentionUntil=%s, "
                "UpdatedAt=NOW(3) WHERE ID=%s",
                (1 if new_freeze else 0, fmt_dt(new_retention), lease_id),
            )
    return fetch_lease_row(conn, lease_id)


def record_stream_play_minutes(conn, user_id, lease_id, minutes):
    """Accumulate active-stream minutes for the current MSK calendar day."""
    if not user_id or not lease_id or minutes <= 0:
        return fetch_lease_row(conn, lease_id)
    play_date = msk_today()
    daily = ensure_daily_play_row(conn, user_id, play_date)
    new_seconds = int(daily.get("StreamSeconds") or 0) + int(minutes) * 60
    with conn.cursor() as cur:
        cur.execute(
            "UPDATE CloudStreaming_DailyPlay SET StreamSeconds=%s "
            "WHERE UserID=%s AND PlayDateMSK=%s",
            (new_seconds, user_id, play_date),
        )
    daily["StreamSeconds"] = new_seconds
    return apply_save_retention_rules(conn, user_id, lease_id, new_seconds, daily)


def build_save_retention_payload(conn, user_id, lease_id):
    """Structured fields + dialog text for post-stream save freeze UI."""
    lease = fetch_lease_row(conn, lease_id) if lease_id else None
    play_date = msk_today()
    daily = None
    if user_id:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT * FROM CloudStreaming_DailyPlay WHERE UserID=%s AND PlayDateMSK=%s",
                (user_id, play_date),
            )
            daily = cur.fetchone()
    played_min = int((daily or {}).get("StreamSeconds") or 0) // 60
    freeze_active = bool(int((lease or {}).get("SaveFreezeActive") or 0))
    retention_until = parse_db_datetime((lease or {}).get("RetentionUntil")) if lease else None
    ext_granted = bool(int((daily or {}).get("ExtensionGranted") or 0))
    first_day = is_lease_first_msk_day(lease)
    mins_to_freeze = max(0, SAVE_FREEZE_THRESHOLD_MIN - played_min) if first_day else 0
    mins_to_extend = (
        0 if ext_granted else max(0, SAVE_EXTEND_THRESHOLD_MIN - played_min)
    )

    if not freeze_active:
        if first_day and mins_to_freeze > 0:
            message = (
                "Заморозка сохранений не произойдёт.\n\n"
                "Чтобы ваши сохранения хранились %s часов после игры, "
                "нужно отыграть ещё %s мин. сегодня (по московскому времени).\n"
                "Сегодня отыграно: %s мин из %s мин.\n"
                "Первоначальная заморозка доступна только в первый день после выдачи аккаунта."
                % (
                    SAVE_INITIAL_RETENTION_HOURS,
                    mins_to_freeze,
                    played_min,
                    SAVE_FREEZE_THRESHOLD_MIN,
                )
            )
        elif first_day:
            message = (
                "Заморозка сохранений будет активирована после достижения порога "
                "(%s мин за первый день по МСК)." % SAVE_FREEZE_THRESHOLD_MIN
            )
        else:
            message = (
                "Заморозка сохранений не активна.\n\n"
                "Первоначальная заморозка (48 ч) доступна только в первый день "
                "после выдачи аккаунта (не менее %s мин. игры за этот день по МСК)."
                % SAVE_FREEZE_THRESHOLD_MIN
            )
    else:
        until_str = fmt_dt_msk(retention_until)
        parts = ["Сохранения заморожены до:\n%s." % until_str]
        if ext_granted:
            parts.append(
                "Продление хранения на +1 день за сегодня уже получено "
                "(не более одного раза в сутки по МСК)."
            )
        elif mins_to_extend > 0:
            parts.append(
                "Чтобы продлить хранение сохранений ещё на 1 день, "
                "отыграйте ещё %s мин. сегодня (по московскому времени).\n"
                "Сегодня отыграно: %s мин из %s мин."
                % (mins_to_extend, played_min, SAVE_EXTEND_THRESHOLD_MIN)
            )
        else:
            parts.append(
                "Порог для продления на +1 день достигнут — срок хранения будет обновлён."
            )
        message = "\n\n".join(parts)

    return {
        "save_freeze_active": freeze_active,
        "save_retention_until": fmt_dt(retention_until) if retention_until else "",
        "daily_play_minutes": played_min,
        "minutes_to_freeze": mins_to_freeze,
        "minutes_to_extend": mins_to_extend,
        "extension_granted_today": ext_granted,
        "save_retention_message": message,
    }


def sql_minutes_left_expr():
    """Remaining paid minutes derived from PaidUntil (read-only, no wallet columns)."""
    return "GREATEST(0, TIMESTAMPDIFF(MINUTE, NOW(3), s.PaidUntil))"


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


def session_minutes_left(sess, now=None):
    """Compute remaining minutes from PaidUntil without writing to DB."""
    now = now or datetime.now()
    paid_until = parse_db_datetime(sess.get("PaidUntil"))
    if not paid_until or paid_until <= now:
        return 0
    return max(0, int((paid_until - now).total_seconds() // 60))


def session_effective_paid_until(sess, now=None):
    """Best PaidUntil including legacy Plus/Owned minute columns during migration."""
    now = now or datetime.now()
    best = parse_db_datetime(sess.get("PaidUntil"))
    legacy_mins = int(sess.get("PlusMinutesLeft") or 0) + int(
        sess.get("OwnedMinutesLeft") or 0
    )
    if legacy_mins > 0:
        legacy_until = now + timedelta(minutes=legacy_mins)
        if not best or legacy_until > best:
            best = legacy_until
    return best


def sync_renew_at(conn, sess_id):
    renew_min = int(RENEW_LEAD.total_seconds() // 60)
    with conn.cursor() as cur:
        cur.execute(
            "UPDATE CloudStreaming_Sessions SET RenewAt = "
            "GREATEST(NOW(3), DATE_SUB(PaidUntil, INTERVAL %s MINUTE)) WHERE ID=%s",
            (renew_min, sess_id),
        )


def credit_paid_minutes(conn, sess_id, minutes):
    """Extend PaidUntil by `minutes` from max(PaidUntil, now)."""
    minutes = max(0, int(minutes))
    with conn.cursor() as cur:
        cur.execute(
            "UPDATE CloudStreaming_Sessions SET "
            "PaidUntil = DATE_ADD(GREATEST(COALESCE(PaidUntil, NOW(3)), NOW(3)), "
            "INTERVAL %s MINUTE), "
            "PlusMinutesLeft = 0, OwnedMinutesLeft = 0 "
            "WHERE ID=%s",
            (minutes, sess_id),
        )
    sync_renew_at(conn, sess_id)
    with conn.cursor() as cur:
        cur.execute("SELECT * FROM CloudStreaming_Sessions WHERE ID=%s LIMIT 1", (sess_id,))
        return cur.fetchone()


def migrate_legacy_wallets_to_paid_until(conn):
    """One-time: merge Plus/Owned minute columns into PaidUntil."""
    with conn.cursor() as cur:
        cur.execute("SELECT * FROM CloudStreaming_Sessions")
        rows = cur.fetchall()
    now = datetime.now()
    for sess in rows:
        effective = session_effective_paid_until(sess, now)
        legacy = int(sess.get("PlusMinutesLeft") or 0) + int(
            sess.get("OwnedMinutesLeft") or 0
        )
        current = parse_db_datetime(sess.get("PaidUntil"))
        needs = legacy > 0 or (
            effective and (not current or effective > current)
        )
        if not needs:
            continue
        if not effective or effective <= now:
            effective = now
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE CloudStreaming_Sessions SET PaidUntil=%s, "
                "PlusMinutesLeft=0, OwnedMinutesLeft=0 WHERE ID=%s",
                (fmt_dt(effective), sess["ID"]),
            )
        sync_renew_at(conn, sess["ID"])


def tick_session_balance(conn, sess):
    """Burn paid time only while the stream is active (updates PaidUntil on tick)."""
    if not sess:
        return sess
    if not int(sess.get("StreamActive") or 0):
        return sess
    if sess.get("Status") == "pending_payment":
        return sess
    now = datetime.now()
    tick_at = parse_db_datetime(sess.get("BalanceTickAt")) or parse_db_datetime(
        sess.get("LastHeartbeatAt")
    )
    if not tick_at:
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE CloudStreaming_Sessions SET BalanceTickAt=NOW(3) WHERE ID=%s",
                (sess["ID"],),
            )
            cur.execute("SELECT * FROM CloudStreaming_Sessions WHERE ID=%s LIMIT 1", (sess["ID"],))
            return cur.fetchone()
    elapsed = int((now - tick_at).total_seconds() // 60)
    if elapsed <= 0:
        return sess
    paid_until = session_effective_paid_until(sess, now)
    before = session_minutes_left({"PaidUntil": paid_until}, now)
    if not paid_until:
        paid_until = now
    new_until = paid_until - timedelta(minutes=elapsed)
    if new_until < now:
        new_until = now
    after = session_minutes_left({"PaidUntil": new_until}, now)
    with conn.cursor() as cur:
        cur.execute(
            "UPDATE CloudStreaming_Sessions SET PaidUntil=%s, BalanceTickAt=NOW(3), "
            "PlusMinutesLeft=0, OwnedMinutesLeft=0 WHERE ID=%s",
            (fmt_dt(new_until), sess["ID"]),
        )
    sync_renew_at(conn, sess["ID"])
    record_stream_play_minutes(conn, sess["UserID"], sess["LeaseID"], elapsed)
    with conn.cursor() as cur:
        cur.execute("SELECT * FROM CloudStreaming_Sessions WHERE ID=%s LIMIT 1", (sess["ID"],))
        sess = cur.fetchone()
    if after <= 0 and before > 0:
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE CloudStreaming_Sessions SET Status='ended', EndedAt=NOW(3), "
                "EndReason='time_expired', StreamActive=0, "
                "UiMessage=%s WHERE ID=%s",
                ("Оплаченное время закончилось.", sess["ID"]),
            )
            cur.execute("SELECT * FROM CloudStreaming_Sessions WHERE ID=%s LIMIT 1", (sess["ID"],))
            sess = cur.fetchone()
    return sess


def archive_session_row(conn, sess, reason="consolidate"):
    if not sess:
        return
    cols = (
        "ID", "SessionToken", "UserID", "LeaseID", "AccountID", "GameID", "ServiceType",
        "GameIdentifier", "Status", "BlockNo", "BlockStartedAt", "PaidUntil", "RenewAt",
        "StreamActive", "LastHeartbeatAt", "EndedAt", "EndReason", "UiMessage",
        "PlusMinutesLeft", "OwnedMinutesLeft", "BillingPool", "BalanceTickAt",
        "CreatedAt", "UpdatedAt",
    )
    values = []
    for c in cols:
        if c in ("PlusMinutesLeft", "OwnedMinutesLeft"):
            values.append(int(sess.get(c) or 0))
        else:
            values.append(sess.get(c))
    with conn.cursor() as cur:
        cur.execute(
            "INSERT INTO CloudStreaming_SessionsArchive ("
            + ", ".join(cols)
            + ", ArchivedAt, ArchiveReason) VALUES ("
            + ", ".join(["%s"] * len(cols))
            + ", NOW(3), %s) "
            "ON DUPLICATE KEY UPDATE ArchiveReason=VALUES(ArchiveReason), ArchivedAt=NOW(3)",
            tuple(values) + (reason,),
        )
        cur.execute("DELETE FROM CloudStreaming_Sessions WHERE ID=%s", (sess["ID"],))


def _session_rank(sess):
    status_rank = {
        "active": 0,
        "grace_no_stream": 1,
        "renewal_pending": 2,
        "pending_payment": 3,
        "ended": 4,
        "failed": 5,
    }
    return (
        status_rank.get(sess.get("Status"), 9),
        -session_minutes_left(sess),
        -int(sess.get("ID") or 0),
    )


def consolidate_user_sessions(conn, user_id=None):
    """Keep one live Sessions row per user; move the rest to archive."""
    with conn.cursor() as cur:
        if user_id is None:
            cur.execute("SELECT DISTINCT UserID FROM CloudStreaming_Sessions")
        else:
            cur.execute(
                "SELECT DISTINCT UserID FROM CloudStreaming_Sessions WHERE UserID=%s",
                (user_id,),
            )
        user_ids = [r["UserID"] for r in cur.fetchall()]
    for uid in user_ids:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT * FROM CloudStreaming_Sessions WHERE UserID=%s FOR UPDATE",
                (uid,),
            )
            rows = cur.fetchall()
        if len(rows) <= 1:
            continue
        rows_sorted = sorted(rows, key=_session_rank)
        keep = dict(rows_sorted[0])
        now = datetime.now()
        best_until = None
        max_block = max(int(r.get("BlockNo") or 1) for r in rows)
        for r in rows:
            effective = session_effective_paid_until(r, now)
            if effective and (not best_until or effective > best_until):
                best_until = effective
        if not best_until or best_until <= now:
            best_until = now
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE CloudStreaming_Sessions SET PaidUntil=%s, BlockNo=%s, "
                "PlusMinutesLeft=0, OwnedMinutesLeft=0 WHERE ID=%s",
                (fmt_dt(best_until), max_block, keep["ID"]),
            )
        sync_renew_at(conn, keep["ID"])
        for r in rows:
            if r["ID"] == keep["ID"]:
                continue
            archive_session_row(conn, r, reason="consolidate")
        log.info(
            "sessions consolidate user=%s keep=%s paid_until=%s archived=%s",
            uid,
            keep["ID"],
            fmt_dt(best_until),
            len(rows) - 1,
        )


def ensure_sessions_balance_schema(conn):
    """Idempotent: archive table, wallet columns, consolidate, unique UserID."""
    with conn.cursor() as cur:
        cur.execute(
            """
            CREATE TABLE IF NOT EXISTS CloudStreaming_SessionsArchive (
                ID BIGINT UNSIGNED NOT NULL,
                SessionToken CHAR(36) NOT NULL,
                UserID BIGINT UNSIGNED NOT NULL,
                LeaseID BIGINT UNSIGNED NOT NULL,
                AccountID BIGINT UNSIGNED NOT NULL,
                GameID BIGINT UNSIGNED NOT NULL,
                ServiceType ENUM('pscloud','psnow') NOT NULL,
                GameIdentifier VARCHAR(128) NOT NULL,
                Status VARCHAR(32) NOT NULL,
                BlockNo INT NOT NULL DEFAULT 1,
                BlockStartedAt DATETIME(3) NOT NULL,
                PaidUntil DATETIME(3) NOT NULL,
                RenewAt DATETIME(3) NOT NULL,
                StreamActive TINYINT(1) NOT NULL DEFAULT 0,
                LastHeartbeatAt DATETIME(3) NULL,
                EndedAt DATETIME(3) NULL,
                EndReason VARCHAR(64) NULL,
                UiMessage VARCHAR(512) NULL,
                PlusMinutesLeft INT NOT NULL DEFAULT 0,
                OwnedMinutesLeft INT NOT NULL DEFAULT 0,
                BillingPool ENUM('plus','owned') NULL,
                BalanceTickAt DATETIME(3) NULL,
                CreatedAt DATETIME(3) NULL,
                UpdatedAt DATETIME(3) NULL,
                ArchivedAt DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
                ArchiveReason VARCHAR(64) NOT NULL DEFAULT 'consolidate',
                PRIMARY KEY (ID),
                KEY idx_cs_sess_arch_user (UserID, ArchivedAt),
                KEY idx_cs_sess_arch_token (SessionToken)
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
            """
        )
    # Drop charges FK so archived SessionIDs remain valid historically.
    with conn.cursor() as cur:
        cur.execute(
            "SELECT CONSTRAINT_NAME FROM information_schema.KEY_COLUMN_USAGE "
            "WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME='CloudStreaming_Charges' "
            "AND CONSTRAINT_NAME='fk_cs_charge_session' LIMIT 1"
        )
        if cur.fetchone():
            cur.execute("ALTER TABLE CloudStreaming_Charges DROP FOREIGN KEY fk_cs_charge_session")
    for col, ddl in (
        ("PlusMinutesLeft", "INT NOT NULL DEFAULT 0 AFTER UiMessage"),
        ("OwnedMinutesLeft", "INT NOT NULL DEFAULT 0 AFTER PlusMinutesLeft"),
        ("BillingPool", "ENUM('plus','owned') NULL AFTER OwnedMinutesLeft"),
        ("BalanceTickAt", "DATETIME(3) NULL AFTER BillingPool"),
    ):
        if not _column_exists(conn, "CloudStreaming_Sessions", col):
            with conn.cursor() as cur:
                cur.execute(
                    "ALTER TABLE CloudStreaming_Sessions ADD COLUMN %s %s" % (col, ddl)
                )
    migrate_legacy_wallets_to_paid_until(conn)
    consolidate_user_sessions(conn)
    with conn.cursor() as cur:
        cur.execute(
            "SELECT COUNT(*) AS c FROM information_schema.STATISTICS "
            "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='CloudStreaming_Sessions' "
            "AND INDEX_NAME='uq_cs_sess_user_one'"
        )
        if int((cur.fetchone() or {}).get("c") or 0) == 0:
            cur.execute(
                "ALTER TABLE CloudStreaming_Sessions ADD UNIQUE KEY uq_cs_sess_user_one (UserID)"
            )
    conn.commit()


def get_user_session(conn, user_id, for_update=False):
    with conn.cursor() as cur:
        cur.execute(
            "SELECT COUNT(*) AS c FROM CloudStreaming_Sessions WHERE UserID=%s",
            (user_id,),
        )
        count = int((cur.fetchone() or {}).get("c") or 0)
    if count > 1:
        consolidate_user_sessions(conn, user_id)
    sql = "SELECT * FROM CloudStreaming_Sessions WHERE UserID=%s LIMIT 1"
    if for_update:
        sql += " FOR UPDATE"
    with conn.cursor() as cur:
        cur.execute(sql, (user_id,))
        return cur.fetchone()


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


def annotate_client(out, email=None, user_id=None):
    """Attach client identity to every billing reply for logs + client UID display."""
    if not isinstance(out, dict):
        return out
    if email:
        out["email"] = email
    if user_id is not None:
        out["user_id"] = int(user_id)
    return out


def resolve_client_from_payload(conn, payload, out=None):
    """Best-effort email/uid for logging and reply enrichment."""
    email = (payload.get("email") or "").strip().lower()
    user_id = None
    if isinstance(out, dict):
        if out.get("email"):
            email = str(out.get("email")).strip().lower() or email
        if out.get("user_id") is not None:
            try:
                user_id = int(out.get("user_id"))
            except (TypeError, ValueError):
                user_id = None
        if user_id is None and out.get("session_token"):
            with conn.cursor() as cur:
                cur.execute(
                    "SELECT u.ID AS UserID, u.User AS Email FROM CloudStreaming_Sessions s "
                    "JOIN CloudStreaming_Users u ON u.ID = s.UserID "
                    "WHERE s.SessionToken=%s LIMIT 1",
                    (out.get("session_token"),),
                )
                row = cur.fetchone()
                if row:
                    user_id = row.get("UserID")
                    email = (row.get("Email") or email or "").strip().lower()
    if email and user_id is None:
        try:
            user = ensure_user(conn, email)
            user_id = user.get("ID")
        except Exception:
            pass
    if not email and payload.get("session_token"):
        with conn.cursor() as cur:
            cur.execute(
                "SELECT u.ID AS UserID, u.User AS Email FROM CloudStreaming_Sessions s "
                "JOIN CloudStreaming_Users u ON u.ID = s.UserID "
                "WHERE s.SessionToken=%s LIMIT 1",
                (payload.get("session_token"),),
            )
            row = cur.fetchone()
            if row:
                user_id = row.get("UserID")
                email = (row.get("Email") or "").strip().lower()
    return email, user_id


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
    ensure_sessions_balance_schema(conn)


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
            "WHERE l.UserID = %s AND "
            + lease_valid_sql()
            + " ORDER BY l.LastActivityAt DESC LIMIT 1",
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
            "WHERE l.UserID = %s AND "
            + lease_valid_sql()
            + " ORDER BY l.LastActivityAt DESC",
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


def game_billing_pool(conn, account_id, game):
    """Plus/F2P/free catalog vs per-title owned (CloudStreaming_AccountOwnedGames)."""
    if not game:
        return "plus"
    if game.get("ID"):
        game = refresh_game_access_type(conn, game)
    if account_id and account_owns_game(
        conn,
        account_id,
        game["ServiceType"],
        game["GameIdentifier"],
        game.get("ID"),
    ):
        return "owned"
    return "plus"


def session_billing_pool(conn, session_row):
    account_id = session_row.get("AccountID")
    game_id = session_row.get("GameID")
    if not game_id:
        return "plus"
    with conn.cursor() as cur:
        cur.execute("SELECT * FROM CloudStreaming_Games WHERE ID=%s LIMIT 1", (game_id,))
        game = cur.fetchone()
    if not game:
        return "plus"
    return game_billing_pool(conn, account_id, game)


def _billing_pool_account_id(conn, user_id, game):
    lease = find_active_lease(conn, user_id, game) if game else None
    if not lease:
        lease = find_any_user_lease(conn, user_id)
    return lease["AccountID"] if lease else None


def _session_matches_resumable(conn, row, target_pool, st, identifiers, catalog_ids, game_id):
    if target_pool == "plus":
        return session_billing_pool(conn, row) == "plus"
    return _session_same_game(row, st, identifiers, catalog_ids, game_id)


def find_resumable_session(conn, user_id, service_type, game_identifier, game_id=None, game=None):
    """Resume when the shared paid-time balance still has minutes."""
    sess = get_user_session(conn, user_id, for_update=True)
    if not sess:
        return None
    sess = tick_session_balance(conn, sess)
    if sess.get("Status") == "pending_payment":
        return None
    if session_minutes_left(sess) <= 0:
        return None
    # Recover wrongly ended rows that still have wallet time.
    if sess.get("Status") in ("ended", "failed"):
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE CloudStreaming_Sessions SET Status='grace_no_stream', "
                "StreamActive=0, EndedAt=NULL, EndReason=NULL, "
                "UiMessage=%s WHERE ID=%s",
                ("Восстановлен остаток оплаченного времени…", sess["ID"]),
            )
            cur.execute("SELECT * FROM CloudStreaming_Sessions WHERE ID=%s LIMIT 1", (sess["ID"],))
            sess = cur.fetchone()
    return sess


def find_other_active_session(conn, user_id, service_type, game_identifier, game_id=None, game=None):
    """Obsolete with one row + shared pool wallets — never forfeit sibling time."""
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


def allocate_catalog_account(conn):
    """Soft-assign any free PS account for catalog/search (prefer PS+)."""
    with conn.cursor() as cur:
        cur.execute(
            "SELECT * FROM CloudStreaming_Accounts "
            "WHERE Status = 'available' "
            "ORDER BY HasPsPlus DESC, LastUsedAt IS NULL DESC, LastUsedAt ASC "
            "FOR UPDATE"
        )
        return cur.fetchone()


def ensure_user_catalog_npsso(conn, user_id):
    """
    NPSSO from the player's first assigned CloudStreaming_Accounts row.
    If the player has no active/retention lease yet, soft-assign one (same lease
    table as start) so search uses the same PS Now account they will stream on.
    """
    lease = find_any_user_lease(conn, user_id)
    if lease:
        return {
            "npsso": lease.get("NPSSO") or "",
            "account_id": lease.get("AccountID"),
            "account_label": lease.get("AccountLabel") or "",
            "lease_id": lease.get("ID"),
            "soft_assigned": False,
        }
    account = allocate_catalog_account(conn)
    if not account:
        return None
    now = datetime.now()
    with conn.cursor() as cur:
        cur.execute(
            "INSERT INTO CloudStreaming_Leases "
            "(UserID, AccountID, Status, FirstAssignedAt, LastActivityAt, RetentionUntil, SaveFreezeActive) "
            "VALUES (%s, %s, 'active', %s, %s, %s, 0)",
            (user_id, account["ID"], fmt_dt(now), fmt_dt(now), fmt_dt(now)),
        )
        lease_id = cur.lastrowid
        cur.execute(
            "UPDATE CloudStreaming_Accounts SET Status='leased', CurrentLeaseID=%s, "
            "LastUsedAt=NOW(3) WHERE ID=%s",
            (lease_id, account["ID"]),
        )
    return {
        "npsso": account.get("NPSSO") or "",
        "account_id": account["ID"],
        "account_label": account.get("Label") or "",
        "lease_id": lease_id,
        "soft_assigned": True,
    }


def handle_catalog_npsso(conn, req):
    """Return NPSSO of the player's assigned PS account for PS Now search/catalog."""
    email = (req.get("email") or "").strip().lower()
    req_id = req.get("id")
    if not email:
        return reply(req_id, False, error="Укажите email")
    user = ensure_user(conn, email)
    info = ensure_user_catalog_npsso(conn, user["ID"])
    if not info or not (info.get("npsso") or "").strip():
        return reply(
            req_id,
            False,
            error="Нет свободных PS-аккаунтов для каталога. Попробуйте позже.",
            ui_message="Все аккаунты заняты. Ожидайте освобождения.",
        )
    log.info(
        "catalog_npsso user=%s account_id=%s soft_assigned=%s",
        user["ID"],
        info.get("account_id"),
        info.get("soft_assigned"),
    )
    return reply(
        req_id,
        True,
        npsso=info["npsso"],
        account_id=info["account_id"],
        account_label=info.get("account_label") or "",
        soft_assigned=bool(info.get("soft_assigned")),
    )


def touch_lease(conn, lease_id):
    now = datetime.now()
    with conn.cursor() as cur:
        cur.execute(
            "UPDATE CloudStreaming_Leases SET "
            "LastActivityAt = %s, Status = 'active', UpdatedAt = NOW(3) "
            "WHERE ID = %s",
            (fmt_dt(now), lease_id),
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
    target_account_id=None,
):
    """No-op: each player has a single live Sessions row with Plus/Owned wallets."""
    return


def session_payload(conn, sess, billing_pool=None, persist_pool=False):
    if not sess or not sess.get("ID"):
        return {}
    with conn.cursor() as cur:
        cur.execute(
            "SELECT * FROM CloudStreaming_Sessions WHERE ID=%s LIMIT 1",
            (sess["ID"],),
        )
        fresh = cur.fetchone()
    if not fresh:
        return {}
    fresh = tick_session_balance(conn, fresh)
    with conn.cursor() as cur:
        cur.execute(
            "SELECT s.*, g.Name AS GameName, g.HourlyPrice, a.NPSSO, a.Label AS AccountLabel, "
            "a.Region AS AccountRegion, "
            "l.RetentionUntil "
            "FROM CloudStreaming_Sessions s "
            "JOIN CloudStreaming_Games g ON g.ID = s.GameID "
            "JOIN CloudStreaming_Accounts a ON a.ID = s.AccountID "
            "JOIN CloudStreaming_Leases l ON l.ID = s.LeaseID "
            "WHERE s.ID = %s LIMIT 1",
            (fresh["ID"],),
        )
        row = cur.fetchone()
    if not row:
        return {}
    now = datetime.now()
    paid_until = parse_db_datetime(row.get("PaidUntil"))
    minutes_left = session_minutes_left(row, now)
    renew_at = (
        paid_until - RENEW_LEAD
        if paid_until and paid_until > now
        else now
    )
    region = (row.get("AccountRegion") or "PL").strip().upper() or "PL"
    store_country = "US" if region in AMERICAS_REGIONS else region
    return {
        "session_token": row["SessionToken"],
        "user_id": int(row["UserID"]) if row.get("UserID") is not None else None,
        "npsso": row["NPSSO"],
        "account_label": row.get("AccountLabel"),
        "account_region": region,
        "store_country": store_country,
        "store_lang": "en",
        "service_type": row.get("ServiceType"),
        "game_identifier": row.get("GameIdentifier"),
        "game_name": row.get("GameName"),
        "hourly_price": float(row.get("HourlyPrice") or DEFAULT_HOURLY),
        "paid_until": fmt_dt(paid_until) if paid_until else "",
        "minutes_left": minutes_left,
        "plus_minutes_left": minutes_left,
        "owned_minutes_left": minutes_left,
        "renew_at": fmt_dt(renew_at),
        "renew_soon": minutes_left <= int(RENEW_LEAD.total_seconds() // 60),
        "retention_until": str(row.get("RetentionUntil") or ""),
        "status": row["Status"],
        "stream_active": bool(row.get("StreamActive")),
        "ui_message": row.get("UiMessage") or "",
    }



def catalog_title_key(name):
    """Normalize titles so Director's Cut / Plus suffixes still match for dedupe."""
    if not name:
        return ""
    name = name.lower()
    for junk in (
        "(playstation plus)",
        "playstation plus",
        "director's cut",
        "directors cut",
        "director´s cut",
        "digital deluxe edition",
        "digital deluxe",
        "deluxe edition",
        "standard edition",
        "game of the year edition",
        "game of the year",
        "goty",
        "remastered",
    ):
        name = name.replace(junk, "")
    return "".join(ch for ch in name if ch.isalnum())


AMERICAS_REGIONS = {
    "US", "CA", "MX", "BR", "AR", "CL", "CO", "PE", "EC", "BO",
    "PY", "UY", "CR", "GT", "HN", "NI", "PA", "SV", "DO",
}

# Regional SKU prefix for rented PS accounts (EP for EU/PL, UP for US).
RENTAL_SKU_PREFIX = os.environ.get("CS_RENTAL_SKU_PREFIX", "EP").strip().upper() or "EP"
RENTAL_INCLUDE_PS3 = os.environ.get("CS_RENTAL_INCLUDE_PS3", "0").strip().lower() in (
    "1",
    "true",
    "yes",
)
# Bumped when rental catalog filter rules change — clients invalidate old caches.
CATALOG_FILTER_VERSION = int(os.environ.get("CS_CATALOG_FILTER_VERSION", "12"))

# Non-rental-store product id prefixes (e.g. HP = Hong Kong).
FOREIGN_SKU_PREFIXES = frozenset(
    {
        "UP",
        "HP",
        "HN",
        "JA",
        "KF",
        "KR",
        "AS",
        "IP",
    }
)

# pscloud streamable rows from these imagic lists may work on PS5 cloud (Plus / F2P).
PSCLOUD_RENTAL_SOURCE_LISTS = frozenset(
    (
        "plus-games-list",
        "ubisoft-classics-list",
        "plus-classics-list",
        "plus-monthly-games-list",
        "free-to-play-list",
    )
)


def preferred_sku_prefix(account_region):
    cc = (account_region or "PL").strip().upper()
    return "UP" if cc in AMERICAS_REGIONS else "EP"


def sku_prefix(product_or_stream_id):
    pid = (product_or_stream_id or "").strip().upper()
    if pid.startswith(("EP", "EE", "EC")):
        return "EP"
    if pid.startswith("UP"):
        return "UP"
    return ""


def owned_catalog_ids(conn):
    with conn.cursor() as cur:
        cur.execute("SELECT DISTINCT CatalogID FROM CloudStreaming_AccountOwnedGames")
        return {int(r["CatalogID"]) for r in cur.fetchall() if r.get("CatalogID")}


def catalog_stable_key(product_id):
    """Match chiaki stable-key: EP9000-CUSA… and UP9000-CUSA… share a suffix."""
    pid = (product_id or "").strip().upper()
    if len(pid) > 4 and pid[4] == "-":
        return pid[4:]
    return pid


def catalog_row_primary_id(row):
    return (row.get("StreamIdentifier") or row.get("ProductId") or "").strip().upper()


def catalog_row_is_legacy_classic(row):
    """PS3/PS1/PSP-style ids (NPEA…), not CUSA/PPSA modern segments."""
    pid = catalog_row_primary_id(row)
    dash = pid.find("-")
    if dash < 0:
        return True
    title = pid[dash + 1 :]
    return not (title.startswith("CUSA") or title.startswith("PPSA"))


def catalog_row_rental_region_ok(row):
    """Keep EU rental SKUs; drop US/HK/etc. modern and foreign legacy store ids."""
    pid = catalog_row_primary_id(row)
    if not pid:
        return True
    if pid.startswith(("UP", "NPUB", "NPUG", "NPUA", "NPUJ", "NPUH")):
        return False
    if catalog_row_is_legacy_classic(row):
        return True
    pref = sku_prefix(pid)
    if pref:
        return pref == RENTAL_SKU_PREFIX
    if len(pid) >= 2 and pid[:2] in FOREIGN_SKU_PREFIXES:
        return False
    return True


def filter_rental_playable_catalog(conn, rows):
    """Hourly rental: psnow streamable on PS+ accounts; pscloud only when purchased."""
    owned_ids = owned_catalog_ids(conn)
    kept = []
    dropped_purchaseable = 0
    dropped_pscloud = 0
    dropped_legacy = 0
    dropped_region = 0
    for row in rows:
        catalog_id = row.get("ID")
        if catalog_id in owned_ids:
            kept.append(row)
            continue

        cat = (row.get("Category") or "streamable").lower()
        st = (row.get("ServiceType") or "").lower()
        if cat == "purchaseable":
            dropped_purchaseable += 1
            continue
        if cat not in ("streamable", "owned"):
            continue

        if st == "pscloud":
            source = (row.get("SourceList") or "").strip()
            if cat == "streamable" and source == "free-to-play-list":
                if catalog_row_rental_region_ok(row):
                    kept.append(row)
                continue
            dropped_pscloud += 1
            continue

        if st == "psnow":
            # PS3/PSP/PS1 classics (NPEA/NPEB/…) need PSNW01 — not on rental PS+.
            if catalog_row_is_legacy_classic(row) and not RENTAL_INCLUDE_PS3:
                dropped_legacy += 1
                continue
            if not catalog_row_rental_region_ok(row):
                dropped_region += 1
                continue
            kept.append(row)
    log.info(
        "catalog rental filter v%s: kept=%s drop purchaseable=%s pscloud=%s legacy=%s region=%s",
        CATALOG_FILTER_VERSION,
        len(kept),
        dropped_purchaseable,
        dropped_pscloud,
        dropped_legacy,
        dropped_region,
    )
    return kept


def catalog_row_to_game(row, owned_ids=None):
    """Compact game object (no imageUrl — client builds Chihiro URL)."""
    st = row["ServiceType"]
    stream_id = row["StreamIdentifier"]
    product_id = (row.get("ProductId") or stream_id or "").strip()
    platform = (row.get("Platform") or "unknown").lower()
    if platform == "unknown":
        platform = "ps4" if st == "psnow" else "ps5"
    category = (row.get("Category") or "streamable").lower()
    if owned_ids and row.get("ID") in owned_ids:
        category = "owned"
    image_url = (row.get("ImageUrl") or "").strip()
    out = {
        "productId": product_id,
        "name": row["Name"],
        "category": category,
        "serviceType": st,
        "platform": platform,
        "streamIdentifier": stream_id,
        "streamServiceType": st,
        "sourceList": (row.get("SourceList") or "").strip(),
    }
    if image_url:
        out["imageUrl"] = image_url
    return out


def _catalog_pick_score(game, prefer_prefix="EP"):
    """Higher is better for hourly rental catalog display."""
    score = 0
    st = game.get("serviceType")
    if st == "psnow":
        score += 100
    elif st == "pscloud":
        score += 10
    pid = game.get("productId") or game.get("streamIdentifier") or ""
    pref = sku_prefix(pid)
    if prefer_prefix and pref == prefer_prefix:
        score += 50
    elif pref:
        score += 20
    # Prefer known-good Ghost PS Now SKU (CUSA32709) over CUSA32708 when both exist.
    if "CUSA32709" in pid.upper():
        score += 5
    if game.get("platform") == "ps4" and st == "psnow":
        score += 2
    return score


def dedupe_catalog_games(rows, prefer_prefix="EP", owned_ids=None):
    """Prefer psnow over pscloud; prefer regional SKU prefix (default EP for EU rental)."""
    picked = {}
    extras = []
    for row in rows:
        game = catalog_row_to_game(row, owned_ids)
        key = catalog_title_key(game.get("name"))
        if not key:
            extras.append(game)
            continue
        prev = picked.get(key)
        if not prev or _catalog_pick_score(game, prefer_prefix) > _catalog_pick_score(prev, prefer_prefix):
            picked[key] = game
    games = list(picked.values()) + extras
    games.sort(key=lambda g: (g.get("name") or "").lower())
    return games


def resolve_regional_stream_id(conn, service_type, game_identifier, account_region):
    """Map catalog id to the SKU region of the rented PS account (EP for EU, UP for US)."""
    st = (service_type or "").lower().strip()
    gid = (game_identifier or "").strip()
    prefer = preferred_sku_prefix(account_region)
    catalog = find_catalog_match(conn, st, gid)
    if not catalog:
        return gid, None

    current = (catalog.get("ProductId") or catalog.get("StreamIdentifier") or gid).strip()
    if sku_prefix(current) == prefer:
        return (catalog.get("StreamIdentifier") or current), catalog

    title_key = catalog_title_key(catalog.get("Name"))
    best = None
    best_score = -1
    with conn.cursor() as cur:
        cur.execute(
            "SELECT * FROM CloudStreaming_Catalog "
            "WHERE ServiceType = %s AND IsVisible = 1",
            (st,),
        )
        candidates = cur.fetchall()
    for row in candidates:
        if catalog_title_key(row.get("Name")) != title_key:
            continue
        cand_id = (row.get("ProductId") or row.get("StreamIdentifier") or "").strip()
        if sku_prefix(cand_id) != prefer:
            continue
        score = _catalog_pick_score(catalog_row_to_game(row), prefer)
        if score > best_score:
            best_score = score
            best = row
    if best:
        out = (best.get("StreamIdentifier") or best.get("ProductId") or gid).strip()
        log.info(
            "regional SKU remap %s -> %s (account region=%s prefer=%s)",
            gid,
            out,
            account_region,
            prefer,
        )
        return out, best
    log.warning(
        "no %s SKU for title key=%s (requested %s, region=%s); using original",
        prefer,
        title_key,
        gid,
        account_region,
    )
    return (catalog.get("StreamIdentifier") or current), catalog


# Catalog is served over TCP as one qCompress blob (UDP floods never arrive).
_CATALOG_MEM = {"key": None, "ts": 0.0, "games": None}
CATALOG_MEM_TTL_SEC = int(os.environ.get("CS_CATALOG_CACHE_SEC", "300"))
CATALOG_CHUNK_RAW = int(os.environ.get("CS_CATALOG_CHUNK_RAW", "650"))
CATALOG_SEND_PACE_SEC = float(os.environ.get("CS_CATALOG_SEND_PACE_SEC", "0.002"))


def load_catalog_games(conn, service_type, platform, only_billable, rental_playable=True):
    key = (service_type or "*", platform or "*", bool(only_billable), bool(rental_playable))
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

    owned_ids = owned_catalog_ids(conn)
    if rental_playable:
        before = len(rows)
        rows = filter_rental_playable_catalog(conn, rows)
        log.info("catalog rental filter: %s -> %s rows", before, len(rows))

    games = dedupe_catalog_games(rows, prefer_prefix=RENTAL_SKU_PREFIX, owned_ids=owned_ids)
    _CATALOG_MEM["key"] = key
    _CATALOG_MEM["ts"] = now
    _CATALOG_MEM["games"] = games
    return games


def handle_catalog(conn, req):
    """Build full catalog and return a qCompress blob for chunked UDP send."""
    req_id = req.get("id")
    service_type = (req.get("service_type") or "").strip().lower()
    platform = (req.get("platform") or "").strip().lower()
    only_billable = bool(req.get("only_billable"))
    rental_playable = req.get("rental_playable")
    if rental_playable is None:
        rental_playable = True
    else:
        rental_playable = bool(rental_playable)

    games = load_catalog_games(
        conn, service_type, platform, only_billable, rental_playable=rental_playable
    )
    body = {
        "games": games,
        "totalGames": len(games),
        "catalog_source": "billing_db",
        "catalog_filter_version": CATALOG_FILTER_VERSION,
    }
    raw = json.dumps(body, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    # Qt QByteArray::qUncompress expects big-endian uncompressed length + zlib.
    blob = struct.pack(">I", len(raw)) + zlib.compress(raw, 6)
    log.info(
        "catalog: prepared %s games (%s bytes json -> %s bytes qcompress) "
        "filters service=%s platform=%s only_billable=%s rental_playable=%s",
        len(games),
        len(raw),
        len(blob),
        service_type or "*",
        platform or "*",
        only_billable,
        rental_playable,
    )
    return {
        "id": req_id,
        "ok": True,
        "transfer": "qcompress_chunks",
        "totalGames": len(games),
        "_blob": blob,
    }


def send_udp_reply(sock, out, addr):
    """Send a normal JSON reply. Catalog blobs must use TCP (UDP floods are dropped)."""
    if isinstance(out, dict) and out.get("transfer") == "qcompress_chunks" and out.get("_blob") is not None:
        # Do not blast hundreds of UDP datagrams — NATs/firewalls drop them (client sees 0/0).
        req_id = out.get("id")
        msg = reply(
            req_id,
            False,
            error="catalog_use_tcp",
            ui_message="Каталог нужно загрузить по TCP (порт %s)." % TCP_PORT,
            tcp_port=TCP_PORT,
            totalGames=out.get("totalGames") or 0,
        )
        raw = json.dumps(msg, ensure_ascii=False).encode("utf-8")
        try:
            sock.sendto(raw, addr)
        except OSError as e:
            log.error("sendto %s failed: %s", addr, e)
        return

    raw = json.dumps(out, ensure_ascii=False).encode("utf-8")
    try:
        sock.sendto(raw, addr)
    except OSError as e:
        log.error("sendto %s failed: %s (bytes=%s)", addr, e, len(raw))


def handle_tcp_client(conn, addr):
    """One JSON line request; catalog replies with header line + raw qCompress blob."""
    conn.settimeout(120)
    try:
        f = conn.makefile("rwb")
        line = f.readline(1024 * 1024)
        if not line:
            return
        try:
            payload = json.loads(line.decode("utf-8"))
        except Exception as e:
            log.warning("bad TCP json from %s: %s", addr, e)
            return
        log.info(
            "TCP REQ %s from %s action=%s email=%s",
            payload.get("id"),
            addr,
            payload.get("action"),
            (payload.get("email") or "-"),
        )
        out = dispatch(payload)
        blob = None
        if isinstance(out, dict):
            blob = out.pop("_blob", None)
        if blob is not None:
            header = {
                "id": out.get("id"),
                "ok": True,
                "transfer": "qcompress",
                "totalGames": out.get("totalGames"),
                "nbytes": len(blob),
                "catalog_source": "billing_db",
                "catalog_filter_version": CATALOG_FILTER_VERSION,
            }
            f.write((json.dumps(header, separators=(",", ":")) + "\n").encode("utf-8"))
            f.write(blob)
            f.flush()
            log.info(
                "TCP catalog: sent header + %s bytes to %s (games=%s)",
                len(blob),
                addr,
                out.get("totalGames"),
            )
        else:
            f.write((json.dumps(out, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8"))
            f.flush()
    except Exception as e:
        log.exception("TCP client %s failed: %s", addr, e)
    finally:
        try:
            conn.close()
        except Exception:
            pass


def tcp_server_loop():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((UDP_HOST, TCP_PORT))
    srv.listen(32)
    log.info("Cloud billing TCP listening on %s:%s (catalog)", UDP_HOST, TCP_PORT)
    while True:
        conn, addr = srv.accept()
        threading.Thread(target=handle_tcp_client, args=(conn, addr), daemon=True).start()


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
        mins = session_minutes_left(resumable)
        ui_msg = (
            "У вас осталось %s мин на балансе. "
            "«%s» можно запустить без дополнительного списания."
            % (mins, game["Name"])
        )
        return reply(
            req_id,
            True,
            ui_message=ui_msg,
            hourly_price=0,
            currency=game.get("Currency") or "RUB",
            resume_session=True,
            no_charge=True,
            minutes_left=mins,
            plus_minutes_left=mins,
            owned_minutes_left=mins,
            game_name=game["Name"],
        )

    lease = find_active_lease(conn, user["ID"], game)
    reuse = lease is not None
    if reuse:
        msg = (
            "Будет использован ваш сохранённый аккаунт PS (%s). "
            "Сейчас спишется %s ₽ за 1 час — время добавится на баланс («%s»)."
            % (lease.get("AccountLabel") or "аренда", int(price), game["Name"])
        )
    else:
        msg = (
            "Сейчас спишется %s ₽ за 1 час игры «%s». "
            "Время будет зачислено на баланс. "
            "Вам выделят PS-аккаунт; сохранения хранятся %s дней."
            % (int(price), game["Name"], RETENTION_DAYS)
        )

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
        account_id = resumable.get("AccountID")
        with conn.cursor() as cur:
            cur.execute(
                "SELECT Region FROM CloudStreaming_Accounts WHERE ID=%s LIMIT 1",
                (account_id,),
            )
            acc_row = cur.fetchone() or {}
        region = (acc_row.get("Region") or "PL").strip().upper()
        play_id, _ = resolve_regional_stream_id(
            conn, service_type, game_identifier, region
        )
        if play_id and play_id != game_identifier:
            game_identifier = play_id
            game = ensure_game(conn, service_type, play_id, game_name)
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE CloudStreaming_Sessions SET GameID=%s, GameIdentifier=%s, "
                "ServiceType=%s, StreamActive=1, Status='active', "
                "BalanceTickAt=NOW(3), LastHeartbeatAt=NOW(3), "
                "EndedAt=NULL, EndReason=NULL WHERE ID=%s",
                (game["ID"], game_identifier, service_type, resumable["ID"]),
            )
        touch_lease(conn, resumable["LeaseID"])
        conn.commit()
        with conn.cursor() as cur:
            cur.execute(
                "SELECT * FROM CloudStreaming_Sessions WHERE ID=%s LIMIT 1",
                (resumable["ID"],),
            )
            resumable = cur.fetchone()
        payload = session_payload(conn, resumable)
        mins = int(payload.get("minutes_left") or session_minutes_left(resumable))
        ui_msg = (
            "Запуск «%s». Осталось %s мин на балансе."
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
        payload["minutes_left"] = mins
        return reply(req_id, True, **payload)

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
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO CloudStreaming_Leases "
                "(UserID, AccountID, Status, FirstAssignedAt, LastActivityAt, RetentionUntil, SaveFreezeActive) "
                "VALUES (%s, %s, 'active', %s, %s, %s, 0)",
                (user["ID"], account["ID"], fmt_dt(now), fmt_dt(now), fmt_dt(now)),
            )
            lease_id = cur.lastrowid
            cur.execute(
                "UPDATE CloudStreaming_Accounts SET Status='leased', CurrentLeaseID=%s, "
                "LastUsedAt=NOW(3) WHERE ID=%s",
                (lease_id, account["ID"]),
            )
            cur.execute("SELECT * FROM CloudStreaming_Leases WHERE ID=%s", (lease_id,))
            lease = cur.fetchone()

    region = (account.get("Region") or "PL").strip().upper()
    play_id, _ = resolve_regional_stream_id(conn, service_type, game_identifier, region)
    if play_id and play_id != game_identifier:
        game_identifier = play_id
        game = ensure_game(conn, service_type, game_identifier, game_name)

    token = str(uuid.uuid4())
    now = datetime.now()
    ui_msg = (
        "Подготовка к запуску «%s». Списание %s ₽ произойдёт только после успешного "
        "подключения к игре. Время будет зачислено на баланс."
        % (game["Name"], int(price))
    )

    existing = get_user_session(conn, user["ID"], for_update=True)
    if existing:
        if existing.get("Status") in ("ended", "failed") and session_minutes_left(
            existing
        ) <= 0:
            archive_session_row(conn, existing, reason="new_block")
            existing = None

    if existing:
        block_no = int(existing.get("BlockNo") or 0) + 1
        with conn.cursor() as cur:
            cur.execute(
                "UPDATE CloudStreaming_Sessions SET "
                "SessionToken=%s, LeaseID=%s, AccountID=%s, GameID=%s, ServiceType=%s, "
                "GameIdentifier=%s, Status='pending_payment', BlockNo=%s, "
                "BlockStartedAt=%s, StreamActive=0, "
                "BalanceTickAt=NULL, LastHeartbeatAt=NULL, EndedAt=NULL, EndReason=NULL, "
                "UiMessage=%s WHERE ID=%s",
                (
                    token,
                    lease["ID"],
                    account["ID"],
                    game["ID"],
                    service_type,
                    game_identifier,
                    block_no,
                    fmt_dt(now),
                    ui_msg,
                    existing["ID"],
                ),
            )
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM CloudStreaming_Sessions WHERE ID=%s", (existing["ID"],))
            sess = cur.fetchone()
    else:
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
                    fmt_dt(now),
                    fmt_dt(now),
                    ui_msg,
                ),
            )
            session_id = cur.lastrowid
            cur.execute("SELECT * FROM CloudStreaming_Sessions WHERE ID=%s", (session_id,))
            sess = cur.fetchone()

    conn.commit()
    payload = session_payload(conn, sess)
    payload["payment_pending"] = True
    payload["hourly_price"] = price
    payload["ui_message"] = ui_msg
    return reply(req_id, True, **payload)


def handle_confirm_stream(conn, req):
    """Charge the reserved hour after Gaikai allocation succeeded; credit wallet."""
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

    sess = credit_paid_minutes(conn, sess["ID"], 60)
    with conn.cursor() as cur:
        cur.execute(
            "UPDATE CloudStreaming_Sessions SET Status='active', StreamActive=1, "
            "BalanceTickAt=NOW(3), LastHeartbeatAt=NOW(3), "
            "EndedAt=NULL, EndReason=NULL, UiMessage=%s WHERE ID=%s",
            ("Оплата прошла. Игра запущена.", sess["ID"]),
        )
        cur.execute("SELECT * FROM CloudStreaming_Sessions WHERE ID=%s", (sess["ID"],))
        sess = cur.fetchone()
    touch_lease(conn, sess["LeaseID"])
    conn.commit()

    payload = session_payload(conn, sess)
    payload["ui_message"] = (
        "Списано %s ₽ (+60 мин на баланс). Играйте; за 10 минут до конца при "
        "активном стриме спишется следующий час автоматически."
        % int(price)
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
        if sess["Status"] == "pending_payment":
            return reply(
                req_id,
                False,
                error="Ожидается подтверждение запуска",
                ui_message="Списание ещё не выполнено — дождитесь подключения к игре.",
                status=sess["Status"],
            )
        return reply(req_id, False, error="Сессия завершена", status=sess["Status"])

    # Apply burn for the previous streaming interval before updating StreamActive.
    if int(sess.get("StreamActive") or 0):
        sess = tick_session_balance(conn, sess)

    if session_minutes_left(sess) <= 0:
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
            error="Оплаченное время истекло",
            ui_message="Баланс времени закончился. Запустите игру снова для новой оплаты.",
            minutes_left=0,
            plus_minutes_left=0,
            owned_minutes_left=0,
        )

    new_status = "active" if streaming else "grace_no_stream"
    with conn.cursor() as cur:
        cur.execute(
            "UPDATE CloudStreaming_Sessions SET StreamActive=%s, LastHeartbeatAt=NOW(3), "
            "Status=%s, BalanceTickAt=IF(%s=1, COALESCE(BalanceTickAt, NOW(3)), BalanceTickAt) "
            "WHERE ID=%s",
            (1 if streaming else 0, new_status, 1 if streaming else 0, sess["ID"]),
        )
        # When pausing stream, freeze wallet (no further burn until resume).
        if not streaming:
            cur.execute(
                "UPDATE CloudStreaming_Sessions SET BalanceTickAt=NULL WHERE ID=%s",
                (sess["ID"],),
            )
        cur.execute("SELECT * FROM CloudStreaming_Sessions WHERE ID=%s", (sess["ID"],))
        sess = cur.fetchone()

    touch_lease(conn, sess["LeaseID"])
    conn.commit()

    payload = session_payload(conn, sess)
    if payload.get("renew_soon") and streaming and int(sess.get("StreamActive") or 0):
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

    sess = tick_session_balance(conn, sess)
    block_no = int(sess["BlockNo"]) + 1
    price = float(sess["HourlyPrice"])
    idem = "cs-%s-b%s" % (token, block_no)

    ok_pay, pay_msg = charge_hour(
        conn, email, sess["ID"], sess["UserID"], block_no, price, sess["GameName"], idem
    )
    if not ok_pay:
        conn.commit()
        return reply(req_id, False, error=pay_msg, ui_message=pay_msg)

    with conn.cursor() as cur:
        cur.execute(
            "UPDATE CloudStreaming_Sessions SET BlockNo=%s, BlockStartedAt=NOW(3) WHERE ID=%s",
            (block_no, sess["ID"]),
        )
    sess = credit_paid_minutes(conn, sess["ID"], 60)
    minutes_left = session_minutes_left(sess)
    with conn.cursor() as cur:
        cur.execute(
            "UPDATE CloudStreaming_Sessions SET Status='active', UiMessage=%s WHERE ID=%s",
            (
                "Продлено ещё на 1 час (%s ₽). На балансе %s мин."
                % (int(price), minutes_left),
                sess["ID"],
            ),
        )
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
                "EndReason='payment_aborted', StreamActive=0, BalanceTickAt=NULL, "
                "UiMessage=%s WHERE ID=%s",
                ("Запуск не удался — списание не выполнялось.", sess["ID"]),
            )
        conn.commit()
        return reply(
            req_id,
            True,
            ui_message="Сессия отменена без списания.",
            payment_aborted=True,
        )

    if int(sess.get("StreamActive") or 0):
        sess = tick_session_balance(conn, sess)
    mins = session_minutes_left(sess)
    with conn.cursor() as cur:
        cur.execute(
            "UPDATE CloudStreaming_Sessions SET StreamActive=0, Status='grace_no_stream', "
            "BalanceTickAt=NULL, UiMessage=%s WHERE ID=%s",
            (
                "Стрим приостановлен. Остаток: %s мин." % mins,
                sess["ID"],
            ),
        )
        cur.execute(
            "UPDATE CloudStreaming_Leases SET Status='retention' WHERE ID=%s",
            (sess["LeaseID"],),
        )
    save_info = build_save_retention_payload(conn, sess["UserID"], sess["LeaseID"])
    conn.commit()
    return reply(
        req_id,
        True,
        ui_message=(
            "Стрим остановлен. Остаток времени на балансе: %s мин. "
            "Запустите игру снова, чтобы продолжить без новой оплаты."
            % mins
        ),
        minutes_left=mins,
        plus_minutes_left=mins,
        owned_minutes_left=mins,
        **save_info,
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
        elif action == "catalog_npsso":
            out = handle_catalog_npsso(conn, payload)
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
        elif action == "whoami":
            email = (payload.get("email") or "").strip().lower()
            if not email:
                out = reply(req_id, False, error="Укажите email")
            else:
                user = ensure_user(conn, email)
                out = reply(req_id, True, email=email, user_id=user["ID"])
        else:
            out = reply(req_id, False, error="unknown action: %s" % action)
        email, user_id = resolve_client_from_payload(conn, payload, out)
        out = annotate_client(out, email=email, user_id=user_id)
        if not conn.get_autocommit():
            conn.commit()
        return out
    except Exception as e:
        log.exception("dispatch error action=%s email=%s", action, (payload.get("email") or "-"))
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
                    "ReleaseReason='save_retention_expired' "
                    "WHERE Status IN ('active','retention') "
                    "AND SaveFreezeActive = 1 AND RetentionUntil < NOW(3)"
                )
                cur.execute(
                    "UPDATE CloudStreaming_Accounts a "
                    "JOIN CloudStreaming_Leases l ON l.ID = a.CurrentLeaseID "
                    "SET a.Status='available', a.CurrentLeaseID=NULL "
                    "WHERE l.Status='expired' AND a.Status='leased'"
                )
                cur.execute(
                    "UPDATE CloudStreaming_Sessions SET StreamActive=0, Status='grace_no_stream', "
                    "BalanceTickAt=NULL, "
                    "UiMessage='Аренда аккаунта истекла по неактивности — баланс времени сохранён.' "
                    "WHERE Status IN ('active','grace_no_stream','renewal_pending') "
                    "AND LeaseID IN (SELECT ID FROM CloudStreaming_Leases WHERE Status='expired')"
                )
                cur.execute(
                    "UPDATE CloudStreaming_Sessions SET Status='ended', EndedAt=NOW(3), "
                    "EndReason='payment_aborted', StreamActive=0, BalanceTickAt=NULL "
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
    threading.Thread(target=tcp_server_loop, daemon=True).start()

    while True:
        data, addr = sock.recvfrom(65535)
        try:
            payload = json.loads(data.decode("utf-8"))
        except Exception as e:
            log.warning("bad json from %s: %s", addr, e)
            continue
        log.info(
            "REQ %s from %s action=%s email=%s uid=%s",
            payload.get("id"),
            addr,
            payload.get("action"),
            (payload.get("email") or "-"),
            "-",
        )
        out = dispatch(payload)
        try:
            log.info(
                "REQ %s done action=%s email=%s uid=%s ok=%s",
                payload.get("id"),
                payload.get("action"),
                (out.get("email") if isinstance(out, dict) else None) or (payload.get("email") or "-"),
                (out.get("user_id") if isinstance(out, dict) else None) or "-",
                (out.get("ok") if isinstance(out, dict) else None),
            )
            send_udp_reply(sock, out, addr)
        except Exception as e:
            log.error("send reply %s: %s", addr, e)


if __name__ == "__main__":
    main()
