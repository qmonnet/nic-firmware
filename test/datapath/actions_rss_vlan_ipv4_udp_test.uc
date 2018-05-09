;TEST_INIT_EXEC nfp-reg mereg:i32.me0.XferIn_32=0x7fe3
;TEST_INIT_EXEC nfp-reg mereg:i32.me0.XferIn_33=0xff3f8c02

#include "pkt_vlan_ipv4_udp_x84.uc"

#include <actions.uc>
#include "actions_rss.uc"

.reg pkt_len
.reg tmp
pv_get_length(pkt_len, pkt_vec)

rss_reset_test(pkt_vec)
__actions_rss(pkt_vec)
rss_validate(pkt_vec, NFP_NET_RSS_IPV4_UDP, test_assert_equal, 0x8090a37d)

rss_validate_range(pkt_vec, NFP_NET_RSS_IPV4_UDP, excl, 0, (14 + 4 + 12))
rss_validate_range(pkt_vec, NFP_NET_RSS_IPV4_UDP, incl, (14 + 4 + 12), (14 + 4 + 12 + 8 + 4))
rss_validate_range(pkt_vec, NFP_NET_RSS_IPV4_UDP, excl, (14 + 4 + 12 + 8 + 4), pkt_len)

test_pass()
