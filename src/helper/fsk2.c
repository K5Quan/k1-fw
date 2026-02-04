#include "fsk2.h"
#include "../driver/bk4829.h"
#include "../driver/systick.h"
#include "../driver/uart.h"
#include "../external/printf/printf.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define RF_Write BK4819_WriteRegister
#define RF_Read BK4819_ReadRegister

#define CTC_ERR 0  // 0=0.1Hz error mode;1=0.1% error mode
#define CTC_IN 10  //~=1   Hz for ctcss in  threshold
#define CTC_OUT 15 //~=1.5 Hz for ctcss out threshold

#define REG_37 0x1D00
#define REG_52 CTC_ERR << 12 | CTC_IN << 6 | CTC_OUT

// was 0x0028
const uint16_t REG_59 = (1 << 3)         // fsk sync length = 4B
                        | ((8 - 1) << 4) // preamble len = (v + 1)B 0..15
    ;

uint16_t FSK_TXDATA[FSK_LEN];
uint16_t FSK_RXDATA[FSK_LEN];

void RF_EnterFsk() {
  RF_Write(0x70, 0x00E0); //[7]=1,Enable Tone2 for FSK; [6:0]=Gain

  RF_Write(0x72, 0x3065); // 1200bps
  RF_Write(0x58, 0x00C1);

  RF_Write(0x5C, 0x5665); // disable crc
  // RF_Write(0x5C, 0x5665 & (~(1 << 6)));   // disable crc
  RF_Write(0x5D, (FSK_LEN * 2 - 1) << 8); //[15:8]fsk tx length(byte)

  RF_Write(0x5E, (64 << 3) | 4); // Almost empty=64, almost full=4

  BK4819_WriteRegister(0x40, 0x3000 + 1050);
}

void RF_ExitFsk() {
  RF_Write(0x70, 0x0000); // Disable Tone2
  RF_Write(0x58, 0x0000); // Disable FSK
}

void RF_FskIdle() {
  RF_Write(0x3F, 0x0000); // tx sucs irq mask=0
  RF_Write(0x59, REG_59); // fsk_tx_en=0, fsk_rx_en=0

  BK4819_Idle();
}

bool RF_FskTransmit() {
  SYSTICK_DelayMs(100);

  RF_Write(0x3F, 0x8000); // tx sucs irq mask=1

  RF_Write(0x59, REG_59 | 0x8000); //[15]fifo clear
  RF_Write(0x59, REG_59);

  uint8_t i;

  for (i = 0; i < FSK_LEN; i++) {
    RF_Write(0x5F, FSK_TXDATA[i]); // push data to fifo
  }

  SYSTICK_DelayMs(20);

  RF_Write(0x59, REG_59 | (1 << 11)); //[11]fsk_tx_en

  uint16_t rdata = 0;
  uint8_t cnt = 200; //~=1s protection

  // polling int
  while (cnt && !(rdata & 0x1)) {
    SYSTICK_DelayMs(5);
    rdata = RF_Read(0x0C);
    cnt--;
  }

  RF_Write(0x02, 0x0000); // clear int
  RF_Write(0x59, REG_59); // fsk_tx_en=0, fsk_rx_en=0

  // RF_FskIdle();

  return cnt;
}

typedef enum MsgStatus {
  READY,
  SENDING,
  RECEIVING,
} MsgStatus;

static uint8_t rxIdx;
static MsgStatus msgStatus;

bool RF_FskReceive(uint16_t int_bits) {
  const bool sync = int_bits & BK4819_REG_02_FSK_RX_SYNC;
  const bool fifo_almost_full = int_bits & BK4819_REG_02_FSK_FIFO_ALMOST_FULL;
  const bool finished = int_bits & BK4819_REG_02_FSK_RX_FINISHED;

  if (sync) {
    rxIdx = 0;
    msgStatus = RECEIVING;
    memset(FSK_RXDATA, 0, sizeof(FSK_RXDATA));
  }

  if (fifo_almost_full && msgStatus == RECEIVING) {
    const uint16_t count = RF_Read(BK4819_REG_5E) & 7u;
    for (uint16_t i = 0; i < count; i++) {
      FSK_RXDATA[rxIdx++] = BK4819_ReadRegister(BK4819_REG_5F);
    }
  }

  if (finished) {
    msgStatus = READY;

    const uint16_t fsk_reg59 =
        BK4819_ReadRegister(BK4819_REG_59) &
        ~((1u << 15) | (1u << 14) | (1u << 12) | (1u << 11));

    BK4819_WriteRegister(BK4819_REG_59, (1u << 15) | (1u << 14) | fsk_reg59);
    BK4819_WriteRegister(BK4819_REG_59, (1u << 12) | fsk_reg59);

    printf("%10u: ", Now());
    for (uint8_t i = 0; i < 4; ++i) {
      printf("%04X ", FSK_RXDATA[i]);
    }
    printf("\n");

    rxIdx = 0;
    return true;
  }

  return false;
}
