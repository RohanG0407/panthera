//
// Copyright (c) 2019 Ampere Computing. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause
//

#ifndef AMPERE_CORE_TLBIREQ_H
#define AMPERE_CORE_TLBIREQ_H

#include "ampere/sc/sc.h"
#include "ampere/sc/transaction.h"
#include "ampere/core/instruction.h"

namespace ampere {
namespace core {

using namespace ampere::sc;

class TLBIReqObj;
using TLBIReq = ampere::sc::transaction_ptr<TLBIReqObj>;

class TLBIReqObj : public ampere::sc::Transaction
{
public:
    explicit TLBIReqObj(uint64_t id, std::shared_ptr<const archx::Instruction> tlbi) : id(id), tlbi(std::move(tlbi)) {}

    uint64_t id;
    // The TLBI: one the core committed, or one that invalidated the core's
    // translations without the core executing it (see
    // archx::Instruction::tlb_invalidations_after())
    std::shared_ptr<const archx::Instruction> tlbi;
};

} // namespace core
} // namespace ampere

#endif // AMPERE_CORE_TLBIREQ_H

/*
 * vim: sw=4:ts=4:et:si:cindent:cino=\:0,(0,g0,t0:
 */
