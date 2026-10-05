#include <Arduino.h>
#include <Wire.h>
#include "stm32f3xx_hal.h"  // STM32-style HAL calls

#define SAMPLE_RATE 8000
#define RECORD_SECONDS 3
#define NUM_SAMPLES (SAMPLE_RATE) * (RECORD_SECONDS)
#define KEYPAD_ADDR 0x5F
#define SWITCH_PORT GPIOA
#define SWITCH_PIN_HAL GPIO_PIN_1
#define ENCODER_PORT GPIOB
#define ENCODER_CLK_HAL GPIO_PIN_4
#define ENCODER_DT_HAL GPIO_PIN_5
#define ENCODER_SW_HAL GPIO_PIN_0
#define SAMPLE_SW_PORT GPIOC // or D9
#define SAMPLE_SW_PIN GPIO_PIN_7
#define LINEIN_SW_PORT GPIOB // or D10
#define LINEIN_SW_PIN GPIO_PIN_6

ADC_HandleTypeDef hadc1;    // Handle for ADC setup
DAC_HandleTypeDef hdac1;    // Handle for DAC setup 
HardwareTimer timer(TIM6);  // Handle for timer setup 

volatile uint32_t counter;  // Sample number
volatile uint8_t done_flag; // Flag for recording done
volatile bool sample_flag = false;
volatile bool isRecording = false;
volatile bool isMuted = false;

uint16_t samples[NUM_SAMPLES];  // Global RAM buffer for all samples

/* Prototypes */
void sample_ISR();
void write_ISR();
void MX_ADC1_Init();
void MX_DAC1_Init();
void setup();
void switch_ISR();
void encoder_ISR();
void live_audio_ISR();

int pwmPin = PA8;   // or D7
int pwmPin_2 = PA10; // or D2

const uint16_t filterLookup[16][2] = {
  {98, 84}, {106, 91}, {118, 101}, {134, 114},
  {155, 134}, {186, 160}, {228, 197}, {288, 250},
  {372, 326}, {498, 436}, {675, 595}, {933, 820},
  {1312, 1208}, {1872, 1758}, {2706, 2580}, {3948, 3850}
};

volatile int tableIndex = 0;

const float cutoffFrequencies[16] = {
  100.0, 137.6, 189.3, 260.5, 358.5, 493.2, 678.7, 933.9,
  1285.0, 1768.1, 2432.9, 3347.6, 4606.2, 6338.1, 8721.1, 12000.0
};

volatile bool freqChanged = false;


void setup() {
/* DAC setup (from CubeMX) */
  MX_DAC1_Init(); 
  MX_ADC1_Init();

  Wire.setSDA(PB9);
  Wire.setSCL(PB8);
  Wire.begin();

  pinMode(pwmPin, OUTPUT);
  pinMode(pwmPin_2, OUTPUT);
  pinMode(PC7, OUTPUT);
  pinMode(PB6, OUTPUT);
  pinMode(PA_0, INPUT_ANALOG);
  pinMode(PA1, INPUT_PULLUP);
  pinMode(PB4, INPUT_PULLUP);
  pinMode(PB5, INPUT_PULLUP);
  pinMode(PB0, INPUT_PULLUP);

  HAL_GPIO_WritePin(SAMPLE_SW_PORT, SAMPLE_SW_PIN, GPIO_PIN_SET);
  HAL_GPIO_WritePin(LINEIN_SW_PORT, LINEIN_SW_PIN, GPIO_PIN_SET);

  analogWriteResolution(12);
  analogWriteFrequency(17578);

  attachInterrupt(digitalPinToInterrupt(PA1), switch_ISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(PB4), encoder_ISR, FALLING);
  analogWrite(PA8, filterLookup[0][0]);
  analogWrite(PA10, filterLookup[0][1]);
  

  Serial.begin(115200);
  while (!Serial);
  Serial.println("Press 1 for Audio Samples and 2 for Live Microphone");
}

byte last_key = 0;
int mode = 0;

void loop() {
  if (mode == 2 && sample_flag && isRecording && counter < NUM_SAMPLES) {
  sample_flag = false;

  HAL_ADC_Start(&hadc1);
  HAL_ADC_PollForConversion(&hadc1, 10);
  samples[counter++] = HAL_ADC_GetValue(&hadc1);
  HAL_ADC_Stop(&hadc1);

  if (counter >= NUM_SAMPLES) {
    done_flag = 1;
    isRecording = false;
    timer.pause();
    Serial.println("Recording Done");
    }
  }

  if (freqChanged) {
    Serial.print("Cutoff Frequency: ");
    Serial.print(cutoffFrequencies[tableIndex]);
    Serial.print(" Hz (PWM: ");
    Serial.print(filterLookup[tableIndex][0]);
    Serial.print(", ");
    Serial.print(filterLookup[tableIndex][1]);
    Serial.println(")");
    
    freqChanged = false; // Reset the flag
  }
  Wire.requestFrom(KEYPAD_ADDR, 1);
  if (Wire.available()) {
    byte key_data = Wire.read();
    

    if (key_data != 0 && key_data != last_key) {
      last_key = key_data;
    

      Serial.print("Key pressed: ");
      Serial.println((char)key_data);
      
      // Main Menu
      if (mode == 0) {
        if (key_data == '1'){
          mode = 1;
          Serial.println("AUDIO SAMPLES MODE");
        }
        else if (key_data == '2'){
          mode = 2;
          Serial.println("RECORD / PLAYBACK MODE");
        }
        else if (key_data == '3'){
          mode = 3;
          HAL_GPIO_WritePin(SAMPLE_SW_PORT, SAMPLE_SW_PIN, GPIO_PIN_SET);
          HAL_GPIO_WritePin(LINEIN_SW_PORT, LINEIN_SW_PIN, GPIO_PIN_RESET);
          Serial.println("LIVE AUDIO MODE 44.1kHz");
        }
      }

      // AUDIO MODE
      else if (mode == 1) {
        Serial.println("AUDIO SAMPLES MODE");
        Serial.println("Press 1 for Cricket, Press 2 for Line In");
        if (key_data == '1'){
          Serial.println("Cricket Mode");
          HAL_GPIO_WritePin(LINEIN_SW_PORT, LINEIN_SW_PIN, GPIO_PIN_RESET);
          HAL_GPIO_WritePin(SAMPLE_SW_PORT, SAMPLE_SW_PIN, GPIO_PIN_SET);
        }
        else if(key_data == '2'){
          Serial.println("Line in");
          HAL_GPIO_WritePin(SAMPLE_SW_PORT, SAMPLE_SW_PIN, GPIO_PIN_RESET);
          HAL_GPIO_WritePin(LINEIN_SW_PORT, LINEIN_SW_PIN, GPIO_PIN_SET);
        }
        else if (key_data == '3'){
          Serial.println("Set PWM frequency to 12000Hz");
          analogWrite(pwmPin, 4095);
          analogWrite(pwmPin_2, 4095);
        }
        else if (key_data == 'k') {
          HAL_GPIO_WritePin(SAMPLE_SW_PORT, SAMPLE_SW_PIN, GPIO_PIN_RESET);
          HAL_GPIO_WritePin(LINEIN_SW_PORT, LINEIN_SW_PIN, GPIO_PIN_RESET);
          mode = 0;
          Serial.println("Back to menu");
        }
      }

      else if(mode == 2) {
        Serial.println("Press 1 for Recording, Press 2 for Playback");
        if (key_data == '1' && !isRecording) {
          Serial.println("Recording...");
          /* Initialization */
          counter = 0;
          done_flag = 0;
          isRecording = true;

          /* Timer setup */
          timer.setOverflow(SAMPLE_RATE, HERTZ_FORMAT);
          timer.detachInterrupt();
          timer.attachInterrupt(sample_ISR);
          timer.refresh();
          /* Start timer */
          timer.resume();
          
        }
        else if (key_data == '2' && done_flag == 1) {
          Serial.println("Normalizing and Playing...");

          // Find the Min and Max in  recorded data
          uint16_t min_val = 4095;
          uint16_t max_val = 0;
          for (int i = 0; i < NUM_SAMPLES; i++) {
              if (samples[i] < min_val) min_val = samples[i];
              if (samples[i] > max_val) max_val = samples[i];
          }

          // Stretch the values to fill 0-4095
          float range = max_val - min_val;
          if (range > 0) { // Prevent division by zero
              for (int i = 0; i < NUM_SAMPLES; i++) {
                  // Formula: (Value - Min) * (TargetMax / CurrentRange)
                  samples[i] = (uint16_t)((samples[i] - min_val) * (4095.0 / range));
              }
          }
          analogWrite(pwmPin, 4095);
          analogWrite(pwmPin_2, 4095);
          HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R, 2048);
          delay(500);

          noInterrupts();

          counter = 0;                            // Reset counter
          timer.detachInterrupt();                // Detach sampling ISR
          timer.attachInterrupt(write_ISR);       // Attach playback ISR
          timer.setOverflow(5000, HERTZ_FORMAT);  // Set slower overflow frequency -- still not sure why playing back must be slower
          timer.refresh();
          /* End critical section -- enable interrupts */
          interrupts();

          /* Resume timer, start playing back */
          timer.resume();
            
          /* Wait for recording to finish*/
          delay(2000);
        }
        else if (key_data == 'k') {
          HAL_GPIO_WritePin(SAMPLE_SW_PORT, SAMPLE_SW_PIN, GPIO_PIN_RESET);
          HAL_GPIO_WritePin(LINEIN_SW_PORT, LINEIN_SW_PIN, GPIO_PIN_RESET);
          mode = 0;
          Serial.println("Back to menu");
        }  
      }
      // LIVE MICROPHONE MODE
      else if (mode == 3) {
          Serial.println("Press 1 to start live audio, k to stop");
          if (key_data == '1') {
            Serial.println("Live audio started");
            timer.pause();
            timer.detachInterrupt();
            timer.attachInterrupt(live_audio_ISR);
            timer.setOverflow(44100, HERTZ_FORMAT);
            timer.refresh();
            timer.resume();
          }

          else if (key_data == 'k') {
            timer.pause();
            HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R, 2048);
            HAL_GPIO_WritePin(SAMPLE_SW_PORT, SAMPLE_SW_PIN, GPIO_PIN_RESET);
            HAL_GPIO_WritePin(LINEIN_SW_PORT, LINEIN_SW_PIN, GPIO_PIN_RESET);
            mode = 0;
            Serial.println("Back to menu");
          }
        }
    }

    if (key_data == 0) {
      last_key = 0;
    }
  }

}

void sample_ISR() {
  sample_flag = true;
}

void switch_ISR() {
  isMuted = !isMuted;

  if (isMuted) {
    timer.pause();
    
    HAL_GPIO_WritePin(SAMPLE_SW_PORT, SAMPLE_SW_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(LINEIN_SW_PORT, LINEIN_SW_PIN, GPIO_PIN_SET);
    
    HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R, 2048);
  } else {
    timer.resume();
  }
}

void encoder_ISR() {
  static unsigned long lastTime = 0;
  unsigned long now = millis();
  if (now - lastTime > 50) {
    if (HAL_GPIO_ReadPin(ENCODER_PORT, ENCODER_DT_HAL) != HAL_GPIO_ReadPin(ENCODER_PORT, ENCODER_CLK_HAL)) tableIndex++;
    else tableIndex--;
    tableIndex = constrain(tableIndex, 0, 15);
    analogWrite(PA8, filterLookup[tableIndex][0]); 
    analogWrite(PA10, filterLookup[tableIndex][1]);
    freqChanged = true; 
  }
  lastTime = now;
}

void write_ISR(){
if (counter < NUM_SAMPLES) {
    // 1. Get the raw sample (centered around 2048)
    int32_t sample = samples[counter++];

    // 2. Subtract bias to center it at 0
    sample = sample - 2048;

    // 3. Apply Gain (e.g., 4x volume). Adjust this number!
    sample = sample * 4; 

    // 4. Clip the signal so it doesn't "wrap around" (0 to 4095)
    if (sample > 2047) sample = 2047;
    if (sample < -2048) sample = -2048;

    // 5. Add bias back and send to DAC
    HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R, (uint16_t)(sample + 2048));
  } else {
    timer.pause();
  }
}

void live_audio_ISR() {
  HAL_ADC_Start(&hadc1);

  if (HAL_ADC_PollForConversion(&hadc1, 1) == HAL_OK) {
    uint16_t adcValue = HAL_ADC_GetValue(&hadc1);

    int32_t sample = adcValue - 2048;

    // Mic software gain
    sample = sample * 15;

    if (sample > 2047) sample = 2047;
    if (sample < -2048) sample = -2048;

    HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R, sample + 2048);
  }

  HAL_ADC_Stop(&hadc1);
}

void MX_ADC1_Init() {
  ADC_ChannelConfTypeDef sConfig = {0};

  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1;

  HAL_ADC_Init(&hadc1);

  sConfig.Channel = ADC_CHANNEL_1;   // PA0
  sConfig.Rank = ADC_REGULAR_RANK_1;

  // ⭐ VERY IMPORTANT FOR MIC
  sConfig.SamplingTime = ADC_SAMPLETIME_19CYCLES_5;

  HAL_ADC_ConfigChannel(&hadc1, &sConfig);
}

/* CubeMX generated setup code for the DAC -- see HAL/Low Level Drivers datasheet */
void MX_DAC1_Init()
{

  /* USER CODE BEGIN DAC1_Init 0 */
  /* USER CODE END DAC1_Init 0 */

  DAC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN DAC1_Init 1 */

  /* USER CODE END DAC1_Init 1 */

  /** DAC Initialization
  */
  hdac1.Instance = DAC1;
  HAL_DAC_Init(&hdac1);
  

  /** DAC channel OUT1 config
  */
  sConfig.DAC_Trigger = DAC_TRIGGER_NONE;
  sConfig.DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE;
  HAL_DAC_ConfigChannel(&hdac1, &sConfig, DAC_CHANNEL_1);
  /* USER CODE BEGIN DAC1_Init 2 */

  HAL_DAC_Start(&hdac1, DAC_CHANNEL_1); // <--- THIS ONE
  /* USER CODE END DAC1_Init 2 */

}