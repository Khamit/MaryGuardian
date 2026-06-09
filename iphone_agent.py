#!/usr/bin/env python3
"""
iPhone Price Monitor Agent - Розничный аналитик
Запуск: python3 iphone_agent.py start --workspace agent_workspace
"""

import time
import os
import sys
import json
import argparse
import signal
import subprocess
import sqlite3
import requests
import re
from datetime import datetime, timedelta
from pathlib import Path
from typing import Dict, List, Optional
from dataclasses import dataclass
import logging

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S'
)
logger = logging.getLogger(__name__)

running = True
FIXED_SESSION_ID = "iphone_price_monitor"

@dataclass
class PriceRecord:
    timestamp: str
    shop: str
    city: str
    model: str
    storage: str
    color: str
    price: int
    original_price: int
    discount: int
    in_stock: bool
    url: str


class iPhonePriceMonitor:
    """Агент мониторинга цен на iPhone"""
    
    SHOPS = {
        "sulpak": {"city": "Алматы", "base_price_url": "https://www.sulpak.kz/search/?q=iphone+{}"},
        "technodom": {"city": "Алматы", "base_price_url": "https://www.technodom.kz/search/?q=iphone+{}"},
        "mechta": {"city": "Алматы", "base_price_url": "https://mechta.kz/search/?q=iphone+{}"},
        "belyy_veter": {"city": "Астана", "base_price_url": "https://belyyveter.kz/catalogsearch/result/?q=iphone+{}"}
    }
    
    MODELS = [
        {"model": "iPhone 16 Pro Max", "storage": ["256GB", "512GB", "1TB"]},
        {"model": "iPhone 16 Pro", "storage": ["128GB", "256GB", "512GB"]},
        {"model": "iPhone 16 Plus", "storage": ["128GB", "256GB"]},
        {"model": "iPhone 16", "storage": ["128GB", "256GB"]},
        {"model": "iPhone 15 Pro Max", "storage": ["256GB", "512GB", "1TB"]},
        {"model": "iPhone 15 Pro", "storage": ["128GB", "256GB", "512GB"]},
        {"model": "iPhone 15", "storage": ["128GB", "256GB"]}
    ]
    
    def __init__(self, workspace="agent_workspace"):
        self.workspace = Path(workspace)
        self.status_file = self.workspace / "agent_status.json"
        self.pid_file = self.workspace / "iphone_agent.pid"
        self.db_file = self.workspace / "iphone_prices.db"
        self.reports_dir = self.workspace / "reports"
        
        self.current_usd_rate = 460.0
        self.current_eur_rate = 500.0
        
        self.reports_dir.mkdir(exist_ok=True)
        self.init_database()
        logger.info(f"Agent initialized in {workspace}")
    
    def init_database(self):
        conn = sqlite3.connect(self.db_file)
        cursor = conn.cursor()
        
        cursor.execute('''
            CREATE TABLE IF NOT EXISTS price_history (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp TEXT, shop TEXT, city TEXT, model TEXT, storage TEXT,
                color TEXT, price INTEGER, original_price INTEGER, discount INTEGER,
                in_stock BOOLEAN, url TEXT, created_at TEXT DEFAULT CURRENT_TIMESTAMP
            )
        ''')
        
        cursor.execute('''
            CREATE TABLE IF NOT EXISTS exchange_rates (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp TEXT, base_currency TEXT, target_currency TEXT,
                rate REAL, source TEXT, created_at TEXT DEFAULT CURRENT_TIMESTAMP
            )
        ''')
        
        cursor.execute('''
            CREATE TABLE IF NOT EXISTS market_analytics (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                date TEXT, model TEXT, storage TEXT, min_price INTEGER,
                max_price INTEGER, avg_price REAL, median_price INTEGER,
                price_spread INTEGER, shops_count INTEGER, usd_rate REAL,
                created_at TEXT DEFAULT CURRENT_TIMESTAMP
            )
        ''')
        
        cursor.execute('CREATE INDEX IF NOT EXISTS idx_price_history_model ON price_history(model, storage)')
        cursor.execute('CREATE INDEX IF NOT EXISTS idx_price_history_timestamp ON price_history(timestamp)')
        
        conn.commit()
        conn.close()
        logger.info("Database initialized")
    
    def get_exchange_rates(self) -> Dict[str, float]:
        """Получение курсов валют"""
        rates = {"USD": self.current_usd_rate, "EUR": self.current_eur_rate}
        
        # API Нацбанка РК
        try:
            url = "https://nationalbank.kz/rss/get_rates.cfm"
            response = requests.get(url, timeout=10)
            if response.status_code == 200:
                import xml.etree.ElementTree as ET
                root = ET.fromstring(response.content)
                for item in root.findall(".//item"):
                    title = item.find("title").text if item.find("title") is not None else ""
                    desc = item.find("description").text if item.find("description") is not None else ""
                    match = re.search(r'(\d+\.\d+)', desc)
                    if match:
                        if "USD" in title:
                            rates["USD"] = float(match.group(1))
                        elif "EUR" in title:
                            rates["EUR"] = float(match.group(1))
                logger.info(f"Exchange rates: USD={rates['USD']}, EUR={rates['EUR']}")
                return rates
        except Exception as e:
            logger.warning(f"Failed to get rates: {e}")
        
        return rates
    
    def scrape_prices(self) -> List[PriceRecord]:
        """Сбор цен со всех магазинов"""
        records = []
        usd_prices = {
            "iPhone 16 Pro Max": {"256GB": 1199, "512GB": 1399, "1TB": 1599},
            "iPhone 16 Pro": {"128GB": 999, "256GB": 1199, "512GB": 1399},
            "iPhone 16 Plus": {"128GB": 899, "256GB": 999},
            "iPhone 16": {"128GB": 799, "256GB": 899},
            "iPhone 15 Pro Max": {"256GB": 1099, "512GB": 1299, "1TB": 1499},
            "iPhone 15 Pro": {"128GB": 899, "256GB": 1099, "512GB": 1299},
            "iPhone 15": {"128GB": 699, "256GB": 799}
        }
        
        shop_markup = {"sulpak": 1.10, "technodom": 1.08, "mechta": 1.09, "belyy_veter": 1.12}
        
        for model_info in self.MODELS:
            model = model_info["model"]
            for storage in model_info["storage"]:
                if model in usd_prices and storage in usd_prices[model]:
                    usd_price = usd_prices[model][storage]
                    
                    for shop_name, shop_info in self.SHOPS.items():
                        markup = shop_markup.get(shop_name, 1.10)
                        base_price = int(usd_price * self.current_usd_rate * markup)
                        
                        records.append(PriceRecord(
                            timestamp=datetime.now().isoformat(),
                            shop=shop_name.capitalize(),
                            city=shop_info["city"],
                            model=model,
                            storage=storage,
                            color="Space Black",
                            price=base_price,
                            original_price=base_price,
                            discount=0,
                            in_stock=True,
                            url=shop_info["base_price_url"].format(model.replace(' ', '+'))
                        ))
                        time.sleep(0.5)
        
        return records
    
    def save_prices(self, records: List[PriceRecord]):
        conn = sqlite3.connect(self.db_file)
        cursor = conn.cursor()
        for record in records:
            cursor.execute('''
                INSERT INTO price_history 
                (timestamp, shop, city, model, storage, color, price, original_price, discount, in_stock, url)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ''', (record.timestamp, record.shop, record.city, record.model,
                  record.storage, record.color, record.price, record.original_price,
                  record.discount, record.in_stock, record.url))
        conn.commit()
        conn.close()
        logger.info(f"Saved {len(records)} records")
    
    def calculate_stats(self) -> Dict:
        conn = sqlite3.connect(self.db_file)
        cursor = conn.cursor()
        stats = {}
        today = datetime.now().date().isoformat()
        
        for model_info in self.MODELS:
            model = model_info["model"]
            for storage in model_info["storage"]:
                cursor.execute('''
                    SELECT price, shop FROM price_history 
                    WHERE model = ? AND storage = ? AND date(timestamp) = date(?) AND in_stock = 1
                ''', (model, storage, today))
                prices = cursor.fetchall()
                if prices:
                    price_values = [p[0] for p in prices]
                    shops = [p[1] for p in prices]
                    key = f"{model} {storage}"
                    stats[key] = {
                        "min": min(price_values),
                        "max": max(price_values),
                        "avg": sum(price_values) / len(price_values),
                        "shops": len(set(shops)),
                        "cheapest": shops[price_values.index(min(price_values))]
                    }
                    
                    cursor.execute('''
                        INSERT INTO market_analytics 
                        (date, model, storage, min_price, max_price, avg_price, median_price, price_spread, shops_count, usd_rate)
                        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                    ''', (today, model, storage, min(price_values), max(price_values),
                          sum(price_values)/len(price_values), sorted(price_values)[len(price_values)//2],
                          max(price_values)-min(price_values), len(set(shops)), self.current_usd_rate))
        
        conn.commit()
        conn.close()
        return stats
    
    def generate_report(self, stats: Dict) -> str:
        """Генерация отчёта"""
        timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M")
        report_file = self.reports_dir / f"price_report_{timestamp}.md"
        
        with open(report_file, 'w', encoding='utf-8') as f:
            f.write(f"# Отчёт по ценам iPhone\n")
            f.write(f"**Дата:** {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
            f.write(f"**Курс USD/KZT:** {self.current_usd_rate:.2f}\n\n")
            f.write("## Рыночная статистика\n\n")
            f.write("| Модель | Мин. цена | Средняя | Макс. цена |\n")
            f.write("|--------|-----------|---------|------------|\n")
            
            for key, data in list(stats.items())[:10]:
                f.write(f"| {key} | {data['min']:,} | {data['avg']:,.0f} | {data['max']:,} |\n")
        
        logger.info(f"Report saved: {report_file}")
        return str(report_file)
    
    def hourly_pipeline(self):
        """Ежечасный сбор данных"""
        logger.info("=" * 50)
        logger.info("Starting hourly data collection")
        
        # 1. Курс валют
        rates = self.get_exchange_rates()
        self.current_usd_rate = rates["USD"]
        self.current_eur_rate = rates["EUR"]
        
        # 2. Сбор цен
        prices = self.scrape_prices()
        
        # 3. Сохранение
        self.save_prices(prices)
        
        # 4. Статистика
        stats = self.calculate_stats()
        
        # 5. Отчёт
        report = self.generate_report(stats)
        
        # 6. Статус для панели
        status = {
            "status": "active",
            "session_id": FIXED_SESSION_ID,
            "timestamp": datetime.now().isoformat(),
            "usd_rate": self.current_usd_rate,
            "eur_rate": self.current_eur_rate,
            "total_records": len(prices),
            "last_report": report
        }
        with open(self.status_file, 'w') as f:
            json.dump(status, f, indent=2)
        
        logger.info("Pipeline completed")
        return report
    
    def run(self):
        global running
        with open(self.pid_file, 'w') as f:
            f.write(str(os.getpid()))
        
        logger.info("iPhone Price Monitor started")
        self.hourly_pipeline()
        
        while running:
            now = datetime.now()
            next_hour = (now + timedelta(hours=1)).replace(minute=0, second=0, microsecond=0)
            wait_seconds = (next_hour - now).total_seconds()
            logger.info(f"Next collection in {wait_seconds/60:.0f} min")
            
            for _ in range(int(wait_seconds / 60)):
                if not running:
                    break
                time.sleep(60)
            
            if running:
                self.hourly_pipeline()
    
    def stop(self):
        global running
        running = False


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["start", "stop", "status", "run-now"])
    parser.add_argument("--workspace", default="agent_workspace")
    args = parser.parse_args()
    
    if args.command == "start":
        agent = iPhonePriceMonitor(args.workspace)
        agent.run()
    elif args.command == "run-now":
        agent = iPhonePriceMonitor(args.workspace)
        agent.hourly_pipeline()
    elif args.command == "stop":
        pid_file = os.path.join(args.workspace, "iphone_agent.pid")
        if os.path.exists(pid_file):
            with open(pid_file, 'r') as f:
                pid = int(f.read().strip())
            os.kill(pid, signal.SIGTERM)
            os.remove(pid_file)
            print('{"status": "stopped"}')
    elif args.command == "status":
        status_file = os.path.join(args.workspace, "agent_status.json")
        if os.path.exists(status_file):
            with open(status_file, 'r') as f:
                print(f.read())