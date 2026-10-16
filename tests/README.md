# Kernel tests

`wow64_ldt_test.c` tests running 32-bit code inside a 64-bit Haiku team, and the self-debugging
support Wine needs. It needs the system package built from the `rations/haiku` release branch;
on a stock kernel, the first test reports the new syscalls as missing.

```sh
gcc -O1 -Wall -o wow64_ldt_test wow64_ldt_test.c
./wow64_ldt_test        # all tests, about a minute
./wow64_ldt_test 3      # one test
```

It covers:
- the per-team LDT and per-thread FS segment
- 32-bit loops across context switches on every CPU
- signals that interrupt 32-bit code
- forged sigreturn frames, which must end in SIGSEGV and not a panic
- rejected descriptors
- fork and exec
- 32-bit system call instructions
- many threads using LDT entries
- user traps and hardware breakpoints delivered as SIGTRAP

The JackDAW stack installer runs it after the reboot, as a check that the CPU and kernel handle
32-bit code.
