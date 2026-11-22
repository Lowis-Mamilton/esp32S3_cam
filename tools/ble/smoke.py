"""Talk the MX protocol to MXCam over BLE (Bluetooth firmware build, ADR-0022).

What Studio does, without a browser: GET_INFO, SET_FUNCTION, STREAM_START at
5 fps, PING every second, count FRAME and RESULTS pushes, STREAM_STOP.

    pip install bleak
    python tools/ble/smoke.py [--fn 3] [--seconds 10] [--save frame.jpg] [--ttl COM7]

--ttl also reads the once-per-second stats from the TTL port (needs pyserial)
and prints the vision speed and the lowest free internal RAM while streaming.

Close Studio first if it is connected over Bluetooth (one connection only).
"""
import argparse
import asyncio
import re
import struct
import threading
import time

from bleak import BleakClient, BleakScanner

RX = "4d584300-0011-4b1e-9c5a-7f3e2a1d0c9b"  # host -> MXCam (write)
TX = "4d584300-0012-4b1e-9c5a-7f3e2a1d0c9b"  # MXCam -> host (notify)
CHUNK = 180


def crc16(b: bytes) -> int:
    c = 0xFFFF
    for x in b:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def enc(cmd: int, seq: int, payload: bytes = b"") -> bytes:
    body = bytes([1, cmd, seq]) + struct.pack("<H", len(payload)) + payload
    return b"MX" + body + struct.pack("<H", crc16(body))


class Decoder:
    def __init__(self) -> None:
        self.b = bytearray()
        self.skipped = 0

    def push(self, data: bytes):
        self.b += data
        out = []
        while True:
            i = self.b.find(b"MX")
            if i < 0:
                self.skipped += max(0, len(self.b) - 1)
                del self.b[:-1]
                return out
            self.skipped += i
            del self.b[:i]
            if len(self.b) < 7:
                return out
            ver, ln = self.b[2], struct.unpack_from("<H", self.b, 5)[0]
            if ver != 1:
                del self.b[:1]
                self.skipped += 1
                continue
            if len(self.b) < 9 + ln:
                return out
            body = bytes(self.b[2 : 7 + ln])
            if crc16(body) != struct.unpack_from("<H", self.b, 7 + ln)[0]:
                del self.b[:1]
                self.skipped += 1
                continue
            out.append((self.b[3], self.b[4], body[5:]))
            del self.b[: 9 + ln]


async def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fn", type=int, default=3)
    ap.add_argument("--fps", type=int, default=5)
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--save", help="write the last JPEG frame to this file")
    ap.add_argument("--ttl", help="TTL (log) port, e.g. COM7: also report fps / proc_ms / heap")
    args = ap.parse_args()

    stats: list[str] = []
    stop = threading.Event()
    reader = None
    if args.ttl:
        reader = threading.Thread(target=read_ttl, args=(args.ttl, stats, stop), daemon=True)
        reader.start()

    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: (d.name or ad.local_name or "").startswith("MXCam-"), timeout=10
    )
    if dev is None:
        raise SystemExit("no MXCam-xxxx device found (Bluetooth firmware flashed? PC Bluetooth on?)")

    dec = Decoder()
    frames: asyncio.Queue = asyncio.Queue()

    def on_notify(_, data: bytearray) -> None:
        for f in dec.push(bytes(data)):
            frames.put_nowait(f)

    seq = 0
    pushes = {"frame": 0, "results": 0, "bytes": 0, "last": None, "ids": []}

    def take_push(cmd: int, payload: bytes) -> bool:
        if cmd == 0x60:
            pushes["frame"] += 1
            pushes["bytes"] += len(payload) - 12
            pushes["last"] = payload[12:]
            pushes["ids"].append(struct.unpack_from("<I", payload)[0])
            return True
        if cmd == 0x61:
            pushes["results"] += 1
            return True
        return False

    t0 = time.monotonic()
    async with BleakClient(dev, timeout=30) as c:
        print(f"{dev.name}: connected in {time.monotonic() - t0:.1f} s, mtu {c.mtu_size}")
        await c.start_notify(TX, on_notify)

        async def send(cmd: int, payload: bytes = b"") -> int:
            nonlocal seq
            seq = (seq + 1) & 0xFF
            pkt = enc(cmd, seq, payload)
            for i in range(0, len(pkt), CHUNK):
                await c.write_gatt_char(RX, pkt[i : i + CHUNK], response=True)
            return seq

        async def request(cmd: int, payload: bytes = b"", timeout: float = 2.5):
            t = time.monotonic()
            s = await send(cmd, payload)
            while time.monotonic() - t < timeout:
                try:
                    rc, rs, rp = await asyncio.wait_for(frames.get(), timeout=0.2)
                except asyncio.TimeoutError:
                    continue
                if take_push(rc, rp):
                    continue
                if rs == s and rc in (cmd | 0x80, 0xFF):
                    return rc, rp, time.monotonic() - t
            return None

        def show(name: str, r) -> None:
            if r is None:
                print(f"{name}: NO REPLY")
            else:
                print(f"{name}: 0x{r[0]:02x} {r[1].hex()[:64]} ({r[2] * 1000:.0f} ms)")

        show("GET_INFO", await request(0x01))
        show(f"SET_FUNCTION {args.fn}", await request(0x10, bytes([args.fn])))
        show("GET_CONFIG", await request(0x40))
        show(f"STREAM_START {args.fps} fps", await request(0x50, bytes([3, args.fps, 0])))
        stats.clear()
        t_start = time.monotonic()
        last_ping = 0.0
        pings = lost = 0
        while time.monotonic() - t_start < args.seconds:
            if time.monotonic() - last_ping >= 1.0:
                last_ping = time.monotonic()
                r = await request(0x02, timeout=1.0)
                pings += 1
                lost += r is None
            else:
                try:
                    rc, _, rp = await asyncio.wait_for(frames.get(), timeout=0.1)
                    take_push(rc, rp)
                except asyncio.TimeoutError:
                    pass
        dt = time.monotonic() - t_start
        streaming = list(stats)
        show("STREAM_STOP", await request(0x51))
        n = pushes["frame"]
        ids = pushes["ids"]
        backwards = sum(1 for a, b in zip(ids, ids[1:]) if b <= a)
        print(
            f"frames {n} = {n / dt:.1f} fps, avg jpeg {pushes['bytes'] // max(n, 1)} B, "
            f"{pushes['bytes'] / 1000 / dt:.1f} KB/s, results pushes {pushes['results']}, "
            f"frame_id out of order {backwards}, pings {pings} (no reply {lost}), "
            f"garbage bytes skipped {dec.skipped}"
        )
        if args.ttl:
            pat = re.compile(r"fps=(\d+) fn=(\S+) proc_ms=(\d+)/.*?enc_ms=(\d+)/.*?txfail=(\d+).*?heap=(\d+) min=(\d+)")
            ms = [m for m in map(pat.search, streaming[2:]) if m]
            if ms:
                avg = lambda i: sum(int(m.group(i)) for m in ms) / len(ms)
                print(
                    f"board while streaming: {ms[-1].group(2)} tft {avg(1):.1f} fps, proc {avg(3):.1f} ms, "
                    f"jpeg {avg(4):.1f} ms, txfail {sum(int(m.group(5)) for m in ms)}, "
                    f"internal RAM free {ms[-1].group(6)} / lowest {ms[-1].group(7)}"
                )
            else:
                print("board: no stats lines read from", args.ttl)
        if args.save and pushes["last"]:
            open(args.save, "wb").write(pushes["last"])
            print("saved", args.save, len(pushes["last"]), "bytes, starts", pushes["last"][:3].hex())


def read_ttl(port: str, lines: list, stop: threading.Event) -> None:
    import serial

    p = serial.Serial()
    p.port, p.baudrate, p.timeout = port, 115200, 0.2
    p.dtr = False  # before open(): otherwise opening resets the board
    p.rts = False
    p.open()
    buf = b""
    while not stop.is_set():
        buf += p.read(4096)
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            line = raw.decode("utf-8", "replace")
            if "pipeline: fps=" in line:
                lines.append(line)
    p.close()


asyncio.run(main())
