/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#ifndef TENSORFLOW_LITE_KERNELS_INTERNAL_REFERENCE_CONV_H_
#define TENSORFLOW_LITE_KERNELS_INTERNAL_REFERENCE_CONV_H_

#include <algorithm>

#include "tensorflow/lite/kernels/internal/common.h"
#include "tensorflow/lite/kernels/internal/types.h"

namespace tflite {

namespace reference_ops {


static __attribute__((noinline)) void Conv3x3Depth16P4E5(
    const float* input_data, const float* filter_data,
    const float* bias_data, float* output_data,
    const float output_activation_min,
    const float output_activation_max) {
  constexpr int kInputRowStride = 9 * 16;
  constexpr int kFilterChannelStride = 3 * 3 * 16;
  constexpr int kOutputRowStride = 7 * 32;

  for (int out_y = 0; out_y < 47; ++out_y) {
    const float* row0 = input_data + out_y * kInputRowStride;
    const float* row1 = row0 + kInputRowStride;
    const float* row2 = row1 + kInputRowStride;
    float* output_row = output_data + out_y * kOutputRowStride;

    for (int out_x = 0; out_x < 7; ++out_x) {
      const int x = out_x * 16;

      const float* p00 = row0 + x;
      const float* p01 = p00 + 16;
      const float* p02 = p00 + 32;
      const float* p10 = row1 + x;
      const float* p11 = p10 + 16;
      const float* p12 = p10 + 32;
      const float* p20 = row2 + x;
      const float* p21 = p20 + 16;
      const float* p22 = p20 + 32;

      float* output_pixel = output_row + out_x * 32;

      for (int out_channel = 0; out_channel < 32; ++out_channel) {
        const float* f =
            filter_data + out_channel * kFilterChannelStride;
        float total = 0.0f;

#define P4E5_ACCUM16(P, F)                 \
        total += (P)[0]  * (F)[0];         \
        total += (P)[1]  * (F)[1];         \
        total += (P)[2]  * (F)[2];         \
        total += (P)[3]  * (F)[3];         \
        total += (P)[4]  * (F)[4];         \
        total += (P)[5]  * (F)[5];         \
        total += (P)[6]  * (F)[6];         \
        total += (P)[7]  * (F)[7];         \
        total += (P)[8]  * (F)[8];         \
        total += (P)[9]  * (F)[9];         \
        total += (P)[10] * (F)[10];        \
        total += (P)[11] * (F)[11];        \
        total += (P)[12] * (F)[12];        \
        total += (P)[13] * (F)[13];        \
        total += (P)[14] * (F)[14];        \
        total += (P)[15] * (F)[15]

        P4E5_ACCUM16(p00, f + 0);
        P4E5_ACCUM16(p01, f + 16);
        P4E5_ACCUM16(p02, f + 32);
        P4E5_ACCUM16(p10, f + 48);
        P4E5_ACCUM16(p11, f + 64);
        P4E5_ACCUM16(p12, f + 80);
        P4E5_ACCUM16(p20, f + 96);
        P4E5_ACCUM16(p21, f + 112);
        P4E5_ACCUM16(p22, f + 128);

#undef P4E5_ACCUM16

        const float bias_value =
            bias_data ? bias_data[out_channel] : 0.0f;

        output_pixel[out_channel] =
            ActivationFunctionWithMinMax(
                total + bias_value,
                output_activation_min,
                output_activation_max);
      }
    }
  }
}

inline void Conv(const ConvParams& params, const RuntimeShape& input_shape,
                 const float* input_data, const RuntimeShape& filter_shape,
                 const float* filter_data, const RuntimeShape& bias_shape,
                 const float* bias_data, const RuntimeShape& output_shape,
                 float* output_data, const RuntimeShape& im2col_shape,
                 float* im2col_data) {
  const int stride_width = params.stride_width;
  const int stride_height = params.stride_height;
  const int dilation_width_factor = params.dilation_width_factor;
  const int dilation_height_factor = params.dilation_height_factor;
  const int pad_width = params.padding_values.width;
  const int pad_height = params.padding_values.height;
  const float output_activation_min = params.float_activation_min;
  const float output_activation_max = params.float_activation_max;
  TFLITE_DCHECK_EQ(input_shape.DimensionsCount(), 4);
  TFLITE_DCHECK_EQ(filter_shape.DimensionsCount(), 4);
  TFLITE_DCHECK_EQ(output_shape.DimensionsCount(), 4);

  (void)im2col_data;
  (void)im2col_shape;

  const int batches = MatchingDim(input_shape, 0, output_shape, 0);
  const int input_depth = input_shape.Dims(3);
  const int output_depth = MatchingDim(filter_shape, 0, output_shape, 3);

  if (bias_data) {
    TFLITE_DCHECK_EQ(bias_shape.FlatSize(), output_depth);
  }

  const int input_height = input_shape.Dims(1);
  const int input_width = input_shape.Dims(2);
  const int filter_height = filter_shape.Dims(1);
  const int filter_width = filter_shape.Dims(2);
  const int filter_input_depth = filter_shape.Dims(3);
  const int groups = input_depth / filter_input_depth;
  TFLITE_DCHECK_NE(groups, 0);
  TFLITE_DCHECK_EQ(input_depth % filter_input_depth, 0);
  const int filters_per_group = output_depth / groups;
  TFLITE_DCHECK_NE(filters_per_group, 0);
  const int output_height = output_shape.Dims(1);
  const int output_width = output_shape.Dims(2);

  const int input_row_stride = input_width * input_depth;
  const int input_batch_stride = input_height * input_row_stride;
  const int filter_x_stride = filter_input_depth;
  const int filter_y_stride = filter_width * filter_x_stride;
  const int filter_channel_stride = filter_height * filter_y_stride;
  const int output_row_stride = output_width * output_depth;
  const int output_batch_stride = output_height * output_row_stride;

  const bool p4e2_valid_3x3 =
      (batches == 1) &&
      (groups == 1) &&
      (filter_height == 3) &&
      (filter_width == 3) &&
      (stride_height == 1) &&
      (stride_width == 1) &&
      (dilation_height_factor == 1) &&
      (dilation_width_factor == 1) &&
      (pad_height == 0) &&
      (pad_width == 0) &&
      (output_height == (input_height - 2)) &&
      (output_width == (input_width - 2));

  const bool p4e5_conv2 =
      p4e2_valid_3x3 &&
      (input_height == 49) &&
      (input_width == 9) &&
      (input_depth == 16) &&
      (filter_input_depth == 16) &&
      (output_height == 47) &&
      (output_width == 7) &&
      (output_depth == 32);

  if (p4e5_conv2) {
    Conv3x3Depth16P4E5(
        input_data,
        filter_data,
        bias_data,
        output_data,
        output_activation_min,
        output_activation_max);
    return;
  }

  if (p4e2_valid_3x3) {
    for (int out_y = 0; out_y < output_height; ++out_y) {
      float* output_row = output_data + out_y * output_row_stride;

      for (int out_x = 0; out_x < output_width; ++out_x) {
        float* output_pixel = output_row + out_x * output_depth;
        const float* input_origin =
            input_data + out_y * input_row_stride + out_x * input_depth;

        for (int out_channel = 0;
             out_channel < output_depth;
             ++out_channel) {
          const float* filter_channel =
              filter_data + out_channel * filter_channel_stride;
          float total = 0.0f;

          for (int filter_y = 0; filter_y < 3; ++filter_y) {
            const float* input_row =
                input_origin + filter_y * input_row_stride;
            const float* filter_row =
                filter_channel + filter_y * filter_y_stride;

            for (int filter_x = 0; filter_x < 3; ++filter_x) {
              const float* input_ptr =
                  input_row + filter_x * input_depth;
              const float* filter_ptr =
                  filter_row + filter_x * filter_input_depth;

              if (filter_input_depth == 16) {
                total += input_ptr[0]  * filter_ptr[0];
                total += input_ptr[1]  * filter_ptr[1];
                total += input_ptr[2]  * filter_ptr[2];
                total += input_ptr[3]  * filter_ptr[3];
                total += input_ptr[4]  * filter_ptr[4];
                total += input_ptr[5]  * filter_ptr[5];
                total += input_ptr[6]  * filter_ptr[6];
                total += input_ptr[7]  * filter_ptr[7];
                total += input_ptr[8]  * filter_ptr[8];
                total += input_ptr[9]  * filter_ptr[9];
                total += input_ptr[10] * filter_ptr[10];
                total += input_ptr[11] * filter_ptr[11];
                total += input_ptr[12] * filter_ptr[12];
                total += input_ptr[13] * filter_ptr[13];
                total += input_ptr[14] * filter_ptr[14];
                total += input_ptr[15] * filter_ptr[15];
              } else {
                for (int in_channel = 0;
                     in_channel < filter_input_depth;
                     ++in_channel) {
                  total += input_ptr[in_channel] * filter_ptr[in_channel];
                }
              }
            }
          }

          const float bias_value =
              bias_data ? bias_data[out_channel] : 0.0f;

          output_pixel[out_channel] =
              ActivationFunctionWithMinMax(
                  total + bias_value,
                  output_activation_min,
                  output_activation_max);
        }
      }
    }

    return;
  }

  for (int batch = 0; batch < batches; ++batch) {
    const float* input_batch = input_data + batch * input_batch_stride;
    float* output_batch = output_data + batch * output_batch_stride;

    for (int out_y = 0; out_y < output_height; ++out_y) {
      const int in_y_origin = (out_y * stride_height) - pad_height;
      float* output_row = output_batch + out_y * output_row_stride;

      for (int out_x = 0; out_x < output_width; ++out_x) {
        const int in_x_origin = (out_x * stride_width) - pad_width;
        float* output_pixel = output_row + out_x * output_depth;

        for (int out_channel = 0; out_channel < output_depth; ++out_channel) {
          const int group = out_channel / filters_per_group;
          const int input_channel_base = group * filter_input_depth;
          const float* filter_channel =
              filter_data + out_channel * filter_channel_stride;

          float total = 0.0f;

          for (int filter_y = 0; filter_y < filter_height; ++filter_y) {
            const int in_y =
                in_y_origin + dilation_height_factor * filter_y;

            if ((in_y < 0) || (in_y >= input_height)) {
              continue;
            }

            const float* input_row =
                input_batch + in_y * input_row_stride;
            const float* filter_row =
                filter_channel + filter_y * filter_y_stride;

            for (int filter_x = 0; filter_x < filter_width; ++filter_x) {
              const int in_x =
                  in_x_origin + dilation_width_factor * filter_x;

              if ((in_x < 0) || (in_x >= input_width)) {
                continue;
              }

              const float* input_ptr =
                  input_row + in_x * input_depth + input_channel_base;
              const float* filter_ptr =
                  filter_row + filter_x * filter_x_stride;

              for (int in_channel = 0;
                   in_channel < filter_input_depth;
                   ++in_channel) {
                total += input_ptr[in_channel] * filter_ptr[in_channel];
              }
            }
          }

          const float bias_value =
              bias_data ? bias_data[out_channel] : 0.0f;

          output_pixel[out_channel] =
              ActivationFunctionWithMinMax(
                  total + bias_value,
                  output_activation_min,
                  output_activation_max);
        }
      }
    }
  }
}

inline void Conv(const ConvParams& params, const RuntimeShape& input_shape,
                 const uint8_t* input_data, const RuntimeShape& filter_shape,
                 const uint8_t* filter_data, const RuntimeShape& bias_shape,
                 const int32_t* bias_data, const RuntimeShape& output_shape,
                 uint8_t* output_data, const RuntimeShape& im2col_shape,
                 uint8_t* im2col_data, void* cpu_backend_context) {
  (void)cpu_backend_context;  // only used in optimized code.
  (void)im2col_data;          // only used in optimized code.
  (void)im2col_shape;         // only used in optimized code.
  const int stride_width = params.stride_width;
  const int stride_height = params.stride_height;
  const int dilation_width_factor = params.dilation_width_factor;
  const int dilation_height_factor = params.dilation_height_factor;
  const int pad_width = params.padding_values.width;
  const int pad_height = params.padding_values.height;
  const int32_t input_offset = params.input_offset;
  const int32_t filter_offset = params.weights_offset;
  const int32_t output_offset = params.output_offset;
  const int32_t output_multiplier = params.output_multiplier;
  const int output_shift = params.output_shift;
  const int32_t output_activation_min = params.quantized_activation_min;
  const int32_t output_activation_max = params.quantized_activation_max;
  TFLITE_DCHECK_LE(output_activation_min, output_activation_max);

  TFLITE_DCHECK_EQ(input_shape.DimensionsCount(), 4);
  TFLITE_DCHECK_EQ(filter_shape.DimensionsCount(), 4);
  TFLITE_DCHECK_EQ(output_shape.DimensionsCount(), 4);
  const int batches = MatchingDim(input_shape, 0, output_shape, 0);
  const int input_depth = input_shape.Dims(3);
  const int output_depth = MatchingDim(filter_shape, 0, output_shape, 3);
  if (bias_data) {
    TFLITE_DCHECK_EQ(bias_shape.FlatSize(), output_depth);
  }
  const int input_height = input_shape.Dims(1);
  const int input_width = input_shape.Dims(2);
  const int filter_height = filter_shape.Dims(1);
  const int filter_width = filter_shape.Dims(2);
  const int filter_input_depth = filter_shape.Dims(3);
  const int groups = input_depth / filter_input_depth;
  TFLITE_DCHECK_EQ(input_depth % filter_input_depth, 0);
  const int filters_per_group = output_depth / groups;
  const int output_height = output_shape.Dims(1);
  const int output_width = output_shape.Dims(2);
  for (int batch = 0; batch < batches; ++batch) {
    for (int out_y = 0; out_y < output_height; ++out_y) {
      const int in_y_origin = (out_y * stride_height) - pad_height;
      for (int out_x = 0; out_x < output_width; ++out_x) {
        const int in_x_origin = (out_x * stride_width) - pad_width;
        for (int out_channel = 0; out_channel < output_depth; ++out_channel) {
          auto group = out_channel / filters_per_group;
          int32_t acc = 0;
          for (int filter_y = 0; filter_y < filter_height; ++filter_y) {
            const int in_y = in_y_origin + dilation_height_factor * filter_y;
            for (int filter_x = 0; filter_x < filter_width; ++filter_x) {
              const int in_x = in_x_origin + dilation_width_factor * filter_x;

              // Zero padding by omitting the areas outside the image.
              const bool is_point_inside_image =
                  (in_x >= 0) && (in_x < input_width) && (in_y >= 0) &&
                  (in_y < input_height);

              if (!is_point_inside_image) {
                continue;
              }

              for (int in_channel = 0; in_channel < filter_input_depth;
                   ++in_channel) {
                int32_t input_val =
                    input_data[Offset(input_shape, batch, in_y, in_x,
                                      in_channel + group * filter_input_depth)];
                int32_t filter_val = filter_data[Offset(
                    filter_shape, out_channel, filter_y, filter_x, in_channel)];
                acc +=
                    (filter_val + filter_offset) * (input_val + input_offset);
              }
            }
          }
          if (bias_data) {
            acc += bias_data[out_channel];
          }
          acc = MultiplyByQuantizedMultiplier(acc, output_multiplier,
                                              output_shift);
          acc += output_offset;
          acc = std::max(acc, output_activation_min);
          acc = std::min(acc, output_activation_max);
          output_data[Offset(output_shape, batch, out_y, out_x, out_channel)] =
              static_cast<uint8_t>(acc);
        }
      }
    }
  }
}

inline void HybridConvPerChannel(
    const ConvParams& params, float* scaling_factors_ptr,
    const RuntimeShape& input_shape, const int8_t* input_data,
    const RuntimeShape& filter_shape, const int8_t* filter_data,
    const RuntimeShape& bias_shape, const float* bias_data,
    const RuntimeShape& output_shape, float* output_data,
    const RuntimeShape& im2col_shape, int8_t* im2col_data,
    const float* per_channel_scale, int32_t* input_offset) {
  (void)im2col_data;   // only used in optimized code.
  (void)im2col_shape;  // only used in optimized code.
  const int stride_width = params.stride_width;
  const int stride_height = params.stride_height;
  const int dilation_width_factor = params.dilation_width_factor;
  const int dilation_height_factor = params.dilation_height_factor;
  const int pad_width = params.padding_values.width;
  const int pad_height = params.padding_values.height;
  const float output_activation_min = params.float_activation_min;
  const float output_activation_max = params.float_activation_max;
  TFLITE_DCHECK_EQ(input_shape.DimensionsCount(), 4);
  TFLITE_DCHECK_EQ(filter_shape.DimensionsCount(), 4);
  TFLITE_DCHECK_EQ(output_shape.DimensionsCount(), 4);
  const int batches = MatchingDim(input_shape, 0, output_shape, 0);
  const int input_depth = input_shape.Dims(3);
  const int output_depth = MatchingDim(filter_shape, 0, output_shape, 3);
  if (bias_data) {
    TFLITE_DCHECK_EQ(bias_shape.FlatSize(), output_depth);
  }
  const int input_height = input_shape.Dims(1);
  const int input_width = input_shape.Dims(2);
  const int filter_height = filter_shape.Dims(1);
  const int filter_width = filter_shape.Dims(2);
  const int filter_input_depth = filter_shape.Dims(3);
  const int groups = input_depth / filter_input_depth;
  TFLITE_DCHECK_EQ(input_depth % filter_input_depth, 0);
  const int filters_per_group = output_depth / groups;
  const int output_height = output_shape.Dims(1);
  const int output_width = output_shape.Dims(2);
  for (int batch = 0; batch < batches; ++batch) {
    for (int out_y = 0; out_y < output_height; ++out_y) {
      for (int out_x = 0; out_x < output_width; ++out_x) {
        for (int out_channel = 0; out_channel < output_depth; ++out_channel) {
          auto group = out_channel / filters_per_group;
          const int in_x_origin = (out_x * stride_width) - pad_width;
          const int in_y_origin = (out_y * stride_height) - pad_height;
          int32_t acc = 0;
          for (int filter_y = 0; filter_y < filter_height; ++filter_y) {
            for (int filter_x = 0; filter_x < filter_width; ++filter_x) {
              for (int in_channel = 0; in_channel < filter_input_depth;
                   ++in_channel) {
                const int in_x = in_x_origin + dilation_width_factor * filter_x;
                const int in_y =
                    in_y_origin + dilation_height_factor * filter_y;
                // If the location is outside the bounds of the input image,
                // use zero as a default value.
                if ((in_x >= 0) && (in_x < input_width) && (in_y >= 0) &&
                    (in_y < input_height)) {
                  int32_t input_val = input_data[Offset(
                      input_shape, batch, in_y, in_x,
                      in_channel + group * filter_input_depth)];
                  int32_t filter_val =
                      filter_data[Offset(filter_shape, out_channel, filter_y,
                                         filter_x, in_channel)];
                  acc += filter_val * (input_val - input_offset[batch]);
                }
              }
            }
          }
          float acc_float =
              acc * per_channel_scale[out_channel] * scaling_factors_ptr[batch];
          if (bias_data) {
            acc_float += bias_data[out_channel];
          }
          output_data[Offset(output_shape, batch, out_y, out_x, out_channel)] =
              ActivationFunctionWithMinMax(acc_float, output_activation_min,
                                           output_activation_max);
        }
      }
    }
  }
}

}  // namespace reference_ops
}  // namespace tflite

#endif  // TENSORFLOW_LITE_KERNELS_INTERNAL_REFERENCE_CONV_H_
