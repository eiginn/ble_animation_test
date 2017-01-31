#define BLYNK_PRINT Serial // Enables Serial Monitor
#define BLYNK_USE_DIRECT_CONNECT

#include <Arduino.h>
#include "SAMD_AnalogCorrection.h"

#include <BlynkSimpleSerialBLE.h>
#include "Adafruit_BLE.h"
#include "Adafruit_BluefruitLE_SPI.h"
#include <SPI.h>
#include <SimpleTimer.h>

#include "FastLED.h"
#include <Battery.h>

#define NUM_LEDS 34
#define BRIGHTNESS  30
#define FRAMES_PER_SECOND 100
#define FORWARD 0
#define BACKWARD 1
#define SLOW 250
#define MEDIUM 50
#define FAST 5


//pins for feather BLE, see adafruit web for details
#define BLUEFRUIT_SPI_CS               8
#define BLUEFRUIT_SPI_IRQ              7
#define BLUEFRUIT_SPI_RST              4

#define VBATPIN A7
#define CONF_RESET_PIN 12
#define LEDPIN 13

#ifdef BLYNK_AUTH_KEY
#include "blynk_key.h"
#else
const char auth[] = "..."; //Had to reverse engineer this.
#endif
int mode = 3;
int modechanged = 0;
SimpleTimer timer;

// Fire setup
// Array of temperature readings at each simulation cell
byte heat[NUM_LEDS];

// rainbow setup
uint8_t thishue = 0;
uint8_t deltahue = 10;

Adafruit_BluefruitLE_SPI ble(BLUEFRUIT_SPI_CS, BLUEFRUIT_SPI_IRQ, BLUEFRUIT_SPI_RST);
CRGB leds[NUM_LEDS];
Battery battery(3200, 4200, VBATPIN);

void setup() {
  delay(5000); // sanity delay
  analogReadCorrection(21, 2059);
  pinMode(CONF_RESET_PIN, INPUT_PULLUP);
  pinMode(LEDPIN, OUTPUT);
  digitalWrite(LEDPIN, LOW);

  Serial.begin(9600); // See the connection status in Serial Monitor

  //FastLED.addLeds<WS2812B, 6, GRB>(leds, NUM_LEDS);
  FastLED.addLeds<WS2811, 6, RGB>(leds, NUM_LEDS);
  FastLED.setBrightness( BRIGHTNESS );
  battery.begin(3.28, 2);
  flashStdby();

  ble.begin(true); //true => debug on, you can see bluetooth in the serial monitor.

#ifdef RESET_SWITCH
  if ( digitalRead(CONF_RESET_PIN) == LOW ) {
    Serial.println("CONF RESET");
    ble.factoryReset(); //Optional
    flashRst();
  }
#endif

  ble.setMode(BLUEFRUIT_MODE_DATA);

  Blynk.begin(auth, ble);

  timer.setInterval(30000L, runEveryHalfMinute);
  timer.setInterval(5000L, runEveryFiveSeconds);

  // set some default values in UI
  Blynk.virtualWrite(V1, FastLED.getBrightness());
}

void loop() {
  if ( ! modechanged == 1 ) {
    modechanged = 0;
    FastLED.showColor(CRGB::Black);

    // Things to be cleared
    memset(heat, 0, sizeof(heat));
  }

  handleMode();
  Blynk.run();
  timer.run();
}

BLYNK_WRITE(V0) {
  Blynk.virtualWrite(V2, 3);
  mode = 3;
  modechanged = 1;
}

BLYNK_WRITE(V1) {
  FastLED.setBrightness( param.asInt() );
  Serial.println("Brightness changed");
}

BLYNK_WRITE(V2) {
  switch (param.asInt())
  {
    case 1: // fire
      mode = 1;
      modechanged = 1;
      Serial.println("Mode 1 fire");
      break;
    case 2: // rainbow
      mode = 2;
      modechanged = 1;
      Serial.println("Mode 2 rainbow");
      break;
    case 3: // off
      mode = 3;
      modechanged = 1;
      Serial.println("Mode 3 off");
      break;
  }
}

void runEveryHalfMinute() {
  if ( mode == 3 ) {
    flashStdby();
  }

  BLYNK_LOG("half minute job ran");
}

void runEveryFiveSeconds() {
  Serial.print("bat: "); Serial.print(battery.voltage());
  Serial.print(" "); Serial.print(battery.level());
  Serial.println("%");
  Blynk.virtualWrite(V4, battery.level());
  Blynk.virtualWrite(V3, battery.voltage());
}

void handleMode() {
  switch (mode) {
    case 1:
      // Add entropy to random number generator; we use a lot of it.
      random16_add_entropy( random(0,65535) );

      Fire(); // run simulation frame
      FastLED.show(); // display this frame
      FastLED.delay(1000 / FRAMES_PER_SECOND);
      break;
    case 2:
      rainbow_march();
      FastLED.show(); // display this frame
      //FastLED.delay(1000 / FRAMES_PER_SECOND);
      break;
    case 3:
      break;
  }
}

void flashStdby() {
  FastLED.showColor(CRGB::Black);
  FastLED.showColor(CRGB::MidnightBlue);
  delay(150);
  FastLED.showColor(CRGB::Black);
  delay(150);
  FastLED.showColor(CRGB::MidnightBlue);
  delay(150);
  FastLED.showColor(CRGB::Black);
}

void flashRst() {
  unsigned long startTime = millis();
  while (millis() - startTime <= 5000) {
    digitalWrite(LEDPIN, HIGH);
    delay(200);
    digitalWrite(LEDPIN, LOW);
    delay(200);
  }
}

// Fire2012 by Mark Kriegsman, July 2012
// as part of "Five Elements" shown here: http://youtu.be/knWiGsmgycY
//
// This basic one-dimensional 'fire' simulation works roughly as follows:
// There's a underlying array of 'heat' cells, that model the temperature
// at each point along the line.  Every cycle through the simulation,
// four steps are performed:
//  1) All cells cool down a little bit, losing heat to the air
//  2) The heat from each cell drifts 'up' and diffuses a little
//  3) Sometimes randomly new 'sparks' of heat are added at the bottom
//  4) The heat from each cell is rendered as a color into the leds array
//     The heat-to-color mapping uses a black-body radiation approximation.
//
// Temperature is in arbitrary units from 0 (cold black) to 255 (white hot).
//
// This simulation scales it self a bit depending on NUM_LEDS; it should look
// "OK" on anywhere from 20 to 100 LEDs without too much tweaking.
//
// I recommend running this simulation at anywhere from 30-100 frames per second,
// meaning an interframe delay of about 10-35 milliseconds.
//
//
// There are two main parameters you can play with to control the look and
// feel of your fire: COOLING (used in step 1 above), and SPARKING (used
// in step 3 above).
//
// COOLING: How much does the air cool as it rises?
// Less cooling = taller flames.  More cooling = shorter flames.
// Default 55, suggested range 20-100
#define COOLING  30

// SPARKING: What chance (out of 255) is there that a new spark will be lit?
// Higher chance = more roaring fire.  Lower chance = more flickery fire.
// Default 120, suggested range 50-200.
#define SPARKING 50


void Fire()
{
  // Step 1.  Cool down every cell a little
    for( int i = 0; i < NUM_LEDS; i++) {
      heat[i] = qsub8( heat[i],  random8(0, ((COOLING * 10) / NUM_LEDS) + 2));
    }

    // Step 2.  Heat from each cell drifts 'up' and diffuses a little
    for( int k= NUM_LEDS - 3; k > 0; k--) {
      heat[k] = (heat[k - 1] + heat[k - 2] + heat[k - 2] ) / 3;
    }

    // Step 3.  Randomly ignite new 'sparks' of heat near the bottom
    if( random8() < SPARKING ) {
      int y = random8(7);
      heat[y] = qadd8( heat[y], random8(160,255) );
    }

    // Step 4.  Map from heat cells to LED colors
    for( int j = 0; j < NUM_LEDS; j++) {
        leds[j] = HeatColor( heat[j]);
    }
}


void rainbow_march() {                                        // The fill_rainbow call doesn't support brightness levels
  thishue++;                                                  // Increment the starting hue.
  fill_rainbow(leds, NUM_LEDS, thishue, deltahue);            // Use FastLED's fill_rainbow routine.
}
