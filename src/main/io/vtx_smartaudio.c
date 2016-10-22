#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>

#include "platform.h"

#ifdef VTX_SMARTAUDIO

#include "common/printf.h"
#include "drivers/system.h"
#include "drivers/serial.h"
#include "io/serial.h"
#include "io/vtx_smartaudio.h"

#include "fc/rc_controls.h"
#include "fc/runtime_config.h"

#include "flight/pid.h"
#include "config/config.h"
#include "config/config_eeprom.h"
#include "config/config_profile.h"
#include "config/config_master.h"

#include "build/build_config.h"

#define SMARTAUDIO_EXTENDED_API

#define SMARTAUDIO_DPRINTF
//#define SMARTAUDIO_DEBUG_MONITOR

#ifdef SMARTAUDIO_DPRINTF
#define DPRINTF_SERIAL_PORT SERIAL_PORT_USART3
serialPort_t *debugSerialPort = NULL;
#define dprintf(x) if (debugSerialPort) printf x
#else
#define dprintf(x)
#endif

#include "build/debug.h"

#ifdef OSD
static void smartAudioUpdateStatusString(void); // Forward
#endif

static serialPort_t *smartAudioSerialPort = NULL;

// SmartAudio command and response codes
enum {
    SA_CMD_NONE = 0x00,
    SA_CMD_GET_SETTINGS = 0x01,
    SA_CMD_SET_POWER,
    SA_CMD_SET_CHAN,
    SA_CMD_SET_FREQ,
    SA_CMD_SET_MODE,
    SA_CMD_GET_SETTINGS_V2 = 0x09        // Response only
} smartAudioCommand_e;

#define SACMD(cmd) (((cmd) << 1) | 1)

// opmode flags, GET side
#define SA_MODE_GET_FREQ_BY_FREQ            1
#define SA_MODE_GET_PITMODE                 2
#define SA_MODE_GET_IN_RANGE_PITMODE        4
#define SA_MODE_GET_OUT_RANGE_PITMODE       8
#define SA_MODE_GET_UNLOCK                 16

// opmode flags, SET side
#define SA_MODE_SET_IN_RANGE_PITMODE        1
#define SA_MODE_SET_OUT_RANGE_PITMODE	    2
#define SA_MODE_SET_PITMODE                 4
#define SA_MODE_CLR_PITMODE                 4
#define SA_MODE_SET_UNLOCK                  8
#define SA_MODE_SET_LOCK                    0 // ~UNLOCK

// SetFrequency flags, for pit mode frequency manipulation
#define SA_FREQ_GETPIT                      (1 << 14)
#define SA_FREQ_SETPIT                      (1 << 15)

// Error counters, may be good for post production debugging.
uint16_t saerr_badpre = 0;
uint16_t saerr_badlen = 0;
uint16_t saerr_crc = 0;
uint16_t saerr_oooresp = 0;

// Receive frame reassembly buffer
#define SA_MAX_RCVLEN 11
static uint8_t sa_rbuf[SA_MAX_RCVLEN+4]; // XXX delete 4 byte guard

// CRC8 computations

#define POLYGEN 0xd5

static uint8_t CRC8(uint8_t *data, int8_t len)
{
    uint8_t crc = 0; /* start with 0 so first byte can be 'xored' in */
    uint8_t currByte;

    for (int i = 0 ; i < len ; i++) {
        currByte = data[i];

        crc ^= currByte; /* XOR-in the next input byte */

        for (int i = 0; i < 8; i++) {
            if ((crc & 0x80) != 0) {
                crc = (uint8_t)((crc << 1) ^ POLYGEN);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

// The band/chan to frequency table
// XXX Should really be consolidated among different vtx drivers
static const uint16_t saFreqTable[5][8] =
{
    { 5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725 }, // Boacam A
    { 5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866 }, // Boscam B
    { 5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945 }, // Boscam E
    { 5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880 }, // FatShark
    { 5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917 }, // RaceBand
};

typedef struct saPowerTable_s {
    int rfpower;
    int16_t valueV1;
    int16_t valueV2;
} saPowerTable_t;

static saPowerTable_t saPowerTable[] = {
    {  25,   7,   0 },
    { 200,  16,   1 },
    { 500,  25,   2 },
    { 800,  40,   3 },
};

// Driver defined modes

#define SA_OPMODEL_FREE         0 // Power up transmitting
#define SA_OPMODEL_PIT          1 // Power up in pit mode

#define SA_TXMODE_NODEF         0
#define SA_TXMODE_PIT_OUTRANGE  1
#define SA_TXMODE_PIT_INRANGE   2
#define SA_TXMODE_ACTIVE        3

// Last received device ('hard') states

static int8_t sa_vers = 0; // Will be set to 1 or 2
static int8_t sa_chan = -1;
static int8_t sa_power = -1;
static int8_t sa_opmode = -1;
static uint16_t sa_freq = 0;

static int8_t sa_overs = 0;
static int8_t sa_ochan;
static int8_t sa_opower;
static int8_t sa_oopmode;
static uint16_t sa_ofreq;

static uint16_t sa_pitfreq = 0;

static void smartAudioPrintSettings(void)
{
#ifdef SMARTAUDIO_DPRINTF


    dprintf(("Settings:\r\n"));
    dprintf(("  version: %d\r\n", sa_vers));
    dprintf(("     mode(0x%x): vtx=%s", sa_opmode,  (sa_opmode & 1) ? "freq" : "chan"));
    dprintf((" pit=%s ", (sa_opmode & 2) ? "on " : "off"));
    dprintf((" inb=%s", (sa_opmode & 4) ? "on " : "off"));
    dprintf((" outb=%s", (sa_opmode & 8) ? "on " : "off"));
    dprintf((" lock=%s\r\n", (sa_opmode & 16) ? "unlocked" : "locked"));
    dprintf(("     chan: %d\r\n", sa_chan));
    dprintf(("     freq: %d\r\n", sa_freq));
    dprintf(("    power: %d\r\n", sa_power));
    dprintf(("\r\n"));

#endif
}

static int saDacToPowerIndex(int dac)
{
    int idx;

    for (idx = 0 ; idx < 4 ; idx++) {
        if (saPowerTable[idx].valueV1 <= dac)
            return(idx);
    }
    return(3);
}

// Autobauding

#define SMARTBAUD_MIN 4800
#define SMARTBAUD_MAX 4950
uint16_t smartAudioSmartbaud = SMARTBAUD_MIN;
static int adjdir = 1; // -1=going down, 1=going up
static int baudstep = 50;

#define SMARTAUDIO_CMD_TIMEOUT    120

// Statistics for autobauding

static int sa_pktsent = 0;
static int sa_pktrcvd = 0;

static void saAutobaud(void)
{
    if (sa_pktsent < 10)
        // Not enough samples collected
        return;

#if 0
    dprintf(("autobaud: %d rcvd %d/%d (%d)\r\n",
        smartAudioSmartbaud, sa_pktrcvd, sa_pktsent, ((sa_pktrcvd * 100) / sa_pktsent)));
#endif

    if (((sa_pktrcvd * 100) / sa_pktsent) >= 70) {
        // This is okay
        sa_pktsent = 0; // Should be more moderate?
        sa_pktrcvd = 0;
        return;
    }

    dprintf(("autobaud: adjusting\r\n"));

    if ((adjdir == 1) && (smartAudioSmartbaud == SMARTBAUD_MAX)) {
       adjdir = -1;
       dprintf(("autobaud: now going down\r\n"));
    } else if ((adjdir == -1 && smartAudioSmartbaud == SMARTBAUD_MIN)) {
       adjdir = 1;
       dprintf(("autobaud: now going up\r\n"));
    }

    smartAudioSmartbaud += baudstep * adjdir;

    dprintf(("autobaud: %d\r\n", smartAudioSmartbaud));

    smartAudioSerialPort->vTable->serialSetBaudRate(smartAudioSerialPort, smartAudioSmartbaud);

    sa_pktsent = 0;
    sa_pktrcvd = 0;
}

// Transport level protocol variables

static uint32_t sa_lastTransmission = 0;
static uint8_t sa_outstanding = SA_CMD_NONE; // Outstanding command
static uint8_t sa_osbuf[32]; // Outstanding comamnd frame for retransmission
static int sa_oslen;         // And associate length

static void saProcessResponse(uint8_t *buf, int len)
{
    uint8_t resp = buf[0];

    if (resp == sa_outstanding) {
        sa_outstanding = SA_CMD_NONE;
    } else if ((resp == SA_CMD_GET_SETTINGS_V2) && (sa_outstanding == SA_CMD_GET_SETTINGS)) {
        sa_outstanding = SA_CMD_NONE;
    } else {
        saerr_oooresp++;
        dprintf(("processResponse: outstanding %d got %d\r\n", sa_outstanding, resp));
    }

    switch(resp) {
    case SA_CMD_GET_SETTINGS_V2: // Version 2 Get Settings
    case SA_CMD_GET_SETTINGS: // Version 1 Get Settings
        if (len < 7)
            break;

        sa_vers = (buf[0] == SA_CMD_GET_SETTINGS) ? 1 : 2;
        sa_chan = buf[2];
        sa_power = buf[3];
        sa_opmode = buf[4];
        sa_freq = (buf[5] << 8)|buf[6];

        if ((sa_overs == sa_vers)
                && (sa_ochan == sa_chan)
                && (sa_opower == sa_power)
                && (sa_oopmode == sa_opmode)
                && (sa_ofreq == sa_freq))
            break;

        // Debug
        smartAudioPrintSettings();

        // Export current settings for BFOSD 3.0.0

        smartAudioBand = (sa_chan / 8) + 1;
        smartAudioChan = (sa_chan % 8) + 1;
        smartAudioFreq = saFreqTable[sa_chan / 8][sa_chan % 8];

        if ((sa_opmode & SA_MODE_GET_PITMODE) == 0) {
            smartAudioTxMode = SA_TXMODE_ACTIVE;
        } else if (sa_opmode & SA_MODE_GET_IN_RANGE_PITMODE) {
            smartAudioTxMode = SA_TXMODE_PIT_INRANGE;
        } else {
            smartAudioTxMode = SA_TXMODE_PIT_OUTRANGE;
        }

        smartAudioUpdateStatusString();

        if (sa_vers == 2) {
            smartAudioPower = sa_power + 1; // XXX Take care V1
        } else {
            smartAudioPower = saDacToPowerIndex(sa_power) + 1;
        }

#ifdef SMARTAUDIO_DEBUG_MONITOR
        debug[0] = sa_vers * 100 + sa_opmode;
        debug[1] = sa_chan;
        debug[2] = sa_freq;
        debug[3] = sa_power;
#endif
        sa_overs = sa_vers;
        sa_ochan = sa_chan;
        sa_opower = sa_power;
        sa_oopmode = sa_opmode;
        sa_ofreq = sa_freq;

        break;

    case SA_CMD_SET_POWER: // Set Power
        break;

    case SA_CMD_SET_CHAN: // Set Channel
        break;

    case SA_CMD_SET_FREQ: // Set Frequency
        if (len < 5)
            break;

        uint16_t freq = (buf[2] << 8)|buf[3];

        if (freq & SA_FREQ_GETPIT) {
            sa_pitfreq = freq & ~SA_FREQ_GETPIT;
            dprintf(("saProcessResponse: GETPIT freq %d\r\n", sa_pitfreq));
            smartAudioUpdateStatusString();
        } else if (freq & SA_FREQ_SETPIT) {
            dprintf(("saProcessResponse: SETPIT freq %d\r\n", freq));
        } else {
            dprintf(("saProcessResponse: GETFREQ freq %d\r\n", freq));
        }
        break;

    case SA_CMD_SET_MODE: // Set Mode
        dprintf(("resp SET_MODE 0x%x\r\n", buf[2]));
        break;
    }
}

static void saReceiveFramer(uint8_t c)
{

    static enum saFramerState_e {
        S_WAITPRE1, // Waiting for preamble 1 (0xAA)
        S_WAITPRE2, // Waiting for preamble 2 (0x55)
        S_WAITRESP, // Waiting for response code
        S_WAITLEN,  // Waiting for length
        S_DATA,     // Receiving data
        S_WAITCRC,  // Waiting for CRC
    } state = S_WAITPRE1;

    static int len;
    static int dlen;

    switch(state) {
    case S_WAITPRE1:
        if (c == 0xAA) {
            state = S_WAITPRE2;
        } else {
            state = S_WAITPRE1; // Don't need this
        }
        break;

    case S_WAITPRE2:
        if (c == 0x55) {
            state = S_WAITRESP;
        } else {
            saerr_badpre++;
            state = S_WAITPRE1;
        }
        break;

    case S_WAITRESP:
        sa_rbuf[0] = c;
        state = S_WAITLEN;
        break;

    case S_WAITLEN:
        sa_rbuf[1] = c;
        len = c;

        if (len > SA_MAX_RCVLEN - 2) {
            saerr_badlen++;
            state = S_WAITPRE1;
        } else if (len == 0) {
            state = S_WAITCRC;
        } else {
            dlen = 0;
            state = S_DATA;
        }
        break;

    case S_DATA:
        sa_rbuf[2 + dlen] = c;
        if (++dlen == len) {
            state = S_WAITCRC;
        }
        break;

    case S_WAITCRC:
        if (CRC8(sa_rbuf, 2 + len) == c) {
            // Got a response
            saProcessResponse(sa_rbuf, len + 2);
            sa_pktrcvd++;
        } else if (sa_rbuf[0] & 1) {
            // Command echo (may be)
        } else {
            saerr_crc++;
        }
        state = S_WAITPRE1;
        break;
    }
}

// Output framer

static void saSendFrame(uint8_t *buf, int len)
{
    int i;

    serialWrite(smartAudioSerialPort, 0x00); // Generate 1st start bit

    for (i = 0 ; i < len ; i++)
        serialWrite(smartAudioSerialPort, buf[i]);

    serialWrite(smartAudioSerialPort, 0x00); // XXX Probably don't need this

    sa_lastTransmission = millis();
    sa_pktsent++;
}


/*
 * Retransmission and command queuing
 *
 *   The transport level support includes retransmission on response timeout
 * and command queueing.
 *
 * Resend buffer:
 *   The smartaudio returns response for valid command frames in no less
 * than 60msec, which we can't wait. So there's a need for a resend buffer.
 *
 * Command queueing:
 *   The driver autonomously sends GetSettings command for auto-bauding,
 * asynchronous to user initiated commands; commands issued while another
 * command is outstanding must be queued for later processing.
 *   The queueing also handles the case in which multiple commands are
 * required to implement a user level command.
 */

// Retransmission

static void saResendCmd(void)
{
    saSendFrame(sa_osbuf, sa_oslen);
}

static void saSendCmd(uint8_t *buf, int len)
{
    int i;

    for (i = 0 ; i < len ; i++)
        sa_osbuf[i] = buf[i];

    sa_oslen = len;
    sa_outstanding = (buf[2] >> 1);

    saSendFrame(sa_osbuf, sa_oslen);
}

// Command queue management

typedef struct saCmdQueue_s {
    uint8_t *buf;
    int len;
} saCmdQueue_t;

#define SA_QSIZE 4     // 1 heartbeat (GetSettings) + 2 commands + 1 slack
static saCmdQueue_t sa_queue[SA_QSIZE];
static uint8_t sa_qhead = 0;
static uint8_t sa_qtail = 0;

#ifdef DPRINTF_SMARTAUDIO
static int saQueueLength()
{
    if (sa_qhead >= sa_qtail) {
        return sa_qhead - sa_qtail;
    } else {
        return SA_QSIZE + sa_qhead - sa_qtail;
    }
}
#endif

static bool saQueueEmpty()
{
    return sa_qhead == sa_qtail;
}

static bool saQueueFull()
{
    return ((sa_qhead + 1) % SA_QSIZE) == sa_qtail;
}

static void saQueueCmd(uint8_t *buf, int len)
{
    if (saQueueFull())
         return;

    sa_queue[sa_qhead].buf = buf;
    sa_queue[sa_qhead].len = len;
    sa_qhead = (sa_qhead + 1) % SA_QSIZE;
}

static void saSendQueue(void)
{
    if (saQueueEmpty())
         return;

    saSendCmd(sa_queue[sa_qtail].buf, sa_queue[sa_qtail].len);
    sa_qtail = (sa_qtail + 1) % SA_QSIZE;
}

// Individual commands

static void saGetSettings(void)
{
    static uint8_t bufGetSettings[5] = {0xAA, 0x55, SACMD(SA_CMD_GET_SETTINGS), 0x00, 0x9F};

    saQueueCmd(bufGetSettings, 5);
}

static void saSetFreq(uint16_t freq)
{
    static uint8_t buf[7] = { 0xAA, 0x55, SACMD(SA_CMD_SET_FREQ), 2 };

    dprintf(("smartAudioSetFreq: freq %d\r\n", freq));

    buf[4] = (freq >> 8) & 0xff;
    buf[5] = freq & 0xff;
    buf[6] = CRC8(buf, 6);

    saQueueCmd(buf, 7);
}

#ifdef SMARTAUDIO_EXTENDED_API
static void saSetPitFreq(uint16_t freq)
{
    saSetFreq(freq | SA_FREQ_SETPIT);
}

static void saGetPitFreq(void)
{
    saSetFreq(SA_FREQ_GETPIT);
}
#endif

void smartAudioSetBandChan(uint8_t band, uint8_t chan)
{
    static uint8_t buf[6] = { 0xAA, 0x55, SACMD(SA_CMD_SET_CHAN), 1 };

    buf[4] = band * 8 + chan;
    buf[5] = CRC8(buf, 5);

    saQueueCmd(buf, 6);
}

static void saSetMode(int mode)
{
    static uint8_t buf[6] = { 0xAA, 0x55, SACMD(SA_CMD_SET_MODE), 1 };

    buf[4] = mode & 0x1f;
    buf[5] = CRC8(buf, 5);

    saQueueCmd(buf, 6);
}

void smartAudioSetPowerByIndex(uint8_t index)
{
    static uint8_t buf[6] = { 0xAA, 0x55, SACMD(SA_CMD_SET_POWER), 1 };

    dprintf(("smartAudioSetPowerByIndex: index %d\r\n", index));

    if (sa_vers == 0) {
        // Unknown or yet unknown version.
        return;
    }

    if (index > 3)
        return;

    buf[4] = (sa_vers == 1) ? saPowerTable[index].valueV1 : saPowerTable[index].valueV2;
    buf[5] = CRC8(buf, 5);
    saQueueCmd(buf, 6);
}

bool smartAudioInit()
{
#ifdef SMARTAUDIO_DPRINTF
    // Setup debugSerialPort

    debugSerialPort = openSerialPort(DPRINTF_SERIAL_PORT, FUNCTION_NONE, NULL, 115200, MODE_RXTX, 0);
    if (debugSerialPort) {
        setPrintfSerialPort(debugSerialPort);
        dprintf(("smartAudioInit: OK\r\n"));
    }
#endif

    serialPortConfig_t *portConfig = findSerialPortConfig(FUNCTION_VTX_CONTROL);
    if (portConfig) {
        smartAudioSerialPort = openSerialPort(portConfig->identifier, FUNCTION_VTX_CONTROL, NULL, 4800, MODE_RXTX, SERIAL_BIDIR|SERIAL_BIDIR_PP);
    }

    if (!smartAudioSerialPort) {
        return false;
    }

    smartAudioOpModel = masterConfig.vtx_smartaudio_opmodel;

    return true;
}

void smartAudioConfigurePitFModeByGvar(void *opaque)
{
    UNUSED(opaque);

    if (smartAudioPitFMode == 0) {
        saSetMode(SA_MODE_SET_IN_RANGE_PITMODE);
    } else {
        saSetMode(SA_MODE_SET_OUT_RANGE_PITMODE);
    }
}

void smartAudioConfigureOpModelByGvar(void *opaque)
{
    UNUSED(opaque);

    masterConfig.vtx_smartaudio_opmodel = smartAudioOpModel;

    if (smartAudioOpModel == SA_OPMODEL_FREE) {
        // VTX should power up transmitting.
        // Turn off In-Range and Out-Range bits
        saSetMode(0);
    } else {
        // VTX should power up in pit mode.
        // Setup In-Range or Out-Range bits
        smartAudioConfigurePitFModeByGvar(opaque);
    }
}

#if 0
void 
{
    static int turn = 0;

    if ((turn++ % 2) == 0) {
        saSetMode(SA_MODE_SET_UNLOCK|SA_MODE_SET_PITMODE|SA_MODE_SET_IN_RANGE_PITMODE);
    } else {
        saSetMode(SA_MODE_SET_UNLOCK|SA_MODE_CLR_PITMODE);
    }
}
#endif

void smartAudioProcess(uint32_t now)
{
    static bool initialSent = false;

    if (smartAudioSerialPort == NULL)
        return;

    while (serialRxBytesWaiting(smartAudioSerialPort) > 0) {
        uint8_t c = serialRead(smartAudioSerialPort);
        saReceiveFramer((uint16_t)c);
    }

    // Re-evaluate baudrate after each frame reception
    saAutobaud();

    if (!initialSent) {
        saGetSettings();
        saGetPitFreq();
        saSendQueue();
        initialSent = true;
        return;
    }

    if ((sa_outstanding != SA_CMD_NONE)
            && (now - sa_lastTransmission > SMARTAUDIO_CMD_TIMEOUT)) {
        // Last command timed out
        saResendCmd();
    } else if (!saQueueEmpty()) {
        // Command pending. Send it.
        saSendQueue();
    } else if (now - sa_lastTransmission >= 1000) {
        // Heart beat for autobauding

#ifdef SMARTAUDIO_PITMODE_DEBUG
        static int turn = 0;
        if ((turn++ % 2) == 0) {
            saGetSettings();
        } else {
            smartAudioPitMode();
        }
#else
        saGetSettings();
#endif
        saSendQueue();
    }
}

#ifdef OSD
// API for BFOSD3.0

char smartAudioStatusString[31] = "- - ---- --- ---- -";

uint8_t smartAudioBand = 0;
uint8_t smartAudioChan = 0;
uint8_t smartAudioPower = 0;
uint16_t smartAudioFreq = 0;

static void smartAudioUpdateStatusString(void)
{
    if (sa_vers == 0)
        return;

    tfp_sprintf(smartAudioStatusString, "%c %d %4d %3d ",
        "ABEFR"[sa_chan / 8],
        (sa_chan % 8) + 1,
        saFreqTable[sa_chan / 8][sa_chan % 8],
        (sa_vers == 2) ?  saPowerTable[sa_power].rfpower : saPowerTable[saDacToPowerIndex(sa_power)].rfpower);

    if (sa_vers == 2)
        tfp_sprintf(&smartAudioStatusString[13], "%4d", sa_pitfreq);
    else
        tfp_sprintf(&smartAudioStatusString[13], "%4s", "----");
}

void smartAudioConfigureBandByGvar(void *opaque)
{
    UNUSED(opaque);

    if (sa_vers == 0) {
        // Bounce back; not online yet
        smartAudioBand = 0;
        return;
    }

    if (smartAudioBand == 0) {
        // Bouce back, no going back to undef state
        smartAudioBand = 1;
        return;
    }

    smartAudioSetBandChan(smartAudioBand - 1, smartAudioChan - 1);
}

void smartAudioConfigureChanByGvar(void *opaque)
{
    UNUSED(opaque);

    if (sa_vers == 0) {
        // Bounce back; not online yet
        smartAudioChan = 0;
        return;
    }

    if (smartAudioChan == 0) {
        // Bounce back; no going back to undef state
        smartAudioChan = 1;
        return;
    }

    smartAudioSetBandChan(smartAudioBand - 1, smartAudioChan - 1);
}

void smartAudioConfigurePowerByGvar(void *opaque)
{
    UNUSED(opaque);

    if (sa_vers == 0) {
        // Bounce back; not online yet
        smartAudioPower = 0;
        return;
    }

    if (smartAudioPower == 0) {
        // Bouce back; no going back to undef state
        smartAudioPower = 1;
        return;
    }

    smartAudioSetPowerByIndex(smartAudioPower - 1);
}

void smartAudioSetTxModeByGvar(void *opaque)
{
    UNUSED(opaque);

    if (sa_vers != 2) {
        // Bounce back; not online yet or can't handle mode (V1)
        smartAudioTxMode = SA_TXMODE_NODEF;
        return;
    }

    if (smartAudioTxMode == 0) {
        // Bouce back; no going back to undef state
        ++smartAudioTxMode;
        return;
    }

    if (smartAudioTxMode == SA_TXMODE_ACTIVE) {
        if (smartAudioOpModel == SA_OPMODEL_FREE) {
            saSetMode(SA_MODE_CLR_PITMODE);
        } else {
            if (smartAudioPitFMode == 0)
                saSetMode(SA_MODE_CLR_PITMODE|SA_MODE_SET_IN_RANGE_PITMODE);
            else
                saSetMode(SA_MODE_CLR_PITMODE|SA_MODE_SET_OUT_RANGE_PITMODE);
        }
    } else {
        if ((sa_opmode & SA_MODE_GET_PITMODE) == 0) {
          // Can't go back to pit mode
          smartAudioTxMode = SA_TXMODE_ACTIVE;
        }
    }
}
#endif // OSD
#endif // VTX_SMARTAUDIO
