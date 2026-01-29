#include "fsk2.h"
#include "../driver/bk4829.h"
#include "../driver/systick.h"
#include <stdint.h>

#define RF_Write BK4819_WriteRegister
#define RF_Read BK4819_ReadRegister

#define CTC_ERR 0  // 0=0.1Hz error mode;1=0.1% error mode
#define CTC_IN 10  //~=1   Hz for ctcss in  threshold
#define CTC_OUT 15 //~=1.5 Hz for ctcss out threshold

#define REG_37 0x1D00
#define REG_52 CTC_ERR << 12 | CTC_IN << 6 | CTC_OUT

// was 0x0028
const uint16_t REG_59 = (1 << 3)   // fsk sync length = 4B
                        | (7 << 4) // preamble len = (v + 1)B
    ;

uint16_t FSK_TXDATA[FSK_LEN];
uint16_t FSK_RXDATA[FSK_LEN];

void RF_Txon() {
  RF_Write(0x37, REG_37 | 0x801F); //[1]xtal;[0]bg;[9]ldo_rf_vsel=0 when txon
  RF_Write(0x52, REG_52);          // Set bit[15]=0 to Clear ctcss/cdcss Tail
  RF_Write(0x30, 0x0000);
  RF_Write(0x30, 0xC1FE);
}

void RF_Rxon() {
  //[1]xtal;[0]bg;[9]ldo_rf_vsel=1 when rxon
  RF_Write(0x37, REG_37 | 0x801F | 1 << 9);
  RF_Write(0x30, 0x0000);
  RF_Write(0x30, 0xBFF1); // RF Rxon
}

void RF_EnterFsk() {
  RF_Write(0x70, 0x00E0); //[7]=1,Enable Tone2 for FSK; [6:0]=Gain

  RF_Write(0x72, 0x3065); // 1200bps
  RF_Write(0x58, 0x00C1);

  RF_Write(0x5C, 0x5665);
  RF_Write(0x5D, (FSK_LEN * 2 - 1) << 8); //[15:8]fsk tx length(byte)
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
  RF_Txon();
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

  RF_FskIdle();

  return cnt;
}

bool RF_FskReceive() {
  RF_Rxon();

  RF_Write(0x59, REG_59 | 0x4000);    //[14]fifo clear
  RF_Write(0x59, REG_59 | (1 << 12)); //[12]fsk_rx_en

  RF_Write(0x3F, 0x3000); // rx sucs/fifo_af irq mask=1

  uint16_t rdata;
  uint8_t cnt;
  uint8_t i, j, k = 0;

  for (i = 0; i < (FSK_LEN >> 2); i++) {
    rdata = 0;
    cnt = 200; //~=1s protection

    // polling int
    while (cnt && !(rdata & 0x1)) {
      SYSTICK_DelayMs(5);
      rdata = RF_Read(0x0C);
      cnt--;
    }

    RF_Write(0x02, 0x0000); // clear int

    // over time
    if (!cnt) {
      RF_FskIdle();
      return false;
    }

    for (j = 0; j < 4; j++) {
      rdata = RF_Read(0x5F); // pop data from fifo
      FSK_RXDATA[k] = rdata;
      k++;
    }
  }

  rdata = 0;
  cnt = 200; //~=1s protection

  // polling int
  while (cnt && !(rdata & 0x1)) {
    SYSTICK_DelayMs(5);
    rdata = RF_Read(0x0C);
    cnt--;
  }

  RF_Write(0x02, 0x0000); // clear int

  // over time
  if (!cnt) {
    RF_FskIdle();
    return false;
  }

  cnt = FSK_LEN & 3;

  while (cnt) {
    rdata = RF_Read(0x5F); // pop data from fifo
    FSK_RXDATA[k] = rdata;
    k++;
    cnt--;
  }

  rdata = RF_Read(0x0B); //[4]crc

  RF_FskIdle();
  return true;

  return rdata & 0x10; // CRC ok
}
