

#include <Keypad.h>
#include <U8g2lib.h>
#include <SPI.h>
#include <Wire.h>
#include <midi_serialization.h>
#include <usbmidi.h>

U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C u8g2(U8G2_R0); 

// Accompanying code for Retrokits RK007 MIDI command keypad
// A - send program change : follow by number and press #
// B - send note: follow by note number and press #
// C - 1x set working MIDI channel: follow by channel number 1-16 and press #
//   - 2x set controller number for dial: follow by CC number and press #
// D - send tempo start/stop command
// * = cancel, or when no command active - send note panic
//
// more info on the 4x4 keypad an it's workings:
// https://iamzxlee.wordpress.com/2013/07/24/4x4-matrix-keypad/

// Example seller on ebay:
// https://www.ebay.com/itm/4x4-Matrix-16-Keyboard-Keypad-USE-Keys-PIC-AVR-Stamp-AD/112171244640

// Arduino Pro Micro:
// https://www.ebay.com/itm/New-Pro-Micro-ATmega32U4-5V-16MHz-Replace-ATmega328-Arduino-Pro-Mini-/221891843710

// OLED display connect to GND, 5V, port A5(SCL) and A6(SDA):
// https://www.ebay.com/itm/IIC-0-91-0-96-inch-128x32-Blue-OLED-LCD-Display-Module-For-AVR-5v-M5F2/323942455426

const byte ROWS = 4; // Pad with four rows
const byte COLS = 4; // and four columns

// Define the Keymap
char keys[ROWS][COLS] = {
  {'1','4','7','*'},
  {'2','5','8','0'},
  {'3','6','9','#'},
  {'A','B','C','D'}
};

// arduino connection pins
byte rowPins[ROWS] = { 21,20,19,18};
byte colPins[COLS] = {15,14,16,10}; 

// var definitions for keypad
// holds typed number
int keyval=0;

// holds global midi channel
byte midichannel=0;

byte keymode=0;
// holds pad keymode 

// vars to hold screen states, delays and midiclock state
bool dispUpdate=false;
bool clock_running=false;
long currentTime=0;
byte pc=0;
byte nt=0;
byte lastnt=0;

// midi message codes
#define MIDI_CLOCKSTART 0xFA
#define MIDI_CLOCKSTOP  0xFC

// pin for potmeter 10K
#define potpin          A6

// holds pot value and extras for anti-jitter
byte oldpotval = 0;
byte potval = 0;
byte aController=1;
static uint32_t oldtime=millis();

// Create the Keypad
Keypad kpd = Keypad( makeKeymap(keys), rowPins, colPins, ROWS, COLS );

#define ledpin 13
// LED goes on when in 'number enter' mode and goes off when # is pressed

byte clamp(byte mn, byte mx,int val){
  return (byte)max(mn,min(mx,val));
}
// these commands send out to USB and serial port:
/* -----------------------------------------------*/
void sendClockCommand(uint8_t cmd){
  USBMIDI.write(cmd);
  byte buf[1]={cmd};
  Serial1.write(buf,1);
}
void sendCC(uint8_t channel, uint8_t control, uint8_t value) {
  USBMIDI.write(0xB0 | channel);
  USBMIDI.write(control );
  USBMIDI.write(value );
  byte buf[3]={0xB0 | channel ,control,value};
  Serial1.write(buf,3);
}

void sendNote(uint8_t channel, uint8_t note, uint8_t velocity) {
  USBMIDI.write((velocity != 0 ? 0x90 : 0x80) | channel);
  USBMIDI.write(note );
  USBMIDI.write(velocity );
  byte buf[3]={(velocity != 0 ? 0x90 : 0x80) | channel,note,velocity};
  Serial1.write(buf,3);
}

void sendProgram(uint8_t channel, uint8_t pnumber) {
  USBMIDI.write(0xC0 | channel );
  USBMIDI.write(pnumber );
  byte buf[2]={0xC0 | channel ,pnumber};
  Serial1.write(buf,2);
}
/* -----------------------------------------------*/
// board initialisation
/* -----------------------------------------------*/

void setup()
{
  
  pinMode(potpin, INPUT);
  pinMode(ledpin,OUTPUT);
  Serial1.begin(31250); // midi speed
  while (!Serial1) {
    //wait for serial to become available
  }
  u8g2.begin();
  u8g2.clearBuffer();  // clear the internal oLED memory
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  // choose a suitable font at https://github.com/olikraus/u8g2/wiki/fntlistall
  // extra fonts do take up a lot of memory so beware
  u8g2.drawStr(0,20,"RK-007 MIDI COMMANDER");
  u8g2.drawStr(20,31,"*** READY ***");
  dispOver();
}
/* -----------------------------------------------*/

// main loop for checking keypad and midi
void loop(){
  // eventhough we're not processing midi in, we need to poll to prevent buffer overrun
  USBMIDI.poll();
  while (USBMIDI.available()) {
    u8 b = USBMIDI.read();
  }
  // keeps track of display message timeouts and polls the potmeter not too often
  if ( (millis()-oldtime) > 100) {
    oldtime = millis();
    potval= (analogRead(potpin) * 0.1245); // rescale pot value to 0-127 (MIDI range)

    // potmeter value change a little dithering between old and new value keeps jitter -more or less- away
    if(((oldpotval+potval)/2) != potval){
      dispUpdate=true;
      sendCC(midichannel, aController, potval);
      oldpotval=potval;
    }
  }
  if(dispUpdate){ // do we need an update on the screen for a subscreen?
    if((millis()-currentTime)>1500){
      dispUpdate=false;
      u8g2.clearBuffer();
      u8g2.setCursor(0,19);
      u8g2.print("Dial CC "+String(aController)+":"+String(potval)); 
      if(!clock_running){
        u8g2.drawStr(98,19,"STOP");
      }else{
        u8g2.drawStr(98,19,"PLAY");
      }
      u8g2.setCursor(0,30);
      u8g2.print("Patch:"+String(pc+1)+" MIDI CH:"+String(midichannel+1)); 
      u8g2.sendBuffer();
      if(nt>0){ // there is a note on, off it
        sendNote(midichannel, nt, 0);
        nt=0; // notes are off
      }
    }
  }
  char key = kpd.getKey();
  if (key != NO_KEY)
  {
    // is it numeric:
    if ( (key >= '0') && (key<= '9') ){
      dispUpdate=false; // prevent timeout on input value
      currentTime = millis(); // reset key timeout
       digitalWrite(ledpin, HIGH);
       // add the numeric keys and multiply a possible former key by ten
       keyval= keyval *10;
       // key is a ASCII keycode 0 so deduct '0' means: '0'is actually 0
       keyval+= (key-'0'); // add it to the existing value
       if(keyval>128){
         setSubScreen("     INVALID",true);
         keyval=0;
         return;
       }
       if(keymode==0){
         setSubScreen("Program Change #:"+String(keyval),false);
       }
       if(keymode==1){
         setSubScreen("Note #:"+String(keyval),false);
       }
       if(keymode==2){
         setSubScreen("Controller #:"+String(keyval),false);
       }
       if(keymode==3){
         setSubScreen("Channel #:"+String(keyval),false);
       }
      }
    if ( key == '#') // command close, end our key group
     {
      if(keyval==0){
        // no data entered, send note panic
        setSubScreen("    NOTE PANIC!",true);
        for(byte i=0;i<16;i++){
          sendCC(i,123,0);
        }
        return;
      }
      digitalWrite(ledpin, LOW);
      if(keymode==0){ // A = keymode 0 (Program Change)
        // set patch (deduct 1: 1-128 -> 0-127 internally )
        // clamp the values in midi spec 0-127
        pc=clamp(0,127,keyval-1);
        sendProgram(midichannel,pc);
        
        setSubScreen( "Patch "+String(pc+1)+", Channel:"+String(midichannel+1),true);
      }
      if(keymode==1){ // B = keymode 1 (Note Send)
        // set note (deduct 1: 1-128 -> 0-127 internally )
        // clamp the values in midi spec 0-127
        if(nt>0) sendNote(midichannel, nt, 0);
        nt=clamp(0,127,keyval-1);
        lastnt=nt;
        sendNote(midichannel, nt, 100);
        setSubScreen("Note "+String(nt+1)+", Channel:"+String(midichannel+1),true);
      }
      if(keymode==2){ // set dial CC nr
        aController=clamp(0,127,keyval);
        setSubScreen("Dial CC:"+String(aController)+" OK",true);
      }
      if(keymode==3){ // C = keymode 2 (MIDI Channel)
        // set channel, (deduct 1: 1-16 -> 0-15 internally )
        midichannel=clamp(0,15,keyval-1);
        setSubScreen("Channel:"+String(midichannel+1)+" OK",true);
        //keymode = 0; // after midi channel change, reset to program change input mode
      }
      keyval=0;//reset for next input
      return;
     }
     
    // ------------------------------------ 
    if ( key == '*'){
      // cancel input
      if(keyval>0){
        setSubScreen("     CANCELLED",true);
        keyval=0;
      }else{
          // send clock start/stop
        if(!clock_running){
          //  send clock start
          sendClockCommand(MIDI_CLOCKSTART);  
          setSubScreen("        START",true);
  
        }else{
          //  send clock stop
          sendClockCommand(MIDI_CLOCKSTOP);  
          setSubScreen("        STOP",true);
        }
        clock_running=!clock_running;
      }
    }
    // ------------------------------------ 
    if ( key == 'A'){
      lastnt=0; // erase note repeat mode on B mode
      // set to 'MIDI Patch change' mode
      keymode=0;
      keyval=0;
        setSubScreen(" SEND PROGRAM CHANGE",true);
    }
    
    // ------------------------------------ 
    if ( key == 'B'){
      // quick note send
      if((keymode==1) && (lastnt>0)){
        if(nt>0) sendNote(midichannel, nt, 0);
        nt=lastnt;
        //already in note mode, repeat last entered one
        sendNote(midichannel, nt, 100);
        setSubScreen("Note "+String(lastnt+1)+", Channel:"+String(midichannel+1),true);
      }else{
        keymode=1;
        keyval=0;
        setSubScreen("  SEND NOTE NUMBER",true);
      }
    }
    
    // ------------------------------------ 
    if ( key == 'D'){
      lastnt=0; // erase note repeat mode on B mode
       // set dial controller number
       keymode = 2;
       keyval=0;
       setSubScreen("SET DIAL CONTROLLER",true);
    }
    
    // ------------------------------------ 
    if ( key == 'C'){
      lastnt=0; // erase note repeat mode on B mode
      // set midi channel
      keymode = 3;
      keyval=0;
      setSubScreen("  SET MIDI CHANNEL",true);
    }
    
  }
  USBMIDI.flush();
}

void setSubScreen(String message,bool tmout){
   u8g2.clearBuffer();
   ////u8g2.setFont(u8g2_font_fub20_tf);
   u8g2.setCursor(0,25);
   u8g2.print(message); 
   u8g2.sendBuffer();
   if(tmout){
    dispOver();
   }else{
    u8g2.sendBuffer();
   }
}
// this function sends the screen of a key status update and
// sets the timeout function to revert to the overview screen
void dispOver(){
  u8g2.sendBuffer();
  dispUpdate=true;
  currentTime = millis();
}
