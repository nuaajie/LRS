#include "../../src/targets.h"
#include "CRSF.h"
#include "../../lib/FIFO/FIFO.h"
#include <Arduino.h>
#include "HardwareSerial.h"

#ifdef PLATFORM_ESP32
HardwareSerial SerialPort(1);
HardwareSerial CRSF::Port = SerialPort;

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

///Out FIFO to buffer messages///
FIFO SerialOutFIFO;
TaskHandle_t xHandleSerialOutFIFO = NULL;
#endif

uint8_t CRSF::CSFR_TXpin_Module = GPIO_PIN_RCSIGNAL_TX;
uint8_t CRSF::CSFR_RXpin_Module = GPIO_PIN_RCSIGNAL_RX; // Same pin for RX/TX

//U1RXD_IN_IDX
//U1TXD_OUT_IDX

volatile bool CRSF::ignoreSerialData = false;
volatile bool CRSF::CRSFframeActive = false; //since we get a copy of the serial data use this flag to know when to ignore it

void inline CRSF::nullCallback(void){};

void (*CRSF::RCdataCallback1)() = &nullCallback; // function is called whenever there is new RC data.
void (*CRSF::RCdataCallback2)() = &nullCallback; // function is called whenever there is new RC data.

void (*CRSF::disconnected)() = &nullCallback; // called when CRSF stream is lost
void (*CRSF::connected)() = &nullCallback;    // called when CRSF stream is regained

void (*CRSF::RecvParameterUpdate)() = &nullCallback; // called when recv parameter update req, ie from LUA

bool CRSF::firstboot = true;

bool CRSF::CRSFstate = false;

volatile uint8_t CRSF::SerialInPacketLen = 0;     // length of the CRSF packet as measured
volatile uint8_t CRSF::SerialInPacketPtr = 0;     // index where we are reading/writing
volatile uint8_t CRSF::SerialInBuffer[CRSF_MAX_PACKET_LEN] = {0}; // max 64 bytes for CRSF packet
volatile uint16_t CRSF::ChannelDataIn[16] = {0};
volatile uint16_t CRSF::ChannelDataInPrev[16] = {0};

volatile uint8_t CRSF::ParameterUpdateData[2] = {0};

volatile crsf_channels_s CRSF::PackedRCdataOut;
volatile crsfPayloadLinkstatistics_s CRSF::LinkStatistics;

volatile uint32_t CRSF::RCdataLastRecv = 0;
volatile int32_t CRSF::OpenTXsyncOffset = 0;
volatile uint32_t CRSF::RequestedRCpacketInterval = 4000; // default to 250hz as per 'normal'

//CRSF::CRSF(HardwareSerial &serial) : CRSF_SERIAL(serial){};

void CRSF::Begin()
{

#ifdef PLATFORM_ESP32
    //xTaskHandle UartTaskHandle = NULL;
    xTaskCreatePinnedToCore(ESP32uartTask, "ESP32uartTask", 20000, NULL, 255, NULL, 0);
#endif
    //The master module requires that the serial communication is bidirectional
    //The Reciever uses seperate rx and tx pins
}

#ifdef PLATFORM_ESP32
void ICACHE_RAM_ATTR CRSF::sendLinkStatisticsToTX()
{
    uint8_t outBuffer[LinkStatisticsFrameLength + 4] = {0};

    outBuffer[0] = CRSF_ADDRESS_RADIO_TRANSMITTER;
    outBuffer[1] = LinkStatisticsFrameLength + 2;
    outBuffer[2] = CRSF_FRAMETYPE_LINK_STATISTICS;

    memcpy(outBuffer + 3, (byte *)&LinkStatistics, LinkStatisticsFrameLength);

    uint8_t crc = CalcCRC(&outBuffer[2], LinkStatisticsFrameLength + 1);

    outBuffer[LinkStatisticsFrameLength + 3] = crc;

    //memcpy((uint8_t *)CRSFoutBuffer + 1, outBuffer, LinkStatisticsFrameLength + 4);
    //CRSFoutBuffer[0] = LinkStatisticsFrameLength + 4;

    SerialOutFIFO.push(LinkStatisticsFrameLength + 4); // length
    SerialOutFIFO.pushBytes(outBuffer, LinkStatisticsFrameLength + 4);
    //Serial.println(CRSFoutBuffer[0]);
}

void ICACHE_RAM_ATTR CRSF::JustSentRFpacket()
{
    CRSF::OpenTXsyncOffset = micros() - CRSF::RCdataLastRecv;
    //Serial.println(CRSF::OpenTXsyncOffset);
}

void ICACHE_RAM_ATTR CRSF::sendSyncPacketToTX(void *pvParameters) // in values in us.
{
    vTaskDelay(500);
    for (;;)
    {
        uint32_t packetRate = CRSF::RequestedRCpacketInterval * 10; //convert from us to right format
        int32_t offset = CRSF::OpenTXsyncOffset * 10 - 8000;        // + offset that seems to be needed
        uint8_t outBuffer[OpenTXsyncFrameLength + 4] = {0};

        outBuffer[0] = CRSF_ADDRESS_RADIO_TRANSMITTER; //0xEA
        outBuffer[1] = OpenTXsyncFrameLength + 2;      // equals 14?
        outBuffer[2] = CRSF_FRAMETYPE_RADIO_ID;        // 0x3A

        outBuffer[3] = CRSF_ADDRESS_RADIO_TRANSMITTER; //0XEA
        outBuffer[4] = 0x00;                           //??? not sure doesn't seem to matter
        outBuffer[5] = CRSF_FRAMETYPE_OPENTX_SYNC;     //0X10

        outBuffer[6] = (packetRate & 0xFF000000) >> 24;
        outBuffer[7] = (packetRate & 0x00FF0000) >> 16;
        outBuffer[8] = (packetRate & 0x0000FF00) >> 8;
        outBuffer[9] = (packetRate & 0x000000FF) >> 0;

        outBuffer[10] = (offset & 0xFF000000) >> 24;
        outBuffer[11] = (offset & 0x00FF0000) >> 16;
        outBuffer[12] = (offset & 0x0000FF00) >> 8;
        outBuffer[13] = (offset & 0x000000FF) >> 0;

        uint8_t crc = CalcCRC(&outBuffer[2], OpenTXsyncFrameLength + 1);

        outBuffer[OpenTXsyncFrameLength + 3] = crc;

        SerialOutFIFO.push(OpenTXsyncFrameLength + 4); // length
        SerialOutFIFO.pushBytes(outBuffer, OpenTXsyncFrameLength + 4);

        vTaskDelay(OpenTXsyncPakcetInterval);
    }
}

#endif

#if defined(PLATFORM_ESP8266) || defined(PLATFORM_STM32)
void ICACHE_RAM_ATTR CRSF::sendLinkStatisticsToFC()
{
    uint8_t outBuffer[LinkStatisticsFrameLength + 4] = {0};

    outBuffer[0] = CRSF_ADDRESS_FLIGHT_CONTROLLER;
    outBuffer[1] = LinkStatisticsFrameLength + 2;
    outBuffer[2] = CRSF_FRAMETYPE_LINK_STATISTICS;

    memcpy(outBuffer + 3, (byte *)&LinkStatistics, LinkStatisticsFrameLength);

    uint8_t crc = CalcCRC(&outBuffer[2], LinkStatisticsFrameLength + 1);

    outBuffer[LinkStatisticsFrameLength + 3] = crc;

    this->_dev->write(outBuffer, LinkStatisticsFrameLength + 4);
}

void ICACHE_RAM_ATTR CRSF::sendRCFrameToFC()
{
    uint8_t outBuffer[RCframeLength + 4] = {0};

    outBuffer[0] = CRSF_ADDRESS_FLIGHT_CONTROLLER;
    outBuffer[1] = RCframeLength + 2;
    outBuffer[2] = CRSF_FRAMETYPE_RC_CHANNELS_PACKED;

    memcpy(outBuffer + 3, (byte *)&PackedRCdataOut, RCframeLength);

    uint8_t crc = CalcCRC(&outBuffer[2], RCframeLength + 1);

    outBuffer[RCframeLength + 3] = crc;

    this->_dev->write(outBuffer, RCframeLength + 4);
    //this->_dev->print(".");
}

void ICACHE_RAM_ATTR CRSF::ESP8266ReadUart()
{
}
#endif

#ifdef PLATFORM_ESP32

void ICACHE_RAM_ATTR CRSF::duplex_set_RX()
{
    ESP_ERROR_CHECK(gpio_set_direction((gpio_num_t)GPIO_PIN_RCSIGNAL_RX, GPIO_MODE_INPUT));
    //ESP_ERROR_CHECK(gpio_set_pull_mode((gpio_num_t)GPIO_PIN_RCSIGNAL_RX, GPIO_FLOATING));
    //ESP_ERROR_CHECK(gpio_set_pull_mode(port->config.rx, port->config.inverted ? GPIO_PULLDOWN_ONLY : GPIO_PULLUP_ONLY));
    gpio_matrix_in((gpio_num_t)GPIO_PIN_RCSIGNAL_RX, U1RXD_IN_IDX, true);

    //CRSF::FlushSerial();
}
void ICACHE_RAM_ATTR CRSF::duplex_set_TX()
{
    gpio_matrix_in((gpio_num_t)-1, U1RXD_IN_IDX, false);
    ESP_ERROR_CHECK(gpio_set_pull_mode((gpio_num_t)GPIO_PIN_RCSIGNAL_TX, GPIO_FLOATING));
    ESP_ERROR_CHECK(gpio_set_pull_mode((gpio_num_t)GPIO_PIN_RCSIGNAL_RX, GPIO_FLOATING));
    ESP_ERROR_CHECK(gpio_set_level((gpio_num_t)GPIO_PIN_RCSIGNAL_TX, 0));
    ESP_ERROR_CHECK(gpio_set_direction((gpio_num_t)GPIO_PIN_RCSIGNAL_TX, GPIO_MODE_OUTPUT));
    gpio_matrix_out((gpio_num_t)GPIO_PIN_RCSIGNAL_TX, U1TXD_OUT_IDX, true, false);
}

void ICACHE_RAM_ATTR duplex_set_HIGHZ()
{
}

void ICACHE_RAM_ATTR CRSF::ESP32uartTask(void *pvParameters) //RTOS task to read and write CRSF packets to the serial port
{

    const TickType_t xDelay1 = 1 / portTICK_PERIOD_MS;
    CRSF::Port.begin(CRSF_OPENTX_BAUDRATE, SERIAL_8N1, CSFR_RXpin_Module, CSFR_TXpin_Module, false);
    //gpio_set_drive_capability((gpio_num_t)CSFR_TXpin_Module, GPIO_DRIVE_CAP_0);
    Serial.println("ESP32 CRSF UART LISTEN TASK STARTED");

    uint32_t LastDataTime = millis();
    uint32_t YieldInterval = 200;

    CRSF::duplex_set_RX();
    FlushSerial();

    for (;;)
    {
        if (millis() > (LastDataTime + YieldInterval))
        { //prevents WDT reset by yielding when there is no CRSF data to proces
            vTaskDelay(YieldInterval / portTICK_PERIOD_MS);
            if (CRSFstate == true)
            {
                CRSFstate = false;
                Serial.println("CRSF UART Lost");
                if (xHandleSerialOutFIFO != NULL)
                {
                    vTaskDelete(xHandleSerialOutFIFO);
                }
                SerialOutFIFO.flush();
                disconnected();
            }
        }

        if (CRSF::Port.available())
        {
            LastDataTime = millis();

            if (SerialInPacketPtr > CRSF_MAX_PACKET_LEN-1) // we reached the maximum allowable packet length, so start again because shit fucked up hey.
            {
                SerialInPacketPtr = 0;
            }

            char inChar = CRSF::Port.read();

            if ((inChar == CRSF_ADDRESS_CRSF_TRANSMITTER || CRSF_SYNC_BYTE) && CRSFframeActive == false) // we got sync, reset write pointer
            {
                SerialInPacketPtr = 0;
                CRSFframeActive = true;
            }

            if (SerialInPacketPtr == 1 && CRSFframeActive == true) // we read the packet length and save it
            {
                SerialInPacketLen = inChar;
            }

            SerialInBuffer[SerialInPacketPtr] = inChar;
            SerialInPacketPtr++;

            if (SerialInPacketPtr == SerialInPacketLen + 2) // plus 2 because the packlen is referenced from the start of the 'type' flag, IE there are an extra 2 bytes.
            {
                char CalculatedCRC = CalcCRC((uint8_t *)SerialInBuffer + 2, SerialInPacketPtr - 3);

                if (CalculatedCRC == inChar)
                {
                    ProcessPacket();
                    SerialInPacketPtr = 0;
                    CRSFframeActive = false;
                    //gpio_set_drive_capability((gpio_num_t)CSFR_TXpin_Module, GPIO_DRIVE_CAP_2);
                    //taskYIELD();
                    vTaskDelay(xDelay1);

                    uint8_t peekVal = SerialOutFIFO.peek(); // check if we have data in the output FIFO that needs to be written
                    if (peekVal > 0)
                    {
                        if (SerialOutFIFO.size() >= peekVal)
                        {
                            CRSF::duplex_set_TX();

                            uint8_t OutPktLen = SerialOutFIFO.pop();
                            uint8_t OutData[OutPktLen];

                            SerialOutFIFO.popBytes(OutData, OutPktLen);
                            CRSF::Port.write(OutData, OutPktLen); // write the packet out

                            vTaskDelay(xDelay1);
                            CRSF::duplex_set_RX();
                            FlushSerial(); // we don't need to read back the data we just wrote
                        }
                    }
                    vTaskDelay(xDelay1);
                    //gpio_set_drive_capability((gpio_num_t)CSFR_TXpin_Module, GPIO_DRIVE_CAP_0);
                }
                else
                {
                    Serial.println("CRC failure");
                    Serial.println(SerialInPacketPtr, HEX);
                    Serial.print("Expected: ");
                    Serial.println(CalculatedCRC, HEX);
                    Serial.print("Got: ");
                    Serial.println(inChar, HEX);
                    for (int i = 0; i < SerialInPacketLen + 2; i++)
                    {
                        Serial.print(SerialInBuffer[i], HEX);
                        Serial.print(" ");
                    }
                    Serial.println();
                    Serial.println();
                    CRSFframeActive = false;
                    SerialInPacketPtr = 0;
                    FlushSerial();
                    vTaskDelay(xDelay1);
                }
            }
        }
    }
}
#endif

#ifdef PLATFORM_ESP32

void ICACHE_RAM_ATTR CRSF::ProcessPacket()
{
    if (CRSFstate == false)
    {
        CRSFstate = true;
        Serial.println("CRSF UART Connected");
#ifdef FEATURE_OPENTX_SYNC
        xTaskCreatePinnedToCore(sendSyncPacketToTX, "sendSyncPacketToTX", 2000, NULL, 254, &xHandleSerialOutFIFO, 1);
#endif
        connected();
    }
    //portENTER_CRITICAL(&mux);
    if (CRSF::SerialInBuffer[2] == CRSF_FRAMETYPE_PARAMETER_WRITE)
    {
        Serial.println("Got Other Packet");
        if (SerialInBuffer[3] == CRSF_ADDRESS_CRSF_TRANSMITTER && SerialInBuffer[4] == CRSF_ADDRESS_RADIO_TRANSMITTER)
        {
            ParameterUpdateData[0] = SerialInBuffer[5];
            ParameterUpdateData[1] = SerialInBuffer[6];
            RecvParameterUpdate();
        }
    }

    if (CRSF::SerialInBuffer[2] == CRSF_FRAMETYPE_RC_CHANNELS_PACKED)
    {
        CRSF::RCdataLastRecv = micros();
        GetChannelDataIn();
        (RCdataCallback1)(); // run new RC data callback
        (RCdataCallback2)(); // run new RC data callback
    }
    //portEXIT_CRITICAL(&mux);
    //vTaskDelay(2);
}

#endif

void ICACHE_RAM_ATTR CRSF::GetChannelDataIn() // data is packed as 11 bits per channel
{
#define SERIAL_PACKET_OFFSET 3

    memcpy((uint16_t *)ChannelDataInPrev, (uint16_t *)ChannelDataIn, 16); //before we write the new RC channel data copy the old data

    const crsf_channels_t *const rcChannels = (crsf_channels_t *)&CRSF::SerialInBuffer[SERIAL_PACKET_OFFSET];
    ChannelDataIn[0] = (rcChannels->ch0);
    ChannelDataIn[1] = (rcChannels->ch1);
    ChannelDataIn[2] = (rcChannels->ch2);
    ChannelDataIn[3] = (rcChannels->ch3);
    ChannelDataIn[4] = (rcChannels->ch4);
    ChannelDataIn[5] = (rcChannels->ch5);
    ChannelDataIn[6] = (rcChannels->ch6);
    ChannelDataIn[7] = (rcChannels->ch7);
    ChannelDataIn[8] = (rcChannels->ch8);
    ChannelDataIn[9] = (rcChannels->ch9);
    ChannelDataIn[10] = (rcChannels->ch10);
    ChannelDataIn[11] = (rcChannels->ch11);
    ChannelDataIn[12] = (rcChannels->ch12);
    ChannelDataIn[13] = (rcChannels->ch13);
    ChannelDataIn[14] = (rcChannels->ch14);
    ChannelDataIn[15] = (rcChannels->ch15);
}

void ICACHE_RAM_ATTR CRSF::FlushSerial()
{
    CRSF::Port.flush();
}
