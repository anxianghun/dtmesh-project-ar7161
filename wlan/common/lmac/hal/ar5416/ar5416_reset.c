/*
 * Copyright (c) 2002-2005 Sam Leffler, Errno Consulting
 * Copyright (c) 2009, Atheros Communications Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */
#include "opt_ah.h"

#ifdef AH_SUPPORT_AR5416

#include "ah.h"
#include "ah_xr.h"
#include "ah_internal.h"
#include "ah_devid.h"

#include "ar5416/ar5416.h"
#include "ar5416/ar5416reg.h"
#include "ar5416/ar5416phy.h"

#define N(a)    (sizeof(a)/sizeof(a[0]))

/* Additional Time delay to wait after activiting the Base band */
#define BASE_ACTIVATE_DELAY         100     /* 100 usec */
#define RTC_PLL_SETTLE_DELAY        1000    /* 1 ms     */
#define COEF_SCALE_S                24
#define HT40_CHANNEL_CENTER_SHIFT   10      /* MHz      */

static void ar5416Set11nRegs(struct ath_hal *ah, HAL_CHANNEL *chan, HAL_HT_MACMODE macmode);

static inline HAL_BOOL ar5416SetResetPowerOn(struct ath_hal *ah);
static inline HAL_BOOL ar5416SetReset(struct ath_hal *ah, int type);

static void     ar5416StartNFCal(struct ath_hal *ah);
static void     ar5416LoadNF(struct ath_hal *ah, HAL_CHANNEL_INTERNAL *chan);
static int16_t  ar5416GetNf(struct ath_hal *, HAL_CHANNEL_INTERNAL *);
static void     ar5416UpdateNFHistBuff(struct ath_hal *ah,
                                       HAL_NFCAL_HIST *h, int16_t *nfarray);
static int16_t  ar5416GetNfHistMid(int16_t *nfCalBuffer);
static void     ar5416SetDeltaSlope(struct ath_hal *, HAL_CHANNEL_INTERNAL *);
static void     ar5416SpurMitigate(struct ath_hal *ah, HAL_CHANNEL *chan);
static void     ar9280SpurMitigate(struct ath_hal *, HAL_CHANNEL *, HAL_CHANNEL_INTERNAL *);
#if 0
HAL_BOOL        ar5416SetTransmitPower(struct ath_hal *ah,
                                       HAL_CHANNEL_INTERNAL *chan);
static HAL_BOOL ar5416SetRateTable(struct ath_hal *,
                HAL_CHANNEL *, int16_t tpcScaleReduction, int16_t powerLimit,
                int16_t *minPower, int16_t *maxPower);
static void ar5416GetTargetPowers(struct ath_hal *, HAL_CHANNEL *,
                TRGT_POWER_INFO *pPowerInfo, u_int16_t numChannels,
                TRGT_POWER_INFO *pNewPower);
static u_int16_t ar5416GetMaxEdgePower(u_int16_t channel,
                RD_EDGES_POWER  *pRdEdgesPower);
#endif
static inline HAL_CHANNEL_INTERNAL* ar5416CheckChan(struct ath_hal *ah,
                                                    HAL_CHANNEL *chan);
static inline HAL_STATUS ar5416ProcessIni(struct ath_hal *ah,
                                          HAL_CHANNEL *chan,
                                          HAL_CHANNEL_INTERNAL *ichan,
                                          HAL_HT_MACMODE macmode);
static inline void ar5416SetRfMode(struct ath_hal *ah, HAL_CHANNEL *chan);
static inline void ar5416GetDeltaSlopeValues(struct ath_hal *ah,
                                             u_int32_t coef_scaled,
                                             u_int32_t *coef_mantissa,
                                             u_int32_t *coef_exponent);

/* NB: public for RF backend use */
void    ar5416GetLowerUpperValues(u_int16_t value,
                u_int16_t *pList, u_int16_t listSize,
                u_int16_t *pLowerValue, u_int16_t *pUpperValue);
void    ar5416ModifyRfBuffer(u_int32_t *rfBuf, u_int32_t reg32,
                u_int32_t numBits, u_int32_t firstBit, u_int32_t column);

static inline HAL_BOOL ar5416ChannelChange(struct ath_hal *ah,
                                           HAL_CHANNEL *chan,
                                           HAL_CHANNEL_INTERNAL *ichan,
                                           HAL_HT_MACMODE macmode);

static inline void ar5416OverrideIni(struct ath_hal *ah, HAL_CHANNEL *chan);
static inline void ar5416InitPLL(struct ath_hal *ah, HAL_CHANNEL *chan);
static inline void ar5416InitChainMasks(struct ath_hal *ah);
static inline HAL_BOOL ar5416InitCal(struct ath_hal *ah, HAL_CHANNEL *chan);
inline HAL_BOOL ar5416IsCalSupp(struct ath_hal *ah, HAL_CHANNEL *chan,
                                HAL_CAL_TYPES calType);
inline void ar5416SetupCalibration(struct ath_hal *ah,
                                   HAL_CAL_LIST *currCal);
inline void ar5416ResetCalibration(struct ath_hal *ah,
                                   HAL_CAL_LIST *perCal);
inline HAL_BOOL ar5416RunInitCals(struct ath_hal *ah,
                                  int init_cal_count);
inline void ar5416PerCalibration(struct ath_hal *ah,
                                 HAL_CHANNEL_INTERNAL *ichan,
                                 u_int8_t rxchainmask,
                                 HAL_CAL_LIST *perCal,
                                 HAL_BOOL *isCalDone);

static inline void ar5416SetDma(struct ath_hal *ah);
static inline void ar5416InitBB(struct ath_hal *ah, HAL_CHANNEL *chan);
static inline void ar5416InitInterruptMasks(struct ath_hal *ah,
                                            HAL_OPMODE opmode);
static inline void ar5416InitQOS(struct ath_hal *ah);
static inline void ar5416InitUserSettings(struct ath_hal *ah);
static inline void ar5416AttachHwPlatform(struct ath_hal *ah);
static void ar5416OpenLoopPowerControlTempCompensation(struct ath_hal *ah);
static void ar5416OpenLoopPowerControlInit(struct ath_hal *ah);
static inline void ar5416InitMFP(struct ath_hal *ah);

#define ar5416CheckOpMode(_opmode) \
    ((_opmode == HAL_M_STA) || (_opmode == HAL_M_IBSS) ||\
     (_opmode == HAL_M_HOSTAP) || (_opmode == HAL_M_MONITOR))

#ifdef notyet
HAL_BOOL
ar5416Reset11n(struct ath_hal *ah, HAL_OPMODE opmode,
        HAL_CHANNEL *chan, HAL_BOOL bChannelChange, HAL_STATUS *status)
{
   int         attempts = 0;
   HAL_BOOL    res;
#define RESET_CAL_ATTEMPTS  100

    do {
        res = ar5416ResetCommon(ah, opmode, chan, ht, bChannelChange, status);

        /* force full reset */
        bChannelChange = AH_FALSE;
        attempts++;
    } while ((res == AH_FALSE) && attempts < RESET_CAL_ATTEMPTS);

    return res;
}
#endif

/*
 * Places the device in and out of reset and then places sane
 * values in the registers based on EEPROM config, initialization
 * vectors (as determined by the mode), and station configuration
 *
 * bChannelChange is used to preserve DMA/PCU registers across
 * a HW Reset during channel change.
 */
HAL_BOOL
ar5416Reset(struct ath_hal *ah, HAL_OPMODE opmode, HAL_CHANNEL *chan,
           HAL_HT_MACMODE macmode, u_int8_t txchainmask, u_int8_t rxchainmask,
           HAL_HT_EXTPROTSPACING extprotspacing, HAL_BOOL bChannelChange,
           HAL_STATUS *status)
{
#define FAIL(_code)     do { ecode = _code; goto bad; } while (0)
        u_int32_t               saveLedState;
        struct ath_hal_5416     *ahp = AH5416(ah);
        struct ath_hal_private  *ap  = AH_PRIVATE(ah);
        HAL_CHANNEL_INTERNAL    *ichan;
        HAL_CHANNEL_INTERNAL    *curchan = ap->ah_curchan;
        u_int32_t               saveDefAntenna;
        u_int32_t               macStaId1;
        HAL_STATUS              ecode;
        int                     i, rx_chainmask;

        ahp->ah_extprotspacing = extprotspacing;
        ahp->ah_txchainmask = txchainmask;
        ahp->ah_rxchainmask = rxchainmask;

        if (AR_SREV_KITE(ah)) {
            /* Kite has only one chain support, clear chain 1 & 2 setting */
            ahp->ah_txchainmask &= 0x1;
            ahp->ah_rxchainmask &= 0x1;
        } else if (AR_SREV_MERLIN(ah) || AR_SREV_KIWI(ah)) {
            /* Merlin and Kiwi has only two chain support, clear chain 2 setting */
            ahp->ah_txchainmask &= 0x3;
            ahp->ah_rxchainmask &= 0x3;
        }

        HALASSERT(ar5416CheckOpMode(opmode));

        OS_MARK(ah, AH_MARK_RESET, bChannelChange);

        /*
         * Map public channel to private.
         */
        ichan = ar5416CheckChan(ah, chan);
        if (ichan == AH_NULL) {
                HDPRINTF(ah, HAL_DBG_CHANNEL, "%s: invalid channel %u/0x%x; no mapping\n",
                        __func__, chan->channel, chan->channelFlags);
                FAIL(HAL_EINVAL);
        }


        /* Bring out of sleep mode */
        if (!ar5416SetPowerMode(ah, HAL_PM_AWAKE, AH_TRUE))
            return AH_FALSE;

       
        /* Get the value from the previous NF cal and update history buffer */
        if (curchan)
            ar5416GetNf(ah, curchan);

        /* 
         * Fast channel change (Change synthesizer based on channel freq without resetting chip)
         * Don't do it when
         *   - Flag is not set
         *   - Chip is just coming out of full sleep
         *   - Channel to be set is same as current channel
         *   - Channel flags are different, like when moving from 2GHz to 5GHz channels
         *   - Merlin: Switching in/out of fast clock enabled channels 
                       (not currently coded, since fast clock is enabled across the 5GHz band
         *              and we already do a full reset when switching in/out of 5GHz channels)
         */
        if (bChannelChange && !AR_SREV_MERLIN(ah) &&
            (ahp->ah_chipFullSleep != AH_TRUE) &&
            (AH_PRIVATE(ah)->ah_curchan != AH_NULL) &&
            (chan->channel != AH_PRIVATE(ah)->ah_curchan->channel) &&
            ((chan->channelFlags & CHANNEL_ALL) ==
             (AH_PRIVATE(ah)->ah_curchan->channelFlags & CHANNEL_ALL))) {

                if (ar5416ChannelChange(ah, chan, ichan, macmode)) {
                    chan->channelFlags = ichan->channelFlags;
                    chan->privFlags = ichan->privFlags;
                    
                    /* 
                     * Load the NF from history buffer of the current channel.
                     * NF is slow time-variant, so it is OK to use a historical value.
                     */
                    ar5416LoadNF(ah, AH_PRIVATE(ah)->ah_curchan);

                    /* start NF calibration, without updating BB NF register*/
                    ar5416StartNFCal(ah);
                    
                    /* If ChannelChange completed - skip the rest of reset */
                    return AH_TRUE;
                }
        }

        /*
         * Preserve the antenna on a channel change
         */
        saveDefAntenna = OS_REG_READ(ah, AR_DEF_ANTENNA);
        if (saveDefAntenna == 0)
            saveDefAntenna = 1;

        /* Save hardware flag before chip reset clears the register */
        macStaId1 = OS_REG_READ(ah, AR_STA_ID1) & AR_STA_ID1_BASE_RATE_11B;

        /* Save led state from pci config register */
        saveLedState = OS_REG_READ(ah, AR_CFG_LED) &
                (AR_CFG_LED_ASSOC_CTL | AR_CFG_LED_MODE_SEL |
                 AR_CFG_LED_BLINK_THRESH_SEL | AR_CFG_LED_BLINK_SLOW);

        /* Mark PHY inactive prior to reset, to be undone in ar5416InitBB () */
        ar5416MarkPhyInactive(ah); 

        if (!ar5416ChipReset(ah, chan)) {
                HDPRINTF(ah, HAL_DBG_RESET, "%s: chip reset failed\n", __func__);
                FAIL(HAL_EIO);
        }

        OS_MARK(ah, AH_MARK_RESET_LINE, __LINE__);

        if (AR_SREV_MERLIN_10_OR_LATER(ah)) {
            /* Disable JTAG */
            OS_REG_SET_BIT(ah, AR_GPIO_INPUT_EN_VAL, AR_GPIO_JTAG_DISABLE);
        }

        if( AR_SREV_KIWI_13_OR_LATER(ah) ) {
            /* Enable ASYNC FIFO */
            OS_REG_SET_BIT(ah, AR_MAC_PCU_ASYNC_FIFO_REG3, AR_MAC_PCU_ASYNC_FIFO_REG3_DATAPATH_SEL);
            OS_REG_SET_BIT(ah, AR_PHY_MODE, AR_PHY_MODE_ASYNCFIFO);
            OS_REG_CLR_BIT(ah, AR_MAC_PCU_ASYNC_FIFO_REG3, AR_MAC_PCU_ASYNC_FIFO_REG3_SOFT_RESET);
            OS_REG_SET_BIT(ah, AR_MAC_PCU_ASYNC_FIFO_REG3, AR_MAC_PCU_ASYNC_FIFO_REG3_SOFT_RESET);
        }

        /*
         * Note that ar5416InitChainMasks() is called from within
         * ar5416ProcessIni() to ensure the swap bit is set before
         * the pdadc table is written.
         */

        ecode = ar5416ProcessIni(ah, chan, ichan, macmode);
        if (ecode != HAL_OK) goto bad;

        ahp->ah_immunity_on = AH_FALSE;

        if (ath_hal_getcapability(ah, HAL_CAP_RIFS_RX, 0, AH_NULL) ==
            HAL_ENOTSUPP) {
            /* For devices that need SW assistance for RIFS Rx (Owl), disable
             * RIFS Rx enablement as part of reset.
             */
            if (ahp->ah_rifs_enabled) {
                ahp->ah_rifs_enabled = AH_FALSE;
                OS_MEMZERO(ahp->ah_rifs_reg, sizeof(ahp->ah_rifs_reg));
            }
        } else {
            /* For devices with full HW RIFS Rx support (Sowl/Howl/Merlin, etc),
             * restore register settings from prior to reset.
             */
            if ((AH_PRIVATE(ah)->ah_curchan != AH_NULL) &&
                (ar5416GetCapability(ah, HAL_CAP_BB_RIFS_HANG, 0, AH_NULL)
                 == HAL_OK)) {
                /* Re-program RIFS Rx policy after reset */
                ar5416SetRifsDelay(ah, ahp->ah_rifs_enabled);
            }
        }

        /* Initialize Management Frame Protection */
        ar5416InitMFP(ah);

        ahp->ah_immunity[0] = OS_REG_READ_FIELD(ah, AR_PHY_SFCORR_LOW,
                                        AR_PHY_SFCORR_LOW_M1_THRESH_LOW);
        ahp->ah_immunity[1] = OS_REG_READ_FIELD(ah, AR_PHY_SFCORR_LOW,
                                        AR_PHY_SFCORR_LOW_M2_THRESH_LOW);
        ahp->ah_immunity[2] = OS_REG_READ_FIELD(ah, AR_PHY_SFCORR,
                                        AR_PHY_SFCORR_M1_THRESH);
        ahp->ah_immunity[3] = OS_REG_READ_FIELD(ah, AR_PHY_SFCORR,
                                        AR_PHY_SFCORR_M2_THRESH);
        ahp->ah_immunity[4] = OS_REG_READ_FIELD(ah, AR_PHY_SFCORR,
                                        AR_PHY_SFCORR_M2COUNT_THR);
        ahp->ah_immunity[5] = OS_REG_READ_FIELD(ah, AR_PHY_SFCORR_LOW,
                                        AR_PHY_SFCORR_LOW_M2COUNT_THR_LOW);

        /* Write delta slope for OFDM enabled modes (A, G, Turbo) */
        if (IS_CHAN_OFDM(chan)|| IS_CHAN_HT(chan)) {
                ar5416SetDeltaSlope(ah, ichan);
    }

        /* 
         * For Merlin, spur can break CCK MRC algorithm. SpurMitigate needs to
         * be called in all 11A/B/G/HT modes to disable CCK MRC if spur is found
         * in this channel.
         */
        if (AR_SREV_MERLIN_10_OR_LATER(ah))
            ar9280SpurMitigate(ah, chan, ichan);
        else
            ar5416SpurMitigate(ah, chan);

        if (!ar5416EepromSetBoardValues(ah, ichan)) {
            HDPRINTF(ah, HAL_DBG_EEPROM, "%s: error setting board options\n", __func__);
            FAIL(HAL_EIO);
        }

#ifndef ATH_FORCE_BIAS
        /*
         * Antenna Control without forceBias.
         * This function must be called after 
         * ar5416SetRfRegs() and ar5416EepromSetBoardValues().
         */
        ahp->ah_rfHal.decreaseChainPower(ah, chan);
#endif /* !ATH_FORCE_BIAS */

        OS_MARK(ah, AH_MARK_RESET_LINE, __LINE__);

        OS_REG_WRITE(ah, AR_STA_ID0, LE_READ_4(ahp->ah_macaddr));
        OS_REG_WRITE(ah, AR_STA_ID1, LE_READ_2(ahp->ah_macaddr + 4)
                | macStaId1
                | AR_STA_ID1_RTS_USE_DEF
                | (ap->ah_config.ath_hal_6mb_ack ? AR_STA_ID1_ACKCTS_6MB : 0)
                | ahp->ah_staId1Defaults
        );
        ar5416SetOperatingMode(ah, opmode);

        /* Set Venice BSSID mask according to current state */
        OS_REG_WRITE(ah, AR_BSSMSKL, LE_READ_4(ahp->ah_bssidmask));
        OS_REG_WRITE(ah, AR_BSSMSKU, LE_READ_2(ahp->ah_bssidmask + 4));

        /* Restore previous antenna */
        OS_REG_WRITE(ah, AR_DEF_ANTENNA, saveDefAntenna);

        /* then our BSSID and assocID */
        OS_REG_WRITE(ah, AR_BSS_ID0, LE_READ_4(ahp->ah_bssid));
        OS_REG_WRITE(ah, AR_BSS_ID1, LE_READ_2(ahp->ah_bssid + 4) |
                                     ((ahp->ah_assocId & 0x3fff) << AR_BSS_ID1_AID_S));

        OS_REG_WRITE(ah, AR_ISR, ~0);           /* cleared on write */

        OS_REG_WRITE(ah, AR_RSSI_THR, INIT_RSSI_THR);

        /*
         * Set Channel now modifies bank 6 parameters for FOWL workaround
         * to force rf_pwd_icsyndiv bias current as function of synth
         * frequency.Thus must be called after ar5416ProcessIni() to ensure
         * analog register cache is valid.
         */
        if (!ahp->ah_rfHal.setChannel(ah, ichan))
                FAIL(HAL_EIO);

	/* set RIFS disabled if rifs_bb_hang is present and sec count is non-0 */
	if (AH_PRIVATE(ah)->ah_curchan &&
        	(ar5416GetCapability(ah, HAL_CAP_BB_RIFS_HANG, 0, AH_NULL)
	         == HAL_OK) && (ahp->ah_rifs_sec_cnt  >0 ))
	{
                ar5416SetRifsDelay(ah,AH_FALSE);
	}
        OS_MARK(ah, AH_MARK_RESET_LINE, __LINE__);

        /* Set 1:1 QCU to DCU mapping for all queues */
        for (i = 0; i < AR_NUM_DCU; i++)
                OS_REG_WRITE(ah, AR_DQCUMASK(i), 1 << i);

        ahp->ah_intrTxqs = 0;
        for (i = 0; i < AH_PRIVATE(ah)->ah_caps.halTotalQueues; i++)
                ar5416ResetTxQueue(ah, i);

        ar5416InitInterruptMasks(ah, opmode);

        if (ath_hal_isrfkillenabled(ah))
            ar5416EnableRfKill(ah);

        ar5416InitQOS(ah);

        ar5416InitUserSettings(ah);

        if (AR_SREV_KIWI_13_OR_LATER(ah)) {
            /* Enable ASYNC FIFO
             * If Async FIFO is enabled, the following counters change as MAC now runs at 117 Mhz
             * instead of 88/44MHz when async FIFO is disabled.
             * *NOTE* THE VALUES BELOW TESTED FOR HT40 2 CHAIN
             * Overwrite the delay/timeouts initialized in ProcessIni() above.
             */
            OS_REG_WRITE(ah, AR_D_GBL_IFS_SIFS, AR_D_GBL_IFS_SIFS_ASYNC_FIFO_DUR);
            OS_REG_WRITE(ah, AR_D_GBL_IFS_SLOT, AR_D_GBL_IFS_SLOT_ASYNC_FIFO_DUR);
            OS_REG_WRITE(ah, AR_D_GBL_IFS_EIFS, AR_D_GBL_IFS_EIFS_ASYNC_FIFO_DUR);
            OS_REG_WRITE(ah, AR_TIME_OUT, AR_TIME_OUT_ACK_CTS_ASYNC_FIFO_DUR);
            OS_REG_WRITE(ah, AR_USEC, AR_USEC_ASYNC_FIFO_DUR);

            OS_REG_SET_BIT(ah, AR_MAC_PCU_LOGIC_ANALYZER, AR_MAC_PCU_LOGIC_ANALYZER_DISBUG20768);
            OS_REG_RMW_FIELD(ah, AR_AHB_MODE, AR_AHB_CUSTOM_BURST_EN, AR_AHB_CUSTOM_BURST_ASYNC_FIFO_VAL);
        }

        /* Keep the following piece of code separae from the above block to facilitate selective
         * turning-off through some registry setting or some thing like that */
        if (AR_SREV_KIWI_13_OR_LATER(ah)) {
            /* Enable AGGWEP to accelerate encryption engine */
            OS_REG_SET_BIT(ah, AR_PCU_MISC_MODE2, AR_PCU_MISC_MODE2_ENABLE_AGGWEP);
        }

        AH_PRIVATE(ah)->ah_opmode = opmode;     /* record operating mode */

        OS_MARK(ah, AH_MARK_RESET_DONE, 0);

        /*
         * disable seq number generation in hw
         */
        OS_REG_WRITE(ah, AR_STA_ID1,
                     OS_REG_READ(ah, AR_STA_ID1) | AR_STA_ID1_PRESERVE_SEQNUM);

        ar5416SetDma(ah);

        /*
         * program OBS bus to see MAC interrupts
         */
        OS_REG_WRITE(ah, AR_OBS, 8);

        /*
         * GTT debug mode setting
         */
        // OS_REG_WRITE(ah, 0x64, 0x00320000);
        // OS_REG_WRITE(ah, 0x68, 7);
        // OS_REG_WRITE(ah, 0x4080, 0xC);

       /*
        * Disable general interrupt mitigation by setting MIRT = 0x0
        * Rx and tx interrupt mitigation are conditionally enabled below.
        */
        OS_REG_WRITE(ah, AR_MIRT, 0);
        if (ahp->ah_intrMitigationRx) {
        /* 
         * Enable Interrupt Mitigation for Rx.
         * If no build-specific limits for the rx interrupt mitigation
         * timer have been specified, use conservative defaults.
         */
#ifndef AH_RIMT_VAL_LAST
    #define AH_RIMT_LAST_MICROSEC 500
#endif
#ifndef AH_RIMT_VAL_FIRST
    #define AH_RIMT_FIRST_MICROSEC 2000
#endif
            OS_REG_RMW_FIELD(ah, AR_RIMT, AR_RIMT_LAST, AH_RIMT_LAST_MICROSEC);
            OS_REG_RMW_FIELD(ah, AR_RIMT, AR_RIMT_FIRST, AH_RIMT_FIRST_MICROSEC);
        }
        if (ahp->ah_intrMitigationTx) {
        /* 
         * Enable Interrupt Mitigation for Tx.
         * If no build-specific limits for the tx interrupt mitigation
         * timer have been specified, use the values preferred for
         * the carrier group's products.
         */
#ifndef AH_TIMT_LAST
    #define AH_TIMT_LAST_MICROSEC 300
#endif
#ifndef AH_TIMT_FIRST
    #define AH_TIMT_FIRST_MICROSEC 750
#endif
            OS_REG_RMW_FIELD(ah, AR_TIMT, AR_TIMT_LAST, AH_TIMT_LAST_MICROSEC);
            OS_REG_RMW_FIELD(ah, AR_TIMT, AR_TIMT_FIRST, AH_TIMT_FIRST_MICROSEC);
        }

        ar5416InitBB(ah, chan);

        if (!ar5416InitCal(ah, chan))
            FAIL(HAL_ESELFTEST);

        /*
         * WAR for owl 1.0 - restore chain mask for 2-chain cfgs after cal
         */
        rx_chainmask = ahp->ah_rxchainmask;
        if ((rx_chainmask == 0x5) || (rx_chainmask == 0x3)) {
            OS_REG_WRITE(ah, AR_PHY_RX_CHAINMASK, rx_chainmask);
            OS_REG_WRITE(ah, AR_PHY_CAL_CHAINMASK, rx_chainmask);
        }

        if(AR_SREV_HOWL(ah)) {
            OS_REG_WRITE(ah, AR_CFG_LED,
                (AR_CFG_LED_ASSOC_ACTIVE << AR_CFG_LED_ASSOC_CTL_S) | AR_CFG_SCLK_32KHZ);
        } else {
            /* Restore previous led state */
            OS_REG_WRITE(ah, AR_CFG_LED, saveLedState | AR_CFG_SCLK_32KHZ);
        }

        /* 
         * Restore GPIO configuration and MUX1 state.
         * Be careful not to overwrite the setting for GPIO pin 0, \
         * used for RF Kill.
         */
#if 0
        if (! AH_PRIVATE(ah)->ah_isPciExpress) {
            ar5416GpioCfgOutput(ah, 1, HAL_GPIO_OUTPUT_MUX_AS_NETWORK_LED);
            ar5416GpioCfgOutput(ah, 2, HAL_GPIO_OUTPUT_MUX_AS_POWER_LED);
        }
#endif
#ifdef ATH_BT_COEX
        if (ahp->ah_btCoexEnabled) {
            ar5416InitBTCoex(ah);
        }
#endif

        /* MIMO Power save setting */
        if ((ar5416GetCapability(ah, HAL_CAP_DYNAMIC_SMPS, 0, AH_NULL) == HAL_OK)) {
            ar5416SetSmPowerMode(ah, ahp->ah_smPowerMode);
        }

        /*
         * For big endian systems turn on swapping for descriptors
         */
    if(AR_SREV_HOWL(ah)) {
        u_int32_t mask;
        mask = OS_REG_READ(ah, AR_CFG);
        if(mask & (AR_CFG_SWRB | AR_CFG_SWTB | AR_CFG_SWRG)) {
            HDPRINTF(ah, HAL_DBG_RESET, "%s CFG Byte Swap Set 0x%x\n",
                    __func__, mask);
        } else {
            mask = INIT_CONFIG_STATUS | AR_CFG_SWRB | AR_CFG_SWTB;
            OS_REG_WRITE(ah, AR_CFG, mask);
            HDPRINTF(ah, HAL_DBG_RESET, "%s Setting CFG 0x%x\n",
                    __func__, OS_REG_READ(ah, AR_CFG));
        }
    } else {
#if AH_BYTE_ORDER == AH_BIG_ENDIAN
        OS_REG_WRITE(ah, AR_CFG, AR_CFG_SWTD | AR_CFG_SWRD);
#endif
    }

    if (AR_SREV_HOWL(ah)) {
        /* Enable the MBSSID block-ack fix for HOWL */
        unsigned int reg;
        reg = (OS_REG_READ(ah, AR_STA_ID1) | (1<<22));
        OS_REG_WRITE(ah, AR_STA_ID1, reg);
    }

        chan->channelFlags = ichan->channelFlags;
        chan->privFlags = ichan->privFlags;
        return AH_TRUE;
bad:
        OS_MARK(ah, AH_MARK_RESET_DONE, ecode);
        if (status)
                *status = ecode;
        return AH_FALSE;
#undef FAIL
}

/**************************************************************
 * ar5416ChannelChange
 * Assumes caller wants to change channel, and not reset.
 */
static inline HAL_BOOL
ar5416ChannelChange(struct ath_hal *ah, HAL_CHANNEL *chan,
                    HAL_CHANNEL_INTERNAL *ichan, HAL_HT_MACMODE macmode)
{
    u_int32_t synthDelay, qnum;
    struct ath_hal_5416 *ahp = AH5416(ah);

    /* TX must be stopped by now */
    for (qnum = 0; qnum < AR_NUM_QCU; qnum++) {
        if (ar5416NumTxPending(ah, qnum)) {
            HDPRINTF(ah, HAL_DBG_QUEUE, "%s: Transmit frames pending on queue %d\n", __func__, qnum);
            HALASSERT(0);
            return AH_FALSE;
        }
    }

    /*
     * Kill last Baseband Rx Frame - Request analog bus grant
     */
    OS_REG_WRITE(ah, AR_PHY_RFBUS_REQ, AR_PHY_RFBUS_REQ_EN);
    if (!ath_hal_wait(ah, AR_PHY_RFBUS_GRANT, AR_PHY_RFBUS_GRANT_EN,
                          AR_PHY_RFBUS_GRANT_EN, AH_WAIT_TIMEOUT)) {
        HDPRINTF(ah, HAL_DBG_PHY_IO, "%s: Could not kill baseband RX\n", __func__);
        return AH_FALSE;
    }

    /* Setup 11n MAC/Phy mode registers */
    ar5416Set11nRegs(ah, chan, macmode);

    /*
     * Change the synth
     */
    if (!ahp->ah_rfHal.setChannel(ah, ichan)) {
        HDPRINTF(ah, HAL_DBG_CHANNEL, "%s: failed to set channel\n", __func__);
        return AH_FALSE;
    }

    /*
     * Setup the transmit power values.
     *
     * After the public to private hal channel mapping, ichan contains the
     * valid regulatory power value.
     * ath_hal_getctl and ath_hal_getantennaallowed look up ichan from chan.
     */
    if (ar5416EepromSetTransmitPower(ah, &ahp->ah_eeprom, ichan,
         ath_hal_getctl(ah,chan), ath_hal_getantennaallowed(ah, chan),
         ichan->maxRegTxPower * 2,
         AH_MIN(MAX_RATE_POWER, AH_PRIVATE(ah)->ah_powerLimit)) != HAL_OK) {
        HDPRINTF(ah, HAL_DBG_EEPROM, "%s: error init'ing transmit power\n", __func__);
        return AH_FALSE;
    }

    /*
     * Wait for the frequency synth to settle (synth goes on via PHY_ACTIVE_EN).
     * Read the phy active delay register. Value is in 100ns increments.
     */
    synthDelay = OS_REG_READ(ah, AR_PHY_RX_DELAY) & AR_PHY_RX_DELAY_DELAY;
    if (IS_CHAN_CCK(chan)) {
        synthDelay = (4 * synthDelay) / 22;
    } else {
        synthDelay /= 10;
    }

    OS_DELAY(synthDelay + BASE_ACTIVATE_DELAY);

    /*
     * Release the RFBus Grant.
     */
     OS_REG_WRITE(ah, AR_PHY_RFBUS_REQ, 0);

    /*
     * Calibrations need to be triggered after RFBus Grant is released.
     * Otherwise, cals may not be able to complete.
     */
    if (!ichan->oneTimeCalsDone) {
        /*
         * Start offset and carrier leak cals
         */
    }

    /*
     * Write spur immunity and delta slope for OFDM enabled modes (A, G, Turbo)
     */
    if (IS_CHAN_OFDM(chan)|| IS_CHAN_HT(chan)) {
        ar5416SetDeltaSlope(ah, ichan);
    }

    /*
     * For Merlin, spur can break CCK MRC algorithm. SpurMitigate needs to
     * be called in all 11A/B/G/HT modes to disable CCK MRC if spur is found
     * in this channel.
     */
    if (AR_SREV_MERLIN_10_OR_LATER(ah))
        ar9280SpurMitigate(ah, chan, ichan);
    else
        ar5416SpurMitigate(ah, chan);

    if (!ichan->oneTimeCalsDone) {
        /*
         * wait for end of offset and carrier leak cals
         */
        ichan->oneTimeCalsDone = AH_TRUE;
    }

#if 0
    if (chan->channelFlags & CHANNEL_108G)
        ar5416ArEnable(ah);
    else if ((chan->channelFlags &
             (CHANNEL_A|CHANNEL_ST|CHANNEL_A_HT20| CHANNEL_A_HT40))
             && (chan->privFlags & CHANNEL_DFS))
        ar5416RadarEnable(ah);
#endif

    return AH_TRUE;
}

static void
ar5416Set11nRegs(struct ath_hal *ah, HAL_CHANNEL *chan, HAL_HT_MACMODE macmode)
{
    u_int32_t phymode;
    u_int32_t enableDacFifo = 0;
    struct ath_hal_5416 *ahp = AH5416(ah);

    if (AR_SREV_KITE_10_OR_LATER(ah)) {
        enableDacFifo = (OS_REG_READ(ah, AR_PHY_TURBO) & AR_PHY_FC_ENABLE_DAC_FIFO);
    }

    /* Enable 11n HT, 20 MHz */
    phymode = AR_PHY_FC_HT_EN | AR_PHY_FC_SHORT_GI_40
              | AR_PHY_FC_SINGLE_HT_LTF1 | AR_PHY_FC_WALSH | enableDacFifo;

    /* Configure baseband for dynamic 20/40 operation */
    if (IS_CHAN_HT40(chan)) {
        phymode |= AR_PHY_FC_DYN2040_EN;

        /* Configure control (primary) channel at +-10MHz */
        if (chan->channelFlags & CHANNEL_HT40PLUS) {
            phymode |= AR_PHY_FC_DYN2040_PRI_CH;
        }

        /* Configure 20/25 spacing */
        if (ahp->ah_extprotspacing == HAL_HT_EXTPROTSPACING_25) {
            phymode |= AR_PHY_FC_DYN2040_EXT_CH;
        }
    }
    OS_REG_WRITE(ah, AR_PHY_TURBO, phymode);

    /* Configure MAC for 20/40 operation */
    ar5416Set11nMac2040(ah, macmode);

#ifdef NOT_YET
    if(AR_SREV_HOWL(ah) && (IS_CHAN_5GHZ(chan) ||
            (IS_CHAN_2GHZ(chan) && ((chan->channelFlags & CHANNEL_HT40PLUS)
            ||(chan->channelFlags & CHANNEL_HT40MINUS))))) {
        OS_REG_WRITE(ah, AR_PHY_ADC_CTL, 0x00022002);
        OS_REG_WRITE(ah, AR_PHY_M_SLEEP, 0x00000003);
        OS_REG_WRITE(ah, AR_PHY_TIMING_CTRL4(0), 0x00016000);

        HDPRINTF(ah, HAL_DBG_CALIBRATE, "IQ cal\n");
        if (!ath_hal_wait(ah, AR_PHY_TIMING_CTRL4(0),
            AR_PHY_TIMING_CTRL4_DO_CAL, 0, AH_WAIT_TIMEOUT)) {
            HDPRINTF(ah, HAL_DBG_CALIBRATE, "IQ cal failed\n");
        }
    }
#endif

    /* global transmit timeout (25 TUs default)*/
    /* XXX - put this elsewhere??? */
    OS_REG_WRITE(ah, AR_GTXTO, 25 << AR_GTXTO_TIMEOUT_LIMIT_S) ;

    /* carrier sense timeout */
    OS_REG_WRITE(ah, AR_CST, 0xF << AR_CST_TIMEOUT_LIMIT_S);
}

void
ar5416SetOperatingMode(struct ath_hal *ah, int opmode)
{
        u_int32_t val;

        val = OS_REG_READ(ah, AR_STA_ID1);
        val &= ~(AR_STA_ID1_STA_AP | AR_STA_ID1_ADHOC);
        switch (opmode) {
        case HAL_M_HOSTAP:
                OS_REG_WRITE(ah, AR_STA_ID1, val | AR_STA_ID1_STA_AP
                                        | AR_STA_ID1_KSRCH_MODE);
                OS_REG_CLR_BIT(ah, AR_CFG, AR_CFG_AP_ADHOC_INDICATION);
                break;
        case HAL_M_IBSS:
                OS_REG_WRITE(ah, AR_STA_ID1, val | AR_STA_ID1_ADHOC
                                        | AR_STA_ID1_KSRCH_MODE);
                OS_REG_SET_BIT(ah, AR_CFG, AR_CFG_AP_ADHOC_INDICATION);
                break;
        case HAL_M_STA:
        case HAL_M_MONITOR:
                OS_REG_WRITE(ah, AR_STA_ID1, val | AR_STA_ID1_KSRCH_MODE);
                break;
        }
}

/*
 * Places the PHY and Radio chips into reset.  A full reset
 * must be called to leave this state.  The PCI/MAC/PCU are
 * not placed into reset as we must receive interrupt to
 * re-enable the hardware.
 */
HAL_BOOL
ar5416PhyDisable(struct ath_hal *ah)
{
    return ar5416SetResetReg(ah, HAL_RESET_WARM);
}

/*
 * Places all of hardware into reset
 */
HAL_BOOL
ar5416Disable(struct ath_hal *ah)
{
    if (!ar5416SetPowerMode(ah, HAL_PM_AWAKE, AH_TRUE))
        return AH_FALSE;

    return ar5416SetResetReg(ah, HAL_RESET_COLD);
}

/*
 * Places the hardware into reset and then pulls it out of reset
 */
HAL_BOOL
ar5416ChipReset(struct ath_hal *ah, HAL_CHANNEL *chan)
{

        struct ath_hal_5416     *ahp = AH5416(ah);
        OS_MARK(ah, AH_MARK_CHIPRESET, chan ? chan->channel : 0);

        /*
         * Warm reset is optimistic.
         */
        if (AR_SREV_MERLIN_20_OR_LATER(ah) && ar5416EepromGet(ahp, EEP_OL_PWRCTRL)) {
            if (!ar5416SetResetReg(ah, HAL_RESET_POWER_ON))
                return AH_FALSE;
        } else {
        if (!ar5416SetResetReg(ah, HAL_RESET_WARM))
                return AH_FALSE;
        }

        /* Bring out of sleep mode (AGAIN) */
        if (!ar5416SetPowerMode(ah, HAL_PM_AWAKE, AH_TRUE))
                return AH_FALSE;

        ahp->ah_chipFullSleep = AH_FALSE;

        ar5416InitPLL(ah, chan);

        /*
         * Perform warm reset before the mode/PLL/turbo registers
         * are changed in order to deactivate the radio.  Mode changes
         * with an active radio can result in corrupted shifts to the
         * radio device.
         */
        ar5416SetRfMode(ah, chan);

        return AH_TRUE;
}

/* ar5416SetupCalibration
 * Setup HW to collect samples used for current cal
 */
inline void
ar5416SetupCalibration(struct ath_hal *ah, HAL_CAL_LIST *currCal)
{
    /* Start calibration w/ 2^(INIT_IQCAL_LOG_COUNT_MAX+1) samples */
    OS_REG_RMW_FIELD(ah, AR_PHY_TIMING_CTRL4(0),
                     AR_PHY_TIMING_CTRL4_IQCAL_LOG_COUNT_MAX,
                     currCal->calData->calCountMax);

    /* Select calibration to run */
    switch(currCal->calData->calType) {
    case IQ_MISMATCH_CAL:
        OS_REG_WRITE(ah, AR_PHY_CALMODE, AR_PHY_CALMODE_IQ);
        HDPRINTF(ah, HAL_DBG_CALIBRATE,
                 "%s: starting IQ Mismatch Calibration\n", __func__);
        break;
    case ADC_GAIN_CAL:
        OS_REG_WRITE(ah, AR_PHY_CALMODE, AR_PHY_CALMODE_ADC_GAIN);
        HDPRINTF(ah, HAL_DBG_CALIBRATE,
                 "%s: starting ADC Gain Calibration\n", __func__);
        break;
    case ADC_DC_CAL:
        OS_REG_WRITE(ah, AR_PHY_CALMODE, AR_PHY_CALMODE_ADC_DC_PER);
        HDPRINTF(ah, HAL_DBG_CALIBRATE,
                 "%s: starting ADC DC Calibration\n", __func__);
        break;
    case ADC_DC_INIT_CAL:
        OS_REG_WRITE(ah, AR_PHY_CALMODE, AR_PHY_CALMODE_ADC_DC_INIT);
        HDPRINTF(ah, HAL_DBG_CALIBRATE,
                 "%s: starting Init ADC DC Calibration\n", __func__);
        break;
    }

    /* Kick-off cal */
    OS_REG_SET_BIT(ah, AR_PHY_TIMING_CTRL4(0), AR_PHY_TIMING_CTRL4_DO_CAL);
}

/* ar5416ResetCalibration
 * Initialize shared data structures and prepare a cal to be run.
 */
inline void
ar5416ResetCalibration(struct ath_hal *ah, HAL_CAL_LIST *currCal)
{
    struct ath_hal_5416 *ahp = AH5416(ah);
    int i;

    /* Setup HW for new calibration */
    ar5416SetupCalibration(ah, currCal);

    /* Change SW state to RUNNING for this calibration */
    currCal->calState = CAL_RUNNING;

    /* Reset data structures shared between different calibrations */
    for(i = 0; i < AR5416_MAX_CHAINS; i++) {
        ahp->ah_Meas0.sign[i] = 0;
        ahp->ah_Meas1.sign[i] = 0;
        ahp->ah_Meas2.sign[i] = 0;
        ahp->ah_Meas3.sign[i] = 0;
    }

    ahp->ah_CalSamples = 0;
}

/* ar5416Calibration
 * Wrapper for a more generic Calibration routine. Primarily to abstract to
 * upper layers whether there is 1 or more calibrations to be run.
 */
HAL_BOOL
ar5416Calibration(struct ath_hal *ah,  HAL_CHANNEL *chan, u_int8_t rxchainmask,
                  HAL_BOOL longcal, HAL_BOOL *isCalDone)
{
    struct ath_hal_5416 *ahp = AH5416(ah);
    HAL_CAL_LIST *currCal = ahp->ah_cal_list_curr;
    HAL_CHANNEL_INTERNAL *ichan = ath_hal_checkchannel(ah, chan);

    *isCalDone = AH_TRUE;

    /* Invalid channel check */
    if (ichan == AH_NULL) {
        HDPRINTF(ah, HAL_DBG_CHANNEL,
                 "%s: invalid channel %u/0x%x; no mapping\n",
                 __func__, chan->channel, chan->channelFlags);
        return AH_FALSE;
    }

    OS_MARK(ah, AH_MARK_PERCAL, chan->channel);

    /* For given calibration:
     * 1. Call generic cal routine
     * 2. When this cal is done (isCalDone) if we have more cals waiting
     *    (eg after reset), mask this to upper layers by not propagating
     *    isCalDone if it is set to TRUE.
     *    Instead, change isCalDone to FALSE and setup the waiting cal(s)
     *    to be run.
     */
    if (currCal &&
        (currCal->calState == CAL_RUNNING ||
         currCal->calState == CAL_WAITING)) {
        ar5416PerCalibration(ah, ichan, rxchainmask, currCal, isCalDone);

        if (*isCalDone == AH_TRUE) {
            ahp->ah_cal_list_curr = currCal = currCal->calNext;

            if (currCal->calState == CAL_WAITING) {
                *isCalDone = AH_FALSE;
                ar5416ResetCalibration(ah, currCal);
            }
        }
    }

    /* Do NF cal only at longer intervals */
    if (longcal) {

        if (AR_SREV_MERLIN_20_OR_LATER(ah) && ar5416EepromGet(ahp, EEP_OL_PWRCTRL)) {
            ar5416OpenLoopPowerControlTempCompensation(ah);
        }
        /* Get the value from the previous NF cal and update history buffer */
        ar5416GetNf(ah, ichan);
  
        /* 
         * Load the NF from history buffer of the current channel.
         * NF is slow time-variant, so it is OK to use a historical value.
         */
        ar5416LoadNF(ah, AH_PRIVATE(ah)->ah_curchan);

        /* start NF calibration, without updating BB NF register*/
        ar5416StartNFCal(ah);
  
        if ((ichan->channelFlags & CHANNEL_CW_INT) != 0) {
            /* report up and clear internal state */
            chan->channelFlags |= CHANNEL_CW_INT;
            ichan->channelFlags &= ~CHANNEL_CW_INT;
        }
    }

    return AH_TRUE;
}

/* ar5416IQCalCollect
 * Collect data from HW to later perform IQ Mismatch Calibration
 */
void
ar5416IQCalCollect(struct ath_hal *ah)
{
    struct ath_hal_5416 *ahp = AH5416(ah);
    int i;

    /* 
     * Accumulate IQ cal measures for active chains
     */
    for (i = 0; i < AR5416_MAX_CHAINS; i++) {
        ahp->ah_totalPowerMeasI[i] += OS_REG_READ(ah, AR_PHY_CAL_MEAS_0(i));
        ahp->ah_totalPowerMeasQ[i] += OS_REG_READ(ah, AR_PHY_CAL_MEAS_1(i));
        ahp->ah_totalIqCorrMeas[i] += (int32_t)OS_REG_READ(ah, 
                                               AR_PHY_CAL_MEAS_2(i));
        HDPRINTF(ah, HAL_DBG_CALIBRATE,
                 "%d: Chn %d pmi=0x%08x;pmq=0x%08x;iqcm=0x%08x;\n",
                 ahp->ah_CalSamples, i, ahp->ah_totalPowerMeasI[i],
                 ahp->ah_totalPowerMeasQ[i], ahp->ah_totalIqCorrMeas[i]);
    }
}

/* ar5416AdcGainCalCollect
 * Collect data from HW to later perform ADC Gain Calibration
 */
void
ar5416AdcGainCalCollect(struct ath_hal *ah)
{
    struct ath_hal_5416 *ahp = AH5416(ah);
    int i;

    /*
     * Accumulate ADC Gain cal measures for active chains
     */
    for (i = 0; i < AR5416_MAX_CHAINS; i++) {
        ahp->ah_totalAdcIOddPhase[i]  += OS_REG_READ(ah, AR_PHY_CAL_MEAS_0(i));
        ahp->ah_totalAdcIEvenPhase[i] += OS_REG_READ(ah, AR_PHY_CAL_MEAS_1(i));
        ahp->ah_totalAdcQOddPhase[i]  += OS_REG_READ(ah, AR_PHY_CAL_MEAS_2(i));
        ahp->ah_totalAdcQEvenPhase[i] += OS_REG_READ(ah, AR_PHY_CAL_MEAS_3(i));
 
        HDPRINTF(ah, HAL_DBG_CALIBRATE,
           "%d: Chn %d oddi=0x%08x; eveni=0x%08x; oddq=0x%08x; evenq=0x%08x;\n",
           ahp->ah_CalSamples, i, ahp->ah_totalAdcIOddPhase[i],
           ahp->ah_totalAdcIEvenPhase[i], ahp->ah_totalAdcQOddPhase[i],
           ahp->ah_totalAdcQEvenPhase[i]);
    }
}

void
ar5416AdcDcCalCollect(struct ath_hal *ah)
{
    struct ath_hal_5416 *ahp = AH5416(ah);
    int i;

    for (i = 0; i < AR5416_MAX_CHAINS; i++) {
        ahp->ah_totalAdcDcOffsetIOddPhase[i] +=
                               (int32_t) OS_REG_READ(ah, AR_PHY_CAL_MEAS_0(i));
        ahp->ah_totalAdcDcOffsetIEvenPhase[i] +=
                               (int32_t) OS_REG_READ(ah, AR_PHY_CAL_MEAS_1(i));
        ahp->ah_totalAdcDcOffsetQOddPhase[i] +=
                               (int32_t) OS_REG_READ(ah, AR_PHY_CAL_MEAS_2(i));
        ahp->ah_totalAdcDcOffsetQEvenPhase[i] +=
                               (int32_t) OS_REG_READ(ah, AR_PHY_CAL_MEAS_3(i));
                
        HDPRINTF(ah, HAL_DBG_CALIBRATE,
           "%d: Chn %d oddi=0x%08x; eveni=0x%08x; oddq=0x%08x; evenq=0x%08x;\n",
           ahp->ah_CalSamples, i,
           ahp->ah_totalAdcDcOffsetIOddPhase[i],
           ahp->ah_totalAdcDcOffsetIEvenPhase[i],
           ahp->ah_totalAdcDcOffsetQOddPhase[i],
           ahp->ah_totalAdcDcOffsetQEvenPhase[i]);
    }
}

/* ar5416IQCalibration
 * Use HW data to perform IQ Mismatch Calibration
 */
void
ar5416IQCalibration(struct ath_hal *ah, u_int8_t numChains)
{
    struct ath_hal_5416 *ahp = AH5416(ah);
    u_int32_t powerMeasQ, powerMeasI, iqCorrMeas;
    u_int32_t qCoffDenom, iCoffDenom;
    int32_t qCoff, iCoff;
    int iqCorrNeg, i;

    for (i = 0; i < numChains; i++) {
        powerMeasI = ahp->ah_totalPowerMeasI[i];
        powerMeasQ = ahp->ah_totalPowerMeasQ[i];
        iqCorrMeas = ahp->ah_totalIqCorrMeas[i];
 
        HDPRINTF(ah, HAL_DBG_CALIBRATE,  
                 "Starting IQ Cal and Correction for Chain %d\n", i);

        HDPRINTF(ah, HAL_DBG_CALIBRATE,
                 "Orignal: Chn %diq_corr_meas = 0x%08x\n", 
                 i, ahp->ah_totalIqCorrMeas[i]);

        iqCorrNeg = 0;
 
        /* iqCorrMeas is always negative. */ 
        if (iqCorrMeas > 0x80000000)  {
            iqCorrMeas = (0xffffffff - iqCorrMeas) + 1;
            iqCorrNeg = 1;
        }
  
        HDPRINTF(ah, HAL_DBG_CALIBRATE, "Chn %d pwr_meas_i = 0x%08x\n", 
                 i, powerMeasI);
        HDPRINTF(ah, HAL_DBG_CALIBRATE, "Chn %d pwr_meas_q = 0x%08x\n", 
                 i, powerMeasQ);
        HDPRINTF(ah, HAL_DBG_CALIBRATE, "iqCorrNeg is 0x%08x\n", iqCorrNeg);
  
        iCoffDenom = (powerMeasI/2 + powerMeasQ/2)/ 128;
        qCoffDenom = powerMeasQ / 64;
        /* Protect against divide-by-0 */
        if (powerMeasQ != 0) {
            /* IQ corr_meas is already negated if iqcorr_neg == 1 */
            iCoff = iqCorrMeas/iCoffDenom;
            qCoff = powerMeasI/qCoffDenom - 64;
            HDPRINTF(ah, HAL_DBG_CALIBRATE, "Chn %d iCoff = 0x%08x\n",
                     i, iCoff);
            HDPRINTF(ah, HAL_DBG_CALIBRATE, "Chn %d qCoff = 0x%08x\n",
                     i, qCoff);
                                 
            /* Negate iCoff if iqCorrNeg == 0 */
            iCoff = iCoff & 0x3f;
            HDPRINTF(ah, HAL_DBG_CALIBRATE, "New: Chn %d iCoff = 0x%08x\n", i,
                     iCoff);
            if (iqCorrNeg == 0x0) {
                iCoff = 0x40 - iCoff;
            }
                                 
            if (qCoff > 15) {
                qCoff = 15;
            } else if (qCoff <= -16) {
                qCoff = 16;
            }

            HDPRINTF(ah, HAL_DBG_CALIBRATE,
                     "Chn %d : iCoff = 0x%x  qCoff = 0x%x\n", i, iCoff, qCoff);
                                 
            OS_REG_RMW_FIELD(ah, AR_PHY_TIMING_CTRL4(i),
                             AR_PHY_TIMING_CTRL4_IQCORR_Q_I_COFF, iCoff);
            OS_REG_RMW_FIELD(ah, AR_PHY_TIMING_CTRL4(i),
                             AR_PHY_TIMING_CTRL4_IQCORR_Q_Q_COFF, qCoff);
            HDPRINTF(ah, HAL_DBG_CALIBRATE,  
                     "IQ Cal and Correction done for Chain %d\n", i);
        }
    }

    OS_REG_SET_BIT(ah, AR_PHY_TIMING_CTRL4(0), 
                   AR_PHY_TIMING_CTRL4_IQCORR_ENABLE);
}
                                 
/* ar5416AdcGainCalibration
 * Use HW data to perform ADC Gain Calibration
 */
void
ar5416AdcGainCalibration(struct ath_hal *ah, u_int8_t numChains)
{
    struct ath_hal_5416 *ahp = AH5416(ah);
    u_int32_t iOddMeasOffset, iEvenMeasOffset, qOddMeasOffset, qEvenMeasOffset;
    u_int32_t qGainMismatch, iGainMismatch, val, i;

    for (i = 0; i < numChains; i++) {
        iOddMeasOffset  = ahp->ah_totalAdcIOddPhase[i];
        iEvenMeasOffset = ahp->ah_totalAdcIEvenPhase[i];
        qOddMeasOffset  = ahp->ah_totalAdcQOddPhase[i];
        qEvenMeasOffset = ahp->ah_totalAdcQEvenPhase[i];

        HDPRINTF(ah, HAL_DBG_CALIBRATE,  
                 "Starting ADC Gain Cal for Chain %d\n", i);

        HDPRINTF(ah, HAL_DBG_CALIBRATE, "Chn %d pwr_meas_odd_i = 0x%08x\n",
                 i, iOddMeasOffset);
        HDPRINTF(ah, HAL_DBG_CALIBRATE, "Chn %d pwr_meas_even_i = 0x%08x\n",
                 i, iEvenMeasOffset);
        HDPRINTF(ah, HAL_DBG_CALIBRATE, "Chn %d pwr_meas_odd_q = 0x%08x\n",
                 i, qOddMeasOffset);
        HDPRINTF(ah, HAL_DBG_CALIBRATE, "Chn %d pwr_meas_even_q = 0x%08x\n",
                 i, qEvenMeasOffset);

        if (iOddMeasOffset != 0 && qEvenMeasOffset != 0) {
            iGainMismatch = ((iEvenMeasOffset*32)/iOddMeasOffset) & 0x3f;
            qGainMismatch = ((qOddMeasOffset*32)/qEvenMeasOffset) & 0x3f;

            HDPRINTF(ah, HAL_DBG_CALIBRATE, "Chn %d gain_mismatch_i = 0x%08x\n",
                     i, iGainMismatch);
            HDPRINTF(ah, HAL_DBG_CALIBRATE, "Chn %d gain_mismatch_q = 0x%08x\n",
                     i, qGainMismatch);

            val = OS_REG_READ(ah, AR_PHY_NEW_ADC_DC_GAIN_CORR(i));
            val &= 0xfffff000;
            val |= (qGainMismatch) | (iGainMismatch << 6);
            OS_REG_WRITE(ah, AR_PHY_NEW_ADC_DC_GAIN_CORR(i), val); 

            HDPRINTF(ah, HAL_DBG_CALIBRATE,  
                     "ADC Gain Cal done for Chain %d\n", i);
        }
    }

    OS_REG_WRITE(ah, AR_PHY_NEW_ADC_DC_GAIN_CORR(0),
                 OS_REG_READ(ah, AR_PHY_NEW_ADC_DC_GAIN_CORR(0)) |
                 AR_PHY_NEW_ADC_GAIN_CORR_ENABLE);
}

void
ar5416AdcDcCalibration(struct ath_hal *ah, u_int8_t numChains)
{
    struct ath_hal_5416 *ahp = AH5416(ah);
    u_int32_t iOddMeasOffset, iEvenMeasOffset, val, i;
    int32_t   qOddMeasOffset, qEvenMeasOffset, qDcMismatch, iDcMismatch;
    const HAL_PERCAL_DATA *calData = ahp->ah_cal_list_curr->calData;
    u_int32_t numSamples = (1 << (calData->calCountMax + 5)) *
                            calData->calNumSamples;

    for (i = 0; i < numChains; i++) {
        iOddMeasOffset  = ahp->ah_totalAdcDcOffsetIOddPhase[i];
        iEvenMeasOffset = ahp->ah_totalAdcDcOffsetIEvenPhase[i];
        qOddMeasOffset  = ahp->ah_totalAdcDcOffsetQOddPhase[i];
        qEvenMeasOffset = ahp->ah_totalAdcDcOffsetQEvenPhase[i];

        HDPRINTF(ah, HAL_DBG_CALIBRATE,  
                 "Starting ADC DC Offset Cal for Chain %d\n", i);

        HDPRINTF(ah, HAL_DBG_CALIBRATE, "Chn %d pwr_meas_odd_i = %d\n",
                 i, iOddMeasOffset);
        HDPRINTF(ah, HAL_DBG_CALIBRATE, "Chn %d pwr_meas_even_i = %d\n",
                 i, iEvenMeasOffset);
        HDPRINTF(ah, HAL_DBG_CALIBRATE, "Chn %d pwr_meas_odd_q = %d\n",
                 i, qOddMeasOffset);
        HDPRINTF(ah, HAL_DBG_CALIBRATE, "Chn %d pwr_meas_even_q = %d\n",
                 i, qEvenMeasOffset);

        HALASSERT(numSamples);

        iDcMismatch = (((iEvenMeasOffset - iOddMeasOffset) * 2) /
                        numSamples) & 0x1ff;
        qDcMismatch = (((qOddMeasOffset - qEvenMeasOffset) * 2) /
                        numSamples) & 0x1ff;

        HDPRINTF(ah, HAL_DBG_CALIBRATE,"Chn %d dc_offset_mismatch_i = 0x%08x\n",
                 i, iDcMismatch);
        HDPRINTF(ah, HAL_DBG_CALIBRATE,"Chn %d dc_offset_mismatch_q = 0x%08x\n",
                 i, qDcMismatch);

        val = OS_REG_READ(ah, AR_PHY_NEW_ADC_DC_GAIN_CORR(i));
        val &= 0xc0000fff;
        val |= (qDcMismatch << 12) |
               (iDcMismatch << 21);
        OS_REG_WRITE(ah, AR_PHY_NEW_ADC_DC_GAIN_CORR(i), val); 

        HDPRINTF(ah, HAL_DBG_CALIBRATE,  
                 "ADC DC Offset Cal done for Chain %d\n", i);
    }

    OS_REG_WRITE(ah, AR_PHY_NEW_ADC_DC_GAIN_CORR(0),
                 OS_REG_READ(ah, AR_PHY_NEW_ADC_DC_GAIN_CORR(0)) |
                 AR_PHY_NEW_ADC_DC_OFFSET_CORR_ENABLE);
}

/* ar5416PerCalibration
 * Generic calibration routine.
 * Recalibrate the lower PHY chips to account for temperature/environment
 * changes.
 */
inline void
ar5416PerCalibration(struct ath_hal *ah,  HAL_CHANNEL_INTERNAL *ichan, 
                     u_int8_t rxchainmask, HAL_CAL_LIST *currCal,
                     HAL_BOOL *isCalDone)
{
    struct ath_hal_5416 *ahp = AH5416(ah);

    /* Cal is assumed not done until explicitly set below */
    *isCalDone = AH_FALSE;

    /* Calibration in progress. */
    if (currCal->calState == CAL_RUNNING) {
        /* Check to see if it has finished. */
        if (!(OS_REG_READ(ah,
            AR_PHY_TIMING_CTRL4(0)) & AR_PHY_TIMING_CTRL4_DO_CAL)) {

            /* 
             * Accumulate cal measures for active chains
             */
            currCal->calData->calCollect(ah);

            ahp->ah_CalSamples++;

            if (ahp->ah_CalSamples >= currCal->calData->calNumSamples) {
                int i, numChains = 0;
                for (i = 0; i < AR5416_MAX_CHAINS; i++) {
                    if (rxchainmask & (1 << i)) {
                        numChains++;
                    }
                }
 
                /* 
                 * Process accumulated data
                 */
                currCal->calData->calPostProc(ah, numChains);

                /* Calibration has finished. */
                ichan->CalValid |= currCal->calData->calType;
                currCal->calState = CAL_DONE;
                *isCalDone = AH_TRUE;

           } else {
                /* Set-up collection of another sub-sample until we 
                 * get desired number
                 */
                ar5416SetupCalibration(ah, currCal);
            }
        }
    } else if (!(ichan->CalValid & currCal->calData->calType)) {
        /* If current cal is marked invalid in channel, kick it off */
        ar5416ResetCalibration(ah, currCal);
    }

}

/*
 * Write the given reset bit mask into the reset register
 */
HAL_BOOL
ar5416SetResetReg(struct ath_hal *ah, u_int32_t type)
{
    /*
     * Set force wake
     */
    OS_REG_WRITE(ah, AR_RTC_FORCE_WAKE,
             AR_RTC_FORCE_WAKE_EN | AR_RTC_FORCE_WAKE_ON_INT);

    switch (type) {
    case HAL_RESET_POWER_ON:
        return ar5416SetResetPowerOn(ah);
        break;
    case HAL_RESET_WARM:
    case HAL_RESET_COLD:
        return ar5416SetReset(ah, type);
        break;
    default:
        return AH_FALSE;
    }
}

static inline HAL_BOOL
ar5416SetResetPowerOn(struct ath_hal *ah)
{
    /* Force wake */
    OS_REG_WRITE(ah, AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN |
                                     AR_RTC_FORCE_WAKE_ON_INT);
    /*
     * RTC reset and clear
     */
    OS_REG_WRITE(ah, AR_RTC_RESET, 0);
    OS_DELAY(2);
    OS_REG_WRITE(ah, AR_RTC_RESET, 1);

    /*
     * Poll till RTC is ON
     */
    if (!ath_hal_wait(ah, AR_RTC_STATUS, AR_RTC_STATUS_M, AR_RTC_STATUS_ON,
        AH_WAIT_TIMEOUT)) {
        HDPRINTF(ah, HAL_DBG_RESET, "%s: RTC not waking up\n", __FUNCTION__);
        return AH_FALSE;
    }

    /* 
     * Read Revisions from Chip right after RTC is on for the first time.
     * This helps us detect the chip type early and initialize it accordingly.
     */
    ar5416ReadRevisions(ah);

    if (AR_SREV_HOWL(ah)) {
        /* Howl needs additional AHB-WMAC interface reset in this case */
        return ar5416SetReset(ah, HAL_RESET_COLD);
    } else {
        /*
         * Warm reset if we aren't really powering on,
         * just restarting the driver.
         */
        return ar5416SetReset(ah, HAL_RESET_WARM);
    }
}

static inline HAL_BOOL
ar5416SetReset(struct ath_hal *ah, int type)
{
    u_int32_t rst_flags;
    u_int32_t tmpReg;
    
#ifdef AH_ASSERT
    if (type != HAL_RESET_WARM || type != HAL_RESET_COLD) {
        HALASSERT(0);
    }
#endif

#ifdef AR9100
    if (AR_SREV_HOWL(ah)) {
        u_int32_t val = OS_REG_READ(ah, AR_RTC_DERIVED_CLK);
        val &= ~AR_RTC_DERIVED_CLK_PERIOD;
        val |= SM(1, AR_RTC_DERIVED_CLK_PERIOD);
        OS_REG_WRITE(ah, AR_RTC_DERIVED_CLK, val);
        (void)OS_REG_READ(ah, AR_RTC_DERIVED_CLK);
    }
#endif

    /*
     * RTC Force wake should be done before resetting the MAC.
     * MDK/ART does it that way.
     */
    OS_REG_WRITE(ah, AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN |
                                     AR_RTC_FORCE_WAKE_ON_INT);

#ifdef AR9100
    if (AR_SREV_HOWL(ah)) {
        /*
         * Howl has a reset module and without resetting that,
         * the AHB - WMAC module can't be reset. Hence, the
         * additional flags to reset the Reset control module
         * when doing a MAC reset (this is inline with MDK/ART
         * testing).
         */
        rst_flags = AR_RTC_RC_MAC_WARM | AR_RTC_RC_MAC_COLD |
                    AR_RTC_RC_COLD_RESET | AR_RTC_RC_WARM_RESET;
    } else {
#endif

        /* Reset AHB */
        /* Bug26871 */
        tmpReg = OS_REG_READ(ah, AR_INTR_SYNC_CAUSE);
        if (tmpReg & (AR_INTR_SYNC_LOCAL_TIMEOUT|AR_INTR_SYNC_RADM_CPL_TIMEOUT)) {
            OS_REG_WRITE(ah, AR_INTR_SYNC_ENABLE, 0);
            OS_REG_WRITE(ah, AR_RC, AR_RC_AHB|AR_RC_HOSTIF);
        }
        else {
            OS_REG_WRITE(ah, AR_RC, AR_RC_AHB);
        }

        rst_flags = AR_RTC_RC_MAC_WARM;
        if (type == HAL_RESET_COLD) {
            rst_flags |= AR_RTC_RC_MAC_COLD;
        }
#ifdef AR9100
    }
#endif

    /*
     * Set Mac(BB,Phy) Warm Reset
     */
    OS_REG_WRITE(ah, AR_RTC_RC, rst_flags);

    if (AR_SREV_HOWL(ah)) {
       OS_DELAY( 10000); /* 10 msec */
    } else {
       OS_DELAY(50); /* 50 usec */
    }

    /*
     * Clear resets and force wakeup
     */
    OS_REG_WRITE(ah, AR_RTC_RC, 0);
    if (!ath_hal_wait(ah, AR_RTC_RC, AR_RTC_RC_M, 0, AH_WAIT_TIMEOUT)) {
        HDPRINTF(ah, HAL_DBG_RESET, "%s: RTC stuck in MAC reset\n", __FUNCTION__);
        return AH_FALSE;
    }

    if (!AR_SREV_HOWL(ah)) {
        /* Clear AHB reset */
        OS_REG_WRITE(ah, AR_RC, 0);
    }

    ar5416AttachHwPlatform(ah);

    switch (ar5416Get11nHwPlatform(ah)) {
        case HAL_TRUE_CHIP:
            ar5416InitPLL(ah, AH_NULL);
            break;
        default:
            HALASSERT(0);
            break;
    }

#ifdef AR9100
    if (type != HAL_RESET_WARM) {
        /* Reset the AHB - WMAC interface in Howl. */
        ath_hal_ahb_mac_reset();
        OS_DELAY(50); /* 50 usec */
    }
#endif

    return AH_TRUE;
}

void
ar5416GetNoiseFloor(struct ath_hal *ah,
                    int16_t nfarray[NUM_NF_READINGS])
{
    int16_t nf;

    if (AR_SREV_MERLIN_10_OR_LATER(ah)) {
        nf = MS(OS_REG_READ(ah, AR_PHY_CCA), AR9280_PHY_MINCCA_PWR);
    } else {
        nf = MS(OS_REG_READ(ah, AR_PHY_CCA), AR_PHY_MINCCA_PWR);
    }
    if (nf & 0x100)
            nf = 0 - ((nf ^ 0x1ff) + 1);
    HDPRINTF(ah, HAL_DBG_NF_CAL, "NF calibrated [ctl] [chain 0] is %d\n", nf);
    nfarray[0] = nf;

    if (!AR_SREV_KITE(ah)) {
        if (AR_SREV_MERLIN_10_OR_LATER(ah)) {
            nf = MS(OS_REG_READ(ah, AR_PHY_CH1_CCA), AR9280_PHY_CH1_MINCCA_PWR);
        } else {
            nf = MS(OS_REG_READ(ah, AR_PHY_CH1_CCA), AR_PHY_CH1_MINCCA_PWR);
        }
        if (nf & 0x100)
                nf = 0 - ((nf ^ 0x1ff) + 1);
        HDPRINTF(ah, HAL_DBG_NF_CAL, "NF calibrated [ctl] [chain 1] is %d\n", nf);
        nfarray[1] = nf;

        if (!AR_SREV_MERLIN(ah) && !AR_SREV_KIWI(ah)) {
            nf = MS(OS_REG_READ(ah, AR_PHY_CH2_CCA), AR_PHY_CH2_MINCCA_PWR);
            if (nf & 0x100)
                nf = 0 - ((nf ^ 0x1ff) + 1);
            HDPRINTF(ah, HAL_DBG_NF_CAL, "NF calibrated [ctl] [chain 2] is %d\n", nf);
            nfarray[2] = nf;
        }
    }

    if (AR_SREV_MERLIN_10_OR_LATER(ah)) {
        nf = MS(OS_REG_READ(ah, AR_PHY_EXT_CCA), AR9280_PHY_EXT_MINCCA_PWR);
    } else {
        nf = MS(OS_REG_READ(ah, AR_PHY_EXT_CCA), AR_PHY_EXT_MINCCA_PWR);
    }
    if (nf & 0x100)
            nf = 0 - ((nf ^ 0x1ff) + 1);
    HDPRINTF(ah, HAL_DBG_NF_CAL, "NF calibrated [ext] [chain 0] is %d\n", nf);
    nfarray[3] = nf;

    if(!AR_SREV_KITE(ah)) {
        if (AR_SREV_MERLIN_10_OR_LATER(ah)) {
            nf = MS(OS_REG_READ(ah, AR_PHY_CH1_EXT_CCA), AR9280_PHY_CH1_EXT_MINCCA_PWR);
        } else {
            nf = MS(OS_REG_READ(ah, AR_PHY_CH1_EXT_CCA), AR_PHY_CH1_EXT_MINCCA_PWR);
        }
        if (nf & 0x100)
                nf = 0 - ((nf ^ 0x1ff) + 1);
        HDPRINTF(ah, HAL_DBG_NF_CAL, "NF calibrated [ext] [chain 1] is %d\n", nf);
        nfarray[4] = nf;

        if (!AR_SREV_MERLIN(ah) && !AR_SREV_KIWI(ah)) {
            nf = MS(OS_REG_READ(ah, AR_PHY_CH2_EXT_CCA), AR_PHY_CH2_EXT_MINCCA_PWR);
            if (nf & 0x100)
                nf = 0 - ((nf ^ 0x1ff) + 1);
            HDPRINTF(ah, HAL_DBG_NF_CAL, "NF calibrated [ext] [chain 2] is %d\n", nf);
            nfarray[5] = nf;
        }
    }
}

static HAL_BOOL
getNoiseFloorThresh(struct ath_hal *ah, const HAL_CHANNEL_INTERNAL *chan,
        int16_t *nft)
{
        struct ath_hal_5416 *ahp = AH5416(ah);

        switch (chan->channelFlags & CHANNEL_ALL_NOTURBO) {
            case CHANNEL_A:
            case CHANNEL_A_HT20:
            case CHANNEL_A_HT40PLUS:
            case CHANNEL_A_HT40MINUS:
                *nft = (int8_t)ar5416EepromGet(ahp, EEP_NFTHRESH_5);
                break;
            case CHANNEL_B:
            case CHANNEL_G:
            case CHANNEL_G_HT20:
            case CHANNEL_G_HT40PLUS:
            case CHANNEL_G_HT40MINUS:
                *nft = (int8_t)ar5416EepromGet(ahp, EEP_NFTHRESH_2);
                break;
            default:
                HDPRINTF(ah, HAL_DBG_CHANNEL, "%s: invalid channel flags 0x%x\n",
                        __func__, chan->channelFlags);
                return AH_FALSE;
        }
        return AH_TRUE;
}

static void
ar5416StartNFCal(struct ath_hal *ah)
{
    OS_REG_SET_BIT(ah, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_ENABLE_NF);
    OS_REG_SET_BIT(ah, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_NO_UPDATE_NF);
    OS_REG_SET_BIT(ah, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_NF);
}

static void
ar5416LoadNF(struct ath_hal *ah, HAL_CHANNEL_INTERNAL *chan)
{
    HAL_NFCAL_HIST    *h;
    int i, j;
    int32_t val;
    const u_int32_t ar5416_cca_regs[6] = {
        AR_PHY_CCA,
        AR_PHY_CH1_CCA,
        AR_PHY_CH2_CCA,
        AR_PHY_EXT_CCA,
        AR_PHY_CH1_EXT_CCA,
        AR_PHY_CH2_EXT_CCA
    };
    u_int8_t chainmask;

    /* Force NF calibration for all chains, otherwise Vista station 
     * would conduct a bad performance 
     */
    if (AR_SREV_KITE(ah)) {
        /* Kite has only one chain */
        chainmask = 0x9;
    } else if (AR_SREV_MERLIN(ah) || AR_SREV_KIWI(ah)) {
        /* Merlin and Kiwi has only two chains */
        chainmask = 0x1B;
    } else {
        chainmask = 0x3F;
    }

    /*
     * Write filtered NF values into maxCCApwr register parameter
     * so we can load below.
     */
#ifdef ATH_NF_PER_CHAN
    h = chan->nfCalHist;
#else
    h = AH_PRIVATE(ah)->nfCalHist;
#endif

    for (i = 0; i < NUM_NF_READINGS; i ++) {
        if (chainmask & (1 << i)) { 
            val = OS_REG_READ(ah, ar5416_cca_regs[i]);
            val &= 0xFFFFFE00;
            val |= (((u_int32_t)(h[i].privNF) << 1) & 0x1ff);
            OS_REG_WRITE(ah, ar5416_cca_regs[i], val);
        }
    }
    
    /* Load software filtered NF value into baseband internal minCCApwr variable. */
    OS_REG_CLR_BIT(ah, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_ENABLE_NF);
    OS_REG_CLR_BIT(ah, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_NO_UPDATE_NF);
    OS_REG_SET_BIT(ah, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_NF);

    /* Wait for load to complete, should be fast, a few 10s of us. */
    for (j = 0; j < 1000; j++) {
        if ((OS_REG_READ(ah, AR_PHY_AGC_CONTROL) & AR_PHY_AGC_CONTROL_NF) == 0)
            break;
        OS_DELAY(10);
    }

    /*
     * Restore maxCCAPower register parameter again so that we're not capped
     * by the median we just loaded.  This will be initial (and max) value
     * of next noise floor calibration the baseband does.  
     */
    for (i = 0; i < NUM_NF_READINGS; i ++) {
        if (chainmask & (1 << i)) {    
            val = OS_REG_READ(ah, ar5416_cca_regs[i]);
            val &= 0xFFFFFE00;
            val |= (((u_int32_t)(-50) << 1) & 0x1ff);
            OS_REG_WRITE(ah, ar5416_cca_regs[i], val);
        }
    }
}

/*
 * Read the NF and check it against the noise floor threshhold
 */
static int16_t
ar5416GetNf(struct ath_hal *ah, HAL_CHANNEL_INTERNAL *chan)
{
    int16_t nf, nfThresh;
    int16_t nfarray[NUM_NF_READINGS]= {0};
    HAL_NFCAL_HIST    *h;

    
    chan->channelFlags &= (~CHANNEL_CW_INT);
    if (OS_REG_READ(ah, AR_PHY_AGC_CONTROL) & AR_PHY_AGC_CONTROL_NF) {
        HDPRINTF(ah, HAL_DBG_CALIBRATE, "%s: NF did not complete in calibration window\n",
                __func__);
        nf = 0;
        return (chan->rawNoiseFloor = nf);
    } else {
        ar5416GetNoiseFloor(ah, nfarray);
        /* TODO - enhance for multiple chains and ext ch */
        nf = nfarray[0];
        if (getNoiseFloorThresh(ah, chan, &nfThresh) && nf > nfThresh) {
            HDPRINTF(ah, HAL_DBG_CALIBRATE, "%s: noise floor failed detected; "
                    "detected %d, threshold %d\n", __func__,
                    nf, nfThresh);
            /*
             * NB: Don't discriminate 2.4 vs 5Ghz, if this
             *     happens it indicates a problem regardless
             *     of the band.
             */
            chan->channelFlags |= CHANNEL_CW_INT;
        }
    }

    /* Update the NF buffer for each chain masked by chainmask */ 
#ifdef ATH_NF_PER_CHAN
    h = chan->nfCalHist;    
#else
    h = AH_PRIVATE(ah)->nfCalHist;
#endif

    ar5416UpdateNFHistBuff(ah, h, nfarray);   
    chan->rawNoiseFloor = h[0].privNF;

    return (chan->rawNoiseFloor);
}

/*
 *  Update the noise floor buffer as a ring buffer
 */
static void
ar5416UpdateNFHistBuff(struct ath_hal *ah, HAL_NFCAL_HIST *h, int16_t *nfarray)
{
#define MIN(a,b) (((a) < (b)) ? (a) : (b))
    struct ath_hal_private *ahpriv = AH_PRIVATE(ah);
    HAL_CHANNEL_INTERNAL *ichan = ahpriv->ah_curchan;
    int num_chains = IS_CHAN_HT40(ichan) ? 6 : 3;
    int num_readings = MIN(NUM_NF_READINGS, num_chains);
    int i;

    for (i = 0; i < num_readings; i ++) {
        h[i].nfCalBuffer[h[i].currIndex] = nfarray[i];

        if (++h[i].currIndex >= HAL_NF_CAL_HIST_MAX) {
            h[i].currIndex = 0;
        }       
        if (h[i].invalidNFcount > 0) {
            if (nfarray[i] < AR_PHY_CCA_MIN_BAD_VALUE || nfarray[i] > AR_PHY_CCA_MAX_HIGH_VALUE) {
                h[i].invalidNFcount = HAL_NF_CAL_HIST_MAX;
            } else {
                h[i].invalidNFcount--;
                h[i].privNF = nfarray[i];
            }
        } else {
            h[i].privNF = ar5416GetNfHistMid(h[i].nfCalBuffer);
        }
    }
    return;
#undef MIN
}   

/*
 *  Pick up the medium one in the noise floor buffer and update the corresponding range 
 *  for valid noise floor values
 */
static int16_t 
ar5416GetNfHistMid(int16_t *nfCalBuffer)
{
    int16_t nfval;
    int16_t sort[HAL_NF_CAL_HIST_MAX];
    int i,j;

    for (i = 0; i < HAL_NF_CAL_HIST_MAX; i ++) {
        sort[i] = nfCalBuffer[i];
    }
    for (i = 0; i < HAL_NF_CAL_HIST_MAX-1; i ++) {
        for (j = 1; j < HAL_NF_CAL_HIST_MAX-i; j ++) {
            if (sort[j] > sort[j-1]) {
                nfval = sort[j];
                sort[j] = sort[j-1];
                sort[j-1] = nfval;
            }
        }
    }
    nfval = sort[(HAL_NF_CAL_HIST_MAX-1)>>1];

    return nfval;
}



#define MAX_ANALOG_START        319             /* XXX */

/*
 * Delta slope coefficient computation.
 * Required for OFDM operation.
 */
void
ar5416SetDeltaSlope(struct ath_hal *ah, HAL_CHANNEL_INTERNAL *chan)
{
        u_int32_t coef_scaled, ds_coef_exp, ds_coef_man;
        u_int32_t clockMhzScaled = 0x64000000; /* clock * 2.5 */
        CHAN_CENTERS centers;

        /* half and quarter rate can divide the scaled clock by 2 or 4 */
        /* scale for selected channel bandwidth */
        if (IS_CHAN_HALF_RATE(chan)) {
            clockMhzScaled = clockMhzScaled >> 1;
        } else if (IS_CHAN_QUARTER_RATE(chan)) {
            clockMhzScaled = clockMhzScaled >> 2;
        }

        /*
         * ALGO -> coef = 1e8/fcarrier*fclock/40;
         * scaled coef to provide precision for this floating calculation
         */
        ar5416GetChannelCenters(ah, chan, &centers);
        coef_scaled = clockMhzScaled / centers.synth_center;

        ar5416GetDeltaSlopeValues(ah, coef_scaled, &ds_coef_man, &ds_coef_exp);

        OS_REG_RMW_FIELD(ah, AR_PHY_TIMING3,
                AR_PHY_TIMING3_DSC_MAN, ds_coef_man);
        OS_REG_RMW_FIELD(ah, AR_PHY_TIMING3,
                AR_PHY_TIMING3_DSC_EXP, ds_coef_exp);

        /*
         * For Short GI,
         * scaled coeff is 9/10 that of normal coeff
         */
        coef_scaled = (9 * coef_scaled)/10;

        ar5416GetDeltaSlopeValues(ah, coef_scaled, &ds_coef_man, &ds_coef_exp);

        /* for short gi */
        OS_REG_RMW_FIELD(ah, AR_PHY_HALFGI,
                AR_PHY_HALFGI_DSC_MAN, ds_coef_man);
        OS_REG_RMW_FIELD(ah, AR_PHY_HALFGI,
                AR_PHY_HALFGI_DSC_EXP, ds_coef_exp);
}

static inline void
ar5416GetDeltaSlopeValues(struct ath_hal *ah, u_int32_t coef_scaled,
                          u_int32_t *coef_mantissa, u_int32_t *coef_exponent)
{
    u_int32_t coef_exp, coef_man;

    /*
     * ALGO -> coef_exp = 14-floor(log2(coef));
     * floor(log2(x)) is the highest set bit position
     */
    for (coef_exp = 31; coef_exp > 0; coef_exp--)
            if ((coef_scaled >> coef_exp) & 0x1)
                    break;
    /* A coef_exp of 0 is a legal bit position but an unexpected coef_exp */
    HALASSERT(coef_exp);
    coef_exp = 14 - (coef_exp - COEF_SCALE_S);

    /*
     * ALGO -> coef_man = floor(coef* 2^coef_exp+0.5);
     * The coefficient is already shifted up for scaling
     */
    coef_man = coef_scaled + (1 << (COEF_SCALE_S - coef_exp - 1));

    *coef_mantissa = coef_man >> (COEF_SCALE_S - coef_exp);
    *coef_exponent = coef_exp - 16;
}

/*
 * Convert to baseband spur frequency given input channel frequency 
 * and compute register settings below.
 */
#define SPUR_RSSI_THRESH 40
#if 0
#define SPUR_INVALID     8888
int rf_spurs[] = { 24167, 24400 }; /* defined in 100kHz units */
#define N(a)    (sizeof (a) / sizeof (a[0]))
#endif
void
ar5416SpurMitigate(struct ath_hal *ah, HAL_CHANNEL *chan)
{
    int bb_spur = AR_NO_SPUR;
    int bin, cur_bin;
    int spur_freq_sd;
    int spur_delta_phase;
    int denominator;
    int upper, lower, cur_vit_mask;
    int tmp, new;
    int i;
    int pilot_mask_reg[4] = { AR_PHY_TIMING7, AR_PHY_TIMING8,
                AR_PHY_PILOT_MASK_01_30, AR_PHY_PILOT_MASK_31_60 };
    int chan_mask_reg[4] = { AR_PHY_TIMING9, AR_PHY_TIMING10,
                AR_PHY_CHANNEL_MASK_01_30, AR_PHY_CHANNEL_MASK_31_60 };
    int inc[4] = { 0, 100, 0, 0 };

    int8_t mask_m[123] = { 0 };
    int8_t mask_p[123] = { 0 };
    int8_t mask_amt;
    int tmp_mask;
    int cur_bb_spur;
    HAL_BOOL is2GHz = IS_CHAN_2GHZ(chan);

    /*
     * Need to verify range +/- 9.5 for static ht20, otherwise spur
     * is out-of-band and can be ignored.
     */
     
    for (i = 0; i < AR_EEPROM_MODAL_SPURS; i++) {
        //spurChan = ee->ee_spurChans[i][is2GHz];
        cur_bb_spur = AH_PRIVATE(ah)->ah_eepromGetSpurChan(ah,i,is2GHz);
        if (AR_NO_SPUR == cur_bb_spur)
            break;
        cur_bb_spur = cur_bb_spur - (chan->channel * 10);
        if ((cur_bb_spur > -95) && (cur_bb_spur < 95)) {
            bb_spur = cur_bb_spur;
            break;
        }
    }
#if 0   
    for (i = 0; i < N(rf_spurs); i++) {
        int cur_bb_spur = rf_spurs[i] - (chan->channel * 10);
        if ((cur_bb_spur > -95) && (cur_bb_spur < 95)) {
            bb_spur = cur_bb_spur;
            break;
        }
    }
#endif

    if (AR_NO_SPUR == bb_spur)
        return;

    bin = bb_spur * 32;

    tmp = OS_REG_READ(ah, AR_PHY_TIMING_CTRL4(0));
    new = tmp | (AR_PHY_TIMING_CTRL4_ENABLE_SPUR_RSSI |
        AR_PHY_TIMING_CTRL4_ENABLE_SPUR_FILTER |
        AR_PHY_TIMING_CTRL4_ENABLE_CHAN_MASK |
        AR_PHY_TIMING_CTRL4_ENABLE_PILOT_MASK);

    OS_REG_WRITE(ah, AR_PHY_TIMING_CTRL4(0), new);

    new = (AR_PHY_SPUR_REG_MASK_RATE_CNTL |
        AR_PHY_SPUR_REG_ENABLE_MASK_PPM |
        AR_PHY_SPUR_REG_MASK_RATE_SELECT |
        AR_PHY_SPUR_REG_ENABLE_VIT_SPUR_RSSI |
        SM(SPUR_RSSI_THRESH, AR_PHY_SPUR_REG_SPUR_RSSI_THRESH));
    OS_REG_WRITE(ah, AR_PHY_SPUR_REG, new);
    /*
     * Should offset bb_spur by +/- 10 MHz for dynamic 2040 MHz
     * config, no offset for HT20.
     * spur_delta_phase = bb_spur/40 * 2**21 for static ht20,
     * /80 for dyn2040.
     */
    spur_delta_phase = ((bb_spur * 524288) / 100) &
        AR_PHY_TIMING11_SPUR_DELTA_PHASE;
    /*
     * in 11A mode the denominator of spur_freq_sd should be 40 and
     * it should be 44 in 11G
     */
    denominator = IS_CHAN_2GHZ(chan) ? 440 : 400;
    spur_freq_sd = ((bb_spur * 2048) / denominator) & 0x3ff;

    new = (AR_PHY_TIMING11_USE_SPUR_IN_AGC |
        SM(spur_freq_sd, AR_PHY_TIMING11_SPUR_FREQ_SD) |
        SM(spur_delta_phase, AR_PHY_TIMING11_SPUR_DELTA_PHASE));
    OS_REG_WRITE(ah, AR_PHY_TIMING11, new);


    /*
     * ============================================
     * pilot mask 1 [31:0] = +6..-26, no 0 bin
     * pilot mask 2 [19:0] = +26..+7
     *
     * channel mask 1 [31:0] = +6..-26, no 0 bin
     * channel mask 2 [19:0] = +26..+7
     */
    //cur_bin = -26;
    cur_bin = -6000;
    upper = bin + 100;
    lower = bin - 100;

    for (i = 0; i < 4; i++) {
        int pilot_mask = 0;
        int chan_mask  = 0;
        int bp         = 0;
        for (bp = 0; bp < 30; bp++) {
            if ((cur_bin > lower) && (cur_bin < upper)) {
                pilot_mask = pilot_mask | 0x1 << bp;
                chan_mask  = chan_mask | 0x1 << bp;
            }
            cur_bin += 100;
        }
        cur_bin += inc[i];
        OS_REG_WRITE(ah, pilot_mask_reg[i], pilot_mask);
        OS_REG_WRITE(ah, chan_mask_reg[i], chan_mask);
    }

    /* =================================================
     * viterbi mask 1 based on channel magnitude
     * four levels 0-3
     *  - mask (-27 to 27) (reg 64,0x9900 to 67,0x990c)
     *      [1 2 2 1] for -9.6 or [1 2 1] for +16
     *  - enable_mask_ppm, all bins move with freq
     *
     *  - mask_select,    8 bits for rates (reg 67,0x990c)
     *  - mask_rate_cntl, 8 bits for rates (reg 67,0x990c)
     *      choose which mask to use mask or mask2
     */

    /*
     * viterbi mask 2  2nd set for per data rate puncturing
     * four levels 0-3
     *  - mask_select, 8 bits for rates (reg 67)
     *  - mask (-27 to 27) (reg 98,0x9988 to 101,0x9994)
     *      [1 2 2 1] for -9.6 or [1 2 1] for +16
     */
    cur_vit_mask = 6100;
    upper        = bin + 120;
    lower        = bin - 120;

    for (i = 0; i < 123; i++) {
        if ((cur_vit_mask > lower) && (cur_vit_mask < upper)) {
            if ((abs(cur_vit_mask - bin)) < 75) {
                mask_amt = 1;
            } else {
                mask_amt = 0;
            }
            if (cur_vit_mask < 0) {
                mask_m[abs(cur_vit_mask / 100)] = mask_amt;
            } else {
                mask_p[cur_vit_mask / 100] = mask_amt;
            }
        }
        cur_vit_mask -= 100;
    }

    tmp_mask = (mask_m[46] << 30) | (mask_m[47] << 28)
          | (mask_m[48] << 26) | (mask_m[49] << 24)
          | (mask_m[50] << 22) | (mask_m[51] << 20)
          | (mask_m[52] << 18) | (mask_m[53] << 16)
          | (mask_m[54] << 14) | (mask_m[55] << 12)
          | (mask_m[56] << 10) | (mask_m[57] <<  8)
          | (mask_m[58] <<  6) | (mask_m[59] <<  4)
          | (mask_m[60] <<  2) | (mask_m[61] <<  0);
    OS_REG_WRITE(ah, AR_PHY_BIN_MASK_1, tmp_mask);
    OS_REG_WRITE(ah, AR_PHY_VIT_MASK2_M_46_61, tmp_mask);

    tmp_mask =             (mask_m[31] << 28)
          | (mask_m[32] << 26) | (mask_m[33] << 24)
          | (mask_m[34] << 22) | (mask_m[35] << 20)
          | (mask_m[36] << 18) | (mask_m[37] << 16)
          | (mask_m[48] << 14) | (mask_m[39] << 12)
          | (mask_m[40] << 10) | (mask_m[41] <<  8)
          | (mask_m[42] <<  6) | (mask_m[43] <<  4)
          | (mask_m[44] <<  2) | (mask_m[45] <<  0);
    OS_REG_WRITE(ah, AR_PHY_BIN_MASK_2, tmp_mask);
    OS_REG_WRITE(ah, AR_PHY_MASK2_M_31_45, tmp_mask);

    tmp_mask = (mask_m[16] << 30) | (mask_m[16] << 28)
          | (mask_m[18] << 26) | (mask_m[18] << 24)
          | (mask_m[20] << 22) | (mask_m[20] << 20)
          | (mask_m[22] << 18) | (mask_m[22] << 16)
          | (mask_m[24] << 14) | (mask_m[24] << 12)
          | (mask_m[25] << 10) | (mask_m[26] <<  8)
          | (mask_m[27] <<  6) | (mask_m[28] <<  4)
          | (mask_m[29] <<  2) | (mask_m[30] <<  0);
    OS_REG_WRITE(ah, AR_PHY_BIN_MASK_3, tmp_mask);
    OS_REG_WRITE(ah, AR_PHY_MASK2_M_16_30, tmp_mask);

    tmp_mask = (mask_m[ 0] << 30) | (mask_m[ 1] << 28)
          | (mask_m[ 2] << 26) | (mask_m[ 3] << 24)
          | (mask_m[ 4] << 22) | (mask_m[ 5] << 20)
          | (mask_m[ 6] << 18) | (mask_m[ 7] << 16)
          | (mask_m[ 8] << 14) | (mask_m[ 9] << 12)
          | (mask_m[10] << 10) | (mask_m[11] <<  8)
          | (mask_m[12] <<  6) | (mask_m[13] <<  4)
          | (mask_m[14] <<  2) | (mask_m[15] <<  0);
    OS_REG_WRITE(ah, AR_PHY_MASK_CTL, tmp_mask);
    OS_REG_WRITE(ah, AR_PHY_MASK2_M_00_15, tmp_mask);

    tmp_mask =             (mask_p[15] << 28)
          | (mask_p[14] << 26) | (mask_p[13] << 24)
          | (mask_p[12] << 22) | (mask_p[11] << 20)
          | (mask_p[10] << 18) | (mask_p[ 9] << 16)
          | (mask_p[ 8] << 14) | (mask_p[ 7] << 12)
          | (mask_p[ 6] << 10) | (mask_p[ 5] <<  8)
          | (mask_p[ 4] <<  6) | (mask_p[ 3] <<  4)
          | (mask_p[ 2] <<  2) | (mask_p[ 1] <<  0);
    OS_REG_WRITE(ah, AR_PHY_BIN_MASK2_1, tmp_mask);
    OS_REG_WRITE(ah, AR_PHY_MASK2_P_15_01, tmp_mask);

    tmp_mask =             (mask_p[30] << 28)
          | (mask_p[29] << 26) | (mask_p[28] << 24)
          | (mask_p[27] << 22) | (mask_p[26] << 20)
          | (mask_p[25] << 18) | (mask_p[24] << 16)
          | (mask_p[23] << 14) | (mask_p[22] << 12)
          | (mask_p[21] << 10) | (mask_p[20] <<  8)
          | (mask_p[19] <<  6) | (mask_p[18] <<  4)
          | (mask_p[17] <<  2) | (mask_p[16] <<  0);
    OS_REG_WRITE(ah, AR_PHY_BIN_MASK2_2, tmp_mask);
    OS_REG_WRITE(ah, AR_PHY_MASK2_P_30_16, tmp_mask);

    tmp_mask =             (mask_p[45] << 28)
          | (mask_p[44] << 26) | (mask_p[43] << 24)
          | (mask_p[42] << 22) | (mask_p[41] << 20)
          | (mask_p[40] << 18) | (mask_p[39] << 16)
          | (mask_p[38] << 14) | (mask_p[37] << 12)
          | (mask_p[36] << 10) | (mask_p[35] <<  8)
          | (mask_p[34] <<  6) | (mask_p[33] <<  4)
          | (mask_p[32] <<  2) | (mask_p[31] <<  0);
    OS_REG_WRITE(ah, AR_PHY_BIN_MASK2_3, tmp_mask);
    OS_REG_WRITE(ah, AR_PHY_MASK2_P_45_31, tmp_mask);

    tmp_mask = (mask_p[61] << 30) | (mask_p[60] << 28)
          | (mask_p[59] << 26) | (mask_p[58] << 24)
          | (mask_p[57] << 22) | (mask_p[56] << 20)
          | (mask_p[55] << 18) | (mask_p[54] << 16)
          | (mask_p[53] << 14) | (mask_p[52] << 12)
          | (mask_p[51] << 10) | (mask_p[50] <<  8)
          | (mask_p[49] <<  6) | (mask_p[48] <<  4)
          | (mask_p[47] <<  2) | (mask_p[46] <<  0);
    OS_REG_WRITE(ah, AR_PHY_BIN_MASK2_4, tmp_mask);
    OS_REG_WRITE(ah, AR_PHY_MASK2_P_61_45, tmp_mask);
}

void
ar9280SpurMitigate(struct ath_hal *ah, HAL_CHANNEL *chan, HAL_CHANNEL_INTERNAL *ichan)
{
    int bb_spur = AR_NO_SPUR;
    int freq;
    int bin, cur_bin;
    int bb_spur_off, spur_subchannel_sd;
    int spur_freq_sd;
    int spur_delta_phase;
    int denominator;
    int upper, lower, cur_vit_mask;
    int tmp, newVal;
    int i;
    int pilot_mask_reg[4] = { AR_PHY_TIMING7, AR_PHY_TIMING8,
                AR_PHY_PILOT_MASK_01_30, AR_PHY_PILOT_MASK_31_60 };
    int chan_mask_reg[4] = { AR_PHY_TIMING9, AR_PHY_TIMING10,
                AR_PHY_CHANNEL_MASK_01_30, AR_PHY_CHANNEL_MASK_31_60 };
    int inc[4] = { 0, 100, 0, 0 };
    CHAN_CENTERS centers;

    int8_t mask_m[123];
    int8_t mask_p[123];
    int8_t mask_amt;
    int tmp_mask;
    int cur_bb_spur;
    HAL_BOOL is2GHz = IS_CHAN_2GHZ(chan);

    OS_MEMZERO(&mask_m, sizeof(int8_t) * 123);
    OS_MEMZERO(&mask_p, sizeof(int8_t) * 123);

    ar5416GetChannelCenters(ah, ichan, &centers);
    freq = centers.synth_center;

    /*
     * Need to verify range +/- 9.38 for static ht20 and +/- 18.75 for ht40,
     * otherwise spur is out-of-band and can be ignored.
     */
    AH_PRIVATE(ah)->ah_config.ath_hal_spurMode = SPUR_ENABLE_EEPROM; 
    for (i = 0; i < AR_EEPROM_MODAL_SPURS; i++) {
        cur_bb_spur = AH_PRIVATE(ah)->ah_eepromGetSpurChan(ah, i, is2GHz);
        /* Get actual spur freq in MHz from EEPROM read value */ 
        if (is2GHz) {
            cur_bb_spur =  (cur_bb_spur / 10) + AR_BASE_FREQ_2GHZ;
        } else {
            cur_bb_spur =  (cur_bb_spur / 10) + AR_BASE_FREQ_5GHZ;
        }

        if (AR_NO_SPUR == cur_bb_spur)
            break;
        cur_bb_spur = cur_bb_spur - freq;

        if (IS_CHAN_HT40(chan)) {
            if ((cur_bb_spur > -AR_SPUR_FEEQ_BOUND_HT40) && 
                (cur_bb_spur < AR_SPUR_FEEQ_BOUND_HT40)) {
                bb_spur = cur_bb_spur;
                break;
            }
        } else if ((cur_bb_spur > -AR_SPUR_FEEQ_BOUND_HT20) &&
                   (cur_bb_spur < AR_SPUR_FEEQ_BOUND_HT20)) {
            bb_spur = cur_bb_spur;
            break;
        }
    }

    if (AR_NO_SPUR == bb_spur) {
#if 1
        /*
         * MRC CCK can interfere with beacon detection and cause deaf/mute.
         * Disable MRC CCK until it is fixed or a work around is applied.
         * See bug 33723 and 33590.
         */
        OS_REG_CLR_BIT(ah, AR_PHY_FORCE_CLKEN_CCK, AR_PHY_FORCE_CLKEN_CCK_MRC_MUX);
#else
        /* Enable MRC CCK if no spur is found in this channel. */
        OS_REG_SET_BIT(ah, AR_PHY_FORCE_CLKEN_CCK, AR_PHY_FORCE_CLKEN_CCK_MRC_MUX);
#endif
        return;
    } else {
        /* 
         * For Merlin, spur can break CCK MRC algorithm. Disable CCK MRC if spur
         * is found in this channel.
         */
        OS_REG_CLR_BIT(ah, AR_PHY_FORCE_CLKEN_CCK, AR_PHY_FORCE_CLKEN_CCK_MRC_MUX);
    }

    bin = bb_spur * 320;

    tmp = OS_REG_READ(ah, AR_PHY_TIMING_CTRL4(0));

    newVal = tmp | (AR_PHY_TIMING_CTRL4_ENABLE_SPUR_RSSI |
        AR_PHY_TIMING_CTRL4_ENABLE_SPUR_FILTER |
        AR_PHY_TIMING_CTRL4_ENABLE_CHAN_MASK |
        AR_PHY_TIMING_CTRL4_ENABLE_PILOT_MASK);
    OS_REG_WRITE(ah, AR_PHY_TIMING_CTRL4(0), newVal);

    newVal = (AR_PHY_SPUR_REG_MASK_RATE_CNTL |
        AR_PHY_SPUR_REG_ENABLE_MASK_PPM |
        AR_PHY_SPUR_REG_MASK_RATE_SELECT |
        AR_PHY_SPUR_REG_ENABLE_VIT_SPUR_RSSI |
        SM(SPUR_RSSI_THRESH, AR_PHY_SPUR_REG_SPUR_RSSI_THRESH));
    OS_REG_WRITE(ah, AR_PHY_SPUR_REG, newVal);

    /* Pick control or extn channel to cancel the spur */
    if (IS_CHAN_HT40(chan)) {
        if (bb_spur < 0) {
            spur_subchannel_sd = 1;
            bb_spur_off = bb_spur + 10;
        } else {
            spur_subchannel_sd = 0;
            bb_spur_off = bb_spur - 10;
        }
    } else {
        spur_subchannel_sd = 0;
        bb_spur_off = bb_spur;
    }

    /*
     * spur_delta_phase = bb_spur/40 * 2**21 for static ht20,
     * /80 for dyn2040.
     */
    if (IS_CHAN_HT40(chan))
        spur_delta_phase = ((bb_spur * 262144) / 10) & AR_PHY_TIMING11_SPUR_DELTA_PHASE;    
    else
        spur_delta_phase = ((bb_spur * 524288) / 10) & AR_PHY_TIMING11_SPUR_DELTA_PHASE;

    /*
     * in 11A mode the denominator of spur_freq_sd should be 40 and
     * it should be 44 in 11G
     */
    denominator = IS_CHAN_2GHZ(chan) ? 44 : 40;
    spur_freq_sd = ((bb_spur_off * 2048) / denominator) & 0x3ff;

    newVal = (AR_PHY_TIMING11_USE_SPUR_IN_AGC |
        SM(spur_freq_sd, AR_PHY_TIMING11_SPUR_FREQ_SD) |
        SM(spur_delta_phase, AR_PHY_TIMING11_SPUR_DELTA_PHASE));
    OS_REG_WRITE(ah, AR_PHY_TIMING11, newVal);

    /* Choose to cancel between control and extension channels */
    newVal = spur_subchannel_sd << AR_PHY_SFCORR_SPUR_SUBCHNL_SD_S;
    OS_REG_WRITE(ah, AR_PHY_SFCORR_EXT, newVal);

    /*
     * ============================================
     * Set Pilot and Channel Masks
     *
     * pilot mask 1 [31:0] = +6..-26, no 0 bin
     * pilot mask 2 [19:0] = +26..+7
     *
     * channel mask 1 [31:0] = +6..-26, no 0 bin
     * channel mask 2 [19:0] = +26..+7
     */
    cur_bin = -6000;
    upper = bin + 100;
    lower = bin - 100;

    for (i = 0; i < 4; i++) {
        int pilot_mask = 0;
        int chan_mask  = 0;
        int bp         = 0;
        for (bp = 0; bp < 30; bp++) {
            if ((cur_bin > lower) && (cur_bin < upper)) {
                pilot_mask = pilot_mask | 0x1 << bp;
                chan_mask  = chan_mask | 0x1 << bp;
            }
            cur_bin += 100;
        }
        cur_bin += inc[i];
        OS_REG_WRITE(ah, pilot_mask_reg[i], pilot_mask);
        OS_REG_WRITE(ah, chan_mask_reg[i], chan_mask);
    }

    /* =================================================
     * viterbi mask 1 based on channel magnitude
     * four levels 0-3
     *  - mask (-27 to 27) (reg 64,0x9900 to 67,0x990c)
     *      [1 2 2 1] for -9.6 or [1 2 1] for +16
     *  - enable_mask_ppm, all bins move with freq
     *
     *  - mask_select,    8 bits for rates (reg 67,0x990c)
     *  - mask_rate_cntl, 8 bits for rates (reg 67,0x990c)
     *      choose which mask to use mask or mask2
     */

    /*
     * viterbi mask 2  2nd set for per data rate puncturing
     * four levels 0-3
     *  - mask_select, 8 bits for rates (reg 67)
     *  - mask (-27 to 27) (reg 98,0x9988 to 101,0x9994)
     *      [1 2 2 1] for -9.6 or [1 2 1] for +16
     */
    cur_vit_mask = 6100;
    upper        = bin + 120;
    lower        = bin - 120;

    for (i = 0; i < 123; i++) {
        if ((cur_vit_mask > lower) && (cur_vit_mask < upper)) {
            if ((abs(cur_vit_mask - bin)) < 75) {
                mask_amt = 1;
            } else {
                mask_amt = 0;
            }
            if (cur_vit_mask < 0) {
                mask_m[abs(cur_vit_mask / 100)] = mask_amt;
            } else {
                mask_p[cur_vit_mask / 100] = mask_amt;
            }
        }
        cur_vit_mask -= 100;
    }

    tmp_mask = (mask_m[46] << 30) | (mask_m[47] << 28)
          | (mask_m[48] << 26) | (mask_m[49] << 24)
          | (mask_m[50] << 22) | (mask_m[51] << 20)
          | (mask_m[52] << 18) | (mask_m[53] << 16)
          | (mask_m[54] << 14) | (mask_m[55] << 12)
          | (mask_m[56] << 10) | (mask_m[57] <<  8)
          | (mask_m[58] <<  6) | (mask_m[59] <<  4)
          | (mask_m[60] <<  2) | (mask_m[61] <<  0);
    OS_REG_WRITE(ah, AR_PHY_BIN_MASK_1, tmp_mask);
    OS_REG_WRITE(ah, AR_PHY_VIT_MASK2_M_46_61, tmp_mask);

    tmp_mask =             (mask_m[31] << 28)
          | (mask_m[32] << 26) | (mask_m[33] << 24)
          | (mask_m[34] << 22) | (mask_m[35] << 20)
          | (mask_m[36] << 18) | (mask_m[37] << 16)
          | (mask_m[48] << 14) | (mask_m[39] << 12)
          | (mask_m[40] << 10) | (mask_m[41] <<  8)
          | (mask_m[42] <<  6) | (mask_m[43] <<  4)
          | (mask_m[44] <<  2) | (mask_m[45] <<  0);
    OS_REG_WRITE(ah, AR_PHY_BIN_MASK_2, tmp_mask);
    OS_REG_WRITE(ah, AR_PHY_MASK2_M_31_45, tmp_mask);

    tmp_mask = (mask_m[16] << 30) | (mask_m[16] << 28)
          | (mask_m[18] << 26) | (mask_m[18] << 24)
          | (mask_m[20] << 22) | (mask_m[20] << 20)
          | (mask_m[22] << 18) | (mask_m[22] << 16)
          | (mask_m[24] << 14) | (mask_m[24] << 12)
          | (mask_m[25] << 10) | (mask_m[26] <<  8)
          | (mask_m[27] <<  6) | (mask_m[28] <<  4)
          | (mask_m[29] <<  2) | (mask_m[30] <<  0);
    OS_REG_WRITE(ah, AR_PHY_BIN_MASK_3, tmp_mask);
    OS_REG_WRITE(ah, AR_PHY_MASK2_M_16_30, tmp_mask);

    tmp_mask = (mask_m[ 0] << 30) | (mask_m[ 1] << 28)
          | (mask_m[ 2] << 26) | (mask_m[ 3] << 24)
          | (mask_m[ 4] << 22) | (mask_m[ 5] << 20)
          | (mask_m[ 6] << 18) | (mask_m[ 7] << 16)
          | (mask_m[ 8] << 14) | (mask_m[ 9] << 12)
          | (mask_m[10] << 10) | (mask_m[11] <<  8)
          | (mask_m[12] <<  6) | (mask_m[13] <<  4)
          | (mask_m[14] <<  2) | (mask_m[15] <<  0);
    OS_REG_WRITE(ah, AR_PHY_MASK_CTL, tmp_mask);
    OS_REG_WRITE(ah, AR_PHY_MASK2_M_00_15, tmp_mask);

    tmp_mask =             (mask_p[15] << 28)
          | (mask_p[14] << 26) | (mask_p[13] << 24)
          | (mask_p[12] << 22) | (mask_p[11] << 20)
          | (mask_p[10] << 18) | (mask_p[ 9] << 16)
          | (mask_p[ 8] << 14) | (mask_p[ 7] << 12)
          | (mask_p[ 6] << 10) | (mask_p[ 5] <<  8)
          | (mask_p[ 4] <<  6) | (mask_p[ 3] <<  4)
          | (mask_p[ 2] <<  2) | (mask_p[ 1] <<  0);
    OS_REG_WRITE(ah, AR_PHY_BIN_MASK2_1, tmp_mask);
    OS_REG_WRITE(ah, AR_PHY_MASK2_P_15_01, tmp_mask);

    tmp_mask =             (mask_p[30] << 28)
          | (mask_p[29] << 26) | (mask_p[28] << 24)
          | (mask_p[27] << 22) | (mask_p[26] << 20)
          | (mask_p[25] << 18) | (mask_p[24] << 16)
          | (mask_p[23] << 14) | (mask_p[22] << 12)
          | (mask_p[21] << 10) | (mask_p[20] <<  8)
          | (mask_p[19] <<  6) | (mask_p[18] <<  4)
          | (mask_p[17] <<  2) | (mask_p[16] <<  0);
    OS_REG_WRITE(ah, AR_PHY_BIN_MASK2_2, tmp_mask);
    OS_REG_WRITE(ah, AR_PHY_MASK2_P_30_16, tmp_mask);

    tmp_mask =             (mask_p[45] << 28)
          | (mask_p[44] << 26) | (mask_p[43] << 24)
          | (mask_p[42] << 22) | (mask_p[41] << 20)
          | (mask_p[40] << 18) | (mask_p[39] << 16)
          | (mask_p[38] << 14) | (mask_p[37] << 12)
          | (mask_p[36] << 10) | (mask_p[35] <<  8)
          | (mask_p[34] <<  6) | (mask_p[33] <<  4)
          | (mask_p[32] <<  2) | (mask_p[31] <<  0);
    OS_REG_WRITE(ah, AR_PHY_BIN_MASK2_3, tmp_mask);
    OS_REG_WRITE(ah, AR_PHY_MASK2_P_45_31, tmp_mask);

    tmp_mask = (mask_p[61] << 30) | (mask_p[60] << 28)
          | (mask_p[59] << 26) | (mask_p[58] << 24)
          | (mask_p[57] << 22) | (mask_p[56] << 20)
          | (mask_p[55] << 18) | (mask_p[54] << 16)
          | (mask_p[53] << 14) | (mask_p[52] << 12)
          | (mask_p[51] << 10) | (mask_p[50] <<  8)
          | (mask_p[49] <<  6) | (mask_p[48] <<  4)
          | (mask_p[47] <<  2) | (mask_p[46] <<  0);
    OS_REG_WRITE(ah, AR_PHY_BIN_MASK2_4, tmp_mask);
    OS_REG_WRITE(ah, AR_PHY_MASK2_P_61_45, tmp_mask);
}

/*
 * Set a limit on the overall output power.  Used for dynamic
 * transmit power control and the like.
 *
 * NB: limit is in units of 0.5 dbM.
 */
HAL_BOOL
ar5416SetTxPowerLimit(struct ath_hal *ah, u_int32_t limit, u_int16_t tpcInDb)
{
    struct ath_hal_5416 *ahp = AH5416(ah);
    struct ath_hal_private *ahpriv = AH_PRIVATE(ah);
    HAL_CHANNEL_INTERNAL *ichan = ahpriv->ah_curchan;
    HAL_CHANNEL *chan = (HAL_CHANNEL *)ichan;

    ahpriv->ah_powerLimit = AH_MIN(limit, MAX_RATE_POWER);

    if (ar5416EepromSetTransmitPower(ah, &ahp->ah_eeprom, ichan,
        ath_hal_getctl(ah, chan), ath_hal_getantennaallowed(ah, chan),
        chan->maxRegTxPower * 2,
        AH_MIN(MAX_RATE_POWER, ahpriv->ah_powerLimit)) != HAL_OK)
        return AH_FALSE;

    return AH_TRUE;
}

/*
 * Exported call to check for a recent gain reading and return
 * the current state of the thermal calibration gain engine.
 */
HAL_RFGAIN
ar5416GetRfgain(struct ath_hal *ah)
{
    return HAL_RFGAIN_INACTIVE;
}

#if 0
/*
 * Set the transmit power in the baseband for the given
 * operating channel and mode.
 */
HAL_BOOL
ar5416SetTransmitPower(struct ath_hal *ah, HAL_CHANNEL_INTERNAL *chan)
{
#define POW_OFDM(_r, _s)        (((0 & 1)<< ((_s)+6)) | (((_r) & 0x3f) << (_s)))
#define POW_CCK(_r, _s)         (((_r) & 0x3f) << (_s))
#define N(a)                    (sizeof (a) / sizeof (a[0]))
        static const u_int16_t tpcScaleReductionTable[5] =
                { 0, 3, 6, 9, MAX_RATE_POWER };
        struct ath_hal_5416 *ahp = AH5416(ah);
        int16_t minPower, maxPower, tpcInDb, powerLimit;
        int i;

        OS_MEMZERO(ahp->ah_ratesArray, sizeof(ahp->ah_ratesArray));

        powerLimit = AH_MIN(MAX_RATE_POWER, AH_PRIVATE(ah)->ah_powerLimit);
        if (powerLimit >= MAX_RATE_POWER || powerLimit == 0)
                tpcInDb = tpcScaleReductionTable[AH_PRIVATE(ah)->ah_tpScale];
        else
                tpcInDb = 0;
        if (!ar5416SetRateTable(ah, (HAL_CHANNEL *) chan, tpcInDb, powerLimit,
                &minPower, &maxPower)) {
                HDPRINTF(ah, HAL_DBG_CHANNEL, "%s: unable to set rate table\n", __func__);
                return AH_FALSE;
        }

        /*
         * txPowerIndexOffset is set by the SetPowerTable() call -
         *  adjust the rate table
         */
        for (i = 0; i < N(ahp->ah_ratesArray); i++) {
                ahp->ah_ratesArray[i] += ahp->ah_txPowerIndexOffset;
                if (ahp->ah_ratesArray[i] > 63)
                        ahp->ah_ratesArray[i] = 63;
        }

        /* Write the OFDM power per rate set */
        OS_REG_WRITE(ah, AR_PHY_POWER_TX_RATE1,
                POW_OFDM(ahp->ah_ratesArray[3], 24)
              | POW_OFDM(ahp->ah_ratesArray[2], 16)
              | POW_OFDM(ahp->ah_ratesArray[1],  8)
              | POW_OFDM(ahp->ah_ratesArray[0],  0)
        );
        OS_REG_WRITE(ah, AR_PHY_POWER_TX_RATE2,
                POW_OFDM(ahp->ah_ratesArray[7], 24)
              | POW_OFDM(ahp->ah_ratesArray[6], 16)
              | POW_OFDM(ahp->ah_ratesArray[5],  8)
              | POW_OFDM(ahp->ah_ratesArray[4],  0)
        );

        /* Write the CCK power per rate set */
        OS_REG_WRITE(ah, AR_PHY_POWER_TX_RATE3,
                POW_CCK(ahp->ah_ratesArray[10], 24)
              | POW_CCK(ahp->ah_ratesArray[9],  16)
              | POW_CCK(ahp->ah_ratesArray[15],  8)     /* XR target power */
              | POW_CCK(ahp->ah_ratesArray[8],   0)
        );
        OS_REG_WRITE(ah, AR_PHY_POWER_TX_RATE4,
                POW_CCK(ahp->ah_ratesArray[14], 24)
              | POW_CCK(ahp->ah_ratesArray[13], 16)
              | POW_CCK(ahp->ah_ratesArray[12],  8)
              | POW_CCK(ahp->ah_ratesArray[11],  0)
        );

        /*
         * Set max power to 30 dBm and, optionally,
         * enable TPC in tx descriptors.
         */
        OS_REG_WRITE(ah, AR_PHY_POWER_TX_RATE_MAX, MAX_RATE_POWER |
                (ahp->ah_tpcEnabled ? AR_PHY_POWER_TX_RATE_MAX_TPC_ENABLE : 0));
        AH_PRIVATE(ah)->ah_maxPowerLevel = ahp->ah_ratesArray[0];

        return AH_TRUE;
#undef N
#undef POW_CCK
#undef POW_OFDM
}

/*
 * Sets the transmit power in the baseband for the given
 * operating channel and mode.
 */
static HAL_BOOL
ar5416SetRateTable(struct ath_hal *ah, HAL_CHANNEL *chan,
                   int16_t tpcScaleReduction, int16_t powerLimit,
                   int16_t *pMinPower, int16_t *pMaxPower)
{
        struct ath_hal_5416 *ahp = AH5416(ah);
        u_int16_t *rpow = ahp->ah_ratesArray;
        u_int16_t twiceMaxEdgePower = MAX_RATE_POWER;
        u_int16_t twiceMaxEdgePowerCck = MAX_RATE_POWER;
        u_int16_t twiceMaxRDPower = MAX_RATE_POWER;
        int i;
        u_int8_t cfgCtl;
        int8_t twiceAntennaGain, twiceAntennaReduction;
        RD_EDGES_POWER *rep;
        TRGT_POWER_INFO targetPowerOfdm, targetPowerCck;
        int16_t scaledPower, maxAvailPower = 0;

        twiceMaxRDPower = chan->maxRegTxPower * 2;
        *pMaxPower = -MAX_RATE_POWER;
        *pMinPower = MAX_RATE_POWER;

        /* Get conformance test limit maximum for this channel */
        cfgCtl = ath_hal_getctl(ah, chan);

        for (i = 0; i < ahp->ah_numCtls; i++) {
                u_int16_t twiceMinEdgePower;

                if (ahp->ah_ctl[i] == 0)
                        continue;
                if (ahp->ah_ctl[i] == cfgCtl ||
                    cfgCtl == ((ahp->ah_ctl[i] & CTL_MODE_M) | SD_NO_CTL)) {
                        rep = &ahp->ah_rdEdgesPower[i * NUM_EDGES];
                        twiceMinEdgePower = ar5416GetMaxEdgePower(chan->channel, rep);
                        if ((cfgCtl & ~CTL_MODE_M) == SD_NO_CTL) {
                                /* Find the minimum of all CTL edge powers that apply to this channel */
                                twiceMaxEdgePower = AH_MIN(twiceMaxEdgePower, twiceMinEdgePower);
                        } else {
                                twiceMaxEdgePower = twiceMinEdgePower;
                                break;
                        }
                }
        }

        if (IS_CHAN_G(chan)) {
                /* Check for a CCK CTL for 11G CCK powers */
                cfgCtl = (cfgCtl & ~CTL_MODE_M) | CTL_11B;
                for (i = 0; i < ahp->ah_numCtls; i++) {
                        u_int16_t twiceMinEdgePowerCck;

                        if (ahp->ah_ctl[i] == 0)
                                continue;
                        if (ahp->ah_ctl[i] == cfgCtl ||
                            cfgCtl == ((ahp->ah_ctl[i] & CTL_MODE_M) | SD_NO_CTL)) {
                                rep = &ahp->ah_rdEdgesPower[i * NUM_EDGES];
                                twiceMinEdgePowerCck = ar5416GetMaxEdgePower(chan->channel, rep);
                                if ((cfgCtl & ~CTL_MODE_M) == SD_NO_CTL) {
                                        /* Find the minimum of all CTL edge powers that apply to this channel */
                                        twiceMaxEdgePowerCck = AH_MIN(twiceMaxEdgePowerCck, twiceMinEdgePowerCck);
                                } else {
                                        twiceMaxEdgePowerCck = twiceMinEdgePowerCck;
                                        break;
                                }
                        }
                }
        } else {
                /* Set the 11B cck edge power to the one found before */
                twiceMaxEdgePowerCck = twiceMaxEdgePower;
        }

        /* Get Antenna Gain reduction */
        if (IS_CHAN_5GHZ(chan)) {
                twiceAntennaGain = ahp->ah_antennaGainMax[0];
        } else {
                twiceAntennaGain = ahp->ah_antennaGainMax[1];
        }
        twiceAntennaReduction =
                ath_hal_getantennareduction(ah, chan, twiceAntennaGain);

        if (IS_CHAN_OFDM(chan) || IS_CHAN_HT(chan)) {
                /* Get final OFDM target powers */
                if (IS_CHAN_2GHZ(chan)) {
                        ar5416GetTargetPowers(ah, chan, ahp->ah_trgtPwr_11g,
                                ahp->ah_numTargetPwr_11g, &targetPowerOfdm);
                } else {
                        ar5416GetTargetPowers(ah, chan, ahp->ah_trgtPwr_11a,
                                ahp->ah_numTargetPwr_11a, &targetPowerOfdm);
                }

                /* Get Maximum OFDM power */
                /* Minimum of target and edge powers */
                scaledPower = AH_MIN(twiceMaxEdgePower,
                                twiceMaxRDPower - twiceAntennaReduction);

                /*
                 * If turbo is set, reduce power to keep power
                 * consumption under 2 Watts.  Note that we always do
                 * this unless specially configured.  Then we limit
                 * power only for non-AP operation.
                 */
                if (IS_CHAN_TURBO(chan)
#ifdef AH_ENABLE_AP_SUPPORT
                    && AH_PRIVATE(ah)->ah_opmode != HAL_M_HOSTAP
#endif
                ) {
                        /*
                         * If turbo is set, reduce power to keep power
                         * consumption under 2 Watts
                         */
                        if (ahp->ah_eeversion >= AR_EEPROM_VER3_1)
                                scaledPower = AH_MIN(scaledPower,
                                        ahp->ah_turbo2WMaxPower5);
                        /*
                         * EEPROM version 4.0 added an additional
                         * constraint on 2.4GHz channels.
                         */
                        if (ahp->ah_eeversion >= AR_EEPROM_VER4_0 &&
                            IS_CHAN_2GHZ(chan))
                                scaledPower = AH_MIN(scaledPower,
                                        ahp->ah_turbo2WMaxPower2);
                }

                maxAvailPower = AH_MIN(scaledPower,
                                        targetPowerOfdm.twicePwr6_24);

                /* Reduce power by max regulatory domain allowed restrictions */
                scaledPower = maxAvailPower - (tpcScaleReduction * 2);
                scaledPower = (scaledPower < 0) ? 0 : scaledPower;
                scaledPower = AH_MIN(scaledPower, powerLimit);

                /* Set OFDM rates 9, 12, 18, 24 */
                rpow[0] = rpow[1] = rpow[2] = rpow[3] = rpow[4] = scaledPower;

                /* Set OFDM rates 36, 48, 54, XR */
                rpow[5] = AH_MIN(rpow[0], targetPowerOfdm.twicePwr36);
                rpow[6] = AH_MIN(rpow[0], targetPowerOfdm.twicePwr48);
                rpow[7] = AH_MIN(rpow[0], targetPowerOfdm.twicePwr54);

                if (ahp->ah_eeversion >= AR_EEPROM_VER4_0) {
                        /* Setup XR target power from EEPROM */
                        rpow[15] = AH_MIN(scaledPower, IS_CHAN_2GHZ(chan) ?
                                ahp->ah_xrTargetPower2 : ahp->ah_xrTargetPower5);
                } else {
                        /* XR uses 6mb power */
                        rpow[15] = rpow[0];
                }

                *pMinPower = rpow[7];
                *pMaxPower = rpow[0];

                ahp->ah_ofdmTxPower = *pMaxPower;

                HDPRINTF(ah, HAL_DBG_CHANNEL, "%s: MaxRD: %d TurboMax: %d MaxCTL: %d "
                        "TPC_Reduction %d\n",
                        __func__,
                        twiceMaxRDPower, ahp->ah_turbo2WMaxPower5,
                        twiceMaxEdgePower, tpcScaleReduction * 2);
        }

        if (IS_CHAN_CCK(chan) || IS_CHAN_G(chan)) {
                /* Get final CCK target powers */
                ar5416GetTargetPowers(ah, chan, ahp->ah_trgtPwr_11b,
                        ahp->ah_numTargetPwr_11b, &targetPowerCck);

                /* Reduce power by max regulatory domain allowed restrictions */
                scaledPower = AH_MIN(twiceMaxEdgePowerCck,
                        twiceMaxRDPower - twiceAntennaReduction);
                if (maxAvailPower < AH_MIN(scaledPower, targetPowerCck.twicePwr6_24))
                        maxAvailPower = AH_MIN(scaledPower, targetPowerCck.twicePwr6_24);

                /* Reduce power by user selection */
                scaledPower = AH_MIN(scaledPower, targetPowerCck.twicePwr6_24) - (tpcScaleReduction * 2);
                scaledPower = (scaledPower < 0) ? 0 : scaledPower;
                scaledPower = AH_MIN(scaledPower, powerLimit);

                /* Set CCK rates 2L, 2S, 5.5L, 5.5S, 11L, 11S */
                rpow[8]  = AH_MIN(scaledPower, targetPowerCck.twicePwr6_24);
                rpow[9]  = AH_MIN(scaledPower, targetPowerCck.twicePwr36);
                rpow[10] = rpow[9];
                rpow[11] = AH_MIN(scaledPower, targetPowerCck.twicePwr48);
                rpow[12] = rpow[11];
                rpow[13] = AH_MIN(scaledPower, targetPowerCck.twicePwr54);
                rpow[14] = rpow[13];

                /* Set min/max power based off OFDM values or initialization */
                if (rpow[13] < *pMinPower)
                    *pMinPower = rpow[13];
                if (rpow[9] > *pMaxPower)
                    *pMaxPower = rpow[9];

        }
        ahp->ah_tx6PowerInHalfDbm = *pMaxPower;
        return AH_TRUE;
}

/*
 * Find the maximum conformance test limit for the given channel and CTL info
 */
static u_int16_t
ar5416GetMaxEdgePower(u_int16_t channel, RD_EDGES_POWER *pRdEdgesPower)
{
        /* temp array for holding edge channels */
        u_int16_t tempChannelList[NUM_EDGES];
        u_int16_t clo, chi, twiceMaxEdgePower;
        int i, numEdges;

        /* Get the edge power */
        for (i = 0; i < NUM_EDGES; i++) {
                if (pRdEdgesPower[i].rdEdge == 0)
                        break;
                tempChannelList[i] = pRdEdgesPower[i].rdEdge;
        }
        numEdges = i;

        ar5416GetLowerUpperValues(channel, tempChannelList,
                numEdges, &clo, &chi);
        /* Get the index for the lower channel */
        for (i = 0; i < numEdges && clo != tempChannelList[i]; i++)
                ;
        /* Is lower channel ever outside the rdEdge? */
        HALASSERT(i != numEdges);

        if ((clo == chi && clo == channel) || (pRdEdgesPower[i].flag)) {
                /*
                 * If there's an exact channel match or an inband flag set
                 * on the lower channel use the given rdEdgePower
                 */
                twiceMaxEdgePower = pRdEdgesPower[i].twice_rdEdgePower;
                HALASSERT(twiceMaxEdgePower > 0);
        } else
                twiceMaxEdgePower = MAX_RATE_POWER;
        return twiceMaxEdgePower;
}

/*
 * Returns interpolated or the scaled up interpolated value
 */
static u_int16_t
interpolate(u_int16_t target, u_int16_t srcLeft, u_int16_t srcRight,
        u_int16_t targetLeft, u_int16_t targetRight)
{
        u_int16_t rv;
        int16_t lRatio;

        /* to get an accurate ratio, always scale, if want to scale, then don't scale back down */
        if ((targetLeft * targetRight) == 0)
                return 0;

        if (srcRight != srcLeft) {
                /*
                 * Note the ratio always need to be scaled,
                 * since it will be a fraction.
                 */
                lRatio = (target - srcLeft) * EEP_SCALE / (srcRight - srcLeft);
                if (lRatio < 0) {
                    /* Return as Left target if value would be negative */
                    rv = targetLeft;
                } else if (lRatio > EEP_SCALE) {
                    /* Return as Right target if Ratio is greater than 100% (SCALE) */
                    rv = targetRight;
                } else {
                        rv = (lRatio * targetRight + (EEP_SCALE - lRatio) *
                                        targetLeft) / EEP_SCALE;
                }
        } else {
                rv = targetLeft;
        }
        return rv;
}

/*
 * Return the four rates of target power for the given target power table
 * channel, and number of channels
 */
static void
ar5416GetTargetPowers(struct ath_hal *ah, HAL_CHANNEL *chan,
        TRGT_POWER_INFO *powInfo,
        u_int16_t numChannels, TRGT_POWER_INFO *pNewPower)
{
        /* temp array for holding target power channels */
        u_int16_t tempChannelList[NUM_TEST_FREQUENCIES];
        u_int16_t clo, chi, ixlo, ixhi;
        int i;

        /* Copy the target powers into the temp channel list */
        for (i = 0; i < numChannels; i++)
                tempChannelList[i] = powInfo[i].testChannel;

        ar5416GetLowerUpperValues(chan->channel, tempChannelList,
                numChannels, &clo, &chi);

        /* Get the indices for the channel */
        ixlo = ixhi = 0;
        for (i = 0; i < numChannels; i++) {
                if (clo == tempChannelList[i]) {
                        ixlo = i;
                }
                if (chi == tempChannelList[i]) {
                        ixhi = i;
                        break;
                }
        }

        /*
         * Get the lower and upper channels, target powers,
         * and interpolate between them.
         */
        pNewPower->twicePwr6_24 = interpolate(chan->channel, clo, chi,
                powInfo[ixlo].twicePwr6_24, powInfo[ixhi].twicePwr6_24);
        pNewPower->twicePwr36 = interpolate(chan->channel, clo, chi,
                powInfo[ixlo].twicePwr36, powInfo[ixhi].twicePwr36);
        pNewPower->twicePwr48 = interpolate(chan->channel, clo, chi,
                powInfo[ixlo].twicePwr48, powInfo[ixhi].twicePwr48);
        pNewPower->twicePwr54 = interpolate(chan->channel, clo, chi,
                powInfo[ixlo].twicePwr54, powInfo[ixhi].twicePwr54);
}
#endif

/*
 * Search a list for a specified value v that is within
 * EEP_DELTA of the search values.  Return the closest
 * values in the list above and below the desired value.
 * EEP_DELTA is a factional value; everything is scaled
 * so only integer arithmetic is used.
 *
 * NB: the input list is assumed to be sorted in ascending order
 */
void
ar5416GetLowerUpperValues(u_int16_t v, u_int16_t *lp, u_int16_t listSize,
                          u_int16_t *vlo, u_int16_t *vhi)
{
        u_int32_t target = v * EEP_SCALE;
        u_int16_t *ep = lp+listSize;

        /*
         * Check first and last elements for out-of-bounds conditions.
         */
        if (target < (u_int32_t)(lp[0] * EEP_SCALE - EEP_DELTA)) {
                *vlo = *vhi = lp[0];
                return;
        }
        if (target > (u_int32_t)(ep[-1] * EEP_SCALE + EEP_DELTA)) {
                *vlo = *vhi = ep[-1];
                return;
        }

        /* look for value being near or between 2 values in list */
        for (; lp < ep; lp++) {
                /*
                 * If value is close to the current value of the list
                 * then target is not between values, it is one of the values
                 */
                if (abs((int16_t)lp[0] * EEP_SCALE - (int16_t)target) < EEP_DELTA) {
                        *vlo = *vhi = lp[0];
                        return;
                }
                /*
                 * Look for value being between current value and next value
                 * if so return these 2 values
                 */
                if (target < (u_int32_t)(lp[1] * EEP_SCALE - EEP_DELTA)) {
                        *vlo = lp[0];
                        *vhi = lp[1];
                        return;
                }
        }
}

/*
 * Perform analog "swizzling" of parameters into their location
 */
void
ar5416ModifyRfBuffer(u_int32_t *rfBuf, u_int32_t reg32, u_int32_t numBits,
                     u_int32_t firstBit, u_int32_t column)
{
        u_int32_t tmp32, mask, arrayEntry, lastBit;
        int32_t bitPosition, bitsLeft;

        HALASSERT(column <= 3);
        HALASSERT(numBits <= 32);
        HALASSERT(firstBit + numBits <= MAX_ANALOG_START);

        tmp32 = ath_hal_reverseBits(reg32, numBits);
        arrayEntry = (firstBit - 1) / 8;
        bitPosition = (firstBit - 1) % 8;
        bitsLeft = numBits;
        while (bitsLeft > 0) {
                lastBit = (bitPosition + bitsLeft > 8) ?
                        8 : bitPosition + bitsLeft;
                mask = (((1 << lastBit) - 1) ^ ((1 << bitPosition) - 1)) <<
                        (column * 8);
                rfBuf[arrayEntry] &= ~mask;
                rfBuf[arrayEntry] |= ((tmp32 << bitPosition) <<
                        (column * 8)) & mask;
                bitsLeft -= 8 - bitPosition;
                tmp32 = tmp32 >> (8 - bitPosition);
                bitPosition = 0;
                arrayEntry++;
        }
}

void
ar5416GetChannelCenters(struct ath_hal *ah, HAL_CHANNEL_INTERNAL *chan,
                        CHAN_CENTERS *centers)
{
    int8_t      extoff;
    struct ath_hal_5416 *ahp = AH5416(ah);

    if (!IS_CHAN_HT40(chan)) {
        centers->ctl_center = centers->ext_center =
        centers->synth_center = chan->channel;
        return;
    }

    HALASSERT(IS_CHAN_HT40(chan));

    /*
     * In 20/40 phy mode, the center frequency is
     * "between" the primary and extension channels.
     */
    if (chan->channelFlags & CHANNEL_HT40PLUS) {
        centers->synth_center = chan->channel + HT40_CHANNEL_CENTER_SHIFT;
        extoff = 1;
    }
    else {
        centers->synth_center = chan->channel - HT40_CHANNEL_CENTER_SHIFT;
        extoff = -1;
    }

     centers->ctl_center = centers->synth_center - (extoff *
                            HT40_CHANNEL_CENTER_SHIFT);
     centers->ext_center = centers->synth_center + (extoff *
                ((ahp->ah_extprotspacing == HAL_HT_EXTPROTSPACING_20)? HT40_CHANNEL_CENTER_SHIFT : 15));

}

#define IS(_c,_f)       (((_c)->channelFlags & _f) || 0)

static inline HAL_CHANNEL_INTERNAL*
ar5416CheckChan(struct ath_hal *ah, HAL_CHANNEL *chan)
{
    if ((IS(chan, CHANNEL_2GHZ) ^ IS(chan, CHANNEL_5GHZ)) == 0) {
        HDPRINTF(ah, HAL_DBG_CHANNEL, "%s: invalid channel %u/0x%x; not marked as "
                 "2GHz or 5GHz\n", __func__,
                chan->channel, chan->channelFlags);
        return AH_NULL;
    }

    if ((IS(chan, CHANNEL_OFDM) ^ IS(chan, CHANNEL_CCK) ^
         IS(chan, CHANNEL_HT20) ^ IS(chan, CHANNEL_HT40PLUS) ^ IS(chan, CHANNEL_HT40MINUS)) == 0) {
        HDPRINTF(ah, HAL_DBG_CHANNEL, "%s: invalid channel %u/0x%x; not marked as "
                "OFDM or CCK or HT20 or HT40PLUS or HT40MINUS\n", __func__,
                chan->channel, chan->channelFlags);
        return AH_NULL;
    }

    return (ath_hal_checkchannel(ah, chan));
}
#undef IS

/*
 * TODO: Only write the PLL if we're changing to or from CCK mode
 *
 * WARNING: The order of the PLL and mode registers must be correct.
 */
static inline void
ar5416SetRfMode(struct ath_hal *ah, HAL_CHANNEL *chan)
{
    u_int32_t rfMode = 0;

    if (chan == AH_NULL)
        return;

    switch (ar5416Get11nHwPlatform(ah)) {
        case HAL_TRUE_CHIP:
            rfMode |= (IS_CHAN_B(chan) || IS_CHAN_G(chan))
                      ? AR_PHY_MODE_DYNAMIC : AR_PHY_MODE_OFDM;
            break;
        default:
            HALASSERT(0);
            break;
    }

    if (!AR_SREV_MERLIN_10_OR_LATER(ah)) {
        rfMode |= (IS_CHAN_5GHZ(chan)) ? AR_PHY_MODE_RF5GHZ : AR_PHY_MODE_RF2GHZ;
    }

    /* Merlin 2.0/2.1 - Phy mode bits for 5GHz channels requiring Fast Clock */
    if (AR_SREV_MERLIN_20(ah) && IS_5GHZ_FAST_CLOCK_EN(ah, chan)) {       
        rfMode |= (AR_PHY_MODE_DYNAMIC | AR_PHY_MODE_DYN_CCK_DISABLE);
    }

    OS_REG_WRITE(ah, AR_PHY_MODE, rfMode);
}

static inline HAL_STATUS
ar5416ProcessIni(struct ath_hal *ah, HAL_CHANNEL *chan,
                 HAL_CHANNEL_INTERNAL *ichan,
                 HAL_HT_MACMODE macmode)
{
    int i, regWrites = 0;
    struct ath_hal_5416 *ahp = AH5416(ah);
    struct ath_hal_private *ahpriv = AH_PRIVATE(ah);
    u_int modesIndex, freqIndex;
    HAL_STATUS status;

    /* Setup the indices for the next set of register array writes */
    switch (chan->channelFlags & CHANNEL_ALL) {
        /* TODO:
         * If the channel marker is indicative of the current mode rather
         * than capability, we do not need to check the phy mode below.
         */
        case CHANNEL_A:
        case CHANNEL_A_HT20:
            modesIndex = 1;
            freqIndex  = 1;
            break;

        case CHANNEL_A_HT40PLUS:
        case CHANNEL_A_HT40MINUS:
            modesIndex = 2;
            freqIndex  = 1;
            break;

        case CHANNEL_PUREG:
        case CHANNEL_G_HT20:
        case CHANNEL_B:
            modesIndex = 4;
            freqIndex  = 2;
            break;

        case CHANNEL_G_HT40PLUS:
        case CHANNEL_G_HT40MINUS:
            modesIndex = 3;
            freqIndex  = 2;
            break;

        case CHANNEL_108G:
            modesIndex = 5;
            freqIndex  = 2;
            break;

        default:
            HALASSERT(0);
            return HAL_EINVAL;
    }

    /* Set correct Baseband to analog shift setting to access analog chips. */
    OS_REG_WRITE(ah, AR_PHY(0), 0x00000007);

    /*
     * Write addac shifts
    */
    if (ar5416Get11nHwPlatform(ah) == HAL_TRUE_CHIP) {
        OS_REG_WRITE(ah, AR_PHY_ADC_SERIAL_CTL, AR_PHY_SEL_EXTERNAL_RADIO);

        ar5416EepromSetAddac(ah, ichan);

        if (AR_SREV_5416_V22_OR_LATER(ah)) {
            REG_WRITE_ARRAY(&ahp->ah_iniAddac, 1, regWrites);
        } else {
        struct ar5416IniArray temp;
        u_int32_t addacSize = sizeof(u_int32_t) * ahp->ah_iniAddac.ia_rows * ahp->ah_iniAddac.ia_columns;

        /* Owl 2.1/2.0 */
        OS_MEMCPY(ahp->ah_addacOwl21, ahp->ah_iniAddac.ia_array, addacSize);

        /* override CLKDRV value at [row, column] = [31, 1] */
        (ahp->ah_addacOwl21)[31 *  ahp->ah_iniAddac.ia_columns + 1] = 0;

        temp.ia_array = ahp->ah_addacOwl21;
        temp.ia_columns = ahp->ah_iniAddac.ia_columns;
        temp.ia_rows = ahp->ah_iniAddac.ia_rows;
        REG_WRITE_ARRAY(&temp, 1, regWrites);
        }
        OS_REG_WRITE(ah, AR_PHY_ADC_SERIAL_CTL, AR_PHY_SEL_INTERNAL_ADDAC);
    }

    /*
    ** We need to expand the REG_WRITE_ARRAY macro here to ensure we can insert the 100usec
    ** delay if required.  This is to ensure any writes to the 0x78xx registers have the
    ** proper delay implemented in the Merlin case
    */

    for (i = 0; i < ahp->ah_iniModes.ia_rows; i++) {
        u_int32_t reg = INI_RA(&ahp->ah_iniModes, i, 0);
        u_int32_t val = INI_RA(&ahp->ah_iniModes, i, modesIndex);
 
#ifdef SLOW_ANT_DIV
        if (AH_PRIVATE(ah)->ah_devid == AR5416_DEVID_AR9280_PCI)
            val = ar5416INIFixup(ah, &ahp->ah_eeprom, reg, val);
#endif

        OS_REG_WRITE(ah, reg, val);

        /*
        ** Determine if this is a shift register value, and insert the
        ** 100 usec delay if so.  Can be disabled by config value.
        */

        if (reg >= 0x7800 && reg < 0x7900 &&
            ahpriv->ah_config.ath_hal_analogShiftRegWAR) {
            OS_DELAY(100);
        }

        WAR_6773(regWrites);                                        \
    }

    /* Write rxgain Array Parameters */
    if (AR_SREV_MERLIN_20(ah) || AR_SREV_KIWI_10_OR_LATER(ah)) {
        REG_WRITE_ARRAY(&ahp->ah_iniModesRxgain, modesIndex, regWrites);
    }

    /* Write txgain Array Parameters */
    if (AR_SREV_MERLIN_20(ah) ||
        (AR_SREV_KITE(ah) && AR_SREV_KITE_12_OR_LATER(ah)) || AR_SREV_KIWI_10_OR_LATER(ah)) {
        REG_WRITE_ARRAY(&ahp->ah_iniModesTxgain, modesIndex, regWrites);
    }

    /* Write Common Array Parameters */
    for (i = 0; i < ahp->ah_iniCommon.ia_rows; i++) {
        u_int32_t reg = INI_RA(&ahp->ah_iniCommon, i, 0);
        u_int32_t val = INI_RA(&ahp->ah_iniCommon, i, 1);

        OS_REG_WRITE(ah, reg, val);

        /*
        ** Determine if this is a shift register value, and insert the
        ** 100 usec delay if so.  Can be disabled by config value.
        */

        if (reg >= 0x7800 && reg < 0x7900 &&
            ahpriv->ah_config.ath_hal_analogShiftRegWAR) {
            OS_DELAY(100);
        }
        
        WAR_6773(regWrites);
    }

    ahp->ah_rfHal.writeRegs(ah, modesIndex, freqIndex, regWrites);

    /* Merlin 2.0/2.1 - For 5GHz channels requiring Fast Clock, apply different modal values */
    if (AR_SREV_MERLIN_20(ah) && IS_5GHZ_FAST_CLOCK_EN(ah, chan)) {
        HDPRINTF(ah, HAL_DBG_CHANNEL, "%s: Fast clock enabled, use special ini values\n", __func__);
        REG_WRITE_ARRAY(&ahp->ah_iniModesAdditional, modesIndex, regWrites);
    }

    /* Override INI with chip specific configuration */
    ar5416OverrideIni(ah, chan);

    /* Setup 11n MAC/Phy mode registers */
    ar5416Set11nRegs(ah, chan, macmode);

    /*
     * Moved ar5416InitChainMasks() here to ensure the swap bit is set before
     * the pdadc table is written.  Swap must occur before any radio dependent
     * replicated register access.  The pdadc curve addressing in particular
     * depends on the consistent setting of the swap bit.
     */
     ar5416InitChainMasks(ah);

     if ((AR_SREV_MERLIN_20(ah) || AR_SREV_KIWI_10_OR_LATER(ah)) &&
         ar5416EepromGet(ahp, EEP_OL_PWRCTRL)) {
         ar5416OpenLoopPowerControlInit(ah);
         ar5416OpenLoopPowerControlTempCompensation(ah);
     }

    /*
     * Setup the transmit power values.
     *
     * After the public to private hal channel mapping, ichan contains the
     * valid regulatory power value.
     * ath_hal_getctl and ath_hal_getantennaallowed look up ichan from chan.
     */
    status = ar5416EepromSetTransmitPower(ah, &ahp->ah_eeprom, ichan,
             ath_hal_getctl(ah, chan), ath_hal_getantennaallowed(ah, chan),
             ichan->maxRegTxPower * 2,
             AH_MIN(MAX_RATE_POWER, ahpriv->ah_powerLimit));
    if (status != HAL_OK) {
        HDPRINTF(ah, HAL_DBG_POWER_MGMT, "%s: error init'ing transmit power\n", __func__);
        return HAL_EIO;
    }

    /* Write the analog registers */
    if (!ahp->ah_rfHal.setRfRegs(ah, ichan, freqIndex)) {
        HDPRINTF(ah, HAL_DBG_REG_IO, "%s: ar5416SetRfRegs failed\n", __func__);
        return HAL_EIO;
    }

    return HAL_OK;
}

static inline void
ar5416InitPLL(struct ath_hal *ah, HAL_CHANNEL *chan)
{
    u_int32_t pll;

    
    if (AR_SREV_HOWL(ah)) {
        
        if (chan && IS_CHAN_5GHZ(chan))
            pll = 0x1450;
        else
            pll = 0x1458;
    }
    else
    {
        if (AR_SREV_MERLIN_10_OR_LATER(ah)) {

            pll = SM(0x5, AR_RTC_SOWL_PLL_REFDIV);

            if (chan && IS_CHAN_HALF_RATE(chan)) {
                pll |= SM(0x1, AR_RTC_SOWL_PLL_CLKSEL);
            } else if (chan && IS_CHAN_QUARTER_RATE(chan)) {
                pll |= SM(0x2, AR_RTC_SOWL_PLL_CLKSEL);
            }
            if (chan && IS_CHAN_5GHZ(chan)) {
                pll |= SM(0x28, AR_RTC_SOWL_PLL_DIV);

                /* PLL WAR for Merlin 2.0/2.1
                 * When doing fast clock, set PLL to 0x142c
                 * Else, set PLL to 0x2850 to prevent reset-to-reset variation 
                 */
                if (AR_SREV_MERLIN_20(ah)) {                    
                    if (IS_5GHZ_FAST_CLOCK_EN(ah, chan)) {  
                        pll = 0x142c;  
                    } 
                    else {  
                        pll = 0x2850;  
                    } 
                }
            } else {
                pll |= SM(0x2c, AR_RTC_SOWL_PLL_DIV);
            }

        } else if (AR_SREV_SOWL_10_OR_LATER(ah)) {

              pll = SM(0x5, AR_RTC_SOWL_PLL_REFDIV);

              if (chan && IS_CHAN_HALF_RATE(chan)) {
                  pll |= SM(0x1, AR_RTC_SOWL_PLL_CLKSEL);
              } else if (chan && IS_CHAN_QUARTER_RATE(chan)) {
                  pll |= SM(0x2, AR_RTC_SOWL_PLL_CLKSEL);
              }
              if (chan && IS_CHAN_5GHZ(chan)) {
                  pll |= SM(0x50, AR_RTC_SOWL_PLL_DIV);
              } else {
                  pll |= SM(0x58, AR_RTC_SOWL_PLL_DIV);
              }
        } else {
            pll = AR_RTC_PLL_REFDIV_5 | AR_RTC_PLL_DIV2;

            if (chan && IS_CHAN_HALF_RATE(chan)) {
                pll |= SM(0x1, AR_RTC_PLL_CLKSEL);
            } else if (chan && IS_CHAN_QUARTER_RATE(chan)) {
                pll |= SM(0x2, AR_RTC_PLL_CLKSEL);
            }
            if (chan && IS_CHAN_5GHZ(chan)) {
                pll |= SM(0xa, AR_RTC_PLL_DIV);
            } else {
                pll |= SM(0xb, AR_RTC_PLL_DIV);
        }
        }
    }
    OS_REG_WRITE(ah, AR_RTC_PLL_CONTROL, pll);


    /* TODO:
    * For multi-band owl, switch between bands by reiniting the PLL.
    */
    
    OS_DELAY(RTC_PLL_SETTLE_DELAY);
    
    OS_REG_WRITE(ah, AR_RTC_SLEEP_CLK, AR_RTC_FORCE_DERIVED_CLK);
}

static inline void
ar5416InitChainMasks(struct ath_hal *ah)
{
    struct ath_hal_5416 *ahp = AH5416(ah);
    int rx_chainmask, tx_chainmask;

    rx_chainmask = ahp->ah_rxchainmask;
    tx_chainmask = ahp->ah_txchainmask;

    switch (rx_chainmask) {
        case 0x5:
            OS_REG_SET_BIT(ah, AR_PHY_ANALOG_SWAP, AR_PHY_SWAP_ALT_CHAIN);
            /*
             * fall through !
             */
        case 0x3:
            if ((ar5416Get11nHwPlatform(ah) == HAL_TRUE_CHIP) &&
                ((AH_PRIVATE(ah))->ah_macVersion <= AR_SREV_VERSION_SOWL)) {
                /*
                 * workaround for OWL 1.0 cal failure, always cal 3 chains for
                 * multi chain -- then after cal set true mask value
                 */
                OS_REG_WRITE(ah, AR_PHY_RX_CHAINMASK, 0x7);
                OS_REG_WRITE(ah, AR_PHY_CAL_CHAINMASK, 0x7);
                break;
            }
            /*
             * fall through !
             */
        case 0x1:
        case 0x2: /* Chainmask 0x2 included for Merlin antenna selection */
        case 0x7:
            OS_REG_WRITE(ah, AR_PHY_RX_CHAINMASK, rx_chainmask);
            OS_REG_WRITE(ah, AR_PHY_CAL_CHAINMASK, rx_chainmask);
            break;
        default:
            break;
    }

    OS_REG_WRITE(ah, AR_SELFGEN_MASK, tx_chainmask);
    if (tx_chainmask == 0x5) {
        OS_REG_SET_BIT(ah, AR_PHY_ANALOG_SWAP, AR_PHY_SWAP_ALT_CHAIN);
    }
     if(AR_SREV_HOWL(ah))
         OS_REG_WRITE(ah, AR_PHY_ANALOG_SWAP,
             OS_REG_READ(ah, AR_PHY_ANALOG_SWAP) | 0x00000001);
}

/* ar5416IsCalSupp
 * Determine if calibration is supported by device and channel flags
 */
inline HAL_BOOL
ar5416IsCalSupp(struct ath_hal *ah, HAL_CHANNEL *chan, HAL_CAL_TYPES calType) 
{
    struct ath_hal_5416 *ahp = AH5416(ah);
    HAL_BOOL retval = AH_FALSE;

    switch(calType & ahp->ah_suppCals) {
    case IQ_MISMATCH_CAL:
        /* Run IQ Mismatch for non-CCK only */
        if (!IS_CHAN_B(chan))
            {retval = AH_TRUE;}
        break;
    case ADC_GAIN_CAL:
    case ADC_DC_CAL:
        /* Run ADC Gain Cal for non-CCK & non 2GHz-HT20 only */
        if (!IS_CHAN_B(chan) && !(IS_CHAN_2GHZ(chan) && IS_CHAN_HT20(chan)))
            {retval = AH_TRUE;}
        break;
    }

    return retval;
}


/* ar9285PACal
 * PA Calibration for Kite 1.1 and later versions of Kite.
 * - from system's team.
 */
static inline void
ar9285PACal(struct ath_hal *ah)
{

    u_int32_t regVal;
    int i, offset, offs_6_1, offs_0;
    u_int32_t ccomp_org;
    u_int32_t regList [][2] = {
        { 0x786c, 0 },
        { 0x7854, 0 },
        { 0x7820, 0 },
        { 0x7824, 0 },
        { 0x7868, 0 },
        { 0x783c, 0 },
        { 0x7838, 0 } ,
    };


    /* Kite 1.1 WAR for Bug 35666 
     * Increase the LDO value to 1.28V before accessing analog Reg */
    if (AR_SREV_KITE_11(ah)) {
        OS_REG_WRITE(ah, AR9285_AN_TOP4, (AR9285_AN_TOP4_DEFAULT | 0x14) );
        OS_DELAY(10);
    }

   for (i = 0; i < N(regList); i++) {
       regList[i][1] = OS_REG_READ(ah, regList[i][0]);
   }

   regVal = OS_REG_READ(ah, 0x7834);
   regVal &= (~(0x1));
   OS_REG_WRITE(ah, 0x7834, regVal);
   regVal = OS_REG_READ(ah, 0x9808);
   regVal |= (0x1 << 27);
   OS_REG_WRITE(ah, 0x9808, regVal);

   OS_REG_RMW_FIELD(ah, AR9285_AN_TOP3, AR9285_AN_TOP3_PWDDAC, 1); // pwddac=1
   OS_REG_RMW_FIELD(ah, AR9285_AN_RXTXBB1, AR9285_AN_RXTXBB1_PDRXTXBB1, 1); // pdrxtxbb=1
   OS_REG_RMW_FIELD(ah, AR9285_AN_RXTXBB1, AR9285_AN_RXTXBB1_PDV2I, 1); // pdv2i=1
   OS_REG_RMW_FIELD(ah, AR9285_AN_RXTXBB1, AR9285_AN_RXTXBB1_PDDACIF, 1); // pddacinterface=1
   OS_REG_RMW_FIELD(ah, AR9285_AN_RF2G2, AR9285_AN_RF2G2_OFFCAL, 0); // offcal=0
   OS_REG_RMW_FIELD(ah, AR9285_AN_RF2G7, AR9285_AN_RF2G7_PWDDB, 0);   // pwddb=0
   OS_REG_RMW_FIELD(ah, AR9285_AN_RF2G1, AR9285_AN_RF2G1_ENPACAL, 0); // enpacal=0
   OS_REG_RMW_FIELD(ah, AR9285_AN_RF2G1, AR9285_AN_RF2G1_PDPADRV1, 1); // pdpadrv1=1
   OS_REG_RMW_FIELD(ah, AR9285_AN_RF2G1,AR9285_AN_RF2G1_PDPADRV2,0); // pdpadrv2=0
   OS_REG_RMW_FIELD(ah, AR9285_AN_RF2G1, AR9285_AN_RF2G1_PDPAOUT, 0); // pdpaout=0
   OS_REG_RMW_FIELD(ah, AR9285_AN_RF2G8,AR9285_AN_RF2G8_PADRVGN2TAB0,7); // padrvgn2tab_0=7
   OS_REG_RMW_FIELD(ah, AR9285_AN_RF2G7,AR9285_AN_RF2G7_PADRVGN2TAB0,0); // padrvgn1tab_0=0 - does not matter since we turn it off
   ccomp_org=OS_REG_READ_FIELD(ah, AR9285_AN_RF2G6,AR9285_AN_RF2G6_CCOMP); 
   OS_REG_RMW_FIELD(ah, AR9285_AN_RF2G6,AR9285_AN_RF2G6_CCOMP,7); // ccomp=7 - shares reg with off_6_1

   /* Set localmode=1,bmode=1,bmoderxtx=1,synthon=1,txon=1,paon=1,oscon=1,synthon_force=1 */
   OS_REG_WRITE(ah, AR9285_AN_TOP2, 0xca0358a0); 
   OS_DELAY(30);
   OS_REG_RMW_FIELD(ah, AR9285_AN_RF2G6,AR9285_AN_RF2G6_OFFS,0); 
   OS_REG_RMW_FIELD(ah, AR9285_AN_RF2G3,AR9285_AN_RF2G3_PDVCCOMP,0); //clear off[6:0]

   /* find off_6_1; */
   for (i = 6;i > 0;i--)
   {
       regVal = OS_REG_READ(ah, 0x7834);
       regVal |= (1 << (19 + i));
       OS_REG_WRITE(ah, 0x7834, regVal);  
       OS_DELAY(1);
       regVal = OS_REG_READ(ah, 0x7834);
       regVal &= (~(0x1 << (19 + i)));
       regVal |= ((OS_REG_READ_FIELD(ah, 0x7840, AR9285_AN_RXTXBB1_SPARE9)) << (19 + i));
       OS_REG_WRITE(ah, 0x7834, regVal);
   }

   /* find off_0; */
   OS_REG_RMW_FIELD(ah, AR9285_AN_RF2G3,AR9285_AN_RF2G3_PDVCCOMP,1);  
   OS_DELAY(1);
   OS_REG_RMW_FIELD(ah, AR9285_AN_RF2G3,AR9285_AN_RF2G3_PDVCCOMP,OS_REG_READ_FIELD(ah, AR9285_AN_RF2G9,AR9285_AN_RXTXBB1_SPARE9));
   offs_6_1 = OS_REG_READ_FIELD(ah, AR9285_AN_RF2G6, AR9285_AN_RF2G6_OFFS);
   offs_0   = OS_REG_READ_FIELD(ah, AR9285_AN_RF2G3, AR9285_AN_RF2G3_PDVCCOMP);

   /*  Empirical offset correction  */
   offset= (offs_6_1<<1)|offs_0;
   offset = offset - 0; // here is the correction
   offs_6_1 = offset>>1;   
   offs_0 = offset & 1;

   OS_REG_RMW_FIELD(ah, AR9285_AN_RF2G6, AR9285_AN_RF2G6_OFFS, offs_6_1);
   OS_REG_RMW_FIELD(ah, AR9285_AN_RF2G3, AR9285_AN_RF2G3_PDVCCOMP, offs_0);

   regVal = OS_REG_READ(ah, 0x7834);
   regVal |= 0x1;
   OS_REG_WRITE(ah, 0x7834, regVal);
   regVal = OS_REG_READ(ah, 0x9808);
   regVal &= (~(0x1 << 27));
   OS_REG_WRITE(ah, 0x9808, regVal);

   for (i = 0; i < N(regList); i++) {
       OS_REG_WRITE(ah, regList[i][0], regList[i][1]);
   }
   // Restore Registers to original value
   OS_REG_RMW_FIELD(ah, AR9285_AN_RF2G6,AR9285_AN_RF2G6_CCOMP,ccomp_org);

    /* Kite 1.1 WAR for Bug 35666 
     * Decrease the LDO value back to 1.20V */
    if (AR_SREV_KITE_11(ah))
        OS_REG_WRITE(ah, AR9285_AN_TOP4, AR9285_AN_TOP4_DEFAULT);

}

/* ar5416RunInitCals
 * Runs non-periodic calibrations
 */
inline HAL_BOOL
ar5416RunInitCals(struct ath_hal *ah, int init_cal_count)
{
    struct ath_hal_5416 *ahp = AH5416(ah);
    HAL_CHANNEL_INTERNAL ichan; // bogus
    HAL_BOOL isCalDone;
    HAL_CAL_LIST *currCal = ahp->ah_cal_list_curr;
    const HAL_PERCAL_DATA *calData = currCal->calData;
    int i;

    if (currCal == AH_NULL)
        return AH_FALSE;
        
    ichan.CalValid=0;

    for (i=0; i < init_cal_count; i++) {
        /* Reset this Cal */
        ar5416ResetCalibration(ah, currCal);
        /* Poll for offset calibration complete */
        if (!ath_hal_wait(ah, AR_PHY_TIMING_CTRL4(0),
            AR_PHY_TIMING_CTRL4_DO_CAL, 0, AH_WAIT_TIMEOUT)) {
            HDPRINTF(ah, HAL_DBG_CALIBRATE,
                     "%s: Cal %d failed to complete in 100ms.\n",
                     __func__, calData->calType);
            /* Re-initialize list pointers for periodic cals */
            ahp->ah_cal_list = ahp->ah_cal_list_last = ahp->ah_cal_list_curr
               = AH_NULL;
            return AH_FALSE;
        }
        /* Run this cal */
        ar5416PerCalibration(ah, &ichan, ahp->ah_rxchainmask,
                             currCal, &isCalDone);
        if (isCalDone == AH_FALSE) {
            HDPRINTF(ah, HAL_DBG_CALIBRATE,
                     "%s: Not able to run Init Cal %d.\n", __func__,
                     calData->calType);
        }
        if (currCal->calNext) {
            currCal = currCal->calNext;
            calData = currCal->calData;
        }
    }

    /* Re-initialize list pointers for periodic cals */
    ahp->ah_cal_list = ahp->ah_cal_list_last = ahp->ah_cal_list_curr = AH_NULL;
    return AH_TRUE;
}


/* ar5416InitCal
 * Initialize Calibration infrastructure
 */
static inline HAL_BOOL
ar5416InitCal(struct ath_hal *ah, HAL_CHANNEL *chan)
{
    struct ath_hal_5416 *ahp = AH5416(ah);
    HAL_CHANNEL_INTERNAL *ichan = ath_hal_checkchannel(ah, chan);

    HALASSERT(ichan);

    if (AR_SREV_MERLIN_10_OR_LATER(ah)) {  // Both for Merlin and Kite
        /* Enable Rx Filter Cal */
        OS_REG_CLR_BIT(ah, AR_PHY_ADC_CTL, AR_PHY_ADC_CTL_OFF_PWDADC);
        OS_REG_SET_BIT(ah, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_FLTR_CAL);
    }
    else if (AR_SREV_KIWI_10_OR_LATER(ah)) { /* No need to disable ADC for Kiwi */
        OS_REG_SET_BIT(ah, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_FLTR_CAL);

        /* Clear the carrier leak cal bit */
        OS_REG_CLR_BIT(ah, AR_PHY_CL_CAL_CTL, AR_PHY_CL_CAL_ENABLE);

        /*Kick off the cal */
        OS_REG_WRITE(ah, AR_PHY_AGC_CONTROL,
                OS_REG_READ(ah, AR_PHY_AGC_CONTROL)
                | AR_PHY_AGC_CONTROL_CAL);

        /* Poll for offset calibration complete */
        if (!ath_hal_wait(ah, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_CAL, 0, AH_WAIT_TIMEOUT)) {
            HDPRINTF(ah, HAL_DBG_CALIBRATE,
                    "%s: offset calibration failed to complete in 1ms; noisy environment?\n",
                    __func__);
            return AH_FALSE;
        }

        /* Set the cl cal bit and rerun the cal a 2nd time */
        /* Leave Rx Filter Cal Enabled */
        OS_REG_SET_BIT(ah, AR_PHY_CL_CAL_CTL, AR_PHY_CL_CAL_ENABLE);
    }
    /* Calibrate the AGC */
    OS_REG_WRITE(ah, AR_PHY_AGC_CONTROL,
             OS_REG_READ(ah, AR_PHY_AGC_CONTROL) |
             AR_PHY_AGC_CONTROL_CAL);

    /* Poll for offset calibration complete */
    if (!ath_hal_wait(ah, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_CAL, 0,
        AH_WAIT_TIMEOUT)) {
        HDPRINTF(ah, HAL_DBG_CALIBRATE,
                 "%s: offset calibration failed to complete in 1ms; "
                 "noisy environment?\n", __func__);
        return AH_FALSE;
    }

    if (AR_SREV_MERLIN_10_OR_LATER(ah)) {
        if (!AR_SREV_KIWI_10_OR_LATER(ah)) { /* No need to disable/enable ADC for Kiwi */
            /* Filter Cal done, disable */
            OS_REG_SET_BIT(ah, AR_PHY_ADC_CTL, AR_PHY_ADC_CTL_OFF_PWDADC);
        }
        OS_REG_CLR_BIT(ah, AR_PHY_AGC_CONTROL, AR_PHY_AGC_CONTROL_FLTR_CAL);
    }

    /* Do PA Calibration */
    if (AR_SREV_KITE(ah) && AR_SREV_KITE_11_OR_LATER(ah)) {
        ar9285PACal(ah);
    }

    /*
     * Do NF calibration after DC offset and other CALs.
     * Per system engineers, noise floor value can sometimes be 20 dB
     * higher than normal value if DC offset and noise floor cal are
     * triggered at the same time.
     */
    OS_REG_WRITE(ah, AR_PHY_AGC_CONTROL,
                 OS_REG_READ(ah, AR_PHY_AGC_CONTROL) |
                 AR_PHY_AGC_CONTROL_NF);

    /* Initialize list pointers */
    ahp->ah_cal_list = ahp->ah_cal_list_last = ahp->ah_cal_list_curr = AH_NULL;

    /*
     * Enable IQ, ADC Gain, ADC DC Offset Cals
     */
    if (AR_SREV_HOWL(ah) || AR_SREV_SOWL_10_OR_LATER(ah)) {
        /* Setup all non-periodic, init time only calibrations */
        // XXX: Init DC Offset not working yet
#ifdef not_yet
        if (AH_TRUE == ar5416IsCalSupp(ah, chan, ADC_DC_INIT_CAL)) {
            INIT_CAL(&ahp->ah_adcDcCalInitData);
            INSERT_CAL(ahp, &ahp->ah_adcDcCalInitData);
        }

        /* Initialize current pointer to first element in list */
        ahp->ah_cal_list_curr = ahp->ah_cal_list;

        if (ahp->ah_cal_list_curr) {
            if (ar5416RunInitCals(ah, 0) == AH_FALSE)
                {return AH_FALSE;}
        }
#endif
        /* end - Init time calibrations */

        /* If Cals are supported, add them to list via INIT/INSERT_CAL */
        if (AH_TRUE == ar5416IsCalSupp(ah, chan, ADC_GAIN_CAL)) {
            INIT_CAL(&ahp->ah_adcGainCalData);
            INSERT_CAL(ahp, &ahp->ah_adcGainCalData);
            HDPRINTF(ah, HAL_DBG_CALIBRATE,
                     "%s: enabling ADC Gain Calibration.\n", __func__);
        }
        if (AH_TRUE == ar5416IsCalSupp(ah, chan, ADC_DC_CAL)) {
            INIT_CAL(&ahp->ah_adcDcCalData);
            INSERT_CAL(ahp, &ahp->ah_adcDcCalData);
            HDPRINTF(ah, HAL_DBG_CALIBRATE,
                     "%s: enabling ADC DC Calibration.\n", __func__);
        }
        if (AH_TRUE == ar5416IsCalSupp(ah, chan, IQ_MISMATCH_CAL)) {
            INIT_CAL(&ahp->ah_iqCalData);
            INSERT_CAL(ahp, &ahp->ah_iqCalData);
            HDPRINTF(ah, HAL_DBG_CALIBRATE,
                     "%s: enabling IQ Calibration.\n", __func__);
        }

        /* Initialize current pointer to first element in list */
        ahp->ah_cal_list_curr = ahp->ah_cal_list;

        /* Reset state within current cal */
        if (ahp->ah_cal_list_curr)
            ar5416ResetCalibration(ah, ahp->ah_cal_list_curr);
    }

    /* Mark all calibrations on this channel as being invalid */
    ichan->CalValid = 0;

    return AH_TRUE;
}

/* ar5416ResetCalValid
 * Entry point for upper layers to restart current cal.
 * Reset the calibration valid bit in channel.
 */
void
ar5416ResetCalValid(struct ath_hal *ah, HAL_CHANNEL *chan, HAL_BOOL *isCalDone)
{
    struct ath_hal_5416 *ahp = AH5416(ah);
    HAL_CHANNEL_INTERNAL *ichan = ath_hal_checkchannel(ah, chan);
    HAL_CAL_LIST *currCal = ahp->ah_cal_list_curr;

    *isCalDone = AH_TRUE;

    if (!AR_SREV_HOWL(ah) && !AR_SREV_SOWL_10_OR_LATER(ah)) {
        return;
    }

    if (currCal == AH_NULL) {
        return;
    }

    if (ichan == AH_NULL) {
        HDPRINTF(ah, HAL_DBG_CALIBRATE,
                 "%s: invalid channel %u/0x%x; no mapping\n",
                 __func__, chan->channel, chan->channelFlags);
        return;
    }

    /* Expected that this calibration has run before, post-reset.
     * Current state should be done
     */
    if (currCal->calState != CAL_DONE) {
        HDPRINTF(ah, HAL_DBG_CALIBRATE,
                 "%s: Calibration state incorrect, %d\n",
                 __func__, currCal->calState);
        return;
    }

    /* Verify Cal is supported on this channel */
    if (ar5416IsCalSupp(ah, chan, currCal->calData->calType) == AH_FALSE) {
        return;
    }

    HDPRINTF(ah, HAL_DBG_CALIBRATE,
             "%s: Resetting Cal %d state for channel %u/0x%x\n",
             __func__, currCal->calData->calType, chan->channel,
             chan->channelFlags);

    /* Disable cal validity in channel */
    ichan->CalValid &= ~currCal->calData->calType;
    currCal->calState = CAL_WAITING;
    /* Indicate to upper layers that we need polling, Howl/Sowl bug */
    *isCalDone = AH_FALSE;
}

static inline void
ar5416SetDma(struct ath_hal *ah)
{
    u_int32_t   regval;

    /*
     * set AHB_MODE not to do cacheline prefetches
     */
    regval = OS_REG_READ(ah, AR_AHB_MODE);
    OS_REG_WRITE(ah, AR_AHB_MODE, regval | AR_AHB_PREFETCH_RD_EN);

    /*
     * let mac dma reads be in 128 byte chunks
     */
    regval = OS_REG_READ(ah, AR_TXCFG) & ~AR_TXCFG_DMASZ_MASK;
    OS_REG_WRITE(ah, AR_TXCFG, regval | AR_TXCFG_DMASZ_128B);

    /*
     * Restore TX Trigger Level to its pre-reset value.
     * The initial value depends on whether aggregation is enabled, and is 
     * adjusted whenever underruns are detected.
     */
    OS_REG_RMW_FIELD(ah, AR_TXCFG, AR_FTRIG, AH_PRIVATE(ah)->ah_txTrigLevel);

    /*
     * let mac dma writes be in 128 byte chunks
     */
    regval = OS_REG_READ(ah, AR_RXCFG) & ~AR_RXCFG_DMASZ_MASK;
    OS_REG_WRITE(ah, AR_RXCFG, regval | AR_RXCFG_DMASZ_128B);

    /*
     * Setup receive FIFO threshold to hold off TX activities
     */
    OS_REG_WRITE(ah, AR_RXFIFO_CFG, 0x200);

    /*
     * reduce the number of usable entries in PCU TXBUF to avoid
     * wrap around bugs. (bug 20428)
     */
    if (AR_SREV_KITE(ah)) {
        /* For Kite number of Fifos are reduced to half.
         * So set the usable tx buf size also to half to 
         * avoid data/delimiter underruns
         * See bug #32553 for details
         */
        OS_REG_WRITE(ah, AR_PCU_TXBUF_CTRL, AR_KITE_PCU_TXBUF_CTRL_USABLE_SIZE);
    }
    else {
        OS_REG_WRITE(ah, AR_PCU_TXBUF_CTRL, AR_PCU_TXBUF_CTRL_USABLE_SIZE);
    }
}

/* Override INI values with chip specific configuration.
 *
 */
static inline void
ar5416OverrideIni(struct ath_hal *ah, HAL_CHANNEL *chan)
{

    /* Set the RX_ABORT and RX_DIS and clear it only after
     * RXE is set for MAC. This prevents frames with
     * corrupted descriptor status.
     */
    OS_REG_SET_BIT(ah, AR_DIAG_SW, (AR_DIAG_RX_DIS | AR_DIAG_RX_ABORT));
    /*
     * For Merlin and above, there is a new feature that allows Multicast search
     * based on both MAC Address and Key ID. By default, this feature is enabled.
     * But since the driver is not using this feature, we switch it off; otherwise
     * multicast search based on MAC addr only will fail.
     */
    if (AR_SREV_MERLIN_10_OR_LATER(ah)) {
        u_int32_t val;

        val = OS_REG_READ(ah, AR_PCU_MISC_MODE2) & (~AR_PCU_MISC_MODE2_ADHOC_MCAST_KEYID_ENABLE);
        OS_REG_WRITE(ah, AR_PCU_MISC_MODE2, val);
    }

    if (!AR_SREV_5416_V20_OR_LATER(ah) || AR_SREV_MERLIN_10_OR_LATER(ah)) {
        return;
    }

    /* Disable BB clock gating
     * Necessary to avoid hangs in Owl 2.0
     */
    OS_REG_WRITE(ah, 0x9800+(651<<2), 0x11);

}

static inline void
ar5416InitBB(struct ath_hal *ah, HAL_CHANNEL *chan)
{
    u_int32_t synthDelay;

    /*
     * Wait for the frequency synth to settle (synth goes on
     * via AR_PHY_ACTIVE_EN).  Read the phy active delay register.
     * Value is in 100ns increments.
     */
    synthDelay = OS_REG_READ(ah, AR_PHY_RX_DELAY) & AR_PHY_RX_DELAY_DELAY;
    if (IS_CHAN_CCK(chan)) {
            synthDelay = (4 * synthDelay) / 22;
    } else {
            synthDelay /= 10;
    }

    /* Activate the PHY (includes baseband activate + synthesizer on) */
    OS_REG_WRITE(ah, AR_PHY_ACTIVE, AR_PHY_ACTIVE_EN);

    /*
     * There is an issue if the AP starts the calibration before
     * the base band timeout completes.  This could result in the
     * rx_clear false triggering.  As a workaround we add delay an
     * extra BASE_ACTIVATE_DELAY usecs to ensure this condition
     * does not happen.
     */
    OS_DELAY(synthDelay + BASE_ACTIVATE_DELAY);
}

static inline void
ar5416InitInterruptMasks(struct ath_hal *ah, HAL_OPMODE opmode)
{
    struct ath_hal_5416 *ahp = AH5416(ah);

    /*
     * Setup interrupt handling.  Note that ar5416ResetTxQueue
     * manipulates the secondary IMR's as queues are enabled
     * and disabled.  This is done with RMW ops to insure the
     * settings we make here are preserved.
     */
    ahp->ah_maskReg = AR_IMR_TXERR | AR_IMR_TXURN | AR_IMR_RXERR | AR_IMR_RXORN
                    | AR_IMR_BCNMISC;

    if (ahp->ah_intrMitigationRx) {
        /* enable interrupt mitigation for rx */
        ahp->ah_maskReg |= AR_IMR_RXINTM | AR_IMR_RXMINTR;
    } else {
        ahp->ah_maskReg |= AR_IMR_RXOK;
    }
    if (ahp->ah_intrMitigationTx) {
        /* enable interrupt mitigation for tx */
        ahp->ah_maskReg |= AR_IMR_TXINTM | AR_IMR_TXMINTR;
    } else {
        ahp->ah_maskReg |= AR_IMR_TXOK;
    }

    if (opmode == HAL_M_HOSTAP)
        ahp->ah_maskReg |= AR_IMR_MIB;

    OS_REG_WRITE(ah, AR_IMR, ahp->ah_maskReg);

    OS_REG_WRITE(ah, AR_IMR_S2, OS_REG_READ(ah, AR_IMR_S2) | AR_IMR_S2_GTT);

#ifndef AR9100
    /*
     * debug - enable to see all synchronous interrupts status
     */
    /* Clear any pending sync cause interrupts */
    OS_REG_WRITE(ah, AR_INTR_SYNC_CAUSE, 0xFFFFFFFF);

    /* Allow host interface sync interrupt sources to set cause bit */
    OS_REG_WRITE(ah, AR_INTR_SYNC_ENABLE, AR_INTR_SYNC_DEFAULT);

    /* _Disable_ host interface sync interrupt when cause bits set */
    OS_REG_WRITE(ah, AR_INTR_SYNC_MASK, 0);
#endif
}

static inline void
ar5416InitQOS(struct ath_hal *ah)
{
    OS_REG_WRITE(ah, AR_MIC_QOS_CONTROL, 0x100aa);  /* XXX magic */
    OS_REG_WRITE(ah, AR_MIC_QOS_SELECT, 0x3210);    /* XXX magic */

    /* Turn on NOACK Support for QoS packets */
    OS_REG_WRITE(ah, AR_QOS_NO_ACK,
            SM(2, AR_QOS_NO_ACK_TWO_BIT) |
            SM(5, AR_QOS_NO_ACK_BIT_OFF) |
            SM(0, AR_QOS_NO_ACK_BYTE_OFF));

    /*
     * initialize TXOP for all TIDs
     */
    OS_REG_WRITE(ah, AR_TXOP_X, AR_TXOP_X_VAL);
    OS_REG_WRITE(ah, AR_TXOP_0_3, 0xFFFFFFFF);
    OS_REG_WRITE(ah, AR_TXOP_4_7, 0xFFFFFFFF);
    OS_REG_WRITE(ah, AR_TXOP_8_11, 0xFFFFFFFF);
    OS_REG_WRITE(ah, AR_TXOP_12_15, 0xFFFFFFFF);
}

static inline void
ar5416InitUserSettings(struct ath_hal *ah)
{
    struct ath_hal_5416 *ahp = AH5416(ah);

    /* Restore user-specified settings */
    HDPRINTF(ah, HAL_DBG_RESET, "--AP %s ahp->ah_miscMode 0x%x\n", __func__, ahp->ah_miscMode);
    if (ahp->ah_miscMode != 0)
            OS_REG_WRITE(ah, AR_PCU_MISC, OS_REG_READ(ah, AR_PCU_MISC) | ahp->ah_miscMode);
    if (ahp->ah_slottime != (u_int) -1)
            ar5416SetSlotTime(ah, ahp->ah_slottime);
    if (ahp->ah_acktimeout != (u_int) -1)
            ar5416SetAckTimeout(ah, ahp->ah_acktimeout);
    if (ahp->ah_ctstimeout != (u_int) -1)
            ar5416SetCTSTimeout(ah, ahp->ah_ctstimeout);
    if (ahp->ah_globaltxtimeout != (u_int) -1)
        ar5416SetGlobalTxTimeout(ah, ahp->ah_globaltxtimeout);
    if (AH_PRIVATE(ah)->ah_diagreg != 0)
        OS_REG_SET_BIT(ah, AR_DIAG_SW, AH_PRIVATE(ah)->ah_diagreg);

#ifdef ATH_HEAVY_CLIP
    if ( AH_PRIVATE(ah)->ah_miscFlags & HAL_CAP_MISC_FLAGS_HEAVY_CLIP )
    {
        OS_REG_WRITE(ah,AR_PHY_HEAVY_CLIP_ENABLE,1);
        OS_REG_WRITE(ah,AR_PHY_HEAVY_CLIP_FACTOR_HT20,AR_PHY_HEAVY_CLIP_DEF_FACTOR);
        OS_REG_WRITE(ah,AR_PHY_HEAVY_CLIP_FACTOR_HT40,AR_PHY_HEAVY_CLIP_DEF_FACTOR);
    }
#endif
}

static inline void
ar5416AttachHwPlatform(struct ath_hal *ah)
{
    struct ath_hal_5416 *ahp = AH5416(ah);

    ahp->ah_hwp = HAL_TRUE_CHIP;
    return;
}

int ar5416_getSpurInfo(struct ath_hal * ah, int *enable, int len, u_int16_t *freq)
{
    struct ath_hal_private *ap = AH_PRIVATE(ah);
    int i = 0, j = 0;
    for(i =0; i < len; i++)
    {
        freq[i] =  0;
    }
    *enable =  ap->ah_config.ath_hal_spurMode;
    for(i = 0; i < AR_EEPROM_MODAL_SPURS; i++)
    {
        if(ap->ah_config.ath_hal_spurChans[i][0] != AR_NO_SPUR)
        {
            freq[j++] = ap->ah_config.ath_hal_spurChans[i][0];          
            HDPRINTF(ah, HAL_DBG_ANI,
                     "1. get spur %d\n", ap->ah_config.ath_hal_spurChans[i][0]);
        }
        if(ap->ah_config.ath_hal_spurChans[i][1] != AR_NO_SPUR)
        {
            freq[j++] = ap->ah_config.ath_hal_spurChans[i][1];
            HDPRINTF(ah, HAL_DBG_ANI, 
                     "2. get spur %d\n", ap->ah_config.ath_hal_spurChans[i][1]);
        }
    }
        
    return 0;
}

#define ATH_HAL_2GHZ_FREQ_MIN   20000
#define ATH_HAL_2GHZ_FREQ_MAX   29999
#define ATH_HAL_5GHZ_FREQ_MIN   50000
#define ATH_HAL_5GHZ_FREQ_MAX   59999

int ar5416_setSpurInfo(struct ath_hal * ah, int enable, int len, u_int16_t *freq)
{
    struct ath_hal_private *ap = AH_PRIVATE(ah);
    int i = 0, j = 0, k = 0;
    
    ap->ah_config.ath_hal_spurMode = enable;
    if(ap->ah_config.ath_hal_spurMode == SPUR_ENABLE_IOCTL)
    {
        for(i =0; i < AR_EEPROM_MODAL_SPURS; i++)
        {
            ap->ah_config.ath_hal_spurChans[i][0] = AR_NO_SPUR;
            ap->ah_config.ath_hal_spurChans[i][1] = AR_NO_SPUR;
        }
        for(i =0; i < len; i++)
        {
            /* 2GHz Spur */
            if(freq[i] > ATH_HAL_2GHZ_FREQ_MIN && freq[i] < ATH_HAL_2GHZ_FREQ_MAX)
            {
                if(j < AR_EEPROM_MODAL_SPURS)
                {
                    ap->ah_config.ath_hal_spurChans[j++][1] =  freq[i];
                    HDPRINTF(ah, HAL_DBG_ANI, "1 set spur %d\n", freq[i]);
                }
            }
            /* 5Ghz Spur */
            else if(freq[i] > ATH_HAL_5GHZ_FREQ_MIN && freq[i] < ATH_HAL_5GHZ_FREQ_MAX)
            {
                if(k < AR_EEPROM_MODAL_SPURS)
                {
                    ap->ah_config.ath_hal_spurChans[k++][0] =  freq[i];
                    HDPRINTF(ah, HAL_DBG_ANI, "2 set spur %d\n", freq[i]);
                }
            }   
        }
    }
        
    return 0;
}

static void
ar5416OpenLoopPowerControlTempCompensationKiwi(struct ath_hal *ah)
{
    u_int32_t    rddata;
    int32_t      delta, currPDADC, slope;
    struct ath_hal_5416     *ahp = AH5416(ah);

    rddata = OS_REG_READ(ah, AR_PHY_TX_PWRCTRL4);

    currPDADC = MS(rddata, AR_PHY_TX_PWRCTRL_PD_AVG_OUT);
    if (ahp->initPDADC == 0 || currPDADC == 0) {
        return; // do not run temperature compensation if either is zero.
    } else {
        slope = ar5416EepromGet(ahp, EEP_TEMPSENSE_SLOPE);
        if (slope == 0) { /* to avoid divide by zero case */
            delta = 0;
        } else {
            delta = ((currPDADC - ahp->initPDADC)*4) / slope;
        }
        OS_REG_RMW_FIELD(ah, AR_PHY_CH0_TX_PWRCTRL11,
            AR_PHY_TX_PWRCTRL_OLPC_TEMP_COMP, delta);
        OS_REG_RMW_FIELD(ah, AR_PHY_CH1_TX_PWRCTRL11,
            AR_PHY_TX_PWRCTRL_OLPC_TEMP_COMP, delta);
    }
}

static void
ar5416OpenLoopPowerControlTempCompensation(struct ath_hal *ah)
{
    u_int32_t    rddata, i;
    int32_t      delta, currPDADC, regval;
    struct ath_hal_5416     *ahp = AH5416(ah);

    if (AR_SREV_KIWI_10_OR_LATER(ah) && ar5416EepromGet(ahp, EEP_OL_PWRCTRL)) {
        ar5416OpenLoopPowerControlTempCompensationKiwi(ah);
    } else {
        rddata = OS_REG_READ(ah, AR_PHY_TX_PWRCTRL4);
        currPDADC = MS(rddata, AR_PHY_TX_PWRCTRL_PD_AVG_OUT);

        if (ar5416EepromGet(ahp, EEP_DAC_HPWR_5G)) {
            delta = (currPDADC - ahp->initPDADC + 4) / 8;
        } else {
            delta = (currPDADC - ahp->initPDADC + 5) / 10;
        }
        if ((delta != ahp->PDADCdelta)) {
            ahp->PDADCdelta = delta;
            for(i = 1; i < MERLIN_TX_GAIN_TABLE_SIZE; i++) {
                regval = ahp->originalGain[i] - delta;
                if (regval < 0) regval = 0;
                OS_REG_RMW_FIELD(ah, AR_PHY_TX_GAIN_TBL1 + i * 4,
                    AR_PHY_TX_GAIN, regval);
            }
        }
    }
}

/* PDADC vs Tx power table on open-loop power control mode */
static void
ar5416OpenLoopPowerControlInit(struct ath_hal *ah)
{
    u_int32_t i;
    struct ath_hal_5416     *ahp = AH5416(ah);

    if (AR_SREV_KIWI_10_OR_LATER(ah) && ar5416EepromGet(ahp, EEP_OL_PWRCTRL)) {
        OS_REG_SET_BIT(ah, AR_PHY_TX_PWRCTRL9,
            AR_PHY_TX_PWRCTRL9_RES_DC_REMOVAL);
        analogShiftRegRMW(ah, AR9287_AN_TXPC0, AR9287_AN_TXPC0_TXPCMODE,
            AR9287_AN_TXPC0_TXPCMODE_S, AR9287_AN_TXPC0_TXPCMODE_TEMPSENSE);
        /* delay for RF register writes */
        OS_DELAY(100);
    } else {
        for (i = 0; i < MERLIN_TX_GAIN_TABLE_SIZE; i++) {
            ahp->originalGain[i] =
                MS(OS_REG_READ(ah, AR_PHY_TX_GAIN_TBL1 + i*4), AR_PHY_TX_GAIN);
        }
        ahp->PDADCdelta = 0;
    }
}

static inline void
ar5416InitMFP(struct ath_hal * ah)
{
    u_int32_t   mfpcap;
    /* legacy hardware. No need to setup any MFP registers */
    if (!AR_SREV_SOWL_10_OR_LATER(ah)) {
        return;
    }
    ath_hal_getcapability(ah, HAL_CAP_MFP, 0, &mfpcap);
    if(mfpcap == HAL_MFP_QOSDATA) {
        /* Treat like legacy hardware. Do not touch the MFP registers. */
        return;
    }

    /* MFP support (Sowl 1.0 or greater) */
    if (mfpcap == HAL_MFP_HW_CRYPTO) {
        /* configure hardware MFP support */
        OS_REG_RMW_FIELD(ah, AR_AES_MUTE_MASK1, AR_AES_MUTE_MASK1_FC_MGMT, 0xE7FF);
        OS_REG_SET_BIT(ah, AR_PCU_MISC_MODE2, AR_PCU_MISC_MODE2_MGMT_CRYPTO_ENABLE);
        OS_REG_CLR_BIT(ah, AR_PCU_MISC_MODE2, AR_PCU_MISC_MODE2_NO_CRYPTO_FOR_NON_DATA_PKT);
        OS_REG_RMW_FIELD(ah, AR_PCU_MISC_MODE2, AR_PCU_MISC_MODE2_MGMT_QOS, 0xF);
    } else if (mfpcap == HAL_MFP_PASSTHRU) {
        /* Disable en/decrypt by hardware */
        OS_REG_CLR_BIT(ah, AR_PCU_MISC_MODE2, AR_PCU_MISC_MODE2_MGMT_CRYPTO_ENABLE);
        OS_REG_SET_BIT(ah, AR_PCU_MISC_MODE2, AR_PCU_MISC_MODE2_NO_CRYPTO_FOR_NON_DATA_PKT);
    }
}

#endif /* AH_SUPPORT_AR5416 */

