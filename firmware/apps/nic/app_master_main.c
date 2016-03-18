/*
 * Copyright 2014-2015 Netronome, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * @file          app_master_main.c
 * @brief         ME serving as the NFD NIC application master.
 *
 * This implementation only handles one 40G port and one PCIe island.
 */

#include <assert.h>
#include <nfp.h>
#include <nfp_chipres.h>

#include <nic/nic.h>

#include <nfp/link_state.h>
#include <nfp/me.h>
#include <nfp/mem_bulk.h>
#include <nfp/macstats.h>
#include <nfp/xpb.h>
#include <nfp6000/nfp_mac.h>
#include <nfp6000/nfp_me.h>

#include <std/synch.h>
#include <std/reg_utils.h>

#include <vnic/shared/nfd_cfg.h>
#include <vnic/pci_in.h>
#include <vnic/pci_out.h>

#include <shared/nfp_net_ctrl.h>

/*
 * The application master runs on a single ME and performs a number of
 * functions:
 *
 * - Handle configuration changes.  The PCIe MEs (NFD) notify a single
 *   ME (this ME) of any changes to the configuration BAR.  It is then
 *   up to this ME to disseminate these configuration changes to any
 *   application MEs which need to be informed.  On context in this
 *   handles this.
 *
 * - Periodically read and update the stats maintained by the NFP
 *   MACs. The MAC stats can wrap and need to be read periodically.
 *   Furthermore, some of the MAC stats need to be made available in
 *   the Control BAR of the NIC.  One context in the ME handles this.
 *
 * - Maintain per queue counters.  The PCIe MEs (NFD) maintain
 *   counters in some local (fast) memory.  One context in this ME is
 *   periodically updating the corresponding fields in the control
 *   BAR.
 *
 * - Link state change monitoring.  One context in this ME is
 *   monitoring the Link state of the Ethernet port and updates the
 *   Link status bit in the control BAR as well as generating a
 *   interrupt on changes (if configured).
 */


/*
 * General declarations
 */
#ifndef NFD_PCIE0_EMEM
#error "NFD_PCIE0_EMEM must be defined"
#endif

/* APP Master CTXs assignments */
#define APP_MASTER_CTX_CONFIG_CHANGES   0
#define APP_MASTER_CTX_MAC_STATS        1
#define APP_MASTER_CTX_PERQ_STATS       2
#define APP_MASTER_CTX_LINK_STATE       3
#define APP_MASTER_CTX_FREE0            4
#define APP_MASTER_CTX_FREE1            5
#define APP_MASTER_CTX_FREE2            6
#define APP_MASTER_CTX_FREE3            7


/* Address of the PF Control BAR */


/* Current value of NFP_NET_CFG_CTRL (shared between all contexts) */
__shared __gpr volatile uint32_t nic_control_word;


/*
 * Global declarations for configuration change management
 */

/* The list of all application MEs IDs */
#ifndef APP_MES_LIST
    #error "The list of application MEs IDd must be defined"
#else
    __shared __lmem uint32_t app_mes_ids[] = {APP_MES_LIST};
#endif

__visible SIGNAL nfd_cfg_sig_app_master0;
NFD_CFG_BASE_DECLARE(0);
NFD_FLR_DECLARE;

/* A global synchronization counter to check if all APP MEs has reconfigured */
__export __dram struct synch_cnt nic_cfg_synch;


/*
 * Global declarations for per Q stats updates
 */

/* Sleep cycles between Per-Q counters push */
#define PERQ_STATS_SLEEP            2000

/*
 * Global declarations for Link state change management
 */

/* TODO : REPLACE WITH HEADER FILE DIRECTIVE WHEN AVAILABLE */
__intrinsic extern int msix_pf_send(unsigned int pcie_nr,
                                    unsigned int bar_nr,
                                    unsigned int entry_nr,
                                    unsigned int mask_en);

/* Hard-code the port being monitored by the NIC link status monitoring
 * XXX This should be shared with the MAC stats, of course. */
#define LSC_ACTIVE_LINK_MAC        0
#define LSC_ACTIVE_LINK_ETH_PORT   0

/* Amount of time between each link status check */
#define LSC_POLL_PERIOD            100000

/* Magic address retrieved from nfp-reg --cpp_info
 * xpb:Nbi0IsldXpbMap.NbiTopXpbMap.MacGlbAdrMap.MacEthernet0.
 * MacEthSeg0.EthCmdConfig.EthRxEna */
#define MAC_CONF_ADDR               0x0008440008

/* Addresses of the MAC enqueue inhibit registers */
#define MAC_EQ_INH_ADDR                                     \
    (NFP_MAC_XPB_OFF(0) | NFP_MAC_CSR | NFP_MAC_CSR_EQ_INH)
#define MAC_EQ_INH_DONE_ADDR                                     \
    (NFP_MAC_XPB_OFF(0) | NFP_MAC_CSR | NFP_MAC_CSR_EQ_INH_DONE)

/* XXX - remove once nfp_mac.h has been incorporated into flow-env */
#define NFP_MAC_ETH_SEG_CMD_CONFIG_RX_ENABLE    (1 << 1)

/*
 * Config change management.
 *
 * - Periodically check for configuration changes. If changed:
 * - Set up the mechanism to notify (a shared bit mask)
 * - Ping all application MEs (using @ct_reflect_data())
 * - Wait for them to acknowledge the change
 * - Acknowledge config change to PCIe MEs.
 */

/* XXX Move to some sort of CT reflect library */
__intrinsic static void
ct_reflect_data(unsigned int dst_me, unsigned int dst_xfer,
                unsigned int sig_no, volatile __xwrite void *src_xfer,
                size_t size)
{
    #define OV_SIG_NUM 13

    unsigned int addr;
    unsigned int count = (size >> 2);
    struct nfp_mecsr_cmd_indirect_ref_0 indirect;

    ctassert(__is_ct_const(size));

    /* Where address[29:24] specifies the Island Id of remote ME
     * to write to, address[16] is the XferCsrRegSel select bit (0:
     * Transfer Registers, 1: CSR Registers), address[13:10] is
     * the master number (= FPC + 4) within the island to write
     * data to, address[9:2] is the first register address (Register
     * depends upon XferCsrRegSel) to write to. */
    addr = ((dst_me & 0x3F0)<<20 | ((dst_me & 0xF)<<10 | (dst_xfer & 0xFF)<<2));

    indirect.__raw = 0;
    indirect.signal_num = sig_no;
    local_csr_write(local_csr_cmd_indirect_ref_0, indirect.__raw);

    /* Reflect the value and signal the remote ME */
    __asm {
        alu[--, --, b, 1, <<OV_SIG_NUM];
        ct[reflect_write_sig_remote, *src_xfer, addr, 0, \
           __ct_const_val(count)], indirect_ref;
    };
}

static void
cfg_changes_loop(void)
{
    struct nfd_cfg_msg cfg_msg;
    __xread unsigned int cfg_bar_data[2];
    volatile __xwrite uint32_t cfg_pci_vnic;
    __gpr int i;
    uint32_t mac_conf;
    uint32_t mac_inhibit;
    uint32_t mac_inhibit_done;
    uint32_t mac_port_mask;

    /* Initialisation */
    nfd_cfg_init_cfg_msg(&nfd_cfg_sig_app_master0, &cfg_msg);

    for (;;) {
        nfd_cfg_master_chk_cfg_msg(&cfg_msg, &nfd_cfg_sig_app_master0, 0);

        if (cfg_msg.msg_valid) {

            /* read in the first 64bit of the Control BAR */
            mem_read64(cfg_bar_data, NFD_CFG_BAR_ISL(NIC_PCI, cfg_msg.vnic),
                       sizeof cfg_bar_data);

            /* Reflect the pci island and the vnic number to remote MEs */
            cfg_pci_vnic = (NIC_PCI << 16) | cfg_msg.vnic;
            /* Reset the configuration ack counter */
            synch_cnt_dram_reset(&nic_cfg_synch,
                                 sizeof(app_mes_ids)/sizeof(uint32_t));
            /* Signal all APP MEs about a config change */
            for(i = 0; i < sizeof(app_mes_ids)/sizeof(uint32_t); i++) {
                ct_reflect_data(app_mes_ids[i],
                                APP_ME_CONFIG_XFER_NUM,
                                APP_ME_CONFIG_SIGNAL_NUM,
                                &cfg_pci_vnic, sizeof cfg_pci_vnic);
            }

            /* Set RX appropriately if NFP_NET_CFG_CTRL_ENABLE changed */
            if ((nic_control_word ^ cfg_bar_data[0]) &
                    NFP_NET_CFG_CTRL_ENABLE) {

                mac_conf = xpb_read(MAC_CONF_ADDR);
                if (cfg_bar_data[0] & NFP_NET_CFG_CTRL_ENABLE) {
                    mac_conf |= NFP_MAC_ETH_SEG_CMD_CONFIG_RX_ENABLE;
                    xpb_write(MAC_CONF_ADDR, mac_conf);
                } else {
                    /* Inhibit packets from enqueueing in the MAC RX */
                    mac_port_mask = 0xf << MAC0_PORTS;

                    mac_inhibit = xpb_read(MAC_EQ_INH_ADDR);
                    mac_inhibit |= mac_port_mask;
                    xpb_write(MAC_EQ_INH_ADDR, mac_inhibit);

                    /* Polling to see that the inhibit took effect */
                    do {
                        mac_inhibit_done = xpb_read(MAC_EQ_INH_DONE_ADDR);
                        mac_inhibit_done &= mac_port_mask;
                    } while (mac_inhibit_done != mac_port_mask);

                    /* Disable the RX, and then disable the inhibit feature */
                    mac_conf &= ~NFP_MAC_ETH_SEG_CMD_CONFIG_RX_ENABLE;
                    xpb_write(MAC_CONF_ADDR, mac_conf);

                    mac_inhibit &= ~mac_port_mask;
                    xpb_write(MAC_EQ_INH_ADDR, mac_inhibit);
                }
            }

            /* Save the control word */
            nic_control_word = cfg_bar_data[0];

            /* Wait for all APP MEs to ack the config change */
            synch_cnt_dram_wait(&nic_cfg_synch);

            /* Complete the message */
            cfg_msg.msg_valid = 0;
            nfd_cfg_app_complete_cfg_msg(NIC_PCI, &cfg_msg,
                                         NFD_CFG_BASE_LINK(NIC_PCI),
                                         &nfd_cfg_sig_app_master0);
        }

        ctx_swap();
    }
    /* NOTREACHED */
}


/*
 * Handle per Q statistics
 *
 * - Periodically push TX and RX queue counters maintained by the PCIe
 *   MEs to the control BAR.
 */
static void
perq_stats_loop(void)
{
    SIGNAL rxq_sig;
    SIGNAL txq_sig;
    unsigned int rxq;
    unsigned int txq;

    /* Initialisation */
    nfd_in_recv_init();
    nfd_out_send_init();

    for (;;) {
        for (txq = 0;
             txq < (NFD_MAX_VFS * NFD_MAX_VF_QUEUES + NFD_MAX_PF_QUEUES);
             txq++) {
            __nfd_out_push_pkt_cnt(NIC_PCI, txq, ctx_swap, &txq_sig);
            sleep(PERQ_STATS_SLEEP);
        }

        for (rxq = 0;
             rxq < (NFD_MAX_VFS * NFD_MAX_VF_QUEUES + NFD_MAX_PF_QUEUES);
             rxq++) {
            __nfd_in_push_pkt_cnt(NIC_PCI, rxq, ctx_swap, &rxq_sig);
            sleep(PERQ_STATS_SLEEP);           
        }
    }
    /* NOTREACHED */
}

/*
 * Link state change handling
 *
 * - Periodically check the Link state (@lsc_check()) and update the
 *   status word in the control BAR.
 * - If the link state changed, try to send an interrupt (@lsc_send()).
 * - If the MSI-X entry has not yet been configured, ignore.
 * - If the interrupt is masked, set the pending flag and try again later.
 */

/* Send an LSC MSI-X. return 0 if done or 1 if pending */
__inline static int
lsc_send(void)
{
    __mem char *nic_ctrl_bar = NFD_CFG_BAR_ISL(NIC_PCI, NIC_INTF);
    unsigned int automask;
    __xread unsigned int tmp;
    __gpr unsigned int entry;
    __xread uint32_t mask_r;
    __xwrite uint32_t mask_w;

    int ret = 0;

    mem_read32_le(&tmp, nic_ctrl_bar + NFP_NET_CFG_LSC, sizeof(tmp));
    entry = tmp & 0xff;

    /* Check if the entry is configured. If not return (nothing pending) */
    if (entry == 0xff)
        goto out;

    /* Work out which masking mode we should use */
    automask = nic_control_word & NFP_NET_CFG_CTRL_MSIXAUTO;

    /* If we don't auto-mask, check the ICR */
    if (!automask) {
        mem_read32_le(&mask_r, nic_ctrl_bar + NFP_NET_CFG_ICR(entry),
                      sizeof(mask_r));
        if (mask_r & 0x000000ff) {
            ret = 1;
            goto out;
        }
        mask_w = NFP_NET_CFG_ICR_LSC;
        mem_write8_le(&mask_w, nic_ctrl_bar + NFP_NET_CFG_ICR(entry), 1);
    }

    ret = msix_pf_send(NIC_PCI + 4, PCIE_CPP2PCIE_LSC, entry, automask);

out:
    return ret;
}

/* Check the Link state and try to generate an interrupt if it changed.
 * Return 0 if everything is fine, or 1 if there is pending interrupt. */
__inline static int
lsc_check(__gpr unsigned int *ls_current)
{
    __mem char *nic_ctrl_bar = NFD_CFG_BAR_ISL(NIC_PCI, NIC_INTF);
    __gpr enum link_state ls;
    __gpr int changed = 0;
    __xwrite uint32_t sts = NFP_NET_CFG_STS_LINK;
    __gpr int ret = 0;

    /* XXX Only check link state once the device is up.  This is
     * temporary to avoid a system crash when the MAC gets reset after
     * FW has been loaded. */
    if (!(nic_control_word & NFP_NET_CFG_CTRL_ENABLE))
        goto out;

    /* Read the current link state and if it changed set the bit in
     * the control BAR status */
    ls = mac_eth_port_link_state(LSC_ACTIVE_LINK_MAC, LSC_ACTIVE_LINK_ETH_PORT);

    if (ls != *ls_current)
        changed = 1;
    *ls_current = ls;

    /* Make sure the status bit reflects the link state. Write this
     * every time to avoid a race with resetting the BAR state. */
    if (ls == LINK_DOWN)
        mem_bitclr(&sts, nic_ctrl_bar + NFP_NET_CFG_STS, sizeof(sts));
    else
        mem_bitset(&sts, nic_ctrl_bar + NFP_NET_CFG_STS, sizeof(sts));

    /* If the link state changed, try to send in interrupt */
    if (changed)
        ret = lsc_send();

out:
    return ret;
}

static void
lsc_loop(void)
{
    __gpr unsigned int ls_current = LINK_DOWN;
    __gpr unsigned int pending;
    __gpr int lsc_count = 0;

    pending = lsc_check(&ls_current);

    /* Need to handle pending interrupts more frequent than we need to
     * check for link state changes.  To keep it simple, have a single
     * timer for the pending handling and maintain a counter to
     * determine when to also check for linkstate. */
    for (;;) {
        sleep(LSC_POLL_PERIOD);
        lsc_count++;

       if (pending)
            pending = lsc_send();

        if (lsc_count > 19) {
            lsc_count = 0;
            pending = lsc_check(&ls_current);
        }
    }
    /* NOTREACHED */
}


int
main(void)
{
    switch (ctx()) {
    case APP_MASTER_CTX_CONFIG_CHANGES:
        cfg_changes_loop();
        break;
    case APP_MASTER_CTX_MAC_STATS:
        nic_stats_loop();
        break;
    case APP_MASTER_CTX_PERQ_STATS:
        perq_stats_loop();
        break;
    case APP_MASTER_CTX_LINK_STATE:
        lsc_loop();
        break;
    default:
        ctx_wait(kill);
    }
    /* NOTREACHED */
}