/*
 * Copyright 2016 Netronome, Inc.
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
 * @file          apps/nic/nic_tables.c
 * @brief         allocate tables for Core NIC
 *
 */
#include <nfp.h>
#include <stdint.h>
#include <nfp/mem_bulk.h>
#include <vnic/nfd_common.h>

#include "nic_tables.h"

__intrinsic int
load_vlan_members(uint16_t vlan_id, __xread uint64_t *members)
{
    int ret = 0;

    if (vlan_id <= NIC_MAX_VLAN_ID)
        mem_read64(members, &nic_vlan_to_vnics_map_tbl[vlan_id],
                   sizeof(uint64_t));
    else
        ret = -1;

    return ret;
}


__intrinsic int
add_vlan_member(uint16_t vlan_id, uint16_t vid)
{
    __emem __addr40 uint8_t *bar_base = NFD_CFG_BAR_ISL(NIC_PCI, vid);

    __xread uint64_t members_r;
    __xwrite uint64_t members_w;
    __xread uint32_t rxb_r;
    uint64_t new_member;
    uint64_t min_rxb;

    if (vlan_id > NIC_MAX_VLAN_ID)
	return -1;

    mem_read64(&members_r, &nic_vlan_to_vnics_map_tbl[vlan_id], sizeof(uint64_t));
    min_rxb = (members_r >> 58);
    mem_read32(&rxb_r, (__mem void*) (bar_base + NFP_NET_CFG_FLBUFSZ), sizeof(rxb_r));
    min_rxb = (min_rxb != 0 && min_rxb < ((rxb_r >> 8) & 0x3f)) ? min_rxb : ((rxb_r >> 8) & 0x3f);
    members_w = (members_r | (1ull << vid)) & ((1ull << NFD_MAX_VFS) - 1) | (min_rxb << 58);
    mem_write64(&members_w, &nic_vlan_to_vnics_map_tbl[vlan_id], sizeof(uint64_t));

    return 0;
}

__intrinsic int
remove_vlan_member(uint16_t vid)
{
    __emem __addr40 uint8_t *bar_base = NFD_CFG_BAR_ISL(NIC_PCI, vid);

    __xread uint64_t members_r;
    __xwrite uint64_t members_w;
    __xread uint32_t rxb_r;
    uint64_t members;
    uint64_t min_rxb;
    uint16_t vid_idx;
    uint32_t vlan;

    for (vlan = 0; vlan <= NIC_MAX_VLAN_ID; ++vlan) {
        mem_read64(&members_r, &nic_vlan_to_vnics_map_tbl[vlan], sizeof(uint64_t));
        members = members_r & ((1ull << NFD_MAX_VFS) - 1);
        members &= ~(1ul << vid);
        if (members) {
            min_rxb = (1 << 6) - 1;
            for (vid_idx = 0; NFD_MAX_VFS && vid_idx < NFD_MAX_VFS; vid_idx++) {
	        if (members & (1ull << vid_idx)) {
                    mem_read32(&rxb_r,
		               (__mem void*) (NFD_CFG_BAR_ISL(NIC_PCI, vid_idx) +
                               NFP_NET_CFG_FLBUFSZ), sizeof(rxb_r));
                    min_rxb = (min_rxb < (rxb_r >> 8) & 0x3f) ? min_rxb : (rxb_r >> 8) & 0x3f;
                }
            }
	    members |= (min_rxb << 56);
        }
        members_w = members;
        mem_write64(&members_w, &nic_vlan_to_vnics_map_tbl[vlan], sizeof(uint64_t));
    }

    return 0;
}

