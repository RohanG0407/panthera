# QEMU "Simple" Tracing Plugin

## Capturing Traces

### QEMU/Workload Setup

To use this plugin, you will need:

* A Linux kernel `Image` with perf-semihosting support. Apply Panthera's
  [Linux 5.10 perf-semihosting patch](perf-semihosting-5.10.patch) from the
  kernel source root with `git apply /path/to/panthera/doc/tracing/perf-semihosting-5.10.patch`.
  The patch applies cleanly to Linux commit
  `2c85ebc57b3e1817b6ce1a6b703928e113a90442`; compile with
  `CONFIG_ARM64_PERF_SEMIHOSTING=y` and
  `CONFIG_PID_IN_CONTEXTIDR=y`.
* A
  [qcow2](https://qemu.weilnetz.de/doc/qemu-doc.html#disk_005fimages_005fformats)
  root filesystem containing the workload you want to run and scripts to run
  it. There's an example script for creating an [Alpine
  Linux](https://alpinelinux.org/)-based image at
  [tracing_tools/examples/old/build_qcow_image.sh](../../src/ampere/tracing_tools/examples/old/build_qcow_image.sh)
* Last but not least, a built and tested panthera checkout

The supported way to provide this tool is Panthera's [Nix development
environment](../UserGuide.md#nix-setup). From the repository root, enter it
with `nix-shell` (or enable it with `direnv allow`); it puts the required
plugin-enabled QEMU on your `$PATH`.

If you are not using the Nix environment, the following is also required:

* A `qemu-system-aarch64` binary compiled from [our plugin-enabled
  QEMU](https://github.com/AmpereComputing/tracing-qemu/)
  and on your `$PATH`

### Collecting Traces

```
kernel_dir=./linux
rootfs_qcow=./rootfs.qcow
panthera_root=./panthera

qemu-system-aarch64 \
    -smp 1 \
    -m 4G \
    -M virt \
    -cpu cortex-a57 \
    -nographic \
    -semihosting \
    -icount 0 \
    -singlestep \
    -kernel $kernel_dir/arch/arm64/boot/Image \
    -append 'console=ttyAMA0 root=/dev/vda perf.semihosting init=/bin/sh earlycon=smh' \
    -hda $rootfs_qcow \
    -plugin $panthera_root/install/lib/libinstruction_tracer.so,profile-comm=loopy,instruction-start-trigger=10000,trace-filename-base=loopy_streams,instruction-stop-trigger=500,periodic-instruction-start-trigger=2000,max-traces=40
    -d plugin \
    -D tracing.log
```

The general QEMU options are explained in the [simpoint-tracing
documentation](tracing-simpoints.md), so this document does not cover them. The
above plugin options will trace any process in the process tree of the first one
it finds with the command-name of "loopy". It will start tracing after the
process tree collectively executes 10,000 instructions in EL0 and trace 500
instructions as loopy_streams_0.is. 2,000 after that first trace began, a second
trace will begin, which will save a 500-instruction trace to loopy_streams_1.is.
This will repeat until 40 instruction streams have been captured, or all
processes in this process tree exit.

### Guests with more than one vCPU

An instruction stream describes one core, so on a guest started with more
than one vCPU (`-smp 4`, for example) the plugin traces a single vCPU, chosen
with `trace-vcpu=N` (vCPU 0 by default):

* Only vCPU N's instructions are counted, checked against instruction and PC
  triggers, and recorded. PID and EL tracking follows the thread running on
  vCPU N, so `profile-pid=` and `profile-comm=` count a thread's instructions
  only while it runs there. Other vCPUs' instructions never appear in the
  stream.
* Kernel task events (fork, exec and exit, through perf semihosting) are
  handled from every vCPU, so `profile-comm=` and `profile-children=` follow
  processes and threads wherever they are created.
* Trace markers (`hlt #0x38ce` semihosting calls and `hint #0x55`) mark a
  point in the workload rather than in one core's instructions, so they are
  honored from every vCPU. One executed by another vCPU takes effect just
  before vCPU N's next instruction, and the log names the vCPU it came from.
* Other vCPUs change memory that vCPU N reads, including translation tables.
  The plugin records their writes (stores, atomics and `DC ZVA`) as memory
  events right before vCPU N's next recorded instruction, wherever the stream
  already describes that memory, so translations and memory values in the
  stream stay consistent with what vCPU N saw.
* The TLB invalidations other vCPUs broadcast (the Inner and Outer Shareable
  `TLBI` forms) invalidate vCPU N's translations too. The plugin records each
  as a TLB invalidation event at the same point, so a model that applies them
  does not keep translations that vCPU N no longer had.
* `trace-elf=true` is rejected: an ELF re-executes only the traced vCPU's
  instructions, so the memory other vCPUs write would make it diverge.

Run multi-vCPU guests with `-icount`. With it, QEMU does not use multi-threaded
TCG: it runs all vCPUs on one host thread, taking turns, so the plugin sees one
vCPU at a time and its register and memory accessors act on the vCPU that
executes. The guest clock follows the instruction count of all vCPUs together.
The plugin stops with an error if it sees vCPUs on more than one host thread.
QEMU still emulates the other vCPUs on that thread, with the plugin's
callbacks and with `-singlestep`, so every instruction of vCPU N costs more
host time than on a single-vCPU guest, and instruction triggers on vCPU N take
longer to fire when it is often idle.

Pass `instrument-all=true` as well. By default the plugin instruments only the
instructions it needs, and it has QEMU redo every translation after each
`ERET`, exception entry, syscall, counter read and `CONTEXTIDR_EL1` or
`VBAR_ELx` write of vCPU N, and whenever tracing starts or stops. Each of those
flushes ends the running vCPU's turn, so vCPU N gets less of the host thread
than the other vCPUs, and the guest does different work than it would
untraced. On a server workload, for example, vCPU N's threads then batch more
requests and spend a smaller share of their instructions in the kernel. With
`instrument-all=true` the plugin instruments every instruction from the start
and never flushes for these events, so the guest runs as it would without the
plugin. Emulation before the trace starts is slower.

There are several other methods for causing tracing to stop/start, which are
covered below, or by passing "-h" as an argument to the plugin:

```
General options:
  trace-filename-base=FILE
        Prefix path where the instruction trace(s) should be
        stored (default: \"trace\")
  trace-is=[true|false]
        Trace to an instruction stream (default: true)
  trace-elf=[true|false]
        Trace to an ELF (default: false)
  max-traces=MAX
        Exit if MAX traces have been captured (default 1)
  profile-el1=[true|false]
        Profile EL1 in addition to EL0 when counting instructions
        (default: false)
  profile-comm=COMM
        Restrict tracing to the first PID found with this command
        name (used with instruction and PC triggers)
  profile-pid=PID
        Restrict tracing to the given PID (used with instruction
        and PC triggers)
  profile-children=[true|false]
        For profile-comm= or profile-pid=, profile any child
        processes in addition to the parent. This is recursive -
        all descendent processes will be profiled. (default: false)
  skip-unwatched=[true|false]
        Record only instructions executed while a profiled PID is
        current (and, without profile-el1, only in EL0). Other
        processes' instructions are left out, so the stream jumps
        across context switches; their system register writes and
        the memory they write are still recorded so translation
        state and memory contents stay correct, and so are their TLB
        invalidations, as TLB invalidation events. Their other cache
        maintenance is not recorded. Without this, every instruction
        the CPU executes while a trace is active is recorded.
        (default: false)
  instrument-all=[true|false]
        Instrument every instruction for the whole run, so the
        plugin never has to flush QEMU's translations (each flush
        ends the running vCPU's slice of the icount round robin).
        Without it the plugin instruments only the instructions it
        needs and flushes every translation after each ERET,
        exception entry, syscall, CONTEXTIDR or VBAR write and
        counter read of the traced vCPU, and at every tracing state
        change; on a guest with several vCPUs under -icount that
        slows the traced vCPU down relative to the others and
        changes what the guest does. With it the guest runs as it
        would without the plugin, at the cost of slower emulation
        before the trace starts. (default: false)
  trace-vcpu=N
        On a guest with more than one vCPU, trace vCPU N (numbered
        from 0) and no other. Instruction counts, instruction and PC
        triggers, PID and EL tracking, and recording all follow that
        vCPU alone. Kernel task events from every vCPU still update
        the profiled PIDs, and trace markers (`hlt #0x38ce` and
        `hint #0x55`) executed by another vCPU take effect just
        before vCPU N's next instruction. Memory written by other
        vCPUs, and the TLB invalidations they broadcast, are
        recorded where vCPU N can first observe them. See
        "Guests with more than one vCPU" above. (default: 0)
  offline-warmup-instructions=INSTRS
        Number of instructions worth of data to capture after start
        trigger before online warmup instructions. Setting this
        currently causes both memory and branch streams to be
        captured. (default 0)
  warmup-instructions=INSTRS
        Number of instructions to capture after offline warmup
        before inserting a START_MEASUREMENT marker. (default 0)
  cooldown-instructions=INSTRS
        Number of instructions to capture after inserting a
        STOP_MEASUREMENT marker at the end of the region of
        interest (default 10000)
  flat-map=[true|false]
        Instead of preserving the existing page tables, convert
        them to a flat mapping (only affects ELF tracer) (default: false)
  elf-entry-point=ENTRY_POINT
        The first entry point to consider for the generated ELF file
        (both PA and VA) (default: 0x40000000)
  debug=[true|false]
        Enable debug prints for OS forks, context switches,
        and register values. (default: false)
  cache-filter-size=BYTES
        Size of cache in bytes through which to filter memory
        stream accesses. Only accesses which miss this cache will
        show up in the stream (default 131072)

Simpoint tracing options
  generation-log=JSON_FILE
        Path to read the BBV generation metadata from (must also
        supply checkpoint-tag= if using this option)
  checkpoint-tag=TAG
        The name of the checkpoint being restored, referenced in
        the generation log JSON file.
  override-simpoint-stop-trigger=[true|false]
        Allow the user to set the stop trigger on the command-line
        instead of reading it from the generation log (default: false)

Start triggering options (select only one method)
  instruction-start-trigger=INSTRS
        Initially start tracing after INSTRS instructions have
        executed in PID. This is the default start trigger.
        (default 0)
  periodic-instruction-start-trigger=INSTRS
        Start tracing again after INSTRS instructions have
        executed in PID since the previous trace began. This does
        not apply to the first trace (default 0)
  semihosting-start-trigger=[true|false]
        Start tracing when `hlt #0x38ce` is seen with x0 = 1 (default: false)
  hint-start-trigger=[true|false]
        Start tracing when `hint #0x55` is seen with x0 = 1 (default: false)
  pc-start-trigger=PC
        Start tracing when PC is seen in PID for the Nth time (N
        is defined by pc-start-count=).
  pc-start-count=N
        Count for pc-start-trigger= (default 0)

Stop triggering options (select only one method)
  instruction-stop-trigger=INSTRS
        Stop tracing INSTRS instructions after starting. This is
        the default stop trigger. (default 1000000)
  semihosting-stop-trigger=[true|false]
        Stop tracing when `hlt #0x38ce` is seen with x0 = 2 (default: false)
  hint-stop-trigger=[true|false]
        Stop tracing when `hint #0x55` is seen with x0 = 2 (default: false)
  pc-stop-trigger=PC
        Stop tracing when PC is seen in PID for the Nth time after
        starting (N is defined by pc-stop-count=).
  pc-stop-count=N
        Count for pc-stop-trigger= (default 0),
```
