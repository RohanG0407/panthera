//
// Copyright (c) 2020 Ampere Computing. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
//
#include "ampere/archx/instruction_simple.h"
#include "ampere/core/bp_tage.h"
#include "ampere/sc/testing/test_module.h"

namespace ampere::core {

class BPTageTest : public TestModule
{
protected:
    BPTageTest() : BPTageTest({{"bpu.tage.btb.sets", 1024}}) {}

    explicit BPTageTest(std::initializer_list<std::pair<std::string, sc::json>> settings)
        : TestModule{settings}, context{EL::EL0, 0U, token_generator.next(true)}
    {
    }

    void SetUp() override { archx::Instruction::set_decoders({"furiosa"}); }

    Fetch make_fetch(ProgramCounter pc) { return make<Fetch>(pc, current_fetch_id++); }

    BP_TAGE tage{"tage"};
    BranchHistory bhr{"bhr"};
    TokenGenerator token_generator{71};
    BranchContext context;

    FetchId current_fetch_id{1};
};

TEST_F(BPTageTest, FirstPredictionIsNoBranch)
{
    auto fetch = make_fetch(0x401000_pc);
    tage.predict(fetch, bhr, context);

    EXPECT_EQ(fetch->get_prediction().branch_type, BranchType::NONE);
}

TEST_F(BPTageTest, TrainAndPredict)
{
    auto fetch = make_fetch(0x401000_pc);
    tage.predict(fetch, bhr, context);

    BranchInfo branch_info{BranchType::CONDITIONAL_DIRECT, 0x401000_pc, fetch, 0};
    branch_info.taken = true;
    tage.train(branch_info, bhr, context);

    auto fetch2 = make_fetch(0x401000_pc);
    tage.predict(fetch2, bhr, context);

    EXPECT_EQ(fetch2->get_prediction().branch_type, BranchType::CONDITIONAL_DIRECT);
}

TEST_F(BPTageTest, BTBWontStoreNotTakenBranch)
{
    auto fetch = make_fetch(0x401000_pc);
    tage.predict(fetch, bhr, context);

    BranchInfo branch_info{BranchType::CONDITIONAL_DIRECT, 0x401000_pc, fetch, 0};
    tage.train(branch_info, bhr, context);

    auto fetch2 = make_fetch(0x401000_pc);
    tage.predict(fetch2, bhr, context);

    EXPECT_EQ(fetch2->get_prediction().branch_type, BranchType::NONE);
}

TEST_F(BPTageTest, BTCWrongTargetWillClearBTBSet)
{
    auto fetch = make_fetch(0x401000_pc);
    tage.predict(fetch, bhr, context);

    BranchInfo branch_info{BranchType::CONDITIONAL_DIRECT, 0x401000_pc, fetch, 0};
    branch_info.taken = true;
    tage.train(branch_info, bhr, context);

    auto fetch2 = make_fetch(0x401000_pc);
    tage.predict(fetch2, bhr, context);

    auto inst = make<Instruction>(std::make_shared<archx::InstructionSimple>(0x401000_pc, 0x54000000), fetch2);
    auto flush = make<Flush>(0x401000_pc, 0x401000_pc, fetch2, inst, 1, FlushType::BRANCH_CHECK_TARGET);

    tage.recover(flush, nullptr, bhr, context);

    auto fetch3 = make_fetch(0x401000_pc);
    tage.predict(fetch3, bhr, context);

    EXPECT_EQ(fetch3->get_prediction().branch_type, BranchType::NONE);
}

TEST_F(BPTageTest, BTCWrongBranchTypeWillClearBTBSet)
{
    auto fetch = make_fetch(0x401000_pc);
    tage.predict(fetch, bhr, context);

    BranchInfo branch_info{BranchType::CONDITIONAL_DIRECT, 0x401000_pc, fetch, 0};
    branch_info.taken = true;
    tage.train(branch_info, bhr, context);

    auto fetch2 = make_fetch(0x401000_pc);
    tage.predict(fetch2, bhr, context);

    auto inst = make<Instruction>(std::make_shared<archx::InstructionSimple>(0x401000_pc, 0x94000001), fetch2);
    auto flush = make<Flush>(0x401000_pc, 0x401000_pc, fetch2, inst, 1, FlushType::BRANCH_CHECK_WRONG_TYPE);

    tage.recover(flush, nullptr, bhr, context);

    auto fetch3 = make_fetch(0x401000_pc);
    tage.predict(fetch3, bhr, context);

    EXPECT_EQ(fetch3->get_prediction().branch_type, BranchType::NONE);
}

// Teaching the BTB a call commits that call without an RSB entry, so these tests let the RSB
// ignore such commits.
class BPTageRsbTest : public BPTageTest
{
protected:
    BPTageRsbTest() : BPTageTest({{"tage.rsb.ignore_untracked_commits", true}}) {}

    // Teach the BTB a call at `call_pc` to `callee` and a return at `callee`.
    void learn_call_and_return(ProgramCounter call_pc, ProgramCounter callee)
    {
        auto call_fetch = make_fetch(call_pc);
        tage.predict(call_fetch, bhr, context);
        BranchInfo call{BranchType::CALL_DIRECT, call_pc, call_fetch, 0};
        call.taken = true;
        call.target = callee;
        tage.train(call, bhr, context);

        auto ret_fetch = make_fetch(callee);
        tage.predict(ret_fetch, bhr, context);
        BranchInfo ret{BranchType::RETURN, callee, ret_fetch, 0};
        ret.taken = true;
        ret.target = call_pc + 4;
        tage.train(ret, bhr, context);
    }

    // Predict and commit a return at `pc`; return whether the RSB predicted it.
    bool return_at(ProgramCounter pc)
    {
        auto fetch = make_fetch(pc);
        tage.predict(fetch, bhr, context);
        EXPECT_EQ(fetch->get_prediction().branch_type, BranchType::RETURN);
        BranchInfo ret{BranchType::RETURN, pc, fetch, 0};
        tage.train(ret, bhr, context);
        return ret.rsb_hit;
    }
};

TEST_F(BPTageRsbTest, CsfNoCmtFlushOfCallLeavesNoRsbEntry)
{
    learn_call_and_return(0x401000_pc, 0x402000_pc);

    // A CSF_NO_CMT flush of the predicted call, before it is dispatched (no BOB entry):
    // fetching restarts at the call itself.
    auto fetch = make_fetch(0x401000_pc);
    tage.predict(fetch, bhr, context);
    ASSERT_EQ(fetch->get_prediction().branch_type, BranchType::CALL_DIRECT);
    auto inst = make<Instruction>(std::make_shared<archx::InstructionSimple>(0x401000_pc, 0x94000400), fetch);
    auto flush = make<Flush>(0x401000_pc, 0x401000_pc, fetch, inst, 1, FlushType::CSF_NO_CMT);
    tage.recover(flush, nullptr, bhr, context);

    // The call is fetched again and commits from the new fetch.
    auto refetch = make_fetch(0x401000_pc);
    tage.predict(refetch, bhr, context);
    BranchInfo call{BranchType::CALL_DIRECT, 0x401000_pc, refetch, 0};
    tage.train(call, bhr, context);

    // Its return pops it. Nothing is left for a second return: the flushed fetch's call is gone.
    EXPECT_TRUE(return_at(0x402000_pc));
    EXPECT_FALSE(return_at(0x402000_pc));
}

} // namespace ampere::core
