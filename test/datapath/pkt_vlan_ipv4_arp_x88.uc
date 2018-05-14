;TEST_INIT_EXEC nfp-mem i32.ctm:0x80  0x00000000 0x00000000 0xffffffff 0xffff86c1
;TEST_INIT_EXEC nfp-mem i32.ctm:0x90  0x5ccdfc7e 0x81000190 0x08060001 0x08000604
;TEST_INIT_EXEC nfp-mem i32.ctm:0xa0  0x000186c1 0x5ccdfc7e 0x2801042c 0x00000000
;TEST_INIT_EXEC nfp-mem i32.ctm:0xb0  0x00002801 0x00000404

#include <aggregate.uc>
#include <stdmac.uc>

#include <pv.uc>

.reg pkt_vec[PV_SIZE_LW]
aggregate_zero(pkt_vec, PV_SIZE_LW)
move(pkt_vec[0], 0x3c)
move(pkt_vec[2], 0x88)
move(pkt_vec[3], 0x6)
move(pkt_vec[4], 0x3fcc)
move(pkt_vec[6], 1<<BF_L(PV_QUEUE_IN_TYPE_bf))
move(pkt_vec[9], 0x190)
move(pkt_vec[10], (1 << BF_L(PV_VLD_bf)))
