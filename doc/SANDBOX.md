# Sandbox

klind's worker processes, and the TLS key manager ([KEYMGR](KEYMGR.md)), run in a
sandbox that limits what the kernel lets them do. It is on by default; a
server turns it off for its workers (and its key manager) with:

```
www
{
    type    http
    ...
    sandbox no
}
```

LINUX (seccomp)

A worker is sandboxed after the child_init hook (so a webapp can open
what it needs first) and before it serves. A seccomp-bpf filter, with
PR_SET_NO_NEW_PRIVS (a setuid program cannot raise privileges), sorts
system calls in three groups:

- allowed: what serving needs. Reading and writing files (static files,
  sessions, uploads), sockets (the client, klind, the key manager, and
  connections a page opens, e.g. to a database), memory, time, signals,
  the timer, randomness.
- fatal: what only an attacker needs. ptrace, process_vm_readv/writev,
  mount and pivot_root, chroot, the setuid/setgid family and capset,
  module loading, kexec, bpf, perf_event_open, keyctl, unshare and
  setns (namespaces), userfaultfd, personality, reboot, swap, clock and
  time setting, open_by_handle_at, iopl. The process is killed (SIGSYS);
  klind starts a new worker.
- anything else fails with EPERM, so code that tries something unusual
  gets an error instead of dying.

System calls of another architecture (and the x32 ABI) kill the process.

Some allowed calls have their arguments checked too (EPERM otherwise):

- socket: UNIX, IPv4 and IPv6 sockets only (not netlink, packet, vsock,
  Bluetooth, ...); socketpair: UNIX only;
- mmap and mprotect: never memory that is writable and executable at
  once (W^X), as systemd's MemoryDenyWriteExecute; executable code is
  still mapped from files (libraries, CGI programs);
- kill and tgkill: a worker signals itself only. One that runs CGI
  scripts (which it kills by process group at their timeout, and which
  signal their own processes) may signal any process except klind, the
  key managers, its own process group (klind's: kill 0 or -pgid) and
  every process (-1). A compromised worker could otherwise kill the key
  manager, which stops klind. The scripts inherit the filter, so a
  script cannot use kill 0 either (to signal its own process group: it
  gets EPERM), though it has a process group of its own; it can name the
  group (kill -pgid) or its processes.

Naming one of these calls in sandbox_allow lifts its argument check
(sandbox_allow socket: any family; mmap or mprotect: W+X memory, e.g.
for a JIT; kill or tgkill: any target). Sanitizer builds do not check
kill and tgkill: LeakSanitizer signals its threads.

A server whose configuration has a script_alias or an extension's
handler (CGI, [CGI](CGI.md)) also allows fork, clone, execve, wait, chdir and
(x86-64) arch_prctl, which every new program calls at start: the CGI
scripts inherit the filter. A page that
needs a call outside the list names it:

```
sandbox_allow   flock, inotify_init1, inotify_add_watch
```

Only calls from a known list can be named (bind, listen, `epoll_*`,
eventfd2, `timerfd_*`, `inotify_*`, flock, fallocate, fchown(at), linkat,
symlinkat, mknodat, msync, mlock, utimensat, memfd_create, splice, ...;
and fork/clone/execve/wait for CGI-like uses); others stop klind at
startup. Calls from the fatal group cannot be allowed.

The key manager gets a much shorter list: its socket, memory, randomness
and exit. It signs once before the filter is set, so OpenSSL has loaded
what it needs.

Sanitizer builds (AddressSanitizer) also allow clone and ptrace:
LeakSanitizer uses them at exit.

## Other systems

OpenBSD: workers pledge(2) "stdio rpath wpath cpath fattr inet unix dns
sendfd recvfd" (plus "proc exec" for CGI), the key manager "stdio sendfd
recvfd". This code is not tested: there is no OpenBSD machine in the
test setup. macOS and others: no sandbox.

## What is not sandboxed

klind itself (it forks workers, reaps them, writes logs) and the
single-process server of -d, which is meant for development. CGI
scripts are filtered only as far as the worker's filter goes.

The server tests check, on Linux: every worker and the key manager have
the filter (Seccomp: 2 in /proc), klind does not; a call outside the
list fails with EPERM; unshare(2) kills the worker and the server goes
on serving; a server with sandbox no is unfiltered; a netlink socket, W+X
memory and a signal to klind fail with EPERM (and work without the
sandbox), an IPv6 socket and a signal to itself work, and a worker that
runs CGI scripts cannot signal klind either.
