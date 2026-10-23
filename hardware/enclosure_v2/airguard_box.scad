// =====================================================================
//  AirGuard-S3 — กล่องรุ่น 2 (v2) : พิมพ์ง่าย · ไม่ใช้ support · ไม่ใช้เคเบิลไทร์
//  OpenSCAD 2021.01+ · หน่วย mm · ทุกมิติอยู่ส่วนพารามิเตอร์ด้านบน
//
//  ออกแบบใหม่ 17 ต.ค. 2569 โดยยืมรูปแบบจากงานที่มีคนพิมพ์จริงแล้ว
//    - Printables 52282 "Indoor air quality monitor (snap-fit)" : ห้อง T/RH แยกที่มุม มีช่องลมออกนอกกล่อง
//    - Sensorbox v2 (Printables 1079858)                          : กั้นความร้อนรอบเซนเซอร์อุณหภูมิ · ไม่ใช้ support
//    - Adafruit "Air Quality Sensor Silo House"                    : PMS กดลงหลุมพอดีตัว · ชิ้นย่อยเปลี่ยนตามโมดูล
//    - AirGradient ONE                                             : ฝาบน-ล่างยึดสกรู 4 ตัว
//    - Hubs "CAD modeling for enclosures"                          : ผนัง 2.4 = 6 × หัว 0.4 · ขอบฝาเว้นช่อง · ปุ่มชิ้นแยก
//
//  ค่ามิติอุปกรณ์ทั้งหมดมาจาก docs/bringup/2026-10-11.md (วัดเวอร์เนียร์ 11 ต.ค.) และ DXF ของ carrier
//  ป้าย: [วัด 11 ต.ค. รหัส]  [DXF]  [MANUAL]  [แจ้ง 17 ต.ค.]  [ออกแบบ] = ค่าที่เลือกเอง ไม่ใช่ค่าวัด
//        [TODO ok_xxx] = ยังไม่มีแหล่ง → ห้ามสั่งพิมพ์ body/lid จนกว่าธงเป็น true
//
//  ระบบพิกัด: (0,0) = มุมซ้ายหน้าของ carrier (J8/BZ1 = หน้า, J7 = หลัง) · +x ขวา · +y หลัง · z = 0 ใต้กล่อง
//
//  export (ดู README.md):
//    openscad -D 'part="fit_coupon"' -o fit_coupon.stl airguard_box.scad      ← พิมพ์ได้เลย
//    openscad -D 'part="fit_pms"'    -o fit_pms.stl    airguard_box.scad      ← พิมพ์ได้เลย
//    openscad -D 'part="fit_lid"'    -o fit_lid.stl    airguard_box.scad      ← พิมพ์ได้เลย
//    openscad -D 'part="fit_oled"'   -o fit_oled.stl   airguard_box.scad      ← พิมพ์ได้เลย (เช็คมิติจอที่ประมาณไว้)
//    openscad -D 'part="body"'  -o body.stl  airguard_box.scad   ┐ assert จนกว่าธง ok_* ครบ
//    openscad -D 'part="lid"'   -o lid.stl   airguard_box.scad   │ (-D allow_draft=true = ดูร่างเท่านั้น)
//    openscad -D 'part="pegs"'  -o pegs.stl  airguard_box.scad   │
//    openscad -D 'part="sled"'  -o sled.stl  airguard_box.scad   ┘ (เฉพาะ sd_mode = "sled")
//    เปลี่ยนแบบช่อง SD: -D 'sd_mode="integrated"'
// =====================================================================

/* [Build] */
part        = "preview";  // body | lid | pegs | sled | fit_coupon | fit_pms | fit_lid | fit_oled | preview | exploded
sd_mode     = "sled";     // "sled" = ถาดแยกชิ้น (พิมพ์กล่องก่อน ถาดทีหลัง) · "integrated" = หลุมในกล่อง · "none"
allow_draft = false;      // true = ยอมเรนเดอร์ชิ้นจริงทั้งที่มี TODO (ดูเท่านั้น ห้ามพิมพ์)
led_window  = "hole";     // "hole" = รูทะลุ (ใช้ได้ทุกสี) · "skin" = ผิวบาง 0.6 ให้แสงลอด (ใช้ได้เฉพาะเส้นสีอ่อน)
$fn = 48;

// =====================================================================
// 0) ธงยืนยัน — เปลี่ยนเป็น true เมื่อมีหลักฐานแล้วเท่านั้น (ผู้วัดไม่ตั้งเอง — CLAUDE.md)
// =====================================================================
ok_print_fit   = true;  // 23 ต.ค. เจ้าของเลือก "ค่าเผื่อแบบปลอดภัย" แทนการพิมพ์ fit_coupon/fit_lid (ไม่มีเวลา) → ยังไม่เคยลองกับเครื่องจริง
ok_printer     = false; // H1 H5 — รุ่นเครื่อง + พื้นที่พิมพ์ ≥ ขนาดกล่อง
ok_service     = false; // H2 H3 H4 — nozzle/ชั้น · วัสดุ (ห้ามเส้นผสมโลหะ/คาร์บอน ใกล้เสาอากาศ) · ค่าเผื่อ
ok_pms_ports   = false; // ผู้ใช้แจ้ง 17 ต.ค.: inlet = ช่องกว้าง → ยืนยันกับรูปใน PMS7003 manual V2.5 แล้วจดเลขหน้า
ok_pms_conn    = false; // 23–24 ต.ค.: หัวสาย + แผ่นแปลงอยู่มุมบน ปลายฝั่งช่องเล็ก ยื่นเลยหน้าช่องลม 5.64 · เลยปลาย 10.84
                        // → แบบ ก. (pms_front_gap + pms_side_gap + อุโมงค์) · เหลือ P4 ความสูงแผ่นเหนือผิวบน PMS (pms_board_z)
ok_openings    = true;  // 18 ต.ค. เจ้าของเลือก "เจาะเผื่อกว้าง" แทนการวัด A8/A10b → usb_cut 32 × 13 · j8_d 10 (ดูด้านล่าง)
ok_sd_dims     = false; // โมดูล microSD ตัวใหม่ (มา ~20 ต.ค.) → E1a–E2c + ทิศขา header
ok_ze07_mount  = false; // ความสูงตอขา/รอยบัดกรีใต้ PCB ZE07 หลังตัดขา → ze07_under
ok_bme_pins    = false; // D5 ตำแหน่งชิป · ความสูงแถวพินจากขอบล่าง PCB (bme_pin_z)
ok_cable_h     = false; // A12 วัดซ้ำเมื่อเสียบสายครบ (สายที่งอขึ้นต้องต่ำกว่าใต้ฝา)
ok_ant_keepout = false; // K6 รัศมีเว้นโลหะรอบเสาอากาศ จาก ESP32-S3-WROOM-1 datasheet
ok_bme_sep     = false; // K7 ระยะ AHT21 ↔ ESP32/LDO (ยังไม่มีแหล่งอ้างอิง)
ok_oled_dims   = true;  // 23 ต.ค. เจ้าของตั้งตามค่าวัด O0–O8 (docs/bringup/2026-10-23.md · วัดครั้งเดียว) — ตรวจเฉพาะเมื่อ use_oled

// =====================================================================
// 1) Carrier PCB + DevKit — [DXF] + [วัด 11 ต.ค.]
// =====================================================================
pcb_w = 82.126; pcb_h = 72.921;                         // [DXF] ✅ X1/X2 วัดตรง DXF
pcb_holes = [[3.810, 3.683], [78.232, 3.683], [78.359, 69.596], [3.937, 69.215]]; // [DXF] M3
pcb_t = 1.65;                 // [วัด A1]
under_lead_max = 4.14;        // [วัด A2] ขา THT ใต้บอร์ด (ค่าเผื่อสูง = ผิดทางปลอดภัย)
standoff_h  = under_lead_max + 2.0;   // ผิวพื้น → ใต้ carrier
standoff_od = 7.0;
devkit_rect = [[33.911, 3.171], [59.311, 65.911]];      // [DXF] กรอบ U1
ant_ref     = [(33.911 + 59.311) / 2, 65.911];          // กึ่งกลางขอบปลายเสาอากาศ (ฝั่ง J7)
socket_h  = 11.08;   // [วัด A4a] ผิวบน carrier → ใต้ PCB DevKit
devkit_t  = 1.56;    // [วัด A4b]
devkit_top = 15.51;  // [วัด A4c] ผิวบน carrier → ยอดฝาโลหะ WROOM (ชิ้นสูงสุด)
usb_x   = [39.69, 53.43];  // [วัด A6] กึ่งกลางปาก USB สองช่อง
usb_dz  = 1.23;            // [วัด A5] ผิวบน DevKit → กึ่งกลางปาก USB
j8_x    = 21.08;           // [DXF]
j8_dz   = 3.94;            // [วัด A10a] ผิวบน carrier → กึ่งกลางรูสาย J8
jst_h   = 9.60;            // [วัด A12] หัว JST (ยังไม่เสียบสาย) → ok_cable_h
sw_xy   = [[74.93, 34.41], [75.95, 25.41], [66.29, 25.02]];  // [DXF] SW1–SW3
sw_h    = 1.35;            // [วัด A11] ผิวบน carrier → ปลายปุ่ม (ระยะกด 0.11)
led_xy  = [[70.99, 61.47], [71.32, 45.67], [71.12, 50.67], [70.99, 55.50]]; // [DXF] LED1–4
led_h   = 0.76;            // [วัด K1]
bz_xy   = [67.82, 11.68];  // [DXF] BZ1

// =====================================================================
// 2) การพิมพ์ — [TODO ok_print_fit / ok_service] ตั้งจาก fit_coupon + fit_lid
// =====================================================================
fit_clear = 0.3;     // [ออกแบบ] ช่องว่างรอบโมดูลแต่ละด้าน
m3_tap_d  = 2.5;     // [ปลอดภัย 23 ต.ค. เดิม 2.7] รูสกรู M3 เกลียวปล่อย · แน่นไป → คว้านดอก 2.8 (ไม่ใช้ insert ทองเหลือง = ไม่เพิ่มโลหะ)
m3_thru_d = 3.4;     // รูสกรูทะลุฝา
lid_clear = 0.5;     // [ปลอดภัย 23 ต.ค. เดิม 0.4] ช่องว่างขอบฝา ↔ ผนังใน ต่อด้าน (Hubs แนะนำ 0.5) · หลวมได้ ฝายึดด้วยสกรู 4 ตัว
peg_clear = 0.35;    // [ปลอดภัย 23 ต.ค. เดิม 0.25] ช่องว่างแท่งกดปุ่ม ↔ ปลอก ต่อด้าน · หลวม = แท่งยังกดปุ่มได้
bed_x = 220; bed_y = 220;  // [TODO ok_printer]

// =====================================================================
// 3) กล่อง — [ออกแบบ]
// =====================================================================
wall_t = 2.4; floor_t = 2.4; lid_t = 2.4;   // 6 × หัว 0.4 (Hubs) · 12 ชั้นที่ 0.2
lid_lip = 3.0; lip_t = 1.6;                 // ขอบฝาที่สอดลงในกล่อง
box_r  = 5.0;                               // รัศมีมุมแนวตั้ง (ด้านนอก)
in_r   = 1.0;                               // รัศมีมุมด้านใน — ต้อง ≤ 1.0 ไม่งั้นมุมโค้งชนมุม PMS ที่แนบผนัง 0.3
chamfer = 0.6;                              // ลบมุมขอบที่ติดฐานพิมพ์ (กัน elephant foot)
in_h   = 32;                                // ผิวพื้น → ใต้ฝา (เหลือเหนือ DevKit 8.7 — ตรวจที่ echo)
boss_d = 7.0;

// ---- ผนังกั้นห้องบอร์ด | ห้องเซนเซอร์ ----
div_x = -2.5; div_t = 1.6;
div_notch_y = [6, 70];        // ช่องร้อยสายจาก J1–J6 (DXF: J5 y12 · J1 y23–41 · J3 y43–51 · J2 y58–68)
div_curtain_z = 22;           // ฝามีแผ่นห้อยลงปิดครึ่งบนของช่อง → ลมร้อนชั้นบนจากห้องบอร์ดผ่านไม่ได้

// ---- PMS7003 ---- [วัด X4] 47.94 × 36.92 × 12 · ตรงกับ [MANUAL] 48 × 37 × 12
pms_L = 47.94; pms_W = 36.92; pms_H = 12;
// [วัด B3/B4] [กึ่งกลางตามแนวหน้า (จากปลาย -y), กว้าง, สูง, กึ่งกลางความสูงจากผิวล่างตัวถัง]
pms_outlet = [ 9.68,  9.74, 1.94, 2.15];   // ช่องเล็ก  = outlet [แจ้ง 17 ต.ค.] → ok_pms_ports
pms_inlet  = [36.06, 10.61, 4.02, 3.51];   // ช่องใหญ่ = inlet  [แจ้ง 17 ต.ค.]
pms_port_margin = 1.0;        // เผื่อรอบปากช่องบนผนัง ไม่ให้ขอบผนังบัง
// หน้าช่องลมแนบผนังซ้าย (manual: ระนาบ inlet/outlet ชิดผนังเครื่อง) ผนังระหว่างสองช่อง = แผ่นกั้น
pms_post_h = 8;
// แผ่นแปลงสีเขียว (บัดกรีสายแล้ว) เสียบหัวสายที่มุมบน ปลายฝั่งช่องเล็ก — รูป + วัด 23 ต.ค. · เจ้าของเลือกแบบ ก. 24 ต.ค.
pms_board = [5.64, 10.84];   // [วัด P1] ยื่นเลยหน้าช่องลม (ไม่รวมสาย) · ยื่นเลยปลาย PMS ฝั่งช่องเล็ก
pms_board_in = 15;           // [ประมาณจากรูป] แผ่นกินเข้ามาจากหน้าช่องลม (แนว x) — ใช้ตรวจชนเท่านั้น
pms_board_ov = 4;            // [ประมาณจากรูป] แผ่นทับบนผิวบน PMS จากปลาย (แนว y)
pms_board_z  = [0, 10];      // [TODO P4 · ok_pms_conn] ใต้แผ่น/รอยบัดกรี · จุดสูงสุดรวมสายที่งอขึ้น — วัดจากผิวบน PMS
pms_front_gap = 12.0;        // [ออกแบบ] ผนังหน้า → ปลาย PMS = แผ่นยื่น 10.84 + เผื่อ 1.16
pms_side_gap  = 6.2;         // [ออกแบบ] ผนังซ้าย (ใน) → หน้าช่องลม = แผ่นยื่น 5.64 + เผื่อ 0.56 · ช่วงตัว PMS เติมเนื้อเป็นอุโมงค์ช่องลม
pms_back_gap = 0.6;   // [ออกแบบ] PMS → ผนังกันความร้อนชั้นแรก (ถ้าหัวสายอยู่หน้านี้ ให้เพิ่มเป็นความยาวหัว+สาย)
// ผนังสองชั้นกันความร้อนจาก PMS เข้าห้อง T/RH (แบบ Sensorbox v2) — 17 ต.ค.
// PMS7003 manual V2.5 Technical Index: Active Current ≤ 100 mA ที่ 5 V → ≤ 0.5 W (ค่าสูงสุด ไม่ใช่ค่าวัด)
pms_wall2_t = 1.2;    // ผนังชั้นแรก (ฝั่ง PMS) = 3 เส้นของหัว 0.4
therm_gap   = 3.0;    // [ออกแบบ] ช่องอากาศคั่นระหว่างสองผนัง (เปิดด้านปลายสู่ห้องเซนเซอร์ ไม่เปิดสู่ PMS)

// ---- ZE07-CO ---- [วัด C1/X6] ตัดขาแล้วบัดกรีสายตรง (เจ้าของตัดสิน 11 ต.ค.)
ze07_dx = 22.32; ze07_dy = 25.44; ze07_H = 18.41;    // PCB 1.70 + กระบอก 16.71
ze07_pcb_t = 1.70;
ze07_under = 3.0;     // [TODO ok_ze07_mount] ที่ว่างใต้ PCB ให้ตอขา + รอยบัดกรี (ขาเดิมยื่น 5.96 ก่อนตัด)
ze07_lip   = 0.4;     // ปากล็อกเล็กๆ บนเสามุม — กดโมดูลลงให้ "แป๊ะ" (ถ้าแน่นไปตะไบออกได้)
ze07_y0    = 25.5;    // [ออกแบบ] หลังถาด SD (ถาดยาวถึง y≈22.8 + เสา 1.9)

// ---- ENS160 + AHT21 ในห้องแยกที่มุมซ้ายหลัง ---- [วัด D1/D2]
// แบบ ก (เจ้าของตัดสิน 11 ต.ค.): แถวพินอยู่ขอบล่าง → ชิปสองตัวเรียงแนวนอน ไอร้อน ENS160 ไม่ลอยผ่าน AHT21
// โมดูลตั้งตรงในร่องนำ หันหน้าชิปไปทางผนังซ้ายที่มีช่องลม · ขาพินชี้ +x ลอดใต้แผ่นกั้นออกไปห้องเซนเซอร์
bme_W = 24.14; bme_L = 19.38; bme_T = 1.57; bme_pins = 8.54;
bme_lift  = 3.0;      // [ออกแบบ] ขอบล่าง PCB สูงจากพื้น — ให้หัว dupont มีที่
bme_pin_z = 1.5;      // [TODO ok_bme_pins] กึ่งกลางแถวพิน เหนือขอบล่าง PCB
bme_gap   = 8.5;      // [ออกแบบ] ผนังซ้าย → หน้าชิป (อากาศไหลผ่านหน้าเซนเซอร์)
groove_d  = 1.2;      // ความลึกร่องนำขอบ PCB

// ---- microSD ---- [วัด E1/E2 จากโมดูลตัวเก่า] → โมดูลใหม่มา ~20 ต.ค. ต้องวัดใหม่ (ok_sd_dims)
sd_dx = 17.83; sd_dy = 17.71; sd_H = 3.61;
sd_overhang = 2.5;                 // ซ็อกเก็ตยื่นพ้นขอบ PCB ไปทาง -y
sd_slot = [2.10, 13.67, 2.73];     // ขอบซ้ายโมดูล→ขอบซ้ายปาก, กว้างปาก, ผิวที่โมดูลวาง→ขอบบนปาก
sd_finger = 14; sd_finger_up = 4.0; sd_finger_r = 1.2;  // รอยบากให้นิ้วจับการ์ด
// ถาด (sled)
sled_fp_w = 25; sled_fp_h = 12; sled_fp_t = 2.0; sled_tray_t = 1.2;
sled_open = [19, 9];               // ช่องบนผนังหน้า (กว้าง × สูง) ใหญ่กว่าปากการ์ด → รองรับโมดูลใหม่ที่ตำแหน่งต่างได้ ±2.6
sled_peg_d = 3.0; sled_peg_h = 1.0;   // หมุดต่ำกว่าผิวถาด (ถาดหนา 1.2) ไม่โผล่ชนโมดูล

// ---- ปุ่ม: แท่งกดแยกชิ้น (Hubs: ชิ้นแยกเล็กกว่ารู) ----
peg_d = 6.0; peg_flange_d = 8.0; peg_foot_d = 2.5; peg_proud = 1.2; tube_len = 10; tube_wall = 0.8;

// ---- LED ----
led_bore = 3.2; led_hole = 2.5; led_csk = 4.5; led_skin = 0.6;

// ---- OLED SSD1306 (I²C 0x3C) — เพิ่ม 23 ต.ค. 2569 ตามที่เจ้าของแจ้ง (แทนข้อตัดสินใจ 9 ต.ค. ที่ไม่ใช้ OLED) ----
// ติดใต้ฝาเหนือหลุม PMS จอหันขึ้น อ่านจากด้านหน้า (-y) · ขอบหัวขาอยู่ฝั่ง +y
//   เลือกตรงนี้เพราะ: ใต้ฝาเหนือ PMS ว่าง 20 mm (ที่อื่นเหลือ 8.7–10.6) · อยู่นอกห้อง T/RH มีผนังสองชั้นกั้น · ห่างเสาอากาศ ~100 mm
//   ผนังหน้าใช้ไม่ได้: เหนือ PMS เหลือสูง 20 (PCB จอ ~27.8) · ฝั่ง carrier ห่างผนังใน 2 mm
// มิติจอ [วัด 23 ต.ค. Ox] = เวอร์เนียร์ โมดูล 0.96" ตัวจริง (docs/bringup/2026-10-23.md) · ขาเรียง GND VCC SCL SDA → สายไขว้กับ J3–J5
use_oled      = true;
oled_pcb      = [27.79, 27.95];      // [วัด O1] x = แนวตัวหนังสือ · y = ขอบหัวขา ↔ ขอบล่าง
oled_pcb_t    = 1.11;                // [วัด O2]
oled_glass    = [25.85, 19.28, 2.07];// [วัด O3] กระจก กว้าง, สูง · หนาจากผิว PCB = 3.18 (PCB+กระจก) − 1.11 · อยู่กลางแนว x (O4b)
oled_glass_dy = 4.37;                // [วัด O4] ขอบ PCB ฝั่งหัวขา → ขอบบนกระจก = 23.65 − 19.28
oled_va       = [21.53, 11.25];      // [วัด O5] พื้นที่พิกเซล (เห็นตอนเอียงใต้ไฟ) · อยู่กลางแนว x
oled_va_dy    = 1.715;               // [วัด O5c] ขอบบนกระจก → ขอบบนพิกเซล 2.3 ⇒ กึ่งกลางพิกเซลเยื้องเข้าหาหัวขา 9.64 − 7.925
oled_back_h   = 11.3;                // [วัด O6 8.26 + ประมาณ 3] หลัง PCB → ปลายขาตรง 8.26 + สายบัดกรีที่ปลายขา/โค้งสาย ~3
                                     //   เสียบหัว dupont ตรง ≈ 2.5 + 14 + โค้งสาย 5 = 21.5 → ไม่พอ (มีที่ 16.6) ต้องบัดกรีสายที่ขาหรือดัดขา 90°
oled_win_m    = 1.0;                 // [ออกแบบ] หน้าต่างกว้างกว่าพื้นที่พิกเซลข้างละ 1.0 (มุมมองเฉียง + จอวางคลาด) · ยังเหลือขอบกระจกบังข้างละ ≥ 1.16
oled_pad      = [5, 2.2];            // [ออกแบบ] แผ่นรองมุม PCB (นอกกระจก · ไม่ทับจุดบัดกรีหัวขาที่อยู่กลางขอบ)
oled_gap      = 0.2;                 // [ออกแบบ] กระจก ↔ ใต้ฝา — ฝาไม่กดกระจก แรงลงที่แผ่นรองมุม
oled_frame_t  = 1.0;                 // [ออกแบบ] ผนังกรอบ (บาง ให้ลิ้นสแนปงอได้)
oled_lip      = 0.6;                 // [ออกแบบ] ปากสแนปยื่นเข้า (fit_clear 0.3 → ล็อกได้ 0.3)
oled_tab_len  = 10;                  // [ออกแบบ] ความยาวลิ้นสแนปซ้าย/ขวา

// ---- เสาอากาศ ---- [TODO ok_ant_keepout]
ant_keepout_r = 15;
bme_min_sep   = 30;   // [TODO ok_bme_sep] ไม่มีแหล่งอ้างอิง

// =====================================================================
// 4) เรขาคณิตที่คำนวณได้ — ไล่จากผนังกั้นออกไปทางซ้าย
// =====================================================================
zTop  = floor_t + standoff_h + pcb_t;            // ผิวบน carrier
zDK   = zTop + socket_h;                          // ใต้ PCB DevKit
usb_zc = zDK + devkit_t + usb_dz;                 // กึ่งกลางปาก USB
lid_z = floor_t + in_h;                           // ใต้ฝา = บนผนัง

in_y0 = -2.0;                 // ผนังหน้า (ใน) — ใกล้ปาก USB ให้หัวปลั๊กเสียบถึง
in_x1 = pcb_w + 0.5 + boss_d; // ผนังขวา (ใน) — เสาสกรูมุมขวาอยู่นอกขอบ carrier ทั้งหมด → ใส่/ขันบอร์ดได้

// คอลัมน์ ZE07 + SD (ระหว่าง PMS กับผนังกั้น)
post_t = 1.6; post_leg = 4.0;
ze07_x1 = div_x - div_t / 2 - 0.5 - post_t - fit_clear;
ze07_x0 = ze07_x1 - ze07_dx;
ze07_rect = [[ze07_x0, ze07_y0], [ze07_x1, ze07_y0 + ze07_dy]];
col_x0 = ze07_x0 - fit_clear - post_t;            // ขอบซ้ายคอลัมน์
col_x1 = div_x - div_t / 2;
col_cx = (col_x0 + col_x1) / 2;

// เสาสกรูหน้าซ้าย อยู่ระหว่าง PMS กับคอลัมน์ (เต็มความสูง ไม่ค้ำเหนือ PMS → ยก PMS เข้า-ออกตรงๆ ได้)
boss_fl = [col_x0 - 0.3 - boss_d / 2, in_y0 + boss_d / 2];

// PMS: ช่องลมหัน -x แนบผนังซ้าย
pms_x1 = boss_fl[0] - boss_d / 2 - 0.5;
pms_x0 = pms_x1 - pms_W;
pms_y0 = in_y0 + fit_clear + pms_front_gap;
pms_y1 = pms_y0 + pms_L;
pms_rect = [[pms_x0, pms_y0], [pms_x1, pms_y1]];
in_x0 = pms_x0 - pms_side_gap;
// แผ่นแปลง PMS (ใช้ตรวจชน + ภาพผี) — มุมบน ปลายฝั่งช่องเล็ก
pms_board_rect = [[pms_x0 - pms_board[0], pms_y0 - pms_board[1]], [pms_x0 + pms_board_in, pms_y0 + pms_board_ov]];

// ห้อง ENS160+AHT21 (มุมซ้ายหลัง)
dw_y0 = pms_y1 + pms_back_gap;                    // ผนังกันความร้อนชั้นแรก (dw_y0 → +pms_wall2_t)
ch_y0 = dw_y0 + pms_wall2_t + therm_gap;           // ผนังหน้าห้อง ENS (ch_y0 → +1.6)
ch_wall_t = 1.6;
ch_in_y0 = ch_y0 + ch_wall_t;
// ผนังหลัง (ใน): พอให้ทั้ง carrier (+1 mm) และห้อง ENS (โมดูล + ร่องนำ + เผื่อ 0.74 เท่าแบบก่อน)
in_y1 = max(pcb_h + 1.0, ch_in_y0 + bme_W + 2 * fit_clear + 0.74);
bme_x0 = in_x0 + bme_gap;        // หน้าชิป
bme_x1 = bme_x0 + bme_T;         // หลัง PCB
bme_yc = (ch_in_y0 + in_y1) / 2;
bme_z0 = floor_t + bme_lift;     // ขอบล่าง PCB
bme_z1 = bme_z0 + bme_L;
curt_x0 = bme_x1 + 3.0; curt_x1 = curt_x0 + 1.6;   // แผ่นกั้นห้อง (ห้อยจากฝา) + ธรณีต่ำ (จากพื้น)
pin_zc  = bme_z0 + bme_pin_z;
sill_z1 = pin_zc - 1.5 - 0.5 + 0.0;                // ใต้หัว dupont
curt_z0 = pin_zc + 1.5 + 1.0;                      // เหนือหัว dupont
g0 = bme_yc - (bme_W / 2 + fit_clear);             // ก้นร่องนำ
g1 = bme_yc + (bme_W / 2 + fit_clear);

// microSD
sd_x0 = col_cx - sd_dx / 2; sd_x1 = sd_x0 + sd_dx;
sd_int_y0 = in_y0 + fit_clear + sd_overhang;       // integrated: ขอบหน้า PCB (ซ็อกเก็ตแนบผนังใน)
sled_fp_x0 = col_cx - sled_fp_w / 2; sled_fp_x1 = sled_fp_x0 + sled_fp_w;
sled_fp_y0 = in_y0 + 0.2; sled_fp_y1 = sled_fp_y0 + sled_fp_t;
sd_sled_y0 = sled_fp_y1 + sd_overhang;             // sled: ขอบหน้า PCB (ซ็อกเก็ตแนบแผ่นหน้า)
sled_tray_y1 = sd_sled_y0 + sd_dy + fit_clear + post_t + 0.5;   // ถาดยาวพ้นเสาหลัง (เสาไม่ลอยเลยขอบถาด)
sled_pegs = [[sled_fp_x0 + 1.8, sled_fp_y1 + 9], [sled_fp_x1 - 1.8, sled_fp_y1 + 9]];
sd_y0 = (sd_mode == "sled") ? sd_sled_y0 : sd_int_y0;
sd_z0 = floor_t + ((sd_mode == "sled") ? sled_tray_t : 0);

// OLED (ใต้ฝา เหนือ PMS)
oled_cx = (pms_x0 + pms_x1) / 2;
oled_x0 = oled_cx - oled_pcb[0] / 2; oled_x1 = oled_x0 + oled_pcb[0];
oled_y0 = in_y0 + 4.5;                              // ขอบล่างจอ (ฝั่งผู้ดู) — พ้นขอบสอดฝา
oled_y1 = oled_y0 + oled_pcb[1];                    // ขอบหัวขา
oled_rect = [[oled_x0, oled_y0], [oled_x1, oled_y1]];
oled_gy1 = oled_y1 - oled_glass_dy; oled_gy0 = oled_gy1 - oled_glass[1];   // ขอบกระจก (แกน y)
oled_va_c = [oled_cx, (oled_gy0 + oled_gy1) / 2 + oled_va_dy];
oled_win  = oled_va + 2 * oled_win_m * [1, 1];
oled_zf = lid_z - oled_gap - oled_glass[2];         // ผิวหน้า PCB (แตะแผ่นรองมุม)
oled_zb = oled_zf - oled_pcb_t;                     // ผิวหลัง PCB (แตะปากสแนป)
oled_frame_z0 = oled_zb - 0.3 - 2 * oled_lip - 0.2; // ปลายล่างกรอบ
oled_fo = fit_clear + oled_frame_t;
oled_pk = [oled_rect[0] - [oled_fo, oled_fo], oled_rect[1] + [oled_fo, oled_fo]];   // กรอบรวมผนัง
// ซี่กด PMS จากฝา [x0, y0, กว้าง, ยาว] — มี OLED: ย้ายไปครึ่งหลัง + เพิ่มซี่หน้าขวา (3 จุด)
pms_cx = (pms_x0 + pms_x1) / 2; pms_cy = (pms_y0 + pms_y1) / 2;
rib_by0 = oled_pk[1][1] + 1.5;
pms_ribs = use_oled
    ? [[pms_cx - 10, rib_by0, 2, pms_y1 - 1.5 - rib_by0], [pms_cx + 8, rib_by0, 2, pms_y1 - 1.5 - rib_by0],
       [oled_pk[1][0] + 0.5, pms_y0 + 2, 2, 10]]
    : [[pms_cx - 10, pms_cy - 6, 2, 12], [pms_cx + 8, pms_cy - 6, 2, 12]];

bosses = [boss_fl,
          [in_x0 + boss_d / 2, in_y1 - boss_d / 2],
          [in_x1 - boss_d / 2, in_y0 + boss_d / 2],
          [in_x1 - boss_d / 2, in_y1 - boss_d / 2]];
boss_att = [[[0, -1]], [[-1, 0], [0, 1]], [[1, 0], [0, -1]], [[1, 0], [0, 1]]];  // ผนังที่เสายึด

outer_bb = [[in_x0 - wall_t, in_y0 - wall_t], [in_x1 + wall_t, in_y1 + wall_t]];
inner_bb = [[in_x0, in_y0], [in_x1, in_y1]];
box_size = [outer_bb[1][0] - outer_bb[0][0], outer_bb[1][1] - outer_bb[0][1], lid_z + lid_t];

ens_rib_y0 = bosses[1][1] - boss_d / 2 - 6.5;   // ซี่กดขอบบนโมดูล ENS (ใต้ฝา)
peg_top  = lid_z + lid_t + peg_proud;
peg_bot  = zTop + sw_h;
tube_z0  = lid_z - tube_len;
flange_z1 = tube_z0 - 1.5;           // ยอดกรวยของปีกแท่งกด (ต่ำกว่าปลายปลอก 1.5 = ระยะยกได้)

// =====================================================================
// 5) ตรวจ TODO + รายงานตัวเลข (ดู console)
// =====================================================================
todo = [
    if (!ok_print_fit)   "m3_tap_d / lid_clear / peg_clear (พิมพ์ fit_coupon + fit_lid)",
    if (!ok_printer)     "bed (H1 H5)",
    if (!ok_service)     "nozzle / วัสดุ / ค่าเผื่อ (H2 H3 H4)",
    if (!ok_pms_ports)   "PMS inlet/outlet — ยืนยันกับ manual V2.5",
    if (!ok_pms_conn)    "PMS แผ่นแปลง P4 (ความสูงเหนือผิวบน PMS)",
    if (!ok_openings)    "usb_cut / j8_d (A8 A10b)",
    if (sd_mode == "integrated" && !ok_sd_dims) "microSD ตัวใหม่ (E1 E2)",   // แบบถาด: รอเฉพาะ part="sled" (body/lid ไม่ใช้มิติโมดูล)
    if (!ok_ze07_mount)  "ze07_under (ตอขาใต้ PCB)",
    if (!ok_bme_pins)    "bme_pin_z / ตำแหน่งชิป (D5)",
    if (!ok_cable_h)     "A12 สายงอขึ้น",
    if (!ok_ant_keepout) "ant_keepout_r (K6)",
    if (!ok_bme_sep)     "bme_min_sep (K7)",
    if (use_oled && !ok_oled_dims) "OLED O1–O7 (มิติประมาณจากโมดูล 0.96\" ทั่วไป)"
];
// [ออกแบบ 18 ต.ค.] เจาะเผื่อกว้างแทนการวัดหัวปลั๊ก: ช่องเดียวครอบสองพอร์ต
// กว้าง 32 = ระยะสองพอร์ต 13.74 + เผื่อข้างละ ~9 · สูง 13 (กึ่งกลางปาก USB ±6.5) เหลือผนังเหนือช่อง ~3.8 ใต้ฝา
usb_cut = [32, 13];
j8_d    = 10;                              // [ออกแบบ 18 ต.ค.] รูสายไฟ J8 เผื่อกว้าง — ใส่ปลอกยาง/มัดปมกันกระชากด้านใน

function in_rect(p, r) = p[0] >= r[0][0] && p[0] <= r[1][0] && p[1] >= r[0][1] && p[1] <= r[1][1];
function ov(a, b) = a[0][0] < b[1][0] && b[0][0] < a[1][0] && a[0][1] < b[1][1] && b[0][1] < a[1][1];
function brect(b) = [b - [boss_d / 2, boss_d / 2], b + [boss_d / 2, boss_d / 2]];
function pt_rect_dist(p, r) = let(
    dx = max(r[0][0] - p[0], 0, p[0] - r[1][0]),
    dy = max(r[0][1] - p[1], 0, p[1] - r[1][1])) sqrt(dx * dx + dy * dy);

ens_rect  = [[bme_x0, g0], [bme_x1 + bme_pins, g1]];
sd_rect   = [[sd_x0, sd_y0 - sd_overhang], [sd_x1, sd_y0 + sd_dy]];
sled_rect = [[sled_fp_x0, sled_fp_y0], [sled_fp_x1, sled_tray_y1]];
ze07_pk   = [ze07_rect[0] - [fit_clear + post_t, fit_clear + post_t], ze07_rect[1] + [fit_clear + post_t, fit_clear + post_t]];
carrier   = [[0, 0], [pcb_w, pcb_h]];
chk = [["PMS", pms_rect], ["ZE07 pocket", ze07_pk], ["ENS+pins", ens_rect], ["carrier", carrier],
       if (sd_mode == "sled") ["SD sled", sled_rect], if (sd_mode == "integrated") ["SD", sd_rect],
       for (i = [0 : 3]) [str("boss", i), brect(bosses[i])]];

echo(str("BOX outer = ", box_size[0], " x ", box_size[1], " x ", box_size[2], " mm  (bed ", bed_x, " x ", bed_y, ")"));
echo(str("lid underside z = ", lid_z, " · ใต้ฝาเหลือเหนือ: DevKit ", lid_z - (zTop + devkit_top),
         " · JST(ไม่รวมสาย) ", lid_z - (zTop + jst_h), " · ENS ", lid_z - bme_z1,
         " · ZE07 ", lid_z - (floor_t + ze07_under + ze07_H), " · PMS ", lid_z - (floor_t + pms_H)));
for (i = [0 : len(chk) - 2]) for (j = [i + 1 : len(chk) - 1])
    if (ov(chk[i][1], chk[j][1])) echo(str("!! CLASH 2D: ", chk[i][0], " ↔ ", chk[j][0]));
for (c = chk) if (!(c[1][0][0] >= in_x0 - 0.01 && c[1][1][0] <= in_x1 + 0.01 && c[1][0][1] >= in_y0 - 0.01 && c[1][1][1] <= in_y1 + 0.01))
    echo(str("!! OUTSIDE BOX: ", c[0]));
for (b = bosses) echo(str("lid screw ", b, " → antenna ", round(norm(b - ant_ref) * 10) / 10, " mm",
    norm(b - ant_ref) < ant_keepout_r ? "  !! ใน keepout" : ""));
echo(str("PMS7003 (เปลือกโลหะ) → antenna ", round(pt_rect_dist(ant_ref, pms_rect) * 10) / 10, " mm"));
echo(str("AHT21 (ขอบโมดูล) → DevKit outline ", round(pt_rect_dist([bme_x0, bme_yc], devkit_rect) * 10) / 10,
         " mm  (bme_min_sep ", bme_min_sep, " = ยังไม่มีแหล่ง)"));
echo(str("PMS ports center y: outlet ", pms_y0 + pms_outlet[0], " · inlet ", pms_y0 + pms_inlet[0]));
// แผ่นแปลง PMS: ระยะถึงผนังซ้าย/หน้า และเพดาน (ใต้กรอบ OLED หรือใต้ฝา) — P4 ยังไม่วัด (ok_pms_conn)
pb_ceiling = (use_oled && ov(oled_pk, pms_board_rect)) ? oled_frame_z0 : lid_z;
echo(str("PMS แผ่นแปลง: ห่างผนังซ้าย ", round((pms_board_rect[0][0] - in_x0) * 100) / 100,
         " · ห่างผนังหน้า ", round((pms_board_rect[0][1] - in_y0) * 100) / 100,
         " · P4 จุดสูงสุด (เหนือผิวบน PMS) ต้อง ≤ ", round((pb_ceiling - (floor_t + pms_H) - 1.0) * 100) / 100,
         " · ใต้แผ่นต้อง ≥ −1.0 (อุโมงค์ต่ำกว่ายอด PMS 1.0) · ค่าที่ใส่ตอนนี้ ", pms_board_z));
if (pms_board_rect[0][0] < in_x0 || pms_board_rect[0][1] < in_y0) echo("!! PMS: แผ่นแปลงชนผนัง");
if (floor_t + pms_H + pms_board_z[1] > pb_ceiling - 1.0) echo("!! PMS: แผ่นแปลง/สายชนกรอบ OLED หรือฝา");
if (pms_board_z[0] < -1.0) echo("!! PMS: แผ่นแปลงชนอุโมงค์ช่องลม");
echo(str("peg length = ", peg_top - peg_bot, " mm × 3"));
// แบบถาด: body/lid พิมพ์ก่อนได้ — แต่ถาดต้องลงช่องระหว่างผนังหน้ากับหลุม ZE07 → ขอบเขตโมดูลใหม่ที่กล่องนี้รับได้
if (sd_mode == "sled") {
    sd_len_max = ze07_pk[0][1] - sled_fp_y1 - (fit_clear + post_t + 0.5);
    sd_dx_max  = (col_x1 - col_x0) - 2 * (fit_clear + post_t);
    sd_slot_z_max = sled_open[1] - sled_tray_t - 0.3;
    echo(str("SD ถาด — โมดูลใหม่ต้อง: E1b+E1d (ยาวรวมซ็อกเก็ต) ≤ ", round(sd_len_max * 100) / 100,
             " · E1a ≤ ", round(sd_dx_max * 100) / 100, " · E2c ≤ ", round(sd_slot_z_max * 100) / 100,
             " · ปากการ์ดอยู่ในช่องกว้าง ", sled_open[0], " กลางคอลัมน์ (ตอนนี้ ", sd_overhang + sd_dy, " · ", sd_dx, " · ", sd_slot[2], ")"));
    if (sd_overhang + sd_dy > sd_len_max) echo("!! SD: ถาดชนหลุม ZE07");
}
if (use_oled) {
    oled_below = oled_zb - oled_back_h - (floor_t + pms_H);
    echo(str("OLED PCB x ", oled_x0, "…", oled_x1, " y ", oled_y0, "…", oled_y1, " · หน้าต่าง ", oled_win[0], " × ", oled_win[1],
             " · หลัง PCB → ยอด PMS ", oled_zb - (floor_t + pms_H), " (หัก oled_back_h เหลือ ", oled_below, ")",
             " · → antenna ", round(pt_rect_dist(ant_ref, oled_rect) * 10) / 10, " mm"));
    if (oled_below < 2) echo("!! OLED: ของหลัง PCB เกือบชน/ชนยอด PMS");
    if (oled_win[0] > oled_glass[0] - 0.6 || oled_va_c[1] - oled_win[1] / 2 < oled_gy0 + 0.3 || oled_va_c[1] + oled_win[1] / 2 > oled_gy1 - 0.3)
        echo("!! OLED: หน้าต่างเลยขอบกระจก");
    if (oled_pad[1] > oled_glass_dy - 0.3 || oled_pad[1] > oled_gy0 - oled_y0 - 0.3)
        echo("!! OLED: แผ่นรองมุมทับกระจก");
    if (oled_pk[0][0] < in_x0 + lid_clear + lip_t + 0.5 || oled_pk[0][1] < in_y0 + lid_clear + lip_t + 0.5)
        echo("!! OLED: กรอบชนขอบสอดฝา");
}
if (len(todo) > 0) echo(str("TODO (", len(todo), "): ", todo));

// =====================================================================
// 6) ชิ้นส่วนพื้นฐาน
// =====================================================================
module rrect(bb, r) translate(bb[0] + [r, r]) offset(r = r) square(bb[1] - bb[0] - [2 * r, 2 * r]);

module chamfer_extrude(h, c) {           // ขอบล่างลบมุม 45° (ด้านที่ติดฐานพิมพ์)
    hull() {
        linear_extrude(0.01) offset(delta = -c) children();
        translate([0, 0, c]) linear_extrude(h - c) children();
    }
}

module standoff(h) difference() {
    cylinder(d = standoff_od, h = h);
    translate([0, 0, -1]) cylinder(d = m3_tap_d, h = h + 2);
}

// เสามุมรูปตัว L อยู่นอกกรอบ rect (+clear) — เลือกมุมด้วย corners = [[sx,sy],...]
module lposts(rect, h, corners, lip = 0, lip_z = 0) {
    c = fit_clear;
    lo = rect[0] - [c, c]; hi = rect[1] + [c, c];
    for (k = corners) {
        sx = k[0]; sy = k[1];
        p = [sx ? hi[0] : lo[0], sy ? hi[1] : lo[1]];
        translate([sx ? p[0] - post_leg : p[0] - post_t, sy ? p[1] : p[1] - post_t, 0])
            cube([post_leg + post_t, post_t, h]);
        translate([sx ? p[0] : p[0] - post_t, sy ? p[1] - post_leg : p[1] - post_t, 0])
            cube([post_t, post_leg + post_t, h]);
        if (lip > 0)      // ปากล็อก: ใต้ปากเอียง 45° → พิมพ์ได้ไม่ต้อง support
            for (ax = [0, 1]) hull() {
                if (ax == 0) {   // ปากบนขา y (ยื่นเข้าแกน x)
                    translate([sx ? p[0] - 0.01 : p[0], sy ? p[1] - post_leg : p[1], lip_z]) cube([0.01, post_leg, 0.01]);
                    translate([sx ? p[0] - lip : p[0], sy ? p[1] - post_leg : p[1], lip_z + lip]) cube([lip, post_leg, 0.4]);
                } else {         // ปากบนขา x (ยื่นเข้าแกน y)
                    translate([sx ? p[0] - post_leg : p[0], sy ? p[1] - 0.01 : p[1], lip_z]) cube([post_leg, 0.01, 0.01]);
                    translate([sx ? p[0] - post_leg : p[0], sy ? p[1] - lip : p[1], lip_z + lip]) cube([post_leg, lip, 0.4]);
                }
            }
    }
}

module slots_v(x0, x1, pitch, w = 2) for (x = [x0 : pitch : x1 - w]) translate([x, 0]) square([w, 1]);

// โซนขอบฝา — ผนังภายในต้องเว้นตรงนี้ ไม่ให้ชนขอบฝาที่สอดลงมา
module lip_zone() translate([0, 0, lid_z - lid_lip - 0.5]) linear_extrude(lid_lip + 1) difference() {
    offset(delta = 1) rrect(inner_bb, in_r);
    offset(delta = -(lid_clear + lip_t + 0.4)) rrect(inner_bb, in_r);
}

module boss2d(i) projection() boss(i);
module boss(i) {
    b = bosses[i];
    hull() {
        translate([b[0], b[1], 0]) cylinder(d = boss_d, h = lid_z);
        for (a = boss_att[i])
            translate([a[0] == 0 ? b[0] - boss_d / 2 : (a[0] < 0 ? in_x0 - 1 : in_x1),
                       a[1] == 0 ? b[1] - boss_d / 2 : (a[1] < 0 ? in_y0 - 1 : in_y1), 0])
                cube([a[0] == 0 ? boss_d : 1, a[1] == 0 ? boss_d : 1, lid_z]);   // ฝังเข้าผนัง 1 mm
    }
}

// =====================================================================
// 7) ตัวกล่อง (body)
// =====================================================================
module internal_walls() difference() {
    union() {
        // ผนังกั้น (มีช่องร้อยสายเปิดจากด้านบน — วางสายลงจากข้างบนได้ ไม่ต้องร้อย)
        difference() {
            translate([div_x - div_t / 2, in_y0 - 1, 0]) cube([div_t, in_y1 - in_y0 + 2, lid_z - 0.5]);
            translate([div_x - 2, div_notch_y[0], zTop + 2]) cube([4, div_notch_y[1] - div_notch_y[0], lid_z]);
        }
        // ผนังกันความร้อนชั้นแรก (หลังหลุม PMS) + ช่องอากาศ + ผนังหน้าห้อง ENS — เต็มความสูงทั้งคู่
        translate([in_x0 - 1, dw_y0, 0]) cube([curt_x1 - in_x0 + 1, pms_wall2_t, lid_z - 0.5]);
        translate([in_x0 - 1, ch_y0, 0]) cube([curt_x1 - in_x0 + 1, ch_wall_t, lid_z - 0.5]);
        // ธรณีต่ำใต้แผ่นกั้นห้อง ENS (ขาพินลอดระหว่างธรณีกับแผ่นที่ห้อยจากฝา)
        translate([curt_x0, ch_in_y0 - 0.5, 0]) cube([curt_x1 - curt_x0, in_y1 - ch_in_y0 + 1.5, sill_z1]);
    }
    lip_zone();
}

// เนื้อเติมระหว่างผนังซ้ายกับหน้าช่องลม เฉพาะช่วงตัว PMS → ช่องลมกลายเป็นอุโมงค์ยาว pms_side_gap (เจาะใน body)
// สูงต่ำกว่ายอด PMS 1.0 → แผ่นแปลงที่ทับขอบบน PMS ไม่ชน · ช่วงหน้า PMS (ช่องแผ่นแปลง) ไม่เติม
module pms_duct(zb) translate([in_x0 - 1, pms_y0, zb - 0.01])
    cube([pms_side_gap - fit_clear + 1, pms_L, pms_H - 1.0 + 0.01]);
// กล่องครอบแผ่นแปลง + สายที่งอขึ้น (ตรวจชน/ภาพผี) — ความสูงยังเป็นค่ากะ (P4)
module pms_board_vol() translate([pms_board_rect[0][0], pms_board_rect[0][1], floor_t + pms_H + pms_board_z[0]])
    cube([pms_board_rect[1][0] - pms_board_rect[0][0], pms_board_rect[1][1] - pms_board_rect[0][1], pms_board_z[1] - pms_board_z[0]]);

module pms_mount() translate([0, 0, floor_t - 0.01])
    lposts(pms_rect, pms_post_h + 0.01, [[1, 0], [1, 1]]);       // มุมขวาหน้า + ขวาหลัง (ซ้าย = ผนัง · หลัง = ผนังห้อง ENS)

module ze07_mount() {
    r = ze07_rect; zl = floor_t + ze07_under;
    // แท่นรองเฉพาะ 4 มุม (ใต้ PCB เว้นว่างให้รอยบัดกรีสาย)
    for (sx = [0, 1], sy = [0, 1])
        translate([sx ? r[1][0] - 3 : r[0][0], sy ? r[1][1] - 3 : r[0][1], 0]) cube([3, 3, zl]);
    lposts(r, zl + ze07_pcb_t + ze07_lip + 0.8, [[0, 0], [1, 0], [0, 1], [1, 1]],
           ze07_lip, zl + ze07_pcb_t + 0.1);
}

module ens_guides() {
    // ร่องนำสองฝั่ง: บล็อกติดผนังหน้าห้อง และติดผนังหลัง · ก้นร่องหยุดที่ bme_z0 (โมดูลวางพัก)
    bx0 = bme_x0 - 1.5; bx1 = bme_x1 + 1.5; top = bme_z1 - 3;
    difference() {
        union() {
            translate([bx0, ch_in_y0 - 0.5, 0]) cube([bx1 - bx0, g0 + groove_d - ch_in_y0 + 0.5, top]);
            translate([bx0, g1 - groove_d, 0])   cube([bx1 - bx0, in_y1 - (g1 - groove_d) + 1, top]);
        }
        translate([bme_x0 - 0.25, g0, bme_z0]) cube([bme_T + 0.5, g1 - g0, top]);
    }
    // ขั้นรองขอบล่าง PCB ช่วงกลาง (กันโมดูลจมเมื่อกดฝา)
    translate([bme_x0 - 0.5, bme_yc - 4, 0]) cube([bme_T + 1.0, 8, bme_z0 - 0.0]);
}

module sd_integrated_mount() translate([0, 0, floor_t - 0.01])
    lposts([[sd_x0, sd_y0], [sd_x1, sd_y0 + sd_dy]], 4.01, [[0, 1], [1, 1], [0, 0], [1, 0]]);

module body() {
    difference() {
        union() {
            difference() {
                chamfer_extrude(lid_z, chamfer) rrect(outer_bb, box_r);
                translate([0, 0, floor_t]) linear_extrude(lid_z) rrect(inner_bb, in_r);
            }
            for (i = [0 : 3]) boss(i);
            translate([0, 0, floor_t - 0.01]) for (p = pcb_holes) translate(p) standoff(standoff_h + 0.01);
            internal_walls();
            pms_mount();
            pms_duct(floor_t);
            ze07_mount();
            ens_guides();
            if (sd_mode == "integrated") sd_integrated_mount();
            if (sd_mode == "sled") for (p = sled_pegs) translate([p[0], p[1], floor_t - 0.01]) cylinder(d = sled_peg_d, h = sled_peg_h);
        }
        // เกลียวสกรูฝา
        for (b = bosses) translate([b[0], b[1], lid_z - 10]) cylinder(d = m3_tap_d, h = 11);

        // ---- ผนังหน้า (y = in_y0) ----
        fy = in_y0 - wall_t - 1;
        // USB: ช่องเดียวครอบสองพอร์ต มุมมน (สะพานยาว ~26 mm พิมพ์ได้ไม่ต้อง support)
        translate([(usb_x[0] + usb_x[1]) / 2, fy, usb_zc]) rotate([-90, 0, 0])
            linear_extrude(wall_t + 2) offset(r = 1.5) square([usb_cut[0] - 3, usb_cut[1] - 3], center = true);
        translate([j8_x, fy, zTop + j8_dz]) rotate([-90, 0, 0]) cylinder(d = j8_d, h = wall_t + 2);
        if (sd_mode == "integrated") {
            translate([sd_x0 + sd_slot[0] - 0.3, fy, floor_t + 0.5]) cube([sd_slot[1] + 0.6, wall_t + 2, sd_slot[2] - 0.2]);
            card_funnel(sd_x0 + sd_slot[0] - 0.3, sd_slot[1] + 0.6, floor_t + 0.5, sd_slot[2] - 0.2, in_y0 - wall_t, 1.5);
            sd_finger_cut(sd_x0 + sd_slot[0] + sd_slot[1] / 2, fy, floor_t + sd_slot[2] + 0.3, wall_t + 2);
        }
        if (sd_mode == "sled")
            translate([col_cx - sled_open[0] / 2, fy, floor_t - 0.01]) cube([sled_open[0], wall_t + 2, sled_open[1]]);

        // ---- ผนังซ้าย: ช่องลม PMS (เจาะแยกสองช่อง ขนาด/ความสูงไม่เท่ากัน) ----
        for (q = [pms_outlet, pms_inlet])
            let(z_hi = q[3] + q[2] / 2 + pms_port_margin,
                z_raw = q[3] - q[2] / 2 - pms_port_margin,
                z_lo = (z_raw < 1.2) ? 0 : z_raw)     // ผนังใต้ช่องบางกว่า 1.2 พิมพ์ไม่ติด → เปิดถึงพื้น
            translate([in_x0 - wall_t - 1, pms_y0 + q[0] - q[1] / 2 - pms_port_margin, floor_t + z_lo])
                cube([wall_t + pms_side_gap + 1.5, q[1] + 2 * pms_port_margin, z_hi - z_lo]);   // ทะลุผนัง + อุโมงค์

        // ---- ผนังซ้าย: ช่องลมห้อง ENS (แนวตั้ง — สะพานแค่ 2 mm) ----
        translate([in_x0 - wall_t - 1, 0, 0]) rotate([90, 0, 90]) linear_extrude(wall_t + 2)
            translate([0, floor_t + 2]) scale([1, bme_z1 - floor_t - 2])
                slots_v(ch_in_y0 + 1.5, bosses[1][1] - boss_d / 2 - 1, 4);
        // ---- ผนังหลัง: ลมเข้าห้องเซนเซอร์ (ต่ำ) ----
        translate([0, in_y1 - 1, 0]) rotate([90, 0, 0]) mirror([0, 0, 1]) linear_extrude(wall_t + 2)
            translate([0, floor_t + 2]) scale([1, 12]) slots_v(curt_x1 + 3, div_x - 4, 4);
        // ---- ผนังขวา: ลมเข้าห้องบอร์ด (ต่ำ) ----
        translate([in_x1 - 1, 0, 0]) rotate([90, 0, 90]) linear_extrude(wall_t + 2)
            translate([0, floor_t + 1.5]) scale([1, 12]) slots_v(bosses[2][1] + boss_d / 2 + 3, bosses[3][1] - boss_d / 2 - 3, 4);

        // ข้อความบังคับ CLAUDE.md §8 — แกะใต้กล่อง (ด้านติดฐานพิมพ์)
        translate([(in_x0 + in_x1) / 2, (in_y0 + in_y1) / 2 + 4, -0.01]) mirror([1, 0, 0]) linear_extrude(0.6) {
            text("RESEARCH MONITOR - NOT A CO ALARM", size = 3.6, halign = "center", valign = "center", font = "DejaVu Sans:style=Bold");
            translate([0, -6]) text("NOT A MEDICAL DEVICE - DO NOT REPLACE UL 2034 / EN 50291 CO ALARM", size = 2.6, halign = "center", valign = "center", font = "DejaVu Sans:style=Bold");
        }
    }
}

// ปากกรวยหน้าช่องการ์ด: ปากกว้างที่ผิวนอก ค่อยๆ แคบลงเท่าช่องจริง (45° พิมพ์ได้ไม่ต้อง support)
// ช่องการ์ดต้อง "แคบ" ในแนวตั้ง ไม่งั้นการ์ดมุดใต้/ข้ามซ็อกเก็ต — กรวยช่วยเล็งแทนการขยายช่อง
// ขอบล่างไม่บาน (ไม่กินพื้น) บานแค่สองข้างกับด้านบน
module card_funnel(x0, w, z0, h, y_face, depth) hull() {
    translate([x0 - depth, y_face - 0.01, z0]) cube([w + 2 * depth, 0.01, h + depth]);
    translate([x0, y_face + depth, z0]) cube([w, 0.01, h]);
}
module sd_finger_cut(cx, y, z, len) translate([cx, y, z]) rotate([-90, 0, 0]) hull() {
    for (sx = [-1, 1], up = [0, 1])
        translate([sx * (sd_finger / 2 - sd_finger_r), -up * sd_finger_up, 0]) cylinder(r = sd_finger_r, h = len);
}

// =====================================================================
// 8) ฝา (lid) — พิมพ์คว่ำ (ผิวบนติดฐาน) ทุกอย่างที่ห้อยลงจะงอกขึ้นตอนพิมพ์ → ไม่ต้องใช้ support
// =====================================================================
module lid() {
    led_y0 = min([for (l = led_xy) l[1]]) - led_bore / 2 - 1.0;
    led_y1 = max([for (l = led_xy) l[1]]) + led_bore / 2 + 1.0;
    led_x0 = min([for (l = led_xy) l[0]]) - led_bore / 2 - 1.0;
    led_x1 = max([for (l = led_xy) l[0]]) + led_bore / 2 + 1.0;
    led_zb = zTop + led_h + 1.0;
    difference() {
        union() {
            // แผ่นฝา ลบมุมขอบบน
            hull() {
                translate([0, 0, lid_z]) linear_extrude(lid_t - chamfer) rrect(outer_bb, box_r);
                translate([0, 0, lid_z + lid_t - 0.01]) linear_extrude(0.01) offset(delta = -chamfer) rrect(outer_bb, box_r);
            }
            // ขอบสอด
            translate([0, 0, lid_z - lid_lip]) linear_extrude(lid_lip + 0.01) difference() {
                offset(delta = -lid_clear) rrect(inner_bb, in_r);
                offset(delta = -lid_clear - lip_t) rrect(inner_bb, in_r);
            }
            // ปลอกแท่งกดปุ่ม
            for (c = sw_xy) translate([c[0], c[1], tube_z0]) cylinder(d = peg_d + 2 * peg_clear + 2 * tube_wall, h = tube_len + 0.01);
            // บล็อกบังแสง LED (แสงไม่รั่วข้ามดวง · พาแสงขึ้นฝา) — เช็คแล้วใน DXF ไม่มีชิ้นสูงในกรอบนี้
            translate([led_x0, led_y0, led_zb]) cube([led_x1 - led_x0, led_y1 - led_y0, lid_z - led_zb + 0.01]);
            // แผ่นห้อยปิดครึ่งบนของช่องร้อยสายบนผนังกั้น
            translate([div_x - div_t / 2, div_notch_y[0] + 0.5, div_curtain_z]) cube([div_t, div_notch_y[1] - div_notch_y[0] - 1, lid_z - div_curtain_z + 0.01]);
            // แผ่นกั้นห้อง ENS (ห้อยลงถึงเหนือหัว dupont)
            translate([curt_x0, ch_in_y0 + 0.5, curt_z0]) cube([curt_x1 - curt_x0, in_y1 - lid_clear - ch_in_y0 - 0.5, lid_z - curt_z0 + 0.01]);
            // ซี่กด PMS (เผื่อ 0.5 — แปะฟองน้ำบางได้) · มี OLED: เลี่ยงกรอบจอ
            for (r = pms_ribs) translate([r[0], r[1], floor_t + pms_H + 0.5])
                cube([r[2], r[3], lid_z - floor_t - pms_H - 0.5 + 0.01]);
            if (use_oled) oled_holder();
            // ซี่กดขอบบนโมดูล ENS (ร่องตัว U คร่อม PCB)
            difference() {
                translate([bme_x0 - 1.2, ens_rib_y0, bme_z1 - 1.5]) cube([bme_T + 2.4, 5, lid_z - bme_z1 + 1.5 + 0.01]);
                translate([bme_x0 - 0.3, ens_rib_y0 - 1, bme_z1 - 2]) cube([bme_T + 0.6, 7, 2.5]);
            }
            // ซี่กดถาด SD
            if (sd_mode == "sled")
                translate([sled_fp_x0 + 3, sled_fp_y0, floor_t + sled_fp_h + 0.5]) cube([sled_fp_w - 6, sled_fp_t, lid_z - floor_t - sled_fp_h - 0.5 + 0.01]);
        }
        // สกรูฝา + เว้นขอบสอดรอบเสา
        for (b = bosses) {
            translate([b[0], b[1], lid_z - 1]) cylinder(d = m3_thru_d, h = lid_t + 2);
        }
        for (i = [0 : 3]) translate([0, 0, lid_z - lid_lip - 1]) linear_extrude(lid_lip + 1) offset(delta = 0.5) boss2d(i);  // เว้นขอบสอดรอบเสา (รวมส่วนที่ยึดผนัง)
        // รูแท่งกด
        for (c = sw_xy) translate([c[0], c[1], tube_z0 - 1]) cylinder(d = peg_d + 2 * peg_clear, h = lid_z + lid_t - tube_z0 + 2);
        // LED: รูนำแสง + หน้าต่าง
        for (l = led_xy) {
            translate([l[0], l[1], led_zb - 1]) cylinder(d = led_bore, h = lid_z - led_zb + 1);
            if (led_window == "hole") {
                translate([l[0], l[1], lid_z - 0.5]) cylinder(d = led_hole, h = lid_t + 1);
                translate([l[0], l[1], lid_z + lid_t - (led_csk - led_hole) / 2]) cylinder(d1 = led_hole, d2 = led_csk, h = (led_csk - led_hole) / 2 + 0.01);
            } else
                translate([l[0], l[1], lid_z - 0.5]) cylinder(d = led_bore, h = lid_t - led_skin + 0.5);
        }
        // buzzer
        translate([bz_xy[0], bz_xy[1], lid_z - 1]) {
            cylinder(d = 1.8, h = lid_t + 2);
            for (k = [0 : 5]) rotate(k * 60) translate([3.5, 0, 0]) cylinder(d = 1.8, h = lid_t + 2);
        }
        // ช่องระบาย (เป็นรูในแผ่นที่ติดฐานพิมพ์ — ไม่มีสะพาน)
        translate([0, 0, lid_z - 0.5]) linear_extrude(lid_t + 1) {      // ตัดเฉพาะแผ่นฝา ไม่กินซี่/แผ่นที่ห้อยลง
            for (x = [37 : 4 : 55]) translate([x, 16]) square([2, 38]);                                   // เหนือ DevKit
            for (x = [ze07_x0 + 2 : 4 : ze07_x1 - 3]) translate([x, ze07_y0 + 4]) square([2, ze07_dy - 8]); // เหนือ ZE07
            for (x = [in_x0 + 2 : 4 : bme_x0 - 2]) translate([x, ch_in_y0 + 2]) square([2, ens_rib_y0 - 1 - ch_in_y0 - 2]); // ห้อง ENS (ไม่ทับซี่กด)
        }
        if (use_oled) { oled_window_cut(); oled_slits(); }
        // ชื่อบนฝา (แกะบางๆ ด้านติดฐาน) · มี OLED: เลื่อนไปอยู่ระหว่างจอกับผนังกั้น
        translate([use_oled ? (oled_pk[1][0] + div_x) / 2 : (in_x0 + div_x) / 2 + 12, in_y0 + 8, lid_z + lid_t - 0.4]) linear_extrude(1)
            text("AirGuard", size = 5, halign = "center", valign = "center", font = "DejaVu Sans:style=Bold");
    }
}

// ---- OLED: กรอบห้อยจากฝา + แผ่นรองมุม + ลิ้นสแนปซ้าย/ขวา (พิกัดประกอบจริง) ----
// ฝาพิมพ์คว่ำ → ทุกชิ้นงอกขึ้นจากแผ่นฝา · ปากสแนปด้านที่แตะหลัง PCB เอียง 45° = พิมพ์ได้ไม่ต้อง support
oled_tab_y0 = (oled_y0 + oled_y1) / 2 - oled_tab_len / 2;
module oled_holder() {
    h = lid_z - oled_frame_z0 + 0.01;
    // ผนังกรอบ 4 ด้าน (ด้านหัวขาไม่ชนอะไร — header อยู่ในขอบ PCB สายลงด้านล่าง)
    translate([0, 0, oled_frame_z0]) linear_extrude(h) difference() {
        translate(oled_pk[0]) square(oled_pk[1] - oled_pk[0]);
        translate(oled_rect[0] - [fit_clear, fit_clear]) square(oled_pcb + 2 * fit_clear * [1, 1]);
    }
    // แผ่นรองมุม: ผิวหน้า PCB แตะที่ oled_zf · อยู่ในแถบนอกกระจก
    for (sx = [0, 1], sy = [0, 1])
        translate([sx ? oled_x1 - oled_pad[0] : oled_x0, sy ? oled_y1 - oled_pad[1] : oled_y0, oled_zf])
            cube([oled_pad[0], oled_pad[1], lid_z - oled_zf + 0.01]);
    // ปากสแนป: 0 ที่ zb+0.3 → ยื่นเต็มที่ zb+0.3-lip (45°) → ลาดกลับ 0 (ทางลาดตอนกดจอเข้า)
    za = oled_zb + fit_clear; zb = za - oled_lip; zc = zb - oled_lip;
    for (s = [0, 1])
        translate([s ? oled_x1 + fit_clear : oled_x0 - fit_clear, oled_tab_y0 + oled_tab_len, 0])
            mirror([s, 0, 0]) rotate([90, 0, 0]) linear_extrude(oled_tab_len)
                polygon([[-0.3, za], [0, za], [oled_lip, zb], [0, zc], [-0.3, zc]]);
}
// ร่องสองข้างลิ้นสแนป ให้ลิ้นงอได้อิสระ (ตัดถึงใต้ฝาพอดี ไม่กินแผ่นฝา)
module oled_slits() for (s = [0, 1], y = [oled_tab_y0 - 0.6, oled_tab_y0 + oled_tab_len])
    translate([s ? oled_x1 + fit_clear - 0.5 : oled_pk[0][0] - 0.5, y, oled_frame_z0 - 1])
        cube([oled_frame_t + 1, 0.6, lid_z - oled_frame_z0 + 1]);
// หน้าต่างจอบนฝา: ตรง 0.6 แล้วผายออก 45° (มุมมองกว้าง · พิมพ์คว่ำแล้วเป็นผนังเอียง 45°)
module oled_window_cut() translate([oled_va_c[0], oled_va_c[1], 0]) {
    e = lid_t - 0.6;
    translate([-oled_win[0] / 2, -oled_win[1] / 2, lid_z - 1]) cube([oled_win[0], oled_win[1], lid_t + 2]);
    hull() {
        translate([-oled_win[0] / 2, -oled_win[1] / 2, lid_z + 0.6]) cube([oled_win[0], oled_win[1], 0.01]);
        translate([-oled_win[0] / 2 - e, -oled_win[1] / 2 - e, lid_z + lid_t]) cube([oled_win[0] + 2 * e, oled_win[1] + 2 * e, 0.02]);
    }
}

// =====================================================================
// 9) แท่งกดปุ่ม ×3 — พิมพ์ตั้ง หัวลงฐาน (ปีกเป็นกรวย 45° พิมพ์ได้ไม่ต้อง support)
// =====================================================================
module peg() {   // พิกัดประกอบจริง: z = peg_bot (ปลายตีนแตะปุ่ม) → peg_top (โผล่เหนือฝา)
    L = peg_top - peg_bot;
    f1 = flange_z1 - peg_bot;           // ยอดกรวยปีก
    union() {
        cylinder(d = peg_foot_d, h = 1.6);                                    // ตีนกดปุ่ม
        translate([0, 0, 1.5]) cylinder(d = 4, h = f1 - 2.2 - 1.5 + 0.01);    // แกนล่าง
        translate([0, 0, f1 - 2.2]) cylinder(d = peg_flange_d, h = 1.2);      // ปีกกันหลุด (ชนปลายปลอก)
        translate([0, 0, f1 - 1.0]) cylinder(d1 = peg_flange_d, d2 = peg_d, h = 1.0);
        translate([0, 0, f1 - 1.0]) cylinder(d = peg_d, h = L - (f1 - 1.0) - 0.5);
        translate([0, 0, L - 0.5]) cylinder(d1 = peg_d, d2 = peg_d - 1, h = 0.5); // ลบมุมหัว
    }
}
module pegs_print() for (i = [0 : 2]) translate([i * 12, 0, peg_top - peg_bot]) mirror([0, 0, 1]) peg();

// =====================================================================
// 10) ถาด microSD (sled) — พิมพ์ถาดลงฐาน · แผ่นหน้าตั้งขึ้น
// =====================================================================
module sled() {
    zt = sled_tray_t;
    difference() {
        union() {
            // แผ่นหน้า
            translate([sled_fp_x0, sled_fp_y0, floor_t]) cube([sled_fp_w, sled_fp_t, sled_fp_h]);
            // ถาด
            translate([sled_fp_x0, sled_fp_y0, floor_t]) cube([sled_fp_w, sled_tray_y1 - sled_fp_y0, zt]);
            // เสามุมหลังกันโมดูลถอย (หน้าใช้แผ่นหน้า)
            translate([0, 0, floor_t + zt - 0.01]) lposts([[sd_x0, sd_sled_y0], [sd_x1, sd_sled_y0 + sd_dy]], 3.01, [[0, 1], [1, 1]]);
        }
        // ปากการ์ด + รอยบากนิ้ว บนแผ่นหน้า
        translate([sd_x0 + sd_slot[0] - 0.3, sled_fp_y0 - 1, floor_t + zt + 0.3]) cube([sd_slot[1] + 0.6, sled_fp_t + 2, sd_slot[2]]);
        card_funnel(sd_x0 + sd_slot[0] - 0.3, sd_slot[1] + 0.6, floor_t + zt + 0.3, sd_slot[2], sled_fp_y0, 1.0);
        sd_finger_cut(sd_x0 + sd_slot[0] + sd_slot[1] / 2, sled_fp_y0 - 1, floor_t + zt + sd_slot[2] + 0.6, sled_fp_t + 2);
        // รูรับหมุดบนพื้นกล่อง
        for (p = sled_pegs) translate([p[0], p[1], floor_t - 1]) cylinder(d = sled_peg_d + 0.5, h = zt + 2);
    }
}

// =====================================================================
// 11) ชิ้นทดสอบ — พิมพ์ได้เลย ไม่ติด TODO (เลือกค่าเผื่อจากชิ้นเหล่านี้)
// =====================================================================
// (A) แผ่นรู: สกรู M3 เกลียวปล่อย · รูทะลุ · ปลอกแท่งกด 3 ขนาด + แท่งกดสั้นทดลอง
module fit_coupon() {
    tap  = [2.4, 2.5, 2.6, 2.7, 2.8, 2.9, 3.0];
    thru = [3.2, 3.3, 3.4, 3.5, 3.6];
    tube = [0.15, 0.25, 0.35];           // peg_clear ที่ทดลอง
    w = 80; h = 42; t = 4;
    difference() {
        union() {
            cube([w, h, t]);
            for (i = [0 : 2]) translate([12 + i * 14, 6, 0]) cylinder(d = peg_d + 2 * tube[i] + 2 * tube_wall + 2, h = t + 6);
        }
        for (i = [0 : len(tap) - 1])  translate([6 + i * 11, 35, -1]) cylinder(d = tap[i], h = t + 2);
        for (i = [0 : len(thru) - 1]) translate([6 + i * 11, 23, -1]) cylinder(d = thru[i], h = t + 2);
        for (i = [0 : 2]) translate([12 + i * 14, 6, -1]) cylinder(d = peg_d + 2 * tube[i], h = t + 8);
        for (i = [0 : len(tap) - 1])  translate([3.5 + i * 11, 39, t - 0.6]) linear_extrude(1) text(str(tap[i] * 10), size = 2.4, font = "DejaVu Sans:style=Bold");
        for (i = [0 : len(thru) - 1]) translate([3.5 + i * 11, 27, t - 0.6]) linear_extrude(1) text(str(thru[i] * 10), size = 2.4, font = "DejaVu Sans:style=Bold");
        for (i = [0 : 2]) translate([8 + i * 14, 12.5, t - 0.6]) linear_extrude(1) text(str("+", tube[i] * 100), size = 2.4, font = "DejaVu Sans:style=Bold");
    }
    translate([62, 8, 0]) cylinder(d = peg_d, h = 12);    // แท่งทดลอง Ø6 → ลองสอดปลอกทั้งสาม
}

// (B) มุมกล่อง + มุมฝา: เช็คขอบฝาสอดลงพอดี (lid_clear) + สกรูฝากินเกลียว
module fit_lid() {
    s = 34; hh = 10;
    bb_o = [[0, 0], [s + 20, s + 20]];                 // ขยายเกินแล้วตัดเหลือมุม → ได้มุมมนจริงมุมเดียว
    bb_i = [[wall_t, wall_t], [s + 20 - wall_t, s + 20 - wall_t]];
    b = [wall_t + boss_d / 2, wall_t + boss_d / 2];
    // มุมกล่อง
    difference() {
        union() {
            intersection() {
                difference() {
                    chamfer_extrude(hh, chamfer) rrect(bb_o, box_r);
                    translate([0, 0, floor_t]) linear_extrude(hh) rrect(bb_i, in_r);
                }
                cube([s, s, hh]);
            }
            hull() {   // เสาสกรูติดมุม (แบบเดียวกับกล่องจริง)
                translate([b[0], b[1], 0]) cylinder(d = boss_d, h = hh);
                translate([wall_t, wall_t, 0]) cube([0.01, boss_d, hh]);
                translate([wall_t, wall_t, 0]) cube([boss_d, 0.01, hh]);
            }
        }
        translate([b[0], b[1], 1]) cylinder(d = m3_tap_d, h = hh);
    }
    // มุมฝา (วางข้างๆ ในท่าพิมพ์จริง: ผิวบนติดฐาน ขอบสอดงอกขึ้น)
    translate([s + 6, 0, 0]) difference() {
        union() {
            intersection() { chamfer_extrude(lid_t, chamfer) rrect(bb_o, box_r); cube([s, s, lid_t]); }
            translate([0, 0, lid_t - 0.01]) linear_extrude(lid_lip) intersection() {
                difference() {
                    offset(delta = -lid_clear) rrect(bb_i, in_r);
                    offset(delta = -lid_clear - lip_t) rrect(bb_i, in_r);
                }
                square([s - 4, s - 4]);
            }
        }
        translate([b[0], b[1], -1]) cylinder(d = m3_thru_d, h = lid_t + 2);
        translate([b[0], b[1], lid_t - 0.5]) cylinder(d = boss_d + 1.0, h = lid_lip + 1);
        translate([s / 2 + 4, s / 2 + 4, -0.01]) mirror([1, 0, 0]) linear_extrude(0.41)
            text(str("lid ", lid_clear), size = 3.2, halign = "center", font = "DejaVu Sans:style=Bold");
    }
}

// (C) ผนังซ้ายช่วง PMS + พื้น + เสาขวา: วาง PMS แล้วส่องดูว่าปากช่องลมตรงรูผนัง
module fit_pms() {
    x0 = in_x0 - wall_t; y0 = in_y0 - wall_t;
    translate([-x0, -y0, 0]) difference() {
        union() {
            translate([x0, y0, 0]) cube([pms_x1 - x0 + 4, pms_y1 - y0 + 3, 1.6]);                      // พื้นบาง
            translate([x0, y0, 0]) cube([wall_t, pms_y1 - y0 + 3, floor_t + pms_H + 2]);                // ผนังซ้าย
            translate([x0, y0, 0]) cube([pms_x1 - x0 + 4, wall_t, floor_t + pms_H + 2]);                // ผนังหน้า
            translate([0, 0, 1.6 - 0.01]) lposts(pms_rect, pms_post_h + 0.01, [[1, 0], [1, 1]]);
            pms_duct(1.6);
        }
        for (q = [pms_outlet, pms_inlet])
            let(z_hi = q[3] + q[2] / 2 + pms_port_margin, z_raw = q[3] - q[2] / 2 - pms_port_margin,
                z_lo = (z_raw < 1.2) ? 0 : z_raw)
            translate([x0 - 1, pms_y0 + q[0] - q[1] / 2 - pms_port_margin, 1.6 + z_lo])
                cube([wall_t + pms_side_gap + 1.5, q[1] + 2 * pms_port_margin, z_hi - z_lo]);
        translate([x0 + 0.4, pms_y0 + pms_outlet[0], floor_t + pms_H - 2]) rotate([90, 0, -90]) linear_extrude(1)
            text("OUT", size = 3, halign = "center", font = "DejaVu Sans:style=Bold");
        translate([x0 + 0.4, pms_y0 + pms_inlet[0], floor_t + pms_H - 2]) rotate([90, 0, -90]) linear_extrude(1)
            text("IN", size = 3, halign = "center", font = "DejaVu Sans:style=Bold");
    }
}

// (D) ชิ้นฝาเฉพาะช่วง OLED: หน้าต่าง + กรอบ + แผ่นรอง + ลิ้นสแนป (พิมพ์คว่ำแบบฝาจริง)
//     กดจอจริงเข้า → เช็คว่าแน่น/หลวม · ส่องหน้าต่างว่าเห็นพื้นที่แสดงผลครบไม่ถูกขอบบัง
module fit_oled() {
    m = 3;
    p0 = oled_pk[0] - [m, m]; p1 = oled_pk[1] + [m, m];
    translate([-p0[0], -p0[1], lid_z + lid_t]) mirror([0, 0, 1]) difference() {
        union() {
            translate([p0[0], p0[1], lid_z]) cube([p1[0] - p0[0], p1[1] - p0[1], lid_t]);
            oled_holder();
        }
        oled_window_cut();
        oled_slits();
        translate([p0[0] + 1.5, p0[1] + 0.5, lid_z + lid_t - 0.4]) linear_extrude(1) text(str("OLED ", fit_clear), size = 2.4, font = "DejaVu Sans:style=Bold");
    }
}

// =====================================================================
// 12) ภาพผีโมดูล (เฉพาะ preview)
// =====================================================================
module ghosts() {
    color([0.12, 0.45, 0.22, 0.85]) translate([0, 0, floor_t + standoff_h]) cube([pcb_w, pcb_h, pcb_t]);
    color([0.1, 0.1, 0.12, 0.9]) translate([devkit_rect[0][0], devkit_rect[0][1], zDK]) cube([25.4, 62.74, devkit_t]);
    color([0.75, 0.75, 0.78, 0.9]) translate([devkit_rect[0][0] + 3.5, 45, zDK + devkit_t]) cube([18, 20.9, devkit_top - socket_h - devkit_t]);
    color([0.72, 0.72, 0.76, 0.9]) translate([pms_x0, pms_y0, floor_t]) cube([pms_W, pms_L, pms_H]);
    color([0.15, 0.55, 0.25, 0.9]) pms_board_vol();
    color([0.25, 0.4, 0.9, 0.9]) translate([ze07_x0, ze07_y0, floor_t + ze07_under]) {
        cube([ze07_dx, ze07_dy, ze07_pcb_t]);
        translate([ze07_dx / 2, 2.86 + 10.02, ze07_pcb_t]) cylinder(d = 20.05, h = ze07_H - ze07_pcb_t);
    }
    color([0.55, 0.25, 0.65, 0.9]) translate([bme_x0, bme_yc - bme_W / 2, bme_z0]) cube([bme_T, bme_W, bme_L]);
    color([0.85, 0.75, 0.2, 0.9]) translate([bme_x1, bme_yc - 10, pin_zc - 0.3]) cube([bme_pins, 20, 0.6]);
    if (sd_mode != "none") color([0.2, 0.3, 0.8, 0.9]) translate([sd_x0, sd_y0, sd_z0]) cube([sd_dx, sd_dy, sd_H]);
    for (s = sw_xy) color([0.9, 0.9, 0.9]) translate([s[0], s[1], zTop]) cylinder(d = 3, h = sw_h);
    if (use_oled) {
        color([0.1, 0.3, 0.7, 0.9]) translate([oled_x0, oled_y0, oled_zb]) cube([oled_pcb[0], oled_pcb[1], oled_pcb_t]);
        color([0.05, 0.05, 0.05, 0.95]) translate([oled_cx - oled_glass[0] / 2, oled_gy0, oled_zf]) cube([oled_glass[0], oled_glass[1], oled_glass[2]]);
        color([0.15, 0.15, 0.15, 0.9]) translate([oled_cx - 5.08, oled_y1 - 2.54, oled_zb - 2.5]) cube([10.16, 2.54, 2.5]);   // header
        color([0.85, 0.75, 0.2, 0.6]) translate([oled_cx - 5.08, oled_y1 - 2.54, oled_zb - oled_back_h]) cube([10.16, 2.54, oled_back_h - 2.5]);  // ขา + สาย (oled_back_h)
    }
}

// =====================================================================
// 13) ตัวเลือก part
// =====================================================================
module gate(name) assert(allow_draft || len(todo) == 0, str("ยังมี TODO: ", todo, " — ห้ามสั่งพิมพ์ ", name));

if (part == "body")      { gate("body"); body(); }
else if (part == "lid")  { gate("lid"); translate([0, 0, lid_z + lid_t]) mirror([0, 0, 1]) lid(); }   // คว่ำไว้พิมพ์
else if (part == "pegs") { assert(allow_draft || ok_print_fit, "peg_clear ยังไม่ได้เลือกจาก fit_coupon"); pegs_print(); }
else if (part == "sled") { assert(sd_mode == "sled", "sd_mode ไม่ใช่ sled");
                           assert(allow_draft || (ok_sd_dims && ok_print_fit), "รอวัดโมดูล microSD ตัวใหม่");
                           translate([0, 0, -floor_t]) sled(); }   // ถาดลงฐาน
else if (part == "fit_coupon") fit_coupon();
else if (part == "fit_lid")    fit_lid();
else if (part == "fit_pms")    fit_pms();
else if (part == "fit_oled")   { assert(use_oled, "use_oled = false"); fit_oled(); }
else if (part == "none") { }   // ใช้ตอน include จากไฟล์ทดสอบ
else if (part == "lid_asm")  lid();                 // ตำแหน่งประกอบ (ใช้ทำ viewer)
else if (part == "sled_asm") sled();
else if (part == "pegs_asm") for (s = sw_xy) translate([s[0], s[1], peg_bot]) peg();
else if (part == "dump") echo(str("JSON{", "\"zTop\":", zTop, ",\"zDK\":", zDK, ",\"usb_zc\":", usb_zc, ",\"lid_z\":", lid_z,
    ",\"pms\":", [pms_x0, pms_y0], ",\"ze07\":", [ze07_x0, ze07_y0, floor_t + ze07_under], ",\"bme\":", [bme_x0, bme_yc, bme_z0, bme_x1, pin_zc],
    ",\"sd\":", [sd_x0, sd_y0, sd_z0], ",\"in\":", [in_x0, in_y0, in_x1, in_y1], ",\"outer\":", outer_bb, ",\"bosses\":", bosses,
    ",\"curt\":", [curt_x0, curt_z0], ",\"peg_bot\":", peg_bot, ",\"col_cx\":", col_cx,
    ",\"oled\":", use_oled ? [oled_x0, oled_y0, oled_zb, oled_x1, oled_y1, oled_zf] : [], "}JSON"));
else if (part == "exploded") {
    color([0.93, 0.91, 0.86]) body();
    color([0.86, 0.84, 0.79, 0.9]) translate([0, 0, 40]) lid();
    if (sd_mode == "sled") color([0.7, 0.66, 0.58]) translate([0, -25, 0]) sled();
    for (s = sw_xy) color([0.6, 0.6, 0.6]) translate([s[0], s[1], peg_bot + 40]) peg();
    ghosts();
} else {   // preview: ประกอบแล้ว ฝาโปร่ง
    color([0.93, 0.91, 0.86]) body();
    if (sd_mode == "sled") color([0.7, 0.66, 0.58]) sled();
    for (s = sw_xy) color([0.6, 0.6, 0.6]) translate([s[0], s[1], peg_bot]) peg();
    ghosts();
    %lid();
}
