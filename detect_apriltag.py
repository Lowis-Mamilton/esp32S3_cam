import cv2
import numpy as np
import serial
import struct
import time
from pupil_apriltags import Detector

PORT = "COM11"
BAUD = 921600

# 與 HSD028903 預覽區相同
W, H = 320, 240

detector = Detector(
    families="tag36h11",
    nthreads=2,
    quad_decimate=1.0,   # 同尺寸下用 1.0 較準
    quad_sigma=0.0,
    refine_edges=1,
    decode_sharpening=0.25,
)

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

def drain_old_frames(ser):
    latest = None
    ser.timeout = 0.01
    for _ in range(10):
        f = read_frame(ser)
        if f is None:
            break
        latest = f
    ser.timeout = 1
    return latest

def tag_rotation_deg(corners):
    c = np.array(corners, dtype=np.float32)
    dx = c[1][0] - c[0][0]
    dy = c[1][1] - c[0][1]
    return float(np.degrees(np.arctan2(dy, dx)))

def main():
    print("開啟序列埠:", PORT)
    ser = serial.Serial(PORT, BAUD, timeout=1)
    time.sleep(2)
    ser.reset_input_buffer()

    cv2.namedWindow("AprilTag", cv2.WINDOW_NORMAL)
    cv2.resizeWindow("AprilTag", W, H)  # 與 TFT 影像區同大

    print("顯示 320x240（與 HSD028903 預覽區相同）| 按 Q 離開")
    last_print = 0

    while True:
        frame = drain_old_frames(ser)
        
        frame = cv2.flip(frame, 1)
        if frame is None:
            frame = read_frame(ser)
        if frame is None:
            continue

        # 若電腦與 TFT 左右不一致，再開關這行
        frame = cv2.flip(frame, 1)
        x0 = (frame.shape[1] - 240) // 2   # 320→240，x0=40
        frame = frame[:, x0:x0 + 240]
        # 強制與板子相同解析度（不放大）
        if frame.shape[1] != W or frame.shape[0] != H:
            frame = cv2.resize(frame, (W, H), interpolation=cv2.INTER_LINEAR)

        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        tags = detector.detect(gray)

        view = frame.copy()

        if tags:
            for t in tags:
                tag_id = int(t.tag_id)
                cx, cy = t.center
                corners = t.corners
                rot = tag_rotation_deg(corners)

                pts = corners.astype(np.int32).reshape((-1, 1, 2))
                cv2.polylines(view, [pts], True, (0, 255, 0), 2)
                cv2.circle(view, (int(cx), int(cy)), 4, (0, 0, 255), -1)

                cv2.putText(
                    view, f"ID:{tag_id} {rot:.0f}d",
                    (6, 16),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.45, (0, 255, 0), 1
                )

                now = time.time()
                if now - last_print > 0.4:
                    print(
                        f"[AprilTag] id={tag_id} "
                        f"center=({cx:.1f},{cy:.1f}) rot={rot:.1f}deg"
                    )
                    last_print = now
        else:
            cv2.putText(
                view, "No tag", (6, 16),
                cv2.FONT_HERSHEY_SIMPLEX, 0.45, (0, 0, 255), 1
            )
        print("frame shape:", frame.shape)  # 應為 (240, 240, 3)
        cv2.imshow("AprilTag", view)
        if cv2.waitKey(1) & 0xFF == ord("q"):
            break

    ser.close()
    cv2.destroyAllWindows()

if __name__ == "__main__":
    main()