// Copyright (C) 2020 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include "ngraph/node.hpp"

#include <memory>

namespace vpu {

void dynamicToStaticShapeUnsqueeze(std::shared_ptr<ngraph::Node> target);

}  // namespace vpu
