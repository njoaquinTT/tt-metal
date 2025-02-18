# SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.

# SPDX-License-Identifier: Apache-2.0

import torch
from diffusers import StableDiffusionPipeline
import pytest

from tests.ttnn.utils_for_testing import assert_with_pcc
from models.utility_functions import (
    skip_for_grayskull,
)

import ttnn
from ttnn.model_preprocessing import preprocess_model_parameters
from models.demos.wormhole.stable_diffusion.tt.ttnn_functional_resnetblock2d_new_conv import resnetBlock2D
from models.demos.wormhole.stable_diffusion.custom_preprocessing import custom_preprocessor
from models.demos.wormhole.stable_diffusion.tt.ttnn_functional_utility_functions import conv_cache


def ttnn_to_torch(input):
    input = ttnn.to_layout(input, ttnn.ROW_MAJOR_LAYOUT)
    input = ttnn.from_device(input)
    input = ttnn.to_torch(input)
    return input


@skip_for_grayskull()
@pytest.mark.parametrize("device_params", [{"l1_small_size": 32768}], indirect=True)
@pytest.mark.parametrize(
    "batch_size, in_channels, input_height, input_width, index1, index2, block_name, out_channels",
    [
        # down block 0
        (2, 320, 64, 64, 0, 0, "down", 320),
        (2, 320, 64, 64, 0, 1, "down", 320),
        # down block 1
        (2, 320, 32, 32, 1, 0, "down", 640),
        (2, 640, 32, 32, 1, 1, "down", 640),
        # down block 2
        (2, 640, 16, 16, 2, 0, "down", 1280),
        (2, 1280, 16, 16, 2, 1, "down", 1280),
        # down block 3
        (2, 1280, 8, 8, 3, 0, "down", 1280),
        (2, 1280, 8, 8, 3, 1, "down", 1280),
        # mid
        (2, 1280, 8, 8, 0, 0, "mid", 1280),
        (2, 1280, 8, 8, 0, 1, "mid", 1280),
        # up block 0
        (2, 2560, 8, 8, 0, 0, "up", 1280),
        (2, 2560, 8, 8, 0, 1, "up", 1280),
        (2, 2560, 8, 8, 0, 2, "up", 1280),
        # up block 1
        (2, 2560, 16, 16, 1, 0, "up", 1280),
        (2, 2560, 16, 16, 1, 1, "up", 1280),
        (2, 1920, 16, 16, 1, 2, "up", 1280),
        # up block 2
        (2, 1920, 32, 32, 2, 0, "up", 640),
        (2, 1280, 32, 32, 2, 1, "up", 640),
        (2, 960, 32, 32, 2, 2, "up", 640),
        # up block 3
        (2, 960, 64, 64, 3, 0, "up", 320),
        (2, 640, 64, 64, 3, 1, "up", 320),
        (2, 640, 64, 64, 3, 2, "up", 320),
    ],
)
def test_resnet_block_2d_512x512(
    device, batch_size, in_channels, input_height, input_width, index1, index2, block_name, out_channels
):
    load_from_disk = False
    if not load_from_disk:
        # setup pytorch model
        pipe = StableDiffusionPipeline.from_pretrained("CompVis/stable-diffusion-v1-4", torch_dtype=torch.float32)

        model = pipe.unet
        model.eval()
        config = model.config

        parameters = preprocess_model_parameters(
            initialize_model=lambda: model, custom_preprocessor=custom_preprocessor, device=device
        )

        if block_name == "up":
            parameters = parameters.up_blocks[index1].resnets[index2]
            resnet = pipe.unet.up_blocks[index1].resnets[index2]
        elif block_name == "down":
            parameters = parameters.down_blocks[index1].resnets[index2]
            resnet = pipe.unet.down_blocks[index1].resnets[index2]
        else:
            parameters = parameters.mid_block.resnets[index2]
            resnet = pipe.unet.mid_block.resnets[index2]
        torch.save(resnet, "resnet.pt")
        torch.save(config, "config.pt")

    else:
        resnet = torch.load("resnet.pt")
        config = torch.load("config.pt")
        parameters = preprocess_model_parameters(
            initialize_model=lambda: resnet, custom_preprocessor=custom_preprocessor, device=device
        )

    ttnn.dump_device_memory_state(device, prefix="GN_resnet_1_")

    ############ start of residual block #############
    temb_channels = 1280
    groups = 32
    time_embedding_norm = "default"
    output_scale_factor = 1
    use_in_shortcut = None
    ########## end of residual block #############
    hidden_states_shape = [batch_size, in_channels, input_height, input_width]
    temb_shape = [1, 1, 2, 1280]

    input = torch.randn(hidden_states_shape)
    temb = torch.randn(temb_shape)

    torch_output = resnet(input, temb.squeeze(0).squeeze(0))
    reader_patterns_cache = {}
    conv_cache.clear()
    compute_kernel_config = ttnn.WormholeComputeKernelConfig(
        math_fidelity=ttnn.MathFidelity.LoFi,
        math_approx_mode=True,
        fp32_dest_acc_en=True,
        packer_l1_acc=False,
    )
    resnet_block = resnetBlock2D(
        device,
        parameters,
        reader_patterns_cache,
        batch_size,
        input_height,
        input_width,
        group_norm_on_device=True,
        compute_kernel_config=compute_kernel_config,
    )

    input = torch.permute(input, (0, 2, 3, 1))
    input = ttnn.from_torch(input, ttnn.bfloat16)
    input = ttnn.reshape(input, (1, 1, batch_size * input_height * input_width, in_channels))

    input = ttnn.to_device(input, device, memory_config=ttnn.DRAM_MEMORY_CONFIG)
    input = ttnn.to_layout(input, ttnn.TILE_LAYOUT, dtype=ttnn.bfloat8_b)

    temb = temb.permute(2, 0, 1, 3)  # pre-permute temb
    temb = ttnn.from_torch(temb, ttnn.bfloat16)
    temb = ttnn.to_layout(temb, ttnn.TILE_LAYOUT)
    temb = ttnn.to_device(temb, device, memory_config=ttnn.DRAM_MEMORY_CONFIG)

    ttnn_output_ = resnet_block(
        input,
        temb=temb,
        in_channels=in_channels,
        out_channels=out_channels,
        temb_channels=temb_channels,
        use_in_shortcut=use_in_shortcut,
        eps=1e-6,
        groups=groups,
        time_embedding_norm=time_embedding_norm,
        non_linearity="silu",
        output_scale_factor=output_scale_factor,
    )
    ttnn_output = resnet_block(
        input,
        temb=temb,
        in_channels=in_channels,
        out_channels=out_channels,
        temb_channels=temb_channels,
        use_in_shortcut=use_in_shortcut,
        eps=1e-6,
        groups=groups,
        time_embedding_norm=time_embedding_norm,
        non_linearity="silu",
        output_scale_factor=output_scale_factor,
    )
    ttnn_output = ttnn.to_memory_config(ttnn_output, ttnn.DRAM_MEMORY_CONFIG)
    ttnn_output = ttnn_to_torch(ttnn_output)
    ttnn_output = torch.reshape(
        ttnn_output,
        (
            batch_size,
            input_height,
            input_width,
            out_channels if out_channels is not None else in_channels,
        ),
    )
    ttnn_output = torch.permute(ttnn_output, (0, 3, 1, 2))

    assert_with_pcc(torch_output, ttnn_output, pcc=0.98)
