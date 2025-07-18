#include <cstdint>
#include <cstring>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include <fmt/core.h>

#include <tt-metalium/device.hpp>
#include <tt-metalium/constants.hpp>
#include <tt-metalium/host_api.hpp>
#include <tt-metalium/tilize_utils.hpp>
#include <tt-metalium/work_split.hpp>

// #include "ttnn/tensor/tensor.hpp"
// #include "ttnn/operations/core/core.hpp"

#include "ttnn/tensor/tensor.hpp"
#include "ttnn/tensor/types.hpp"
#include "ttnn/operations/functions.hpp"
#include "ttnn/operations/generic/generic_op.hpp"
#include "ttnn/operations/eltwise/unary/unary.hpp"
#include "ttnn/operations/matmul/matmul.hpp"
#include "ttnn/operations/eltwise/binary/binary.hpp"
#include "ttnn/operations/reduction/argmax/argmax.hpp"

#ifndef OVERRIDE_KERNEL_PREFIX
#define OVERRIDE_KERNEL_PREFIX ""
#endif

using namespace tt;
using namespace tt::tt_metal;
using namespace tt::constants;

uint32_t tile_volume(const ttnn::Tensor& input_tensor) {
    const auto& tile_shape = input_tensor.tensor_spec().tile().get_tile_shape();
    return tile_shape[0] * tile_shape[1];
}

uint32_t get_num_tiles(const ttnn::Tensor& input_tensor) {
    const auto& shape = input_tensor.padded_shape();
    auto tile_vol = tile_volume(input_tensor);
    return shape.volume() / tile_vol;
}

ttnn::Shape get_tiled_shape(const ttnn::Tensor& input_tensor) {
    const auto& tile_shape = input_tensor.tensor_spec().tile().get_tile_shape();
    const auto& shape = input_tensor.padded_shape();
    ttnn::SmallVector<uint32_t> tiled_shape;
    tiled_shape.reserve(shape.rank());
    for (int i = 0; i < shape.rank(); i++) {
        uint32_t dim = 0;
        if (i == shape.rank() - 1) {
            dim = shape[i] / tile_shape[1];
        } else if (i == shape.rank() - 2) {
            dim = shape[i] / tile_shape[0];
        } else {
            dim = shape[i];
        }
        tiled_shape.push_back(dim);
    }
    auto res = ttnn::Shape(tiled_shape);
    return res;
}

ttnn::SmallVector<uint32_t> get_strides(const ttnn::Shape& shape) {
    ttnn::SmallVector<uint32_t> strides(shape.rank());
    strides[shape.rank() - 1] = 1;
    for (int i = shape.rank() - 2; i >= 0; i--) {
        strides[i] = strides[i + 1] * shape[i + 1];
    }
    return strides;
}

static std::vector<uint32_t> compute_strides(const std::vector<uint32_t>& shape) {
    size_t n = shape.size();
    std::vector<uint32_t> strides(n);
    strides[n - 1] = 1;
    for (int i = n - 2; i >= 0; --i) {
        strides[i] = strides[i + 1] * shape[i + 1];
    }
    return strides;
}

// Pretty print a tensor stored as a flat vector
template <typename T>
void pprint(std::vector<T>& tensor, std::vector<uint32_t> dims) {
    size_t ndim = dims.size();
    size_t total_elems = tensor.size();

    // Helper: recursively print
    std::function<void(size_t, size_t, std::string)> print_recursive;
    print_recursive = [&](size_t dim, size_t offset, std::string indent) {
        if (dim == ndim - 1) {
            // Innermost dimension: print elements in one line
            std::cout << indent << "[";
            for (uint32_t i = 0; i < dims[dim]; ++i) {
                std::cout << tensor[offset + i];
                if (i != dims[dim] - 1)
                    std::cout << ", ";
            }
            std::cout << "]";
        } else {
            // Outer dimensions: print nested brackets
            std::cout << indent << "[\n";
            size_t step = 1;
            for (size_t j = dim + 1; j < ndim; ++j)
                step *= dims[j];
            for (uint32_t i = 0; i < dims[dim]; ++i) {
                print_recursive(dim + 1, offset + i * step, indent + "  ");
                if (i != dims[dim] - 1)
                    std::cout << ",\n";
                else
                    std::cout << "\n";
            }
            std::cout << indent << "]";
        }
    };

    // Start recursion
    print_recursive(0, 0, "");
    std::cout << std::endl;
}

void tensor_flip_cpu(
    const std::vector<uint32_t>& src,
    std::vector<uint32_t>& dst,
    const std::vector<uint32_t>& tensor_shape,
    const std::vector<uint32_t>& dims_to_flip) 
{
    const size_t numel = src.size();
    dst.resize(numel);
    auto strides = compute_strides(tensor_shape);
    const size_t ndim = tensor_shape.size();

    for (size_t idx = 0; idx < numel; ++idx) {
        size_t linear = idx, dst_linear = 0;
        for (size_t dim = 0; dim < ndim; ++dim) {
            uint32_t coord = linear / strides[dim];
            linear %= strides[dim];
            // flip coordinate if needed
            if (std::find(dims_to_flip.begin(), dims_to_flip.end(), dim) != dims_to_flip.end()) {
                coord = tensor_shape[dim] - 1 - coord;
            }
            dst_linear += coord * strides[dim];
        }
        dst[dst_linear] = src[idx];
    }
}

int main(int argc, char** argv) {
    std::vector<uint32_t> dims_to_flip = {2, 3};

    constexpr uint32_t N = 1;
    constexpr uint32_t C = 3;
    constexpr uint32_t H = 96;
    constexpr uint32_t W = 96;
    constexpr uint32_t numel = N * C * H * W;

    std::mt19937 gen(69);
    std::uniform_int_distribution<int> dist(0, 10);

    std::vector<uint32_t> src_vec(numel, 0);
    std::vector<uint32_t> shape = {N, C, H, W};
    for (auto& v : src_vec) v = dist(gen);

    std::vector<uint32_t> result_tt(numel, 0);
    std::vector<uint32_t> result_cpu(numel, 0);

    tensor_flip_cpu(src_vec, result_cpu, shape, dims_to_flip);

    // tt part
    constexpr int device_id = 0;
    IDevice* device = CreateDevice(device_id);
    Program program = CreateProgram();
    CommandQueue& cq = device->command_queue();

    ttnn::Shape input_shape({N, C, H, W});
    ttnn::MemoryConfig memory_config(ttnn::TensorMemoryLayout::INTERLEAVED, ttnn::BufferType::DRAM);
    ttnn::PageConfig page_config(ttnn::Layout::TILE);
    ttnn::TensorLayout layout_config(ttnn::DataType::UINT32, page_config, memory_config);
    ttnn::TensorSpec tensor_spec(input_shape, layout_config);

    ttnn::Tensor input_tensor = ttnn::Tensor::from_vector(src_vec, tensor_spec);
    input_tensor = input_tensor.to_device(device);

    ttnn::Tensor output_tensor = ttnn::Tensor::from_vector(result_tt, tensor_spec);
    output_tensor = output_tensor.to_device(device);

    uint32_t rank = input_tensor.logical_shape().rank();
    uint32_t num_tiles = get_num_tiles(input_tensor);
    ttnn::Shape input_tile_shape = get_tiled_shape(input_tensor);
    ttnn::SmallVector<uint32_t> input_tile_strides = get_strides(input_tile_shape);

    fmt::print("input_shape: {}\n", input_shape);
    fmt::print("input_tile_shape: {}\n", input_tile_shape);
    fmt::print("input_tile_strides: {}\n", input_tile_strides);

    // Split the work to all available cores
    auto core_grid = device->compute_with_storage_grid_size();
    auto [num_cores,
        all_cores,
        core_group_1,
        core_group_2,
        num_tiles_per_core_group_1,
        num_tiles_per_core_group_2] = split_work_to_cores(core_grid, num_tiles);

    fmt::print("core_grid: {}\n", core_grid);
    fmt::print("num_cores: {}\n", num_cores);
    fmt::print("all_cores: {}\n", all_cores);
    fmt::print("core_group_1: {}\n", core_group_1);
    fmt::print("core_group_2: {}\n", core_group_2);
    fmt::print("num_tiles_per_core_group_1: {}\n", num_tiles_per_core_group_1);
    fmt::print("num_tiles_per_core_group_2: {}\n", num_tiles_per_core_group_2);

    // Configure Circular Buffers
    constexpr uint32_t tile_size = sizeof(uint32_t) * TILE_HW;
    const auto cb_data_format = tt::DataFormat::UInt32;
    uint32_t cb_size = 2 * tile_size; // double buffering

    auto cb_inp = CreateCircularBuffer(
        program,
        all_cores,
        CircularBufferConfig(cb_size, {{CBIndex::c_0, cb_data_format}})
            .set_page_size(CBIndex::c_0, tile_size));

    // Create kernels
    std::vector<uint32_t> reader_ct_args = {(uint32_t)input_tensor.buffer()->is_dram(), rank};
    auto reader_id = CreateKernel(
        program,
        OVERRIDE_KERNEL_PREFIX "tensor_flip/kernels/reader_kernel.cpp",
        all_cores,
        ReaderDataMovementConfig(reader_ct_args)
    );

    std::vector<uint32_t> writer_ct_args = {(uint32_t)output_tensor.buffer()->is_dram()};
    auto writer_id = CreateKernel(
        program,
        OVERRIDE_KERNEL_PREFIX "tensor_flip/kernels/writer_kernel.cpp",
        all_cores,
        WriterDataMovementConfig(writer_ct_args)
    );

    auto work_groups = {
        std::make_pair(core_group_1, num_tiles_per_core_group_1),
        std::make_pair(core_group_2, num_tiles_per_core_group_2)};

    for (uint32_t src_tile_id = 0; src_tile_id < num_tiles; ++src_tile_id) {
        size_t remaining = src_tile_id;

        std::vector<uint32_t> src_multi_dim(rank, 0);
        std::vector<uint32_t> dst_multi_dim(rank, 0);

        for (uint32_t i = 0; i < rank; ++i) {
            size_t dim = rank - i;
            src_multi_dim[dim] = remaining % input_tile_shape[i];

            bool should_flip = std::find(dims_to_flip.begin(), dims_to_flip.end(), dim) != dims_to_flip.end();
            if (should_flip) {
                // calculate dst tile multi dimension coordinate
                dst_multi_dim[dim] = input_tile_shape[dim] - src_multi_dim[dim] - 1;
            } else {
                dst_multi_dim[dim] = src_multi_dim[dim];
            }

            remaining /= input_tile_shape[i];
        }

        // dst tile multi dimension coordinate -> dst_linear_tile_id
        uint32_t dst_tile_id = 0;
        for (uint32_t j = 0; j < rank; ++j) {
            dst_tile_id += dst_multi_dim[j] * input_tile_strides[j];
        }

        fmt::print("src_tile_id: {}, dst_tile_id: {}, src_multi_dim: {}, dst_multi_dim: {}\n",
            src_tile_id, dst_tile_id, src_multi_dim, dst_multi_dim);
    }

    // Set Runtime Arguments for Kernels
    std::vector<uint32_t> reader_rt_args = {input_tensor.buffer()->address(), 0, 0};
    std::vector<uint32_t> writer_rt_args = {output_tensor.buffer()->address(), 0, 0};

    // reader_rt_args.insert(
    //     reader_runtime_args.end(),
    //     input_tile_strides.begin(),
    //     input_tile_strides.end()
    // );

    uint32_t start_tile = 0;
    uint32_t end_tile = 0;
    for (const auto& [ranges, tiles_per_core] : work_groups) {
        for (const auto& range : ranges.ranges()) {
            for (const auto& core : range) {
                end_tile += tiles_per_core;

                reader_rt_args[1] = start_tile;
                reader_rt_args[2] = end_tile;
                SetRuntimeArgs(program, reader_id, core, reader_rt_args);

                writer_rt_args[1] = start_tile;
                writer_rt_args[2] = end_tile;
                SetRuntimeArgs(program, writer_id, core, writer_rt_args);

                start_tile += tiles_per_core;
            }
        }
    }

    fmt::print("all_close: {}\n", ttnn::allclose<uint32_t>(input_tensor.cpu(), output_tensor.cpu(), 1e-5f, 1e-5f));
    fmt::print("enqueue program\n");
    EnqueueProgram(cq, program, false);
    Finish(cq);

    fmt::print("finished execution\n");
    fmt::print("all_close: {}\n", ttnn::allclose<uint32_t>(input_tensor.cpu(), output_tensor.cpu(), 1e-5f, 1e-5f));

    CloseDevice(device);
    return 0;
}
