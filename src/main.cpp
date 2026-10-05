/******************************************************************************
* Project: JDP26 DIY DJ Deck
* File:  main.cpp, cricket.h, air_horn.h, bruh.h
*
* Student Name: Liam Earle
* Team Members: Phu Nguyen
*
* Course: ECE 304 – Junior Design Project (JDP26)
* Instructor: Prof. Baird Soules
*
* Date: 05/11/2026
*
* Description:
* Briefly describe what this program does.
* This program allows for the user to control the LPF and STM32 system using peripherals like the i2C keyboard, rotary encoder, and knife switch. These will correspond to 3 main features being Live microphone, sample playback, and line-in audio. All functions are handled through interrupts and through interrupt service routines.
*
* Hardware:
* - Microcontroller: STM32F303RE
* - Key components: LPF (low-pass filter), Rotary Encoder, Analog Switch (SN74HC4066N), Master Knife Switch, TLV2372IP op-amp, CardKB I2C Keyboard, Adafruit Amplifier, 8 Ohm Speaker.
*
* Notes:
* Any assumptions, limitations, or special instructions.
******************************************************************************/


#include <Arduino.h>
#include <Wire.h>
#include "stm32f3xx_hal.h"
// Header for Samples
#include "cricket.h"
#include "air_horn.h"
#include "bruh.h"
// SAMPLE RATE SETUP	
#define SAMPLE_RATE_HI 44100
#define SAMPLE_RATE 8000
#define RECORD_SECONDS 3
#define NUM_SAMPLES (SAMPLE_RATE) * (RECORD_SECONDS)
// Define KEYPAD address for I2C
#define KEYPAD_ADDR 0x5F
// GPIO Port and Pin Mappings for HAL
#define SWITCH_PORT      GPIOA
#define SWITCH_PIN_HAL   GPIO_PIN_1
#define SAMPLE_SW_PORT   GPIOC
#define SAMPLE_SW_PIN    GPIO_PIN_7
#define LINEIN_SW_PORT   GPIOB
#define LINEIN_SW_PIN    GPIO_PIN_6
#define ENCODER_PORT     GPIOB
#define ENCODER_CLK_HAL  GPIO_PIN_4
#define ENCODER_DT_HAL   GPIO_PIN_5

// ADC and DAC handler
ADC_HandleTypeDef hadc1;
DAC_HandleTypeDef hdac1;
// Hardware timer object using TIM6
HardwareTimer timer(TIM6);


volatile uint32_t play_index = 0;
volatile uint32_t counter;
volatile uint8_t done_flag;
volatile bool sample_flag = false;
volatile bool isRecording = false;
volatile bool isMuted = false; // New flag to track mute state
// Saved switch GPIO states for resuming after knife switch interrupt.
volatile GPIO_PinState saved_sample_state = GPIO_PIN_SET;
volatile GPIO_PinState saved_linein_state = GPIO_PIN_SET;
//Variables for samples
volatile int current_sample = 0;
uint16_t samples[NUM_SAMPLES];

// Initial function declarations.
void sample_ISR();
void write_ISR();
void MX_ADC1_Init();
void MX_DAC1_Init();
void setup();
void switch_ISR();
void encoder_ISR();
void live_audio_ISR();
void flash_playback_ISR();

// Array for PWM values for Rvar1 and Rvar2 to set cutoff frequency.
const uint16_t filterLookup[16][2] = {
  {98, 84}, {106, 91}, {118, 101}, {134, 114},
  {155, 134}, {186, 160}, {228, 197}, {288, 250},
  {372, 326}, {498, 436}, {675, 595}, {933, 820},
  {1312, 1208}, {1872, 1758}, {2706, 2580}, {3948, 3850}
};


volatile int tableIndex = 0;
//Array of cutoff frequency values. 
const float cutoffFrequencies[16] = {
  100.0, 137.6, 189.3, 260.5, 358.5, 493.2, 678.7, 933.9,
  1285.0, 1768.1, 2432.9, 3347.6, 4606.2, 6338.1, 8721.1, 12000.0
};


// Flag indicating cutoff frequency switch changed
volatile bool freqChanged = false;
 
void setup() {
  MX_DAC1_Init();
  MX_ADC1_Init();

  // Configure pins for I2C Keyboard
  Wire.setSDA(PB9);
  Wire.setSCL(PB8);
  Wire.begin();

  // GPIO pin setup
  pinMode(PA8, OUTPUT);
  pinMode(PA10, OUTPUT);
  pinMode(PC7, OUTPUT);
  pinMode(PB6, OUTPUT);
  pinMode(PA_0, INPUT_ANALOG);
  pinMode(PA1, INPUT);
  pinMode(PB4, INPUT_PULLUP);
  pinMode(PB5, INPUT_PULLUP);

  // Initialize all audio paths to OFF
  HAL_GPIO_WritePin(SAMPLE_SW_PORT, SAMPLE_SW_PIN, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LINEIN_SW_PORT, LINEIN_SW_PIN, GPIO_PIN_RESET);

  // Configure PWM resolution and frequency
  analogWriteResolution(12);
  analogWriteFrequency(17578);

  // Trigger on CHANGE to handle both press and release
  attachInterrupt(digitalPinToInterrupt(PA1), switch_ISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PB4), encoder_ISR, FALLING);
 
  //Set initial cutoff to 100Hz
  analogWrite(PA8, filterLookup[0][0]);
  analogWrite(PA10, filterLookup[0][1]);
  
  // Set up serial connection
  Serial.begin(115200);
  while (!Serial);
  Serial.println("System Ready");
  Serial.println("Press 1 for AUDIO SAMPLES, 2 for RECORD/PLAYBACK, or 3 for LIVE AUDIO on keypad");
}

// Store last keypad input and menu/submenu navigation
byte last_key = 0;
int mode = 0;
int submenu = 0;


void loop() {
  // Exit if audio muted
  if (isMuted) return;
  
  // Recording mode check if is recording
  if (mode == 2 && sample_flag && isRecording && counter < NUM_SAMPLES) {
    sample_flag = false;
    HAL_ADC_Start(&hadc1);
    if (HAL_ADC_PollForConversion(&hadc1, 10) == HAL_OK) {
        samples[counter++] = HAL_ADC_GetValue(&hadc1);
    }
    HAL_ADC_Stop(&hadc1);

    // Check to finish the record
    if (counter >= NUM_SAMPLES) {
      done_flag = 1;
      isRecording = false;
      timer.pause();
      Serial.println("Recording Done");
    }
  }

  // Check to print out the cutoff frequency and set it to false for next change
  if (freqChanged) {
    Serial.print("Cutoff: ");
    Serial.println(cutoffFrequencies[tableIndex]);
    freqChanged = false;
  }

  // Request keypad input data over I2C
  Wire.requestFrom(KEYPAD_ADDR, 1);
  // Check if keypad data is available
  if (Wire.available()) {
    // Read key value from keypad
    byte key_data = Wire.read();

    if (key_data != 0 && key_data != last_key) {
      // Set the key value as the last pressed key
      last_key = key_data;
      // Print key pressed to Serial Monitor
      Serial.print("Key pressed: ");
      Serial.println((char)key_data);

      // Mode 0 - Main Menu
      if (mode == 0) {
        // Mode 1 - Audio Sample Mode
        if (key_data == '1') {
          mode = 1;
          submenu = 0;
		      //set line in and sample channel to off
          HAL_GPIO_WritePin(SAMPLE_SW_PORT, SAMPLE_SW_PIN, GPIO_PIN_RESET);
          HAL_GPIO_WritePin(LINEIN_SW_PORT, LINEIN_SW_PIN, GPIO_PIN_RESET);
          Serial.println("MODE 1: SAMPLES");
          Serial.println("Press 1 for Samples, Press 2 for Line In");
        }
        // Mode 2 - Recording
        else if (key_data == '2') { mode = 2; Serial.println("MODE 2: RECORD"); }
        // Mode 3 - Live Audio Mode
        else if (key_data == '3') {
          mode = 3;
          // set line in off and sample channel on
          HAL_GPIO_WritePin(SAMPLE_SW_PORT, SAMPLE_SW_PIN, GPIO_PIN_SET);
          HAL_GPIO_WritePin(LINEIN_SW_PORT, LINEIN_SW_PIN, GPIO_PIN_RESET);
          Serial.println("MODE 3: LIVE");
        }
      }
      // Mode 1 - Audio Sample Mode
      else if (mode == 1) {
        // If not in submenu
        if (submenu == 0) {
          // Press 1 for Audio Sample Sub Menu
          if(key_data == '1'){
            submenu = 1;
            Serial.println("Press 1 for Cricket, 2 for Airhorn, 3 for Bruh");
          }
          // Press 2 for Line In
          else if(key_data == '2') { 
            Serial.println("Line In");
            //set line in on and sample channel off
            HAL_GPIO_WritePin(SAMPLE_SW_PORT, SAMPLE_SW_PIN, GPIO_PIN_RESET);
            HAL_GPIO_WritePin(LINEIN_SW_PORT, LINEIN_SW_PIN, GPIO_PIN_SET);
          }
          // Press k to exit back to main menu
          else if (key_data == 'k') {
		        //set line in off and sample channel off
            HAL_GPIO_WritePin(SAMPLE_SW_PORT, SAMPLE_SW_PIN, GPIO_PIN_RESET);
            HAL_GPIO_WritePin(LINEIN_SW_PORT, LINEIN_SW_PIN, GPIO_PIN_RESET);
            mode = 0;
            Serial.println("Back to main menu");
            Serial.println("Press 1 for AUDIO SAMPLES, 2 for RECORD/PLAYBACK, or 3 for LIVE AUDIO on keypad");
          }
        }
        // Audio Sample Sub Menu
        else if (submenu == 1) {
          if (key_data == '1') { // Cricket
            Serial.println("Number 1 - Cricket");
            current_sample = 0;
		        //Turn on then off the Line In
            HAL_GPIO_WritePin(LINEIN_SW_PORT, LINEIN_SW_PIN, GPIO_PIN_SET);  
            HAL_GPIO_WritePin(LINEIN_SW_PORT, LINEIN_SW_PIN, GPIO_PIN_RESET);
            delay(50);
            // Turn on the sample
            HAL_GPIO_WritePin(SAMPLE_SW_PORT, SAMPLE_SW_PIN, GPIO_PIN_SET);
            play_index = 0;
            timer.pause();
            timer.detachInterrupt();
            timer.attachInterrupt(flash_playback_ISR);
            timer.setOverflow(SAMPLE_RATE_HI, HERTZ_FORMAT);
            timer.refresh();
            timer.resume();
          }
          else if (key_data == '2') { // Airhorn
            Serial.println("Number 2 - Airhorn");
            current_sample = 1;
            //Turn on then off the Line In
            HAL_GPIO_WritePin(LINEIN_SW_PORT, LINEIN_SW_PIN, GPIO_PIN_SET);  
            HAL_GPIO_WritePin(LINEIN_SW_PORT, LINEIN_SW_PIN, GPIO_PIN_RESET);
            delay(50);
            // Turn on the sample
            HAL_GPIO_WritePin(SAMPLE_SW_PORT, SAMPLE_SW_PIN, GPIO_PIN_SET);
            play_index = 0;
            timer.pause();
            timer.detachInterrupt();
            timer.attachInterrupt(flash_playback_ISR);
            timer.setOverflow(SAMPLE_RATE_HI, HERTZ_FORMAT);
            timer.refresh();
            timer.resume();
          }
          else if (key_data == '3') { // Bruh
            Serial.println("Number 3 - Bruh");
            current_sample = 2;
            //Turn on then off the Line In
            HAL_GPIO_WritePin(LINEIN_SW_PORT, LINEIN_SW_PIN, GPIO_PIN_SET);  
            HAL_GPIO_WritePin(LINEIN_SW_PORT, LINEIN_SW_PIN, GPIO_PIN_RESET);
            delay(50);
            // Turn on the sample
            HAL_GPIO_WritePin(SAMPLE_SW_PORT, SAMPLE_SW_PIN, GPIO_PIN_SET);
            play_index = 0;
            timer.pause();
            timer.detachInterrupt();
            timer.attachInterrupt(flash_playback_ISR);
            timer.setOverflow(SAMPLE_RATE_HI, HERTZ_FORMAT);
            timer.refresh();
            timer.resume();
          }
          else if (key_data == 'k') {
            timer.pause();
		        //set line in off and sample channel off
            HAL_GPIO_WritePin(SAMPLE_SW_PORT, SAMPLE_SW_PIN, GPIO_PIN_RESET);
            HAL_GPIO_WritePin(LINEIN_SW_PORT, LINEIN_SW_PIN, GPIO_PIN_RESET);
            // Return to Mode 1 Menu
            submenu = 0;
            Serial.println("Press 1 for Samples, Press 2 for Line In");
          }
        }
      }
      // Mode 2 - RECORDING/PLAYBACK
      else if(mode == 2) {
        Serial.println("Press 1 for Recording, Press 2 for Playback");
        // Recording
        if (key_data == '1' && !isRecording) {
          Serial.println("Recording...");
          counter = 0; done_flag = 0; isRecording = true;
          timer.setOverflow(SAMPLE_RATE, HERTZ_FORMAT);
          timer.detachInterrupt();
          timer.attachInterrupt(sample_ISR);
          timer.refresh(); timer.resume();
        }
        // Playback
        else if (key_data == '2' && done_flag == 1) {
          Serial.println("Normalizing and Playing...");
         
          counter = 0;
          noInterrupts();
          timer.detachInterrupt();
          timer.attachInterrupt(write_ISR);
          timer.setOverflow(5000, HERTZ_FORMAT);
          timer.refresh();
          interrupts();
          timer.resume();
        }
        // Press K to Main Menu
        else if (key_data == 'k') {
          HAL_GPIO_WritePin(SAMPLE_SW_PORT, SAMPLE_SW_PIN, GPIO_PIN_RESET);
          HAL_GPIO_WritePin(LINEIN_SW_PORT, LINEIN_SW_PIN, GPIO_PIN_RESET);
          mode = 0;
          // Press K to go back to the main menu
          Serial.println("Back to main menu");
          Serial.println("Press 1 for AUDIO SAMPLES, 2 for RECORD/PLAYBACK, or 3 for LIVE AUDIO on keypad");


        }  
      }
      // Mode 3 - LIVE AUDIO MODE
      else if (mode == 3) {
          Serial.println("Press 1 to start live audio, k to stop");
          // Press 1 to start live audio
          if (key_data == '1') {
            Serial.println("Live audio started");
            timer.pause();
            timer.detachInterrupt();
            timer.attachInterrupt(live_audio_ISR);
            timer.setOverflow(44100, HERTZ_FORMAT);
            timer.refresh(); timer.resume();
          }
          // Press k to stop live audio and go back to main menu
          else if (key_data == 'k') {
            timer.pause();
            HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R, 2048);
            //set line in off and sample channel off
            HAL_GPIO_WritePin(SAMPLE_SW_PORT, SAMPLE_SW_PIN, GPIO_PIN_RESET);
            HAL_GPIO_WritePin(LINEIN_SW_PORT, LINEIN_SW_PIN, GPIO_PIN_RESET);
            mode = 0;
            Serial.println("Back to main menu");
            Serial.println("Press 1 for AUDIO SAMPLES, 2 for RECORD/PLAYBACK, or 3 for LIVE AUDIO on keypad");
          }
        }
    }
    // Reset last key when no key is pressed
    if (key_data == 0) last_key = 0;
  }
}

// Timer interrupt sets sample flag for audio sampling
void sample_ISR() { sample_flag = true; }

// Interrupt Service Routine to mute and unmute with Master Knife Switch
void switch_ISR() {
  if (HAL_GPIO_ReadPin(SWITCH_PORT, SWITCH_PIN_HAL) == GPIO_PIN_SET) {
    if (!isMuted) {  // only save if not already muted
        //Save analog switch state
      saved_sample_state = HAL_GPIO_ReadPin(SAMPLE_SW_PORT, SAMPLE_SW_PIN);
      saved_linein_state = HAL_GPIO_ReadPin(LINEIN_SW_PORT, LINEIN_SW_PIN);
    }
    isMuted = true;
    timer.pause();
    HAL_GPIO_WritePin(SAMPLE_SW_PORT, SAMPLE_SW_PIN, GPIO_PIN_RESET);  // was SET
    HAL_GPIO_WritePin(LINEIN_SW_PORT, LINEIN_SW_PIN, GPIO_PIN_RESET);  // was SET
    HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R, 2048);
    Serial.println("Muted");
  } else {
    //Restore saved state when switch opens
    HAL_GPIO_WritePin(SAMPLE_SW_PORT, SAMPLE_SW_PIN, saved_sample_state);
    HAL_GPIO_WritePin(LINEIN_SW_PORT, LINEIN_SW_PIN, saved_linein_state);
    isMuted = false;
    timer.resume();
    Serial.println("Unmuted");
  }
}
// Interrupt Service Routine to play stored samples
void flash_playback_ISR() {
  const uint8_t* arr;
  uint32_t len;

  if (current_sample == 0) {
    arr = cricket;  // cricket
    len = CRICKET_SAMPLES;
  } else if (current_sample == 1) {
    arr = air_horn;
    len = AIR_SAMPLES;
  } else {
    arr = bruh;       // bruh
    len = BRUH_SAMPLES;
  }
  //Loop and play samples from array on the STM32
  if (play_index < len) {
    uint16_t dac_val = (uint16_t)arr[play_index] << 4;
    HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R, dac_val);
    play_index++;
  } else {
    timer.pause();
    play_index = 0;
    HAL_GPIO_WritePin(SAMPLE_SW_PORT, SAMPLE_SW_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(LINEIN_SW_PORT, LINEIN_SW_PIN, GPIO_PIN_SET);
  }
}

// Interrupt Service Routine for Rotary Encoder (adjust cut off frequency)
void encoder_ISR() {
  static unsigned long lastTime = 0;
  unsigned long now = millis();
  //Added delay to help reduce double triggers
  if (now - lastTime > 200) {
    //Check direction of turn.
    if (HAL_GPIO_ReadPin(ENCODER_PORT, ENCODER_DT_HAL) != HAL_GPIO_ReadPin(ENCODER_PORT, ENCODER_CLK_HAL)) tableIndex++;
    else tableIndex--;
    tableIndex = constrain(tableIndex, 0, 15);
    analogWrite(PA8, filterLookup[tableIndex][0]);
    analogWrite(PA10, filterLookup[tableIndex][1]);
    freqChanged = true;
  }
  lastTime = now;
}


void write_ISR() {
  // Old sampling logic for mode 2. Not a special feature.
  if (counter < NUM_SAMPLES) {
    int32_t s = samples[counter++] - 2048;
    s *= 4;
    if (s > 2047) s = 2047;
    if (s < -2048) s = -2048;
    HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R, (uint16_t)(s + 2048));
  } else {
    timer.pause();
  }
}

// Interrupt Service Routine for Live Audio Mode 3
void live_audio_ISR() {
  HAL_ADC_Start(&hadc1);
  // Normalize and play audio sample by sample 
  if (HAL_ADC_PollForConversion(&hadc1, 1) == HAL_OK) {
    int32_t s = (int32_t)HAL_ADC_GetValue(&hadc1) - 2048;
    s *= 15; // Added digital gain
    if (s > 2047) s = 2047;
    if (s < -2048) s = -2048;
    HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R, (uint16_t)(s + 2048));
  }
  HAL_ADC_Stop(&hadc1);
}


void MX_ADC1_Init() {
  ADC_ChannelConfTypeDef sConfig = {0};
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1;
  HAL_ADC_Init(&hadc1);
  sConfig.Channel = ADC_CHANNEL_1;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_19CYCLES_5;
  HAL_ADC_ConfigChannel(&hadc1, &sConfig);
}


void MX_DAC1_Init() {
  DAC_ChannelConfTypeDef sConfig = {0};
  hdac1.Instance = DAC1;
  HAL_DAC_Init(&hdac1);
  sConfig.DAC_Trigger = DAC_TRIGGER_NONE;
  sConfig.DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE;
  HAL_DAC_ConfigChannel(&hdac1, &sConfig, DAC_CHANNEL_1);
  HAL_DAC_Start(&hdac1, DAC_CHANNEL_1);
  //We tried a separate amplifier for the live mic to reduce noise, but           we could not get it loud enough.
  //sConfig.DAC_Trigger = DAC_TRIGGER_NONE;
  //sConfig.DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE;
  //HAL_DAC_ConfigChannel(&hdac1, &sConfig, DAC_CHANNEL_2);
  //HAL_DAC_Start(&hdac1, DAC_CHANNEL_2);
}
