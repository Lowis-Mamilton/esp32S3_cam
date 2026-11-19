import type { Lang } from '../../i18n'
import type { FunctionId } from '../layout/types'

const ALGO: Record<FunctionId, string> = { 1: 'MXCAM_ALGO_COLOR', 2: 'MXCAM_ALGO_TAG', 3: 'MXCAM_ALGO_FACE' }

const TEXT = {
  'zh-TW': {
    serial: '// 序列埠監控視窗',
    uart: '// Serial1 接 MXCam',
    combine: '// 也可以同時開多個：MXCAM_ALGO_COLOR | MXCAM_ALGO_TAG | MXCAM_ALGO_FACE',
    ask: '// 向 MXCam 要一次結果',
    each: '// 一個一個讀出來',
    colorId: '// r.id 是學習的顏色 ID',
    tagId: '// r.id 是 tag 編號',
    faceId: '// 人臉的 r.id 沒有意義',
    box: '// 位置與大小：r.x, r.y, r.width, r.height',
  },
  en: {
    serial: '// Serial Monitor',
    uart: '// Serial1 is wired to MXCam',
    combine: '// You can combine them: MXCAM_ALGO_COLOR | MXCAM_ALGO_TAG | MXCAM_ALGO_FACE',
    ask: '// Ask MXCam for the current results',
    each: '// Read them one by one',
    colorId: '// r.id is the learned colour ID',
    tagId: '// r.id is the tag number',
    faceId: '// r.id carries no meaning for faces',
    box: '// Position and size: r.x, r.y, r.width, r.height',
  },
} as const

/** Arduino sketch using the MXCam library API of SPEC 6.3. Comments follow the UI language. */
export function snippetFor(fn: FunctionId, lang: Lang): string {
  const c = TEXT[lang]
  const idNote = fn === 1 ? c.colorId : fn === 2 ? c.tagId : c.faceId
  return `#include <MXCam.h>

MXCam cam;

void setup() {
  Serial.begin(115200);   ${c.serial}
  Serial1.begin(115200);  ${c.uart}
  cam.begin(Serial1);
  cam.setAlgorithm(${ALGO[fn]});
  ${c.combine}
}

void loop() {
  if (cam.request()) {    ${c.ask}
    while (cam.available()) {  ${c.each}
      MXCamResult r = cam.read();
      ${idNote}
      ${c.box}
      Serial.print(r.id);
      Serial.print(" ");
      Serial.print(r.x);
      Serial.print(" ");
      Serial.print(r.y);
      Serial.print(" ");
      Serial.print(r.width);
      Serial.print(" ");
      Serial.println(r.height);
    }
  }
  delay(50);
}
`
}
