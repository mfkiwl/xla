/* Copyright 2024 The OpenXLA Authors.

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

#ifndef XLA_SERVICE_GPU_MODEL_INDEXING_MAP_SERIALIZATION_H_
#define XLA_SERVICE_GPU_MODEL_INDEXING_MAP_SERIALIZATION_H_

#include <optional>
#include <ostream>
#include <string>

#include "absl/types/span.h"
#include "llvm/ADT/StringRef.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/MLIRContext.h"
#include "xla/service/gpu/model/indexing_map.h"

namespace xla {
namespace gpu {

// Parses the given string into an IndexingMap.
std::optional<IndexingMap> ParseIndexingMap(llvm::StringRef input,
                                            mlir::MLIRContext* context);

// Prints AffineExpr using the default (d0, d1, ..., s0, s1, ...) variable
// names.
std::string ToString(mlir::AffineExpr affine_expr);

// Prints AffineExpr using the provided variable names.
std::string ToString(mlir::AffineExpr affine_expr,
                     absl::Span<const std::string> dim_names,
                     absl::Span<const std::string> symbol_names);

std::ostream& operator<<(std::ostream& out, mlir::AffineExpr affine_expr);

// Prints AffineMap using the default (d0, d1, ..., s0, s1, ...) variable names.
std::string ToString(mlir::AffineMap affine_map);

// Prints AffineMap using the provided variable names.
std::string ToString(mlir::AffineMap affine_map,
                     absl::Span<const std::string> dim_names,
                     absl::Span<const std::string> symbol_names);

std::ostream& operator<<(std::ostream& out, mlir::AffineMap affine_map);

// Prints IndexingMap using the default (d0, d1, ..., s0, s1, ...) variable
// names.
std::string ToString(const IndexingMap& indexing_map);

// Prints IndexingMap using the provided variable names.
std::string ToString(const IndexingMap& indexing_map,
                     absl::Span<const std::string> dim_names,
                     absl::Span<const std::string> symbol_names);

std::ostream& operator<<(std::ostream& out, const IndexingMap& indexing_map);

}  // namespace gpu
}  // namespace xla

#endif  // XLA_SERVICE_GPU_MODEL_INDEXING_MAP_SERIALIZATION_H_