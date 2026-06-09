#!/usr/bin/env python3
"""
Agent API - интерфейс между агентом и C++ динамической моделью
Запуск: python3 agent_api.py --port 8081
"""

import json
import sqlite3
from datetime import datetime, timedelta
from pathlib import Path
from flask import Flask, request, jsonify
from flask_cors import CORS
import threading
import time

app = Flask(__name__)
CORS(app)

WORKSPACE = Path("agent_workspace")
DB_FILE = WORKSPACE / "iphone_prices.db"
REPORTS_DIR = WORKSPACE / "reports"

# ============================================================================
# API для получения данных агентом (отдаём данные динамической модели)
# ============================================================================

@app.route('/api/agent/status', methods=['GET'])
def get_agent_status():
    """Получить статус агента"""
    status_file = WORKSPACE / "agent_status.json"
    if status_file.exists():
        with open(status_file, 'r') as f:
            return jsonify(json.load(f))
    return jsonify({"status": "not_running"})


@app.route('/api/agent/latest_prices', methods=['GET'])
def get_latest_prices():
    """Получить последние цены"""
    conn = sqlite3.connect(DB_FILE)
    cursor = conn.cursor()
    
    cursor.execute('''
        SELECT shop, model, storage, price, timestamp 
        FROM price_history 
        WHERE timestamp = (SELECT MAX(timestamp) FROM price_history)
        ORDER BY model, shop
    ''')
    
    prices = []
    for row in cursor.fetchall():
        prices.append({
            "shop": row[0], "model": row[1], "storage": row[2],
            "price": row[3], "timestamp": row[4]
        })
    
    conn.close()
    return jsonify({"prices": prices, "count": len(prices)})


@app.route('/api/agent/exchange_rates', methods=['GET'])
def get_exchange_rates():
    """Получить курс валют"""
    conn = sqlite3.connect(DB_FILE)
    cursor = conn.cursor()
    
    cursor.execute('''
        SELECT base_currency, rate, timestamp 
        FROM exchange_rates 
        ORDER BY timestamp DESC LIMIT 2
    ''')
    
    rates = {}
    for row in cursor.fetchall():
        rates[row[0]] = {"rate": row[1], "timestamp": row[2]}
    
    conn.close()
    return jsonify(rates)


@app.route('/api/agent/market_stats', methods=['GET'])
def get_market_stats():
    """Получить рыночную статистику"""
    days = request.args.get('days', 1, type=int)
    
    conn = sqlite3.connect(DB_FILE)
    cursor = conn.cursor()
    
    date_limit = (datetime.now() - timedelta(days=days)).date().isoformat()
    
    cursor.execute('''
        SELECT model, storage, 
               MIN(min_price) as min_price,
               MAX(max_price) as max_price,
               AVG(avg_price) as avg_price
        FROM market_analytics 
        WHERE date >= ?
        GROUP BY model, storage
    ''', (date_limit,))
    
    stats = []
    for row in cursor.fetchall():
        stats.append({
            "model": row[0], "storage": row[1],
            "min_price": row[2], "max_price": row[3], "avg_price": round(row[4], 0)
        })
    
    conn.close()
    return jsonify({"stats": stats, "days": days})


@app.route('/api/agent/reports', methods=['GET'])
def get_reports():
    """Получить список отчётов"""
    reports = []
    if REPORTS_DIR.exists():
        for f in sorted(REPORTS_DIR.glob("*.md"), reverse=True)[:10]:
            reports.append({
                "name": f.name,
                "path": str(f),
                "modified": datetime.fromtimestamp(f.stat().st_mtime).isoformat()
            })
    return jsonify({"reports": reports})


@app.route('/api/agent/report/<filename>', methods=['GET'])
def get_report(filename):
    """Получить содержимое отчёта"""
    report_file = REPORTS_DIR / filename
    if report_file.exists():
        with open(report_file, 'r', encoding='utf-8') as f:
            return jsonify({"content": f.read(), "filename": filename})
    return jsonify({"error": "Report not found"}), 404


@app.route('/api/agent/run_now', methods=['POST'])
def run_agent_now():
    """Запустить сбор данных сейчас"""
    import subprocess
    try:
        result = subprocess.run(
            ["python3", "iphone_agent.py", "run-now", "--workspace", str(WORKSPACE)],
            capture_output=True, text=True, timeout=120
        )
        return jsonify({"success": True, "output": result.stdout})
    except Exception as e:
        return jsonify({"success": False, "error": str(e)}), 500


# ============================================================================
# API для отправки данных в динамическую модель (сигналы для C++)
# ============================================================================

@app.route('/api/signals/state', methods=['POST'])
def send_state_signal():
    """
    Отправить состояние системы в динамическую модель
    Ожидает: { "q": [...], "p": [...], "energy": float, "timestamp": string }
    """
    data = request.json
    print(f"[SIGNAL] State received: energy={data.get('energy', 0)}")
    # Здесь C++ модель получает состояние
    return jsonify({"status": "ok", "received": True})


@app.route('/api/signals/reward', methods=['POST'])
def send_reward_signal():
    """
    Отправить сигнал награды в динамическую модель
    Ожидает: { "reward": float, "action": string, "success": bool }
    """
    data = request.json
    print(f"[SIGNAL] Reward: {data.get('reward', 0)} for action {data.get('action', 'unknown')}")
    return jsonify({"status": "ok"})


@app.route('/api/signals/analysis', methods=['POST'])
def send_analysis_signal():
    """
    Отправить результат анализа LLM в динамическую модель
    Ожидает: { "analysis": string, "confidence": float, "tokens": int }
    """
    data = request.json
    analysis = data.get('analysis', '')[:100]
    print(f"[SIGNAL] Analysis: {analysis}... (conf={data.get('confidence', 0)})")
    return jsonify({"status": "ok"})


@app.route('/api/signals/price_change', methods=['POST'])
def send_price_change_signal():
    """
    Отправить сигнал об изменении цены
    Ожидает: { "shop": string, "model": string, "old_price": int, "new_price": int, "change_percent": float }
    """
    data = request.json
    print(f"[SIGNAL] Price change: {data.get('shop')} {data.get('model')} {data.get('change_percent', 0):+.1f}%")
    return jsonify({"status": "ok"})


@app.route('/api/signals/currency_change', methods=['POST'])
def send_currency_change_signal():
    """
    Отправить сигнал об изменении курса валют
    Ожидает: { "currency": string, "old_rate": float, "new_rate": float, "change_percent": float }
    """
    data = request.json
    print(f"[SIGNAL] Currency: {data.get('currency')} {data.get('change_percent', 0):+.2f}%")
    return jsonify({"status": "ok"})


@app.route('/api/signals/report_created', methods=['POST'])
def send_report_signal():
    """
    Отправить сигнал о создании отчёта
    Ожидает: { "report_file": string, "size_bytes": int }
    """
    data = request.json
    print(f"[SIGNAL] Report created: {data.get('report_file')}")
    return jsonify({"status": "ok"})


# ============================================================================
# API для получения данных ИЗ динамической модели
# ============================================================================

@app.route('/api/model/risk', methods=['GET'])
def get_model_risk():
    """Получить текущий риск от динамической модели"""
    # Здесь C++ модель возвращает риск
    return jsonify({
        "risk": 0.0,
        "energy_error": 0.0,
        "violations": 0,
        "lagrangian_risk": 0.0,
        "timestamp": datetime.now().isoformat()
    })


@app.route('/api/model/entropy', methods=['GET'])
def get_model_entropy():
    """Получить энтропию системы"""
    return jsonify({
        "entropy": 0.5,
        "target_entropy": 0.5,
        "temperature": 1.0
    })


@app.route('/api/model/state', methods=['GET'])
def get_model_state():
    """Получить текущее состояние модели (q, p, energy)"""
    return jsonify({
        "q": [0.5] * 32,
        "p": [0.0] * 32,
        "energy": 0.0,
        "kinetic": 0.0,
        "potential": 0.0
    })


# ============================================================================
# ЗАПУСК
# ============================================================================

if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=8081, type=int)
    parser.add_argument("--workspace", default="agent_workspace")
    args = parser.parse_args()
    
    WORKSPACE = Path(args.workspace)
    DB_FILE = WORKSPACE / "iphone_prices.db"
    REPORTS_DIR = WORKSPACE / "reports"
    
    print(f"Agent API starting on port {args.port}")
    print(f"Workspace: {WORKSPACE}")
    app.run(host='0.0.0.0', port=args.port, debug=False, threaded=True)