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
#include "ah_internal.h"

#include "ar5416/ar5416.h"
#include "ar5416/ar5416reg.h"
#include "ar5416/ar5416desc.h"

/*
 * Notify Power Mgt is enabled in self-generated frames.
 * If requested, force chip awake.
 *
 * Returns A_OK if chip is awake or successfully forced awake.
 *
 * WARNING WARNING WARNING
 * There is a problem with the chip where sometimes it will not wake up.
 */
HAL_BOOL
ar5416SetPowerModeAwake(struct ath_hal *ah, int setChip)
{
#define POWER_UP_TIME   200000
    u_int32_t val;
    int i;

    if (setChip) {
        /* Do a Power-On-Reset if OWL is shutdown */
        if ((OS_REG_READ(ah, AR_RTC_STATUS) & AR_RTC_STATUS_M) ==
                AR_RTC_STATUS_SHUTDOWN) {
            if (ar5416SetResetReg(ah, HAL_RESET_POWER_ON) != AH_TRUE) {
                HALASSERT(0);
                return AH_FALSE;
            }
        }
        if(AR_SREV_HOWL(ah)) /* HOWL needs this bit to set to wake up -was cleared in ar5416SetPowerModeSleep() */
            OS_REG_SET_BIT(ah, AR_RTC_RESET, AR_RTC_RESET_EN);
        
        OS_REG_SET_BIT(ah, AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN);

        if (AR_SREV_HOWL(ah)) {
           OS_DELAY(10000);   /* Give chip the chance to awake */
        } else {
           OS_DELAY(50);
        }

        for (i = POWER_UP_TIME / 50; i > 0; i--) {
            val = OS_REG_READ(ah, AR_RTC_STATUS) & AR_RTC_STATUS_M;
            if (val == AR_RTC_STATUS_ON)
                break;
            OS_DELAY(50);
            OS_REG_SET_BIT(ah, AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN);
        }
        if (i == 0) {
            HDPRINTF(ah, HAL_DBG_POWER_MGMT, "%s: Failed to wakeup in %uus\n",
                     __func__, POWER_UP_TIME/20);
            return AH_FALSE;
        }

    }

    OS_REG_CLR_BIT(ah, AR_STA_ID1, AR_STA_ID1_PWR_SAV);
    return AH_TRUE;
#undef POWER_UP_TIME
}

/*
 * Notify Power Mgt is disabled in self-generated frames.
 * If requested, force chip to sleep.
 */
static void
ar5416SetPowerModeSleep(struct ath_hal *ah, int setChip)
{
    OS_REG_SET_BIT(ah, AR_STA_ID1, AR_STA_ID1_PWR_SAV);
    if (setChip) {
        /* Clear the RTC force wake bit to allow the mac to go to sleep */
        OS_REG_CLR_BIT(ah, AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN);
        if(!AR_SREV_HOWL(ah)) /*HOWL hangs in this case --AJP*/
            OS_REG_WRITE(ah, AR_RC, AR_RC_AHB|AR_RC_HOSTIF);
        /* Shutdown chip. Active low */
        OS_REG_CLR_BIT(ah, AR_RTC_RESET, AR_RTC_RESET_EN);
    }
}

/*
 * Notify Power Management is enabled in self-generating
 * frames. If request, set power mode of chip to
 * auto/normal.  Duration in units of 128us (1/8 TU).
 */
static void
ar5416SetPowerModeNetworkSleep(struct ath_hal *ah, int setChip)
{
    OS_REG_SET_BIT(ah, AR_STA_ID1, AR_STA_ID1_PWR_SAV);
    if (setChip) {
        HAL_CAPABILITIES *pCap = &AH_PRIVATE(ah)->ah_caps;

        if (! pCap->halAutoSleepSupport) {
            /* Set WakeOnInterrupt bit; clear ForceWake bit */
            OS_REG_WRITE(ah, AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_ON_INT);
        }
        else {
            /* Clear the RTC force wake bit to allow the mac to go to sleep */
            OS_REG_CLR_BIT(ah, AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_EN);
        }
    }
}

/*
 * Set power mgt to the requested mode, and conditionally set
 * the chip as well
 */
HAL_BOOL
ar5416SetPowerMode(struct ath_hal *ah, HAL_POWER_MODE mode, int setChip)
{
    struct ath_hal_5416 *ahp = AH5416(ah);
#ifdef AH_DEBUG
    static const char* modes[] = {
        "AWAKE",
        "FULL-SLEEP",
        "NETWORK SLEEP",
        "UNDEFINED"
    };
#endif
    int status = AH_TRUE;

    HDPRINTF(ah, HAL_DBG_POWER_MGMT, "%s: %s -> %s (%s)\n", __func__,
        modes[ahp->ah_powerMode], modes[mode],
        setChip ? "set chip " : "");

    switch (mode) {
    case HAL_PM_AWAKE:
        status = ar5416SetPowerModeAwake(ah, setChip);
        break;
    case HAL_PM_FULL_SLEEP:
        ar5416SetPowerModeSleep(ah, setChip);
        ahp->ah_chipFullSleep = AH_TRUE;
        break;
    case HAL_PM_NETWORK_SLEEP:
        ar5416SetPowerModeNetworkSleep(ah, setChip);
        break;
    default:
        HDPRINTF(ah, HAL_DBG_POWER_MGMT, "%s: unknown power mode %u\n", __func__, mode);
        return AH_FALSE;
    }
    ahp->ah_powerMode = mode;
    return status;
}

/*
 * Return the current sleep mode of the chip
 */
HAL_POWER_MODE
ar5416GetPowerMode(struct ath_hal *ah)
{
    int mode = OS_REG_READ(ah, AR_RTC_STATUS) & AR_RTC_STATUS_M;
    switch (mode) {
    case AR_RTC_STATUS_ON:
    case AR_RTC_STATUS_WAKEUP:
        return HAL_PM_AWAKE;
        break;
    case AR_RTC_STATUS_SLEEP:
        return HAL_PM_NETWORK_SLEEP;
        break;
    case AR_RTC_STATUS_SHUTDOWN:
        return HAL_PM_FULL_SLEEP;
        break;
    default:
        HDPRINTF(ah, HAL_DBG_POWER_MGMT, "%s: unknown power mode 0x%x\n", __func__, mode);
        return HAL_PM_UNDEFINED;    
    }
}

/*  
 * Set SM power save mode
 */  
void
ar5416SetSmPowerMode(struct ath_hal *ah, HAL_SMPS_MODE mode)
{
    int regval;
    struct ath_hal_5416 *ahp = AH5416(ah);

    if (ar5416GetCapability(ah, HAL_CAP_DYNAMIC_SMPS, 0, AH_NULL) != HAL_OK)
        return;
    
    /* Program low & high power chainmask settings and enable MAC control */
    regval = SM(AR_PCU_SMPS_LPWR_CHNMSK_VAL, AR_PCU_SMPS_LPWR_CHNMSK) |
             SM(ahp->ah_rxchainmask, AR_PCU_SMPS_HPWR_CHNMSK) |
             AR_PCU_SMPS_MAC_CHAINMASK;

    /* Program registers according to required SM power mode.*/
    switch (mode) {
    case HAL_SMPS_SW_CTRL_LOW_PWR:
        OS_REG_WRITE(ah, AR_PCU_SMPS, regval);
        break;
    case HAL_SMPS_SW_CTRL_HIGH_PWR:
        OS_REG_WRITE(ah, AR_PCU_SMPS, regval | AR_PCU_SMPS_SW_CTRL_HPWR);
        break;
    case HAL_SMPS_HW_CTRL:
        OS_REG_WRITE(ah, AR_PCU_SMPS, regval | AR_PCU_SMPS_HW_CTRL_EN);
        break;
    case HAL_SMPS_DEFAULT:
        OS_REG_WRITE(ah, AR_PCU_SMPS, 0);
        break;
    default:
        break;
    }
    ahp->ah_smPowerMode = mode;
}

#if ATH_WOW
/* 
 * This routine is called to configure the SerDes register for the
 * Merlin 2.0 and above chip during WOW sleep.
 */
static void
ar9280ConfigSerDes_WowSleep(struct ath_hal *ah)
{
    int i;
    struct ath_hal_5416 *ahp = AH5416(ah);

    /*
     * For WOW sleep, we reprogram the SerDes so that the PLL and CHK REQ
     * are both enabled. This uses more power but the Apple team reported
     * that otherwise, WOW sleep is unstable and chip may disappears.
     */
    for (i = 0; i < ahp->ah_iniPcieSerdesWow.ia_rows; i++) {
        OS_REG_WRITE(ah, INI_RA(&ahp->ah_iniPcieSerdesWow, i, 0), INI_RA(&ahp->ah_iniPcieSerdesWow, i, 1));
    }
    OS_DELAY(1000);
}

static HAL_BOOL
ar5416WowCreateKeepAlivePattern(struct ath_hal *ah)
{
    struct ath_hal_5416 *ahp = AH5416(ah);
    u_int32_t  frame_len = 28;
    u_int32_t  tpc = 0x3f;
    u_int32_t  antenna_mode = 1;
    u_int32_t  transmit_rate;
    u_int32_t  frame_type = 0x2;    // Frame Type -> Data; 
    u_int32_t  sub_type = 0x4;      // Subtype -> Null Data
    u_int32_t  to_ds = 1;
    u_int32_t  duration_id = 0x3d;
    u_int8_t   *StaMacAddr, *ApMacAddr;
    u_int8_t   *addr1, *addr2, *addr3;
    u_int32_t  ctl[12] = { 0 };
    u_int32_t  data_word0 = 0, data_word1 = 0, data_word2 = 0,
               data_word3 = 0, data_word4 = 0, data_word5 = 0;
    u_int32_t  i;

    StaMacAddr = (u_int8_t *)ahp->ah_macaddr;
    ApMacAddr = (u_int8_t *)ahp->ah_bssid;
    addr2 = StaMacAddr;
    addr1 = addr3 = ApMacAddr;

    if(AH_PRIVATE(ah)->ah_curchan->channelFlags & CHANNEL_CCK) {
        transmit_rate = 0x1B;    // CCK_1M
    } else {
        transmit_rate = 0xB;     // OFDM_6M
    }

    /* Set the Transmit Buffer. */
    ctl[0] = (frame_len | (tpc << 16)) + (antenna_mode << 25);
    ctl[1] = 0;
    ctl[2] = 0x7 << 16;  /* tx_tries0 */
    ctl[3] = transmit_rate;
    ctl[4] = 0;
    ctl[7] = ahp->ah_txchainmask << 2;
    
    for (i = 0; i < 12; i++) {
        OS_REG_WRITE(ah, (AR_WOW_KA_DESC_WORD2 + i * 4), ctl[i]);
    }

    data_word0 = (frame_type << 2) | (sub_type << 4) | (to_ds << 8) | (duration_id << 16);
    data_word1 = (((u_int32_t)addr1[3] << 24) | ((u_int32_t)addr1[2] << 16) |
                  ((u_int32_t)addr1[1]) << 8 | ((u_int32_t)addr1[0]));
    data_word2 = (((u_int32_t)addr2[1] << 24) | ((u_int32_t)addr2[0] << 16) |
                  ((u_int32_t)addr1[5]) << 8 | ((u_int32_t)addr1[4]));
    data_word3 = (((u_int32_t)addr2[5] << 24) | ((u_int32_t)addr2[4] << 16) |
                  ((u_int32_t)addr2[3]) << 8 | ((u_int32_t)addr2[2]));
    data_word4 = (((u_int32_t)addr3[3] << 24) | ((u_int32_t)addr3[2] << 16) |
                  ((u_int32_t)addr3[1]) << 8 | (u_int32_t)addr3[0]);
    data_word5 = (((u_int32_t)addr3[5]) << 8 | ((u_int32_t)addr3[4]));

    OS_REG_WRITE(ah, AR_WOW_KA_DATA_WORD0, data_word0);
    OS_REG_WRITE(ah, AR_WOW_KA_DATA_WORD1, data_word1);
    OS_REG_WRITE(ah, AR_WOW_KA_DATA_WORD2, data_word2);
    OS_REG_WRITE(ah, AR_WOW_KA_DATA_WORD3, data_word3);
    OS_REG_WRITE(ah, AR_WOW_KA_DATA_WORD4, data_word4);
    OS_REG_WRITE(ah, AR_WOW_KA_DATA_WORD5, data_word5);

    return AH_TRUE;
}

/* TBD: Should querying hal for hardware capability */
#define MAX_PATTERN_SIZE            256
#define MAX_PATTERN_MASK_SIZE       32
#define MAX_NUM_USER_PATTERN        6    /* Deducting the disassociate/deauthenticate packets */

void
ar5416WowApplyPattern(struct ath_hal *ah, u_int8_t *pAthPattern, u_int8_t *pAthMask, int32_t pattern_count, u_int32_t athPatternLen)
{
    int i;
    u_int32_t    reg_pat[] = {
                  AR_WOW_TB_PATTERN0,
                  AR_WOW_TB_PATTERN1,
                  AR_WOW_TB_PATTERN2,
                  AR_WOW_TB_PATTERN3,
                  AR_WOW_TB_PATTERN4,
                  AR_WOW_TB_PATTERN5,
                  AR_WOW_TB_PATTERN6,
                  AR_WOW_TB_PATTERN7
                 };
    u_int32_t    reg_mask[] = {
                  AR_WOW_TB_MASK0,
                  AR_WOW_TB_MASK1,
                  AR_WOW_TB_MASK2,
                  AR_WOW_TB_MASK3,
                  AR_WOW_TB_MASK4,
                  AR_WOW_TB_MASK5,
                  AR_WOW_TB_MASK6,
                  AR_WOW_TB_MASK7
                 };
    u_int32_t   pattern_val;
    u_int32_t   mask_val;
    u_int8_t    mask_bit = 0x1;
    u_int8_t    pattern;

    /* TBD: should check count by querying the hardware capability */
    if (pattern_count >= MAX_NUM_USER_PATTERN) {
        return;
    }

    pattern = (u_int8_t)OS_REG_READ(ah, AR_WOW_PATTERN_REG);
    pattern = pattern | (mask_bit << pattern_count);
    OS_REG_WRITE(ah, AR_WOW_PATTERN_REG, pattern);

    /* Set the registers for pattern */
    for (i = 0; i < MAX_PATTERN_SIZE; i+=4) {
        pattern_val = (((u_int32_t)pAthPattern[i]) |
                       ((u_int32_t)pAthPattern[i+1] << 8) |
                       ((u_int32_t)pAthPattern[i+2] << 16) |
                       ((u_int32_t)pAthPattern[i+3] << 24));
        OS_REG_WRITE(ah, (reg_pat[pattern_count] + i), pattern_val);
    }   

    /* Set the registers for mask */
    for (i = 0; i < MAX_PATTERN_MASK_SIZE; i+=4) {
        mask_val = (((u_int32_t)pAthMask[i]) |
                    ((u_int32_t)pAthMask[i+1] << 8) |
                    ((u_int32_t)pAthMask[i+2] << 16) |
                    ((u_int32_t)pAthMask[i+3] << 24));
        OS_REG_WRITE(ah, (reg_mask[pattern_count] + i), mask_val);
    }

    if (AR_SREV_KITE_10_OR_LATER(ah)) {
        /* Set the pattern length to be matched */
        u_int32_t val;
        if (pattern_count < 4) {
            /* Pattern 0-3 uses AR_WOW_LENGTH1_REG register */
            val = OS_REG_READ(ah, AR_WOW_LENGTH1_REG);
            val = ((val & (~AR_WOW_LENGTH1_MASK(pattern_count))) | 
                   ((athPatternLen & AR_WOW_LENGTH_MAX) << AR_WOW_LENGTH1_SHIFT(pattern_count)));
            OS_REG_WRITE(ah, AR_WOW_LENGTH1_REG, val);
        }
        else {
            /* Pattern 4-7 uses AR_WOW_LENGTH2_REG register */
            val = OS_REG_READ(ah, AR_WOW_LENGTH2_REG);
            val = ((val & (~AR_WOW_LENGTH2_MASK(pattern_count))) | 
                   ((athPatternLen & AR_WOW_LENGTH_MAX) << AR_WOW_LENGTH2_SHIFT(pattern_count)));
            OS_REG_WRITE(ah, AR_WOW_LENGTH2_REG, val);
        }
    }

    AH_PRIVATE(ah)->ah_wowEventMask |= (1 << (pattern_count + AR_WOW_PATTERN_FOUND_SHIFT));
    
    return;
}

HAL_BOOL
ar5416SetPowerModeWowSleep(struct ath_hal *ah)
{
    OS_REG_SET_BIT(ah, AR_STA_ID1, AR_STA_ID1_PWR_SAV);

    OS_REG_WRITE(ah, AR_CR, AR_CR_RXD);    /* Set receive disable bit */
    if (!ath_hal_wait(ah, AR_CR, AR_CR_RXE, 0, AH_WAIT_TIMEOUT)) {
        HDPRINTF(ah, HAL_DBG_POWER_MGMT, "%s: dma failed to stop in 10ms\n"
                 "AR_CR=0x%08x\nAR_DIAG_SW=0x%08x\n", __func__,
                 OS_REG_READ(ah, AR_CR), OS_REG_READ(ah, AR_DIAG_SW));
        return AH_FALSE;
    } else {
        OS_REG_WRITE(ah, AR_RXDP, 0x0);

        /* Merlin 2.0/2.1 WOW has sleep issue, do not set it to sleep */
        if (AR_SREV_MERLIN_20(ah)) {
            return AH_TRUE;
        } else {
            OS_REG_WRITE(ah, AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_ON_INT);
        }

        return AH_TRUE;
    }
}

u_int32_t
ar5416WowEnable(struct ath_hal *ah, u_int32_t patternEnable)
{
    uint32_t init_val, val, rval=0;
    const int ka_timo = 900;    /* Send a Keep-Alive frame every 900 millisec */
    const int ka_delay = 4;     /* Delay of 4 millisec between two KeepAlive's */
    uint32_t wow_event_mask;

    /*
     * ah_wowEventMask is a mask to the AR_WOW_PATTERN_REG register to indicate
     * which WOW events that we have enabled. The WOW Events are from the
     * patternEnable in this function and pattern_count of ar5416WowApplyPattern()
     */
    wow_event_mask = AH_PRIVATE(ah)->ah_wowEventMask;

    /*
     * Untie Power-On-Reset from the PCI-E Reset. When we are in WOW sleep,
     * we do not want the Reset from the PCI-E to disturb our hw state.
     */
     if (AR_SREV_MERLIN_20_OR_LATER(ah) && 
        (AH_PRIVATE(ah)->ah_isPciExpress == AH_TRUE)) {
        /* 
         * We need to untie the internal POR (power-on-reset) to the external
         * PCI-E reset. We also need to tie the PCI-E Phy reset to the PCI-E reset.
         */
        u_int32_t wa_reg_val;
        if (AR_SREV_KITE(ah))
            wa_reg_val = AR9285_WA_DEFAULT;
        else
            wa_reg_val = AR9280_WA_DEFAULT;
        wa_reg_val = wa_reg_val & ~(AR_WA_UNTIE_RESET_EN);
        wa_reg_val = wa_reg_val | AR_WA_RESET_EN | AR_WA_POR_SHORT;
        OS_REG_WRITE(ah, AR_WA, wa_reg_val);
    
        if (!AR_SREV_KITE(ah)) {
            /* 
             * For WOW sleep, we reprogram the SerDes so that the PLL and CHK REQ
             * are both enabled. This uses more power but the Apple team reported
             * that otherwise, WOW sleep is unable and chip may disappears.
             */
             ar9280ConfigSerDes_WowSleep(ah);
        }
    }

    /*
     * Set the power states appropriately and enable pme.
     */
    val = OS_REG_READ(ah, AR_PCIE_PM_CTRL);
    val |= AR_PMCTRL_HOST_PME_EN | \
           AR_PMCTRL_PWR_PM_CTRL_ENA | AR_PMCTRL_AUX_PWR_DET;

    val &= ~AR_PMCTRL_WOW_PME_CLR;
    OS_REG_WRITE(ah, AR_PCIE_PM_CTRL, val);

    /*
     * Setup for for: 
     *     - beacon misses
     *     - magic pattern
     *     - keep alive timeout
     *     - pattern matching 
     */

    /*
     * Program some default values for keep-alives, beacon misses, etc.
     */
    init_val = OS_REG_READ(ah, AR_WOW_PATTERN_REG);
    val = AR_WOW_BACK_OFF_SHIFT(AR_WOW_PAT_BACKOFF) | init_val;
    OS_REG_WRITE(ah, AR_WOW_PATTERN_REG, val);
    rval = OS_REG_READ(ah, AR_WOW_PATTERN_REG);
    
    init_val = OS_REG_READ(ah, AR_WOW_COUNT_REG);
    val = AR_WOW_AIFS_CNT(AR_WOW_CNT_AIFS_CNT) | \
          AR_WOW_SLOT_CNT(AR_WOW_CNT_SLOT_CNT) | \
          AR_WOW_KEEP_ALIVE_CNT(AR_WOW_CNT_KA_CNT);
    OS_REG_WRITE(ah, AR_WOW_COUNT_REG, val);
    rval = OS_REG_READ(ah, AR_WOW_COUNT_REG);
  
    
    init_val = OS_REG_READ(ah, AR_WOW_BCN_TIMO_REG);
    if (patternEnable & AH_WOW_BEACON_MISS) {
        val = AR_WOW_BEACON_TIMO;
    }
    else {
        /* We are not using the beacon miss. Program a large value. */
        val = AR_WOW_BEACON_TIMO_MAX;
    }
    OS_REG_WRITE(ah, AR_WOW_BCN_TIMO_REG, val);
    rval = OS_REG_READ(ah, AR_WOW_BCN_TIMO_REG);

    init_val = OS_REG_READ(ah, AR_WOW_KEEP_ALIVE_TIMO_REG);

    /* 
     * Keep Alive Timo in ms.
     */
    if (patternEnable == 0) {
        val =  AR_WOW_KEEP_ALIVE_NEVER;
    } else {
        val =  ka_timo * 32;
    }
    OS_REG_WRITE(ah, AR_WOW_KEEP_ALIVE_TIMO_REG, val);
    rval = OS_REG_READ(ah, AR_WOW_KEEP_ALIVE_TIMO_REG);

    init_val = OS_REG_READ(ah, AR_WOW_KEEP_ALIVE_DELAY_REG);
    /*
     * Keep Alive delay in us.
     */
    val = ka_delay * 1000;
    OS_REG_WRITE(ah, AR_WOW_KEEP_ALIVE_DELAY_REG, val);
    rval = OS_REG_READ(ah, AR_WOW_KEEP_ALIVE_DELAY_REG);

    /*
     * Create KeepAlive Pattern to respond to beacons.
     */
    ar5416WowCreateKeepAlivePattern(ah);

    /*
     * Configure Mac Wow Registers.
     */

    val = OS_REG_READ(ah, AR_WOW_KEEP_ALIVE_REG);
    /*
     * Send keep alive timeouts anyway.
     */
    val &= ~AR_WOW_KEEP_ALIVE_AUTO_DIS;

    if (patternEnable & AH_WOW_LINK_CHANGE) {
        val &= ~ AR_WOW_KEEP_ALIVE_FAIL_DIS;
        wow_event_mask |= AR_WOW_KEEP_ALIVE_FAIL;
    } else {
        val |=  AR_WOW_KEEP_ALIVE_FAIL_DIS;
    }
    OS_REG_WRITE(ah, AR_WOW_KEEP_ALIVE_REG, val);
    val = OS_REG_READ(ah, AR_WOW_KEEP_ALIVE_REG);
    val = OS_REG_READ(ah, AR_WOW_BCN_EN_REG);
    
    /*
     * We are relying on a bmiss failure. Ensure we have enough
     * threshold to prevent false positives.
     */
    OS_REG_RMW_FIELD(ah, AR_RSSI_THR, AR_RSSI_THR_BM_THR,
        AR_WOW_BMISSTHRESHOLD);

    /*
     * Beacon miss & user pattern events are broken in Owl.
     * Enable only for Merlin.
     * The workaround is done at the ath_dev layer using the 
     * sc->sc_wow_bmiss_intr field. 
     * XXX: we need to cleanup this workaround before the next chip that
     * fixes this issue.
     */
    if (!AR_SREV_MERLIN_10_OR_LATER(ah)) 
        patternEnable &= ~AH_WOW_BEACON_MISS;

    if (patternEnable & AH_WOW_BEACON_MISS) {
        val |= AR_WOW_BEACON_FAIL_EN;
        wow_event_mask |= AR_WOW_BEACON_FAIL;
    } else {
        val &= ~AR_WOW_BEACON_FAIL_EN;
    }
    OS_REG_WRITE(ah, AR_WOW_BCN_EN_REG, val);
    val = OS_REG_READ(ah, AR_WOW_BCN_EN_REG);

    /*
     * Enable the magic packet registers.
     */
    val = OS_REG_READ(ah, AR_WOW_PATTERN_REG);
    if (patternEnable & AH_WOW_MAGIC_PATTERN_EN) {
        val |= AR_WOW_MAGIC_EN;
        wow_event_mask |= AR_WOW_MAGIC_PAT_FOUND;
    } else {
        val &= ~AR_WOW_MAGIC_EN;
    }
    val |= AR_WOW_MAC_INTR_EN;
    OS_REG_WRITE(ah, AR_WOW_PATTERN_REG, val);
    val = OS_REG_READ(ah, AR_WOW_PATTERN_REG);

    /* For Kite and later version of the chips
     * enable wow pattern match for packets less than
     * 256 bytes for all patterns.
     */
    if (AR_SREV_KITE_10_OR_LATER(ah)) {
        OS_REG_WRITE(ah, AR_WOW_PATTERN_MATCH_LT_256B_REG, AR_WOW_PATTERN_SUPPORTED);
    }

    /*
     * Set the power states appropriately and enable pme.
     */
    val = OS_REG_READ(ah, AR_PCIE_PM_CTRL);
    val |=  AR_PMCTRL_PWR_STATE_D1D3 | AR_PMCTRL_HOST_PME_EN | AR_PMCTRL_PWR_PM_CTRL_ENA;
    OS_REG_WRITE(ah, AR_PCIE_PM_CTRL, val);
    
    ar5416SetPowerModeWowSleep(ah);

    AH_PRIVATE(ah)->ah_wowEventMask = wow_event_mask;

    return (AH_TRUE);
}

u_int32_t
ar5416WowWakeUp(struct ath_hal *ah)
{
    uint32_t wowStatus = 0;
    uint32_t val = 0, rval;

    /*
     * Read the WOW Status register to know the wakeup reason.
     */
    rval = OS_REG_READ(ah, AR_WOW_PATTERN_REG);
    val = AR_WOW_STATUS(rval);
    
    /* 
     * Mask only the WOW events that we have enabled. Sometimes, we have spurious
     * WOW events from the AR_WOW_PATTERN_REG register. This mask will clean it up.
     */
    val &= AH_PRIVATE(ah)->ah_wowEventMask;
    
    if (val) {
        if (val & AR_WOW_MAGIC_PAT_FOUND) {
            wowStatus |= AH_WOW_MAGIC_PATTERN_EN;
        }
        if (AR_WOW_PATTERN_FOUND(val)) {
            wowStatus |= AH_WOW_USER_PATTERN_EN;
        }
        if (val & AR_WOW_KEEP_ALIVE_FAIL) {
            wowStatus |= AH_WOW_LINK_CHANGE;
        }
        if (val & AR_WOW_BEACON_FAIL) {
            wowStatus |= AH_WOW_BEACON_MISS;
        }
    }

    /*
     * Set and clear WOW_PME_CLEAR registers for the chip to generate next wow signal.
     * Disable D3 before accessing other registers ?
     */
    val = OS_REG_READ(ah, AR_PCIE_PM_CTRL);
    val &= ~AR_PMCTRL_PWR_STATE_D1D3; // Check the bit value 0x01000000 (7-10) ??
    val |= AR_PMCTRL_WOW_PME_CLR;
    OS_REG_WRITE(ah, AR_PCIE_PM_CTRL, val);

    /*
     * Clear all events.
     */
    OS_REG_WRITE(ah, AR_WOW_PATTERN_REG,
        AR_WOW_CLEAR_EVENTS(OS_REG_READ(ah, AR_WOW_PATTERN_REG)));

    /*
     * Tie reset register.
     * FIXME: Per David Quan not tieing it back might have some repurcussions.
     */
    if (AR_SREV_MERLIN_10_OR_LATER(ah)) {
        OS_REG_WRITE(ah, AR_WA, OS_REG_READ(ah, AR_WA) |
            AR_WA_UNTIE_RESET_EN | AR_WA_POR_SHORT | AR_WA_RESET_EN);
    }

    /* Restore the Beacon Threshold to init value */
    OS_REG_WRITE(ah, AR_RSSI_THR, INIT_RSSI_THR);

    /*
     * Restore the way the PCI-E Reset, Power-On-Reset, external PCIE_POR_SHORT
     * pins are tied to its original value. Previously just before WOW sleep,
     * we untie the PCI-E Reset to our Chip's Power On Reset so that
     * any PCI-E reset from the bus will not reset our chip.
     */
    if (AR_SREV_MERLIN_20_OR_LATER(ah) && 
        (AH_PRIVATE(ah)->ah_isPciExpress == AH_TRUE)) {
    
        ar5416ConfigPciPowerSave(ah, 0);
    }

    AH_PRIVATE(ah)->ah_wowEventMask = 0;

    return (wowStatus);
}

void
ar5416WowSetGpioResetLow(struct ath_hal *ah)
{
    uint32_t val;

    val = OS_REG_READ(ah, AR_GPIO_OE_OUT);
    val |= (1 << (2 * 2));
    OS_REG_WRITE(ah, AR_GPIO_OE_OUT, val);
    val = OS_REG_READ(ah, AR_GPIO_OE_OUT);
    val = OS_REG_READ(ah,AR_GPIO_IN_OUT );
}
#endif /* ATH_WOW */

#endif /* AH_SUPPORT_AR5416 */
