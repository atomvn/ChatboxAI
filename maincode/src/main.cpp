/**
  7:01: fixing mic 
 */

// Libraries
#include <driver/i2s.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "config.h"

// RTOS Ticks Delay
#define TickDelay(ms) vTaskDelay(pdMS_TO_TICKS(ms))

// INMP441 Ports
#define I2S_WS 15
#define I2S_SD 4
#define I2S_SCK 2

// MAX98357A Ports
#define I2S_DOUT 26
#define I2S_BCLK 27
#define I2S_LRC 14

// Wake-up Button
#define Button_Pin GPIO_NUM_33

// LED Ports
#define isWifiConnectedPin 25
#define isAudioRecording 32

// MAX98357A I2S Setup
#define MAX_I2S_NUM I2S_NUM_1
#define MAX_I2S_SAMPLE_RATE (16000)
#define MAX_I2S_SAMPLE_BITS (16)
#define MAX_I2S_READ_LEN (256)

// INMP441 I2S Setup
#define I2S_PORT I2S_NUM_0
#define I2S_SAMPLE_RATE (16000)
#define I2S_SAMPLE_BITS (16)
#define I2S_READ_LEN (16 * 1024)
#define RECORD_TIME (5) // Seconds
#define I2S_CHANNEL_NUM (1)
#define FLASH_RECORD_SIZE (I2S_CHANNEL_NUM * I2S_SAMPLE_RATE * I2S_SAMPLE_BITS / 8 * RECORD_TIME)

File file;
SemaphoreHandle_t i2sFinishedSemaphore;
const char audioRecordfile[] = "/recording.wav";
const char audioResponsefile[] = "/voicedby.wav";
const int headerSize = 44;

bool isWIFIConnected;

// Node Js server Adresses
const char *serverUploadUrl = "http://192.168.1.4:3000/uploadAudio";
const char *serverBroadcastUrl = "http://192.168.1.4:3000/broadcastAudio";
const char *broadcastPermitionUrl = "http://192.168.1.4:3000/checkVariable";

// Prototypes
void SPIFFSInit();
void listSPIFFS(void);
void i2sInitINMP441();
void i2sInitMax98357A();
void wifiConnect(void *pvParameters);
void I2SAudioRecord(void *arg);
void I2SAudioRecord_dataScale(uint8_t *d_buff, uint8_t *s_buff, uint32_t len);
void wavHeader(byte *header, int wavSize);
void uploadFile();
void semaphoreWait(void *arg);
void broadcastAudio(void *arg);
void printSpaceInfo();

void setup()
{
  esp_sleep_enable_ext0_wakeup(Button_Pin, 1);
  if (digitalRead(Button_Pin) == LOW)
  {
    esp_deep_sleep_start();
  }

  Serial.begin(115200);
  TickDelay(500);
  pinMode(isWifiConnectedPin, OUTPUT);
  digitalWrite(isWifiConnectedPin, LOW);
  pinMode(isAudioRecording, OUTPUT);
  digitalWrite(isAudioRecording, LOW);

  SPIFFSInit();
  i2sInitINMP441();
  i2sFinishedSemaphore = xSemaphoreCreateBinary();
  xTaskCreate(I2SAudioRecord, "I2SAudioRecord", 4096, NULL, 2, NULL);
  TickDelay(500);
  xTaskCreate(wifiConnect, "wifi_Connect", 2048, NULL, 1, NULL);
  TickDelay(500);
  xTaskCreate(semaphoreWait, "semaphoreWait", 2048, NULL, 0, NULL);
}

void loop()
{
}

void SPIFFSInit()
{
  //Xóa toàn bộ dữ liệu hiện có trong vùng nhờ PIFS và định lại lại hệ thống, đưa về trạng thái rỗng
  //SPIFFS.format();
  if (!SPIFFS.begin(true))
  {
    Serial.println("SPIFFS initialisation failed!");
    while (1)
      yield();
  }

  // nếu spiff còn file cũ, xóa file 
  if (SPIFFS.exists(audioRecordfile))
  {
    SPIFFS.remove(audioRecordfile);
  }
  if (SPIFFS.exists(audioResponsefile))
  {
    SPIFFS.remove(audioResponsefile);
  }
  
  //Mở tệp ở chế độ ghi 
  file = SPIFFS.open(audioRecordfile, FILE_WRITE);
  if (!file)
  {
    Serial.println("File is not available!");
  }

  

  //tạo header WAV cho tệp ghi âm
  byte header[headerSize];
  wavHeader(header, FLASH_RECORD_SIZE);

  //ghi header vào tệp
  file.write(header, headerSize);

  //+Đóng file để ghi đc 
  file.close();

  //gọi hàm list spiff để debug
  listSPIFFS();
}

void i2sInitINMP441()
{
  i2s_config_t i2s_config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = I2S_SAMPLE_RATE,
      .bits_per_sample = i2s_bits_per_sample_t(I2S_SAMPLE_BITS),
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_I2S),
      .intr_alloc_flags = 0,
      .dma_buf_count = 64,
      .dma_buf_len = 1024,
      .use_apll = 1};

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);

  const i2s_pin_config_t pin_config = {
      .bck_io_num = I2S_SCK,
      .ws_io_num = I2S_WS,
      .data_out_num = -1,
      .data_in_num = I2S_SD};

  i2s_set_pin(I2S_PORT, &pin_config);
}
/*
//xử lí dữ liệu âm thanh trước khi ghi vào recording.wav 
void I2SAudioRecord_dataScale(uint8_t *d_buff, uint8_t *s_buff, uint32_t len)
{
  uint32_t j = 0;
  uint32_t dac_value = 0;
  for (int i = 0; i < len; i += 2)
  {
    //đọc 2 byte từ s_buff và chuyển thành giá trị số
    dac_value = ((((uint16_t)(s_buff[i + 1] & 0xf) << 8) | ((s_buff[i + 0]))));
    //                                    //8 byte cao        //8 byte thấp
    //d_buff[j++] = 0; - Ghi một byte 0 vào d_buff tại vị trí j, sau đó tăng j.
    //Byte 0 này không cần thiết cho định dạng WAV mono (1 kênh). Nó tạo ra dữ liệu giống 
    //như stereo (2 byte/mẫu, nhưng chỉ 1 byte có giá trị), 
    //sai với cấu hình I2S_CHANNEL_NUM = 1 (mono).
    d_buff[j++] = dac_value * 256 / 2048;
  }
}
*/

//chuyển sang xử lý thử bằng hàm dưới đây, ghi trực tiếp giữ liệu từ s_buff vào d _buff sau đó khuếch đại lên
void I2SAudioRecord_dataScale(uint8_t *d_buff, uint8_t *s_buff, uint32_t len) {
  // Sao chép trực tiếp
  memcpy(d_buff, s_buff, len);
  // Tùy chọn: Khuếch đại âm thanh (nếu cần)
  for (uint32_t i = 0; i < len; i += 2) {
    int16_t sample = ((int16_t)(s_buff[i + 1] << 8) | s_buff[i]); // Little-endian
    sample = sample * 15; // Khuếch đại gấp 2 (cẩn thận clipping), tăng giá trị biên độ lên để âm to hơn
    d_buff[i] = (uint8_t)(sample & 0xFF);
    d_buff[i + 1] = (uint8_t)(sample >> 8);
  }
}

void I2SAudioRecord(void *arg)
{

  int i2s_read_len = I2S_READ_LEN;  // kích thước mỗi lần đọc, 16x1024 = 16384 bytes
  int flash_wr_size = 0; //số byte đã ghi 
  size_t bytes_read; // lưu số byte thực tế mỗi lần đọc 

  char *i2s_read_buff = (char *)calloc(i2s_read_len, sizeof(char)); //cấp phát vùng nhớ lưu dữ liệu thô, cấp phát 16384 bytes cho mỗi buffer, khởi tạo tất cả về 0 đảm bảo buffer sạch
  uint8_t *flash_write_buff = (uint8_t *)calloc(i2s_read_len, sizeof(char)); //cấp phát vùng nhớ lưu dữ liệu đã xử lý
  //+thêm code để dubug xem có cấp phát đủ vùng nhớ không 
  if (!i2s_read_buff || !flash_write_buff) {
    Serial.println("Failed to allocate buffers");
    vTaskDelete(NULL);
  }

  //đọc hai lần dữ liệu từ I2S để ổn định trc khi ghi âm 
  //đọc từ PORT 0, lưu vào bộ nhớ read, độ dài tối đa read_len, lưu số byte thực tế đọc đc vào bytes_read, delay vô hạn nếu lỗi
  i2s_read(I2S_PORT, (void *)i2s_read_buff, i2s_read_len, &bytes_read, portMAX_DELAY);
  //+thêm để debug xem thực tế mỗi lần đọc đc bnh, tránh trg hợp mic ko ghi vẫn tiếp tục
  Serial.printf("Initial read 1: %u bytes\n", bytes_read);
  i2s_read(I2S_PORT, (void *)i2s_read_buff, i2s_read_len, &bytes_read, portMAX_DELAY);
  //+thêm để debug xem thực tế mỗi lần đọc đc bnh, tránh trg hợp mic ko ghi vẫn tiếp tục
  Serial.printf("Initial read 1: %u bytes\n", bytes_read);

  digitalWrite(isAudioRecording, HIGH);
  Serial.println(" *** Recording Start *** ");

  //+Thêm code để mở Spiff trước khi ghi 
  file = SPIFFS.open(audioRecordfile, FILE_APPEND);
  if (!file) {
    Serial.println("Failed to open file for appending");
    free(i2s_read_buff);
    free(flash_write_buff);
    vTaskDelete(NULL);
  }

  while (flash_wr_size < FLASH_RECORD_SIZE)
  {
    // đọc 16384 từ I2S vào i2s_read_buff
    i2s_read(I2S_PORT, (void *)i2s_read_buff, i2s_read_len, &bytes_read, portMAX_DELAY);
    //+thêm để debug xem thực tế mỗi lần đọc đc bnh, tránh trg hợp mic ko ghi vẫn tiếp tục
    Serial.printf("Read: %u bytes\n", bytes_read);

    // xử lý dữ liệu từ i2s_read_buff và lưu vào flash_write_size 
    I2SAudioRecord_dataScale(flash_write_buff, (uint8_t *)i2s_read_buff, i2s_read_len);
    //Code  cũ ghi vào file write 
    //file.write((const byte *)flash_write_buff, i2s_read_len);
    //flash_wr_size += i2s_read_len;

    //+thêm đoạn debug số byte đã đọc ghi vào flash_wrie_buff
    //tránh mất dữ liệu khi gián đoạn trước khi tệp đc đóng
    file.flush();
    size_t bytes_written = file.write((const byte *)flash_write_buff, bytes_read);
    if (bytes_written != bytes_read) {
      Serial.printf("Failed to write to file, written: %u, expected: %u\n", bytes_written, bytes_read);
      break;
    }
    flash_wr_size += bytes_written;

    ets_printf("Sound recording %u%%\n", flash_wr_size * 100 / FLASH_RECORD_SIZE);
    ets_printf("Never Used Stack Size: %u\n", uxTaskGetStackHighWaterMark(NULL));
  }

  file.close();

  digitalWrite(isAudioRecording, LOW);

  //giải phóng bộ nhớ 
  free(i2s_read_buff);
  i2s_read_buff = NULL;
  free(flash_write_buff);
  flash_write_buff = NULL;

  //liệt kê danh sách file 
  listSPIFFS();

  //nếu có mạng upload
  if (isWIFIConnected)
  {
    uploadFile();
  }

  //give semaphore báo hiệu đã xong 
  xSemaphoreGive(i2sFinishedSemaphore); // После завершения задачи I2SAudioRecord отдаем семафор
  vTaskDelete(NULL);
}

void wavHeader(byte *header, int wavSize)
{
  header[0] = 'R';
  header[1] = 'I';
  header[2] = 'F';
  header[3] = 'F';
  unsigned int fileSize = wavSize + headerSize - 8;
  header[4] = (byte)(fileSize & 0xFF);
  header[5] = (byte)((fileSize >> 8) & 0xFF);
  header[6] = (byte)((fileSize >> 16) & 0xFF);
  header[7] = (byte)((fileSize >> 24) & 0xFF);
  header[8] = 'W';
  header[9] = 'A';
  header[10] = 'V';
  header[11] = 'E';
  header[12] = 'f';
  header[13] = 'm';
  header[14] = 't';
  header[15] = ' ';
  header[16] = 0x10;
  header[17] = 0x00;
  header[18] = 0x00;
  header[19] = 0x00;
  header[20] = 0x01;
  header[21] = 0x00;
  header[22] = 0x01;
  header[23] = 0x00;
  header[24] = 0x80;
  header[25] = 0x3E;
  header[26] = 0x00;
  header[27] = 0x00;
  header[28] = 0x00;
  header[29] = 0x7D;
  header[30] = 0x01;
  header[31] = 0x00;
  header[32] = 0x02;
  header[33] = 0x00;
  header[34] = 0x10;
  header[35] = 0x00;
  header[36] = 'd';
  header[37] = 'a';
  header[38] = 't';
  header[39] = 'a';
  header[40] = (byte)(wavSize & 0xFF);
  header[41] = (byte)((wavSize >> 8) & 0xFF);
  header[42] = (byte)((wavSize >> 16) & 0xFF);
  header[43] = (byte)((wavSize >> 24) & 0xFF);
}

void listSPIFFS(void)
{
  // DEBUG
  printSpaceInfo();
  Serial.println(F("\r\nListing SPIFFS files:"));
  static const char line[] PROGMEM = "=================================================";

  Serial.println(FPSTR(line));
  Serial.println(F("  File name                              Size"));
  Serial.println(FPSTR(line));

  fs::File root = SPIFFS.open("/");
  if (!root)
  {
    Serial.println(F("Failed to open directory"));
    return;
  }
  if (!root.isDirectory())
  {
    Serial.println(F("Not a directory"));
    return;
  }

  fs::File file = root.openNextFile();
  while (file)
  {

    if (file.isDirectory())
    {
      Serial.print("DIR : ");
      String fileName = file.name();
      Serial.print(fileName);
    }
    else
    {
      String fileName = file.name();
      Serial.print("  " + fileName);
      // File path can be 31 characters maximum in SPIFFS
      int spaces = 33 - fileName.length(); // Tabulate nicely
      if (spaces < 1)
        spaces = 1;
      while (spaces--)
        Serial.print(" ");
      String fileSize = (String)file.size();
      spaces = 10 - fileSize.length(); // Tabulate nicely
      if (spaces < 1)
        spaces = 1;
      while (spaces--)
        Serial.print(" ");
      Serial.println(fileSize + " bytes");
    }

    file = root.openNextFile();
  }

  Serial.println(FPSTR(line));
  Serial.println();
  TickDelay(1000);
}

void wifiConnect(void *pvParameters)
{
  isWIFIConnected = false;

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  if (WiFi.status() != WL_CONNECTED)
  {
    digitalWrite(isWifiConnectedPin, LOW);
  }
  while (WiFi.status() != WL_CONNECTED)
  {
    vTaskDelay(500);
    Serial.print("Connection failed!");
  }
  isWIFIConnected = true;
  digitalWrite(isWifiConnectedPin, HIGH);
  while (true)
  {
    vTaskDelay(1000);
  }
}

void uploadFile()
{
  //Mở tệp spiffs
  file = SPIFFS.open(audioRecordfile, FILE_READ);
  if (!file)
  {
    Serial.println("FILE IS NOT AVAILABLE!");
    return;
  }

  Serial.println("===> Upload FILE to Node.js Server");

  //+Thêm code để debug, in ra dung lượng file, heap còn trống và kiểm tra kết nối wifi để debug 
  Serial.printf("File size: %u bytes\n", file.size());
  Serial.printf("Free heap before upload: %u bytes\n", ESP.getFreeHeap());

  if (!isWIFIConnected || WiFi.status() != WL_CONNECTED)
  {
    Serial.println("WiFi not connected, cannot upload");
    file.close();
    i2s_driver_uninstall(I2S_NUM_0);
    esp_deep_sleep_start();
    return;
  }

  //Tạo đối tượng http, để gửi êu cầu POST
  HTTPClient client;
  //upload file với header audio/wav
  client.begin(serverUploadUrl);

  //+Thêm code debug, để tránh server đóng kết nối TCP sớm, thêm timout để chương trình có thể xử lí tín hiệu
  client.setTimeout(30000); // Timeout 30 giây
  client.setConnectTimeout(15000); // Timeout kết nối 15 giây

  client.addHeader("Content-Type", "audio/wav");
  int httpResponseCode = client.sendRequest("POST", &file, file.size());
  Serial.print("httpResponseCode : ");
  Serial.println(httpResponseCode);

  //Nếu trả về mã 200 thì thực hiện chương trình
  if (httpResponseCode == 200)
  {
    String response = client.getString();
    Serial.println("==================== Transcription ====================");
    Serial.println(response);
    Serial.println("====================      End      ====================");
  }
  else
  {
    Serial.println("Server is not available... Deep sleep.");
    esp_deep_sleep_start();
  }
  //đóng file, client
  file.close();
  client.end();

  // Освобождение I2S ресурсов
  i2s_driver_uninstall(I2S_NUM_0);
}

void semaphoreWait(void *arg) {
  HTTPClient http;
  while (true) {
    if (xSemaphoreTake(i2sFinishedSemaphore, 0) == pdTRUE) {
      http.begin(broadcastPermitionUrl);
      //http.setTimeout(60000); // Timeout 60 giây
      //http.setConnectTimeout(30000); // Timeout kết nối 30 giây
      int httpResponseCode = http.GET();

      if (httpResponseCode > 0) {
        String payload = http.getString();
        if (payload.indexOf("\"ready\":true") > -1) {
          Serial.println("Recieving confirmed! Start broadcasting...");
          xTaskCreate(broadcastAudio, "broadcastAudio", 8192, NULL, 2, NULL); // Tăng stack
          http.end();
          break;
        } else {
          Serial.println("Waiting for broadcast confirmation from Server...");
        }
      } else {
        Serial.print("HTTP request failed with error code: ");
        Serial.println(httpResponseCode);
        Serial.println("Start sleep.");
        esp_deep_sleep_start();
      }
      xSemaphoreGive(i2sFinishedSemaphore);
      http.end();
    }
    vTaskDelay(500);
  }
  vTaskDelete(NULL);
}

void broadcastAudio(void *arg) {
  //Init I2S để truyền âm từ ESP32 ra lòa
  i2sInitMax98357A();
  //Khởi tạo kết nối HTTP
  HTTPClient http;
  //Khởi tạo kết nối đến endpoint /broadcastAudio trên server
  http.begin(serverBroadcastUrl);
  http.setTimeout(30000); // Thời gian tối đa để ESP32 đợi server trả dữ liệu về sau khi kết nối thành công
  http.setConnectTimeout(10000); //thời gian tối đa connect tới server, nếu ko kết nối đc báo lỗi kết nối
  //Kiểm tra heap: In dung lượng heap còn trống để debug, đảm bảo đủ bộ nhớ cho luồng HTTP và I2S
  Serial.printf("Free heap before stream: %u bytes\n", ESP.getFreeHeap());
  //+chỗ này có thể thêm đoạn code debug nếu heap nhỏ hơn 10KB, thì thông báo
  /*
  if (ESP.getFreeHeap() < 10000) {
  Serial.println("Low heap, aborting broadcast");
  http.end();
  i2s_driver_uninstall(MAX_I2S_NUM);
  esp_deep_sleep_start();
  vTaskDelete(NULL);
  } 
  */
  //gửi yêu cầu HTTP GET, trả về mã trạng thái 200, 400, 500, 1, 11
  int httpCode = http.GET();
  //Luồng xử lý âm thanh, nếu trạng thái trả về 200 thì bắt đầu phát
  if (httpCode == HTTP_CODE_OK) {
    //trả về con trỏ đến đối tượng WiFiClient
    WiFiClient *stream = http.getStreamPtr();
    //tạo buffer để lưu dữ liệu âm thanh tạm thời
    uint8_t buffer[MAX_I2S_READ_LEN]; //256 bytes
    //Bắt đầu phát
    Serial.println("Starting broadcastAudio ");
    //vòng lặp phát âm
    while (stream->connected() && stream->available()) {
      //len = số byte thực tế đọc được
      int len = stream->read(buffer, sizeof(buffer)); //đọc luồng: đọc tối đa 256 bytes vào buffer 
      //Nếu buffer nhận được dữ liệu, bắt đầu phát
      if (len > 0) {
        //ghi len từ buffer vào I2S để phát ra loa
        size_t bytes_written; //số byte thực tế ghi vào I2S
        i2s_write((i2s_port_t)MAX_I2S_NUM, buffer, len, &bytes_written, portMAX_DELAY);
        //+có thể thêm đoạn code debug, kiểm tra xem số byte ghi vào I2S có bằng số byte nhận được từ server không
        /*
        if (bytes_written != len) {
          Serial.printf("I2S write error: wrote %u, expected %d\n", bytes_written, len);
        }
          */
        //In số byte đã stream và heap còn lại
        Serial.printf("Streamed %d bytes, heap: %u\n", len, ESP.getFreeHeap());
      }
    }
    Serial.println("Audio playback completed");
  } else {
    //thông báo lỗi nếu không get
    Serial.printf("HTTP GET failed, error: %s\n", http.errorToString(httpCode).c_str());
  }

  //dọn dẹp tài nguyên
  http.end();
  i2s_driver_uninstall(MAX_I2S_NUM);
  Serial.println("Going to sleep after broadcasting");
  esp_deep_sleep_start();
  //Hủy task FreeRTOS hiện tại, giải phóng tài nguyên.
  //Đảm bảo task không tiếp tục chạy sau khi hoàn thành.
  //Dòng này không cần thiết vì esp_deep_sleep_start() 
  //đã dừng toàn bộ chương trình. Có thể bỏ để đơn giản hóa.
  //vTaskDelete(NULL);
}

void i2sInitMax98357A()
{
  i2s_config_t i2s_config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
      .sample_rate = MAX_I2S_SAMPLE_RATE,
      .bits_per_sample = i2s_bits_per_sample_t(MAX_I2S_SAMPLE_BITS),
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S),
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = MAX_I2S_READ_LEN,
      .use_apll = false,
      .tx_desc_auto_clear = true,
      .fixed_mclk = 0};

  i2s_pin_config_t pin_config = {
      .bck_io_num = I2S_BCLK,
      .ws_io_num = I2S_LRC,
      .data_out_num = I2S_DOUT,
      .data_in_num = I2S_PIN_NO_CHANGE};

  i2s_driver_install(MAX_I2S_NUM, &i2s_config, 0, NULL);
  i2s_set_pin(MAX_I2S_NUM, &pin_config);
  i2s_zero_dma_buffer(MAX_I2S_NUM);
}

void printSpaceInfo()
{
  size_t totalBytes = SPIFFS.totalBytes();
  size_t usedBytes = SPIFFS.usedBytes();
  size_t freeBytes = totalBytes - usedBytes;

  Serial.print("Total space: ");
  Serial.println(totalBytes);
  Serial.print("Used space: ");
  Serial.println(usedBytes);
  Serial.print("Free space: ");
  Serial.println(freeBytes);
}