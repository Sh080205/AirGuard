# archive — open frame (ถูกแทนด้วยกล่องปิด-เปิดได้ 3 ต.ค. 2569)

> ⚠️ **ARCHIVED 2026-10-09 — ล้าสมัย ห้ามใช้อ้างอิง ดู CLAUDE.md และ [`../README.md`](../README.md)**

ไฟล์หลักปัจจุบันคือ `../airguard_enclosure.scad` · viewer ปัจจุบันคือ `../enclosure_viewer.html`

`../fit_pms.stl` ที่ track อยู่ export จาก `airguard_frame.scad` ในโฟลเดอร์นี้ (`pms_fin_len = 15`) — **มีครีบ**
ตรวจจากไฟล์ STL 9 ต.ค. 2569 (Python อ่าน ASCII STL): ส่วนที่สูงสุด z = 15.2 mm เป็นแผ่นยาว 15.0 mm (x 3.7–18.7) หนา 1.6 mm (y 27.2–28.8)
→ ตรงกับครีบ `pms_fin_len = 15` · `pms_fin_t = 1.6` · bounding box ทั้งชิ้น 55.60 × 54.60 × 15.20 mm
กล่องใช้ `pms_fin_len = 0` (`../airguard_enclosure.scad:109`) → export `fit_pms` จากกล่องจะไม่มีครีบ `[ต้องรันเอง]`
