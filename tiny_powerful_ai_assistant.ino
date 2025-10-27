/*
 * XIAO ESP32S3 Unified Voice Assistant (Recorder, STT, LLM, TTS)
 *
 * This unified code performs the following steps:
 * 1. Records audio from the built-in PDM microphone (triggered by button D1).
 * 2. Sends the audio to ElevenLabs for Speech-to-Text (STT).
 * 3. Takes the transcribed text and sends it to the Gemini API (gemini-2.5-flash) as a prompt.
 * 4. Prints the AI's response to the Serial Monitor.
 * 5. Speaks the AI's response using the MAX98357A I2S Speaker module.
 *
 * Requirements:
 * - XIAO ESP32S3
 * - MAX98357A I2S Speaker connected to D2, D3, D4 (GPIO4, GPIO5, GPIO6)
 * - SD Card module (for temporary WAV storage). CS pin must be connected to GPIO21.
 * - ArduinoJson library
 * - ESP32-audioI2S library
 */

#include "driver/i2s_pdm.h"
#include "driver/gpio.h"
#include <Arduino.h>
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "Audio.h" // ESP32-audioI2S library for TTS

// --------------------------------------------------------------------------
// Configuration
// --------------------------------------------------------------------------

const char* ssid = "Your_wifi_SSID";
const char* password = "your_wifi_passward";
const char* elevenlabs_api_key = "your_elevenlabs_api_key";
const char* elevenlabs_stt_url = "https://api.elevenlabs.io/v1/speech-to-text";
const char* gemini_api_key = "your_gemini_api_key"; 
const char* gemini_url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash-preview-09-2025:generateContent";
const char* system_instruction = "You are a helpful, concise, and friendly voice assistant. Respond directly to the user's question or command in a brief and clear manner.";

// Audio recording settings
#define WAV_FILE_NAME "recording"
#define SAMPLE_RATE 16000U
#define SAMPLE_BITS 16
#define WAV_HEADER_SIZE 44
#define VOLUME_GAIN 2

// I2S PDM Configuration for XIAO ESP32S3 built-in microphone
#define I2S_NUM I2S_NUM_0
#define PDM_CLK_GPIO (gpio_num_t)42  // D9
#define PDM_DIN_GPIO (gpio_num_t)41  // D10
#define BUTTON_PIN D1                // GPIO3

// MAX98357A Speaker I2S Pin Definitions for XIAO ESP32S3
#define I2S_DOUT (gpio_num_t)4       // D2 - Data
#define I2S_BCLK (gpio_num_t)5       // D3 - Bit Clock
#define I2S_LRC  (gpio_num_t)6       // D4 - Word Clock (Left/Right Clock)

// --------------------------------------------------------------------------
// Global Variables
// --------------------------------------------------------------------------

i2s_chan_handle_t rx_handle = NULL;
bool recording_active = false;
String last_transcription = "";
bool wifi_connected = false;
String current_recording_file = "";
bool isPressed = false;

Audio audio;

// --------------------------------------------------------------------------
// Function Declarations
// --------------------------------------------------------------------------
bool connectToWiFi();
bool init_i2s_pdm();
void deinit_i2s_pdm();
void cleanupOldRecordings();
void record_wav_streaming();
void process_recording();
String send_to_elevenlabs_stt(String filename);
String send_to_gemini(String prompt);
void generate_wav_header(uint8_t* wav_header, uint32_t wav_size, uint32_t sample_rate);
bool isEndOfSentence(char c);
void speakAnswer(String answer);

// --------------------------------------------------------------------------
// Gemini LLM Processing Function 
// --------------------------------------------------------------------------
String send_to_gemini(String prompt) {
    if (!wifi_connected || WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected, cannot send to Gemini");
        return "";
    }
    HTTPClient http;
    String full_url = gemini_url;
    if (String(gemini_api_key).length() > 0) {
        full_url += "?key=";
        full_url += gemini_api_key;
    }
    if (!http.begin(full_url)) {
        Serial.println("Failed to initialize HTTP connection for Gemini");
        return "";
    }
    http.setTimeout(30000);
    http.setConnectTimeout(10000);
    http.addHeader("Content-Type", "application/json");
    DynamicJsonDocument doc(1024);
    JsonArray systemInstructionParts = doc["systemInstruction"]["parts"].to<JsonArray>();
    systemInstructionParts.add();
    systemInstructionParts[0]["text"] = system_instruction;
    JsonArray contents = doc["contents"].to<JsonArray>();
    contents.add();
    JsonArray userParts = contents[0]["parts"].to<JsonArray>();
    userParts.add();
    userParts[0]["text"] = prompt;
    String requestBody;
    serializeJson(doc, requestBody);
    Serial.println("Sending prompt to Gemini API:");
    Serial.println(requestBody);
    int httpResponseCode = http.POST(requestBody);
    String gemini_answer = "";
    if (httpResponseCode == 200) {
        String response = http.getString();
        DynamicJsonDocument responseDoc(2048);
        if (deserializeJson(responseDoc, response) == DeserializationError::Ok) {
            if (responseDoc.containsKey("candidates")) {
                JsonArray candidates = responseDoc["candidates"].as<JsonArray>();
                if (candidates.size() > 0) {
                    JsonObject candidate = candidates[0].as<JsonObject>();
                    if (candidate.containsKey("content")) {
                        JsonObject content = candidate["content"].as<JsonObject>();
                        if (content.containsKey("parts")) {
                            JsonArray parts = content["parts"].as<JsonArray>();
                            if (parts.size() > 0) {
                                gemini_answer = parts[0]["text"].as<String>();
                            }
                        }
                    }
                }
            }
        }
    } else {
        Serial.printf("Gemini HTTP Error: %d\n", httpResponseCode);
        Serial.println("Response: " + http.getString());
    }
    http.end();
    return gemini_answer;
}

// --------------------------------------------------------------------------
// ElevenLabs STT and Processing (MODIFIED)
// --------------------------------------------------------------------------
void process_recording() {
    if (current_recording_file.isEmpty()) return;
    Serial.printf("Sending %s to ElevenLabs...\n", current_recording_file.c_str());
    String transcription = send_to_elevenlabs_stt(current_recording_file);
    if (transcription.length()) {
        Serial.println("---------------------------------------------------");
        Serial.println("Transcription:");
        Serial.println(transcription);
        last_transcription = transcription;
        Serial.println("Sending transcription to Gemini LLM for processing...");
        String gemini_answer = send_to_gemini(transcription);
        if (gemini_answer.length()) {
            Serial.println("---------------------------------------------------");
            Serial.println("Gemini Answer Received:");
            Serial.println(gemini_answer);
            Serial.println("---------------------------------------------------");
            speakAnswer(gemini_answer); 
        } else {
            Serial.println("Gemini processing failed. Speaking error message.");
            speakAnswer("Sorry, the language model failed to provide a response.");
        }
    } else {
        Serial.println("STT failed. Speaking error message.");
        speakAnswer("I couldn't understand what you said. Please try again.");
    }
    if (SD.exists(current_recording_file.c_str())) {
        SD.remove(current_recording_file.c_str());
        Serial.printf("Deleted temporary file: %s\n", current_recording_file.c_str());
    }
    current_recording_file = "";
}

// --------------------------------------------------------------------------
// TTS Functions (Modified for Efficient Mic/Speaker Switching)
// --------------------------------------------------------------------------
bool isEndOfSentence(char c) {
    return c == ' ' || c == '.' || c == '?' || c == '!' || c == ',';
}

void speakAnswer(String answer) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi disconnected, cannot perform TTS.");
        return;
    }
    const int chunkSize = 50;
    answer.trim();
    int len = answer.length();
    int start = 0;

    deinit_i2s_pdm(); // Mic off, enable speaker

    Serial.println("Starting TTS playback...");

    while (start < len) {
        int end = min(start + chunkSize, len);
        int chunkEnd = end;
        while (chunkEnd > start && !isEndOfSentence(answer.charAt(chunkEnd))) {
            chunkEnd--;
        }
        if (chunkEnd == start) chunkEnd = end;
        String chunk = answer.substring(start, chunkEnd);
        Serial.printf("Speaking chunk: '%s'\n", chunk.c_str());
        audio.connecttospeech(chunk.c_str(), "Kore");
        while(audio.isRunning()) {
            audio.loop();
        }
        start = chunkEnd;
        while (start < len && isEndOfSentence(answer.charAt(start))) start++; // skip punctuation
    }
    // Wait for any final playback activity to finish
    while(audio.isRunning()) {
        audio.loop();
    }
    // Safely re-enable mic only after speaker is truly done
    init_i2s_pdm();
    Serial.println("TTS playback finished. Microphone re-enabled.");
}

// --------------------------------------------------------------------------
// I2S PDM Mic, WiFi, and Utility Functions
// --------------------------------------------------------------------------
bool init_i2s_pdm() {
    Serial.println("Initializing I2S PDM...");
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    if (i2s_new_channel(&chan_cfg, NULL, &rx_handle) != ESP_OK) {
        Serial.println("Failed to create I2S channel");
        return false;
    }
    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = PDM_CLK_GPIO,
            .din = PDM_DIN_GPIO,
            .invert_flags = { .clk_inv = false },
        },
    };
    if (i2s_channel_init_pdm_rx_mode(rx_handle, &pdm_rx_cfg) != ESP_OK) return false;
    if (i2s_channel_enable(rx_handle) != ESP_OK) return false;
    Serial.println("I2S PDM initialized successfully");
    return true;
}

void deinit_i2s_pdm() {
    if (rx_handle != NULL) {
        i2s_channel_disable(rx_handle);
        i2s_del_channel(rx_handle);
        rx_handle = NULL;
    }
    Serial.println("I2S PDM de-initialized.");
}

bool connectToWiFi() {
    Serial.println("Connecting to WiFi...");
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi connected!");
        Serial.print("IP: ");
        Serial.println(WiFi.localIP());
        wifi_connected = true;
        return true;
    } else {
        Serial.println("\nWiFi connection failed");
        wifi_connected = false;
        return false;
    }
}

void record_wav_streaming() {
    if (rx_handle == NULL) return;
    const uint32_t max_record_time = 30;  // sec
    String filename = "/" + String(WAV_FILE_NAME) + "_" + String(millis()) + ".wav";
    current_recording_file = filename;
    File file = SD.open(filename.c_str(), FILE_WRITE);
    if (!file) {
        Serial.println("Failed to open file");
        current_recording_file = "";
        return;
    }
    uint8_t wav_header[WAV_HEADER_SIZE];
    generate_wav_header(wav_header, 0, SAMPLE_RATE);
    file.write(wav_header, WAV_HEADER_SIZE);
    uint8_t* buffer = (uint8_t*)malloc(512);
    if (!buffer) return;
    recording_active = true;
    size_t total_bytes = 0;
    unsigned long startTime = millis();
    Serial.println("Recording...");
    while (digitalRead(BUTTON_PIN) == LOW && (millis() - startTime < max_record_time * 1000)) {
        size_t bytes_read = 0;
        if (i2s_channel_read(rx_handle, buffer, 512, &bytes_read, pdMS_TO_TICKS(100)) != ESP_OK) continue;
        for (size_t i = 0; i < bytes_read; i += 2) {
            int16_t* sample = (int16_t*)&buffer[i];
            int32_t amp = (*sample) << VOLUME_GAIN;
            if (amp > 32767) amp = 32767;
            if (amp < -32768) amp = -32768;
            *sample = (int16_t)amp;
        }
        file.write(buffer, bytes_read);
        total_bytes += bytes_read;
    }
    recording_active = false;
    free(buffer);
    file.seek(0);
    generate_wav_header(wav_header, total_bytes, SAMPLE_RATE);
    file.write(wav_header, WAV_HEADER_SIZE);
    file.close();
    Serial.printf("Recording finished: %s (%d bytes)\n", filename.c_str(), total_bytes);
}

String send_to_elevenlabs_stt(String filename) {
    uint32_t t_start = millis();
    if (!wifi_connected || WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected, cannot send to STT");
        return "";
    }
    File file = SD.open(filename.c_str());
    if (!file) {
        Serial.println("Failed to open audio file");
        return "";
    }
    size_t file_size = file.size();
    if (file_size > 500000) {
        Serial.println("File too large for STT request (>500KB)");
        file.close();
        return "";
    }
    uint8_t* audio_data = (uint8_t*)malloc(file_size);
    if (!audio_data) {
        Serial.println("Failed to allocate memory for audio data!");
        file.close();
        return "";
    }
    size_t bytesRead = file.read(audio_data, file_size);
    file.close();
    HTTPClient http;
    if (!http.begin(elevenlabs_stt_url)) {
        Serial.println("Failed to initialize HTTP connection");
        free(audio_data);
        return "";
    }
    http.setTimeout(30000);
    http.setConnectTimeout(10000);
    http.addHeader("xi-api-key", elevenlabs_api_key);
    String boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
    http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    String body_start = "--" + boundary + "\r\n";
    body_start += "Content-Disposition: form-data; name=\"model_id\"\r\n\r\n";
    body_start += "scribe_v1\r\n";
    body_start += "--" + boundary + "\r\n";
    body_start += "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n";
    body_start += "Content-Type: audio/wav\r\n\r\n";
    String body_end = "\r\n--" + boundary + "--\r\n";
    size_t total_size = body_start.length() + file_size + body_end.length();
    uint8_t* complete_body = (uint8_t*)malloc(total_size);
    memcpy(complete_body, body_start.c_str(), body_start.length());
    memcpy(complete_body + body_start.length() + file_size, body_end.c_str(), body_end.length());
    if (bytesRead > 0) {
        memcpy(complete_body + body_start.length(), audio_data, bytesRead);
    }
    free(audio_data);
    int httpResponseCode = http.POST(complete_body, total_size);
    free(complete_body);
    String transcription = "";
    String response = http.getString();
    if (httpResponseCode == 200) {
        DynamicJsonDocument doc(2048);
        if (deserializeJson(doc, response) == DeserializationError::Ok) {
            if (doc.containsKey("text")) {
                transcription = doc["text"].as<String>();
            }
        }
    } else {
        Serial.printf("HTTP Error: %d\n", httpResponseCode);
        Serial.println("Response: " + response);
    }
    http.end();
    return transcription;
}

void generate_wav_header(uint8_t* wav_header, uint32_t wav_size, uint32_t sample_rate) {
    uint32_t file_size = wav_size + WAV_HEADER_SIZE - 8;
    uint32_t byte_rate = sample_rate * SAMPLE_BITS / 8;
    const uint8_t header[] = {
        'R', 'I', 'F', 'F',
        (uint8_t)(file_size),
        (uint8_t)(file_size >> 8),
        (uint8_t)(file_size >> 16),
        (uint8_t)(file_size >> 24),
        'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
        0x10, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x01, 0x00,
        (uint8_t)(sample_rate),
        (uint8_t)(sample_rate >> 8),
        (uint8_t)(sample_rate >> 16),
        (uint8_t)(sample_rate >> 24),
        (uint8_t)(byte_rate),
        (uint8_t)(byte_rate >> 8),
        (uint8_t)(byte_rate >> 16),
        (uint8_t)(byte_rate >> 24),
        0x02, 0x00, 0x10, 0x00,
        'd', 'a', 't', 'a',
        (uint8_t)(wav_size),
        (uint8_t)(wav_size >> 8),
        (uint8_t)(wav_size >> 16),
        (uint8_t)(wav_size >> 24),
    };
    memcpy(wav_header, header, sizeof(header));
}

void cleanupOldRecordings() {
    File root = SD.open("/");
    File file = root.openNextFile();
    while (file) {
        String filename = file.name();
        if (filename.startsWith(WAV_FILE_NAME) && filename.endsWith(".wav")) {
            file.close();
            SD.remove("/" + filename);
        } else {
            file.close();
        }
        file = root.openNextFile();
    }
    root.close();
}

// --------------------------------------------------------------------------
// Setup and Loop
// --------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    if (!SD.begin(21)) {
        Serial.println("Failed to mount SD Card!");
        while (1);
    }
    Serial.println("SD Card initialized");
    cleanupOldRecordings();
    connectToWiFi();
    if (!init_i2s_pdm()) {
        Serial.println("I2S PDM Mic init failed!");
        while (1);
    }
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio.setVolume(80); // Set a good default volume (0-100)
    Serial.println("Setup complete, starting main loop.");
}

void loop() {
    audio.loop();
    bool currentState = digitalRead(BUTTON_PIN) == LOW;
    if (currentState && !isPressed) {
        isPressed = true;
        Serial.println("Button pressed -> start recording");
        record_wav_streaming();
        if (WiFi.status() == WL_CONNECTED) {
            process_recording();
        } else {
            Serial.println("WiFi disconnected, attempting to reconnect...");
            if (connectToWiFi()) {
                process_recording();
            } else {
                Serial.println("Reconnection failed, skipping STT/LLM/TTS.");
            }
        }
    }
    if (!currentState && isPressed) {
        isPressed = false;
        Serial.println("Button released");
    }
    delay(50);
}
