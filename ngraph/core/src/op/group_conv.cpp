//*****************************************************************************
// Copyright 2017-2020 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************

#include <numeric>

#include "ngraph/attribute_visitor.hpp"
#include "ngraph/builder/reshape.hpp"
#include "ngraph/builder/split.hpp"
#include "ngraph/op/concat.hpp"
#include "ngraph/op/convolution.hpp"
#include "ngraph/op/group_conv.hpp"
#include "ngraph/op/reshape.hpp"
#include "ngraph/op/slice.hpp"
#include "ngraph/validation_util.hpp"

using namespace std;
using namespace ngraph;

NGRAPH_SUPPRESS_DEPRECATED_START

//------------------------------------------------------------------------------
//                        v1::GroupConvolution
//------------------------------------------------------------------------------

NGRAPH_RTTI_DEFINITION(op::v1::GroupConvolution, "GroupConvolution", 1);

shared_ptr<Node> op::v1::GroupConvolution::get_default_value() const
{
    return op::Constant::create(get_element_type(), get_shape(), {0});
}

op::v1::GroupConvolution::GroupConvolution(const Output<Node>& data_batch,
                                           const Output<Node>& filters,
                                           const Strides& strides,
                                           const CoordinateDiff& pads_begin,
                                           const CoordinateDiff& pads_end,
                                           const Strides& dilations,
                                           const PadType& auto_pad)
    : Op({data_batch, filters})
    , m_strides(strides)
    , m_dilations(dilations)
    , m_pads_begin(pads_begin)
    , m_pads_end(pads_end)
    , m_auto_pad(auto_pad)
{
    constructor_validate_and_infer_types();
}

bool ngraph::op::v1::GroupConvolution::visit_attributes(AttributeVisitor& visitor)
{
    visitor.on_attribute("strides", m_strides);
    visitor.on_attribute("pads_begin", m_pads_begin);
    visitor.on_attribute("pads_end", m_pads_end);
    visitor.on_attribute("dilations", m_dilations);
    visitor.on_attribute("auto_pad", m_auto_pad);
    return true;
}

void op::v1::GroupConvolution::validate_and_infer_types()
{
    PartialShape data_batch_shape = get_input_partial_shape(0);
    PartialShape filters_shape = get_input_partial_shape(1);
    element::Type data_batch_et = get_input_element_type(0);
    element::Type filters_et = get_input_element_type(1);

    element::Type result_et;

    NODE_VALIDATION_CHECK(
        this,
        element::Type::merge(result_et, data_batch_et, filters_et),
        "Element types for data batch and filters do not match (data batch element type: ",
        data_batch_et,
        ", filters element type: ",
        filters_et,
        ").");

    PartialShape result_shape{PartialShape::dynamic()};

    if (data_batch_shape.rank().is_static())
    {
        result_shape =
            std::vector<Dimension>(data_batch_shape.rank().get_length(), Dimension::dynamic());
        result_shape[0] = data_batch_shape[0];
    }

    Dimension groups(1);
    // we need to adjust filters_shape to reuse helpers for normal convolution
    if (filters_shape.rank().is_static() && filters_shape.rank().get_length() > 2)
    {
        groups = filters_shape[0];
        filters_shape[1] *= groups;
        auto dim_vec = static_cast<std::vector<Dimension>>(filters_shape);
        dim_vec.erase(dim_vec.begin());
        filters_shape = PartialShape(dim_vec);
        if (data_batch_shape.rank().is_static())
        {
            result_shape[1] = filters_shape[0];
        }
    }

    if (data_batch_shape.rank().is_static() && data_batch_shape.rank().get_length() > 2 &&
        data_batch_shape[1].is_static() && groups.is_static())
    {
        data_batch_shape[1] = Dimension(data_batch_shape[1].get_length() / groups.get_length());
    }

    if (m_strides.size() == 0)
    {
        m_strides = conv_default_strides(this, data_batch_shape, filters_shape);
    }

    if (m_dilations.size() == 0)
    {
        m_dilations = conv_default_strides(this, data_batch_shape, filters_shape);
    }

    if (m_pads_begin.size() == 0 || m_auto_pad == PadType::VALID)
    {
        m_pads_begin = conv_default_padding(this, data_batch_shape, filters_shape);
    }

    if (m_pads_end.size() == 0 || m_auto_pad == PadType::VALID)
    {
        m_pads_end = conv_default_padding(this, data_batch_shape, filters_shape);
    }

    if (m_auto_pad == PadType::SAME_UPPER || m_auto_pad == PadType::SAME_LOWER)
    {
        bool auto_padding_applied = false;
        if (filters_shape.is_static())
        {
            m_pads_begin.clear();
            m_pads_end.clear();
            auto filters_static_shape = filters_shape.to_shape();
            filters_static_shape.erase(filters_static_shape.begin(),
                                       filters_static_shape.begin() + 2); // Remove {O,I}
            auto_padding_applied = try_apply_auto_padding(data_batch_shape,
                                                          filters_static_shape,
                                                          m_strides,
                                                          m_dilations,
                                                          m_auto_pad,
                                                          m_pads_end,
                                                          m_pads_begin);
        }
        if (!auto_padding_applied)
        {
            set_output_type(0, result_et, result_shape);
            return;
        }
    }

    result_shape = infer_convolution_forward(this,
                                             data_batch_shape,
                                             Strides(m_strides.size(), 1), // dummy data dilations
                                             m_pads_begin,
                                             m_pads_end,
                                             filters_shape,
                                             m_strides,
                                             m_dilations);

    set_output_type(0, result_et, result_shape);
}

shared_ptr<Node> op::v1::GroupConvolution::clone_with_new_inputs(const OutputVector& new_args) const
{
    check_new_args_count(this, new_args);
    return make_shared<v1::GroupConvolution>(new_args.at(0),
                                             new_args.at(1),
                                             m_strides,
                                             m_pads_begin,
                                             m_pads_end,
                                             m_dilations,
                                             m_auto_pad);
}

//------------------------------------------------------------------------------
//                        v1::GroupConvolutionBackpropData
//------------------------------------------------------------------------------

constexpr NodeTypeInfo op::v1::GroupConvolutionBackpropData::type_info;

op::v1::GroupConvolutionBackpropData::GroupConvolutionBackpropData(
    const Output<Node>& data,
    const Output<Node>& filters,
    const Output<Node>& output_shape,
    const Strides& strides,
    const CoordinateDiff& pads_begin,
    const CoordinateDiff& pads_end,
    const Strides& dilations,
    const PadType& auto_pad,
    const CoordinateDiff& output_padding)
    : FusedOp({data, filters, output_shape})
    , m_strides(strides)
    , m_dilations(dilations)
    , m_pads_begin(pads_begin)
    , m_pads_end(pads_end)
    , m_auto_pad(auto_pad)
    , m_output_padding(output_padding)
{
    constructor_validate_and_infer_types();
}

op::v1::GroupConvolutionBackpropData::GroupConvolutionBackpropData(
    const Output<Node>& data,
    const Output<Node>& filters,
    const Output<Node>& output_shape,
    const Strides& strides,
    const Strides& dilations,
    const PadType& auto_pad,
    const CoordinateDiff& output_padding)
    : GroupConvolutionBackpropData(data,
                                   filters,
                                   output_shape,
                                   strides,
                                   CoordinateDiff(),
                                   CoordinateDiff(),
                                   dilations,
                                   auto_pad,
                                   output_padding)
{
}

op::v1::GroupConvolutionBackpropData::GroupConvolutionBackpropData(
    const Output<Node>& data,
    const Output<Node>& filters,
    const Strides& strides,
    const CoordinateDiff& pads_begin,
    const CoordinateDiff& pads_end,
    const Strides& dilations,
    const PadType& auto_pad,
    const CoordinateDiff& output_padding)
    : FusedOp({data, filters})
    , m_strides(strides)
    , m_dilations(dilations)
    , m_pads_begin(pads_begin)
    , m_pads_end(pads_end)
    , m_auto_pad(auto_pad)
    , m_output_padding(output_padding)
{
    constructor_validate_and_infer_types();
}

bool ngraph::op::v1::GroupConvolutionBackpropData::visit_attributes(AttributeVisitor& visitor)
{
    visitor.on_attribute("strides", m_strides);
    visitor.on_attribute("pads_begin", m_pads_begin);
    visitor.on_attribute("pads_end", m_pads_end);
    visitor.on_attribute("dilations", m_dilations);
    visitor.on_attribute("auto_pad", m_auto_pad);
    visitor.on_attribute("output_padding", m_output_padding);
    return true;
}

bool op::v1::GroupConvolutionBackpropData::is_dynamic() const
{
    bool is_dynamic = Node::is_dynamic();
    if (inputs().size() == 3 && !is_dynamic)
    {
        return !is_type<op::Constant>(input_value(2).get_node());
    }
    return is_dynamic;
}

const PartialShape op::v1::GroupConvolutionBackpropData::get_convolution_output_shape() const
{
    auto data_pshape = get_input_partial_shape(0);

    PartialShape shape;
    if (data_pshape.rank().is_static())
    {
        shape = PartialShape{vector<Dimension>(data_pshape.rank().get_length() - 2)};
    }
    else
    {
        shape = PartialShape{vector<Dimension>(m_strides.size())};
    }
    bool is_output_shape_present = inputs().size() == 3;
    if (is_output_shape_present)
    {
        if (auto const_op = as_type<op::Constant>(input_value(2).get_node()))
        {
            shape = const_op->get_shape_val();
        }
        else
        {
            shape = PartialShape::dynamic();
        }
    }
    return shape;
}

void op::v1::GroupConvolutionBackpropData::set_output_shape(const Shape& shape)
{
    this->input(2).replace_source_output(
        op::Constant::create(this->get_input_element_type(2), Shape{shape.size()}, shape)
            ->output(0));
}

void op::v1::GroupConvolutionBackpropData::infer_conv_backprop_output_spatial_shape(
    const vector<Dimension>& input_data_shape,
    const vector<Dimension>& filters_shape,
    const Strides& strides,
    const Strides& dilations,
    const CoordinateDiff& pads_begin,
    const CoordinateDiff& pads_end,
    const CoordinateDiff& output_padding,
    vector<Dimension>& output_spatial_shape)
{
    size_t num_spatial_dims = input_data_shape.size();
    NGRAPH_CHECK(filters_shape.size() == num_spatial_dims && strides.size() == num_spatial_dims &&
                 dilations.size() == num_spatial_dims && pads_begin.size() == num_spatial_dims &&
                 pads_end.size() == num_spatial_dims && output_padding.size() == num_spatial_dims);

    for (size_t i = 0; i < num_spatial_dims; ++i)
    {
        if (input_data_shape[i].is_static() && filters_shape[i].is_static())
        {
            int64_t val = strides[i] * (input_data_shape[i].get_length() - 1) +
                          dilations[i] * (filters_shape[i].get_length() - 1) + 1 - pads_begin[i] -
                          pads_end[i] + output_padding[i];
            output_spatial_shape.push_back(val);
        }
        else
        {
            output_spatial_shape.push_back(Dimension::dynamic());
        }
    }
}

void op::v1::GroupConvolutionBackpropData::pre_validate_and_infer_types()
{
    const auto& data_pshape = get_input_partial_shape(0);
    element::Type data_et = get_input_element_type(0);

    const auto& filters_pshape = get_input_partial_shape(1);
    element::Type filters_et = get_input_element_type(1);

    element::Type result_et;
    NODE_VALIDATION_CHECK(
        this,
        element::Type::merge(result_et, data_et, filters_et),
        "Element types for data batch and filters do not match (data batch element type: ",
        data_et,
        ", filters element type: ",
        filters_et,
        ").");

    if (data_pshape.rank().is_static() && filters_pshape.rank().is_static())
    {
        if (filters_pshape[0].is_static() && filters_pshape[1].is_static() &&
            data_pshape[1].is_static())
        {
            auto groups = filters_pshape[0].get_length();
            auto input_channels = filters_pshape[1].get_length();
            auto n_data_channels = data_pshape[1].get_length();

            NODE_VALIDATION_CHECK(this,
                                  n_data_channels % groups == 0,
                                  "Number of data channels not a multiple of group size.");
            NODE_VALIDATION_CHECK(this,
                                  n_data_channels / groups == input_channels,
                                  "Data second dimension has incompatible value "
                                  "with number of input channels.");
        }

        if (m_pads_begin.size() == 0)
        {
            m_pads_begin = conv_default_padding(this, data_pshape, filters_pshape);
        }
        if (m_pads_end.size() == 0)
        {
            m_pads_end = conv_default_padding(this, data_pshape, filters_pshape);
        }
        if (m_output_padding.size() == 0)
        {
            m_output_padding = conv_default_padding(this, data_pshape, filters_pshape);
        }
        if (m_strides.size() == 0)
        {
            m_strides = conv_default_strides(this, data_pshape, filters_pshape);
        }
        if (m_dilations.size() == 0)
        {
            m_dilations = conv_default_strides(this, data_pshape, filters_pshape);
        }

        const auto num_spatial_dims = data_pshape.rank().get_length() - 2;

        NODE_VALIDATION_CHECK(this,
                              m_strides.size() == num_spatial_dims,
                              "Strides should be defined for all and only spatial features.");

        NODE_VALIDATION_CHECK(this,
                              m_dilations.size() == num_spatial_dims,
                              "Dilations should be defined for all and only spatial features.");

        NODE_VALIDATION_CHECK(this,
                              m_output_padding.size() == num_spatial_dims,
                              "Output padding should be defined for all and only "
                              "spatial features.");
    }

    bool is_output_shape_present = inputs().size() == 3;
    PartialShape output_pshape;

    // If output shape is provided, ignore current values for padding begin/end
    // and infer them.
    if (is_output_shape_present)
    {
        output_pshape = get_convolution_output_shape();

        if (output_pshape.is_static() && data_pshape.is_static() && filters_pshape.is_static())
        {
            Shape output_shape = output_pshape.to_shape();
            const Shape& data_shape = data_pshape.to_shape();
            const Shape& filters_shape = filters_pshape.to_shape();
            const size_t num_spatial_dims = data_shape.size() - 2;
            NODE_VALIDATION_CHECK(this,
                                  output_shape.size() == num_spatial_dims,
                                  "Output shape should be specified only and for "
                                  "all spatial dimensions.");

            // If auto_pad has one of following mode we infer paddings. Otherwise in
            // EXPLICIT auto_pad mode we use what is provided.
            if (m_auto_pad == PadType::SAME_UPPER || m_auto_pad == PadType::SAME_LOWER)
            {
                opset1::infer_conv_backprop_auto_padding(
                    Shape{std::next(data_shape.begin(), 2), std::end(data_shape)},
                    Shape{std::next(filters_shape.begin(), 3), std::end(filters_shape)},
                    output_shape,
                    m_strides,
                    m_dilations,
                    m_auto_pad,
                    m_output_padding,
                    m_pads_begin,
                    m_pads_end);
            }

            // GROUP * C_OUTPUT
            output_shape.insert(output_shape.begin(), filters_shape.at(0) * filters_shape.at(2));
            // N
            output_shape.insert(output_shape.begin(), data_shape.at(0));
            output_pshape = output_shape;
        }
        set_input_is_relevant_to_shape(2);
    }
    // Deduce output shape from input spatial shape, strides, dilations, output padding
    // and padding values.
    else
    {
        if (m_auto_pad == PadType::SAME_UPPER || m_auto_pad == PadType::SAME_LOWER ||
            m_auto_pad == PadType::VALID)
        {
            m_pads_begin.assign(m_pads_begin.size(), 0);
            m_pads_end.assign(m_pads_end.size(), 0);
        }

        if (data_pshape.rank().is_static() && filters_pshape.is_static())
        {
            vector<Dimension> data_shape{data_pshape}, filters_shape{filters_pshape}, output_shape;

            infer_conv_backprop_output_spatial_shape(
                vector<Dimension>{std::next(data_shape.begin(), 2), std::end(data_shape)},
                vector<Dimension>{std::next(filters_shape.begin(), 3), std::end(filters_shape)},
                m_strides,
                m_dilations,
                m_pads_begin,
                m_pads_end,
                m_output_padding,
                output_shape);

            // GROUP * C_OUTPUT
            output_shape.insert(output_shape.begin(), filters_shape.at(0) * filters_shape.at(2));
            // N
            output_shape.insert(output_shape.begin(), data_shape.at(0));
            output_pshape = PartialShape{output_shape};
        }
        else
        {
            output_pshape = PartialShape::dynamic(data_pshape.rank());
        }
    }

    set_input_is_relevant_to_shape(0);
    set_input_is_relevant_to_shape(1);
    set_output_type(0, result_et, output_pshape);
}

OutputVector op::v1::GroupConvolutionBackpropData::decompose_op() const
{
    auto data = input_value(0);
    auto filters = input_value(1);
    NodeVector conv_groups;

    auto groups = filters.get_shape()[0];
    // slice data
    OutputVector sliced_data = builder::split(data, groups, 1);
    // slice filters
    OutputVector sliced_filters = builder::split(filters, groups, 0);
    // We have to squeeze first empty dimension (groups).
    std::transform(
        std::begin(sliced_filters),
        std::end(sliced_filters),
        std::begin(sliced_filters),
        [](const Output<Node>& n) -> Output<Node> { return builder::opset1::squeeze(n); });

    for (auto i = 0; i < groups; ++i)
    {
        if (input_values().size() == 3)
        {
            conv_groups.push_back(
                std::make_shared<op::v1::ConvolutionBackpropData>(sliced_data[i],
                                                                  sliced_filters[i],
                                                                  input_value(2),
                                                                  m_strides,
                                                                  m_pads_begin,
                                                                  m_pads_end,
                                                                  m_dilations,
                                                                  m_auto_pad,
                                                                  m_output_padding));
        }
        else
        {
            conv_groups.push_back(
                std::make_shared<op::v1::ConvolutionBackpropData>(sliced_data[i],
                                                                  sliced_filters[i],
                                                                  m_strides,
                                                                  m_pads_begin,
                                                                  m_pads_end,
                                                                  m_dilations,
                                                                  m_auto_pad,
                                                                  m_output_padding));
        }
    }

    size_t concatenation_axis = 1;
    return {std::make_shared<ngraph::op::Concat>(conv_groups, concatenation_axis)};
}

shared_ptr<Node>
    op::v1::GroupConvolutionBackpropData::clone_with_new_inputs(const OutputVector& new_args) const
{
    check_new_args_count(this, new_args);
    if (new_args.size() == 3)
    {
        return make_shared<v1::GroupConvolutionBackpropData>(new_args.at(0),
                                                             new_args.at(1),
                                                             new_args.at(2),
                                                             m_strides,
                                                             m_pads_begin,
                                                             m_pads_end,
                                                             m_dilations,
                                                             m_auto_pad,
                                                             m_output_padding);
    }
    else
    {
        return make_shared<v1::GroupConvolutionBackpropData>(new_args.at(0),
                                                             new_args.at(1),
                                                             m_strides,
                                                             m_pads_begin,
                                                             m_pads_end,
                                                             m_dilations,
                                                             m_auto_pad,
                                                             m_output_padding);
    }
}
