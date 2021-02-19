/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/mlir/tosa/transforms/legalize_utils.h"

#include "mlir/Dialect/Tosa/IR/TosaOps.h"  // from @llvm-project
#include "mlir/Dialect/Tosa/Utils/QuantUtils.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/lite/ir/tfl_ops.h"
#include "tensorflow/compiler/mlir/tosa/transforms/legalize_common.h"

// Implements legalization and post-legalization optimization helper functions

namespace mlir {
namespace tosa {

// Create a TOSA rescale op from TFLite scaling, zero points and rounding mode
Value buildRescale(PatternRewriter& rewriter, Operation* op,
                   RankedTensorType output_type, Value input_val, double scale,
                   int64_t input_zp, int64_t output_zp, bool double_round,
                   bool scale32) {
  int32_t multiplier;
  int32_t shift;

  int32_t scale_width = scale32 ? 32 : 16;

  computeMultiplierAndShift(scale, multiplier, shift, scale_width);

  auto rescale_op = rewriter.create<tosa::RescaleOp>(
      op->getLoc(), output_type, input_val,
      rewriter.getI32IntegerAttr(static_cast<int32_t>(input_zp)),
      rewriter.getI32IntegerAttr(static_cast<int32_t>(output_zp)),
      rewriter.getI32ArrayAttr({multiplier}), rewriter.getI32ArrayAttr({shift}),
      rewriter.getBoolAttr(scale32), rewriter.getBoolAttr(double_round),
      rewriter.getBoolAttr(false));

  return rescale_op.getResult();
}

// Creates TOSA rescale op with int32 output
Value buildRescaleToInt32(PatternRewriter& rewriter, Operation* op,
                          Value input_val, double input_scale,
                          int64_t input_zp) {
  // Output is always int32 type
  auto input_type = input_val.getType().dyn_cast<mlir::RankedTensorType>();
  assert(input_type);
  auto output_type =
      RankedTensorType::get(input_type.getShape(), rewriter.getI32Type());

  return buildRescale(rewriter, op, output_type, input_val, input_scale,
                      input_zp, 0, false, true);
}

// Creates TOSA rescale op with int32 input
Value buildRescaleFromInt32(PatternRewriter& rewriter, Operation* op,
                            RankedTensorType output_type, Value input_val,
                            double output_scale, int64_t output_zp) {
  // Input should be int32 type
  auto input_type = input_val.getType().dyn_cast<mlir::RankedTensorType>();
  (void)input_type;
  assert(input_type && input_type.getElementType().isInteger(32) &&
         "expected rescale input element type to be i32");

  // Potentially check input_shape == output_shape here
  return buildRescale(rewriter, op, output_type, input_val, output_scale, 0,
                      output_zp, true, true);
}

// Creates a TOSA rescale op based on conv2d parameters.
Value buildRescaleOpConvOutput(PatternRewriter& rewriter, Operation* op,
                               Value conv_val, RankedTensorType input_type,
                               RankedTensorType weight_type,
                               RankedTensorType output_type) {
  auto input_qtype =
      input_type.getElementType().dyn_cast<mlir::quant::UniformQuantizedType>();
  auto output_qtype = output_type.getElementType()
                          .dyn_cast<mlir::quant::UniformQuantizedType>();

  double input_scale = input_qtype.getScale();

  int64_t output_zp = output_qtype.getZeroPoint();
  double output_scale = output_qtype.getScale();

  bool scale32 = isScale32(output_qtype);
  int32_t scale_width = scale32 ? 32 : 16;

  if (auto weight_per_tensor_qtype =
          weight_type.getElementType()
              .dyn_cast<mlir::quant::UniformQuantizedType>()) {
    // Per-tensor quantization
    double weight_scale = weight_per_tensor_qtype.getScale();

    int32_t multiplier;
    int32_t shift;

    double op_tensor_scale = (input_scale * weight_scale) / output_scale;

    computeMultiplierAndShift(op_tensor_scale, multiplier, shift, scale_width);

    auto rescale_op = rewriter.create<tosa::RescaleOp>(
        op->getLoc(), output_type, conv_val, rewriter.getI32IntegerAttr(0),
        rewriter.getI32IntegerAttr(output_zp),
        rewriter.getI32ArrayAttr({multiplier}),
        rewriter.getI32ArrayAttr({shift}), rewriter.getBoolAttr(scale32),
        rewriter.getBoolAttr(true), rewriter.getBoolAttr(false));

    return rescale_op.getResult();

  } else if (auto weight_per_channel_qtype =
                 weight_type.getElementType()
                     .dyn_cast<mlir::quant::UniformQuantizedPerAxisType>()) {
    // Per-channel quantization
    auto output_last_axis = output_type.getShape().size() - 1;
    uint32_t output_channels = output_type.getShape()[output_last_axis];

    llvm::SmallVector<int32_t, 4> multiplier_arr;
    llvm::SmallVector<int32_t, 4> shift_arr;

    llvm::SmallVector<double, 4> weight_scale_arr(
        weight_per_channel_qtype.getScales().begin(),
        weight_per_channel_qtype.getScales().end());

    int64_t output_zp = output_qtype.getZeroPoint();
    double output_scale = output_qtype.getScale();

    for (uint32_t oc = 0; oc < output_channels; oc++) {
      double weight_scale = weight_scale_arr[oc];

      int32_t multiplier;
      int32_t shift;

      double op_channel_scale = (input_scale * weight_scale) / output_scale;

      computeMultiplierAndShift(op_channel_scale, multiplier, shift,
                                scale_width);

      multiplier_arr.push_back(multiplier);
      shift_arr.push_back(shift);
    }

    auto rescale_op = rewriter.create<tosa::RescaleOp>(
        op->getLoc(), output_type, conv_val, rewriter.getI32IntegerAttr(0),
        rewriter.getI32IntegerAttr(output_zp),
        rewriter.getI32ArrayAttr(multiplier_arr),
        rewriter.getI32ArrayAttr(shift_arr), rewriter.getBoolAttr(scale32),
        rewriter.getBoolAttr(true), rewriter.getBoolAttr(true));

    return rescale_op.getResult();

  } else {
    op->emitOpError("buildConvRescaleOp: unknown weight quantized type");
    return nullptr;
  }
}

// Create a 8-bit TOSA TABLE constant tensor
// Follow PopulateLookupTable() tensorflow/lite/kernels/activations.cc
Value getTosaConst8bitTable(PatternRewriter& rewriter, Operation* op,
                            double input_scale, int32_t input_zp,
                            double output_scale, int32_t output_zp,
                            std::function<double(double)> func) {
  llvm::SmallVector<int16_t, 4> table_vec;

  // TODO: rewrite this with table[256]
  for (int32_t i = -256; i <= 256; i++) {
    double dequantized = input_scale * (i - input_zp);
    double transformed = func(dequantized);
    int32_t rescaled = std::llround(transformed / output_scale);
    int32_t quantized = static_cast<int32_t>(rescaled + output_zp);
    table_vec.push_back(
        static_cast<int16_t>(std::min(std::max(quantized, -32768), 32767)));
  }

  auto element_qtype =
      UniformQuantizedType::get(true, rewriter.getIntegerType(16),
                                rewriter.getF32Type(), 1.0f, 0, -32768, 32767);
  auto const_type = RankedTensorType::get({513}, element_qtype);
  auto storage_type =
      RankedTensorType::get({513}, element_qtype.getStorageType());
  auto const_attr = DenseElementsAttr::get(
      storage_type, llvm::makeArrayRef<int16_t>(table_vec));

  auto const_op =
      rewriter.create<tosa::ConstOp>(op->getLoc(), const_type, const_attr);
  return const_op.getResult();
}

// Create a 16-bit TOSA TABLE constant tensor
// Only used for 16-bit softmax now
// Follow gen_lut() tensorflow/lite/kernels/internal/common.h
Value getTosaConst16bitTable(PatternRewriter& rewriter, Operation* op,
                             std::function<double(double)> func, double min,
                             double max) {
  llvm::SmallVector<int16_t, 4> table_vec;

  double step = (max - min) / 512.0f;
  double half_step = step / 2.0f;
  for (int32_t i = 0; i <= 512; i++) {
    int32_t sample_val = std::llround(func(min + (i * step)) * 32768.0);
    double midpoint_interp_val =
        std::round(((func(min + (i + 1) * step) * 32768.0) +
                    std::round(func(min + (i * step)) * 32768.0)) /
                   2.0);
    double midpoint_val =
        std::round(func(min + (i * step) + half_step) * 32768.0);
    double midpoint_err = midpoint_interp_val - midpoint_val;
    int32_t bias = std::llround(midpoint_err / 2.0);

    table_vec.push_back(static_cast<int16_t>(
        std::min(std::max(sample_val - bias, -32768), 32767)));
  }

  int32_t max_val = std::llround(func(max) * 32768.0);
  table_vec.push_back(
      static_cast<int16_t>(std::min(std::max(max_val, -32768), 32767)));

  auto element_qtype =
      UniformQuantizedType::get(true, rewriter.getIntegerType(16),
                                rewriter.getF32Type(), 1.0f, 0, -32768, 32767);
  auto const_type = RankedTensorType::get({513}, element_qtype);
  auto storage_type =
      RankedTensorType::get({513}, element_qtype.getStorageType());
  auto const_attr = DenseElementsAttr::get(
      storage_type, llvm::makeArrayRef<int16_t>(table_vec));

  auto const_op =
      rewriter.create<tosa::ConstOp>(op->getLoc(), const_type, const_attr);
  return const_op.getResult();
}

// Create a 32-bit TOSA TABLE constant tensor
// Output is restricted to [-1.0, 1.0] as s0.31 format
void getTosaConst32bitTable(PatternRewriter& rewriter, Operation* op,
                            double input_scale, int32_t input_zp,
                            std::function<double(double)> func,
                            Value& upper_const, Value& lower_const) {
  std::array<int16_t, 513> upper_table_array, lower_table_array;

  double output_inv_scale = static_cast<double>(1L << 31);

  for (int32_t i = -256; i <= 256; i++) {
    double dequantized = input_scale * (i - input_zp);
    double transformed = func(dequantized);
    double truncated = std::min(std::max(transformed, -1.0), 1.0);
    int64_t rescaled =
        static_cast<int64_t>(std::round(truncated * output_inv_scale));

    // 2^31 is not representable in int32_t, so store as 2^31 - 1 instead
    if (rescaled == static_cast<int64_t>(1L << 31)) {
      rescaled = static_cast<int64_t>(1L << 31) - 1;
    }

    int32_t upper = (rescaled >> 16) & 0xFFFF;
    // TABLE output is signed 16 bits with range [-32768, 32767]
    // Lower 16 bits are unsigned and ranges [0, 65536]
    // Need to adjust value with offset 0x8000 in table generation
    // Legalization should add this back before recovering 32-bit value
    int32_t lower = (rescaled & 0xFFFF) - 0x8000;

    upper_table_array[i + 256] = upper;
    lower_table_array[i + 256] = lower;
  }

  auto element_qtype =
      UniformQuantizedType::get(true, rewriter.getIntegerType(16),
                                rewriter.getF32Type(), 1.0f, 0, -32768, 32767);
  auto const_type = RankedTensorType::get({513}, element_qtype);
  auto storage_type =
      RankedTensorType::get({513}, element_qtype.getStorageType());

  auto upper_const_attr = DenseElementsAttr::get(
      storage_type, llvm::makeArrayRef<int16_t>(upper_table_array));
  auto lower_const_attr = DenseElementsAttr::get(
      storage_type, llvm::makeArrayRef<int16_t>(lower_table_array));

  upper_const =
      rewriter.create<tosa::ConstOp>(op->getLoc(), const_type, upper_const_attr)
          .getResult();
  lower_const =
      rewriter.create<tosa::ConstOp>(op->getLoc(), const_type, lower_const_attr)
          .getResult();
}

// Create a 32-bit float constant operator from a float
Value getTosaConstTensorSingleF32(PatternRewriter& rewriter, Operation* op,
                                  float val) {
  auto const_type = RankedTensorType::get({}, rewriter.getF32Type());
  auto const_attr = DenseElementsAttr::get(const_type, val);

  auto const_op =
      rewriter.create<tosa::ConstOp>(op->getLoc(), const_type, const_attr);
  return const_op.getResult();
}

// Create a 32-bit integer constant operator from an int
Value getTosaConstTensorSingleI32(PatternRewriter& rewriter, Operation* op,
                                  int32_t val) {
  auto const_type = RankedTensorType::get({}, rewriter.getIntegerType(32));
  auto const_attr = DenseElementsAttr::get(const_type, val);

  auto const_op =
      rewriter.create<tosa::ConstOp>(op->getLoc(), const_type, const_attr);
  return const_op.getResult();
}

// Create a vector from a 32-bit value tensor.  Returns the size of
// the new vector or -1 on error.
int getVectorFromValue32(Value val, llvm::SmallVector<int32_t, 4>& vec) {
  int i = 0;

  ElementsAttr elems;

  if (!matchPattern(val, m_Constant(&elems))) return -1;

  for (auto idx : elems.getValues<IntegerAttr>()) {
    vec.push_back(idx.getInt());
    i++;
  }

  return i;
}

// Calculates the TOSA padding values based on TF operators padded with
// SAME/VALID.
//
// This could pass tensorflow::FilterTensorFormat and do
// GetFilterTensorSpatialDimIndex but the current TF core libs do not support
// FORMAT_OHWI parsing by that function in core/util/tensor_format.h
bool getPaddingValuesFromPadType(
    tensorflow::Padding tf_pad, tensorflow::TensorFormat data_format_tf,
    uint32_t first_filter_spatial_dim, RankedTensorType input_type,
    RankedTensorType filter_type, ArrayAttr strides, ArrayAttr dilations,
    PatternRewriter& rewriter, ArrayAttr& explicit_padding) {
  assert(tf_pad != tensorflow::Padding::EXPLICIT);

  // Storing the numeric padding values is useful for TOSA codegen, as opposed
  // to holding the padding regime mnemonic, i.e. SAME, VALID, FULL, ...
  SmallVector<int64_t, 4> computed_paddings;

  int64_t pad_before, pad_after;
  for (int i = 0; i < 2; i++) {  // Two spatial dimensions X&Y
    int64_t ifm_dim = GetTensorSpatialDimIndex(
        4, data_format_tf, i);  // 4D tensor, NHWC/NCHW format
    int64_t filter_dim = first_filter_spatial_dim + i;

    int64_t dim_dilation = dilations[i].template cast<IntegerAttr>().getInt();
    int64_t dim_stride = strides[i].template cast<IntegerAttr>().getInt();

    tensorflow::int64 op_size, pad_before_tf,
        pad_after_tf;  // Complains if using int64_T
    tensorflow::Status status = tensorflow::GetWindowedOutputSizeVerboseV2(
        input_type.getDimSize(ifm_dim), filter_type.getDimSize(filter_dim),
        dim_dilation, dim_stride, tf_pad, &op_size, &pad_before_tf,
        &pad_after_tf);
    if (!status.ok()) return false;

    pad_before = pad_before_tf;
    pad_after = pad_after_tf;
    computed_paddings.push_back(pad_before);
    computed_paddings.push_back(pad_after);
  }

  explicit_padding = rewriter.getI64ArrayAttr(computed_paddings);
  return true;
}

// Calculates the TOSA padding values for explicit-padded TF operators.
//
// This function only handles the TF padding array explicit_padding, which is
// only present in certain TF ops. All others encode padding using the string
// SAME/VALID, which is interpreted using the getPaddingValuesFromPadString
// function below.

// The explicit padding array in TF holds 2 pad values for every
// dimension, even those that are not the 2 spatial ones. Just extract the
// 2x pad values for the XY dims.
ArrayAttr getPaddingValuesFromExplicitPadAttr(
    ArrayAttr explicit_pad, tensorflow::TensorFormat data_format_tf,
    PatternRewriter& rewriter) {
  SmallVector<int64_t, 4> computed_paddings;

  int64_t pad_before, pad_after;
  for (int i = 0; i < 2; i++) {  // Two spatial dimensions X&Y
    int64_t dim = GetTensorSpatialDimIndex(4, data_format_tf,
                                           i);  // 4D tensor, NHWC/NCHW format

    pad_before = explicit_pad[dim * 2].template cast<IntegerAttr>().getInt();
    pad_after = explicit_pad[dim * 2 + 1].template cast<IntegerAttr>().getInt();
    computed_paddings.push_back(pad_before);
    computed_paddings.push_back(pad_after);
  }

  return rewriter.getI64ArrayAttr(computed_paddings);
}

// Calculates the TOSA padding values for transposeConv2d
bool getTransposeConv2dPaddingValues(
    tensorflow::Padding tf_pad, tensorflow::TensorFormat data_format_tf,
    uint32_t first_filter_spatial_dim, RankedTensorType input_type,
    RankedTensorType filter_type, RankedTensorType output_type,
    ArrayAttr strides, ArrayAttr dilations, PatternRewriter& rewriter,
    ArrayAttr& explicit_padding) {
  assert(tf_pad != tensorflow::Padding::EXPLICIT);

  // Storing the numeric padding values is useful for TOSA codegen, as opposed
  // to holding the padding regime mnemonic, i.e. SAME, VALID, FULL, ...

  SmallVector<int64_t, 2> computed_paddings;

  int64_t pad_before, pad_after;
  for (int i = 0; i < 2; i++) {  // Two spatial dimensions X&Y
    int64_t ifm_dim = GetTensorSpatialDimIndex(
        4, data_format_tf, i);  // 4D tensor, NHWC/NCHW format
    int64_t ofm_dim = GetTensorSpatialDimIndex(
        4, data_format_tf, i);  // 4D tensor, NHWC/NCHW format
    int64_t filter_dim = first_filter_spatial_dim + i;

    int64_t ifm_size = input_type.getDimSize(ifm_dim);
    int64_t filter_size = filter_type.getDimSize(filter_dim);
    int64_t ofm_size = output_type.getDimSize(ofm_dim);
    int64_t dim_dilation = dilations[i].template cast<IntegerAttr>().getInt();
    int64_t dim_stride = strides[i].template cast<IntegerAttr>().getInt();

    int effective_filter_size = (filter_size - 1) * dim_dilation + 1;
    int total_padding =
        ((ifm_size - 1) * dim_stride + effective_filter_size - ofm_size);
    total_padding = total_padding > 0 ? total_padding : 0;

    pad_before = total_padding / 2;
    pad_after = total_padding - pad_before;

    computed_paddings.push_back(pad_before);
  }

  explicit_padding = rewriter.getI64ArrayAttr(computed_paddings);
  return true;
}

// Templated function to create a constant op in a given dialect and with a
// given type.  Specializations below.

// T0: target dialect constant op
// T1: native c++ integer type
template <typename T0, typename T1>
Value get1DConstTensor(PatternRewriter& rewriter, Operation* op,
                       SmallVector<T1, 8> arr) {
  auto const_type =
      RankedTensorType::get({static_cast<int32_t>(arr.size())},
                            rewriter.getIntegerType(sizeof(T1) * 8));
  auto const_attr =
      DenseElementsAttr::get(const_type, llvm::makeArrayRef<T1>(arr));

  auto const_op = rewriter.create<T0>(op->getLoc(), const_type, const_attr);
  return const_op.getResult();
}

// Specialization for Const ops
template <>
Value get1DConstTensor<tosa::ConstOp, float>(PatternRewriter& rewriter,
                                             Operation* op,
                                             SmallVector<float, 8> arr) {
  auto const_type = RankedTensorType::get({static_cast<int32_t>(arr.size())},
                                          rewriter.getF32Type());
  auto const_attr =
      DenseElementsAttr::get(const_type, llvm::makeArrayRef<float>(arr));

  auto const_op =
      rewriter.create<tosa::ConstOp>(op->getLoc(), const_type, const_attr);
  return const_op.getResult();
}

template Value get1DConstTensor<tosa::ConstOp, int32_t>(
    PatternRewriter&, Operation*, SmallVector<int32_t, 8> arr);
template Value get1DConstTensor<tosa::ConstOp, int64_t>(
    PatternRewriter&, Operation*, SmallVector<int64_t, 8> arr);
template Value get1DConstTensor<TFL::ConstOp, int32_t>(
    PatternRewriter&, Operation*, SmallVector<int32_t, 8> arr);
template Value get1DConstTensor<TFL::ConstOp, int64_t>(
    PatternRewriter&, Operation*, SmallVector<int64_t, 8> arr);

// Same as get1DConstTensor, but int48 is not native c++ type, needs additional
// interface
Value get1DConstTensorInt48(PatternRewriter& rewriter, Operation* op,
                            ArrayRef<APInt>& arr) {
  auto const_type = RankedTensorType::get({static_cast<int32_t>(arr.size())},
                                          rewriter.getIntegerType(48));
  auto const_attr = DenseElementsAttr::get(const_type, arr);

  auto const_op =
      rewriter.create<tosa::ConstOp>(op->getLoc(), const_type, const_attr);
  return const_op.getResult();
}

static ElementsAttr getDefiningOpConstElementsAttr(Value input) {
  if (!input.getDefiningOp()) {
    return nullptr;
  }
  if (auto qconst_op = dyn_cast<TFL::QConstOp>(input.getDefiningOp())) {
    return qconst_op.value().dyn_cast<ElementsAttr>();
  }
  if (auto tosa_const_op = dyn_cast<tosa::ConstOp>(input.getDefiningOp())) {
    return tosa_const_op.value().dyn_cast<ElementsAttr>();
  }
  return nullptr;
}

// Strip off quantization information for bias tensor and return a unquantized
// bias. This assumes that the input is defined as a constant.
Value getUnquantizedBias(PatternRewriter& rewriter, Operation* op,
                         Value input) {
  auto input_type = input.getType().dyn_cast<mlir::RankedTensorType>();
  assert(input_type && "bias input is not a RankedTensorType");
  auto input_element_type = input_type.getElementType();
  auto input_element_qtype =
      input_element_type.dyn_cast<mlir::quant::QuantizedType>();
  ElementsAttr input_value_attr = getDefiningOpConstElementsAttr(input);

  if (input_element_qtype && input_value_attr) {
    auto output_type = RankedTensorType::get(
        input_type.getShape(),
        rewriter.getIntegerType(
            input_element_qtype.getStorageTypeIntegralWidth()));
    auto const_op = rewriter.create<tosa::ConstOp>(op->getLoc(), output_type,
                                                   input_value_attr);
    return const_op.getResult();
  }

  return input;
}

// Check if scale32 mode is used for given output_element_type
bool isScale32(mlir::quant::UniformQuantizedType output_element_type) {
  if (output_element_type.getStorageTypeIntegralWidth() == 8)
    return true;
  else
    return false;
}

}  // namespace tosa
}  // namespace mlir
