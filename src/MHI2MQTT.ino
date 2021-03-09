/*
##############################################################################################################################################################################################
MHI ESP2MQTT Interface v1.0.0
Arduino-based communication interface for Mitsubishi Heavy Industries (MHI) SRK/SRF series air conditioners.
Connects to the MHI CNS connector and synchronizes to its Serial Peripheral Interface (SPI). Updates from the MHI are processed and sent via serial to an ESP8266 running an MQTT client.
Updates received via MQTT are sent from the ESP8266 to the Arduino over serial and injected into the SPI data frames to update the MHI.
R.J. Dekker, June 2018
##############################################################################################################################################################################################
*/

#include <FS.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>                                                                                           //https://github.com/tzapu/WiFiManager
#include <ArduinoJson.h>                                                                                           //https://github.com/bblanchon/ArduinoJson
#include <PubSubClient.h>                                                                                          //https://github.com/knolleary/pubsubclient
#include <SPIFFS.h>
#include <SPI.h>
//#include <EasyTransfer.h>                                                                                          //https://github.com/madsci1016/Arduino-EasyTransfer

//Access point that WiFiManager starts for configuration. Name and password should be set below before flashing. This is hardcoded and cannot be changed later.
#define configSSID  "MHI Roomname"                                                                                 //AP name (give every unit a unique name before flashing)
#define configPW    "mitsubishi"                                                                                   //Password to connect to the AP

TaskHandle_t SerialTask;
//Variables below are initial values that can be changed at any time from the WiFiManager configuration portal and will be stored in flash memory. If there are different values in config.json, they are overwritten.
char mqtt_server[16]     = "0.0.0.0";
char mqtt_port[9]        = "1883";
char mqtt_user[20]       = "";
char mqtt_pass[20]       = "";
char wifiTimeout[3]      = "5";                                                                                    //Timeout in minutes (max. 99) before WiFi configuration portal is turned off and the ESP tries to connect again to the previously configured AP (if any)
char Room[20]            = "Roomname";
char Thing[20]           = "Aircon";
char Setpoint[60]        = "Setpoint";
char statusSetpoint[60]  = "statusSetpoint";
char State[60]           = "State";
char statusState[60]     = "statusState";
char statusRoomtemp[60]  = "statusRoomtemp";
char Vanes[60]           = "Vanes";
char statusVanes[60]     = "statusVanes";
char Fanspeed[60]        = "Fanspeed";
char statusFanspeed[60]  = "statusFanspeed";
char debug[60]           = "debug";                                                                                //Send only
char service[60]         = "service";                                                                              //Receive only

//Variables below hold the current values of bit fields 4-7 and all adjustable settings to check if anything changed after receiving an update from the MHI/Arduino
//Bit field variables are initialized with 255 to force an MQTT update with the most recent MHI settings directly after booting
//The minimal set of bit fields needed to communicate power, mode, setpoint, roomtemp, vanes and fans speed is bit fields 4-7 and 10
byte current_Bitfield4   = 255;                                                                                    //Power, mode and vane swing settings
int  current_Bitfield5   = 255;                                                                                    //Vanes setting 1-4 and fan speed 1-3 (4 is in bit field 10)
byte current_Bitfield6   = 255;                                                                                    //Temperature setpoint
byte current_Bitfield7   = 255;                                                                                    //Room temperature
byte current_Mode        = 255;
byte current_Vanes       = 255;
bool current_Swing       = false;
byte current_Fanspeed    = 255;
bool current_Fanspeed4   = false;

bool debugit = false;                                                                                              //Send some info (eg. MHI SPI bit field updates and errors) to debug topic
int connectionFails = 0;                                                                                           //Count number of failed MQTT connection attempts for restart

//                               HEAT        COOL        AUTO        DRY         FAN
static byte modeValues[5] = { 0b00010001, 0b00001001, 0b00000001, 0b00000101, 0b00001101 };                        //Used to extract current mode from bit field 4

WiFiClient espClient;
PubSubClient client(espClient);

void callback(char* topic, byte* payload, unsigned int length);                                                    //Callback function header

//Flag for saving data
bool shouldSaveConfig = false;

//Setup EasyTransfer (by Bill Porter)
//EasyTransfer ETin, ETout;

struct RECEIVE_DATA_STRUCTURE {                                                                                    //Variables received from Arduino
  byte currentMHI[8];                                                                                              //Contains bitfields last received from MHI for bitfields 4 - 10 (currentMHI[0]-[6]). currentMHI[7] holds the number of SPI-MHI sync errors.
};

struct SEND_DATA_STRUCTURE {                                                                                       //Variable send to Arduino
  byte mode;                                                                                                       //Mode      [1]OFF,  [2]HEAT,  [3]COOL,  [4]AUTO,  [5]DRY,   [6]FAN,  [7]ON,  [64]RESET
  byte vanes;                                                                                                      //Vanes     [1]UP,   [2]2,     [3]3,     [4]DOWN,  [5]SWING
  byte fanspeed;                                                                                                   //Fanspeed  [1]1,    [2]2,     [3]3,     [4]4
  byte setpoint;                                                                                                   //Setpoint  [18]18 -> [30]30 degrees Celsius
} __attribute__((packed));                                                                                         //Necessary for correct transfer of struct between Arduino and ESP8266

RECEIVE_DATA_STRUCTURE   fromArduino;
SEND_DATA_STRUCTURE   toArduino;

bool checksumError = false;                                                                                        //Flag for checksum error in frame 2 or 47. If two errors occur at both these positions in a single 48-frame cycle -> SPI sync lost -> resync SPI

byte newMode     = 0;                                                                                              //Temporary storage of settings received from ESP
byte newVanes    = 0;
byte newFanspeed = 0;
byte newSetpoint = 0;

const byte rx_frameSignature[3] =  { 0x6C, 0x80, 0x04 };                                                           //SPI frame start signature: first 3 bytes in a full SPI data frame. Used to sync to MHI SPI data in SPI_sync() routine. Might be different on other unit types!

//SPI frame that is currently being sent during the SPI interrupt routine. Contains base values that will be updated after the first 48-frame cycle with values received from MHI.
//                         Bitfield:    1     2     3     4     5     6     7     8     9    10    11    12    13    14    15    16    17    18    19    20
volatile byte tx_SPIframe[20]   =  { 0xA9, 0x00, 0x07, 0x4C, 0x00, 0x2A, 0xFF, 0x00, 0x00, 0x40, 0x00, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0x0F, 0x04, 0x05, 0xF5 };

volatile byte rx_SPIframe[20];                                                                                     //Array to collect a single frame of SPI data received from the MHI unit

byte rx_bitfield4_10[7];                                                                                           //Array containing bitfields 4-10 from rx_SPIframe, which holds current MHI mode, vanes, fans speed, ambient temperature and setpoint

//Alternating bitfield 10-18 variations, each successively send for 48 frames. Bit 3 in bitfield 18 functions as a clock and is 1 for 24 frames and 0 for the subsequent 24 frames. I have never seen bit fields 11-12 changing, so don't know what they are for.
//                         Bitfield:   10    11    12    13    14    15    16    17    18
const byte frameVariant[3][9]  {   { 0x40, 0x00, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0x0F, 0x04 },  //variant number 0
                                   { 0x80, 0x00, 0x00, 0x32, 0xD6, 0x01, 0x00, 0x0F, 0x04 },  //variant number 1
                                   { 0x80, 0x00, 0x00, 0xF1, 0xF7, 0xFF, 0xFF, 0x0F, 0x04 }   //variant number 2
                               };

//MODE bitmasks                            Bitfield #4
const byte modeMask[8][2]      { //     CLEAR   |    SET
                                   { 0b00100010, 0b00000000 },  //0 = Unchanged (only clear 'write' bits)
                                   { 0b00100011, 0b00000010 },  //1 = OFF
                                   { 0b00111111, 0b00110011 },  //2 = HEAT
                                   { 0b00111111, 0b00101011 },  //3 = COOL
                                   { 0b00111111, 0b00100011 },  //4 = AUTO
                                   { 0b00111111, 0b00100111 },  //5 = DRY
                                   { 0b00111111, 0b00101111 },  //6 = FAN
                                   { 0b00100011, 0b00000011 }   //7 = ON (using last mode)
                               };

//VANES bitmasks                          Bitfield #4             Bitfield #5
const byte vanesMask[6][4]     { //     CLEAR   |    SET        CLEAR   |    SET
                                   { 0b10000000, 0b00000000, 0b10000000, 0b00000000 },  //0 = Unchanged (only clear 'write' bits)
                                   { 0b11000000, 0b10000000, 0b10110000, 0b10000000 },  //1 = 1 (up)
                                   { 0b11000000, 0b10000000, 0b10110000, 0b10010000 },  //2 = 2
                                   { 0b11000000, 0b10000000, 0b10110000, 0b10100000 },  //3 = 3
                                   { 0b11000000, 0b10000000, 0b10110000, 0b10110000 },  //4 = 4 (down)
                                   { 0b11000000, 0b11000000, 0b10000000, 0b00000000 }   //5 = swing
                               };

//FANSPEED bitmasks                        Bitfield #5             Bitfield #10
const byte fanspeedMask[5][4]  { //     CLEAR   |    SET        CLEAR   |    SET
                                   { 0b00001000, 0b00000000, 0b11011000, 0b00000000 },  //0 = Unchanged (only clear 'write' bits)
                                   { 0b00001111, 0b00001000, 0b11011000, 0b00000000 },  //1 = Speed 1
                                   { 0b00001111, 0b00001001, 0b11011000, 0b00000000 },  //2 = Speed 2
                                   { 0b00001111, 0b00001010, 0b11011000, 0b00000000 },  //3 = Speed 3
                                   { 0b00001111, 0b00001010, 0b11011000, 0b00010001 }   //4 = Speed 4
                               };

volatile byte state = 0;                                                                                           //'State machine' in loop(): [State=0] Priority is given to SPI interrupt to collect a full 20-byte SPI data frame. No other processing is allowed to keep in sync. [State=1] Pulse clock, set new/updated frames etc.. [State=2] Check for data received from ESP via serial
volatile char bitfield = 0;                                                                                        //Bitfield position counter for sending (tx_SPIframe[]) and recceiving (rx_SPIframe[])
byte variantnumber = 0;                                                                                            //Frame variation that is currently being sent (0, 1 or 2 in frameVariant[])
byte framenumber = 1;                                                                                              //Counter for how many times a frame variation has been sent (max. = 48)


//Callback notifying us of the need to save WiFiManager config to FS
void saveConfigCallback ()
{
  //Serial.println("Should save config");
  shouldSaveConfig = true;
}

template <typename Generic> void debug2mqtt(Generic text)
{
  if (debugit)
    {
      client.publish(debug, text, true);
    }
  }

inline bool verify_checksum (void)                                                                                 //Routine to verify checksum of a received SPI frame. Don't care to pass the array to the function, because it's the same array that needs to be checked every time.
{
  uint16_t sum = 0;                                                                                                //Reset checksum

  for (int bf = 0; bf < 18; bf++)                                                                                  //Calculate SPI byte frame checksum (sum bytes 1 to 18)
    {
      sum += rx_SPIframe[bf];
    }

  return (rx_SPIframe[18] == highByte(sum) && rx_SPIframe[19] == lowByte(sum));                                    //Calculate MSB and LSB of checksum and compare with byte 19 and 20. Returns true if checksum is correct.
}

inline void update_checksum (void)                                                                                 //Update the checksum MSB (byte 19) and LSB (byte 20). Don't care to pass the array to the function, because it's the same array that needs to be updated every time.
{
  uint16_t sum = 0;

  for (int bf = 0; bf < 18; bf++)
    {
      sum += tx_SPIframe[bf];                                                                                      //Calculate checksum by summing bitfield 1 - 18 of SPI frame
    }

  tx_SPIframe[18] = highByte(sum);                                                                                 //Calculate MSB and LSB of checksum and write to byte 19 and 20
  tx_SPIframe[19] = lowByte(sum);
}

void softwareReset(void)          //(uint8_t prescaler)                                                            //Watchdog not working as aspected (bootloader bug?). Revert to more primitive reset method.
{
  asm volatile ("  jmp 0");        //wdt_enable(prescaler);                                                        //Start watchdog with the provided prescaler
  while(1) {}
}


void SPI_sync (void)
{
  SPCR &= ~(1<<SPIE);                                                                                              //Turn off SPI interrupts during syncing

  memcpy(&tx_SPIframe[9], &frameVariant[0][0], 9);                                                                 //Copy (part of) the next frame to the current frame for sending on the upcoming bitfield 18 clock cycle
  update_checksum();                                                                                               //Recalculate checksum of tx_SPIframe

  int resyncAttempts = 0;                                                                                          //Count number of resync attempts. If resyncing doesn't work -> restart system

  resync:
  //Finds the start of the first complete frame by looking for a signature.
  //When the first SPI transfer is started, the data sometimes starts in the middle of a frame
  //or out of sync with the SPI clock. This routine scans for the first 3 bytes as given
  //in rx_frameSignature[]. It then reads and discards the next 17 bytes before handing further
  //SPI data exchange to the interrupt routine ISR(SPI_STC_vect).

  //Serial.print("Syncing SPI to master...");
  int hits = 0;                                                                                                    //Number of times consecutive signature bytes have been encountered
  int cycle = 0;                                                                                                   //Stores number of bytes checked for signature. If too high -> SPI out of sync?
  byte r;                                                                                                          //Used to store read byte

  SPCR &= ~(1<<SPE);                                                                                               //Turn off SPI hardware
  delay(15);                                                                                                       //For resyncs: if sync is lost because SPI data exhange is shifted out of phase a few bits, then waiting 15 msecs should end up somewhere between two SPI frames -> resync
  SPCR |= (1<<SPE);                                                                                                //Turn SPI hardware back on, hopefully in sync

  while(1)
    {
      if (cycle++ > 25)                                                                                            //If scan takes >25 SPI bytes -> SPI CLK out-of-sync -> reset SPI and try again
        {
          if (resyncAttempts > 2)                                                                                  //Too many resync attempts -> restart system
            {
              resyncAttempts = 0;
              //Serial.println("Too many SPI sync errors!");
              //Serial.println("Restarting system...");
              delay(500);
              softwareReset();    //softwareReset(WDTO_60MS);                                                      //Restart using Watchdog Timer
              delay(5000);
            }

          //Serial.println("SPI sync error!");
          //Serial.println("Restarting SPI hardware...");

          cycle = 0;                                                                                               //Signature took too long -> SPI bytes bit-shifted? -> 1st try = restart SPI

          ++resyncAttempts;                                                                                        //Track number of resync attempts
          goto resync;                                                                                             //Restart signature scan. Not very elegant, but it works.
        }

      hits = 0;

      do                                                                                                           //Scan for 3-byte signature
        {
          SPDR = 0;                                                                                                //Send back zero for each read byte
          while(!(SPSR & (1<<SPIF))) {};                                                                           //Wait for new byte in SPI data register
          r = SPDR;                                                                                                //Read byte from SPI data register
        }
      while (r == rx_frameSignature[hits++] & hits < 3);

      if (hits == 3) break;                                                                                        //3 hits in a row -> signature found!
    }


  for (int t = 0; t < 17; t++)                                                                                     //Discard the next 17 bytes after signature to skip to the start byte of the next frame
    {
      SPDR = 0;
      while(!(SPSR & (1<<SPIF))) {};
      r = SPDR;
    }

  //Reset counters and state for resyncs
  variantnumber = 0;
  framenumber = 1;
  bitfield = 0;
  state = 0;

  checksumError = false;

  SPDR = tx_SPIframe[0];                                                                                           //Prepare SPI data register (SPDR) to send 1st byte of 1st frame on 1st interrupt

  //Serial.println("synced!");

  SPCR |= (1<<SPIE);                                                                                               //Turn on SPI interrupts

}  //End of routine SPI_sync


ISR (SPI_STC_vect)
{
  //SPI interrupt routine: This interrupt is triggered when a SPI read/write has just occurred.
  //The byte that needs to be sent should already be in the SPI data register (SPDR)
  //when this interrupt is called. This is therefore done before this interrupt routine ends.

  rx_SPIframe[bitfield]  =  SPDR;                                                                                  //Read new byte from SPI register (MOSI). Byte previously written to SPDR is send simultaneously (MISO).

  if (bitfield == 19)                                                                                              //If frame completely sent, switch to next state (processing frames, sending/reveiving to/from ESP)
    {
      state = 1;
      bitfield = -1;
    }
  else
    {
      state = 0;
    }

  SPDR = tx_SPIframe[++bitfield];                                                                                  //Increase bitfield position counter and setup next byte in SPI register to send on next interrupt

}   //End of interrupt routine SPI_STC_vect


void SerialTaskcode( void * parameter) {
  for(;;) {
    
  switch (state)
    {
      case 0:                                                                                                      //<<<STATE 0>>> Do nothing (wait until a complete SPI frame has been send/received)

        break;

      case 1:                                                                                                      //<<<STATE 1>>> Complete frame send/received -> decide what to do based on current frame number (out of 48)
        switch (framenumber)
          {
            case 2:                                                                                                //<FRAME 2> Verify checksum on SPI frame that was just received and send to ESP8266 if correct
              if (verify_checksum())                                                                               //Verify checksum
                {
                  if (variantnumber == 1 || updateESP)                                                             //Send updated MHI settings to ESP9288 in the 10th SPI frame repeat of frame 2 out of 3 (every ~6 seconds) or immediately after a new setting was sent to the MHI
                    {
                      memcpy(&toESP.currentMHI, &rx_SPIframe[3], 7);                                               //Copy bitfields 4-10 from the most recent MHI SPI frame to new array for sending to ESP
                      ETout.sendData();                                                                            //Send to ESP using EasyTransfer.

                      updateESP = false;                                                                           //Uncheck flag to send update only once
                    }

                  checksumError = false;
                }
              else
                {
                  if (checksumError)                                                                               //If true then the previous checksum at frame 47 was also wrong -> SPI sync lost? -> resync
                    {
                      toESP.currentMHI[7]++;                                                                       //Count resyncs triggered by consecutive checksum errors and send to ESP for debugging
                      updateESP = true; SPI_sync(); return;
                    }

                  checksumError = true;
                  updateESP     = true;
                }

              state = 0;
              break;                                                                                               //Start from beginning of loop() and wait for next complete SPI frame

            case 24:                                                                                               //<FRAME 24> Current frame variation has been sent 24 times -> clear clock bit in bit field 18 for the next 24 frames
              bitClear(tx_SPIframe[17], 2);                                                                        //Clear clock bit 3 in bitfield 18-> update checksum and resend for 24 cycles
              update_checksum();                                                                                   //Recalculate checksum of tx_SPIframe

              state = 0;
              break;                                                                                               //Start from beginning of loop() and wait for next complete SPI frame

            case 47:                                                                                               //<FRAME 47> Collect the most recent bit fields 4-10 for constructing an updated tx_SPIframe after the upcoming frame (48)
              if (verify_checksum())                                                                               //Verify checksum
                {
                  memcpy(&rx_bitfield4_10, &rx_SPIframe[3], 7);                                                    //Get bitfields 4-10 from the last MHI SPI frame to use for the upcoming tx_SPIframe update
                  checksumError = false;
                }
              else
                {
                  if (checksumError)                                                                               //If true then the previous checksum at frame 2 was also wrong -> SPI sync lost? -> resync
                    {
                      toESP.currentMHI[7]++;                                                                       //Count resyncs triggered by consecutive checksum errors and send to ESP for debugging
                      updateESP = true; SPI_sync(); return;
                    }

                  checksumError = true;
                }

              state = 0;
              break;

            case 48:                                                                                               //<FRAME 48> Current frame variation has been send 48 times -> construct next frame variant using most recent bit fields 4-10 collected in frame 47
              framenumber = 0;                                                                                     //Reset repeat frame counter

              if (++variantnumber > 2) variantnumber = 0;                                                          //Increase frame counter -> test if all 3 frames sent -> restart with frame 1

              memcpy(&tx_SPIframe[9], &frameVariant[variantnumber][0], 9);                                         //Copy (part of) the next frame to the current frame for sending on the upcoming bitfield 18 clock cycle

              //******************* CONSTRUCTION OF UPDATED BIT FIELDS *******************
              //Set 'state change' bits and 'write' bits if MQTT update received from ESP
              //otherwise only clear 'write' bits using masks from the xxxMask[0][] arrays
              //Bitfields 4, 5, 6, 10 are based on the last received MHI values (frame 47)
              tx_SPIframe[3]  =  rx_bitfield4_10[0] & ~modeMask[newMode][0];                                       //Clear mode bits (bitfield 4)
              tx_SPIframe[3] |=  modeMask[newMode][1];                                                             //Set mode bits

              tx_SPIframe[3] &= ~vanesMask[newVanes][0];                                                           //Clear vanes bits (bitfield 4)
              tx_SPIframe[3] |=  vanesMask[newVanes][1];                                                           //Set vanes bits

              tx_SPIframe[4]  =  rx_bitfield4_10[1] & ~vanesMask[newVanes][2];                                     //Clear vanes bits (bitfield 5)
              tx_SPIframe[4] |=  vanesMask[newVanes][3];                                                           //Set vanes bits

              tx_SPIframe[4] &= ~fanspeedMask[newFanspeed][0];                                                     //Clear fanspeed bits (bitfield 5)
              tx_SPIframe[4] |=  fanspeedMask[newFanspeed][1];                                                     //Set fanspeed bits

              bitWrite(rx_bitfield4_10[6], 0, bitRead(rx_bitfield4_10[6], 6));                                     //Copy bit 7 from rx_SPIframe[9] to bit 1 as the status bits for fan speed 4 appear to be swapped (!?) between MISO and MOSI
              tx_SPIframe[9] &=  ~0b00111111;                                                                      //Clear bits 1-6 and keep variant bits 7-8

              tx_SPIframe[9] |=  (rx_bitfield4_10[6] & ~fanspeedMask[newFanspeed][2]);
              tx_SPIframe[9] |=  fanspeedMask[newFanspeed][3];                                                     //Set fanspeed bits

              //Construct setpoint bitfield (#6) from last MHI value or MQTT update
              if (newSetpoint == 0)
                {
                  tx_SPIframe[5] = rx_bitfield4_10[2] & ~0b10000000;                                               //Copy last received MHI setpoint and clear the write bit
                }
              else
                {
                  tx_SPIframe[5] = (newSetpoint << 1) | 0b10000000;                                                //MQTT updated setpoint in degrees Celsius -> shift 1 bit left and set write bit (#8)
                }

              update_checksum();                                                                                   //Recalculate checksum of tx_SPIframe

              //Reset all state changes
              newMode     = 0;
              newVanes    = 0;
              newFanspeed = 0;
              newSetpoint = 0;

              state = 0;
              break;                                                                                               //Start from beginning of loop() and wait for next complete SPI frame.

            default:
              state = 2;                                                                                           //Use time (~30 ms) until next frame for receiving commands from ESP
          }

        framenumber++;                                                                                             //Increase repeat counter to keep track of the number of times the current frame has been sent

      case 2:                                                                                                      //<<<STATE 2>>> Check once if data received from ESP via serial (EasyTransfer)
        if (ETin.receiveData())
          {
            delay(1);                                                                                              //Delay to allow fromESP.xxx to be updated
            if (fromESP.mode == 64)   softwareReset();  //softwareReset(WDTO_60MS);                                //Requested reset by ESP8266 via MQTT service topic (bitfield = 32 received)

            //Store new commands received from ESP in newXXX, but only if not equal to zero
            //to prevent cancellation of previous commands that have not been send yet
            newMode     = (fromESP.mode > 0) ? fromESP.mode : newMode;
            newVanes    = (fromESP.vanes > 0) ? fromESP.vanes : newVanes;
            newFanspeed = (fromESP.fanspeed > 0) ? fromESP.fanspeed : newFanspeed;
            newSetpoint = (fromESP.setpoint > 0) ? fromESP.setpoint : newSetpoint;

            updateESP = true;                                                                                      //Flag for MHI status feedback: get an MHI update after frame number 2 and send it to the ESP for updating status of the MQTT topics
          }

        state = 0;                                                                                                 //Check for serial data once between every SPI frame (approx. every 40 ms). If this is done continuously in the loop and/or after the calculations done on frames 2, 24, 47, 48, then SPI sync can be lost.
        break;

    }  //End switch..case

  }
}


void setup()
{
//  ETin.begin(details(fromArduino), &Serial);                                                                       //Start the EasyTransfer library, pass in the data details and the name of the serial port
//  ETout.begin(details(toArduino), &Serial);

  //Read configuration from FS json
  //Serial.println("Mounting FS...");

xTaskCreatePinnedToCore(
      SerialTaskcode, /* Function to implement the task */
      "SerialTask", /* Name of the task */
      10000,  /* Stack size in words */
      NULL,  /* Task input parameter */
      0,  /* Priority of the task */
      &SerialTask,  /* Task handle. */
      0); /* Core where the task should run */

  if (SPIFFS.begin())
    {
      //Serial.println("Mounted file system");

      if (SPIFFS.exists("/config.json"))
        {
          //File exists, reading and loading
          //Serial.println("Reading config file");
          File configFile = SPIFFS.open("/config.json", "r");

          if (configFile)
            {
              //Serial.println("Opened config file");
              size_t size = configFile.size();

              //Allocate a buffer to store contents of the file.
              std::unique_ptr<char[]> buf(new char[size]);

              configFile.readBytes(buf.get(), size);
              DynamicJsonBuffer jsonBuffer;
              JsonObject& json = jsonBuffer.parseObject(buf.get());
              //json.printTo(Serial);

              if (json.success())
                {
                  //Serial.println("\nParsed json");
                  strcpy(mqtt_server, json["mqtt_server"]);
                  strcpy(mqtt_port, json["mqtt_port"]);
                  strcpy(mqtt_user, json["mqtt_user"]);
                  strcpy(mqtt_pass, json["mqtt_pass"]);
                  strcpy(wifiTimeout, json["wifiTimeout"]);
                  strcpy(Room, json["Room"]);
                  strcpy(Thing, json["Thing"]);
                  strcpy(Setpoint, json["Setpoint"]);
                  strcpy(statusSetpoint, json["statusSetpoint"]);
                  strcpy(State, json["State"]);
                  strcpy(statusState, json["statusState"]);
                  strcpy(statusRoomtemp, json["statusRoomtemp"]);
                  strcpy(Vanes, json["Vanes"]);
                  strcpy(statusVanes, json["statusVanes"]);
                  strcpy(Fanspeed, json["Fanspeed"]);
                  strcpy(statusFanspeed, json["statusFanspeed"]);
                  strcpy(debug, json["debug"]);
                  strcpy(service, json["service"]);
                }
              else
                {
                  //Serial.println("Failed to load json config");
                }
            }
        }
    }
  else
    {
      //Serial.println("Failed to mount FS");
    }

  //The extra parameters to be configured (can be either global or just in the setup)
  //After connecting, parameter.getValue() will get you the configured value
  //id/name placeholder/prompt default length
  WiFiManagerParameter custom_mqtt_server("server", "MQTT Server", mqtt_server, 16);
  WiFiManagerParameter custom_mqtt_port("port", "MQTT Port", mqtt_port, 9);
  WiFiManagerParameter custom_mqtt_user("user", "MQTT Username", mqtt_user, 20);
  WiFiManagerParameter custom_mqtt_pass("pass", "MQTT Password", mqtt_pass, 20);
  WiFiManagerParameter custom_wifiTimeout("timeout", "5", wifiTimeout, 3);
  WiFiManagerParameter custom_topic_Room("Room", "Room name", Room, 20);
  WiFiManagerParameter custom_topic_Thing("Thing", "Thing name", Thing, 20);
  WiFiManagerParameter custom_topic_Setpoint("Setpoint", "Setpoint", Setpoint, 40);
  WiFiManagerParameter custom_topic_statusSetpoint("statusSetpoint", "statusSetpoint", statusSetpoint, 40);
  WiFiManagerParameter custom_topic_State("State", "State", State, 40);
  WiFiManagerParameter custom_topic_statusState("statusState", "statusState", statusState, 40);
  WiFiManagerParameter custom_topic_statusRoomtemp("statusRoomtemp", "statusRoomtemp", statusRoomtemp, 40);
  WiFiManagerParameter custom_topic_Vanes("Vanes", "Vanes", Vanes, 40);
  WiFiManagerParameter custom_topic_statusVanes("statusVanes", "statusVanes", statusVanes, 40);
  WiFiManagerParameter custom_topic_Fanspeed("Fanspeed", "Fanspeed", Fanspeed, 40);
  WiFiManagerParameter custom_topic_statusFanspeed("statusFanspeed", "statusFanspeed", statusFanspeed, 40);
  WiFiManagerParameter custom_topic_debug("debug", "debug", debug, 40);
  WiFiManagerParameter custom_topic_service("service", "service", service, 40);

  //WiFiManager
  //Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;
  wifiManager.setDebugOutput(false);

  //Set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  //Optional: Set static ip
  //wifiManager.setSTAStaticIPConfig(IPAddress(10,0,1,99), IPAddress(10,0,1,1), IPAddress(255,255,255,0));

  //Add all parameters here
  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_mqtt_port);
  wifiManager.addParameter(&custom_mqtt_user);
  wifiManager.addParameter(&custom_mqtt_pass);
  wifiManager.addParameter(&custom_wifiTimeout);
  wifiManager.addParameter(&custom_topic_Room);
  wifiManager.addParameter(&custom_topic_Thing);
  wifiManager.addParameter(&custom_topic_Setpoint);
  wifiManager.addParameter(&custom_topic_statusSetpoint);
  wifiManager.addParameter(&custom_topic_State);
  wifiManager.addParameter(&custom_topic_statusState);
  wifiManager.addParameter(&custom_topic_statusRoomtemp);
  wifiManager.addParameter(&custom_topic_Vanes);
  wifiManager.addParameter(&custom_topic_statusVanes);
  wifiManager.addParameter(&custom_topic_Fanspeed);
  wifiManager.addParameter(&custom_topic_statusFanspeed);
  wifiManager.addParameter(&custom_topic_debug);
  wifiManager.addParameter(&custom_topic_service);

  //Set minimum quality of signal so it ignores AP's under that quality
  //Defaults to 8%
  wifiManager.setMinimumSignalQuality(5);

  //Sets timeout until configuration portal gets turned off
  //and retries connecting to the preconfigured AP
  wifiManager.setConfigPortalTimeout(atoi(wifiTimeout) * 60);                                                      //Convert minutes to seconds

  //Fetches ssid and pass and tries to connect
  //If it does not connect it starts an access point with the specified name
  //and goes into a blocking loop awaiting configuration
  if (!wifiManager.autoConnect(configSSID, configPW))
    {
      //Serial.println("Failed to connect and hit timeout");
      delay(3000);
      //Reset and try again, or maybe put it to deep sleep
      ESP.reset();
      delay(5000);
    }

  //If you get here you have connected to the WiFi
  //Serial.println("Connected...yeey :)");

  //Read updated parameters
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_port, custom_mqtt_port.getValue());
  strcpy(mqtt_user, custom_mqtt_user.getValue());
  strcpy(mqtt_pass, custom_mqtt_pass.getValue());
  strcpy(wifiTimeout, custom_wifiTimeout.getValue());
  strcpy(Room, custom_topic_Room.getValue());
  strcpy(Thing, custom_topic_Thing.getValue());

  //Construct topic names
  //Topic prefix = "Room/Thing/"
  char topic_prefix[42] = "";
  strncpy(topic_prefix, Room, 20);
  strcat (topic_prefix, "/");
  strncat(topic_prefix, Thing, 20);
  strcat (topic_prefix, "/");

  //Start all topics with topic prefix
  strcpy(Setpoint, topic_prefix);
  strcpy(statusSetpoint, topic_prefix);
  strcpy(State, topic_prefix);
  strcpy(statusState, topic_prefix);
  strcpy(statusRoomtemp, topic_prefix);
  strcpy(Vanes, topic_prefix);
  strcpy(statusVanes, topic_prefix);
  strcpy(Fanspeed, topic_prefix);
  strcpy(statusFanspeed, topic_prefix);
  strcpy(debug, topic_prefix);
  strcpy(service, topic_prefix);

  //Append final topic level
  size_t maxAppend = 60 - sizeof(topic_prefix);
  strncat(Setpoint, custom_topic_Setpoint.getValue(), maxAppend);
  strncat(statusSetpoint, custom_topic_statusSetpoint.getValue(), maxAppend);
  strncat(State, custom_topic_State.getValue(), maxAppend);
  strncat(statusState, custom_topic_statusState.getValue(), maxAppend);
  strncat(statusRoomtemp, custom_topic_statusRoomtemp.getValue(), maxAppend);
  strncat(Vanes, custom_topic_Vanes.getValue(), maxAppend);
  strncat(statusVanes, custom_topic_statusVanes.getValue(), maxAppend);
  strncat(Fanspeed, custom_topic_Fanspeed.getValue(), maxAppend);
  strncat(statusFanspeed, custom_topic_statusFanspeed.getValue(), maxAppend);
  strncat(debug, custom_topic_debug.getValue(), maxAppend);
  strncat(service, custom_topic_service.getValue(), maxAppend);

  //Debug resulting topics to serial
/*  Serial.println("Constructed topics:");
  Serial.println(Setpoint);
  Serial.println(statusSetpoint);
  Serial.println(State);
  Serial.println(statusState);
  Serial.println(statusRoomtemp);
  Serial.println(Vanes);
  Serial.println(statusVanes);
  Serial.println(Fanspeed);
  Serial.println(statusFanspeed);
  Serial.println(debug);
  Serial.println(service);
*/
  //Save the custom parameters to FS
  if (shouldSaveConfig)
    {
      //Serial.println("Saving config...");
      DynamicJsonBuffer jsonBuffer;
      JsonObject& json = jsonBuffer.createObject();
      json["mqtt_server"] = mqtt_server;
      json["mqtt_port"] = mqtt_port;
      json["mqtt_user"] = mqtt_user;
      json["mqtt_pass"] = mqtt_pass;
      json["wifiTimeout"] = wifiTimeout;
      json["Room"] = Room;
      json["Thing"] = Thing;
      json["Setpoint"] = custom_topic_Setpoint.getValue();
      json["statusSetpoint"] = custom_topic_statusSetpoint.getValue();
      json["State"] = custom_topic_State.getValue();
      json["statusState"] = custom_topic_statusState.getValue();
      json["statusRoomtemp"] = custom_topic_statusRoomtemp.getValue();
      json["Vanes"] = custom_topic_Vanes.getValue();
      json["statusVanes"] = custom_topic_statusVanes.getValue();
      json["Fanspeed"] = custom_topic_Fanspeed.getValue();
      json["statusFanspeed"] = custom_topic_statusFanspeed.getValue();
      json["debug"] = custom_topic_debug.getValue();
      json["service"] = custom_topic_service.getValue();

      File configFile = SPIFFS.open("/config.json", "w");
/*      if (!configFile)
        {
          Serial.println("Failed to open config file for writing");
        }
*/
      //json.printTo(Serial);
      json.printTo(configFile);
      configFile.close();
    }

  //Serial.println();
  //Serial.print("Local IP adres: ");
  //Serial.println(WiFi.localIP());

  //Connect to MQTT broker and set callback
  client.setServer(mqtt_server, atoi(mqtt_port));
  client.setCallback(callback);

  //Configure Arduino OTA updater
  ArduinoOTA.setHostname(configSSID);                                                                              //Set OTA hostname and password (same as local access point for WiFiManager)
  ArduinoOTA.setPassword((const char *)configPW);
  //ArduinoOTA.setPort(3232);                                                                                      //Port defaults to 3232

  ArduinoOTA.onError([](ota_error_t error)                                                                         //Send OTA error messages to MQTT debug topic
    {
      if      (error == OTA_AUTH_ERROR)      client.publish(debug, "<OTA> ERROR -> Auth failed", true);
      else if (error == OTA_BEGIN_ERROR)     client.publish(debug, "<OTA> ERROR -> Begin failed", true);
      else if (error == OTA_CONNECT_ERROR)   client.publish(debug, "<OTA> ERROR -> Connect failed", true);
      else if (error == OTA_RECEIVE_ERROR)   client.publish(debug, "<OTA> ERROR -> Receive failed", true);
      else if (error == OTA_END_ERROR)       client.publish(debug, "<OTA> ERROR -> End failed", true);
    });

  ArduinoOTA.begin();

  Serial.begin (500000);
  while(Serial.available()) Serial.read();                                                                         //Empty serial read buffer from possible junk send by ESP during boot

  //wdt_enable(WDTO_8S);                                                                                           //Start Watchdog Timer (WDT) to detect hang ups

  ETin.begin(details(fromESP), &Serial);                                                                           //Start the EasyTransfer library, pass in the data details and the name of the serial port
  ETout.begin(details(toESP), &Serial);

  pinMode(SCK, INPUT);
  pinMode(MISO, OUTPUT);
  SPCR = (0<<SPIE)|(0<<SPE)|(1<<DORD)|(0<<MSTR)|(1<<CPOL)|(1<<CPHA);                                               //Configurate SPI slave: no interrupts (yet) | disable SPI hardware | LSB first | slave mode | SCK high when idle | data valid on falling edge

  toESP.currentMHI[7] = 0;                                                                                         //Reset checksum/resync errors

  SPI_sync();                                                                                                      //Sync SPI to master (MOSI)


}

void connect()
{
  // Loop until we're (re)connected
  while (!client.connected())
    {
      //Serial.print("Attempting MQTT connection...");

      //MQTT connection: Attempt to connect to MQTT broker 3 times: SUCCESS -> continue | FAILED restart ESP
      //On restart it will first try to connect to the previously set AP. If that fails the config portal will be started.
      //If the config portal is not used within wifiTimeout (set in portal), the ESP will retry connecting to the previously set AP again.
      if (client.connect(configSSID, mqtt_user, mqtt_pass))
        {
          //Serial.println("connected!");

          //Subscribe to topics that control the MHI state
          client.subscribe(Setpoint, 1);
          client.subscribe(State, 1);
          client.subscribe(Vanes, 1);
          client.subscribe(Fanspeed, 1);
          client.subscribe(service, 1);

          char msg[62] = "MHI2MQTT connected to MQTT broker at ";
          strncat(msg, mqtt_server, 15);
          strcat(msg, ":");
          strncat(msg, mqtt_port, 8);
          client.publish(debug, msg , true);                                                                       //Publish message to debug topic to test/notify MQTT connection

          connectionFails = 0;

          Serial.begin(500000);
          while(Serial.available()) Serial.read();                                                                 //Empty serial read buffer. Arduino keeps sending updates over serial during wifi configuration and connecting MQTT broker.
        }
      else
        {
/*        Serial.print("failed, rc = ");
          Serial.println(client.state());
          Serial.print("Failed connection attempts: ");
          Serial.println(connectionFails); */

          if (++connectionFails == 3)
            {
              //Serial.println("MQTT broker connection timeout...restarting!");
              delay(1000);
              ESP.restart();
              delay(5000);
              break;
            }
          //Serial.println("Try again in 5 seconds...");
          delay(5000);
        }
    }
}

void callback(char* topic, byte* payload, unsigned int length)
{
  //Serial.print("Message arrived on topic [");
  //Serial.print(topic);
  //Serial.print("]: ");

  char buffer[length + 1];

  for (int i = 0; i < length; i++)                                                                                 //Copy payload to buffer string
    {
      buffer[i] = (char)payload[i];
      //Serial.print((char)payload[i]);
    }

  buffer[length] = '\0';                                                                                           //Terminate string

  //Serial.println();

  //SERVICE COMMANDS
  if (strcmp(topic, service) == 0)
    {
      if (strcmp(buffer, "reboot") == 0)
        {
          toArduino.mode     = 64;                                                                                 //Send code to restart Arduino
          toArduino.vanes    = 0;
          toArduino.fanspeed = 0;
          toArduino.setpoint = 0;
          ETout.sendData();

          client.publish(debug, " << Rebooting... >>", true);
          delay(2000);
0          ESP.restart();                                                                                           //Now restart ESP
          delay(5000);
          return;
        }

      if (strcmp(buffer, "reinit") == 0)                                                                           //Start WiFiManager after erasing previously stored settings
        {
          client.publish(debug, " << Reinitializing...(erase flash, reboot and start WiFiManager) >>", true);
          delay(500);
          SPIFFS.format();                                                                                         //Erase flash
          delay(1000);
          WiFi.disconnect();                                                                                       //Start WiFiManager  for reconfiguration
          delay(1000);
          ESP.restart();
          delay(5000);
          return;
        }

      if (strcmp(buffer, "wifimanager") == 0)                                                                      //Start WiFiManager with previously stored settings
        {
          client.publish(debug, " << Starting WiFiManager... >>", true);
          delay(500);
          WiFi.disconnect();
          delay(1000);
          ESP.restart();
          delay(5000);
          return;
        }

      if (strcmp(buffer, "debugon") == 0)                                                                          //Send some info on debug topic, see debug2mqtt() in the code
        {
          client.publish(debug, " << Debug ON >>", true);
          debugit = true;
          return;
        }


      if (strcmp(buffer, "debugoff") == 0)
        {
          client.publish(debug, " << Debug OFF >>", true);
          debugit = false;
          return;
        }

      if (strcmp(buffer, "help") == 0)
        {
          client.publish(debug, "<reboot> -> restart Arduino & ESP8266 | <reinit> -> erase flash and start WiFiManager", true);
          client.publish(debug, "<wifimanager> -> Start WiFiManager | <debugon><debugoff> -> Show some info on debug topic", true);
          return;
        }

      client.publish(debug, " << Unknown service command >>", true);

      return;
    }

  int value = atoi(buffer);                                                                                        //Convert payload to integer variable

  //POWER & MODE: payload = 0 [OFF], 1 [HEAT], 2 [COOL], 3 [AUTO], 4 [DRY], 5 [FAN], 6 [ON]
  if (strcmp(topic, State) == 0)
    {
      if (value >= 0 && value < 7)
        {
          toArduino.mode = value + 1;
          ETout.sendData();                                                                                        //Send updated settings to Arduino using EasyTransfer

          //Serial.print("Mode change: ");
          //Serial.println(toArduino.mode);

          toArduino.mode = 0;
          debug2mqtt("<ESP> Updated power/mode settings send to Arduino/MHI.");
        }
      else
        {
          debug2mqtt(" << Error >> Value on MODE topic out of range [0-6]");
        }

      return;
    }

  //VANES: payload = 1 [1-UP], 2 [2], 3 [3], 4 [4-DOWN], 5 [SWING]
  if (strcmp(topic, Vanes) == 0)
    {
      if (value > 0 && value < 6)
        {
          toArduino.vanes = value;
          ETout.sendData();                                                                                        //Send updated settings to Arduino using EasyTransfer

          //Serial.print("Vanes change: ");
          //Serial.println(toArduino.vanes);

          toArduino.vanes = 0;
          debug2mqtt("<ESP> Updated vanes settings send to Arduino/MHI.");
        }
      else
        {
          debug2mqtt(" << Error >> Value on VANES topic out of range [1-5]");
        }

      return;
    }

  //FAN SPEED: payload = 1 [1-LOW], 2 [2], 3 [3], 4 [4-HIGH]
  if (strcmp(topic, Fanspeed) == 0)
    {
      if (value > 0 && value < 5)
        {
          toArduino.fanspeed = value;
          ETout.sendData();                                                                                        //Send updated settings to Arduino using EasyTransfer

          //Serial.print("Fan speed change: ");
          //Serial.println(toArduino.fanspeed);

          toArduino.fanspeed = 0;
          debug2mqtt("<ESP> Updated fan speed settings send to Arduino/MHI.");
        }
      else
        {
          debug2mqtt(" << Error >> Value on FAN SPEED topic out of range [1-4]");
        }

      return;
    }


  //TEMPERATURE SETPOINT: payload = temperature in degrees Celsius
  if (strcmp(topic, Setpoint) == 0)
    {
      if (value > 17 && value < 31)
        {
          toArduino.setpoint   = value;                                                                            //Bitfield containing target temperature
          ETout.sendData();                                                                                        //Send updated settings to Arduino using EasyTransfer

          //Serial.print("Setpoint change: ");
          //Serial.println(toArduino.setpoint);

          toArduino.setpoint = 0;
          debug2mqtt("<ESP> Updated setpoint settings send to Arduino/MHI.");
        }
      else
        {
          debug2mqtt(" << Error >> Value on SETPOINT topic out of range [18-30]");
        }

      return;
    }

}

void loop()
{
  ArduinoOTA.handle();                                                                                             //Handle Arduino OTA updates

  if (!client.connected())                                                                                         //Check MQTT connection
    {
      connect();                                                                                                   //Connect first time. Reconnect when connection lost.
    }

  client.loop();

  if (ETin.receiveData())                                                                                          //Check for new data on serial (EasyTransfer). Returns false or true.
    {
      delay(1);                                                                                                    //Make sure receive is complete. I've had occasional problems and this appears to solve them.

      if (debugit)                                                                                                 //If debug = on -> send part of SPI byte frame to MQTT debug topic
        {
          char buffer[54] = "<MHI> Updated bit field 4-10:  ";
          int loc = 31;

          for (int i = 0; i < 7; i++)
            {
              snprintf(buffer + loc, 53 - loc , "%02X ", fromArduino.currentMHI[i]);
              loc += 3;
            }

          client.publish(debug, buffer, true);                                                                     //Send latest MHI bit field update to MQTT broker

          snprintf(buffer, 53, "<MHI> %d SPI resync/checksum errors", fromArduino.currentMHI[7]);
          client.publish(debug, buffer, true);                                                                     //Send cumulative number of checksum errors on Arduino-SPI-MHI connection to MQTT
        }

      //Process MHI bit field 4-10 updates and only publish changes to corresponding MQTT statuses
      char msg[5] = "";                                                                                            //Buffer string holding payload to publish
      byte buf = 0;

      //####### Bit field 4 >>> POWER, MODE & VANES (swing only) #######
      if (fromArduino.currentMHI[0] != current_Bitfield4)                                                          //Any change compared to previous bit field 4?
        {
          debug2mqtt("<MHI> Bit field 4 changed");
          current_Bitfield4 = fromArduino.currentMHI[0];                                                           //Store new current bit field 4

          //POWER and/or MODE changed
          if ((current_Bitfield4 & 0b00011101) != current_Mode)                                                    //Extract mode bits (3-5) en power bit (1)
            {
              current_Mode = current_Bitfield4 & 0b00011101;

              //Get POWER and MODE states
              if (bitRead(current_Mode, 0) == 0)                                                                   //Power is OFF
                {
                  buf = 0;
                }
              else
                {
                  for (int i = 0; i < 5; i++)                                                                      //Power is ON -> get MODE
                    {
                      if (current_Mode == modeValues[i])
                        {
                          buf = i + 1;
                          break;
                        }
                    }
                }

              snprintf(msg, 2, "%1d", buf);
              client.publish(statusState, msg, true);                                                              //Send update to MQTT broker

              debug2mqtt("<MHI> Mode/Power changed");

/*            Serial.println("State (Mode/Power) changed");
              Serial.print("MQTT publish [");
              Serial.print(statusState);
              Serial.print("]: ");
              Serial.println(msg); */
            }

          //VANES changed to swing
          if (bitRead(current_Bitfield4, 6))                                                                       //Check if new vanes setting is swing
            {
              if (!current_Swing)                                                                                  //Check if changed compared to previous setting
                {
                  current_Swing = true;
                  client.publish(statusVanes, "5", true);                                                          //Send update to MQTT broker

                  debug2mqtt("<MHI> Vanes changed");

/*                Serial.println("Vanes changed");
                  Serial.print("MQTT publish [");
                  Serial.print(statusVanes);
                  Serial.print("]: ");
                  Serial.println("5"); */
                }
            }
          else if (current_Swing)                                                                                  //SWING changed from ON to OFF
            {
              current_Bitfield5 = -1;                                                                              //Force update of VANES below (bit field 5) by setting out-of-range values
              current_Vanes     = 255;
              current_Swing     = false;
            }
        }

      //####### Bit field 10 >>> FAN SPEED (setting 4 only) #######
      if (bitRead(fromArduino.currentMHI[6], 6))                                                                   //Check if new speed setting is 4
        {
          if (!current_Fanspeed4)                                                                                  //Check if changed compared to current setting
            {
              debug2mqtt("<MHI> Bit field 10 changed");

              current_Fanspeed4 = true;
              client.publish(statusFanspeed, "4", true);                                                           //Send update to MQTT broker

              debug2mqtt("<MHI> Fan speed changed");

/*            Serial.println("Fanspeed changed");
              Serial.print("MQTT publish [");
              Serial.print(statusFanspeed);
              Serial.print("]: ");
              Serial.println(msg); */
            }
        }
      else if (current_Fanspeed4)                                                                                  //FAN SPEED changed from 4 to 1-3
        {
          current_Bitfield5 = -1;                                                                                  //Force update of FANSPEED below (bit field 5) by setting out-of-range values
          current_Fanspeed  = 255;
          current_Fanspeed4 = false;
        }

      //####### Bit field 5 >>> VANES (position 1-4) & FAN SPEED (setting 1-3) #######
      if ((int)fromArduino.currentMHI[1] != current_Bitfield5)                                                     //Any change compared to previous bit field 5? Force update of VANES if SWING was just switched off. Same for FAN SPEED 4.
        {
          debug2mqtt("<MHI> Bit field 5 changed");
          current_Bitfield5 = (int)fromArduino.currentMHI[1];                                                      //New current bit field 5

          //VANES changed
          if ((current_Bitfield5 & 0b00110000) != current_Vanes && !current_Swing)                                 //Extract vanes bits (5-6)
            {
              current_Vanes = current_Bitfield5 & 0b00110000;

              //Get VANES position
              buf = (current_Vanes >> 4) + 1;                                                                      //Convert to vanes position [1-4]

              snprintf(msg, 2, "%1d", buf);
              client.publish(statusVanes, msg, true);                                                              //Send update to MQTT broker

              debug2mqtt("<MHI> Vanes changed");

/*            Serial.println("Vanes changed");
              Serial.print("MQTT publish [");
              Serial.print(statusVanes);
              Serial.print("]: ");
              Serial.println(msg); */
            }

          //FAN SPEED (setting 1-3) changed
          if ((current_Bitfield5 & 0b00000111) != current_Fanspeed && !current_Fanspeed4)                          //Extract fan speed bits (1-3)
            {
              current_Fanspeed = current_Bitfield5 & 0b00000111;

              //Get FAN SPEED
              buf = current_Fanspeed + 1;                                                                          //Convert to fan speed (1-3)

              snprintf(msg, 2, "%1d", buf);
              client.publish(statusFanspeed, msg, true);                                                           //Send update to MQTT broker

              debug2mqtt("<MHI> Fan speed changed");

/*            Serial.println("Fanspeed changed");
              Serial.print("MQTT publish [");
              Serial.print(statusFanspeed);
              Serial.print("]: ");
              Serial.println(msg); */
            }
        }

      //####### Bit field 6 >>> TEMPERATURE SETPOINT #######
      if (fromArduino.currentMHI[2] != current_Bitfield6)
        {
          debug2mqtt("<MHI> Bit field 6 changed");
          current_Bitfield6 = fromArduino.currentMHI[2];                                                           //Store new setpoint byte value for reference in next SPI frame update

          //Extract new temperature setpoint (bitfield 6)
          buf = current_Bitfield6;
          buf = bitClear(buf, 7) >> 1;                                                                             //Clear bit 8 and shift right 1 bit (= divide by 2) -> buf = temperature in degr. Celsius
          snprintf(msg, 3, "%2d", buf);

          client.publish(statusSetpoint, msg, true);                                                               //Send update to MQTT broker

          debug2mqtt("<MHI> Setpoint changed");

/*        Serial.println("Setpoint changed");
          Serial.print("MQTT publish [");
          Serial.print(statusSetpoint);
          Serial.print("]: ");
          Serial.println(msg); */
        }

      //####### Bit field 7 >>> ROOM TEMPERATURE #######
      if (fromArduino.currentMHI[3] != current_Bitfield7)
        {
          debug2mqtt("<MHI> Bit field 7 changed");
          current_Bitfield7 = fromArduino.currentMHI[3];                                                           //Store new setpoint byte value for reference in next SPI frame update

          //Calculate current room temperature in degrees Celsius from bitfield 7 using:
          // (BF7 - 61) / 4 (note: Calibration of temperature needs to be checked further)
          int temp = (int)current_Bitfield7 - 61;
          int dec = (temp % 4) * 25;                                                                               //Decimal value (can be xx.00; xx.25; xx.50; xx.75)
          temp /= 4;                                                                                               //Truncated temperature (rounded down)

          snprintf(msg, 6, "%d.%02d", temp, dec);                                                                  //Construct temperature payload string

          client.publish(statusRoomtemp, msg, true);                                                               //Send update to MQTT broker

          debug2mqtt("<MHI> Room temperature changed");

/*        Serial.println("Roomtemp changed");
          Serial.print("MQTT publish [");
          Serial.print(statusRoomtemp);
          Serial.print("]: ");
          Serial.println(msg); */
        }
    }
}
