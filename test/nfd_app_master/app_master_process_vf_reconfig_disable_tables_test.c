//TEST_REQ_BLM
//TEST_REQ_RESET

/*
    Tests the process_pf_reconfig funtion in app_master
*/

#include "defines.h"
#include "test.c"
#include "app_master_test.h"
#include "vnic_setup.c"
#include "app_private.c"
#include "app_config_tables.c"
#include "nic_tables.c"
#include "map_cmsg_rx.c"
#include "app_control_lib.c"
#include "action_parse.c"
#include "nfd_cfg_base_decl.c"

void test(uint32_t pcie) {
    uint32_t type, vnic, vid, pf, control, update;
    int vf;
    struct nfd_cfg_msg cfg_msg;

    //First indicate PF's are enabled
    for (pf = 0; pf < NFD_MAX_PFS; pf++) {
        set_nic_control_word(NIC_PCI, NFD_PF2VID(pf),
                get_nic_control_word(NIC_PCI,
                    NFD_PF2VID(pf)) | NFP_NET_CFG_CTRL_ENABLE);
        setup_pf_mac(NIC_PCI, NFD_PF2VID(pf), TEST_MAC);
    }

    for (vf = 0; vf < NFD_MAX_VFS; vf++) {

        vid = NFD_VF2VID(vf);
        NFD_VID2VNIC(type, vnic, vid);

        reset_cfg_msg(&cfg_msg, vid, 0);

        setup_sriov_cfg_data(NIC_PCI, vf, 0, 0,
                NFD_VF_CFG_CTRL_LINK_STATE_ENABLE | (0 << NFD_VF_CFG_CTRL_TRUSTED_shf));

        control = NFD_CFG_VF_CAP & ~NFP_NET_CFG_CTRL_ENABLE;
        update = NFD_CFG_VF_LEGAL_UPD;
        if (process_vf_reconfig(NIC_PCI, control, update, vid, &cfg_msg)) {
            test_fail();
        }

    }

    for (vf = 0; vf < NFD_MAX_VFS; vf++) {
        ctassert(NFD_MAX_VF_QUEUES == 1);
        verify_host_action_list(NIC_PCI, NFD_VID2QID(NFD_VF2VID(vf), 0));
    }

    test_pass();
}

void main(void)
{
    switch (ctx()) {
        case 0:
            test(0);
            break;
        default:
            map_cmsg_rx();
            break;
    }
}
