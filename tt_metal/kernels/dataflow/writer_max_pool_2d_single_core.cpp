// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include "dataflow_api.h"

// #include "debug_print.h"
// SliceRange srt = SliceRange{ .h0 = 0, .h1 = 32, .hs = 8, .w0 = 0, .w1 = 32, .ws = 8 };
// SliceRange srr = SliceRange{ .h0 = 0, .h1 = 1, .hs = 8, .w0 = 0, .w1 = 64, .ws = 2 };
// SliceRange srr2 = SliceRange{ .h0 = 0, .h1 = 1, .hs = 8, .w0 = 0, .w1 = 64, .ws = 2 };

/**
 * Max-pool 2D. Highly Unoptimized!!
 */
void kernel_main() {
    const uint32_t out_addr = get_arg_val<uint32_t>(1);
    const int32_t out_h = get_arg_val<int32_t>(10);
    const int32_t out_w = get_arg_val<int32_t>(11);
    const uint32_t out_nbytes_c = get_arg_val<uint32_t>(15);
    const uint32_t out_cb_pagesize = get_arg_val<uint32_t>(23);
    const uint32_t out_w_loop_count = get_arg_val<uint32_t>(25);
    const uint32_t nbatch = get_arg_val<uint32_t>(27);

    constexpr bool is_out_dram = get_compile_time_arg_val(1) == 1;
    constexpr uint32_t out_nelems = get_compile_time_arg_val(3);

    constexpr uint32_t out_cb_id = tt::CB::c_out0;

    // ROW_MAJOR output
    const InterleavedAddrGen<is_out_dram> s_out = {
        .bank_base_address = out_addr,
        .page_size = out_nbytes_c   // TODO: Ensure this is 32B aligned
    };

    uint32_t out_hw = out_h * out_w;

    uint32_t batch_offset = 0;
    for (uint32_t batch = 0; batch < nbatch; ++ batch) {
        uint32_t out_row_id = 0;
        // for every output pixel
        for (int32_t out_h_i = 0; out_h_i < out_h; ++ out_h_i) {
            for (uint32_t out_w_i = 0; out_w_i < out_w_loop_count; ++ out_w_i) {
                cb_wait_front(out_cb_id, out_nelems);
                // kernel_profiler::mark_time(13);
                uint32_t out_l1_read_addr = get_read_ptr(out_cb_id);
                for (uint32_t out_elem_i = 0; out_elem_i < out_nelems; ++ out_elem_i) {
                    // TODO [AS]: skip OOB indices when out_nelems is not multiple of out_w
                    uint64_t out_noc_addr = get_noc_addr(batch_offset + out_row_id, s_out);
                    noc_async_write(out_l1_read_addr, out_noc_addr, out_nbytes_c);
                    ++ out_row_id;
                    out_l1_read_addr += out_cb_pagesize;
                }
                noc_async_write_barrier();
                // kernel_profiler::mark_time(14);
                cb_pop_front(out_cb_id, out_nelems);
            }
        }
        batch_offset += out_hw;
    }
} // kernel_main()
