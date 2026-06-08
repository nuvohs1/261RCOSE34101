#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define MIN_PROCESSES  3   
#define MAX_PROCESSES  6   
#define MAX_IO         4   /* max I/O requests per process */

/* process states */
typedef enum {
    P_NEW, P_READY, P_RUNNING, P_WAITING, P_TERMINATED
} ProcessState;

typedef struct {
    /* input attributes (fixed after creation) */
    int pid;
    int arrival_time;   /* arrival time */
    int cpu_burst;      /* total CPU time needed */
    int priority;       /* smaller = higher priority */

    int io_count;                 /* number of I/O requests */
    int io_request_at[MAX_IO];    /* executed-CPU time each I/O fires at */
    int io_burst[MAX_IO];         /* length of each I/O */
    int queue_level;              /* MLQ: fixed queue (0=high, 1=low) */
    int deadline;                 /* EDF absolute deadline */

    /* runtime state (reset before each run) */
    ProcessState state;
    int remaining_cpu;  /* CPU time left */
    int executed_cpu;   /* CPU time done so far (triggers I/O) */
    int next_io;        /* next I/O index */
    int io_remaining;   /* time left in current I/O */
    int eff_priority;   /* aging: effective priority */
    int age_counter;    /* aging: ticks waited in ready */
    int mlfq_level;     /* MLFQ: current level (0=top) */

    /* results */
    int start_time;     /* first CPU time (-1 = none yet) */
    int finish_time;
    int waiting_time;
    int turnaround_time;
    int response_time;
} Process;

/* random value ranges */
#define ARRIVAL_MAX   10
#define CPU_BURST_MIN  3
#define CPU_BURST_MAX 12
#define PRIORITY_MAX   5
#define IO_BURST_MIN   1
#define IO_BURST_MAX   5

/* random int in [lo, hi] */
static int rand_range(int lo, int hi) {
    return lo + rand() % (hi - lo + 1);
}

/* qsort comparator: ascending int */
static int cmp_int(const void *a, const void *b) {
    return (*(const int *)a - *(const int *)b);
}

/* reset runtime fields (input attributes untouched) */
static void reset_runtime(Process *p) {
    p->state          = P_NEW;
    p->remaining_cpu  = p->cpu_burst;
    p->executed_cpu   = 0;
    p->next_io        = 0;
    p->io_remaining   = 0;
    p->eff_priority   = p->priority;
    p->age_counter    = 0;
    p->mlfq_level     = 0;
    p->start_time     = -1;
    p->finish_time    = 0;
    p->waiting_time   = 0;
    p->turnaround_time = 0;
    p->response_time  = -1;
}

/* generate one random process */
static void create_one(Process *p, int pid) {
    p->pid          = pid;
    p->arrival_time = rand_range(0, ARRIVAL_MAX);
    p->cpu_burst    = rand_range(CPU_BURST_MIN, CPU_BURST_MAX);
    p->priority     = rand_range(1, PRIORITY_MAX);
    /* MLQ: top half of priorities -> queue 0, rest -> queue 1 */
    p->queue_level  = (p->priority <= PRIORITY_MAX / 2) ? 0 : 1;

    /* at most min(cpu_burst-1, MAX_IO) I/O requests */
    int max_io_count = p->cpu_burst - 1;
    if (max_io_count > MAX_IO) max_io_count = MAX_IO;
    p->io_count = (max_io_count <= 0) ? 0 : rand_range(0, max_io_count);

    /* pick distinct trigger points in (1, cpu_burst-1), then sort ascending */
    int used[CPU_BURST_MAX] = {0};
    for (int i = 0; i < p->io_count; i++) {
        int t;
        do { t = rand_range(1, p->cpu_burst - 1); } while (used[t]);
        used[t] = 1;
        p->io_request_at[i] = t;
    }
    qsort(p->io_request_at, p->io_count, sizeof(int), cmp_int);

    /* random length for each I/O */
    for (int i = 0; i < p->io_count; i++)
        p->io_burst[i] = rand_range(IO_BURST_MIN, IO_BURST_MAX);

    /* deadline = arrival + CPU + total I/O + slack */
    int io_total = 0;
    for (int i = 0; i < p->io_count; i++) io_total += p->io_burst[i];
    p->deadline = p->arrival_time + p->cpu_burst + io_total + rand_range(5, 15);

    reset_runtime(p);
}

/* generate n random processes */
void create_processes(Process procs[], int n) {
    for (int i = 0; i < n; i++)
        create_one(&procs[i], i + 1);   /* pid starts at 1 */
}

/* print the generated process table */
void print_process_table(const Process procs[], int n) {
    printf("\n===================== Created Processes =====================\n");
    printf("PID  Arrival  CPU  Prio    DL  #IO    IO schedule [at:burst] ...\n");
    printf("------------------------------------------------------------------\n");
    for (int i = 0; i < n; i++) {
        const Process *p = &procs[i];
        printf("P%-3d %6d  %4d  %4d  %4d  %3d   ",
               p->pid, p->arrival_time, p->cpu_burst, p->priority, p->deadline, p->io_count);
        if (p->io_count == 0) {
            printf("(none)");
        } else {
            for (int k = 0; k < p->io_count; k++)
                printf("[%d:%d] ", p->io_request_at[k], p->io_burst[k]);
        }
        printf("\n");
    }
    printf("============================================================\n");
}

#define MAX_TIME 2000      /* max simulated time (safety bound) */
#define TIME_QUANTUM 4     /* RR time slice */
#define AGING_INTERVAL 5   /* aging: boost every N ticks waited */
#define MLFQ_LEVELS    3   /* MLFQ queue levels */
#define MLFQ_BOOST    20   /* MLFQ: boost all to top every N ticks */

typedef enum {
    SCHED_FCFS,            /* first come first served */
    SCHED_SJF,             /* non-preemptive SJF (by remaining CPU) */
    SCHED_SJF_P,           /* preemptive SJF = SRTF */
    SCHED_PRIO,            /* non-preemptive priority */
    SCHED_PRIO_P,          /* preemptive priority */
    SCHED_RR,              /* round robin */
    SCHED_PRIO_AGING,      /* priority + aging */
    SCHED_MLQ,             /* multilevel queue (Q0=RR, Q1=FCFS) */
    SCHED_MLFQ,            /* multilevel feedback queue */
    SCHED_HRRN,            /* highest response ratio next */
    SCHED_EDF              /* earliest deadline first */
} SchedPolicy;

/* one row of the comparison summary */
typedef struct {
    const char *name;
    double avg_wait;
    double avg_tat;
    double avg_resp;
    double cpu_util;     /* CPU utilization (%) */
    double throughput;   /* processes per time unit */
    int    max_late;     /* max(finish - deadline), <0 = all finish in deadline*/
} Metrics;

/* sum of all I/O bursts */
static int total_io_time(const Process *p) {
    int sum = 0;
    for (int i = 0; i < p->io_count; i++)
        sum += p->io_burst[i];
    return sum;
}

/* print a text Gantt chart from the per-tick timeline */
static void print_gantt(const int timeline[], int total, const char *name) {
    printf("\n==================== Gantt Chart (%s) ====================\n", name);
    if (total <= 0) { printf("(no execution)\n\n"); return; }

    /* merge equal consecutive ticks into [pid, end_time] segments */
    int seg_pid[MAX_TIME], seg_end[MAX_TIME], nseg = 0;
    int cur = timeline[0];                          /* pid of the current segment */
    for (int t = 1; t <= total; t++) {
        if (t == total || timeline[t] != cur) {     /* segment boundary at t */
            seg_pid[nseg] = cur;
            seg_end[nseg] = t;
            nseg++;
            if (t < total)
                cur = timeline[t];
        }
    }

    for (int s = 0; s < nseg; s++) {
        if (seg_pid[s] == 0) printf("| IDLE");      /* 0 = idle */
        else                 printf("| P%-3d", seg_pid[s]);
    }
    printf("|\n");

    printf("%-6d", 0);                              /* time axis */
    for (int s = 0; s < nseg; s++) printf("%-6d", seg_end[s]);
    printf("\n\n");
}

/* print per-process metrics + averages, return them for the summary */
static Metrics print_results(const Process procs[], int n, const char *name, int total_time, int idle) {
    printf("-------------------- Results (%s) --------------------\n", name);
    printf("PID  Arrival  CPU   IO  Start  Finish  Wait  TAT  Resp\n");
    double sum_wait = 0.0, sum_tat = 0.0, sum_resp = 0.0;
    int max_late = -1000;   /* sentinel below any real lateness */
    for (int i = 0; i < n; i++) {
        const Process *p = &procs[i];
        printf("P%-3d %6d  %4d %4d  %5d  %6d  %4d %4d  %4d\n",
               p->pid, p->arrival_time, p->cpu_burst, total_io_time(p),
               p->start_time, p->finish_time,
               p->waiting_time, p->turnaround_time, p->response_time);
        sum_wait += p->waiting_time;
        sum_tat  += p->turnaround_time;
        sum_resp += p->response_time;
        int lateness = p->finish_time - p->deadline;   /* >0 = late */
        if (lateness > max_late) max_late = lateness;
    }
    double util = (total_time > 0) ? 100.0 * (total_time - idle) / total_time : 0.0;
    double thru = (total_time > 0) ? (double) n / total_time : 0.0;

    printf("------------------------------------------------------\n");
    printf("Average waiting time    = %.2f\n", sum_wait / n);
    printf("Average turnaround time = %.2f\n", sum_tat  / n);
    printf("CPU utilization = %.1f%%   Throughput = %.3f proc/unit\n", util, thru);
    printf("======================================================\n");

    Metrics m = { name, sum_wait / n, sum_tat / n, sum_resp / n, util, thru, max_late };
    return m;
}

/* policy-specific helpers */

/* MLFQ quantum per level: 2, 4, 8 (lower levels get longer slices) */
static int mlfq_quantum(int level) {
    return 2 << level;
}

/* is this policy preemptive? */
static int is_preemptive(SchedPolicy policy) {
    return policy == SCHED_SJF_P || policy == SCHED_PRIO_P
        || policy == SCHED_PRIO_AGING || policy == SCHED_MLQ
        || policy == SCHED_MLFQ || policy == SCHED_EDF;
}

/* is a preferred over b? if tie, just keep FIFO order */
static int sched_better(SchedPolicy policy, const Process *a, const Process *b) {
    switch (policy) {
        case SCHED_SJF:
        case SCHED_SJF_P:
            return a->remaining_cpu < b->remaining_cpu;   /* shorter job first */
        case SCHED_PRIO:
        case SCHED_PRIO_P:
            return a->priority < b->priority;             /* smaller = higher priority */
        case SCHED_PRIO_AGING:
            return a->eff_priority < b->eff_priority;     /* aged priority */
        case SCHED_MLQ:
            return a->queue_level < b->queue_level;        /* lower queue first */
        case SCHED_MLFQ:
            return a->mlfq_level < b->mlfq_level;           /* lower level first */
        case SCHED_HRRN:
            /* highest (W+S)/S first; cross-multiply to avoid float */
            return (a->waiting_time + a->cpu_burst) * b->cpu_burst
                 > (b->waiting_time + b->cpu_burst) * a->cpu_burst;
        case SCHED_EDF:
            return a->deadline < b->deadline;             /* earliest deadline first */
        case SCHED_FCFS:
        case SCHED_RR:      /* FIFO front */
        default:
            return 0;        /* never reorder: keep FIFO front */
    }
}

/* index in ready[] of the best process to run next */
static int best_ready_pos(SchedPolicy policy, const Process procs[], const int ready[], int rc) {
    int best = 0;
    for (int k = 1; k < rc; k++)
        if (sched_better(policy, &procs[ready[k]], &procs[ready[best]]))
            best = k;
    return best;
}

/* run one algorithm to completion. Tick-based: each loop iteration is one time unit.
   Input attributes stay fixed so all algorithms share the data. (Only reset runtime) */
Metrics run_scheduler(Process procs[], int n, SchedPolicy policy, const char *name) {
    for (int i = 0; i < n; i++)
        reset_runtime(&procs[i]);

    int ready[MAX_PROCESSES];      /* ready queue (indices into procs[]) */
    int rc = 0;                    /* ready queue length */
    int timeline[MAX_TIME];        /* timeline[t] = running pid (0 = idle) */
    int running = -1;              /* running process index (-1 = idle) */
    int done = 0;                  /* terminated count */
    int t = 0;
    int quantum_used = 0;          /* ticks used in current slice */
    int waiting[MAX_PROCESSES];    /* waiting queue (doing I/O) */
    int wc = 0;                    /* waiting queue length */

    while (done < n && t < MAX_TIME) {
        /* (0) MLFQ boost: periodically lift all live processes back to top */
        if (policy == SCHED_MLFQ && t > 0 && t % MLFQ_BOOST == 0) {
            for (int i = 0; i < n; i++)
                if (procs[i].state != P_TERMINATED)
                    procs[i].mlfq_level = 0;
            quantum_used = 0;          /* restart slice */
        }

        /* (1) admit arrivals at time t (pid order) */
        for (int i = 0; i < n; i++)
            if (procs[i].state == P_NEW && procs[i].arrival_time == t) {
                procs[i].state = P_READY;
                ready[rc] = i;
                rc++;
            }

        /* (2) I/O finished: move from waiting queue to ready */
        for (int w = 0; w < wc; ) {
            int idx = waiting[w];
            if (procs[idx].io_remaining == 0) {
                procs[idx].state = P_READY;
                ready[rc++] = idx;
                for (int k = w; k < wc - 1; k++) waiting[k] = waiting[k + 1];
                wc--;                       /* removed; recheck same index */
            } else {
                w++;
            }
        }

        /* (3a) preempt if a ready process beats the running one */
        if (running != -1 && is_preemptive(policy) && rc > 0) {
            int pos = best_ready_pos(policy, procs, ready, rc);
            if (sched_better(policy, &procs[ready[pos]], &procs[running])) {
                procs[running].state = P_READY;
                ready[rc++] = running;
                running = -1;
            }
        }

        /* (3a-RR) quantum used up: send running to back of ready (RR, MLQ Q0) */
        if (running != -1 && quantum_used >= TIME_QUANTUM &&
            (policy == SCHED_RR ||
             (policy == SCHED_MLQ && procs[running].queue_level == 0))) {
            procs[running].state = P_READY;
            ready[rc++] = running;
            running = -1;
        }

        /* (3a-MLFQ) used the whole level quantum -> demote one level */
        if (running != -1 && policy == SCHED_MLFQ &&
            quantum_used >= mlfq_quantum(procs[running].mlfq_level)) {
            if (procs[running].mlfq_level < MLFQ_LEVELS - 1)
                procs[running].mlfq_level++;
            procs[running].state = P_READY;
            ready[rc++] = running;
            running = -1;
        }

        /* (3b) dispatch: pick the best ready process when the CPU is idle */
        if (running == -1 && rc > 0) {
            int pos = best_ready_pos(policy, procs, ready, rc);
            running = ready[pos];
            for (int k = pos; k < rc - 1; k++) ready[k] = ready[k + 1];
            rc--;
            procs[running].state = P_RUNNING;
            quantum_used = 0;          /* new slice */
            if (policy == SCHED_PRIO_AGING) {     /* reset aging on dispatch */
                procs[running].eff_priority = procs[running].priority;
                procs[running].age_counter  = 0;
            }
            if (procs[running].start_time == -1) {
                procs[running].start_time    = t;
                procs[running].response_time = t - procs[running].arrival_time;
            }
        }

        /* (4) ready processes wait one tick; aging boosts priority over time */
        for (int k = 0; k < rc; k++) {
            Process *p = &procs[ready[k]];
            p->waiting_time++;
            if (policy == SCHED_PRIO_AGING) {
                p->age_counter++;
                if (p->age_counter >= AGING_INTERVAL && p->eff_priority > 1) {
                    p->eff_priority--;          /* boost */
                    p->age_counter = 0;
                }
            }
        }

        /* (5) advance I/O by one tick for everyone in the waiting queue */
        for (int w = 0; w < wc; w++)
            if (procs[waiting[w]].io_remaining > 0)
                procs[waiting[w]].io_remaining--;

        /* (6) run the CPU one tick, then check completion / I/O block */
        timeline[t] = (running == -1) ? 0 : procs[running].pid;
        if (running != -1) {
            Process *p = &procs[running];
            p->executed_cpu++;
            p->remaining_cpu--;
            quantum_used++;            /* count toward quantum */

            if (p->remaining_cpu == 0) {                   /* finished */
                p->state = P_TERMINATED;
                p->finish_time = t + 1;
                done++;
                running = -1;
            }
            else if (p->next_io < p->io_count && p->executed_cpu == p->io_request_at[p->next_io]) {
                p->io_remaining = p->io_burst[p->next_io]; /* block for I/O */
                p->next_io++;
                p->state = P_WAITING;
                waiting[wc++] = running;       /* enter waiting queue */
                running  = -1;
            }
        }
        t++;
    }

    for (int i = 0; i < n; i++)
        procs[i].turnaround_time = procs[i].finish_time - procs[i].arrival_time;

    int idle = 0;                          /* idle ticks, for CPU utilization */
    for (int i = 0; i < t; i++)
        if (timeline[i] == 0) idle++;

    print_gantt(timeline, t, name);
    return print_results(procs, n, name, t, idle);
}

/* print the final comparison table */
static void print_summary(const Metrics m[], int count) {
    printf("\n==================== Summary: Algorithm Comparison =============================\n");
    printf("%-26s %8s %8s %8s %7s %8s %8s\n",
           "Algorithm", "AvgWait", "AvgTAT", "AvgResp", "CPU%", "Thrupt", "MaxLate");
    printf("-------------------------------------------------------------------------------\n");
    for (int i = 0; i < count; i++)
        printf("%-26s %8.2f %8.2f %8.2f %7.1f %8.3f %8d\n",
               m[i].name, m[i].avg_wait, m[i].avg_tat,
               m[i].avg_resp, m[i].cpu_util, m[i].throughput, m[i].max_late);
    printf("===============================================================================\n");
}

int main(void) {
    srand((unsigned)time(NULL));   /* seed RNG */

    Process procs[MAX_PROCESSES];
    int n = rand_range(MIN_PROCESSES, MAX_PROCESSES);   /* random process count */

    create_processes(procs, n);
    print_process_table(procs, n);

    Metrics results[11];
    results[0] = run_scheduler(procs, n, SCHED_FCFS,       "FCFS");
    results[1] = run_scheduler(procs, n, SCHED_SJF,        "SJF (non-preemptive)");
    results[2] = run_scheduler(procs, n, SCHED_SJF_P,      "SJF (preemptive)/ SRTF");
    results[3] = run_scheduler(procs, n, SCHED_PRIO,       "Priority (non-preemptive)");
    results[4] = run_scheduler(procs, n, SCHED_PRIO_P,     "Priority (preemptive)");
    results[5] = run_scheduler(procs, n, SCHED_RR,         "Round Robin (RR)");
    results[6] = run_scheduler(procs, n, SCHED_PRIO_AGING, "Priority + Aging");
    results[7] = run_scheduler(procs, n, SCHED_MLQ,        "Multilevel Queue (MLQ)");
    results[8] = run_scheduler(procs, n, SCHED_MLFQ,       "Multilevel Feedback (MLFQ)");
    results[9] = run_scheduler(procs, n, SCHED_HRRN,       "HRRN (response ratio)");
    results[10] = run_scheduler(procs, n, SCHED_EDF,       "EDF (earliest deadline)");

    print_summary(results, 11);

    return 0;
}
