#ifndef AIRGUARD_TASK_CONFIG_H
#define AIRGUARD_TASK_CONFIG_H

/* FW-4: priority / stack / queue sizing of the logger tasks, in one place.
 *
 * ── Priority order (FreeRTOS, higher number = runs first) ─────────────────
 * ESP-IDF v5.5.5 system tasks, checked in esp_system/include/esp_task.h and
 * lwip/Kconfig [ขึ้นกับเวอร์ชัน ESP-IDF], configMAX_PRIORITIES = 25:
 *   esp_timer 22 · sys_evt (default event loop) 20 · tcpip (lwIP) 18
 *   Wi-Fi driver task is also above ours (Wi-Fi docs); it runs on core 0
 *   by default (CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0).
 * Ours:
 *   sampler 10  ต้องตรงเวลา 1 Hz → สูงสุดในของเรา · ทำงานสั้น (poll UART/I²C
 *               ไม่มี busy-wait) แล้วนอนด้วย xTaskDelayUntil
 *               pin core 1 เพื่อไม่ชนกับ Wi-Fi/lwIP ที่อยู่ core 0 และมี
 *               priority สูงกว่า (สมมติฐาน ต้องวัด jitter ที่ GPIO4)
 *   io       5  (FW-1, tasks_io.c) ปุ่ม/LED ต้องตอบใน ~100 ms แต่ไม่สำคัญกว่า
 *               การเก็บตัวอย่าง · งานสั้น
 *   storage  3  งานช้า (FAT/SD อาจค้างหลายร้อย ms) → ต่ำกว่า sampler/io
 *               เพื่อไม่ให้ดึงรอบ 1 Hz และปุ่ม · queue คั่นกลาง
 *   console  2  คนพิมพ์ settime · ช้าที่สุดก็ไม่เป็นไร
 * ทั้งหมดเป็นค่าคาดหมาย ต้องยืนยันด้วยการวัด (jitter ที่ GPIO4, SD latency)
 */
#define SAMPLER_TASK_PRIORITY   10U
#define SAMPLER_TASK_CORE        1
#define STORAGE_TASK_PRIORITY    3U
#define CONSOLE_TASK_PRIORITY    2U

/* ── Stack (ESP-IDF counts bytes) ──────────────────────────────────────────
 * อ้างอิงจาก FW-1: io task ที่ทำ log + GPIO + queue ใช้ 4096 B
 * printf/ESP_LOG ใน newlib ใช้ stack ราว 1.5–2 KB ต่อการเรียก (ค่าคาดหมาย)
 *
 * sampler: locals ~600 B (csv_sample_t ~60, raw AHT21 7, pms frame 12,
 *          stats ~200, struct tm/timeval/strings ~300)
 *          + ESP_LOG/printf ~2048 + i2c_master/uart/localtime ~1024
 *          + margin ~1400 → 5120 B
 *          FW-5a ENS160 adds ~100 B of locals (raw 6, ens160_data_t 12,
 *          MISR bytes, summary string 48) and no deeper call chain than the
 *          AHT21 path (i2c_master + ESP_LOG with float in the summary, as in
 *          the AHT21 log) → stays 5120 B. Measured before FW-5a (14 Oct):
 *          HWM 2808-2844 B free = 55 % of 5120.
 * storage: locals ~400 B (row text 128, path 48 ×2, item ~60)
 *          + stdio → VFS → FatFS → sdmmc → SPI driver ~2560 (เส้นลึกสุด,
 *          LFN buffer อยู่ heap จึงไม่อยู่ใน stack) + ESP_LOG ~2048
 *          + mount path (sdmmc_card_init) ไม่ซ้อนกับ log ข้างบน
 *          + margin ~1100 → 6144 B
 * console: line buffer 64 + uart_read_bytes + parser + settimeofday
 *          + ESP_LOG ~2048 + margin → 3072 B
 * ทุกค่าเป็นค่าคาดหมาย ต้องยืนยันด้วยการวัด: high-water mark ถูก log ทุก 60 s
 * เป้า: เหลือ ≥ 25% (docs/tasks/README.md)
 */
#define SAMPLER_STACK_BYTES   5120U
#define STORAGE_STACK_BYTES   6144U
#define CONSOLE_STACK_BYTES   3072U

#endif
