/**
 * Copyright (c) 2017-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "glow/Importer/ONNXIFILoader.h"

#include "onnx/onnx.pb.h"

namespace glow {
namespace onnxifi {

/// Creates tensor \p T from the input \p in. Note, there is no data associated
/// with the Tensor. This method makes sure that the tensor is created with the
/// proper shape and element type.
static void setTensorType(const ONNX_NAMESPACE::TypeProto &in, Tensor *T) {
  std::vector<size_t> dim;
  for (auto d : in.tensor_type().shape().dim()) {
    dim.push_back(d.dim_value());
  }

  if (in.tensor_type().elem_type() == ONNX_NAMESPACE::TensorProto::FLOAT) {
    T->reset(ElemKind::FloatTy, dim);
  } else if (in.tensor_type().elem_type() ==
             ONNX_NAMESPACE::TensorProto::INT64) {
    // TODO: either switch IndexTy to be 64 bit, or switch to another type here.
    T->reset(ElemKind::IndexTy, dim);
  } else {
    assert(false && "Only float and index tensors are supported");
  }
}

void ModelLoader::loadInputs(ONNX_NAMESPACE::GraphProto &net) {
  for (const auto &in : net.input()) {
    Tensor *T = new Tensor();
    setTensorType(in.type(), T);
    auto *var =
        createAndRememberVariable(in.name(), *T, VisibilityKind::Public);
    onnxNameToInputVars_.try_emplace(in.name(), var);
  }
}

/// Loads tensor \p T from the input \p in.
static bool loadWeight(const onnxTensorDescriptorV1 &in, Tensor *T) {
  // Only support CPU memory tensors.
  if (in.memoryType != ONNXIFI_MEMORY_TYPE_CPU) {
    return false;
  }

  std::vector<size_t> dims;
  for (unsigned i = 0; i < in.dimensions; ++i) {
    dims.push_back(in.shape[i]);
  }

  if (in.dataType == ONNXIFI_DATATYPE_FLOAT32) {
    T->reset(ElemKind::FloatTy, dims);

    auto TH = T->getHandle<>();
    float *data = (float *)in.buffer;
    for (size_t i = 0; i < TH.size(); ++i) {
      TH.raw(i) = data[i];
    }
  } else if (in.dataType == ONNXIFI_DATATYPE_UINT64) {
    // TODO: either switch IndexTy to be 64 bit, or switch to another type here.
    T->reset(ElemKind::IndexTy, dims);

    auto TH = T->getHandle<size_t>();
    int64_t *data = (int64_t *)in.buffer;
    for (size_t i = 0; i < TH.size(); ++i) {
      TH.raw(i) = data[i];
    }
  } else {
    llvm_unreachable("Only float and index tensors are supported");
  }

  return true;
}

bool ModelLoader::loadWeights(uint32_t weightsCount,
                              const onnxTensorDescriptorV1 *weightDescriptors) {
  for (uint32_t i = 0; i < weightsCount; ++i) {
    Tensor *T = new Tensor();

    if (!loadWeight(weightDescriptors[i], T)) {
      return false;
    }

    tensors_[weightDescriptors[i].name] = T;
  }

  return true;
}

std::unique_ptr<ModelLoader> ModelLoader::parse(
    const void *onnxModel, uint32_t onnxModelSize, uint32_t weightsCount,
    const onnxTensorDescriptorV1 *weightDescriptors, Function &F) {
  std::unique_ptr<ModelLoader> loader(new ModelLoader(F));

  ONNX_NAMESPACE::ModelProto modelDef;
  if (!loader->loadProto(modelDef, onnxModel, onnxModelSize)) {
    return nullptr;
  }
  loader->setVersion(modelDef);

  ONNX_NAMESPACE::GraphProto graphDef = modelDef.graph();
  loader->loadInputs(graphDef);

  if (!loader->loadWeights(weightsCount, weightDescriptors)) {
    return nullptr;
  }

  if (!loader->loadNetwork(graphDef)) {
    return nullptr;
  }

  if (!loader->setOutputNodes(graphDef)) {
    return nullptr;
  }

  return loader;
}

std::unique_ptr<std::pair<Kinded::Kind, ElemKind>>
ModelLoader::parseOperator(const void *onnxModel, size_t onnxModelSize) {

  ONNX_NAMESPACE::ModelProto modelDef;
  if (ONNXModelLoader::loadProto(modelDef, onnxModel, onnxModelSize)) {
    return nullptr;
  }

  ONNX_NAMESPACE::GraphProto graph = modelDef.graph();

  // Only single operator is allowed to be in the onnxModel.
  if (graph.node_size() != 1) {
    return nullptr;
  }

  std::unique_ptr<std::pair<Kinded::Kind, ElemKind>> result;
  const std::string &operation = graph.node(0).op_type();

  // Quantized and non-quantized operations are handled by
  // different ONNX operators, for now only handle fp32.
  // TODO: Add more operators.
  if (operation == "conv") {
    result.reset(new std::pair<Kinded::Kind, ElemKind>(
        Kinded::Kind::ConvolutionNodeKind, ElemKind::FloatTy));
  } else if (operation == "Relu") {
    result.reset(new std::pair<Kinded::Kind, ElemKind>(
        Kinded::Kind::ReluNodeKind, ElemKind::FloatTy));
  } else if (operation == "Softmax") {
    result.reset(new std::pair<Kinded::Kind, ElemKind>(
        Kinded::Kind::SoftMaxNodeKind, ElemKind::FloatTy));
  }

  return result;
}

} // namespace onnxifi
} // namespace glow
