/**************************************************************************************************
  Filename:       mac_mcu.c
  Revised:        $Date: 2009-04-07 09:46:20 -0700 (Tue, 07 Apr 2009) $
  Revision:       $Revision: 19717 $

  Description:    Describe the purpose and contents of the file.


  Copyright 2006-2009 Texas Instruments Incorporated. All rights reserved.

  IMPORTANT: Your use of this Software is limited to those specific rights
  granted under the terms of a software license agreement between the user
  who downloaded the software, his/her employer (which must be your employer)
  and Texas Instruments Incorporated (the "License").  You may not use this
  Software unless you agree to abide by the terms of the License. The License
  limits your use, and you acknowledge, that the Software may not be modified,
  copied or distributed unless embedded on a Texas Instruments microcontroller
  or used solely and exclusively in conjunction with a Texas Instruments radio
  frequency transceiver, which is integrated into your product.  Other than for
  the foregoing purpose, you may not use, reproduce, copy, prepare derivative
  works of, modify, distribute, perform, display or sell this Software and/or
  its documentation for any purpose.

  YOU FURTHER ACKNOWLEDGE AND AGREE THAT THE SOFTWARE AND DOCUMENTATION ARE
  PROVIDED �AS IS� WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESS OR IMPLIED,
  INCLUDING WITHOUT LIMITATION, ANY WARRANTY OF MERCHANTABILITY, TITLE,
  NON-INFRINGEMENT AND FITNESS FOR A PARTICULAR PURPOSE. IN NO EVENT SHALL
  TEXAS INSTRUMENTS OR ITS LICENSORS BE LIABLE OR OBLIGATED UNDER CONTRACT,
  NEGLIGENCE, STRICT LIABILITY, CONTRIBUTION, BREACH OF WARRANTY, OR OTHER
  LEGAL EQUITABLE THEORY ANY DIRECT OR INDIRECT DAMAGES OR EXPENSES
  INCLUDING BUT NOT LIMITED TO ANY INCIDENTAL, SPECIAL, INDIRECT, PUNITIVE
  OR CONSEQUENTIAL DAMAGES, LOST PROFITS OR LOST DATA, COST OF PROCUREMENT
  OF SUBSTITUTE GOODS, TECHNOLOGY, SERVICES, OR ANY CLAIMS BY THIRD PARTIES
  (INCLUDING BUT NOT LIMITED TO ANY DEFENSE THEREOF), OR OTHER SIMILAR COSTS.

  Should you have any questions regarding your right to use this Software,
  contact Texas Instruments Incorporated at www.TI.com.
**************************************************************************************************/

/* ------------------------------------------------------------------------------------------------
 *                                          Includes
 * ------------------------------------------------------------------------------------------------
 */

/* hal */
#include "hal_defs.h"
#include "hal_mcu.h"

/* low-level specific */
#include "mac_rx.h"
#include "mac_tx.h"
#include "mac_backoff_timer.h"
#include "mac_csp_tx.h"
#include "mac_rx_onoff.h"
#include "mac_low_level.h"

/* target specific */
#include "mac_mcu.h"
#include "mac_radio_defs.h"

/* debug */
#include "mac_assert.h"


/* ------------------------------------------------------------------------------------------------
 *                                           Defines
 * ------------------------------------------------------------------------------------------------
 */

#if !defined (OSC32K_CRYSTAL_INSTALLED) || (defined (OSC32K_CRYSTAL_INSTALLED) && (OSC32K_CRYSTAL_INSTALLED == TRUE))
#define T2CNF_BASE_VALUE    (RUN | SYNC)
#else
#define T2CNF_BASE_VALUE     RUN
#endif

/* for optimized indexing of uint32's */
#if HAL_MCU_LITTLE_ENDIAN()
#define UINT32_NDX0   0
#define UINT32_NDX1   1
#define UINT32_NDX2   2
#define UINT32_NDX3   3
#else
#define UINT32_NDX0   3
#define UINT32_NDX1   2
#define UINT32_NDX2   1
#define UINT32_NDX3   0
#endif

/* ------------------------------------------------------------------------------------------------
 *                                        Local Variables
 * ------------------------------------------------------------------------------------------------
 */
uint8       macChipVersion = 0;
static int8 maxRssi;


/*
 *  This number is used to calculate the precision count for OSAL timer update. In Beacon mode,
 *  the overflow count may be initialized to zero or to a constant. The "skip" in overflow count
 *  needs to be accounted for in this variable.
 */
static uint32 accumulatedOverflowCount = 0;

/* ------------------------------------------------------------------------------------------------
 *                                       Local Prototypes
 * ------------------------------------------------------------------------------------------------
 */
static void mcuRecordMaxRssiIsr(void);
static uint32 macMcuOverflowGetCompare(void);


/**************************************************************************************************
 * @fn          macMcuInit
 *
 * @brief       Initialize the MCU.
 *
 * @param       none
 *
 * @return      none
 **************************************************************************************************
 */
void macMcuInit(void)
{
  halIntState_t  s;

  /* This CORR_THR value should be changed to 0x14 before attempting RX. Testing has shown that
   * too many false frames are received if the reset value is used. Make it more likely to detect
   * sync by removing the requirement that both symbols in the SFD must have a correlation value
   * above the correlation threshold, and make sync word detection less likely by raising the
   * correlation threshold.
   */
  MDMCTRL1 = CORR_THR;

  /* tuning adjustments for optimal radio performance; details available in datasheet */
  RXCTRL = 0x3F;

  /* Raises the CCA threshold from about -108dBm to about -80 dBm input level.
   */
  CCACTRL0 = CCA_THR;

  /* Makes sync word detection less likely by requiring two zero symbols before the sync word.
   * details available in datasheet.
   */
  MDMCTRL0 = 0x85;

  /* Adjust current in synthesizer; details available in datasheet. */
  FSCTRL = 0x5A;

  /* Adjust current in VCO; details available in datasheet. */
  FSCAL1 = 0x00;

  /* Adjust target value for AGC control loop; details available in datasheet. */
  AGCCTRL1 = 0x15;

  /* Disable source address matching an autopend for now */
  SRCMATCH = 0;

  /* Tune ADC performance, details available in datasheet. */
  ADCTEST0 = 0x10;
  ADCTEST1 = 0x0E;
  ADCTEST2 = 0x03;

  /* Sets TX anti-aliasing filter to appropriate bandwidth.
   * Reduces spurious emissions close to signal.
   */
  TXFILTCFG = TXFILTCFG_RESET_VALUE;

  /* disable the CSPT register compare function */
  CSPT = 0xFF;

  /* enable general RF interrupts */
  IEN2 |= RFIE;

  /* enable general REERR interrupts */
  IEN0 |= RFERRIE;

  /* set RF interrupts one notch above lowest priority (four levels available) */
  IP0 |=  IP_RFERR_RF_DMA_BV;
  IP1 &= ~IP_RFERR_RF_DMA_BV;

  /* read chip version */
  macChipVersion = CHVER;

  /*-------------------------------------------------------------------------------
   *  Initialize MAC timer.
   */

  /* set timer rollover */
  HAL_ENTER_CRITICAL_SECTION(s);
  MAC_MCU_T2_ACCESS_PERIOD_VALUE();
  T2M0 = MAC_RADIO_TIMER_TICKS_PER_BACKOFF() & 0xFF;
  T2M1 = MAC_RADIO_TIMER_TICKS_PER_BACKOFF() >> 8;
  HAL_EXIT_CRITICAL_SECTION(s);

  /* start timer */
  MAC_RADIO_TIMER_WAKE_UP();

  /* Enable latch mode */
  T2CTRL |= LATCH_MODE;

  /* enable timer interrupts */
  T2IE = 1;

 /*----------------------------------------------------------------------------------------------
  *  Initialize random seed value.
  */

  /*
   *  Set radio for infinite reception.  Once radio reaches this state,
   *  it will stay in receive mode regardless RF activity.
   */
  FRMCTRL0 = FRMCTRL0_RESET_VALUE | RX_MODE_INFINITE_RECEPTION;

  /* turn on the receiver */
  macRxOn();

  /*
   *  Wait for radio to reach infinite reception state by checking RSSI valid flag.
   *  Once it does, the least significant bit of ADTSTH should be pretty random.
   */
  while (!(RSSISTAT & 0x01));

  /* put 16 random bits into the seed value */
  {
    uint16 rndSeed;
    uint8  i;

    rndSeed = 0;

    for(i=0; i<16; i++)
    {
      /* use most random bit of analog to digital receive conversion to populate the random seed */
      rndSeed = (rndSeed << 1) | (RFRND & 0x01);
    }

    /*
     *  The seed value must not be zero or 0x0380 (0x8003 in the polynomial).  If it is, the psuedo
     *  random sequence won�t be random.  There is an extremely small chance this seed could randomly
     *  be zero or 0x0380.  The following check makes sure this does not happen.
     */
    if (rndSeed == 0x0000 || rndSeed == 0x0380)
    {
      rndSeed = 0xBABE; /* completely arbitrary "random" value */
    }

    /*
     *  Two writes to RNDL will set the random seed.  A write to RNDL copies current contents
     *  of RNDL to RNDH before writing new the value to RNDL.
     */
    RNDL = rndSeed & 0xFF;
    RNDL = rndSeed >> 8;
  }

  /* turn off the receiver */
  macRxOff();

  /* take receiver out of infinite reception mode; set back to normal operation */
  FRMCTRL0 = FRMCTRL0_RESET_VALUE | RX_MODE_NORMAL_OPERATION;

  /* Turn on autoack */
  MAC_RADIO_TURN_ON_AUTO_ACK();

  /* Initialize SRCEXTPENDEN and SRCSHORTPENDEN to zeros */
  MAC_RADIO_SRC_MATCH_INIT_EXTPENDEN();
  MAC_RADIO_SRC_MATCH_INIT_SHORTPENDEN();
}


/**************************************************************************************************
 * @fn          macMcuRandomByte
 *
 * @brief       Returns a random byte using a special hardware feature that generates new
 *              random values based on the truly random seed set earlier.
 *
 * @param       none
 *
 * @return      a random byte
 **************************************************************************************************
 */
uint8 macMcuRandomByte(void)
{
  /* clock the random generator to get a new random value */
  ADCCON1 = (ADCCON1 & ~RCTRL_BITS) | RCTRL_CLOCK_LFSR;

  /* return new randomized value from hardware */
  return(RNDH);
}

/**************************************************************************************************
 * @fn          macMcuTimerForceDelay
 *
 * @brief       Delay the timer by the requested number of ticks.
 *
 * @param       none
 *
 * @return      none
 **************************************************************************************************
 */
void macMcuTimerForceDelay(uint16 x)
{
  halIntState_t  s;

  HAL_ENTER_CRITICAL_SECTION(s);
  MAC_MCU_T2_ACCESS_COUNT_VALUE();
  T2M0 = (x) & 0xFF;
  T2M1 = (x) >> 8;
  HAL_EXIT_CRITICAL_SECTION(s);
}

/**************************************************************************************************
 * @fn          macMcuTimerCapture
 *
 * @brief       Returns the last timer capture.  This capture should have occurred at the
 *              receive time of the last frame (the last time SFD transitioned to active).
 *
 * @param       none
 *
 * @return      last capture of hardware timer (full 16-bit value)
 **************************************************************************************************
 */
uint16 macMcuTimerCapture(void)
{
  uint16         timerCapture;
  halIntState_t  s;

  HAL_ENTER_CRITICAL_SECTION(s);
  MAC_MCU_T2_ACCESS_CAPTURE_VALUE();
  timerCapture = T2M1 << 8;
  timerCapture |= T2M0;
  HAL_EXIT_CRITICAL_SECTION(s);

  return (timerCapture);
}


/**************************************************************************************************
 * @fn          macMcuOverflowCount
 *
 * @brief       Returns the value of the overflow counter which is a special hardware feature.
 *              The overflow count actually is 24 bits of information.
 *
 * @param       none
 *
 * @return      value of overflow counter
 **************************************************************************************************
 */
uint32 macMcuOverflowCount(void)
{
  uint32         overflowCount;
  halIntState_t  s;

  /* for efficiency, the 32-bit value is encoded using endian abstracted indexing */

  HAL_ENTER_CRITICAL_SECTION(s);

  /* This T2 access macro allows accessing both T2MOVFx and T2Mx */
  MAC_MCU_T2_ACCESS_OVF_COUNT_VALUE();

  /* Latch the entire T2MOVFx first by reading T2M0. */
  T2M0;
  ((uint8 *)&overflowCount)[UINT32_NDX0] = T2MOVF0;
  ((uint8 *)&overflowCount)[UINT32_NDX1] = T2MOVF1;
  ((uint8 *)&overflowCount)[UINT32_NDX2] = T2MOVF2;
  ((uint8 *)&overflowCount)[UINT32_NDX3] = 0;
  HAL_EXIT_CRITICAL_SECTION(s);

  return (overflowCount);
}


/**************************************************************************************************
 * @fn          macMcuOverflowCapture
 *
 * @brief       Returns the last capture of the overflow counter.  A special hardware feature
 *              captures the overflow counter when the regular hardware timer is captured.
 *
 * @param       none
 *
 * @return      last capture of overflow count
 **************************************************************************************************
 */
uint32 macMcuOverflowCapture(void)
{
  uint32         overflowCapture;
  halIntState_t  s;

  /* for efficiency, the 32-bit value is encoded using endian abstracted indexing */
  HAL_ENTER_CRITICAL_SECTION(s);
  MAC_MCU_T2_ACCESS_OVF_CAPTURE_VALUE();
  ((uint8 *)&overflowCapture)[UINT32_NDX0] = T2MOVF0;
  ((uint8 *)&overflowCapture)[UINT32_NDX1] = T2MOVF1;
  ((uint8 *)&overflowCapture)[UINT32_NDX2] = T2MOVF2;
  ((uint8 *)&overflowCapture)[UINT32_NDX3] = 0;
  HAL_EXIT_CRITICAL_SECTION(s);

  return (overflowCapture);
}


/**************************************************************************************************
 * @fn          macMcuOverflowSetCount
 *
 * @brief       Sets the value of the hardware overflow counter.
 *
 * @param       count - new overflow count value
 *
 * @return      none
 **************************************************************************************************
 */
void macMcuOverflowSetCount(uint32 count)
{
  halIntState_t  s;

  MAC_ASSERT(! (count >> 24) );   /* illegal count value */

  /* save the current overflow count */
  accumulatedOverflowCount += macMcuOverflowCount();

  /* deduct the initial count */
  accumulatedOverflowCount -= count;

  HAL_ENTER_CRITICAL_SECTION(s);
  MAC_MCU_T2_ACCESS_OVF_COUNT_VALUE();

  /* for efficiency, the 32-bit value is decoded using endian abstracted indexing */
  /* T2OF2 must be written last */
  T2MOVF0 = ((uint8 *)&count)[UINT32_NDX0];
  T2MOVF1 = ((uint8 *)&count)[UINT32_NDX1];
  T2MOVF2 = ((uint8 *)&count)[UINT32_NDX2];
  HAL_EXIT_CRITICAL_SECTION(s);
}


/**************************************************************************************************
 * @fn          macMcuOverflowSetCompare
 *
 * @brief       Set overflow count compare value.  An interrupt is triggered when the overflow
 *              count equals this compare value.
 *
 * @param       count - overflow count compare value
 *
 * @return      none
 **************************************************************************************************
 */
void macMcuOverflowSetCompare(uint32 count)
{
  halIntState_t  s;
  uint8 enableCompareInt = 0;

  MAC_ASSERT( !(count >> 24) );   /* illegal count value */

  HAL_ENTER_CRITICAL_SECTION(s);

  /*  Disable overflow compare interrupts. */
  if (T2IRQM & TIMER2_OVF_COMPARE1M)
  {
    enableCompareInt = 1;
    T2IRQM &= ~TIMER2_OVF_COMPARE1M;
  }

  MAC_MCU_T2_ACCESS_OVF_CMP1_VALUE();

  /* for efficiency, the 32-bit value is decoded using endian abstracted indexing */
  T2MOVF0 = ((uint8 *)&count)[UINT32_NDX0];
  T2MOVF1 = ((uint8 *)&count)[UINT32_NDX1];
  T2MOVF2 = ((uint8 *)&count)[UINT32_NDX2];

  /*
   *  Now that new compare value is stored, clear the interrupt flag.  This is important just
   *  in case a false match was generated as the multi-byte compare value was written.
   */
  T2IRQF &= ~TIMER2_OVF_COMPARE1F;

  /* re-enable overflow compare interrupts if they were previously enabled */
  if (enableCompareInt)
  {
    T2IRQM |= TIMER2_OVF_COMPARE1M;
  }

  HAL_EXIT_CRITICAL_SECTION(s);
}


/**************************************************************************************************
 * @fn          macMcuOverflowGetCompare
 *
 * @brief       Get overflow count compare value.
 *
 * @param       none
 *
 * @return      overflow count compare value
 **************************************************************************************************
 */
uint32 macMcuOverflowGetCompare(void)
{
  halIntState_t  s;
  uint32         compare;

  HAL_ENTER_CRITICAL_SECTION(s);

  MAC_MCU_T2_ACCESS_OVF_CMP1_VALUE();

  /* for efficiency, the 32-bit value is decoded using endian abstracted indexing */
  ((uint8 *)&compare)[UINT32_NDX0] = T2MOVF0;
  ((uint8 *)&compare)[UINT32_NDX1] = T2MOVF1;
  ((uint8 *)&compare)[UINT32_NDX2] = T2MOVF2;
  ((uint8 *)&compare)[UINT32_NDX3] = 0;

  HAL_EXIT_CRITICAL_SECTION(s);

  return(compare);
}


/**************************************************************************************************
 * @fn          macMcuTimer2Isr
 *
 * @brief       Interrupt service routine for timer2, the MAC timer.
 *
 * @param       none
 *
 * @return      none
 **************************************************************************************************
 */
HAL_ISR_FUNCTION( macMcuTimer2Isr, T2_VECTOR )
{
  uint8 t2irqm = T2IRQM;
  uint8 t2irqf = T2IRQF;

  /*------------------------------------------------------------------------------------------------
   *  Overflow compare interrupt - triggers when then overflow counter is
   *  equal to the overflow compare register.
   */
  if ((t2irqf & TIMER2_OVF_COMPARE1F) & t2irqm)
  {

    /* call function for dealing with the timer compare interrupt */
    macBackoffTimerCompareIsr();

    /* clear overflow compare interrupt flag */
    T2IRQF &= ~TIMER2_OVF_COMPARE1F;
  }

  /*------------------------------------------------------------------------------------------------
   *  Overflow interrupt - triggers when the hardware timer rolls over.
   */
  else if ((t2irqf & TIMER2_PERF) & t2irqm)
  {
    /* call energy detect interrupt function, this interrupt not used for any other functionality */
    mcuRecordMaxRssiIsr();

    /* clear the interrupt flag */
    T2IRQF &= ~TIMER2_PERF;
  }
}


/**************************************************************************************************
 * @fn          macMcuTimer2OverflowWorkaround
 *
 * @brief       T2 overflow compare 1 interrupt may not be generated sometimes. The problem only
 *              occurs when POWER_SAVING is turned on, i.e. the 32KHz drives the chip in sleep and
 *              SYNC start is used. This function is called from ST interrupt.
 *
 * @param       none
 *
 * @return      none
 **************************************************************************************************
 */
void macMcuTimer2OverflowWorkaround(void)
{
  if (T2IRQM & TIMER2_OVF_COMPARE1F)
  {
    /* T2 comapre 1 interrupt is enabled but T2 compare 1 intererrupt is not generated */
    if (!(T2IRQF & TIMER2_OVF_COMPARE1F))
    {
      if (MAC_RADIO_BACKOFF_COUNT() > macMcuOverflowGetCompare())
      {
        /* Set the flag to trigger the timer compare interrupt */
        macBackoffTimerCompareIsr();
        T2IRQF &= ~TIMER2_OVF_COMPARE1F;
      }
    }
  }
}


/**************************************************************************************************
 * @fn          macMcuPrecisionCount
 *
 * @brief       This function is used by higher layer to read a free running counter driven by
 *              MAC timer.
 *
 * @param       none
 *
 * @return      overflowCount
 **************************************************************************************************
 */
uint16 macMcuPrecisionCount(void)
{
  uint16         overflowCount;
  halIntState_t  s;

  HAL_ENTER_CRITICAL_SECTION(s);

  /* This T2 access macro allows accessing both T2MOVFx and T2Mx */
  MAC_MCU_T2_ACCESS_OVF_COUNT_VALUE();

  /* Latch the entire T2MOVFx first by reading T2M0.
   * T2M0 and T2MOVF2 are discarded.
   */
  T2M0;
  ((uint8 *)&overflowCount)[UINT32_NDX0] = T2MOVF0;
  ((uint8 *)&overflowCount)[UINT32_NDX1] = T2MOVF1;
  HAL_EXIT_CRITICAL_SECTION(s);

  /* the overflowCount needs to account for the accumulated overflow count in Beacon mode.
   * It's okay to let it overflow since only LSBs are used.
   */
  overflowCount += (uint16)accumulatedOverflowCount;

  return(overflowCount);
}


/**************************************************************************************************
 * @fn          macMcuRfIsr
 *
 * @brief       Interrupt service routine that handles all RF interrupts.  There are a number
 *              of conditions "ganged" onto this one ISR so each condition must be tested for.
 *
 * @param       none
 *
 * @return      none
 **************************************************************************************************
 */
HAL_ISR_FUNCTION( macMcuRfIsr, RF_VECTOR )
{
  uint8 rfim;

  rfim = RFIRQM1;

  /*  The CPU level RF interrupt flag must be cleared here (before clearing RFIRQFx).
   *  to allow the interrupts to be nested.
   */
  S1CON = 0x00;

  if ((RFIRQF1 & IRQ_CSP_MANINT) & rfim)
  {
    /*
     *  Important!  Because of how the CSP programs are written, CSP_INT interrupts should
     *  be processed before CSP_STOP interrupts.  This becomes an issue when there are
     *  long critical sections.
     */
    /* clear flag */
    RFIRQF1 = ~IRQ_CSP_MANINT;
    macCspTxIntIsr();
  }
  else if ((RFIRQF1 & IRQ_CSP_STOP) & rfim)
  {
    /* clear flag */
    RFIRQF1 = ~IRQ_CSP_STOP;
    macCspTxStopIsr();
  }
  else if ((RFIRQF1 & IRQ_TXACKDONE) & rfim)
  {
    /* disable interrupt - set up is for "one shot" operation */
    RFIRQM1 &= ~IM_TXACKDONE;
    macRxAckTxDoneCallback();
  }

  rfim = RFIRQM0;

  /* process RFIRQF0 next */
  if ((RFIRQF0 & IRQ_FIFOP) & rfim)
  {
    /* continue to execute interrup                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        t handler as long as FIFOP is active */
    do
    {
      macRxThresholdIsr();
      RFIRQF0 = ~IRQ_FIFOP;
    } while (FSMSTAT1 & FIFOP);
  }
}


/**************************************************************************************************
 * @fn          macMcuRfErrIsr
 *
 * @brief       Interrupt service routine that handles all RF Error interrupts.  Only the RX FIFO
 *              overflow condition is handled.
 *
 * @param       none
 *
 * @return      none
 **************************************************************************************************
 */
HAL_ISR_FUNCTION( macMcuRfErrIsr, RFERR_VECTOR )
{
  uint8 rferrm = RFERRM;

  if ((RFERRF & RFERR_RXOVERF) & rferrm)
  {
    RFERRF &= ~RFERR_RXOVERF;
    macRxFifoOverflowIsr();
  }
}


/**************************************************************************************************
 * @fn          macMcuRecordMaxRssiStart
 *
 * @brief       Starts recording of the maximum received RSSI value.
 *
 * @param       none
 *
 * @return      none
 **************************************************************************************************
 */
void macMcuRecordMaxRssiStart(void)
{
  /* start maximum recorded value at the lowest possible value */
  maxRssi = -128;

  /* enable timer overflow interrupt */
  T2IRQM |= TIMER2_PERM;
}


/**************************************************************************************************
 * @fn          macMcuRecordMaxRssiStop
 *
 * @brief       Stops recording of the maximum received RSSI.  It returns the maximum value
 *              received since starting the recording.
 *
 * @param       none
 *
 * @return      maximum received RSSI value
 **************************************************************************************************
 */
int8 macMcuRecordMaxRssiStop(void)
{
  /* disable timer overflow interrupt */
  T2IRQM &= ~TIMER2_PERM;

  return(maxRssi);
}


/*=================================================================================================
 * @fn          macMcuRecordMaxRssiIsr
 *
 * @brief       Interrupt service routine called during recording of max RSSI value.
 *
 * @param       none
 *
 * @return      none
 *=================================================================================================
 */
static void mcuRecordMaxRssiIsr(void)
{
  int8 rssi;

  /* read latest RSSI value */
  rssi = RSSI;

  /* if new RSSI value is greater than the maximum already received, it is the new maximum */
  if (rssi > maxRssi)
  {
    maxRssi = rssi;
  }
}


/**************************************************************************************************
 *                                  Compile Time Integrity Checks
 **************************************************************************************************
 */
#if ((IRQ_SFD != IM_SFD) || (IRQ_FIFOP != IM_FIFOP) || (IRQ_TXACKDONE != IM_TXACKDONE))
#error "ERROR: Compile time error with RFIRQFx vs RFIRQMx register defines."
#endif

/**************************************************************************************************
*/
