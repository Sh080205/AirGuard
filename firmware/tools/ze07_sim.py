"""Send simulated ZE07-CO frames to the board, to test the CO path without CO.

FW-5b step 4. docs/03-CO_SAFETY.md §4 forbids making real CO to test the
sensor; this script tests the firmware logic instead (ALARM, ACK mute,
hysteresis, FAULT, latency) by sending frames in the exact format of the
Winsen ZE07-CO manual V1.7, Table 4 (initiative upload), from a PC.

Wiring (board powered off while connecting):
  * unplug J2 (the real ZE07): two transmitters on one line is a short
  * 3.3 V USB-UART adapter TX  -> TP8 (CO_TX net = GPIO5, UART2 RX)
  * adapter GND               -> TP1 (GND)
  * adapter must be 3.3 V logic, never 5 V (GPIO5 is a 3.3 V pin)
  * leave adapter RX unconnected (the firmware sends nothing)

The script is a PC-side test tool; it never runs on the device.

Script = comma-separated segments "WHAT:SECONDS", one frame per --period:
  3.7:20    send 3.7 ppm for 20 s
  95:30     send 95.0 ppm (>= 90 -> ALARM at once)
  off:10    send nothing for 10 s (> 6 s -> FAULT)
  bad:5     send frames with a wrong checksum (rejected, FAULT after 6 s)
Example (ALARM, hold, release, FAULT, recover):
    python tools\\ze07_sim.py --port COM9 --script "0:20,95:30,85:30,80:70,off:10,3.7:20"

Run it from the "ESP-IDF 5.5 CMD" window (its Python has pyserial).
Watch the board in another window: idf.py -p COMx monitor (different port).
"""

import argparse
import sys
import time

FRAME_SIZE = 9
START, GAS_CO, UNIT_PPM, DECIMALS = 0xFF, 0x04, 0x03, 0x01
FULL_RANGE_X10 = 5000          # Table 4 example: 0x1388 = 500.0 ppm


def checksum(frame):
    """Manual V1.7 p.6 §3: Not(Byte1 + ... + Byte7) + 1, 8-bit."""
    return (-sum(frame[1:8])) & 0xFF


def make_frame(conc_x10, good=True):
    """One Table 4 frame for a concentration in 0.1 ppm (0..65535)."""
    if not 0 <= conc_x10 <= 0xFFFF:
        raise ValueError("concentration out of 16-bit range")
    frame = bytearray([START, GAS_CO, UNIT_PPM, DECIMALS,
                       conc_x10 >> 8, conc_x10 & 0xFF,
                       FULL_RANGE_X10 >> 8, FULL_RANGE_X10 & 0xFF, 0])
    frame[8] = checksum(frame)
    if not good:
        frame[8] ^= 0x01
    return bytes(frame)


def ppm_to_x10(text):
    """'3.7' -> 37, exact for one decimal (no float rounding surprises)."""
    whole, _, frac = text.strip().partition(".")
    if not whole.isdigit() or (frac and (len(frac) > 1 or not frac.isdigit())):
        raise ValueError(f"ppm must look like 12 or 12.3, got {text!r}")
    return int(whole) * 10 + (int(frac) if frac else 0)


def parse_script(text):
    """'3.7:20,off:10' -> [('ppm', 37, 20.0), ('off', None, 10.0)]."""
    segments = []
    for part in text.split(","):
        what, sep, seconds = part.strip().partition(":")
        if not sep:
            raise ValueError(f"segment {part!r} needs WHAT:SECONDS")
        duration = float(seconds)
        if duration <= 0:
            raise ValueError(f"segment {part!r}: seconds must be > 0")
        what = what.strip().lower()
        if what in ("off", "bad"):
            segments.append((what, None, duration))
        else:
            segments.append(("ppm", ppm_to_x10(what), duration))
    if not segments:
        raise ValueError("empty script")
    return segments


def run(link, segments, period, clock=time.monotonic, sleep=time.sleep, log=print):
    """Send the script; returns the number of frames written."""
    sent = 0
    t0 = clock()
    next_send = t0
    for kind, x10, duration in segments:
        end = next_send + duration
        log(f"[{next_send - t0:7.1f} s] segment {kind} "
            f"{'' if x10 is None else f'{x10 // 10}.{x10 % 10} ppm '}for {duration:g} s")
        while next_send < end:
            delay = next_send - clock()
            if delay > 0:
                sleep(delay)
            if kind != "off":
                frame = make_frame(x10 if x10 is not None else 0, good=(kind == "ppm"))
                link.write(frame)
                sent += 1
                log(f"[{clock() - t0:7.1f} s] {frame.hex(' ').upper()}")
            next_send += period
    return sent


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", required=True, help="USB-UART adapter port, e.g. COM9")
    parser.add_argument("--script", required=True, help='e.g. "0:20,95:30,off:10"')
    parser.add_argument("--period", type=float, default=1.0,
                        help="seconds between frames (default 1.0; the real module "
                             "sends 'every other 1 s', manual p.4)")
    args = parser.parse_args(argv)
    try:
        segments = parse_script(args.script)
    except ValueError as exc:
        parser.error(str(exc))
    if args.period <= 0:
        parser.error("--period must be > 0")

    import serial  # pyserial: bundled with the ESP-IDF Python environment

    # manual V1.7 p.4 Table 3: 9600 baud, 8 data bits, 1 stop bit, no parity
    with serial.Serial(args.port, 9600, bytesize=8, parity="N", stopbits=1) as link:
        sent = run(link, segments, args.period)
    print(f"done: {sent} frames sent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
