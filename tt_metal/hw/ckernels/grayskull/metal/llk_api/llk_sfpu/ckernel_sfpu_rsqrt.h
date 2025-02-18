// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "ckernel.h"
#include "ckernel_defs.h"
#include "noc_nonblocking_api.h"
#include "ckernel_sfpu_recip.h"
#include <limits>

using namespace sfpi;

namespace ckernel {
namespace sfpu {

template <bool APPROXIMATION_MODE, int ITERATIONS, int RECIPROCAL_ITERATIONS>
inline void calculate_rsqrt() {
    for (int d = 0; d < ITERATIONS; d++) {
        vFloat in = dst_reg[0];
        vFloat result = sfpu_reciprocal<false>(in);
        v_if(dst_reg[0] < 1.0f) { result = 1.0f; }
        v_endif;

        for (int r = 0; r < RECIPROCAL_ITERATIONS; r++) {
            // y = y * (1.5 - 0.5 * x * y * y) Newton's method iteration.
            result = result * (1.5F - 0.5F * dst_reg[0] * result * result);
        }

        v_if(dst_reg[0] == 0.0f) { dst_reg[0] = std::numeric_limits<float>::infinity(); }
        v_else { dst_reg[0] = result; }
        v_endif;

        dst_reg++;
    }
}

}  // namespace sfpu
}  // namespace ckernel
