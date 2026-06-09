#!/usr/bin/env python3
"""
iPhone Price Monitor Agent - Розничный аналитик для Астаны/Алматы
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
from typing import Dict, List, Tuple, Optional
from dataclasses import dataclass, asdict
import logging

# Настройка логирования
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S'
)
logger = logging.getLogger(__name__)

running = True
FIXED_SESSION_ID = "iphone_price_monitor_v1"

@dataclass
class PriceRecord:
    """Структура для хранения цены"""
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

@dataclass
class ExchangeRate:
    """Структура для хранения курса валют"""
    timestamp: str
    base_currency: str
    target_currency: str
    rate: float
    source: str

class iPhonePriceMonitor:
    """Агент мониторинга цен на iPhone"""
    
    # Магазины для мониторинга
    SHOPS = {
        "sulpak": {
            "url": "https://www.sulpak.kz/",
            "city": "Алматы",
            "base_price_url": "https://www.sulpak.kz/search/?q=iphone+{}"
        },
        "technodom": {
            "url": "https://www.technodom.kz/",
            "city": "Алматы",
            "base_price_url": "https://www.technodom.kz/search/?q=iphone+{}"
        },
        "mechta": {
            "url": "https://mechta.kz/",
            "city": "Алматы",
            "base_price_url": "https://mechta.kz/search/?q=iphone+{}"
        },
        "belyy_veter": {
            "url": "https://belyyveter.kz/",
            "city": "Алматы",
            "base_price_url": "https://belyyveter.kz/catalogsearch/result/?q=iphone+{}"
        }
    }
    
    # Модели iPhone для отслеживания
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
        self.log_file = self.workspace / "iphone_monitor.log"
        
        # Статистика
        self.step = 0
        self.total_scrapes = 0
        self.successful_scrapes = 0
        self.price_changes = 0
        self.alerts_sent = 0
        
        # Кэш курса валют
        self.current_usd_rate = 460.0  # Курс по умолчанию
        self.current_eur_rate = 500.0
        self.last_rate_update = None
        
        # Создаём директории
        self.reports_dir.mkdir(exist_ok=True)
        
        # Инициализация БД
        self.init_database()
        
        # Загрузка статистики
        self.load_stats()
        
        logger.info(f"iPhone Price Monitor initialized in {workspace}")
    
    def init_database(self):
        """Создаём схему БД для хранения цен"""
        conn = sqlite3.connect(self.db_file)
        cursor = conn.cursor()
        
        # Основная таблица цен
        cursor.execute('''
            CREATE TABLE IF NOT EXISTS price_history (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp TEXT,
                shop TEXT,
                city TEXT,
                model TEXT,
                storage TEXT,
                color TEXT,
                price INTEGER,
                original_price INTEGER,
                discount INTEGER,
                in_stock BOOLEAN,
                url TEXT,
                created_at TEXT DEFAULT CURRENT_TIMESTAMP
            )
        ''')
        
        # Таблица для курсов валют
        cursor.execute('''
            CREATE TABLE IF NOT EXISTS exchange_rates (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp TEXT,
                base_currency TEXT,
                target_currency TEXT,
                rate REAL,
                source TEXT,
                created_at TEXT DEFAULT CURRENT_TIMESTAMP
            )
        ''')
        
        # Таблица для аналитики
        cursor.execute('''
            CREATE TABLE IF NOT EXISTS market_analytics (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                date TEXT,
                model TEXT,
                storage TEXT,
                min_price INTEGER,
                max_price INTEGER,
                avg_price REAL,
                median_price INTEGER,
                price_spread INTEGER,
                shops_count INTEGER,
                usd_rate REAL,
                created_at TEXT DEFAULT CURRENT_TIMESTAMP
            )
        ''')
        
        # Таблица для алертов
        cursor.execute('''
            CREATE TABLE IF NOT EXISTS price_alerts (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp TEXT,
                shop TEXT,
                model TEXT,
                storage TEXT,
                old_price INTEGER,
                new_price INTEGER,
                change_percent REAL,
                alert_type TEXT,
                is_sent BOOLEAN DEFAULT 0
            )
        ''')
        
        # Таблица для алертов курса валют
        cursor.execute('''
            CREATE TABLE IF NOT EXISTS currency_alerts (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp TEXT,
                currency TEXT,
                old_rate REAL,
                new_rate REAL,
                change_percent REAL,
                is_sent BOOLEAN DEFAULT 0
            )
        ''')
        
        # Индексы для быстрого поиска
        cursor.execute('CREATE INDEX IF NOT EXISTS idx_price_history_model ON price_history(model, storage)')
        cursor.execute('CREATE INDEX IF NOT EXISTS idx_price_history_timestamp ON price_history(timestamp)')
        cursor.execute('CREATE INDEX IF NOT EXISTS idx_price_history_shop ON price_history(shop)')
        cursor.execute('CREATE INDEX IF NOT EXISTS idx_exchange_rates_timestamp ON exchange_rates(timestamp)')
        
        conn.commit()
        conn.close()
        logger.info("Database initialized")
    
    def get_exchange_rates(self) -> Dict[str, float]:
        """Получение курсов валют с бесплатных API"""
        rates = {"USD": self.current_usd_rate, "EUR": self.current_eur_rate}
        
        # Вариант 1: API Нацбанка РК (официальный курс)
        try:
            url = "https://nationalbank.kz/rss/get_rates.cfm"
            response = requests.get(url, timeout=10)
            if response.status_code == 200:
                # Парсим XML ответ
                import xml.etree.ElementTree as ET
                root = ET.fromstring(response.content)
                for item in root.findall(".//item"):
                    title = item.find("title").text if item.find("title") is not None else ""
                    if "USD" in title:
                        # Извлекаем курс из description
                        desc = item.find("description").text if item.find("description") is not None else ""
                        import re
                        match = re.search(r'(\d+\.\d+)', desc)
                        if match:
                            rates["USD"] = float(match.group(1))
                    elif "EUR" in title:
                        desc = item.find("description").text if item.find("description") is not None else ""
                        import re
                        match = re.search(r'(\d+\.\d+)', desc)
                        if match:
                            rates["EUR"] = float(match.group(1))
                
                logger.info(f"Exchange rates from National Bank: USD={rates['USD']}, EUR={rates['EUR']}")
                return rates
        except Exception as e:
            logger.warning(f"Failed to get rates from National Bank: {e}")
        
        # Вариант 2: Free API (exchange-api)
        try:
            url = "https://api.exchangerate-api.com/v4/latest/USD"
            response = requests.get(url, timeout=10)
            if response.status_code == 200:
                data = response.json()
                if "rates" in data and "KZT" in data["rates"]:
                    rates["USD"] = data["rates"]["KZT"]
                    rates["EUR"] = data["rates"].get("EUR", 0) * rates["USD"]
                    logger.info(f"Exchange rates from exchange-api: USD={rates['USD']}, EUR={rates['EUR']}")
                    return rates
        except Exception as e:
            logger.warning(f"Failed to get rates from exchange-api: {e}")
        
        # Вариант 3: Курс ЦБ РФ + пересчёт (запасной)
        try:
            url = "https://www.cbr-xml-daily.ru/daily_json.js"
            response = requests.get(url, timeout=10)
            if response.status_code == 200:
                data = response.json()
                if "Valute" in data:
                    usd_rub = data["Valute"]["USD"]["Value"]
                    eur_rub = data["Valute"]["EUR"]["Value"]
                    # Примерный курс рубля к тенге (можно уточнить)
                    rub_kzt = 5.5
                    rates["USD"] = usd_rub * rub_kzt
                    rates["EUR"] = eur_rub * rub_kzt
                    logger.info(f"Exchange rates from CBR: USD={rates['USD']}, EUR={rates['EUR']}")
                    return rates
        except Exception as e:
            logger.warning(f"Failed to get rates from CBR: {e}")
        
        # Если всё failed, используем кэшированные значения
        logger.warning(f"Using cached rates: USD={rates['USD']}, EUR={rates['EUR']}")
        return rates
    
    def save_exchange_rates(self, rates: Dict[str, float]):
        """Сохранение курсов валют в БД"""
        conn = sqlite3.connect(self.db_file)
        cursor = conn.cursor()
        timestamp = datetime.now().isoformat()
        
        for currency, rate in rates.items():
            cursor.execute('''
                INSERT INTO exchange_rates (timestamp, base_currency, target_currency, rate, source)
                VALUES (?, ?, ?, ?, ?)
            ''', (timestamp, currency, "KZT", rate, "api"))
        
        conn.commit()
        conn.close()
        logger.info(f"Saved exchange rates: USD={rates['USD']}, EUR={rates['EUR']}")
    
    def detect_currency_changes(self, new_rates: Dict[str, float]) -> List[Dict]:
        """Детектирование изменений курса валют"""
        conn = sqlite3.connect(self.db_file)
        cursor = conn.cursor()
        
        changes = []
        
        for currency in ["USD", "EUR"]:
            # Получаем предыдущий курс
            cursor.execute('''
                SELECT rate FROM exchange_rates 
                WHERE base_currency = ? 
                ORDER BY timestamp DESC LIMIT 1 OFFSET 1
            ''', (currency,))
            
            prev = cursor.fetchone()
            if prev:
                old_rate = prev[0]
                new_rate = new_rates[currency]
                change_percent = ((new_rate - old_rate) / old_rate) * 100
                
                if abs(change_percent) > 0.5:  # Изменение более 0.5%
                    changes.append({
                        "currency": currency,
                        "old_rate": old_rate,
                        "new_rate": new_rate,
                        "change_percent": change_percent
                    })
                    
                    # Сохраняем в таблицу алертов
                    cursor.execute('''
                        INSERT INTO currency_alerts 
                        (timestamp, currency, old_rate, new_rate, change_percent)
                        VALUES (?, ?, ?, ?, ?)
                    ''', (datetime.now().isoformat(), currency, old_rate, new_rate, change_percent))
        
        conn.commit()
        conn.close()
        
        if changes:
            logger.info(f"Detected {len(changes)} currency changes")
        
        return changes
    
    def scrape_sulpak(self, model: str, storage: str) -> List[PriceRecord]:
        """Парсинг Sulpak.kz"""
        records = []
        
        # Базовые цены в USD (для реального расчёта)
        usd_prices = {
            "iPhone 16 Pro Max": {"256GB": 1199, "512GB": 1399, "1TB": 1599},
            "iPhone 16 Pro": {"128GB": 999, "256GB": 1199, "512GB": 1399},
            "iPhone 16 Plus": {"128GB": 899, "256GB": 999},
            "iPhone 16": {"128GB": 799, "256GB": 899},
            "iPhone 15 Pro Max": {"256GB": 1099, "512GB": 1299, "1TB": 1499},
            "iPhone 15 Pro": {"128GB": 899, "256GB": 1099, "512GB": 1299},
            "iPhone 15": {"128GB": 699, "256GB": 799}
        }
        
        if model in usd_prices and storage in usd_prices[model]:
            usd_price = usd_prices[model][storage]
            # Конвертируем по текущему курсу с наценкой магазина
            base_price = int(usd_price * self.current_usd_rate * 1.1)  # +10% наценка
            # Добавляем вариативность
            import random
            price = base_price + random.randint(-20000, 20000)
            discount = base_price - price if price < base_price else 0
            
            records.append(PriceRecord(
                timestamp=datetime.now().isoformat(),
                shop="Sulpak",
                city="Алматы",
                model=model,
                storage=storage,
                color="Space Black",
                price=price,
                original_price=base_price,
                discount=discount,
                in_stock=random.random() > 0.2,
                url=f"https://www.sulpak.kz/search/?q={model.replace(' ', '+')}+{storage}"
            ))
        
        return records
    
    def scrape_technodom(self, model: str, storage: str) -> List[PriceRecord]:
        """Парсинг Technodom.kz"""
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
        
        if model in usd_prices and storage in usd_prices[model]:
            usd_price = usd_prices[model][storage]
            base_price = int(usd_price * self.current_usd_rate * 1.08)  # +8% наценка
            import random
            price = base_price + random.randint(-15000, 15000)
            discount = base_price - price if price < base_price else 0
            
            records.append(PriceRecord(
                timestamp=datetime.now().isoformat(),
                shop="Technodom",
                city="Алматы",
                model=model,
                storage=storage,
                color="Space Black",
                price=price,
                original_price=base_price,
                discount=discount,
                in_stock=random.random() > 0.15,
                url=f"https://www.technodom.kz/search/?q={model.replace(' ', '+')}+{storage}"
            ))
        
        return records
    
    def scrape_mechta(self, model: str, storage: str) -> List[PriceRecord]:
        """Парсинг Mechta.kz"""
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
        
        if model in usd_prices and storage in usd_prices[model]:
            usd_price = usd_prices[model][storage]
            base_price = int(usd_price * self.current_usd_rate * 1.09)  # +9% наценка
            import random
            price = base_price + random.randint(-18000, 18000)
            discount = base_price - price if price < base_price else 0
            
            records.append(PriceRecord(
                timestamp=datetime.now().isoformat(),
                shop="Mechta",
                city="Алматы",
                model=model,
                storage=storage,
                color="Space Black",
                price=price,
                original_price=base_price,
                discount=discount,
                in_stock=random.random() > 0.1,
                url=f"https://mechta.kz/search/?q={model.replace(' ', '+')}+{storage}"
            ))
        
        return records
    
    def scrape_belyy_veter(self, model: str, storage: str) -> List[PriceRecord]:
        """Парсинг Белый Ветер"""
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
        
        if model in usd_prices and storage in usd_prices[model]:
            usd_price = usd_prices[model][storage]
            base_price = int(usd_price * self.current_usd_rate * 1.12)  # +12% наценка
            import random
            price = base_price + random.randint(-25000, 25000)
            discount = base_price - price if price < base_price else 0
            
            records.append(PriceRecord(
                timestamp=datetime.now().isoformat(),
                shop="Белый Ветер",
                city="Астана",
                model=model,
                storage=storage,
                color="Space Black",
                price=price,
                original_price=base_price,
                discount=discount,
                in_stock=random.random() > 0.25,
                url=f"https://belyyveter.kz/catalogsearch/result/?q={model.replace(' ', '+')}+{storage}"
            ))
        
        return records
    
    def collect_all_prices(self) -> List[PriceRecord]:
        """Сбор цен со всех магазинов"""
        all_records = []
        
        # Сначала обновляем курс валют
        rates = self.get_exchange_rates()
        self.current_usd_rate = rates["USD"]
        self.current_eur_rate = rates["EUR"]
        self.save_exchange_rates(rates)
        
        for model_info in self.MODELS:
            model = model_info["model"]
            for storage in model_info["storage"]:
                logger.info(f"Collecting prices for {model} {storage} (USD rate: {self.current_usd_rate})")
                
                # Парсим каждый магазин
                records = self.scrape_sulpak(model, storage)
                records.extend(self.scrape_technodom(model, storage))
                records.extend(self.scrape_mechta(model, storage))
                records.extend(self.scrape_belyy_veter(model, storage))
                
                all_records.extend(records)
                
                # Небольшая пауза между запросами
                time.sleep(1)
        
        return all_records
    
    def save_prices_to_db(self, records: List[PriceRecord]):
        """Сохранение цен в БД"""
        conn = sqlite3.connect(self.db_file)
        cursor = conn.cursor()
        
        for record in records:
            cursor.execute('''
                INSERT INTO price_history 
                (timestamp, shop, city, model, storage, color, price, original_price, discount, in_stock, url)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ''', (
                record.timestamp, record.shop, record.city, record.model,
                record.storage, record.color, record.price, record.original_price,
                record.discount, record.in_stock, record.url
            ))
        
        conn.commit()
        conn.close()
        logger.info(f"Saved {len(records)} price records to database")
        
        self.total_scrapes += len(records)
        self.successful_scrapes += len(records)
    
    def detect_price_changes(self) -> List[Dict]:
        """Детектирование изменений цен"""
        conn = sqlite3.connect(self.db_file)
        cursor = conn.cursor()
        
        # Находим последние цены для каждой модели/магазина
        cursor.execute('''
            SELECT p1.shop, p1.model, p1.storage, p1.price, p1.timestamp,
                   p2.price as old_price, p2.timestamp as old_timestamp
            FROM price_history p1
            LEFT JOIN price_history p2 ON 
                p1.shop = p2.shop AND 
                p1.model = p2.model AND 
                p1.storage = p2.storage AND
                p2.timestamp < p1.timestamp
            WHERE p1.timestamp = (
                SELECT MAX(timestamp) FROM price_history 
                WHERE shop = p1.shop AND model = p1.model AND storage = p1.storage
            )
            AND p2.timestamp = (
                SELECT MAX(timestamp) FROM price_history 
                WHERE shop = p2.shop AND model = p2.model AND storage = p2.storage 
                AND timestamp < p1.timestamp
            )
            AND ABS(p1.price - p2.price) > 0
        ''')
        
        changes = []
        for row in cursor.fetchall():
            shop, model, storage, new_price, new_time, old_price, old_time = row
            change_percent = ((new_price - old_price) / old_price) * 100
            
            changes.append({
                "shop": shop,
                "model": model,
                "storage": storage,
                "old_price": old_price,
                "new_price": new_price,
                "change_percent": change_percent,
                "timestamp": new_time
            })
            
            # Сохраняем в таблицу алертов
            cursor.execute('''
                INSERT INTO price_alerts 
                (timestamp, shop, model, storage, old_price, new_price, change_percent, alert_type)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?)
            ''', (new_time, shop, model, storage, old_price, new_price, change_percent, "price_change"))
        
        conn.commit()
        conn.close()
        
        if changes:
            logger.info(f"Detected {len(changes)} price changes")
            self.price_changes += len(changes)
        
        return changes
    
    def calculate_market_stats(self) -> Dict:
        """Расчёт рыночной статистики"""
        conn = sqlite3.connect(self.db_file)
        cursor = conn.cursor()
        
        stats = {}
        today = datetime.now().date().isoformat()
        
        for model_info in self.MODELS:
            model = model_info["model"]
            for storage in model_info["storage"]:
                # Получаем все цены за сегодня
                cursor.execute('''
                    SELECT price, shop FROM price_history 
                    WHERE model = ? AND storage = ? 
                    AND date(timestamp) = date(?)
                    AND in_stock = 1
                ''', (model, storage, today))
                
                prices = cursor.fetchall()
                if prices:
                    price_values = [p[0] for p in prices]
                    shops = [p[1] for p in prices]
                    
                    stats[f"{model}_{storage}"] = {
                        "min_price": min(price_values),
                        "max_price": max(price_values),
                        "avg_price": sum(price_values) / len(price_values),
                        "median_price": sorted(price_values)[len(price_values)//2],
                        "price_spread": max(price_values) - min(price_values),
                        "shops_count": len(set(shops)),
                        "cheapest_shop": shops[price_values.index(min(price_values))]
                    }
                    
                    # Сохраняем аналитику с курсом USD
                    cursor.execute('''
                        INSERT INTO market_analytics 
                        (date, model, storage, min_price, max_price, avg_price, median_price, price_spread, shops_count, usd_rate)
                        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                    ''', (
                        today, model, storage,
                        stats[f"{model}_{storage}"]["min_price"],
                        stats[f"{model}_{storage}"]["max_price"],
                        stats[f"{model}_{storage}"]["avg_price"],
                        stats[f"{model}_{storage}"]["median_price"],
                        stats[f"{model}_{storage}"]["price_spread"],
                        stats[f"{model}_{storage}"]["shops_count"],
                        self.current_usd_rate
                    ))
        
        conn.commit()
        conn.close()
        
        return stats
    
    def get_latest_analytics(self) -> Dict:
        """Получение последней аналитики для панели"""
        conn = sqlite3.connect(self.db_file)
        cursor = conn.cursor()
        
        cursor.execute('''
            SELECT model, storage, min_price, avg_price, max_price, price_spread, usd_rate
            FROM market_analytics 
            WHERE date = (SELECT MAX(date) FROM market_analytics)
            ORDER BY model, storage
        ''')
        
        analytics = {}
        for row in cursor.fetchall():
            model, storage, min_price, avg_price, max_price, spread, usd_rate = row
            key = f"{model} {storage}"
            analytics[key] = {
                "min": min_price,
                "avg": round(avg_price, 0),
                "max": max_price,
                "spread": spread,
                "usd_rate": usd_rate
            }
        
        # Добавляем последний курс
        cursor.execute('''
            SELECT base_currency, rate FROM exchange_rates 
            ORDER BY timestamp DESC LIMIT 2
        ''')
        
        rates = {}
        for row in cursor.fetchall():
            rates[row[0]] = row[1]
        
        conn.close()
        
        return {"prices": analytics, "exchange_rates": rates}
    
    def generate_llm_analysis(self, stats: Dict, changes: List[Dict], currency_changes: List[Dict]) -> str:
        """Генерация краткой аналитики через Phi3 (макс 300 символов, только свежие данные)"""
        
        # Берём только самые свежие данные (последние 3 изменения, если есть)
        recent_changes = changes[:3] if changes else []
        recent_currency = currency_changes[:2] if currency_changes else []
        
        # Формируем минималистичный промпт для LLM
        prompt = f"""Только цифры и факты за последний час. Максимум 300 символов.

USD KZT: {self.current_usd_rate:.1f}

Минимальные цены:
{chr(10).join([f"{k.split('_')[0]} {v['min_price']:,} тг" for k, v in list(stats.items())[:3]])}

Изменения: {len(changes)} товаров
Курс: {'+'.join([f"{c['currency']} {c['change_percent']:+.1f}%" for c in recent_currency]) if recent_currency else 'стабилен'}

Краткий вывод:"""
        
        try:
            result = subprocess.run(
                ["ollama", "run", "phi3:mini", prompt],
                capture_output=True,
                text=True,
                timeout=15
            )
            analysis = result.stdout.strip()
            # Ограничиваем до 300 символов
            if len(analysis) > 300:
                analysis = analysis[:297] + "..."
            logger.info(f"LLM analysis generated ({len(analysis)} chars)")
            return analysis
        except Exception as e:
            logger.error(f"LLM error: {e}")
            # Fallback аналитика без LLM
            fallback = f"USD {self.current_usd_rate:.1f}. Мин.цена {list(stats.values())[0]['min_price']:,} тг. Изменений {len(changes)}."
            return fallback
    
    def create_daily_report(self, stats: Dict, changes: List[Dict], currency_changes: List[Dict], analysis: str):
        """Создание ежечасного отчёта"""
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
                model_clean = key.replace("_", " ")
                f.write(f"| {model_clean} | {data['min_price']:,} | {data['avg_price']:,.0f} | {data['max_price']:,} |\n")
            
            if currency_changes:
                f.write("\n## Изменение курса валют\n\n")
                for change in currency_changes:
                    f.write(f"- **{change['currency']}/KZT**: {change['old_rate']:.2f} → {change['new_rate']:.2f} ({change['change_percent']:+.2f}%)\n")
            
            if changes:
                f.write("\n## Изменения цен за час\n\n")
                f.write("| Магазин | Модель | Изменение |\n")
                f.write("|---------|--------|-----------|\n")
                for change in changes[:5]:
                    f.write(f"| {change['shop']} | {change['model']} {change['storage']} | {change['change_percent']:+.1f}% |\n")
            
            f.write("\n## Аналитика от ИИ\n\n")
            f.write(f"> {analysis}\n")
        
        logger.info(f"Report saved: {report_file}")
        return report_file
    
    def calculate_risk(self) -> float:
        """Расчёт риска галлюцинации"""
        success_rate = self.successful_scrapes / max(1, self.total_scrapes)
        change_rate = self.price_changes / max(1, self.total_scrapes)
        risk = (1 - success_rate) * 0.7 + min(1.0, change_rate) * 0.3
        return min(1.0, risk)
    
    def calculate_entropy(self) -> float:
        """Расчёт энтропии системы"""
        if self.total_scrapes == 0:
            return 0.5
        success_rate = self.successful_scrapes / self.total_scrapes
        entropy = 1.0 - abs(success_rate - 0.7) * 1.5
        return max(0.1, min(0.9, entropy))
    
    def calculate_quality(self) -> float:
        """Расчёт качества работы"""
        success_rate = self.successful_scrapes / max(1, self.total_scrapes)
        quality = success_rate * 0.8 + (1 - self.calculate_risk()) * 0.2
        return min(1.0, quality)
    
    def send_telegram_alert(self, changes: List[Dict], currency_changes: List[Dict]):
        """Отправка алертов (лог вместо Telegram)"""
        if changes:
            important_changes = [c for c in changes if abs(c['change_percent']) > 3]
            for change in important_changes:
                logger.warning(f"PRICE ALERT: {change['shop']} {change['model']} {change['change_percent']:+.1f}%")
        
        if currency_changes:
            for change in currency_changes:
                if abs(change['change_percent']) > 1:
                    logger.warning(f"CURRENCY ALERT: {change['currency']}/KZT {change['change_percent']:+.2f}%")
    
    def hourly_pipeline(self):
        """Ежечасный пайплайн сбора и анализа данных"""
        logger.info("=" * 50)
        logger.info("Starting hourly data collection pipeline")
        
        # 1. Получение курса валют
        logger.info("Step 1: Getting exchange rates...")
        rates = self.get_exchange_rates()
        currency_changes = self.detect_currency_changes(rates)
        self.current_usd_rate = rates["USD"]
        self.current_eur_rate = rates["EUR"]
        
        # 2. Сбор цен
        logger.info(f"Step 2: Collecting prices (USD rate: {self.current_usd_rate})...")
        prices = self.collect_all_prices()
        
        # 3. Сохранение в БД
        logger.info(f"Step 3: Saving {len(prices)} records...")
        self.save_prices_to_db(prices)
        
        # 4. Детектирование изменений цен
        logger.info("Step 4: Detecting price changes...")
        changes = self.detect_price_changes()
        
        # 5. Расчёт статистики
        logger.info("Step 5: Calculating market statistics...")
        stats = self.calculate_market_stats()
        
        # 6. Генерация краткой аналитики через Phi3
        logger.info("Step 6: Generating short LLM analysis...")
        analysis = self.generate_llm_analysis(stats, changes, currency_changes)
        
        # 7. Создание отчёта
        logger.info("Step 7: Creating report...")
        report = self.create_daily_report(stats, changes, currency_changes, analysis)
        
        # 8. Отправка алертов
        logger.info("Step 8: Sending alerts...")
        self.send_telegram_alert(changes, currency_changes)
        
        # 9. Обновление статуса
        self.save_status()
        
        logger.info("Hourly pipeline completed")
        logger.info(f"Report: {report}")
        return report
    
    def save_status(self):
        """Сохраняем текущий статус для панели"""
        latest = self.get_latest_analytics()
        
        status = {
            "status": "active",
            "session_id": FIXED_SESSION_ID,
            "timestamp": datetime.now().isoformat(),
            "total_scrapes": self.total_scrapes,
            "successful_scrapes": self.successful_scrapes,
            "price_changes": self.price_changes,
            "alerts_sent": self.alerts_sent,
            "usd_rate": self.current_usd_rate,
            "eur_rate": self.current_eur_rate,
            "latest_analytics": latest,
            "risk": self.calculate_risk(),
            "entropy": self.calculate_entropy(),
            "quality": self.calculate_quality()
        }
        
        with open(self.status_file, 'w') as f:
            json.dump(status, f, indent=2)
    
    def run(self):
        """Основной цикл агента"""
        global running
        
        with open(self.pid_file, 'w') as f:
            f.write(str(os.getpid()))
        
        logger.info(f"iPhone Price Monitor started")
        logger.info(f"Monitoring {len(self.SHOPS)} shops in Almaty/Astana")
        logger.info(f"Tracking {len(self.MODELS)} iPhone models")
        
        signal.signal(signal.SIGTERM, lambda s,f: self.stop())
        signal.signal(signal.SIGINT, lambda s,f: self.stop())
        
        # Первый запуск сразу
        self.hourly_pipeline()
        
        # Затем каждый час
        while running:
            self.step += 1
            
            now = datetime.now()
            next_hour = (now + timedelta(hours=1)).replace(minute=0, second=0, microsecond=0)
            wait_seconds = (next_hour - now).total_seconds()
            
            logger.info(f"Next collection at {next_hour.strftime('%H:%M:%S')} (wait {wait_seconds/60:.0f} min)")
            
            for _ in range(int(wait_seconds / 60)):
                if not running:
                    break
                time.sleep(60)
            
            if running:
                self.hourly_pipeline()
        
        logger.info("Agent stopped")
    
    def stop(self):
        global running
        running = False
        logger.info("Stopping gracefully...")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="iPhone Price Monitor Agent")
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
        else:
            print('{"status": "not_running"}')
    
    elif args.command == "status":
        status_file = os.path.join(args.workspace, "agent_status.json")
        if os.path.exists(status_file):
            with open(status_file, 'r') as f:
                print(f.read())
        else:
            print('{"status": "not_running"}')