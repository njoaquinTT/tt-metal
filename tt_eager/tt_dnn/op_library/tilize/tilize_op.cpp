// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include <math.h>

#include "tt_dnn/op_library/tilize/tilize_op.hpp"
#include "tt_dnn/op_library/clone/clone_op.hpp"
#include "tt_dnn/op_library/math.hpp"

#include "tt_metal/host_api.hpp"
#include "tt_metal/common/constants.hpp"
#include "tt_metal/detail/util.hpp"
#include "tt_metal/common/math.hpp"

#include "tt_stl/reflection.hpp"

using namespace tt::constants;

namespace tt {

namespace tt_metal {

void Tilize::validate(const std::vector<Tensor> &input_tensors) const {
    const auto& input_tensor_a = input_tensors.at(0);
    TT_ASSERT(input_tensor_a.storage_type() == StorageType::DEVICE, "Operands to tilize need to be on device!");
    TT_ASSERT(input_tensor_a.buffer() != nullptr , "Operands to tilize need to be allocated in buffers on device!");
    TT_ASSERT(input_tensor_a.layout() == Layout::ROW_MAJOR, "Can only tilize row major data");

    TT_ASSERT(input_tensor_a.volume() % TILE_HW == 0);

    uint32_t stick_s =  input_tensor_a.layout() == Layout::ROW_MAJOR ? input_tensor_a.shape()[3] : input_tensor_a.shape()[1];
    uint32_t num_sticks = input_tensor_a.layout() == Layout::ROW_MAJOR ? input_tensor_a.volume() / input_tensor_a.shape()[3] : input_tensor_a.volume() / input_tensor_a.shape()[1];
    TT_ASSERT(input_tensor_a.dtype() == DataType::BFLOAT16);

    uint32_t stick_size = stick_s * input_tensor_a.element_size(); // Assuming bfloat16 dataformat

    TT_ASSERT((stick_size % 2) == 0, "Stick size must be divisible by 2");

    if (input_tensor_a.memory_config().is_sharded()) {
        TT_ASSERT(input_tensor_a.memory_config().memory_layout == TensorMemoryLayout::HEIGHT_SHARDED);
        TT_ASSERT(this->output_mem_config == input_tensor_a.memory_config());
        TT_ASSERT(this->use_multicore == true);
    } else {
        TT_ASSERT(input_tensor_a.memory_config().memory_layout == TensorMemoryLayout::INTERLEAVED);
        TT_ASSERT(this->output_mem_config.memory_layout == TensorMemoryLayout::INTERLEAVED);
    }
}

std::vector<Shape> Tilize::compute_output_shapes(const std::vector<Tensor> &input_tensors) const {
    const auto& input_tensor_a = input_tensors.at(0);
    auto output_shape = input_tensor_a.shape();
    return {output_shape};
}

std::vector<Tensor> Tilize::create_output_tensors(const std::vector<Tensor> &input_tensors) const {
    const auto& input_tensor = input_tensors.at(0);
    if (input_tensor.memory_config().is_sharded()) {
        return {create_sharded_device_tensor(this->compute_output_shapes(input_tensors).at(0), input_tensor.dtype(), Layout::TILE, input_tensor.device(), input_tensor.memory_config(), input_tensor.shard_spec().value())};
    } else {
        return operation::generic_create_output_tensors(*this, input_tensors, input_tensor.dtype(), Layout::TILE, this->output_mem_config);
    }
}

operation::ProgramWithCallbacks Tilize::create_program(const std::vector<Tensor>& input_tensors, std::vector<Tensor> &output_tensors) const {
    const auto& input_tensor_a = input_tensors.at(0);
    auto& output_tensor = output_tensors.at(0);
    if (use_multicore) {
        return tilize_multi_core(input_tensor_a, output_tensor);
    } else {
        return tilize_single_core(input_tensor_a, output_tensor);
    }
}

tt::stl::reflection::Attributes Tilize::attributes() const {
    return {
        {"output_mem_config", this->output_mem_config},
    };
}

Tensor tilize(const Tensor &input_tensor_a, const MemoryConfig& output_mem_config, bool use_multicore) {
    // No-op (Will do a tensor copy)
    if (input_tensor_a.layout() == Layout::TILE) {
        log_warning("Perf warning: tilize called on already tilized tensor.");
        if (input_tensor_a.memory_config() != output_mem_config) {
            return clone(input_tensor_a, output_mem_config);
        } else {
            return input_tensor_a;
        }
    }
    return operation::run_without_autoformat(Tilize{output_mem_config, use_multicore}, {input_tensor_a}).at(0);
}

void TilizeWithValPadding::validate(const std::vector<Tensor> &input_tensors) const {
    const auto& input_tensor_a = input_tensors.at(0);
    TT_ASSERT(input_tensor_a.storage_type() == StorageType::DEVICE, "Operands need to be on device!");
    TT_ASSERT(input_tensor_a.buffer() != nullptr , "Operands need to be allocated in buffers on device!");
    TT_ASSERT(input_tensor_a.layout() == Layout::ROW_MAJOR, "Can only tilize row major data");
    TT_ASSERT(input_tensor_a.dtype() == DataType::BFLOAT16);

    TT_ASSERT(input_tensor_a.shape()[0] + this->input_tensor_start[0] <= this->output_tensor_shape[0]);
    TT_ASSERT(input_tensor_a.shape()[1] + this->input_tensor_start[1] <= this->output_tensor_shape[1]);
    TT_ASSERT(input_tensor_a.shape()[2] + this->input_tensor_start[2] <= this->output_tensor_shape[2]);
    TT_ASSERT(input_tensor_a.shape()[3] + this->input_tensor_start[3] <= this->output_tensor_shape[3]);
    TT_ASSERT((this->input_tensor_start[0] == 0 && this->input_tensor_start[1] == 0 && this->input_tensor_start[2] == 0 && this->input_tensor_start[3] == 0), "On device padding only supports padding at end of dims");

    uint32_t num_rows = this->output_tensor_shape[2];
    uint32_t inner_dim = this->output_tensor_shape[3];
    TT_ASSERT(num_rows % TILE_HEIGHT == 0, "Output shape must be tilizable");
    TT_ASSERT(inner_dim % TILE_WIDTH == 0, "Output shape must be tilizable");
}
std::vector<Shape> TilizeWithValPadding::compute_output_shapes(const std::vector<Tensor> &input_tensors) const {
    auto input_shape = input_tensors.at(0).shape();
    auto dimensions_pads = std::vector<Padding::PadDimension>();
    for (auto index = 0; index < input_shape.rank(); index++) {
        auto front = this->input_tensor_start[index];
        auto back = this->output_tensor_shape[index] - (this->input_tensor_start[index] + input_shape[index]);
        dimensions_pads.push_back(Padding::PadDimension{.front=front, .back=back});
    }
    const auto padding = Padding(dimensions_pads, Padding::PadValue::Any);
    return {Shape(this->output_tensor_shape, padding)};
}
std::vector<Tensor> TilizeWithValPadding::create_output_tensors(const std::vector<Tensor> &input_tensors) const {
    const auto& input_tensor_a = input_tensors.at(0);
    return operation::generic_create_output_tensors(*this, input_tensors, input_tensor_a.dtype(), Layout::TILE, this->output_mem_config);
}

// TODO: If pad is called on a tile and output is not tile, we could untilize then pad, and output is RM
// Currently calling pad on a tile requires the output pad shape to be tile
operation::ProgramWithCallbacks TilizeWithValPadding::create_program(const std::vector<Tensor>& input_tensors, std::vector<Tensor> &output_tensors) const {
    const auto& input_tensor_a = input_tensors.at(0);
    auto& output_tensor = output_tensors.at(0);
    return tilize_with_val_padding_single_core(input_tensor_a, output_tensor, this->output_tensor_shape, this->input_tensor_start, this->pad_value);
}

tt::stl::reflection::Attributes TilizeWithValPadding::attributes() const {
    return {
        {"output_tensor_shape", this->output_tensor_shape},
        {"input_tensor_start", this->input_tensor_start},
        {"pad_value", this->pad_value},
        {"output_mem_config", this->output_mem_config},
    };
}

Tensor tilize_with_val_padding(const Tensor &input_tensor_a, const Shape &output_tensor_shape, const Shape &input_tensor_start, const float pad_value, const MemoryConfig& output_mem_config) {
    // No-op (Will do a tensor copy)
    // TODO: We need to run asserts before this
    if (input_tensor_a.layout() == Layout::TILE) {
        if (output_tensor_shape == input_tensor_a.shape()) {
            log_warning("Perf warning: tilize with padding called on already tilized tensor of target shape.");
            return input_tensor_a;
        } else {
            TT_ASSERT(false, "Cannot tilize and pad tensor that is already tilized");
        }
    }
    return operation::run_without_autoformat(TilizeWithValPadding{output_tensor_shape, input_tensor_start, pad_value, output_mem_config}, {input_tensor_a}).at(0);

}

Tensor tilize_with_zero_padding(const Tensor &input_tensor_a, const MemoryConfig& output_mem_config) {
    // No-op (Will do a tensor copy)
    if (input_tensor_a.layout() == Layout::TILE) {
        log_warning("Perf warning: tilize called on already tilized tensor.");
        if (input_tensor_a.memory_config() != output_mem_config) {
            return clone(input_tensor_a, output_mem_config);
        } else {
            return input_tensor_a;
        }
    }
    auto shape = input_tensor_a.shape();


    shape[2] = round_up(shape[2], TILE_HEIGHT);
    shape[3] = round_up(shape[3], TILE_WIDTH);
    return tilize_with_val_padding(input_tensor_a, shape, {0, 0, 0, 0}, 0, output_mem_config);
}

}  // namespace tt_metal

}  // namespace tt
