#ifndef _ACTIONS_UC
#define _ACTIONS_UC

#include <kernel/nfp_net_ctrl.h>

#include "app_config_instr.h"
#include "protocols.h"

#include "pv.uc"
#include "pkt_io.uc"


#macro __actions_read(out_data, in_mask, in_shf)
    #if (streq('in_mask', '--'))
        #if (streq('in_shf', '--'))
            alu[out_data, --, B, *$index++]
        #else
            alu[out_data, --, B, *$index++, in_shf]
        #endif
    #else
        #if (streq('in_shf', '--'))
            alu[out_data, in_mask, AND, *$index++]
        #else
            alu[out_data, in_mask, AND, *$index++, in_shf]
        #endif
    #endif
    alu[act_t_idx, act_t_idx, +, 4]
#endm


#macro __actions_restore_t_idx()
    local_csr_wr[T_INDEX, act_t_idx]
    nop
    nop
    nop
#endm


#macro __actions_next()
    br_bclr[*$index, INSTR_PIPELINE_BIT, next#]
#endm


#macro __actions_statistics(in_pkt_vec)
.begin
    .reg stats_base
    .reg tmp
    .reg hi

    __actions_read(stats_base, 0xff, --)
    pv_stats_select(in_pkt_vec, stats_base)
    br[check_mac#]

broadcast#:
    pv_stats_select(in_pkt_vec, PV_STATS_BC)
    br[done#]

multicast#:
    alu[hi, --, B, *$index++]
    alu[tmp, --, B, 1, <<16]
    alu[--, *$index++, +, tmp]  // byte_align_be[] not necessary for MAC (word aligned)
    alu[--, hi, +carry, 0]
    __actions_restore_t_idx()
    bcs[broadcast#]

    pv_stats_select(in_pkt_vec, PV_STATS_MC)
    br[done#]

check_mac#:
    pv_seek(in_pkt_vec, 0, PV_SEEK_CTM_ONLY)

    br_bset[*$index, BF_L(MAC_MULTICAST_bf), multicast#]

    __actions_restore_t_idx()

done#:
.end
#endm


#macro __actions_check_mtu(in_pkt_vec, DROP_LABEL)
.begin
    .reg mask
    .reg mtu
    .reg pkt_len

    immed[mask, 0x3fff]
    __actions_read(mtu, mask, --)
    pv_check_mtu(in_pkt_vec, mtu, DROP_LABEL)
.end
#endm


#macro __actions_check_mac(in_pkt_vec, DROP_LABEL)
.begin
    .reg mac[2]
    .reg tmp

    __actions_read(mac[1], --, <<16)
    __actions_read(mac[0], --, --)

    pv_seek(in_pkt_vec, 0, PV_SEEK_CTM_ONLY)

    // permit multicast and broadcast addresses to pass
    br_bset[*$index, BF_L(MAC_MULTICAST_bf), pass#]

    alu[--, mac[0], XOR, *$index++] // byte_align_be[] not necessary for MAC (word aligned)
    bne[DROP_LABEL]

    alu[tmp, mac[1], XOR, *$index++]
    alu[--, --, B, tmp, >>16]
    bne[DROP_LABEL]

pass#:
    __actions_restore_t_idx()
.end
#endm


#macro __actions_rss(in_pkt_vec)
.begin
    .reg data
    .reg hash
    .reg hash_type
    .reg ipv4_delta
    .reg ipv6_delta
    .reg idx
    .reg key
    .reg l3_info
    .reg l3_offset
    .reg l4_offset
    .reg opcode
    .reg parse_status
    .reg pkt_type // 0 - IPV6_TCP, 1 - IPV6_UDP, 2 - IPV4_TCP, 3 - IPV4_UDP
    .reg queue
    .reg queue_shf
    .reg queue_off
    .reg udp_delta
    .reg write $metadata

    __actions_read(opcode, --, --)
    __actions_read(key, --, --)

    // skip RSS for unkown L3
    bitfield_extract__sz1(l3_info, BF_AML(in_pkt_vec, PV_PARSE_L3I_bf))
    beq[skip_rss#]

    // skip RSS for MPLS
    bitfield_extract__sz1(--, BF_AML(in_pkt_vec, PV_PARSE_MPD_bf))
    bne[skip_rss#]

    // skip RSS for 2 or more VLANs (Catamaran L4 offsets unreliable)
    br_bset[BF_A(in_pkt_vec, PV_PARSE_VLD_bf), BF_M(PV_PARSE_VLD_bf), skip_rss#]

    local_csr_wr[CRC_REMAINDER, key]
    immed[hash_type, 1]

    // seek to L3 source address
    alu[l3_offset, (1 << 2), AND, BF_A(in_pkt_vec, PV_PARSE_VLD_bf), >>(BF_L(PV_PARSE_VLD_bf) - 2)] // 4 bytes for VLAN
    alu[l3_offset, l3_offset, +, (14 + 8)] // 14 bytes for Ethernet, 8 bytes of IP header
    alu[ipv4_delta, (1 << 2), AND, BF_A(in_pkt_vec, PV_PARSE_L3I_bf), >>(BF_M(PV_PARSE_L3I_bf) - 2)] // 4 bytes extra for IPv4
    alu[l3_offset, l3_offset, +, ipv4_delta]
    pv_seek(in_pkt_vec, l3_offset, PV_SEEK_CTM_ONLY)

    byte_align_be[--, *$index++]
    byte_align_be[data, *$index++]
    br_bset[BF_A(in_pkt_vec, PV_PARSE_L3I_bf), BF_M(PV_PARSE_L3I_bf), process_l4#], defer[3] // branch if IPv4, 2 words hashed
        crc_be[crc_32, --, data]
        byte_align_be[data, *$index++]
        crc_be[crc_32, --, data]

    // hash 6 more words for IPv6
    #define_eval LOOP (0)
    #while (LOOP < 6)
        byte_align_be[data, *$index++]
        crc_be[crc_32, --, data]
        #define_eval LOOP (LOOP + 1)
    #endloop
    #undef LOOP

    alu[hash_type, hash_type, +, 1]

process_l4#:
    // skip L4 if offset unknown
    alu[l4_offset, 0xfe, AND, BF_A(in_pkt_vec, PV_PARSE_L4_OFFSET_bf), >>(BF_L(PV_PARSE_L4_OFFSET_bf) - 1)]
    beq[skip_l4#] // zero for unsupported protocols or unparsable IP extention headers

    // skip L4 if unknown
    bitfield_extract__sz1(parse_status, BF_AML(in_pkt_vec, PV_PARSE_STS_bf))
    beq[skip_l4#]

    // skip L4 if packet is fragmented
    br=byte[parse_status, 0, 7, skip_l4#]

    // skip L4 if not configured
    alu[pkt_type, l3_info, AND, 2]
    alu[pkt_type, pkt_type, OR, parse_status, >>2]
    alu[--, pkt_type, OR, 0]
    alu[--, opcode, AND, 1, <<indirect]
    beq[skip_l4#]

    pv_seek(in_pkt_vec, l4_offset, 4, PV_SEEK_CTM_ONLY)
    byte_align_be[--, *$index++]
    byte_align_be[data, *$index++]
    crc_be[crc_32, --, data]

        alu[hash_type, hash_type, +, 3]
        alu[udp_delta, (1 << 1), AND, parse_status, >>1]
        alu[udp_delta, udp_delta, OR, parse_status, >>2]
        alu[hash_type, hash_type,  +, udp_delta]

skip_l4#:
    pv_meta_push_type(in_pkt_vec, hash_type)
    pv_meta_push_type(in_pkt_vec, 1)

    local_csr_rd[CRC_REMAINDER]
    immed[hash, 0]

    alu[$metadata, --, B, hash]
    pv_meta_prepend(in_pkt_vec, $metadata, 4]

    alu[queue_off, 0x1f, AND, hash, >>2]
    alu[idx, 0x7f, AND, opcode, >>8]
    alu[idx, idx, +, queue_off]
    local_csr_wr[NN_GET, idx]
        __actions_restore_t_idx()
        alu[queue_shf, 0x18, AND, hash, <<3]
        alu[--, queue_shf, OR, 0]
    alu[queue, 0xff, AND, *n$index, >>indirect]
    pv_set_egress_queue(in_pkt_vec, queue)

skip_rss#:
.end
#endm


#macro __actions_checksum_complete(in_pkt_vec)
.begin
    .reg carries
    .reg checksum
    .reg data_len
    .reg iteration_bytes
    .reg iteration_words
    .reg jump_idx
    .reg last_bits
    .reg mask
    .reg offset
    .reg padded
    .reg pkt_len
    .reg remaining_words
    .reg shift
    .reg write $metadata
    .reg t_idx

    .sig sig_read

    __actions_read(--, --, --)

    // invalidate packet cache
    alu[BF_A(in_pkt_vec, PV_SEEK_BASE_bf), BF_A(in_pkt_vec, PV_SEEK_BASE_bf), OR, 0xff, <<BF_L(PV_SEEK_BASE_bf)]

    alu[offset, (3 << 2), AND, BF_A(in_pkt_vec, PV_PARSE_VLD_bf), >>(BF_L(PV_PARSE_VLD_bf) - 2)] // 4 bytes per VLAN
    br=byte[offset, 0, (3 << 2), too_many_vlans#]
    alu[offset, offset, +, 14]

    pv_get_length(pkt_len, in_pkt_vec)
    alu[data_len, pkt_len, -, offset]
    alu[remaining_words, --, B, data_len, >>2]

    immed[checksum, 0]
    immed[carries, 0]

loop#:
    ov_start(OV_LENGTH)
    ov_set_use(OV_LENGTH, 32, OVF_SUBTRACT_ONE)
    ov_clean()
    mem[read8, $__pv_pkt_data[0], BF_A(in_pkt_vec, PV_CTM_ADDR_bf), offset, max_32], indirect_ref, ctx_swap[sig_read]

    local_csr_wr[T_INDEX, t_idx_ctx]

    alu[jump_idx, 8, -, remaining_words]
    ble[max#]

    jump[jump_idx, w0#], targets[w0#,  w1#,  w2#,  w3#,  w4#,  w5#,  w6#, w7#], defer[3]
       alu[iteration_words, --, B, remaining_words]
       alu[iteration_bytes, --, B, remaining_words, <<2]
       alu[offset, offset, +, iteration_bytes]

too_many_vlans#:
    // TODO: parse through VLANS to find payload offset, resort to checksum unnecessary for now
    pv_propagate_mac_csum_status(in_pkt_vec)
    br[end#]

max#:
    alu[offset, offset, +, 32]
    immed[iteration_words, 8]

#define_eval LOOP_UNROLL (0)
#while (LOOP_UNROLL < 8)
w/**/LOOP_UNROLL#:
    alu[checksum, checksum, +carry, *$index++]
    #define_eval LOOP_UNROLL (LOOP_UNROLL + 1)
#endloop
#undef LOOP_UNROLL

    alu[carries, carries, +carry, 0] // accumulate carries that would be lost to looping construct alu[]s

    alu[remaining_words, remaining_words, -, iteration_words]
    bgt[loop#]

    alu[last_bits, (3 << 3), AND, data_len, <<3]
    beq[finalize#]

    mem[read8, $__pv_pkt_data[0], BF_A(in_pkt_vec, PV_CTM_ADDR_bf), offset, 4], ctx_swap[sig_read]

    local_csr_wr[T_INDEX, t_idx_ctx]
    alu[shift, 32, -, last_bits]
    alu[mask, shift, ~B, 0]
    alu[mask, --, B, mask, <<indirect]
    alu[padded, mask, AND, *$index++]
    alu[checksum, checksum, +, padded]
    alu[carries, carries, +carry, 0]

finalize#:
    alu[checksum, checksum, +, carries]
    alu[checksum, checksum, +carry, 0]
    alu[$metadata, checksum, +carry, 0] // adding carries might cause another carry

    pv_meta_push_type(in_pkt_vec, 6)
    pv_meta_prepend(in_pkt_vec, $metadata, 4)

    __actions_restore_t_idx()
end#:
.end
#endm


#macro actions_execute(in_pkt_vec, EGRESS_LABEL, DROP_LABEL, ERROR_LABEL)
.begin
    .reg act_t_idx
    .reg addr
    .reg egress_q_base
    .reg egress_q_mask
    .reg jump_idx

    .reg volatile read $actions[NIC_MAX_INSTR]
    .xfer_order $actions
    .sig sig_actions

    pv_get_instr_addr(addr, in_pkt_vec, (NIC_MAX_INSTR * 4))
    ov_start(OV_LENGTH)
    ov_set_use(OV_LENGTH, 16, OVF_SUBTRACT_ONE)
    ov_clean()
    cls[read, $actions[0], 0, addr, max_16], indirect_ref, defer[2], ctx_swap[sig_actions]
        alu[act_t_idx, t_idx_ctx, OR, &$actions[0], <<2]
        immed[egress_q_mask, BF_MASK(PV_QUEUE_OUT_bf)]

    local_csr_wr[T_INDEX, act_t_idx]
    nop
    nop
    nop

next#:
    alu[jump_idx, --, B, *$index, >>INSTR_OPCODE_LSB]
    jump[jump_idx, ins_0#], targets[ins_0#, ins_1#, ins_2#, ins_3#, ins_4#, ins_5#, ins_6#, ins_7#] ;actions_jump

        ins_0#: br[DROP_LABEL]
        ins_1#: br[statistics#]
        ins_2#: br[mtu#]
        ins_3#: br[mac#]
        ins_4#: br[rss#]
        ins_5#: br[checksum_complete#]
        ins_6#: br[tx_host#]
        ins_7#: br[tx_wire#]

statistics#:
    __actions_statistics(in_pkt_vec)
    __actions_next()

mtu#:
    __actions_check_mtu(in_pkt_vec, DROP_LABEL)
    __actions_next()

mac#:
    __actions_check_mac(in_pkt_vec, DROP_LABEL)
    __actions_next()

rss#:
    __actions_rss(in_pkt_vec)
    __actions_next()

checksum_complete#:
    __actions_checksum_complete(in_pkt_vec)
    __actions_next()

tx_host#:
    __actions_read(egress_q_base, egress_q_mask, --)
    pkt_io_tx_host(in_pkt_vec, egress_q_base, EGRESS_LABEL, DROP_LABEL)

tx_wire#:
    __actions_read(egress_q_base, egress_q_mask, --)
    pkt_io_tx_wire(in_pkt_vec, egress_q_base, EGRESS_LABEL, DROP_LABEL)

.end
#endm

#endif

