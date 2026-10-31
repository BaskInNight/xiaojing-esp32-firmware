#include <driver/i2s.h>
#define I2S_WS 15
#define I2S_SD 13
#define I2S_SCK 2
#define I2S_PORT I2S_NUM_0
#define bufferLen 64

int16_t sBuffer[bufferLen];
void setup() {
  Serial.begin(115200);
  Serial.println("Setup I2S ...");

  delay(1000);
  i2s_install();
  i2s_setpin();
  i2s_start(I2S_PORT);
  delay(500);
}

void loop() {

  size_t bytesIn = 0;
  esp_err_t result = i2s_read(I2S_PORT, &sBuffer, bufferLen, &bytesIn, portMAX_DELAY);
  if (result == ESP_OK)
  {
    int samples_read = bytesIn / 8;
    if (samples_read > 0) {
      float mean = 0;
      for (int i = 0; i < samples_read; ++i) {
        mean += (sBuffer[i]);
      }
      mean /= samples_read;
      Serial.println(mean);
    }
  }
}

void i2s_install(){
    const i2s_config_t i2s_config = {
          .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX), // Receive, not transfer
          .sample_rate = 44100,                         // 16KHz
          .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, // could only get it to work with 32bits
          .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // although the SEL config should be left, it seems to transmit on right
          .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
          .intr_alloc_flags = 0,     // Interrupt level 1
          .dma_buf_count = 8,                           // number of buffers
          .dma_buf_len = bufferLen                              // 8 samples per buffer (minimum)  
  };

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
}

void i2s_setpin(){
  const i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = -1,
    .data_in_num = I2S_SD
  };

  i2s_set_pin(I2S_PORT, &pin_config);
}
