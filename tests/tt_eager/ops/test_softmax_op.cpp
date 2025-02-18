// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include <tt-metalium/host_api.hpp>
#include "ttnn/tensor/tensor.hpp"
#include "ttnn/operations/normalization/softmax/softmax.hpp"
#include "ttnn/operations/functions.hpp"

#include <algorithm>
#include <functional>
#include <random>

using namespace tt;
using namespace tt::tt_metal;
using namespace constants;

void run_softmax(IDevice* device, const ttnn::Shape& shape) {
    Tensor input_tensor = ttnn::random::random(shape).to_layout(Layout::TILE).to_device(device);
    Tensor device_output_tensor = ttnn::softmax_in_place(input_tensor);
    Tensor output_tensor = device_output_tensor.cpu();
}

//////////////////////////////////////////////////////////////////////////////////////////
// TODO: explain what test does
//////////////////////////////////////////////////////////////////////////////////////////
int main(int argc, char** argv) {
    bool pass = true;
    ////////////////////////////////////////////////////////////////////////////
    //                      Device Setup
    ////////////////////////////////////////////////////////////////////////////
    int device_id = 0;
    tt_metal::IDevice* device = tt_metal::CreateDevice(device_id);

    run_softmax(device, Shape({1, 1, TILE_HEIGHT, TILE_WIDTH}));
    run_softmax(device, Shape({1, 1, TILE_HEIGHT * 2, TILE_WIDTH * 2}));
    pass &= CloseDevice(device);

    if (pass) {
        log_info(LogTest, "Test Passed");
    } else {
        TT_THROW("Test Failed");
    }

    TT_FATAL(pass, "Error");

    return 0;
}
