
#include <HX711.h>

#define DOUT_PIN 26
#define SCK_PIN  27

HX711 scale;

void setup() {
  Serial.begin(115200);
  scale.begin(DOUT_PIN, SCK_PIN);

  Serial.println("Đặt lại mặc định về 0, đảm bảo cân đang trống...");
  scale.set_scale();   // chua co he so calib, set sau
  scale.tare();        // dat ve 0
  Serial.println("Xong! Đặt vật lên để test.");
}

void loop() {
  if (scale.is_ready()) {
    long reading = scale.get_value(10); // trung binh 10 lan doc
    Serial.print("Raw value: ");
    Serial.println(reading);
  } else {
    Serial.println("HX711 chưa sẵn sàng");
  }
  delay(1000);
}
