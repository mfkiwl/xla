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

#include "xla/service/all_gather_combiner.h"

#include <memory>
#include <string>

#include "absl/strings/substitute.h"
#include "xla/hlo/ir/hlo_computation.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_module.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/hlo/utils/hlo_matchers.h"
#include "xla/tests/hlo_test_base.h"
#include "xla/xla_data.pb.h"

namespace xla {
namespace {

using ::testing::Matcher;
namespace op = xla::testing::opcode_matchers;
int64_t kMaxCombineCount = 256;

int64_t AllGatherCount(const HloModule& module) {
  int64_t count = 0;
  for (HloComputation* computation : module.computations()) {
    if (computation->IsFusionComputation()) {
      continue;
    }
    for (HloInstruction* hlo : computation->instructions()) {
      if (hlo->opcode() == HloOpcode::kAllGather) {
        ++count;
      }
    }
  }
  return count;
}

class AllGatherCombinerTest : public HloTestBase,
                              public ::testing::WithParamInterface<bool> {
 protected:
  bool HasSchedule() const { return GetParam(); }
};

INSTANTIATE_TEST_SUITE_P(ParamTests, AllGatherCombinerTest,
                         ::testing::Values(false, true));

// Tests combination of several AllGather instructions.
TEST_P(AllGatherCombinerTest, CombineAllGathers) {
  std::string hlo_string =
      absl::Substitute(R"(
HloModule Module$0

ENTRY entry {
  param0 = f32[32] parameter(0)
  param1 = f32[32] parameter(1)
  allgather0 = f32[128] all-gather(param0), replica_groups={}, dimensions={0}
  allgather1 = f32[128] all-gather(param1), replica_groups={}, dimensions={0}
  ROOT tuple = (f32[128], f32[128]) tuple(allgather0, allgather1)
}
)",
                       HasSchedule() ? ", is_scheduled=true" : "");
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo_string));

  AllGatherCombiner combine(1024 * 1024, kMaxCombineCount);
  ASSERT_EQ(AllGatherCount(*module), 2);
  TF_ASSERT_OK_AND_ASSIGN(bool changed, combine.Run(module.get()));
  EXPECT_TRUE(changed);

  Matcher<const HloInstruction*> combined_all_gather =
      op::AllGather(op::Parameter(0), op::Parameter(1));
  EXPECT_THAT(module->entry_computation()->root_instruction(),
              op::Tuple(op::GetTupleElement(combined_all_gather, 0),
                        op::GetTupleElement(combined_all_gather, 1)));
}

// Tests combination of several cross replica gather instructions with
// different gather dimensions.
TEST_P(AllGatherCombinerTest, CombineAllGathersByAllGatherDimension) {
  std::string hlo_string =
      absl::Substitute(R"(
HloModule Module$0

ENTRY entry {
  param0 = f32[2,2] parameter(0)
  param1 = f32[2,2] parameter(1)
  param2 = f32[2,2] parameter(2)
  param3 = f32[2,2] parameter(3)
  param4 = f32[2,2] parameter(4)
  allgather0 = f32[8,2] all-gather(param0), replica_groups={}, dimensions={0}
  allgather1 = f32[8,2] all-gather(param1), replica_groups={}, dimensions={0}
  allgather2 = f32[2,8] all-gather(param2), replica_groups={}, dimensions={1}
  allgather3 = f32[2,8] all-gather(param3), replica_groups={}, dimensions={1}
  allgather4 = f32[8,2] all-gather(param4), replica_groups={}, dimensions={0}
  ROOT tuple = (f32[8,2], f32[8,2], f32[2,8], f32[2,8], f32[8,2])
    tuple(allgather0, allgather1, allgather2, allgather3, allgather4)
}
)",
                       HasSchedule() ? ", is_scheduled=true" : "");
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo_string));

  AllGatherCombiner combine(1024 * 1024, kMaxCombineCount);
  ASSERT_EQ(AllGatherCount(*module), 5);
  TF_ASSERT_OK_AND_ASSIGN(bool changed, combine.Run(module.get()));
  EXPECT_TRUE(changed);

  Matcher<const HloInstruction*> combined_all_gather0 =
      op::AllGather(op::Parameter(0), op::Parameter(1), op::Parameter(4));
  Matcher<const HloInstruction*> combined_all_gather1 =
      op::AllGather(op::Parameter(2), op::Parameter(3));
  EXPECT_THAT(module->entry_computation()->root_instruction(),
              op::Tuple(op::GetTupleElement(combined_all_gather0, 0),
                        op::GetTupleElement(combined_all_gather0, 1),
                        op::GetTupleElement(combined_all_gather1, 0),
                        op::GetTupleElement(combined_all_gather1, 1),
                        op::GetTupleElement(combined_all_gather0, 2)));
}

// Tests that the combination threshold is respected.
TEST_P(AllGatherCombinerTest, DoNotCombineOverThreshold) {
  std::string hlo_string =
      absl::Substitute(R"(
HloModule Module$0

ENTRY entry {
  param0 = f32[8] parameter(0)
  param1 = f32[8] parameter(1)
  allgather0 = f32[32] all-gather(param0), replica_groups={}, dimensions={0}
  allgather1 = f32[32] all-gather(param1), replica_groups={}, dimensions={0}
  ROOT tuple = (f32[32], f32[32]) tuple(allgather0, allgather1)
}
)",
                       HasSchedule() ? ", is_scheduled=true" : "");
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo_string));

  // Run the AllGather combiner optimization pass with threshold less than
  // the combined size of the all gather ops so that the combination
  // cannot occur.
  AllGatherCombiner combine(255, kMaxCombineCount);
  ASSERT_EQ(AllGatherCount(*module), 2);
  TF_ASSERT_OK_AND_ASSIGN(bool changed, combine.Run(module.get()));
  EXPECT_EQ(AllGatherCount(*module), 2);
  EXPECT_FALSE(changed);
}

// Tests that the combination threshold is respected.
TEST_P(AllGatherCombinerTest, CombineUpToThreshold) {
  std::string hlo_string =
      absl::Substitute(R"(
HloModule Module$0

ENTRY entry {
  param0 = f32[8] parameter(0)
  param1 = f32[8] parameter(1)
  allgather0 = f32[32] all-gather(param0), replica_groups={}, dimensions={0}
  allgather1 = f32[32] all-gather(param1), replica_groups={}, dimensions={0}
  ROOT tuple = (f32[32], f32[32]) tuple(allgather0, allgather1)
}
)",
                       HasSchedule() ? ", is_scheduled=true" : "");
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo_string));

  // Run the AllGather combiner optimization pass with a threshold just higher
  // than that required such that the combination can occur.
  AllGatherCombiner combine(256, kMaxCombineCount);
  ASSERT_EQ(AllGatherCount(*module), 2);
  TF_ASSERT_OK_AND_ASSIGN(bool changed, combine.Run(module.get()));
  EXPECT_EQ(AllGatherCount(*module), 1);
  EXPECT_TRUE(changed);
}

// Tests that dependent all gathers are not combined.
TEST_P(AllGatherCombinerTest, NoDependentCombination) {
  std::string hlo_string =
      absl::Substitute(R"(
HloModule Module$0

ENTRY entry {
  param = f32[1] parameter(0)
  allgather0 = f32[2] all-gather(param), replica_groups={}, dimensions={0}
  ROOT allgather1 = f32[4] all-gather(allgather0), replica_groups={}, dimensions={0}
}
)",
                       HasSchedule() ? ", is_scheduled=true" : "");
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo_string));

  AllGatherCombiner combine(1024 * 1024, kMaxCombineCount);
  ASSERT_EQ(AllGatherCount(*module), 2);
  TF_ASSERT_OK_AND_ASSIGN(bool changed, combine.Run(module.get()));
  EXPECT_EQ(AllGatherCount(*module), 2);
  EXPECT_FALSE(changed);
}

// Tests that AllGather ops with different groups are not combined.
TEST_P(AllGatherCombinerTest, NoDifferentReplicaGroupsCombination) {
  std::string hlo_string =
      absl::Substitute(R"(
HloModule Module$0

ENTRY entry {
  param0 = f32[32] parameter(0)
  param1 = f32[32] parameter(1)
  allgather0 = f32[64] all-gather(param0), replica_groups={{0, 1}, {2, 3}},
    dimensions={0}
  allgather1 = f32[64] all-gather(param1), replica_groups={{0, 2}, {1, 3}},
    dimensions={0}
  ROOT tuple = (f32[64], f32[64]) tuple(allgather0, allgather1)
}
)",
                       HasSchedule() ? ", is_scheduled=true" : "");
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo_string));

  AllGatherCombiner combine(1024 * 1024, kMaxCombineCount);
  ASSERT_EQ(AllGatherCount(*module), 2);
  TF_ASSERT_OK_AND_ASSIGN(bool changed, combine.Run(module.get()));
  EXPECT_EQ(AllGatherCount(*module), 2);
  EXPECT_FALSE(changed);
}

TEST_P(AllGatherCombinerTest, DomainPreventsCombining) {
  std::string hlo_string =
      absl::Substitute(R"(
HloModule Module$0

ENTRY entry {
  param0 = f32[32] parameter(0), sharding={maximal device=0}
  param1 = f32[32] parameter(1), sharding={maximal device=1}
  allgather0 = f32[128] all-gather(param0),
    replica_groups={}, dimensions={0}, sharding={maximal device=0}
  allgather1 = f32[128] all-gather(param1),
    replica_groups={}, dimensions={0}, sharding={maximal device=1}
  domain0 = f32[128] domain(allgather0),
    domain={kind="sharding", entry={{maximal device=0}, {maximal device=1}},
    exit={maximal device=0}}
  domain1 = f32[128] domain(allgather1),
    domain={kind="sharding", entry={{maximal device=0}, {maximal device=1}},
    exit={maximal device=1}}
  ROOT tuple = (f32[128], f32[128]) tuple(domain0, domain1),
    sharding={{maximal device=0}, {maximal device=1}}
}
)",
                       HasSchedule() ? ", is_scheduled=true" : "");
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo_string));

  AllGatherCombiner combine(1024 * 1024, kMaxCombineCount);
  ASSERT_EQ(AllGatherCount(*module), 2);
  TF_ASSERT_OK_AND_ASSIGN(bool changed, combine.Run(module.get()));
  EXPECT_EQ(AllGatherCount(*module), 2);
  EXPECT_FALSE(changed);
}

// This test checks that two AllGather instructions that are in separate domains
// but with the same domain metadata can be combined.
TEST_P(AllGatherCombinerTest, CombineFromTwoDomainsWithSameMetadata) {
  std::string hlo_string =
      absl::Substitute(R"(
HloModule Module$0

ENTRY entry {
  param0 = f32[32] parameter(0), sharding={maximal device=0}
  param1 = f32[32] parameter(1), sharding={maximal device=1}
  param2 = f32[32] parameter(2), sharding={maximal device=1}
  allgather0 = f32[128] all-gather(param0),
    replica_groups={}, dimensions={0}, sharding={maximal device=0}
  allgather1 = f32[128] all-gather(param1),
    replica_groups={}, dimensions={0}, sharding={maximal device=1}
  allgather2 = f32[128] all-gather(param2),
    replica_groups={}, dimensions={0}, sharding={maximal device=0}
  domain0 = f32[128] domain(allgather0),
    domain={kind="sharding", entry={{maximal device=0}, {maximal device=1},
    {maximal device=0}}, exit={maximal device=0}}
  domain1 = f32[128] domain(allgather1),
    domain={kind="sharding", entry={{maximal device=0}, {maximal device=1},
    {maximal device=0}}, exit={maximal device=1}}
  domain2 = f32[128] domain(allgather2),
    domain={kind="sharding", entry={{maximal device=0}, {maximal device=1},
    {maximal device=0}}, exit={maximal device=0}}
  ROOT tuple = (f32[128], f32[128], f32[128]) tuple(domain0, domain1,
  domain2),
    sharding={{maximal device=0}, {maximal device=1}, {maximal device=0}}
}
)",
                       HasSchedule() ? ", is_scheduled=true" : "");
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo_string));

  AllGatherCombiner combine(1024 * 1024, kMaxCombineCount);
  ASSERT_EQ(AllGatherCount(*module), 3);
  TF_ASSERT_OK_AND_ASSIGN(bool changed, combine.Run(module.get()));
  EXPECT_EQ(AllGatherCount(*module), 2);
  EXPECT_TRUE(changed);

  // Verify that the sharding is combined correctly.
  const HloInstruction* param0 =
      module->entry_computation()->parameter_instruction(0);
  ASSERT_EQ(param0->user_count(), 1);
  const HloInstruction* combined_ag = param0->users().front();
  ASSERT_EQ(combined_ag->opcode(), HloOpcode::kAllGather);
  EXPECT_THAT(combined_ag, testing::opcode_matchers::Sharding(
                               "{{maximal device=0}, {maximal device=0}}"));
}

TEST_P(AllGatherCombinerTest, DoNotCombineCrossShardAndCrossReplicaInSPMD) {
  std::string hlo_string =
      absl::Substitute(R"(
HloModule Module$0

ENTRY entry {
  param0 = f32[32] parameter(0), sharding={maximal device=0}
  param1 = f32[32] parameter(1), sharding={maximal device=1}
  cross_shard_ag = f32[128] all-gather(param0),
    replica_groups={{0}}, dimensions={0}, channel_id=1
  cross_replica_ag = f32[128] all-gather(param1),
    replica_groups={{0}}, dimensions={0}, sharding={maximal device=1}
  ROOT tuple = (f32[128], f32[128]) tuple(cross_shard_ag, cross_replica_ag)
}
)",
                       HasSchedule() ? ", is_scheduled=true" : "");
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo_string));

  AllGatherCombiner combine(1024 * 1024, kMaxCombineCount);
  ASSERT_EQ(AllGatherCount(*module), 2);
  TF_ASSERT_OK_AND_ASSIGN(bool changed, combine.Run(module.get()));
  EXPECT_EQ(AllGatherCount(*module), 2);
  EXPECT_FALSE(changed);
}

TEST_P(AllGatherCombinerTest, CombineContiguousGroups) {
  std::string hlo_string =
      absl::Substitute(R"(
HloModule Module$0

ENTRY entry {
  param0 = u32[32] parameter(0)
  param1 = u32[32] parameter(1)
  param2 = u32[32] parameter(2)
  param3 = u32[32] parameter(3)
  ag0 = u32[64] all-gather(param0), replica_groups={}, dimensions={0}
  ag1 = u32[64] all-gather(param1), replica_groups={}, dimensions={0}
  foo = u32[64] add(ag0, ag1)
  ag2 = u32[64] all-gather(param2), replica_groups={}, dimensions={0}
  ag3 = u32[64] all-gather(param3), replica_groups={}, dimensions={0}

  ROOT tuple = (u32[64], u32[64], u32[64], u32[64]) tuple(ag0, ag1, ag2, ag3)
}
)",
                       HasSchedule() ? ", is_scheduled=true" : "");
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo_string));

  AllGatherCombiner combine(1024 * 1024, kMaxCombineCount);
  ASSERT_EQ(AllGatherCount(*module), 4);
  TF_ASSERT_OK_AND_ASSIGN(bool changed, combine.Run(module.get()));
  EXPECT_TRUE(changed);

  if (HasSchedule()) {
    Matcher<const HloInstruction*> combined0 =
        op::AllGather(op::Parameter(0), op::Parameter(1));
    Matcher<const HloInstruction*> combined1 =
        op::AllGather(op::Parameter(2), op::Parameter(3));
    EXPECT_THAT(module->entry_computation()->root_instruction(),
                op::Tuple(op::GetTupleElement(combined0, 0),
                          op::GetTupleElement(combined0, 1),
                          op::GetTupleElement(combined1, 0),
                          op::GetTupleElement(combined1, 1)));
  } else {
    Matcher<const HloInstruction*> combined = op::AllGather(
        op::Parameter(0), op::Parameter(1), op::Parameter(2), op::Parameter(3));
    EXPECT_THAT(module->entry_computation()->root_instruction(),
                op::Tuple(op::GetTupleElement(combined, 0),
                          op::GetTupleElement(combined, 1),
                          op::GetTupleElement(combined, 2),
                          op::GetTupleElement(combined, 3)));
  }
}

}  // namespace
}  // namespace xla
