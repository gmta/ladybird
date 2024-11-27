/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/Selection/Selection.h>

namespace Web::Editing {

// https://w3c.github.io/editing/docs/execCommand/#record-the-values
struct RecordedNodeValue {
    DOM::Node& node;
    FlyString const& command;
    Optional<String> specified_command_value;
};

// Below algorithms are specified here:
// https://w3c.github.io/editing/docs/execCommand/#assorted-common-algorithms

String canonical_space_sequence(u32 length, bool non_breaking_start, bool non_breaking_end);
void canonicalize_whitespace(DOM::Node&, u32 offset, bool fix_collapsed_space = true);
void delete_the_selection(Selection::Selection const&);
DOM::Node const* editing_host_of_node(DOM::Node const&);
bool follows_a_line_break(DOM::Node const*);
bool is_allowed_child_of_node(DOM::Node const&, Variant<DOM::Node*, String> parent);
bool is_block_boundary_point(DOM::Node const&, u32 offset);
bool is_block_end_point(DOM::Node const&, u32 offset);
bool is_block_node(DOM::Node const&);
bool is_block_start_point(DOM::Node const&, u32 offset);
bool is_collapsed_whitespace_node(DOM::Node const&);
bool is_editing_host(DOM::Node const&);
bool is_extraneous_line_break(DOM::Node const&);
bool is_in_same_editing_host(DOM::Node const&, DOM::Node const&);
bool is_inline_node(DOM::Node const&);
bool is_invisible_node(DOM::Node const&);
bool is_visible_node(DOM::Node const&);
bool is_whitespace_node(DOM::Node const&);
void move_node_preserving_ranges(DOM::Node&, DOM::Node& new_parent, u32 new_index);
void normalize_sublists_in_node(DOM::Node&);
bool precedes_a_line_break(DOM::Node const*);
Vector<RecordedNodeValue> record_the_values_of_nodes(Vector<DOM::Node*> const&);
void remove_extraneous_line_breaks_at_the_end_of_node(DOM::Node&);
void remove_extraneous_line_breaks_before_node(DOM::Node&);
void remove_extraneous_line_breaks_from_a_node(DOM::Node&);
void remove_node_preserving_its_descendants(DOM::Node&);
void restore_the_values_of_nodes(Vector<RecordedNodeValue> const&);
Optional<String> specified_command_value(DOM::Element const&, FlyString const& command);
void split_the_parent_of_nodes(Vector<DOM::Node*> const&);

}
