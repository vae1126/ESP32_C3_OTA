"""
OTA 固件管理平台 — 数据模型 (SQLite)
"""
import sqlite3
import os
from datetime import datetime
from config import DATABASE


def get_db():
    """获取数据库连接"""
    conn = sqlite3.connect(DATABASE)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA foreign_keys=ON")
    return conn


def init_db():
    """初始化数据库表结构"""
    conn = get_db()
    conn.executescript("""
        CREATE TABLE IF NOT EXISTS firmware (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            version     TEXT    NOT NULL,
            filename    TEXT    NOT NULL,
            file_size   INTEGER DEFAULT 0,
            sha256      TEXT    DEFAULT '',
            changelog   TEXT    DEFAULT '',
            is_latest   INTEGER DEFAULT 0,
            created_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        );

        CREATE TABLE IF NOT EXISTS devices (
            id              INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id       TEXT    UNIQUE NOT NULL,
            device_name     TEXT    DEFAULT '',
            current_version TEXT    DEFAULT 'unknown',
            target_version  TEXT    DEFAULT '',
            status          TEXT    DEFAULT 'online',
            last_seen       TIMESTAMP,
            created_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        );

        CREATE TABLE IF NOT EXISTS upgrade_logs (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id     TEXT,
            from_version  TEXT,
            to_version    TEXT,
            status        TEXT    DEFAULT 'pending',
            started_at    TIMESTAMP,
            completed_at  TIMESTAMP
        );
    """)
    conn.commit()
    conn.close()


# ---- 固件 CRUD ----

def firmware_add(version, filename, file_size=0, sha256="", changelog="", is_latest=0):
    conn = get_db()
    conn.execute(
        "INSERT INTO firmware (version, filename, file_size, sha256, changelog, is_latest) VALUES (?,?,?,?,?,?)",
        (version, filename, file_size, sha256, changelog, is_latest)
    )
    # 如果标记为 latest，取消其他版本的 latest 标志
    if is_latest:
        conn.execute("UPDATE firmware SET is_latest=0 WHERE version != ?", (version,))
    conn.commit()
    conn.close()


def firmware_list():
    conn = get_db()
    rows = conn.execute("SELECT * FROM firmware ORDER BY created_at DESC").fetchall()
    conn.close()
    return [dict(r) for r in rows]


def firmware_get_by_id(fw_id):
    conn = get_db()
    row = conn.execute("SELECT * FROM firmware WHERE id=?", (fw_id,)).fetchone()
    conn.close()
    return dict(row) if row else None


def firmware_get_latest():
    conn = get_db()
    row = conn.execute("SELECT * FROM firmware WHERE is_latest=1 ORDER BY created_at DESC LIMIT 1").fetchone()
    conn.close()
    return dict(row) if row else None


def firmware_delete(fw_id):
    """删除固件记录和对应的 .bin 文件"""
    from config import UPLOAD_DIR
    fw = firmware_get_by_id(fw_id)
    if fw:
        filepath = os.path.join(UPLOAD_DIR, fw["filename"])
        if os.path.exists(filepath):
            os.remove(filepath)
        conn = get_db()
        conn.execute("DELETE FROM firmware WHERE id=?", (fw_id,))
        conn.commit()
        conn.close()
        return True
    return False


def firmware_set_latest(fw_id):
    conn = get_db()
    conn.execute("UPDATE firmware SET is_latest=0")
    conn.execute("UPDATE firmware SET is_latest=1 WHERE id=?", (fw_id,))
    conn.commit()
    conn.close()


# ---- 设备管理 ----

def device_update(device_id, device_name="", current_version="", status="online"):
    conn = get_db()
    conn.execute(
        """INSERT INTO devices (device_id, device_name, current_version, status, last_seen)
           VALUES (?,?,?,?,CURRENT_TIMESTAMP)
           ON CONFLICT(device_id) DO UPDATE SET
           device_name=excluded.device_name,
           current_version=excluded.current_version,
           status=excluded.status,
           last_seen=CURRENT_TIMESTAMP""",
        (device_id, device_name, current_version, status)
    )
    conn.commit()
    conn.close()


def device_list():
    conn = get_db()
    rows = conn.execute("SELECT * FROM devices ORDER BY last_seen DESC").fetchall()
    conn.close()
    return [dict(r) for r in rows]


# ---- 升级日志 ----

def log_add(device_id, from_version, to_version, status="pending"):
    conn = get_db()
    conn.execute(
        "INSERT INTO upgrade_logs (device_id, from_version, to_version, status, started_at) VALUES (?,?,?,?,CURRENT_TIMESTAMP)",
        (device_id, from_version, to_version, status)
    )
    conn.commit()
    conn.close()


def log_update(device_id, status):
    conn = get_db()
    conn.execute(
        "UPDATE upgrade_logs SET status=?, completed_at=CURRENT_TIMESTAMP WHERE device_id=? AND status='pending'",
        (status, device_id)
    )
    conn.commit()
    conn.close()


def log_list(limit=50):
    conn = get_db()
    rows = conn.execute("SELECT * FROM upgrade_logs ORDER BY started_at DESC LIMIT ?", (limit,)).fetchall()
    conn.close()
    return [dict(r) for r in rows]
