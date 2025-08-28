/* 
  Vox Caster / Real-time voice modulator
  By MrHarbinger

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

/*
Derived from the the Audio Hacker Library by nootropic design.
The program is designed to work with the MCP3201 12-bit ADC and MCP4921 12-bit DAC.
A rudimentary soundboard is implemented using DFPlayerMini MP3 player. If no such module
is connected to pins 9 and 10, make sure "usingDFRobot" is not #defined, by commenting out "#define usingDFRobot".
If you are using DFRobotPlayerMini, make sure the respective libraries are installed (SoftwareSerial and DFRobotDFPlayerMini).
*/

//#define usingDFRobot

#if defined(usingDFRobot)
  #include "SoftwareSerial.h"
  #include "DFRobotDFPlayerMini.h"
  SoftwareSerial mySoftwareSerial(9, 10); // RX, TX
  DFRobotDFPlayerMini myDFPlayer;
#endif

static constexpr uint8_t buttonPin1 = 4;
static constexpr uint8_t buttonPin2 = 5;
static constexpr uint8_t buttonPin3 = 6;
static constexpr uint8_t ledPin = 3;
static constexpr uint8_t DAC_SS_Pin = 8;
static constexpr uint8_t ADC_SS_Pin = 7;
static constexpr uint16_t bufferSize = 512;
static constexpr uint8_t nbrAudioFiles1 = 16;
static constexpr uint8_t nbrAudioFiles2 = 4;

/*Macros for fast register operations*/
#define SCK_LOW PORTB &= ~(1 << PB5) //Write D13 low
#define SCK_HIGH PORTB |= (1 << PB5) //Write D13 high
#define MISO_READ PINB & (1 << PB4) //Compare pin D12 MISO
#define SS_DAC PORTB &= ~(1 << PB0) //Slave select DAC = D8
#define DS_DAC PORTB |= (1 << PB0)
#define SS_ADC PORTD &= ~(1 << PD7) //Slave select ADC = D7
#define DS_ADC PORTD |= (1 << PD7)
#define NOP __asm__ __volatile__ ("nop\n\t") //Do nothing for 1 CPU cycle
#define FORCE_INLINE inline __attribute__((always_inline)) //Force compiler to inline functions (faster than function calls)

/*Global variables*/
volatile uint16_t output = 0x1000 / 2; //Initialize output at midpoint (half VCC, 12-bit resolution)
volatile uint16_t _count = 0;
uint8_t modulatorVal = 7;
bool noMod = 0;
volatile uint32_t outputAdress = 0;
volatile uint32_t sampleAdress = 0;
uint16_t sampleBuffer[bufferSize];

/*SPI functions for use by the ADC and DAC methods*/
FORCE_INLINE static void spiToggle() {
  NOP; NOP; NOP; NOP; SCK_HIGH; NOP; NOP; NOP; NOP; SCK_LOW;
}

FORCE_INLINE static uint8_t spiWrite(uint8_t data) {
  SPDR = data;
  while (!(SPSR & _BV(SPIF)));
  return SPDR;
}

FORCE_INLINE static void spiRead(uint16_t &payload, uint8_t bit) {
  NOP; NOP; NOP;
  SCK_HIGH;
  NOP; NOP;
  if (MISO_READ) payload |= (1 << bit);
  SCK_LOW;
}

/*Audio class definitions and methods*/
class audioClass {
public:
  audioClass();
  void initialize();
  uint16_t RADC(); //Read from ADC
  void WDAC(uint16_t DACoutput); //Write to DAC
};

audioClass::audioClass() {
}

void audioClass::initialize() {

  DDRB |= (1 << PB5) | (1 << PB3) | (1 << PB0); //D8, D11, D13 as outputs
  DDRD |= (1 << PD3) | (1 << PD7); //D3 and D7 as outputs
  DDRB &= ~(1 << PB4); //Ensure D12 is input
  DDRD &= ~((1 << PD4) | (1 << PD5) | (1 << PD6)); //D4, D5, D6 are inputs
  PORTD |= (1 << PD4) | (1 << PD5) | (1 << PD6) | (1 << PD7); //D4, D5, D6 Pull-ups, D3 and D7 high
  PORTB |= (1 << PB0); //D8 High
  PORTB &= ~((1 << PB5) | (1 << PB3)); //D11 and D13 low

#ifndef usingDFRobot
  DDRB |= (1 << PB2); 
  PORTB |= (1 << PB2); //If not using pin10 for DFrobot, enable pullup for SPI
#endif

  //Start SPI at 8MHz (fastest Nano can do, MCP4921 supports up to 20MHz)
  SPCR |= _BV(SPE);    // enable SPI
  SPCR &= ~_BV(SPIE);  // Interrupts off
  SPCR &= ~_BV(DORD);  // MSB first
  SPCR |= _BV(MSTR);   // Arduino as master
  SPCR &= ~_BV(CPOL);  // Rising edge
  SPCR &= ~_BV(CPHA);  // SCK idle state low
  SPCR &= ~_BV(SPR1);  // speed = clock/4
  SPCR &= ~_BV(SPR0);  
  SPSR |= _BV(SPI2X);  // 2X speed

  //Configure Timer1 interrupts
  static constexpr uint16_t SR = 22050;// Sampling Rate
  cli(); // disable global interrupts during config
  TCCR1A = 0;  // normal port operation
  TCCR1B = 0;  
  OCR1A = (F_CPU / SR);     // Interrupt at ~22050Hz ((16Mhz / 22.05kHz) = 725)
  TCCR1B |= (1 << WGM12);   // CTC mode: reset on OCR1A
  TCCR1B |= (1 << CS10);    // prescaler = 1
  TIMSK1 |= (1 << OCIE1A);  // enable interrupt on compare match
  sei(); //Re-enable interrupts
}

/* MCP3201's maximum SPI Speed is 1.6MHz. Done with bit-banging.
  Adapted from Audiohacker and McpSarAdc library under GPL version 3.*/
uint16_t audioClass::RADC() { //Reading from the ADC
  uint16_t sample = 0;
  SPCR &= ~_BV(SPE);    // disable HWSPI

  SS_ADC; //Slave select ADC

  // Start sample
  spiToggle();
  // Stop sample
  spiToggle();
  // Null bit
  spiToggle();

  // Assemble 12-bit sample from MISO
  spiRead(sample, 11);
  spiRead(sample, 10);
  spiRead(sample,  9);
  spiRead(sample,  8);
  spiRead(sample,  7);
  spiRead(sample,  6);
  spiRead(sample,  5);
  spiRead(sample,  4);
  spiRead(sample,  3);
  spiRead(sample,  2);
  spiRead(sample,  1);
  spiRead(sample,  0);

  DS_ADC; //Deselect ADC
  SPCR |= _BV(SPE);    // re-enable HWSPI

  return sample;
}

void audioClass::WDAC(uint16_t DACoutput) { //Writing to the DAC (at 8MHz)
  static constexpr uint16_t CONFIG = 0x30; // DAC config settings, gain=1, no buffered, disable shutdown
  uint8_t firstByte = (CONFIG | (DACoutput >> 8) & 0xF); // 4 config bits and 4 upper bits of output
  SS_DAC; //Slave select DAC
  spiWrite(firstByte); //Send first byte
  spiWrite(DACoutput); //Send last byte
  DS_DAC; //Deselect DAC
}

/*Button functions.*/
__attribute__((cold)) void button1Call() {
  modulatorVal--;
  if (modulatorVal < 2) {
    modulatorVal = 10;
  }
  noMod = (modulatorVal == 10); //Disable modulation if modulatorVal == 10
}

__attribute__((cold)) void button2Call(uint8_t *lastfile) {
#if defined(usingDFRobot) //For DFPlayer soundboard Folder 1
  myDFPlayer.playFolder(1, *lastfile);
  *lastfile = *lastfile + 1;
  if (*lastfile > nbrAudioFiles1) {
    *lastfile = 1;
  }     
#endif  
}

__attribute__((cold)) void button3Call(uint8_t *lastfile) { 
#if defined(usingDFRobot) //DFPlayer Soundboard Folder 2
  myDFPlayer.playFolder(2, *lastfile);
  *lastfile = *lastfile + 1;
  if (*lastfile > nbrAudioFiles2) {
    *lastfile = 1;
  }
#endif    
}

audioClass audioMod = audioClass();


/*Setup and loop starts here*/
void setup() {

/*DFPlayer setup*/
#if defined(usingDFRobot)
  mySoftwareSerial.begin(9600);
  delay(100); //Wait for serial to establish
  if (!myDFPlayer.begin(mySoftwareSerial)) { 
    while(true);
  }
  myDFPlayer.volume(30);
#endif   

  audioMod.initialize();
}

void loop() {
  static constexpr uint16_t debounceTimer = 300;
  uint32_t currentTime = millis();
  static uint32_t lastButtonPress;
  static uint8_t lastfile1 = 1;
  static uint8_t lastfile2 = 1;    
  static uint32_t llu;
  static bool ledstat = 0;

  if(((currentTime-lastButtonPress)>=debounceTimer)){
    if((digitalRead(buttonPin1) == 0)){
         button1Call();
        lastButtonPress = currentTime;
    } else if((digitalRead(buttonPin2) == 0)){
        button2Call(&lastfile1);
        lastButtonPress = currentTime;
    } else if(digitalRead(buttonPin3) == 0){
        button3Call(&lastfile2);
        lastButtonPress = currentTime;
    }
  }

/*LEDControl*/
    if (((currentTime-llu)>=800)) {
    digitalWrite(ledPin, HIGH);
    //ledstat = !ledstat;
    //digitalWrite(ledPin, ledstat);    
    llu = currentTime;
  }
}

ISR(TIMER1_COMPA_vect) {
  uint16_t sample;

  audioMod.WDAC(output);
  sample = audioMod.RADC();
  sampleBuffer[sampleAdress] = sample;

  _count++;
  if ((_count < modulatorVal) || (noMod)) { 

    output = sampleBuffer[outputAdress];
    outputAdress++;
    uint16_t diff = bufferSize - sampleAdress;
    if (diff <= 16) { //When nearing end of buffer, mix input and modulated output
      uint16_t mix = (output * diff) + (sample * (16-diff)); 
      mix = mix >> 4; // DAC only takes 12 bits
      output = mix;
    }
  } else {
    _count = 0;
  }

  sampleAdress++;
  if (sampleAdress >= bufferSize) {
    sampleAdress = 0; // At end of buffer, reset index to start
    outputAdress = 0;
  }
}
