import type { Lang } from '../../i18n'
import type { FunctionId } from '../layout/types'

const ALGO: Record<FunctionId, string> = { 1: 'MXCAM_ALGO_COLOR', 2: 'MXCAM_ALGO_TAG', 3: 'MXCAM_ALGO_FACE', 4: 'MXCAM_ALGO_LINE', 5: 'MXCAM_ALGO_TRACK', 8: 'MXCAM_ALGO_GESTURE' }

const TEXT = {
  'zh-TW': {
    serial: '// 序列埠監控視窗',
    uart: '// Serial1 接 MXCam',
    ask: '// 向 MXCam 要一次結果',
    each: '// 一個一個讀出來',
    colorId: '// r.id 是學習的顏色 ID',
    tagId: '// r.id 是 tag 編號',
    faceId: '// 人臉的 r.id 沒有意義，信心度是 r.confidence（0–100）',
    gestureId: '// r.id 是手勢：1–5 比幾根手指、6 讚、7 OK、8 打電話、9 倒讚、0 有手但認不出；r.confidence 是 0–100',
    gestureLike: '// 比讚的時候',
    box: '// 位置與大小：r.x, r.y, r.width, r.height',
    lineVals: '// r.angle 是角度（0 = 垂直向上、順時針為正）、r.offset 是底端相對畫面中央的偏移（正 = 線在右邊）',
    lineCross: '// 遇到路口',
    trackVals: '// r.x、r.width 是物體的位置與大小，r.confidence 是 0–100',
    trackLost: '// 沒有結果 = 物體跟丟了',
  },
  en: {
    serial: '// Serial Monitor',
    uart: '// Serial1 is wired to MXCam',
    ask: '// Ask MXCam for the current results',
    each: '// Read them one by one',
    colorId: '// r.id is the learned colour ID',
    tagId: '// r.id is the tag number',
    faceId: '// r.id carries no meaning for faces; r.confidence is 0-100',
    gestureId: '// r.id is the gesture: 1-5 fingers up, 6 like, 7 OK, 8 call, 9 dislike, 0 a hand it cannot name; r.confidence is 0-100',
    gestureLike: '// When the hand gives a thumbs up',
    box: '// Position and size: r.x, r.y, r.width, r.height',
    lineVals: '// r.angle: 0 = straight up, clockwise positive; r.offset: bottom end minus the picture centre (positive = line on the right)',
    lineCross: '// At a crossing',
    trackVals: '// r.x, r.width: where the object is and how big; r.confidence is 0-100',
    trackLost: '// No result = the object is lost',
  },
} as const

/** Arduino sketch using the MXCam library API of SPEC 6.3. Comments follow the UI language. */
export function snippetFor(fn: FunctionId, lang: Lang): string {
  const c = TEXT[lang]
  const body: Record<FunctionId, string> = {
    1: boxBody(c.colorId, c.box, 'r.id'),
    2: boxBody(c.tagId, c.box, 'r.id'),
    3: boxBody(c.faceId, c.box, 'r.confidence'),
    4: `      ${c.lineVals}
      Serial.print(r.angle);
      Serial.print(" ");
      Serial.println(r.offset);
      if (r.flags & MXCAM_LINE_CROSS) {  ${c.lineCross}
        Serial.println("cross!");
      }`,
    5: `      ${c.trackVals}
      Serial.print(r.x);
      Serial.print(" ");
      Serial.print(r.width);
      Serial.print(" ");
      Serial.println(r.confidence);`,
    8: `${boxBody(c.gestureId, c.box, 'r.id')}
      if (r.id == MXCAM_GESTURE_LIKE) {  ${c.gestureLike}
        Serial.println("like!");
      }`,
  }
  return `#include <MXCam.h>

MXCam cam;

void setup() {
  Serial.begin(115200);   ${c.serial}
  Serial1.begin(115200);  ${c.uart}
  cam.begin(Serial1);
  cam.setAlgorithm(${ALGO[fn]});
}

void loop() {
  if (cam.request()) {    ${c.ask}
${fn === 5 ? `    if (cam.available()) {     ${c.each}
      MXCamResult r = cam.read();
${body[fn]}
    } else {  ${c.trackLost}
      Serial.println("lost");
    }` : `    while (cam.available()) {  ${c.each}
      MXCamResult r = cam.read();
${body[fn]}
    }`}
  }
  delay(50);
}
`
}

/** The common body: a note on r.id, the box, and the box printed. */
function boxBody(idNote: string, boxNote: string, first: string): string {
  return `      ${idNote}
      ${boxNote}
      Serial.print(${first});
      Serial.print(" ");
      Serial.print(r.x);
      Serial.print(" ");
      Serial.print(r.y);
      Serial.print(" ");
      Serial.print(r.width);
      Serial.print(" ");
      Serial.println(r.height);`
}
