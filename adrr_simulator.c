#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CLAMP(x, low, high)                                                    \
  (((x) < (low)) ? (low) : (((x) > (high)) ? (high) : (x)))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX_PROCESSES 1000

// ═══ ENUMS & DATA STRUCTURES ══════════════════════════════
typedef enum {
  ALGO_ADRR,
  ALGO_FP_DRR,
  ALGO_ERRDTQ,
  ALGO_MDRR,
  ALGO_MLFQ,
  ALGO_ADRR_V3
} AlgoType;

typedef struct {
  int PID;
  int arrival_time;
  int burst_time;
  double remaining;
  int CT, TAT, WT, RT;
  bool first_seen;
  int current_queue; // For MLFQ priority levels
} Process;

typedef struct Node {
  Process *p;
  struct Node *next, *prev;
} Node;

typedef struct {
  Node *head, *tail;
  int size;
} Queue;

// ═══ QUEUE LOGIC ══════════════════════════════════════════
Queue *create_q() {
  Queue *q = malloc(sizeof(Queue));
  q->head = q->tail = NULL;
  q->size = 0;
  return q;
}

void enqueue(Queue *q, Process *p) {
  Node *n = malloc(sizeof(Node));
  n->p = p;
  n->next = NULL;
  if (!q->head) {
    n->prev = NULL;
    q->head = q->tail = n;
  } else {
    n->prev = q->tail;
    q->tail->next = n;
    q->tail = n;
  }
  q->size++;
}

Process *dequeue(Queue *q) {
  if (!q->head)
    return NULL;
  Node *t = q->head;
  Process *p = t->p;
  q->head = q->head->next;
  if (q->head)
    q->head->prev = NULL;
  else
    q->tail = NULL;
  free(t);
  q->size--;
  return p;
}

Process* dequeue_smart(Queue* q, int current_time, int wait_limit, int fg_threshold, int* job_type) {
    if (!q->head) return NULL;
    Node* curr = q->head;
    
    Node* best_starving = NULL;
    int max_wait = -1;
    Node* best_fg = NULL;
    double min_fg_remain = 999999;
    Node* best_bg = NULL;
    double min_bg_remain = 999999;
    
    while (curr) {
        int wait_time = current_time - curr->p->arrival_time - (curr->p->burst_time - curr->p->remaining);
        if (wait_time > wait_limit) {
            if (wait_time > max_wait) { max_wait = wait_time; best_starving = curr; }
        }
        if (curr->p->remaining < fg_threshold) {
            if (curr->p->remaining < min_fg_remain) { min_fg_remain = curr->p->remaining; best_fg = curr; }
        } else {
            if (curr->p->remaining < min_bg_remain) { min_bg_remain = curr->p->remaining; best_bg = curr; }
        }
        curr = curr->next;
    }
    
    Node* target = NULL;
    if (best_starving) { target = best_starving; *job_type = 2; } // 2 = Starving
    else if (best_fg) { target = best_fg; *job_type = 1; }        // 1 = Foreground
    else { target = best_bg; *job_type = 0; }                     // 0 = Batch
    
    if (!target) { target = q->head; *job_type = 0; }
    
    if (target->prev) target->prev->next = target->next;
    else q->head = target->next;
    if (target->next) target->next->prev = target->prev;
    else q->tail = target->prev;
    q->size--;
    Process* p = target->p;
    free(target);
    return p;
}

// ═══ HELPER FUNCTIONS ═════════════════════════════════════
int cmp_arrival(const void *a, const void *b) {
  return ((Process *)a)->arrival_time - ((Process *)b)->arrival_time;
}

int cmp_double(const void *a, const void *b) {
  double da = *(double *)a, db = *(double *)b;
  return (da > db) - (da < db);
}

void admit(int time, Process proc[], int n, Queue *q, int *idx) {
  while (*idx < n && proc[*idx].arrival_time <= time) {
    enqueue(q, &proc[*idx]);
    (*idx)++;
  }
}

// ═══ TQ CALCULATIONS (Round Robin Variants) ════════════════
double calculate_tq(Queue *q, double prev_tq, AlgoType algo) {
  int n_q = q->size;
  if (n_q == 0)
    return 0;
  if (n_q == 1)
    return q->head->p->remaining;

  double *bt = malloc(n_q * sizeof(double));
  Node *curr = q->head;
  double sum = 0, hm_sum = 0;

  for (int i = 0; i < n_q; i++) {
    bt[i] = curr->p->remaining;
    sum += bt[i];
    if (bt[i] > 0)
      hm_sum += (1.0 / bt[i]);
    curr = curr->next;
  }
  qsort(bt, n_q, sizeof(double), cmp_double);

  double median =
      (n_q % 2 != 0) ? bt[n_q / 2] : (bt[(n_q - 1) / 2] + bt[n_q / 2]) / 2.0;
  double final_tq = 0;

  switch (algo) {
  case ALGO_MDRR:
    final_tq = median;
    break;
  case ALGO_FP_DRR:
    final_tq = ((sum / n_q) + median) / 2.0;
    break;
  case ALGO_ERRDTQ: {
    int idx = (int)ceil(0.8 * n_q) - 1;
    final_tq = bt[CLAMP(idx, 0, n_q - 1)];
    break;
  }
  case ALGO_ADRR: {
    double HM = (hm_sum > 0) ? (n_q / hm_sum) : 0;
    double AM = sum / n_q;
    double base = (HM + AM) / 2.0;
    double IQR = bt[(int)(3 * n_q / 4.0)] - bt[(int)(n_q / 4.0)];
    double shift = (prev_tq > 0) ? fabs(base - prev_tq) / prev_tq : 0.5;
    double beta = CLAMP(shift * (1.0 / sqrt(n_q)), 0.1, 0.9);
    double smooth = (prev_tq > 0)
                        ? (shift * (base + beta * IQR) + (1 - shift) * prev_tq)
                        : (base + beta * IQR);
    final_tq = CLAMP(smooth, bt[0], bt[n_q - 1]);
    break;
  }
  case ALGO_ADRR_V3: {
    // For Batch tasks: 80th percentile for massive uninterrupted rhythm
    int idx = (int)ceil(0.8 * n_q) - 1;
    final_tq = bt[CLAMP(idx, 0, n_q - 1)];
    final_tq = final_tq * 1.5;
    if (final_tq < 50.0) final_tq = 50.0; // Floor to prevent thrashing
    break;
  }
  default:
    break;
  }
  free(bt);
  return final_tq;
}

// ═══ MLFQ SCHEDULER ═══════════════════════════════════════
void run_mlfq(Process proc[], int n, double *avg_tat, double *avg_wt,
              double *avg_rt, int *cs) {
  qsort(proc, n, sizeof(Process), cmp_arrival);
  Queue *q0 = create_q(), *q1 = create_q(), *q2 = create_q();
  int time = 0, idx = 0, completed = 0;
  *cs = 0;

  while (completed < n) {
    while (idx < n && proc[idx].arrival_time <= time) {
      proc[idx].current_queue = 0;
      enqueue(q0, &proc[idx++]);
    }
    if (q0->size == 0 && q1->size == 0 && q2->size == 0) {
      if (idx < n)
        time = proc[idx].arrival_time;
      continue;
    }
    Process *p;
    int tq;
    if (q0->size > 0) {
      p = dequeue(q0);
      tq = 8;
    } else if (q1->size > 0) {
      p = dequeue(q1);
      tq = 16;
    } else {
      p = dequeue(q2);
      tq = (int)p->remaining;
    } // FCFS for Q2

    (*cs)++;
    if (!p->first_seen) {
      p->RT = time - p->arrival_time;
      p->first_seen = true;
    }
    int slice = MIN(tq, p->remaining);
    time += slice;
    p->remaining -= slice;

    while (idx < n && proc[idx].arrival_time <= time) {
      proc[idx].current_queue = 0;
      enqueue(q0, &proc[idx++]);
    }
    if (p->remaining <= 0) {
      p->CT = time;
      completed++;
    } else {
      if (p->current_queue == 0) {
        p->current_queue = 1;
        enqueue(q1, p);
      } else {
        p->current_queue = 2;
        enqueue(q2, p);
      }
    }
  }
  free(q0);
  free(q1);
  free(q2);
}

// ═══ MASTER SCHEDULER ENGINE ══════════════════════════════
void run_scheduler(Process proc[], int n, AlgoType algo, double *avg_tat,
                   double *avg_wt, double *avg_rt, int *cs) {
  if (algo == ALGO_MLFQ) {
    run_mlfq(proc, n, avg_tat, avg_wt, avg_rt, cs);
  } else {
    qsort(proc, n, sizeof(Process), cmp_arrival);
    Queue *q = create_q();
    int time = 0, idx = 0;
    double tq_prev = 0;
    *cs = 0;
    if (n > 0)
      time = proc[0].arrival_time;
    admit(time, proc, n, q, &idx);
    while (q->size > 0 || idx < n) {
      if (q->size == 0) {
        time = proc[idx].arrival_time;
        admit(time, proc, n, q, &idx);
        continue;
      }
      double tq = calculate_tq(q, tq_prev, algo);
      tq_prev = tq;
      int snap = q->size;
      for (int i = 0; i < snap; i++) {
        Process *p = NULL;
        int job_type = 0;
        if (algo == ALGO_ADRR_V3) {
          // Increase wait_limit to 1000ms to stop long jobs from constantly preempting each other
          p = dequeue_smart(q, time, 1000, 100, &job_type);
        }
        if (!p)
          p = dequeue(q);
        (*cs)++;
        if (!p->first_seen) {
          p->RT = time - p->arrival_time;
          p->first_seen = true;
        }
        
        double current_tq = tq;
        if (algo == ALGO_ADRR_V3) {
            // Foreground tasks (job_type=1) get tightly capped at 100ms.
            // Starving (job_type=2) and Batch (job_type=0) get the massive batch TQ.
            current_tq = (job_type == 1) ? 100.0 : tq; 
        }
        
        double slice = MIN(current_tq, p->remaining);
        if (algo == ALGO_ADRR && p->remaining <= tq * 1.3)
          slice = p->remaining;
        if (algo == ALGO_ADRR_V3 && p->remaining <= current_tq * 2.0)
          slice = p->remaining; // Higher tolerance
        time += (int)ceil(slice);
        p->remaining -= slice;
        admit(time, proc, n, q, &idx);
        if (p->remaining <= 0.0001)
          p->CT = time;
        else
          enqueue(q, p);
      }
    }
    free(q);
  }
  double tTAT = 0, tWT = 0, tRT = 0;
  for (int i = 0; i < n; i++) {
    proc[i].TAT = proc[i].CT - proc[i].arrival_time;
    proc[i].WT = proc[i].TAT - proc[i].burst_time;
    tTAT += proc[i].TAT;
    tWT += proc[i].WT;
    tRT += proc[i].RT;
  }
  *avg_tat = tTAT / n;
  *avg_wt = tWT / n;
  *avg_rt = tRT / n;
}

// ═══ UBUNTU /proc SCANNER ═════════════════════════════════
int fetch_dynamic_workload(Process p[], int max_procs) {
  DIR *dir;
  struct dirent *ent;
  int count = 0;
  long hertz = sysconf(_SC_CLK_TCK);
  if ((dir = opendir("/proc")) == NULL)
    return 0;
  while ((ent = readdir(dir)) != NULL && count < max_procs) {
    if (isdigit(ent->d_name[0])) {
      char path[512];
      snprintf(path, sizeof(path), "/proc/%s/stat", ent->d_name);
      FILE *fp = fopen(path, "r");
      if (fp) {
        char buf[2048];
        if (fgets(buf, sizeof(buf), fp)) {
          unsigned long ut, st;
          char *s = strrchr(buf, ')');
          if (s &&
              sscanf(s + 2,
                     "%*c %*d %*d %*d %*d %*d %*u %*lu %*lu %*lu %*lu %lu %lu",
                     &ut, &st) == 2) {
            int bt = (int)(((ut + st) * 1000) / hertz);
            if (bt > 0) {
              p[count].PID = atoi(ent->d_name);
              p[count].burst_time = bt;
              count++;
            }
          }
        }
        fclose(fp);
      }
    }
  }
  closedir(dir);
  srand(time(NULL));
  for (int i = 0; i < count; i++)
    p[i].arrival_time = rand() % 200;
  return count;
}

// ═══ MAIN ═════════════════════════════════════════════════
int main() {
  Process orig[MAX_PROCESSES], test[MAX_PROCESSES];

  AlgoType types[] = {ALGO_MDRR, ALGO_FP_DRR, ALGO_ERRDTQ,
                      ALGO_MLFQ, ALGO_ADRR,   ALGO_ADRR_V3};
  const char *names[] = {"MDRR",         "F&P",       "ERRDTQ",
                         "3-Level MLFQ", "ADRR v2.0", "ADRR v3.0 (Batch+RT)"};

  double total_tat[6] = {0}, total_wt[6] = {0}, total_rt[6] = {0};
  int total_cs[6] = {0};
  int num_runs = 50;
  int total_procs = 0;

  printf("\n=== RUNNING %d SIMULATIONS ===\n", num_runs);

  for (int run = 0; run < num_runs; run++) {
    int n = fetch_dynamic_workload(orig, MAX_PROCESSES);
    total_procs += n;
    for (int i = 0; i < 6; i++) {
      for (int j = 0; j < n; j++) {
        test[j] = orig[j];
        test[j].remaining = test[j].burst_time;
        test[j].first_seen = false;
      }
      double tat, wt, rt;
      int cs;
      run_scheduler(test, n, types[i], &tat, &wt, &rt, &cs);
      total_tat[i] += tat;
      total_wt[i] += wt;
      total_rt[i] += rt;
      total_cs[i] += cs;
    }
  }

  printf(
      "\n=== AVERAGED BENCHMARK COMPARISON (%d runs, avg %d Processes) ===\n",
      num_runs, total_procs / num_runs);
  printf("%-20s | %-8s | %-8s | %-8s | %-5s\n", "Algorithm", "Avg TAT",
         "Avg WT", "Avg RT", "CS");
  printf(
      "------------------------------------------------------------------\n");

  for (int i = 0; i < 6; i++) {
    printf("%-20s | %-8.2f | %-8.2f | %-8.2f | %-5d\n", names[i],
           total_tat[i] / num_runs, total_wt[i] / num_runs,
           total_rt[i] / num_runs, total_cs[i] / num_runs);
  }
  return 0;
}


