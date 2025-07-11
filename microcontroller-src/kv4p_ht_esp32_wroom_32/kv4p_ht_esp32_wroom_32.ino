/*
KV4P-HT (see http://kv4p.com)
Copyright (C) 2024 Vance Vagell

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <Arduino.h>
#include <DRA818.h>
#include <esp_task_wdt.h>
#include "globals.h"
#include "debug.h"
#include "led.h"
#include "protocol.h"
#include "rxAudio.h"
#include "txAudio.h"
#include "buttons.h"
#include "utils.h"

#include "hardware-test.h"

// Set some default values.  Used to be set in globals.h, but that breaks #pragma once
hw_ver_t hardware_version = HW_VER_V1;  // lowest common denominator
Mode mode = MODE_STOPPED;
bool squelched = false;
uint8_t sqPin = SQ_PIN_HW1;

const uint16_t FIRMWARE_VER = 14;

const uint32_t RSSI_REPORT_INTERVAL_MS = 100;
const uint16_t USB_BUFFER_SIZE = 1024*2;

DRA818 sa818_vhf(&Serial2, SA818_VHF);
DRA818 sa818_uhf(&Serial2, SA818_UHF);
DRA818 &sa818 = sa818_vhf;

// Were we able to communicate with the radio module during setup()?
const char RADIO_MODULE_NOT_FOUND = 'x';
const char RADIO_MODULE_FOUND     = 'f';
char radioModuleStatus            = RADIO_MODULE_NOT_FOUND;

Debounce squelchDebounce(100);

void setMode(Mode newMode) {
  if (mode == newMode) {
    return;
  }
  mode = newMode;
  switch (mode) {
    case MODE_STOPPED:
      _LOGI("MODE_STOPPED");
      digitalWrite(PTT_PIN, HIGH);
      endI2STx();
      endI2SRx();
    break;
    case MODE_RX:
      _LOGI("MODE_RX");
      digitalWrite(PTT_PIN, HIGH);
      squelchDebounce.forceState(true);
      endI2STx();
      initI2SRx();
    break;
    case MODE_TX:
      _LOGI("MODE_TX");
      txStartTime = millis();
      digitalWrite(PTT_PIN, LOW);
      endI2SRx();
      initI2STx();
    break;
  }
}

hw_ver_t get_hardware_version() {
  pinMode(HW_VER_PIN_0, INPUT);
  pinMode(HW_VER_PIN_1, INPUT);
  hw_ver_t ver = 0x00;
  ver |= (digitalRead(HW_VER_PIN_0) == HIGH ? 0x0F : 0x00);
  ver |= (digitalRead(HW_VER_PIN_1) == HIGH ? 0xF0 : 0x00);
  // In the future, we're replace these with analogRead()s and
  // use values between 0x0 and 0xF. For now, just binary.
  return ver;
}

void setup() {
  // Used for setup, need to know early.
  hardware_version = get_hardware_version();

  // Communication with Android via USB cable
  Serial.setRxBufferSize(USB_BUFFER_SIZE);
  Serial.setTxBufferSize(USB_BUFFER_SIZE);
  Serial.begin(115200);
  Serial.println();
  Serial.println("===== kv4p serial output =====");
  Serial.println("This port will emit binary data using the kv4p protocol.");
  Serial.println("You will see watchdog resets — this is expected behavior.");
  Serial.println("Use `logcat` or a kv4p decoder to view readable logs.");
  Serial.println("More info: https://github.com/VanceVagell/kv4p-ht/blob/main/microcontroller-src/kv4p_ht_esp32_wroom_32/readme.md");
  Serial.println("To enter Hardware Test mode, Reset the board while pressing both PTT buttons.");
  Serial.println("==============================");
  // Hardware dependent pin assignments.
  switch (hardware_version) {
    case HW_VER_V1:
      COLOR_HW_VER = {0, 32, 0};
      sqPin = SQ_PIN_HW1;
      break;
    case HW_VER_V2_0C:
      COLOR_HW_VER = {32, 0, 0};
      sqPin = SQ_PIN_HW2;
      break;
    case HW_VER_V2_0D:
      COLOR_HW_VER = {0, 0, 32};
      sqPin = SQ_PIN_HW2;
      break;
    default:
      // Unknown version detected. Indicate this some way?
      COLOR_HW_VER = {16, 16, 16};
      break;
  }

  // Test mode is enabled by pressing both PTT buttons on RESET.
  buttonsSetup();
  if (digitalRead(PHYS_PTT_PIN1) == LOW && digitalRead(PHYS_PTT_PIN2) == LOW) {
    // Go into the hardware test code.  This bypasses the ENTIRE kv4p application.
    Serial.println("Both PTT are pressed, entering Hardware Test Mode. Reset (without pressing PTT) to enter kv4p HT mode.");
    testSetup();
    // Never returns.
  }

  // Configure watch dog timer (WDT), which will reset the system if it gets stuck somehow.
  esp_task_wdt_init(10, true);  // Reboot if locked up for a bit
  esp_task_wdt_add(NULL);       // Add the current task to WDT watch
  // Set up radio module defaults
  pinMode(PD_PIN, OUTPUT);
  digitalWrite(PD_PIN, HIGH);  // Power on
  pinMode(PIN_RADIO_HI_LOW, OUTPUT);
  digitalWrite(PIN_RADIO_HI_LOW, LOW); // LOW disables the MOSFET pull-down, which puts the radio in High Power mode.
  pinMode(sqPin, INPUT);
  pinMode(PTT_PIN, OUTPUT);
  digitalWrite(PTT_PIN, HIGH);  // Rx
  // Communication with DRA818V radio module via GPIO pins
  Serial2.begin(9600, SERIAL_8N1, RXD2_PIN, TXD2_PIN);
  Serial2.setTimeout(10);  // Very short so we don't tie up rx audio while reading from radio module (responses are tiny so this is ok)
  //
  debugSetup();
  // Begin in STOPPED mode
  squelched = (digitalRead(sqPin) == HIGH);
  setMode(MODE_STOPPED);
  ledSetup();
  sendHello();
  _LOGI("Setup is finished");
}

void doConfig(Config const &config) {
  if(config.radioType == SA818_UHF) {
    sa818 = sa818_uhf;
  } else {
    sa818 = sa818_vhf;
  }
  radioModuleStatus = RADIO_MODULE_NOT_FOUND;
  // The sa818.handshake() has 3 retries internally with 2 seconds between each attempt.
  // We have 3 retries on top of that, so total wait time is up to 20 seconds.
  // This should allow the radio module to power up and respond.
  for (int i = 0; i < 3; i++) {
    esp_task_wdt_reset();
    if (sa818.handshake()) { //Check if radio responded to handshake attempt
      radioModuleStatus = RADIO_MODULE_FOUND;
      sa818.volume(hardware_version == HW_VER_V2_0C ? 6 : 8); // v2.0c has a lower input ADC range.
      sa818.filters(false, false, false);
      break;
    }
  }
  sendVersion(FIRMWARE_VER, radioModuleStatus, hardware_version, USB_BUFFER_SIZE);
  esp_task_wdt_reset();
}

void handleCommands(RcvCommand command, uint8_t *params, size_t param_len) {
  switch (command) {
    case COMMAND_HOST_CONFIG:
      if (param_len == sizeof(Config)) {
        Config config;
        memcpy(&config, params, sizeof(Config));
        doConfig(config);
        esp_task_wdt_reset();
      }
      break;
    case COMMAND_HOST_FILTERS:
      if (param_len == sizeof(Filters)) {
        Filters filters;
        memcpy(&filters, params, sizeof(Filters));
        while (!sa818.filters((filters.flags & FILTER_PRE), (filters.flags & FILTER_HIGH), (filters.flags & FILTER_LOW)));
        esp_task_wdt_reset();
      }
      break;
    case COMMAND_HOST_GROUP:
      if (param_len == sizeof(Group)) {
        Group group;
        memcpy(&group, params, sizeof(Group));
        while (!sa818.group(group.bw, group.freq_tx, group.freq_rx, group.ctcss_tx, group.squelch, group.ctcss_rx));
        esp_task_wdt_reset();
        if (mode == MODE_STOPPED) {
          setMode(MODE_RX);   
        }
      } 
      break;
    case COMMAND_HOST_STOP:
      setMode(MODE_STOPPED);
      esp_task_wdt_reset();
      break;
    case COMMAND_HOST_PTT_DOWN:
      setMode(MODE_TX);
      esp_task_wdt_reset();
      break;
    case COMMAND_HOST_PTT_UP:
      setMode(MODE_RX);
      esp_task_wdt_reset();
      break;
    case COMMAND_HOST_TX_AUDIO:
      if (mode == MODE_TX) {
        processTxAudio(params, param_len);
        esp_task_wdt_reset();
      }
      break;                              
  }
}

void rssiLoop() {
  if (mode == MODE_RX) {
    EVERY_N_MILLISECONDS(RSSI_REPORT_INTERVAL_MS) {
      // TODO fix the dra818 library's implementation of rssi(). Right now it just drops the
      // return value from the module, and just tells us success/fail.
      // int rssi = dra->rssi();
      Serial2.println("RSSI?");
      String rssiResponse = Serial2.readString();
      if (rssiResponse.length() > 7) {
        String rssiStr = rssiResponse.substring(5);
        int rssiInt    = rssiStr.toInt();
        if (rssiInt >= 0 && rssiInt <= 255) {
          sendRssi((uint8_t)rssiInt);
        }
      }
    }
    END_EVERY_N_MILLISECONDS();
  }
}

void loop() {
  squelched = squelchDebounce.debounce((digitalRead(sqPin) == HIGH));
  debugLoop();
  ledLoop();
  buttonsLoop();
  protocolLoop();
  rxAudioLoop();
  txAudioLoop();
  rssiLoop();
}