#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

uint32 nice_to_weight[40] = {
  /* 0 */ 88761, /* 1 */ 71755, /* 2 */ 56483, /* 3 */ 46273, /* 4 */ 36291,
  /* 5 */ 29154, /* 6 */ 23254, /* 7 */ 18705, /* 8 */ 14949, /* 9 */ 11916,
  /*10*/  9548,  /*11*/  7620,  /*12*/  6100,  /*13*/  4904,  /*14*/  3906,
  /*15*/  3121,  /*16*/  2501,  /*17*/  1991,  /*18*/  1586,  /*19*/  1277,
  /*20*/  1024,  /*21*/   820,  /*22*/   655,  /*23*/   526,  /*24*/   423,
  /*25*/   335,  /*26*/   272,  /*27*/   215,  /*28*/   172,  /*29*/   137,
  /*30*/   110,  /*31*/    87,  /*32*/    70,  /*33*/    56,  /*34*/    45,
  /*35*/    36,  /*36*/    29,  /*37*/    23,  /*38*/    18,  /*39*/    15,
};


struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    char *pa = kalloc();
    if (pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int)(p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void procinit(void)
{
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->kstack = KSTACK((int)(p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc *
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int allocpid()
{
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc *
allocproc(void)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->state == UNUSED)
    {
      goto found;
    }
    else
    {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->nice = 20;
  p->state = USED;
  p->runtime   = 0;
  p->vruntime  = 0;
  p->vdeadline = 0;
  p->timeslice = 5;
  p->is_eligible = 1;

  // Allocate a trapframe page.
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if (p->trapframe)
    kfree((void *)p->trapframe);
  p->trapframe = 0;
  if (p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if (pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE,
               (uint64)trampoline, PTE_R | PTE_X) < 0)
  {
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if (mappages(pagetable, TRAPFRAME, PGSIZE,
               (uint64)(p->trapframe), PTE_R | PTE_W) < 0)
  {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
void userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if (n > 0)
  {
    if (sz + n > TRAPFRAME)
    {
      return -1;
    }
    if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0)
    {
      return -1;
    }
  }
  else if (n < 0)
  {
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0)
  {
    return -1;
  }

  // Copy user memory from parent to child.
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0)
  {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);

  np->vruntime = p->vruntime;
  np->nice = p->nice;
  np->runtime = 0;
  np->timeslice = 5;
  np->vdeadline = np->vruntime + 5 * 1024 / nice_to_weight[np->nice];
  np->is_eligible = 1;

  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void reparent(struct proc *p)
{
  struct proc *pp;

  for (pp = proc; pp < &proc[NPROC]; pp++)
  {
    if (pp->parent == p)
    {
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void kexit(int status)
{
  struct proc *p = myproc();

  if (p == initproc)
    panic("init exiting");

  // Close all open files.
  for (int fd = 0; fd < NOFILE; fd++)
  {
    if (p->ofile[fd])
    {
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (pp = proc; pp < &proc[NPROC]; pp++)
    {
      if (pp->parent == p)
      {
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if (pp->state == ZOMBIE)
        {
          // Found one.
          pid = pp->pid;
          if (addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                   sizeof(pp->xstate)) < 0)
          {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || killed(p))
    {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock); // DOC: wait-sleep
  }
}

void scheduler(void)
{
  struct proc *p;
  struct proc *chosen;
  struct cpu *c = mycpu();
  uint64 v0;
  uint64 sum_w;
  uint64 sum_vw;

  c->proc = 0;
  for (;;)
  {
    intr_on();
    intr_off();

    // 매 루프마다 초기화
    chosen = 0;
    v0 = ~0ULL;
    sum_w = 0;
    sum_vw = 0;

    // EEVDF: 1-1단계 - v0(최소 vruntime) 계산 (AI was used)
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE || p->state == RUNNING)
      {
        if (p->vruntime < v0)
          v0 = p->vruntime;
      }
      release(&p->lock);
    }

    if (v0 == ~0ULL)
      v0 = 0;

    // EEVDF: 1-2단계 - sum_w, sum_vw 계산 (AI was used)
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE || p->state == RUNNING)
      {
        sum_w += nice_to_weight[p->nice];
        sum_vw += (p->vruntime - v0) * nice_to_weight[p->nice];
      }
      release(&p->lock);
    }

    // EEVDF: 2단계 - eligibility 계산 및 업데이트 (AI was used)
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE || p->state == RUNNING)
      {
        uint64 rhs = (p->vruntime - v0) * sum_w;
        p->is_eligible = (sum_vw >= rhs) ? 1 : 0;
      }
      release(&p->lock);
    }

    // EEVDF: 3단계 - eligible 중 vdeadline 가장 작은 프로세스 선택 (AI was used)
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE && p->is_eligible)
      {
        if (chosen == 0 || p->vdeadline < chosen->vdeadline)
          chosen = p;
      }
      release(&p->lock);
    }

    // eligible 없으면 전체 RUNNABLE 중 vdeadline 가장 작은 것 선택 (starvation 방지) (AI was used)
    if (chosen == 0)
    {
      for (p = proc; p < &proc[NPROC]; p++)
      {
        acquire(&p->lock);
        if (p->state == RUNNABLE)
        {
          if (chosen == 0 || p->vdeadline < chosen->vdeadline)
            chosen = p;
        }
        release(&p->lock);
      }
    }

    // EEVDF: 선택된 프로세스 실행 (AI was used)
    if (chosen != 0)
    {
      acquire(&chosen->lock);
      if (chosen->state == RUNNABLE)  // 락 재획득 후 상태 재확인
      {
        chosen->state = RUNNING;
        c->proc = chosen;
        swtch(&c->context, &chosen->context);
        c->proc = 0;
      }
      release(&chosen->lock);
    }
    else
    {
      asm volatile("wfi");
    }
  }
}
// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched RUNNING");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (first)
  {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){"/init", 0});
    if (p->trapframe->a0 == -1)
    {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Sleep on channel chan, releasing condition lock lk.
// Re-acquires lk when awakened.
void sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock); // DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on channel chan.
// Caller should hold the condition lock.
void wakeup(void *chan)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p != myproc())
    {
      acquire(&p->lock);
      if (p->state == SLEEPING && p->chan == chan)
      {
        // EEVDF: timeslice 리셋, vdeadline 및 eligibility 재계산
        p->timeslice = 5;
        p->vdeadline = p->vruntime + 5 * 1024 / nice_to_weight[p->nice];
        p->is_eligible = 1;

        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int kkill(int pid)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->pid == pid)
    {
      p->killed = 1;
      if (p->state == SLEEPING)
      {
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int killed(struct proc *p)
{
  int k;

  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if (user_dst)
  {
    return copyout(p->pagetable, dst, src, len);
  }
  else
  {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if (user_src)
  {
    return copyin(p->pagetable, dst, src, len);
  }
  else
  {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void procdump(void)
{
  static char *states[] = {
      [UNUSED] "unused",
      [USED] "used",
      [SLEEPING] "sleep ",
      [RUNNABLE] "runble",
      [RUNNING] "run   ",
      [ZOMBIE] "zombie"};
  struct proc *p;
  char *state;

  printf("\n");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

int getnice(int pid)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->pid == pid)
    {
      int nice = p->nice;
      release(&p->lock);
      return nice;
    }
    release(&p->lock);
  }
  return -1;
}

int setnice(int pid, int value)
{
  struct proc *p;

  // Validate the nice value BEFORE searching for the process.
  // Valid range is 0–39. Anything outside this is rejected.
  if (value < 0 || value > 39)
    return -1;

  // Walk the global process table to find the matching pid.
  for (p = proc; p < &proc[NPROC]; p++)
  {

    // Acquire the lock before reading or writing any field of p.
    // This prevents race conditions with other CPUs.
    acquire(&p->lock);

    if (p->pid == pid)
    {
      // Found the process — update its nice value.
      p->nice = value;

      p->vdeadline = p->vruntime + 5 * 1024 / nice_to_weight[p->nice];
      p->is_eligible = 1;

      release(&p->lock);
      return 0; // Success
    }

    release(&p->lock);
  }

  return -1;
}

void ps(int pid) //ai was used
{
  struct proc *p;
  int found = 0;

  static char *states[] = {
      [UNUSED]   "unused",
      [USED]     "used",
      [SLEEPING] "sleeping",
      [RUNNABLE] "runnable",
      [RUNNING]  "running",
      [ZOMBIE]   "zombie"
  };

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);

    if (p->state == UNUSED) {
      release(&p->lock);
      continue;
    }

    if (pid == 0 || p->pid == pid) {
      // 헤더는 출력할 프로세스가 있을 때만 한 번만 찍기
      if (found == 0) {
        printf("name\tpid\tstate\tpriority\trt/w\t\truntime\t\tvruntime\tvdeadline\teligible\ttotaltick\n");
        found = 1;
      }

      char *state;
      if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
        state = states[p->state];
      else
        state = "???";

      uint64 runtime_w = p->runtime * 1000 / nice_to_weight[p->nice];

      // %-10s 대신 \t 으로 간격 맞추기
      printf("%s\t%d\t%s\t%d\t\t%lu\t\t%lu\t\t%lu\t\t%lu\t\t%d\t\t%lu\n",
        p->name,
        p->pid,
        state,
        p->nice,
        runtime_w,
        p->runtime   * 1000,
        p->vruntime  * 1000,
        p->vdeadline * 1000,
        p->is_eligible,
        (uint64)ticks * 1000
);
    }

    release(&p->lock);
  }
}
// suspends execution until the specified process terminates.
// Returns 0 when the specified process terminates succsessfully.
// -1 if the process does not exist or the calling process does not have pemission to wait for it.
int waitpid(int pid)
{
  struct proc *p;
  struct proc *myp = myproc();
// Check if the target process exists
  int found = 0;
//AI was used
  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->pid == pid)
    {
      if (p->parent != myp)
      {
        
        release(&p->lock);
        return -1; // Permission denied
      }
      found = 1;
      release(&p->lock);
      break;
    }
    release(&p->lock);
  }

  if (!found)
    return -1;

  // Spin-wait until the target process is in ZOMBIE or UNUSED state
  for (;;)
  {
    int done = 0;

    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->pid == pid)
      {
        if (p->state == ZOMBIE || p->state == UNUSED)
        {
          done = 1;
        }
        release(&p->lock);
        break;
      }
      release(&p->lock);
    }

    if (done)
      return 0;

    yield();
  }
}

