#include <MXCam.h>

MXCam cam;

void setup() {
  Serial.begin(115200);
  Serial.println(cam.version());
}

void loop() {}
