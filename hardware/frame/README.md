# AirGuard-S3 · Open frame (OpenSCAD)

ไฟล์หลัก `airguard_frame.scad` — ทุกมิติเป็นตัวแปรอยู่ด้านบนไฟล์ · ทุกค่าติดป้ายที่มา `[DXF]` `[CLAUDE]` `[MANUAL]` `[TODO]`

## ไฟล์

| ไฟล์ | สถานะ | ใช้ทำอะไร |
|---|---|---|
| `fit_coupon.stl` | ✅ พิมพ์ได้เลย | เลือก `m3_tap_d` (แถวบน 2.4–3.0), รูทะลุ M3 (แถวกลาง 3.2–3.6), ช่องเคเบิลไทร์ `zt_w` (แถวล่าง 2.5–4.5) — ตัวเลขที่แกะไว้ = mm × 10 |
| `fit_board.stl` | ✅ พิมพ์ได้เลย | วงแหวนตามขอบบอร์ด + เสา 4 ต้นตาม DXF → ขันสกรูจริง เช็ครูตรง + ขาใต้บอร์ดไม่ชนฐาน |
| `fit_pms.stl` | ⚠️ ขนาดตาม manual · ตำแหน่งครีบยังเป็น TODO | วาง PMS7003 ลงไป เช็คว่าพอดี และครีบอยู่ระหว่างช่องลมเข้า/ออกจริง |
| `frame_DRAFT_DO_NOT_PRINT.stl` | 🔴 ห้ามพิมพ์ | ร่างจากค่าสมมติ ไว้ดูการจัดวางเท่านั้น |
| `preview_*.png` | — | ภาพจัดวาง (สีโปร่ง = ภาพผีของชิ้นจริง) |
| `frame_viewer.html` | — | 3D viewer เปิดในเบราว์เซอร์ได้เลย: หมุนดู · ป้ายบอกว่าอะไรใส่ตรงไหน · โหมดประกอบทีละขั้น (โหลด three.js จาก jsDelivr ต้องต่อเน็ต) |

## สั่ง export

```bash
openscad -D 'part="fit_coupon"' -o fit_coupon.stl airguard_frame.scad
openscad -D 'part="fit_board"'  -o fit_board.stl  airguard_frame.scad
openscad -D 'part="fit_pms"'    -o fit_pms.stl    airguard_frame.scad
openscad -D 'part="frame"'      -o frame.stl      airguard_frame.scad   # assert จนกว่า ok_* = true ทั้งหมด
```

console จะพิมพ์ระยะสกรู/โมดูล → เสาอากาศ, ระยะ AHT21 → DevKit และรายการ TODO ที่เหลือ

## ลำดับงาน

1. พิมพ์ `fit_coupon` → ใส่ `fit_clear`, `m3_tap_d`, `zt_w`, `zt_t` → `ok_print_fit = true`
2. วัดขาใต้บอร์ดยาวสุด → `under_lead_max` แล้วพิมพ์ `fit_board` ทาบบอร์ดจริง
3. วัดโมดูล (ZE07-CO, ENS160+AHT21, microSD, OLED) + ช่องลม PMS7003 → เปลี่ยนธง `ok_*`
4. `part="frame"` ผ่าน assert ครบ → พิมพ์ชิ้นจริง

## ข้อกำหนด → สิ่งที่ไฟล์ทำ

| ข้อกำหนด | การออกแบบ |
|---|---|
| AHT21 ห่าง ESP32/LDO | อยู่บนเสาตั้งมุมซ้ายบน ห่างกรอบ DevKit (รวม LDO) ~89 mm ในแนวราบ · assert `bme_min_sep` · มีช่องระบายหลังโมดูล |
| ทางลม PMS7003 | หน้าช่องลมหันออกขอบฐาน · ครีบกั้น inlet/outlet ตาม manual V2.5 ("some shield should be placed between inset and outset") · ZE07 อยู่หลังตัว PMS ไม่โดนลมออก (manual ZE07: ไม่ใช้ในที่ลมพัดแรง) |
| USB / J8 / ปุ่ม / microSD | ด้านบนเปิดทั้งหมด · ฐานฝั่งบอร์ดชิดขอบล่างพอดี สาย USB/J8 งอลงได้ · ช่องการ์ด microSD หันออกขอบหน้า |
| LED / buzzer | ด้านบนเปิด ไม่มีอะไรบัง |
| ไม่มีโลหะใกล้เสาอากาศ | ไม่ใช้ insert ทองเหลือง (สกรูเกลียวปล่อยกัดพลาสติก) · โมดูลทุกตัวห่างเสาอากาศ > 50 mm · PMS7003 (เปลือกโลหะ) ห่าง ~85 mm · echo ระยะสกรูทุกตัว |
| ชิ้นทดสอบก่อน | 3 ชิ้นด้านบน |

## ข้อควรรู้จาก manual

- PMS7003: 48 × 37 × 12 mm · ติดตั้งสูงจากพื้นอย่างน้อย 20 cm · **เปลือกโลหะต่อ GND** ห้ามให้แตะส่วนอื่นของวงจร
- `docs/3d.html` ใส่ PMS7003 เป็น 50 × 38 × 21 mm → ไม่ตรงกับ manual

---

## กล่องปิด-เปิดได้ (ตัดสินใจ 3 ต.ค. 2569 — ใช้แทน open frame)

ไฟล์หลัก `airguard_enclosure.scad` (ต่อยอดจากไฟล์ frame ตำแหน่งภายในใช้ชุดเดิม) · viewer `enclosure_viewer.html` (แท็บ "ค่าที่ต้องวัด" กรอกค่าแล้วคัดลอกส่งได้)

- ชิ้นพิมพ์ 2 ชิ้น: `part="enc_body"` + `part="enc_lid"` ยึดด้วยสกรู M3 หัวเรียบ 4 ตัว
- สองห้อง: ห้องเซนเซอร์ (ซ้าย) ลมเข้าร่องล่างด้านหน้า + ผนังหลังตรง AHT21 · ห้องบอร์ด (ขวา) ลมร้อนออกทางฝา
- ผนังซ้ายแนบช่องลม PMS7003 ผ่านฟองน้ำ (แทนครีบ) · ช่อง USB / J8 / microSD / หน้าต่าง OLED อยู่ผนังหน้า · ปุ่มเป็นลิ้นกดบนฝา · LED ผ่านรู Ø3 + ท่อแสง (ต้องทดสอบ)
- export จะ assert จนกว่าธง `ok_*` และ `ok_heights / ok_openings / ok_gasket / ok_oled_win / ok_lightpipe / ok_service` เป็น true
- 🔴 ข้อมูลเทรนต้องเก็บในกล่องตัวจริง — เปลี่ยนตัวเครื่องหลังเก็บข้อมูล = mean/std ตอนเทรนใช้ไม่ได้
