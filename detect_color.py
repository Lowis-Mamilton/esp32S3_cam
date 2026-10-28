import cv2
import numpy as np
import serial
import struct
import time

PORT = "COM11"
BAUD = 921600

COLORS = {
    "Blue": (64, 138, 70, 211, 70, 255, 200),
    "Black": (0, 105, 0, 108, 14, 95, 200),
}

def read_exact(ser, n):
    data = bytearray()
    while len(data) < n:
        chunk = ser.read(n - len(data))
        if not chunk:
            return None
        data.extend(chunk)
    return bytes(data)

def read_frame(ser):
    while True:
        b = ser.read(1)
        if not b:
            return None
        if b[0] != 0xAA:
            continue
        b2 = ser.read(1)
        if not b2 or b2[0] != 0x55:
            continue
        break

    header = read_exact(ser, 4)
    if not header:
        return None
    length = struct.unpack("<I", header)[0]
    if length < 100 or length > 200000:
        return None

    jpeg = read_exact(ser, length)
    if not jpeg:
        return None
    return cv2.imdecode(np.frombuffer(jpeg, dtype=np.uint8), cv2.IMREAD_COLOR)

def drain_old_frames(ser, keep_one=True):
    """清掉緩衝區裡堆積的舊圖，只留最新，移動時比較即時"""
    latest = None
    # 非阻塞多讀幾幀
    ser.timeout = 0.01
    for _ in range(8):
        f = read_frame(ser)
        if f is None:
            break
        latest = f
    ser.timeout = 1
    return latest

def detect_colors(frame):
    hsv = cv2.cvtColor(frame, cv2.COLOR_BGR2HSV)
    best_name = None
    best_area = 0
    best_box = None

    for name, (h1, h2, s1, s2, v1, v2, min_area) in COLORS.items():
        mask = cv2.inRange(
            hsv,
            np.array([h1, s1, v1]),
            np.array([h2, s2, v2])
        )
        contours, _ = cv2.findContours(
            mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE
        )
        if not contours:
            continue

        c = max(contours, key=cv2.contourArea)
        area = cv2.contourArea(c)
        if area < min_area:
            continue

        x, y, w, h = cv2.boundingRect(c)
        if area > best_area:
            best_area = area
            best_name = name
            best_box = (x, y, w, h)

    return best_name, best_area, best_box

def main():
    print("開啟序列埠:", PORT)
    ser = serial.Serial(PORT, BAUD, timeout=1)
    time.sleep(2)
    ser.reset_input_buffer()

    cv2.namedWindow("Live")
    print("即時畫面 + 多色辨識 | 按 Q 離開")
    last_print = 0

    while True:
        # 先盡量清舊幀，再取最新
        frame = drain_old_frames(ser)
        if frame is None:
            frame = read_frame(ser)
        if frame is None:
            continue

        frame = cv2.flip(frame, 1)
        frame = cv2.resize(frame, (320, 240), interpolation=cv2.INTER_LINEAR)

        name, area, box = detect_colors(frame)
        view = frame.copy()

        if name and box:
            x, y, w, h = box
            cv2.rectangle(view, (x, y), (x + w, y + h), (0, 255, 0), 1)

            # 小字、固定在左上，不會超出畫面
            label = f"{name} {int(area)}"
            cv2.putText(
                view, label, (8, 18),
                cv2.FONT_HERSHEY_SIMPLEX, 0.45, (0, 255, 0), 1
            )

            now = time.time()
            if now - last_print > 0.5:
                print(f"[{name}] area={int(area)} center=({x + w // 2},{y + h // 2})")
                last_print = now
        else:
            cv2.putText(
                view, "No target", (8, 18),
                cv2.FONT_HERSHEY_SIMPLEX, 0.45, (0, 0, 255), 1
            )

        cv2.imshow("Live", view)
        if cv2.waitKey(1) & 0xFF == ord("q"):
            break

    ser.close()
    cv2.destroyAllWindows()

if __name__ == "__main__":
    main()