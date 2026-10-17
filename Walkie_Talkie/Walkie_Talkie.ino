#include <WiFi.h>
#include <esp_now.h>
#include <driver/i2s.h>

//Inputs and outputs
#define I2S_WS      19  // L/R clock for INMP441 i2s microphone module
#define I2S_SCK     18  // BCLK for INMP441 i2s microphone module
#define I2S_SD      21  // Data out from INMP441 i2s microphone module
#define I2S_DOUT    5   // Data out pin for the MAX98357A i2s amplifier
#define SendPin     23  // Latch switch pin (3.3V to send)

//Code EDITABLE Variables
bool broadcast_mode = true;  //If broadcast mode is on, all the devices on the network will receive the message
uint8_t BroadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  
uint8_t receiverMac[] = {0xB4, 0xE6, 0x2D, 0xB7, 0x49, 0x1D};   
const int GAIN = 5; 
const i2s_port_t I2S_SPEAKER_PORT = I2S_NUM_0;  
const i2s_port_t I2S_MIC_PORT = I2S_NUM_1;      

//Code CONSTANT Variables
bool SendPinState = false;  // *CHANGED*: Start with state LOW because of pulldown
bool sending = false;       
esp_err_t err;              


/////////////////////////////////////////////////////////////////
///////////////////SETUP THE MICROPHONE I2S//////////////////////
void setupI2SMic() {
  const i2s_config_t i2s_config_mic = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S_MSB,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 128,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  const i2s_pin_config_t pin_config_mic = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };
  err = i2s_driver_install(I2S_MIC_PORT, &i2s_config_mic, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("Failed installing Microphone driver: %d\n", err);    
  }  
  err = i2s_set_pin(I2S_MIC_PORT, &pin_config_mic);
  if (err != ESP_OK) {
    Serial.printf("Failed setting microphone pins: %d\n", err);
  }
  i2s_zero_dma_buffer(I2S_MIC_PORT);
}

/////////////////////////////////////////////////////////////////
////////////////////SETUP THE AMPLIFIER I2S//////////////////////
void setupI2SAmplifier() {
  const i2s_config_t i2s_config_speaker = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S_MSB,
    .intr_alloc_flags = 0,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  const i2s_pin_config_t pin_config_speaker = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  err = i2s_driver_install(I2S_SPEAKER_PORT, &i2s_config_speaker, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("Failed installing Amplifier driver: %d\n", err);    
  }  
  err = i2s_set_pin(I2S_SPEAKER_PORT, &pin_config_speaker);
  if (err != ESP_OK) {
    Serial.printf("Failed setting speaker pins: %d\n", err);
  }
  i2s_zero_dma_buffer(I2S_SPEAKER_PORT);
}

/////////////////////////////////////////////////////////////////
/////////////////////SETUP OTHER RECEIVERS///////////////////////
void setPeer (){
  esp_now_peer_info_t peerInfo = {};     
  memcpy(peerInfo.peer_addr, receiverMac, 6);  
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    return;
  }
}

/////////////////////////////////////////////////////////////////
//////////WHAT HAPPENS WHEN WE RECEIVE DATA WITH ESPNOW//////////
void OnDataRecv(const esp_now_recv_info *info, const uint8_t *data, int len) {
  if (len > 0 && len <= 512 && len % 2 == 0) {
    size_t bytes_written;
    Serial.println("Received");
    if(!sending){
      i2s_write(I2S_SPEAKER_PORT, data, len, &bytes_written, portMAX_DELAY);
    }
  }
}

/////////////////////////////////////////////////////////////////
////////////WHAT HAPPENS WHEN WE SEND DATA WITH ESPNOW///////////
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status){
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
}


void setup() {  
  if(broadcast_mode){
    for(int i=0; i<6; i++){
      receiverMac[i] = BroadcastMac[i];
    }
  }
  
  pinMode(SendPin, INPUT_PULLDOWN);           
  
  Serial.begin(115200);                     
  
  // ---> ADD THESE TWO LINES HERE <---
  // Wake up the amplifier on Pin 22!
  pinMode(22, OUTPUT);
  digitalWrite(22, HIGH);
  
  WiFi.mode(WIFI_STA);                      
  setupI2SAmplifier();                      

  if (esp_now_init() != ESP_OK) {           
    Serial.println("ESP-NOW Init Failed");  
    return;
  }

  setPeer();                                
  esp_now_register_recv_cb((esp_now_recv_cb_t)OnDataRecv);     

  Serial.println("ESP32 Walkie Talkie Initialized");
}

void loop() {
  /////////////////////////////////////////////////////////////////
  ////////////WHEN WE FLIP LATCH TO ON (3.3V) WE SEND AUDIO////////
  if(digitalRead(SendPin) == HIGH && !SendPinState){
    SendPinState = true;       
    Serial.println("Latch ON: Transmitting...");
    
    // 1. Uninstall the speaker to free up the shared I2S pins
    i2s_driver_uninstall(I2S_SPEAKER_PORT);
    
    // 2. Setup the microphone
    setupI2SMic();                          
    esp_now_register_send_cb((esp_now_send_cb_t)OnDataSent); 
    sending = true;       
  }

  ////////////WHEN WE FLIP LATCH TO OFF (GND) WE RECEIVE AUDIO/////
  if(digitalRead(SendPin) == LOW && SendPinState){    
    SendPinState = false;
    Serial.println("Latch OFF: Listening...");

    // 1. Uninstall the microphone to free up the shared I2S pins
    i2s_driver_uninstall(I2S_MIC_PORT);
    
    // 2. Setup the speaker back to receive mode
    setupI2SAmplifier();                    
    esp_now_register_recv_cb((esp_now_recv_cb_t)OnDataRecv);
    sending = false;  
  }
  
  /////////////////////////////////////////////////////////////////////
  //WHEN "SENDING" VARIABLE IS 1 WE READ MICROPHONE AND SEND THE DATA//
  if(sending){
    const int samples = 100;  // 100 samples x 2 bytes = 200 bytes
    int16_t buffer[samples];
    size_t bytesRead;

    // Read I2S data from mic
    i2s_read(I2S_MIC_PORT, &buffer, sizeof(buffer), &bytesRead, portMAX_DELAY);

    if (bytesRead > 0) {
      // Apply software gain safely
      for (int i = 0; i < samples; i++) {
        int32_t amplified = buffer[i] * GAIN;        
        // Clamp to int16_t range to prevent overflow
        if (amplified > 32767) amplified = 32767;
        if (amplified < -32768) amplified = -32768;
        buffer[i] = (int16_t)amplified;
      }
      // Send audio buffer over ESP-NOW
      esp_now_send(receiverMac, (uint8_t*)buffer, bytesRead);
    }
    delay(2);  // Light throttle for stability
  }
}
