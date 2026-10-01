#!/usr/bin/env python3
"""下載 Google Sheet 為 CSV (給 agent 整理用)。

Sheet 需設為「知道連結的人可檢視」。支援一般編輯網址:
  python fetch_sheet.py "https://docs.google.com/spreadsheets/d/<ID>/edit#gid=0" > raw.csv
"""
import re, sys, urllib.request

url = sys.argv[1]
m = re.search(r"/spreadsheets/d/([\w-]+)", url)
if not m:
    sys.exit("不是有效的 Google Sheet 網址")
gid = re.search(r"gid=(\d+)", url)
csv_url = f"https://docs.google.com/spreadsheets/d/{m.group(1)}/export?format=csv&gid={gid.group(1) if gid else 0}"
sys.stdout.buffer.write(urllib.request.urlopen(csv_url).read())
