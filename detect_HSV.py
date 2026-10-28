import cv2
import numpy as np
import serial
import struct
import time

# ========== 改成你的 COM 埠 ==========
PORT = "COM11"
BAUD = 921600

def nothing(x):
    pass

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

    img = cv2.imdecode(np.frombuffer(jpeg, dtype=np.uint8), cv2.IMREAD_COLOR)
    return img

def main():
    print("開啟序列埠:", PORT)
    ser = serial.Serial(PORT, BAUD, timeout=1)
    time.sleep(2)
    ser.reset_input_buffer()

    cv2.namedWindow("Adjust")
    cv2.namedWindow("Result")

    cv2.createTrackbar("H Min", "Adjust", 0, 179, nothing)
    cv2.createTrackbar("H Max", "Adjust", 179, 179, nothing)
    cv2.createTrackbar("S Min", "Adjust", 0, 255, nothing)
    cv2.createTrackbar("S Max", "Adjust", 255, 255, nothing)
    cv2.createTrackbar("V Min", "Adjust", 0, 255, nothing)
    cv2.createTrackbar("V Max", "Adjust", 255, 255, nothing)

    print("按 Q 離開")

    while True:
        frame = read_frame(ser)
        if frame is None:
            continue
        frame = cv2.flip(frame, 1)
        # ===== 修正 180° 相反（只改 Python）=====
        #frame = cv2.rotate(frame, cv2.ROTATE_180)
        # 若還是不對，改成下面其中一行：
        # frame = cv2.rotate(frame, cv2.ROTATE_90_CLOCKWISE)
        # frame = cv2.rotate(frame, cv2.ROTATE_90_COUNTERCLOCKWISE)
        # frame = cv2.flip(frame, 0)   # 上下翻
        # frame = cv2.flip(frame, 1)   # 左右翻

        # 顯示放大一點，比較好調 HSV
        frame = cv2.resize(frame, (320, 240), interpolation=cv2.INTER_LINEAR)

        hsv = cv2.cvtColor(frame, cv2.COLOR_BGR2HSV)
        h_min = cv2.getTrackbarPos("H Min", "Adjust")
        h_max = cv2.getTrackbarPos("H Max", "Adjust")
        s_min = cv2.getTrackbarPos("S Min", "Adjust")
        s_max = cv2.getTrackbarPos("S Max", "Adjust")
        v_min = cv2.getTrackbarPos("V Min", "Adjust")
        v_max = cv2.getTrackbarPos("V Max", "Adjust")

        mask = cv2.inRange(
            hsv,
            np.array([h_min, s_min, v_min]),
            np.array([h_max, s_max, v_max])
        )
        result = cv2.bitwise_and(frame, frame, mask=mask)

        view = frame
        contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
        if contours:
            c = max(contours, key=cv2.contourArea)
            area = cv2.contourArea(c)
            if area > 200:
                x, y, w, h = cv2.boundingRect(c)
                cv2.rectangle(view, (x, y), (x + w, y + h), (0, 255, 0), 2)
                cv2.putText(
                    view, f"Area:{int(area)}", (x, max(20, y - 8)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 2
                )

        cv2.imshow("Adjust", view)
        cv2.imshow("Result", result)

        if cv2.waitKey(1) & 0xFF == ord("q"):
            break

    ser.close()
    cv2.destroyAllWindows()

if __name__ == "__main__":
    main()