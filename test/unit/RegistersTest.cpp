/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <array>
#include <gtest/gtest.h>

#include "DexInstruction.h"

static constexpr int kMaxSources = 5;

static void test_opcode(DexOpcode opcode) {
  DexInstruction insn(opcode);
  const std::string text = std::string("for opcode ") + show(opcode);
  const size_t src_count = insn.srcs_size();
  const bool has_dest = (insn.dests_size() > 0);
  const int dest_width = has_dest ? insn.dest_bit_width() : 0;
  const bool dest_is_src0 = insn.dest_is_src();

  // Populate source test values
  // We want to ensure that setting registers don't stomp each other
  // Create a unique bit pattern for each source based on its idx
  uint16_t dest_value = (1U << dest_width) - 1;
  uint16_t src_values[kMaxSources];
  for (int src_idx = 0; src_idx < src_count; src_idx++) {
    int src_width = insn.src_bit_width(src_idx);
    EXPECT_GE(src_width, 0) << text;
    uint16_t bits = (src_idx + 5);
    bits |= (bits << 4);
    bits |= (bits << 8);
    bits &= ((1U << src_width) - 1);
    src_values[src_idx] = bits;
  }

  // Set test values, and ensure nothing stomps anything else
  if (has_dest) {
    EXPECT_GE(dest_width, 0) << text;
    insn.set_dest(dest_value);
  }
  for (int i = 0; i < src_count; i++) {
    insn.set_src(i, src_values[i]);
  }
  // ensure nothing was stomped, except for what we expect to be stomped
  if (has_dest) {
    EXPECT_EQ(insn.dest(), dest_is_src0 ? src_values[0] : dest_value) << text;
  }
  for (int i = 0; i < src_count; i++) {
    EXPECT_EQ(insn.src(i), src_values[i]) << text;
  }

  // Ensure we can successfully set and then get the min and max register value
  if (has_dest) {
    uint16_t max = (1U << dest_width) - 1;
    insn.set_dest(0);
    EXPECT_EQ(insn.dest(), 0) << text;
    insn.set_dest(max);
    EXPECT_EQ(insn.dest(), max) << text;
  }
  for (int i = 0; i < src_count; i++) {
    uint16_t max = (1U << insn.src_bit_width(i)) - 1;
    insn.set_src(i, 0);
    EXPECT_EQ(insn.src(i), 0) << text;
    insn.set_src(i, max);
    EXPECT_EQ(insn.src(i), max) << text;
  }
}

TEST(Registers, RoundTrip) {
  for (DexOpcode op : all_opcodes) {
    test_opcode(op);
  }
}
