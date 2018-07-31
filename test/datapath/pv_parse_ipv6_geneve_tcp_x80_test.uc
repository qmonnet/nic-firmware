#include <single_ctx_test.uc>
#include <global.uc>
#include <actions.uc>
#include <bitfields.uc>

#include "pkt_ipv6_geneve_tcp_x80.uc"

.reg o_l4_offset
.reg o_l3_offset
.reg i_l4_offset
.reg i_l3_offset
.reg proto
.reg expected_o_l3_offset
.reg expected_o_l4_offset
.reg expected_i_l3_offset
.reg expected_i_l4_offset
.reg expected_proto
.reg pkt_len


pv_get_length(pkt_len, pkt_vec)
move(port_tun_args, 0x80f)

bitfield_extract__sz1(expected_i_l4_offset, BF_AML(pkt_vec, PV_HEADER_OFFSET_INNER_L4_bf))
bitfield_extract__sz1(expected_i_l3_offset, BF_AML(pkt_vec, PV_HEADER_OFFSET_INNER_IP_bf))
bitfield_extract__sz1(expected_o_l4_offset, BF_AML(pkt_vec, PV_HEADER_OFFSET_OUTER_L4_bf))
bitfield_extract__sz1(expected_o_l3_offset, BF_AML(pkt_vec, PV_HEADER_OFFSET_OUTER_IP_bf))
bitfield_extract__sz1(expected_proto, BF_AML(pkt_vec, PV_PROTO_bf))

move(BF_A(pkt_vec, PV_HEADER_STACK_bf), 0)
move(BF_A(pkt_vec, PV_PROTO_bf), 0)

pv_seek(pkt_vec, 0, (PV_SEEK_INIT | PV_SEEK_CTM_ONLY))
alu[--, --, B, *$index++]
alu[--, --, B, *$index++]
alu[--, --, B, *$index++]

pv_hdr_parse(pkt_vec, port_tun_args, check_result#)

check_result#:
bitfield_extract__sz1(proto, BF_AML(pkt_vec, PV_PROTO_bf))

bitfield_extract__sz1(i_l4_offset, BF_AML(pkt_vec, PV_HEADER_OFFSET_INNER_L4_bf))
bitfield_extract__sz1(i_l3_offset, BF_AML(pkt_vec, PV_HEADER_OFFSET_INNER_IP_bf))
bitfield_extract__sz1(o_l4_offset, BF_AML(pkt_vec, PV_HEADER_OFFSET_OUTER_L4_bf))
bitfield_extract__sz1(o_l3_offset, BF_AML(pkt_vec, PV_HEADER_OFFSET_OUTER_IP_bf))
bitfield_extract__sz1(proto, BF_AML(pkt_vec, PV_PROTO_bf))

test_assert_equal(i_l3_offset, expected_i_l3_offset)
test_assert_equal(i_l4_offset, expected_i_l4_offset)
test_assert_equal(o_l3_offset, expected_o_l3_offset)
test_assert_equal(o_l4_offset, expected_o_l4_offset)
test_assert_equal(proto, expected_proto)

test_pass()

#pragma warning(push)
#pragma warning(disable: 4701)
#pragma warning(disable: 5116)
PV_HDR_PARSE_SUBROUTINE#:
pv_hdr_parse_subroutine(pkt_vec, port_tun_args)
#pragma warning(pop)
