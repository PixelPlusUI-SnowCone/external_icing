// Copyright (C) 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ICING_TRANSFORM_SIMPLE_NONE_NORMALIZER_FACTORY_H_
#define ICING_TRANSFORM_SIMPLE_NONE_NORMALIZER_FACTORY_H_

#include <memory>
#include <string_view>

#include "icing/text_classifier/lib3/utils/base/statusor.h"
#include "icing/absl_ports/canonical_errors.h"
#include "icing/transform/normalizer.h"
#include "icing/transform/simple/none-normalizer.h"

namespace icing {
namespace lib {

namespace normalizer_factory {

// Creates a dummy normalizer. The term is not normalized, but
// the text will be truncated to max_term_byte_size if it exceeds the max size.
//
// Returns:
//   A normalizer on success
//   INVALID_ARGUMENT if max_term_byte_size <= 0
//   INTERNAL_ERROR on errors
libtextclassifier3::StatusOr<std::unique_ptr<Normalizer>> Create(
    int max_term_byte_size) {
  if (max_term_byte_size <= 0) {
    return absl_ports::InvalidArgumentError(
        "max_term_byte_size must be greater than zero.");
  }

  return std::make_unique<NoneNormalizer>(max_term_byte_size);
}

}  // namespace normalizer_factory

}  // namespace lib
}  // namespace icing

#endif  // ICING_TRANSFORM_SIMPLE_NONE_NORMALIZER_FACTORY_H_
