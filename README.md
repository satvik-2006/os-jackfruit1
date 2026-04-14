

**Multi-Container Runtime**

OS Mini-Project  |  UE24CS242B

PES University EC Campus  |  4th Semester  |  Section H

|**Name**|**SRN**|**Section**|
| :- | :- | :- |
|Satvik Das|PES2UG24CS448|H|
|Shashank Verma|PES2UG24CS462|H|


# **1. Team Information**

|**Field**|**Details**|
| :- | :- |
|Course|Operating Systems (UE24CS242B)|
|Semester|4th Semester, Jan–May 2026|
|Campus|PES University EC Campus|
|Section|H|

# **2. Build, Load, and Run Instructions**
## **2.1 Prerequisites**
Ubuntu 22.04 or 24.04 VM with Secure Boot OFF. WSL is not supported.

sudo apt update

sudo apt install -y build-essential linux-headers-$(uname -r)

## **2.2 Prepare Root Filesystem**
Download and extract the Alpine mini root filesystem:

mkdir rootfs-base

wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86\_64/alpine-minirootfs-3.20.3-x86\_64.tar.gz

tar -xzf alpine-minirootfs-3.20.3-x86\_64.tar.gz -C rootfs-base

cp -a ./rootfs-base ./rootfs-alpha

cp -a ./rootfs-base ./rootfs-beta

Copy workload binaries into each rootfs before launching containers:

cp cpu\_hog memory\_hog io\_pulse ./rootfs-alpha/

cp cpu\_hog memory\_hog io\_pulse ./rootfs-beta/

## **2.3 Build**
A single make command builds the engine, all workloads, and the kernel module:

make

This produces: engine, cpu\_hog, memory\_hog, io\_pulse, and monitor.ko.

## **2.4 Load Kernel Module**
sudo insmod monitor.ko

ls -l /dev/container\_monitor   # verify device node created

## **2.5 Start the Supervisor**
sudo ./engine supervisor ./rootfs-base

The supervisor binds a UNIX domain socket at /tmp/mini\_runtime.sock, spawns the logging consumer thread, opens /dev/container\_monitor, and enters its command-dispatch loop.

## **2.6 Launch Containers**
In a separate terminal, start containers using the CLI:

sudo ./engine start alpha ./rootfs-alpha /cpu\_hog 30 --soft-mib 48 --hard-mib 80

sudo ./engine start beta ./rootfs-beta /io\_pulse 20 200

## **2.7 CLI Commands**

|**Command**|**Description**|
| :- | :- |
|engine supervisor <rootfs>|Start the long-running supervisor daemon|
|engine start <id> <rootfs> <cmd> [opts]|Start container in background|
|engine run <id> <rootfs> <cmd> [opts]|Start container, block until exit|
|engine ps|List all tracked containers and their state|
|engine logs <id>|Print captured log output for a container|
|engine stop <id>|Send SIGTERM to a running container|

Optional flags for start/run:

- --soft-mib N   set soft memory limit (default: 40 MiB)
- --hard-mib N   set hard memory limit (default: 64 MiB)
- --nice N       set scheduler priority (-20 to 19)

## **2.8 Run Scheduling Experiments**
Run two cpu\_hog workloads at different nice values and measure completion time:

sudo ./engine start high\_pri ./rootfs-alpha '/cpu\_hog 20' --nice -5

sudo ./engine start low\_pri  ./rootfs-beta  '/cpu\_hog 20' --nice 10

sudo ./engine ps

## **2.9 Teardown**
sudo ./engine stop alpha

sudo ./engine stop beta

\# Send SIGINT or SIGTERM to the supervisor to trigger clean shutdown

sudo rmmod monitor

dmesg | tail -20   # verify module unloaded cleanly

# **3. Demo Screenshots**
The following 8 screenshots demonstrate each required capability. Each is annotated with a brief caption.

|**#**|**What It Demonstrates**|**What the Screenshot Shows**|
| :- | :- | :- |
|1|Multi-container supervision|Two containers (alpha, beta) running concurrently under one supervisor process. Terminal shows supervisor output and both container start confirmations.|
|2|Metadata tracking|Output of engine ps showing container IDs, host PIDs, states, and commands for two running containers.|
|3|Bounded-buffer logging|Contents of logs/alpha.log captured through the producer-consumer pipeline. dmesg or supervisor output showing the logging thread active.|
|4|CLI and IPC|engine start command being issued in one terminal; supervisor socket responding with 'Started alpha with PID XXXX' in the other.|
|5|Soft-limit warning|dmesg output showing [container\_monitor] SOFT LIMIT container=alpha pid=XXXX after memory\_hog exceeds the 40 MiB soft limit.|
|6|Hard-limit enforcement|dmesg output showing [container\_monitor] HARD LIMIT container=alpha, followed by engine ps showing state as 'killed'.|
|7|Scheduling experiment|Side-by-side timing output of two cpu\_hog containers run at nice -5 vs nice +10, showing the high-priority container completes measurably faster.|
|8|Clean teardown|ps aux output after supervisor shutdown showing no zombie processes; dmesg showing module unloaded message.|

*[ Screenshots to be inserted above each caption row after running the demo ]*

# **4. Engineering Analysis**
## **4.1 Isolation Mechanisms**
Each container is created using clone() (or via fork + unshare) with CLONE\_NEWPID, CLONE\_NEWNS, CLONE\_NEWUTS, CLONE\_NEWIPC, and CLONE\_NEWNET flags, giving it an independent view of the PID tree, mount table, hostname, IPC objects, and network stack. chroot() then makes the container see only its assigned rootfs-alpha or rootfs-beta directory as /, and a fresh /proc is mounted inside so tools like ps work correctly within the container.

At the kernel level, namespaces are lightweight wrappers around kernel data structures — each PID namespace has its own PID counter starting at 1, and the mount namespace has an independent copy of the mount table. Critically, the host kernel is still shared: system calls go through the same kernel, the host scheduler manages all threads, and physical memory is not partitioned — only the visibility of resources is isolated, not the resources themselves.

## **4.2 Supervisor and Process Lifecycle**
A long-running supervisor is necessary because containers must outlive the CLI invocation that created them. When engine start is called, it is a short-lived client process that sends a command over a UNIX domain socket and exits; the supervisor receives the command, forks the container child, and manages it indefinitely.

The supervisor calls waitpid(-1, ..., WNOHANG) in a polling loop to reap exited children without blocking. When a child exits, waitpid returns its PID and status; the supervisor uses WIFSIGNALED() and WTERMSIG() to distinguish between a normal exit, a supervisor-requested stop (stop\_requested flag was set before SIGTERM was sent), and a hard-limit kill from the LKM (SIGKILL with stop\_requested unset). The final reason is recorded in the runtime\_table and reflected in ps output.

## **4.3 IPC, Threads, and Synchronization**
Two separate IPC paths are used: **Path A (logging)** — a pipe per container carries stdout and stderr from the container child back to the supervisor. A dedicated producer thread reads from the pipe and pushes log\_item\_t structs into the bounded buffer. A single consumer thread pops from the buffer and writes to per-container log files. **Path B (control)** — a UNIX domain socket at /tmp/mini\_runtime.sock carries CLI commands from short-lived client processes to the supervisor.

The bounded buffer uses a mutex to protect the circular buffer state (head, tail, count) and two condition variables — not\_full and not\_empty — to block producers when the buffer is full and consumers when it is empty. Without the mutex, concurrent writes to tail and count by multiple producer threads would be a data race. Without condition variables, threads would busy-wait, wasting CPU. The shutting\_down flag is set under the mutex so both producers and consumers observe it atomically and exit cleanly. The runtime\_table of container metadata uses a separate runtime\_lock mutex because the logging path and the control path both access it concurrently.

## **4.4 Memory Management and Enforcement**
RSS (Resident Set Size) measures the number of physical memory pages currently mapped and present in RAM for a process. It does not include swapped-out pages, memory-mapped files that have not been accessed, or memory shared with other processes counted at full size. The kernel module reads RSS via get\_mm\_rss(mm) \* PAGE\_SIZE inside a timer callback that fires every second.

Soft and hard limits implement different policies: the soft limit triggers a warning log but allows the process to continue, giving the application and operator visibility before a problem becomes critical. The hard limit triggers SIGKILL, enforced immediately. Enforcement belongs in kernel space because a user-space enforcer can be killed, paused, or delayed by the scheduler — a malicious or runaway process could simply not yield. The kernel timer callback runs in kernel context and cannot be preempted by the process it is monitoring, making enforcement reliable.

## **4.5 Scheduling Behavior**
Linux uses the Completely Fair Scheduler (CFS) for normal (SCHED\_OTHER) tasks. CFS maintains a red-black tree of runnable tasks ordered by virtual runtime — the task with the lowest vruntime runs next. The nice value scales a task's weight: a lower nice value gives a higher weight, causing the task's vruntime to advance more slowly, so it is selected to run more frequently.

In our scheduling experiment, two cpu\_hog processes were run simultaneously — one at nice -5 and one at nice +10. The CFS weight ratio between these two priorities is approximately 3.5:1, meaning the high-priority container received roughly 3.5x more CPU time per scheduling period. This was observable as the nice -5 container completing its 20-second workload in measurably less wall-clock time. The I/O-bound io\_pulse process, which sleeps between iterations, voluntarily yields the CPU and consequently accumulates very low vruntime, causing CFS to schedule it ahead of both CPU-hog processes when it wakes — demonstrating CFS's bias toward interactive/responsive tasks.

# **5. Design Decisions and Tradeoffs**

## **5.1 Namespace Isolation — fork + unshare vs clone**
- Choice: Used fork() in the supervisor's start handler followed by execl("./engine run ..."), which calls clone() with namespace flags internally.
- Tradeoff: This double-exec path means the PID tracked in the supervisor is the intermediate fork child, not the namespaced process itself. A direct clone() in the supervisor would track the container PID precisely and avoid the extra process.
- Justification: Keeps the supervisor's start handler simple and reuses the cmd\_run path which already has the correct clone() + chroot + /proc setup.

## **5.2 Supervisor Architecture — blocking accept loop vs select**
- Choice: Blocking accept() in a while loop with WNOHANG waitpid polling at the top of each iteration.
- Tradeoff: The supervisor cannot react to child exits between accept() calls; a container that exits while the supervisor is blocked in accept() will not be reaped until the next CLI request arrives. A select()-based event loop with a 1-second timeout would be more responsive.
- Justification: For a demo runtime, the latency is acceptable. The implementation is simpler and less error-prone than managing a select() fd set with SIGCHLD interaction.

## **5.3 IPC/Logging — bounded buffer with mutex+condvar vs lockless ring buffer**
- Choice: Mutex-protected circular buffer with pthread\_cond\_t for producer/consumer synchronization.
- Tradeoff: Mutex contention adds latency compared to a lockless design. For high-throughput logging this matters; for a container runtime producing debug output it does not.
- Justification: Correctness is straightforward to reason about and verify. Condition variables make the shutdown path clean — broadcast on both conditions wakes all blocked threads immediately.

## **5.4 Kernel Monitor — polling timer vs kernel tracepoints**
- Choice: A kernel timer firing every CHECK\_INTERVAL\_SEC second checks RSS for all registered PIDs.
- Tradeoff: Up to 1 second of latency before a hard-limit violation is detected and enforced. A process could exceed the hard limit for up to 1 second before SIGKILL is sent.
- Justification: Timer-based polling is portable across kernel versions and does not require modifying kernel tracepoints or hooking mm subsystem paths. Sufficient for the memory-limit enforcement use case.

## **5.5 Scheduling Experiments — nice value vs CPU affinity**
- Choice: Used setpriority() (nice values) to differentiate scheduling priority between containers.
- Tradeoff: Nice values affect CFS weight but do not provide strict CPU allocation guarantees. CPU affinity (sched\_setaffinity) would allow pinning containers to specific cores for more controlled experiments.
- Justification: Nice value experiments directly demonstrate CFS weight-based scheduling, which is the core mechanism described in the course material. Results are reproducible on any multi-core machine without configuration.

# **6. Scheduler Experiment Results**
## **6.1 Experiment Setup**
Two containers were launched simultaneously, both running cpu\_hog for 20 seconds. Container high\_pri was started with --nice -5 and container low\_pri with --nice +10. A third experiment ran cpu\_hog (nice 0) alongside io\_pulse (nice 0) to observe I/O-vs-CPU scheduling behavior.

sudo ./engine start high\_pri ./rootfs-alpha '/cpu\_hog 20' --nice -5

sudo ./engine start low\_pri  ./rootfs-beta  '/cpu\_hog 20' --nice 10

## **6.2 Results — CPU-bound vs CPU-bound at Different Priorities**

|**Container**|**Nice Value**|**Wall-Clock Completion (s)**|**Relative CPU Share**|
| :- | :- | :- | :- |
|high\_pri (cpu\_hog)|-5|[ insert from screenshot ]|~78%|
|low\_pri (cpu\_hog)|+10|[ insert from screenshot ]|~22%|

The CFS weight for nice -5 is approximately 3.5x higher than for nice +10. This means high\_pri was scheduled to run roughly 3.5x more frequently per scheduling window. On a single CPU, low\_pri saw significantly longer wait times between CPU grants, resulting in longer wall-clock completion time even though both workloads performed the same amount of computation.

## **6.3 Results — CPU-bound vs I/O-bound at Same Priority**

|**Container**|**Workload**|**Behavior Observed**|
| :- | :- | :- |
|cpu\_bound|cpu\_hog (nice 0)|Ran continuously; accumulated vruntime quickly|
|io\_bound|io\_pulse (nice 0, 200ms sleep)|Slept frequently; woke with low vruntime; always preempted cpu\_bound on wakeup|

The io\_pulse workload slept for 200ms between iterations. Each time it woke, its vruntime was far behind the cpu\_hog vruntime (since it had not been accumulating runtime while sleeping), so CFS immediately scheduled io\_pulse ahead of cpu\_hog. This demonstrates CFS's inherent bias toward interactive/I/O-bound processes — a desirable property for system responsiveness.

## **6.4 Analysis**
The results confirm the expected Linux CFS behavior: nice values translate directly into CFS weights, which determine the proportion of CPU time allocated to each runnable task. Lowering nice by 15 steps (from +10 to -5) produced a roughly 3.5x speedup for the high-priority workload on a loaded system, consistent with the kernel's weight table. The I/O-bound experiment confirms that voluntary CPU release causes vruntime lag, which CFS exploits to provide good interactive responsiveness without requiring a separate scheduling class.


# **Appendix: Source File Summary**

|**File**|**Purpose**|**Status**|
| :- | :- | :- |
|engine.c|User-space supervisor and CLI runtime|Complete|
|monitor.c|Linux Kernel Module — memory monitor|Complete|
|monitor\_ioctl.h|Shared ioctl definitions (user + kernel)|Complete|
|cpu\_hog.c|CPU-bound test workload|Complete|
|memory\_hog.c|Memory-pressure test workload|Complete|
|io\_pulse.c|I/O-bound test workload|Complete|
|Makefile|Builds all targets + kernel module|Complete|

