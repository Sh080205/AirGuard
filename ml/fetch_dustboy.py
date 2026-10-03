#!/usr/bin/env python3
"""
fetch_dustboy.py — ดึงข้อมูล DustBoy (CMU CCDC) สำหรับโปรเจกต์ AirGuard-S3

ต้องมี API key ก่อน:
  1. ลงทะเบียนที่ https://open-api.cmuccdc.org/#account
  2. เลือกหน่วยงานสังกัด = "นักศึกษา นักวิจัย", วัตถุประสงค์ = "งานวิจัย"
  3. ยืนยันอีเมล -> รอแอดมินอนุมัติ -> ได้ API key

การใช้งาน:
  export DUSTBOY_API_KEY="xxxxx"
  python3 fetch_dustboy.py nearby   13.11 100.92 20    # หาสถานีใกล้ ม.เกษตร ศรีราชา
  python3 fetch_dustboy.py history  <station_id> 1year
  python3 fetch_dustboy.py monthly  202601

⚠️ เงื่อนไขการใช้งาน: ต้องอ้างอิงแหล่งที่มาทุกครั้งที่นำข้อมูลไปแสดง
   "ข้อมูลจากเครื่องตรวจวัดคุณภาพอากาศ DustBoy — ศูนย์ข้อมูลการเปลี่ยนแปลง
    สภาพภูมิอากาศ มหาวิทยาลัยเชียงใหม่ (CCDC CMU)"

⚠️ API ให้ข้อมูลจุดติดตั้ง "นอกอาคาร" เท่านั้น
   ข้อมูล indoor ต้องขออนุมัติเป็นรายกรณี: dustboy.3e@gmail.com
"""

import os
import sys
import json
import time
import urllib.request
import urllib.error

BASE = "https://open-api.cmuccdc.org/api"
KEY = os.environ.get("DUSTBOY_API_KEY", "")

# พิกัดโดยประมาณของ ม.เกษตรศาสตร์ วิทยาเขตศรีราชา
# ⚠️ ตรวจสอบพิกัดจริงจาก Google Maps ก่อนใช้ อย่าเชื่อค่า default นี้
KU_SRIRACHA = (13.11, 100.92)


def call(path: str, retries: int = 3) -> dict:
    """เรียก API พร้อม Bearer token และ retry แบบ exponential backoff"""
    if not KEY:
        sys.exit("ยังไม่ได้ตั้ง DUSTBOY_API_KEY\n"
                 '  export DUSTBOY_API_KEY="your_key_here"')

    req = urllib.request.Request(
        f"{BASE}{path}",
        headers={"Authorization": f"Bearer {KEY}",
                 "Accept": "application/json"},
    )

    for attempt in range(retries):
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                return json.loads(r.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            # 403 = สิทธิ์ไม่พอสำหรับ endpoint นี้ -> retry ไม่ช่วย
            if e.code == 403:
                sys.exit(f"HTTP 403: บัญชีไม่มีสิทธิ์เรียก {path}\n"
                         "  บัญชี 'ประชาชนทั่วไป' ดึงได้สูงสุด 10 จุด และไม่มีสิทธิ์ย้อนหลังบางระดับ\n"
                         "  ลองสมัครใหม่เป็น 'นักศึกษา นักวิจัย'")
            if attempt == retries - 1:
                raise
            time.sleep(2 ** attempt)
        except urllib.error.URLError:
            if attempt == retries - 1:
                raise
            time.sleep(2 ** attempt)
    return {}


def save(obj, name: str):
    path = f"data/raw/{name}"
    os.makedirs("data/raw", exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=2)
    n = len(obj.get("data", [])) if isinstance(obj, dict) else "?"
    print(f"  บันทึก {path}  ({n} รายการ)")


def cmd_nearby(lat: float, lon: float, dist: float):
    """หาสถานีใกล้พิกัดที่กำหนด — รัศมีสูงสุด 20 กม. ตามที่ API กำหนด"""
    if dist > 20:
        print("⚠️ API จำกัดรัศมีสูงสุด 20 กม. — ปรับเป็น 20")
        dist = 20
    res = call(f"/dustboy/nearme/{lat}/{lon}/{dist}")
    save(res, f"dustboy_nearby_{lat}_{lon}.json")

    print(f"\nสถานีในรัศมี {dist} กม. จาก ({lat}, {lon}):")
    for s in res.get("data", []):
        # ชื่อ field อาจต่างจากนี้ -> ดู JSON ที่บันทึกไว้เพื่อยืนยัน
        print(f"  id={s.get('dustboy_id', s.get('id','?')):>8}  "
              f"pm25={s.get('pm25','?'):>7}  "
              f"{s.get('dustboy_name_th', s.get('name',''))[:50]}")
    print("\n→ เอา id ไปใช้กับคำสั่ง history")


def cmd_history(station_id: str, span: str):
    """ดึงข้อมูลย้อนหลังของสถานีเดียว: 30day | 1year | 5year"""
    ep = {"30day": "data30day", "1year": "data1year", "5year": "data5year"}
    if span not in ep:
        sys.exit(f"span ต้องเป็นหนึ่งใน {list(ep)}")
    res = call(f"/dustboy/{ep[span]}/{station_id}")
    save(res, f"dustboy_{station_id}_{span}.json")


def cmd_monthly(yyyymm: str):
    """ดึงข้อมูลทุกสถานีของเดือนนั้น เช่น 202601

    หมายเหตุ: หน้าเว็บ open-api มีลิงก์รายเดือนแต่ลิงก์ทั้งหมดชี้ไป 202412
    เหมือนกันหมด (น่าจะเป็นบั๊กของเว็บ) จึงต้องประกอบ URL เองแบบนี้
    """
    if not (len(yyyymm) == 6 and yyyymm.isdigit()):
        sys.exit("รูปแบบต้องเป็น YYYYMM เช่น 202601")
    res = call(f"/dustboy/database/{yyyymm}")
    save(res, f"dustboy_all_{yyyymm}.json")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]

    if cmd == "nearby":
        lat = float(sys.argv[2]) if len(sys.argv) > 2 else KU_SRIRACHA[0]
        lon = float(sys.argv[3]) if len(sys.argv) > 3 else KU_SRIRACHA[1]
        dist = float(sys.argv[4]) if len(sys.argv) > 4 else 20
        cmd_nearby(lat, lon, dist)
    elif cmd == "history":
        cmd_history(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else "1year")
    elif cmd == "monthly":
        cmd_monthly(sys.argv[2])
    else:
        sys.exit(__doc__)
