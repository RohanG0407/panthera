# Copyright (c) 2025-2026 Ampere Computing. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause

# Data-prefetcher headroom study, adapted from core_study.py. Each trace runs
# three times on the Domino (Connor) core: with the production prefetchers,
# with every data prefetcher off, and with oracle data prefetchers at L1D and
# L2. The gap between the first two is what the prefetchers deliver today; the
# gap between the first and the last is what a better prefetcher could add.

from panthera.cheetah import Cmd, argument_parser, run_study
from panthera.cheetah.studies.bobcat import BobcatSim
from panthera.cheetah.studies.core import CoreSim, trace_list

CORE_CFG = "{panthera_root}/config/model/core"

configurations = [
    {
        # Production prefetchers: L1D PCP and GRETCH, L2 BOP, CBusy throttling.
        "configuration": "baseline",
        "more_core_args": [],
    },
    {
        # PCP, GRETCH and L2 BOP off. The instruction prefetchers stay on.
        # This file needs a core with GRETCH, so it does not load on Siryn.
        "configuration": "no-prefetch",
        "more_core_args": [
            "-sys.soc_0.core_0:append-cfg", f"{CORE_CFG}/pv/all_prefetchers_disabled.cfg",
        ],
    },
    {
        # Oracle prefetchers replace the L1D and L2 prefetchers. They know the
        # future load addresses but still fetch through the real queues, fill
        # buffers, mesh and memory (see core/prefetchers/oracle.h), so this is
        # an estimate of the headroom and not a strict upper bound.
        "configuration": "oracle",
        "more_core_args": [
            "-sys.soc_0.core_0.lsu.prefetcher:override-cfg", f"{CORE_CFG}/lsu_prefetchers/oracle.cfg",
            "-sys.soc_0.core_0.l2c.prefetcher:override-cfg", f"{CORE_CFG}/l2c_prefetchers/oracle.cfg",
        ],
    },
]  # fmt: skip

bobcat_cmd = BobcatSim(
    bobcat_args=[
        "-bs", Cmd.join_paths("{workload_root}", "{pan_branch_warmup_file}"),
        "-top.core.decoders", '"[\\"furiosa\\"]"',
        "-top.core:cfg", "{panthera_root}/config/model/core/connor/connor-tage.cfg",
        "-top.core.bpu.save_warmup_file", "{name}.bpu_warmup.json.gz",
    ],
    only_if=Cmd.has("pan_branch_warmup_file"),
)  # fmt: skip
amperesys_cmd = CoreSim(
    core="domino",
    extra_core_args=[
        "{more_core_args}",
        Cmd.if_set(
            "pan_branch_warmup_file",
            ["-sys.soc_0.core_0.bpu.warmup_file", "{name}.bpu_warmup.json.gz"],
        ),
    ],
)
commands = bobcat_cmd + amperesys_cmd

parser = argument_parser(prog=__file__, description="Data-prefetcher headroom study on a study list")
for command in commands:
    command.parser_add_args(parser)
args = parser.parse_args()

traces = trace_list(args.workloads)

run_study(args.study, commands.apply(traces).apply([vars(args)]).apply(configurations), args)
