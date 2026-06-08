# CPU 스케줄링 시뮬레이터 (CPU Scheduling Simulator)

운영체제 텀 프로젝트. 여러 CPU 스케줄링 알고리즘을 하나의 시뮬레이터로 구현하고,
동일한 입력 데이터에 대해 성능을 비교한다. C 언어로 작성되었으며 Linux 환경에서
컴파일·실행한다.

## 개요

- 시간 틱(tick) 단위로 동작하는 이산 시뮬레이션 엔진
- 입출력(I/O) 동작을 포함한 프로세스 모델 (I/O 발생 시점과 횟수 모두 난수)
- 정책 무관 단일 엔진 구조: 도착·I/O·시간 진행 같은 공통 동작은 하나의 엔진이
  처리하고, 알고리즘마다 달라지는 부분은 "준비 큐에서 무엇을 고를 것인가"라는
  선택 규칙 하나로 분리
- 모든 알고리즘이 동일한 난수 데이터셋을 공유하므로 공정한 비교가 가능
- Gantt 차트와 평가 지표(평균 대기·반환·응답 시간, CPU 이용률, 처리율, 최대 지각)를 출력

## 구현 알고리즘 (총 11종)

| 알고리즘 | 선택 기준 | 선점 |
|----------|-----------|:----:|
| FCFS | 준비 큐의 front (도착 순, FIFO) | X |
| SJF (비선점) | 남은 CPU 시간 최소 | X |
| SRTF (선점 SJF) | 남은 CPU 시간 최소 | O |
| Priority (비선점) | priority 최소 (작을수록 높음) | X |
| Priority (선점) | priority 최소 | O |
| Round Robin | 큐 front + 타임 퀀텀(4) | O |
| Aging | 유효 우선순위 최소 (대기 중 상승) | O |
| MLQ | 큐 레벨 최소 (Q0=RR, Q1=FCFS, 고정 배정) | O |
| MLFQ | 큐 레벨 최소 (동적, 강등·주기적 부스트) | O |
| HRRN | 응답 비율 (W+S)/S 최대 | X |
| EDF | 마감 시각 최소 (가장 임박) | O |

과제 필수 6종(FCFS, SJF, SRTF, Priority 비선점·선점, Round Robin)에 더해,
Aging, MLQ, MLFQ, HRRN, EDF 다섯 종을 추가로 구현하였다.

## 빌드 및 실행

```bash
make            # gcc -Wall -Wextra -std=c11 -g -o cpu_scheduler cpu_scheduler.c
make run        # 빌드 후 실행
make clean      # 빌드 산출물 삭제
```

Makefile 없이 직접 빌드할 수도 있다.

```bash
gcc -Wall -Wextra -std=c11 -o cpu_scheduler cpu_scheduler.c
./cpu_scheduler
```

실행할 때마다 프로세스 개수와 각 속성이 난수로 새로 생성된다. 출력이 길기 때문에
파일로 저장해서 확인하는 것을 권장한다.

```bash
./cpu_scheduler > output.txt
```

## 출력 구성

1. **Created Processes** — 생성된 프로세스 표 (PID / 도착 / CPU 버스트 / 우선순위 / 마감 / I/O 일정)
2. **알고리즘별 결과** — 각 알고리즘의 Gantt 차트와 프로세스별 대기·반환·응답 시간
3. **Summary** — 11개 알고리즘의 성능 지표 비교 요약표

## 설계 개요

- **상태**: NEW / READY / RUNNING / WAITING / TERMINATED
- **자료구조**: Ready Queue 와 Waiting Queue 를 명시적 배열로 관리. 프로세스는 항상
  정확히 하나의 큐에 있거나 실행 중이며, 이는 상태 전이와 1:1로 대응한다.
- **핵심 함수**
  - `run_scheduler()` — 틱 기반 엔진 (모든 알고리즘이 공유)
  - `sched_better()` — 두 프로세스 중 어느 것을 먼저 실행할지 판정하는 선택 기준
  - `best_ready_pos()` — 준비 큐에서 선택 기준상 가장 우선되는 위치를 반환
  - `is_preemptive()` — 정책의 선점 여부
- **엔진 한 틱 처리 순서**: (0) MLFQ 부스트 → (1) 도착 처리 → (2) I/O 완료 복귀 →
  (3a) 선점 / (3b) 디스패치 → (4) 대기 시간 누적·Aging → (5) I/O 진행 → (6) CPU 실행·상태 전이

## 난수 파라미터 (소스의 `#define`)

| 항목 | 값 | 매크로 |
|------|----|--------|
| 프로세스 수 | 3 ~ 6 | `MIN_PROCESSES`, `MAX_PROCESSES` |
| 도착 시각 | 0 ~ 10 | `ARRIVAL_MAX` |
| CPU 버스트 | 3 ~ 12 | `CPU_BURST_MIN`, `CPU_BURST_MAX` |
| 우선순위 | 1 ~ 5 (작을수록 높음) | `PRIORITY_MAX` |
| I/O 횟수 | 0 ~ min(버스트-1, 4) | `MAX_IO` |
| I/O 버스트 | 1 ~ 5 | `IO_BURST_MIN`, `IO_BURST_MAX` |
| 타임 퀀텀 | 4 | `TIME_QUANTUM` |
| Aging 주기 | 5틱마다 1단계 상승 | `AGING_INTERVAL` |
| MLFQ | 3레벨, 퀀텀 2/4/8, 20틱마다 부스트 | `MLFQ_LEVELS`, `MLFQ_BOOST` |
| 마감 여유 | 최소 완료시간 + 5 ~ 15 | (deadline slack) |

## 평가 지표

- **대기 시간(Waiting)**: 준비 큐에 머문 시간의 합 (실행·I/O 시간 제외)
- **반환 시간(Turnaround)**: 종료 시각 − 도착 시각
- **응답 시간(Response)**: 최초 실행 시각 − 도착 시각
- **CPU 이용률**: (전체 시간 − idle 시간) / 전체 시간
- **처리율(Throughput)**: 완료된 프로세스 수 / 전체 시간
- **최대 지각(Max Lateness)**: max(종료 시각 − 마감). 값이 음수이면 전원이 마감 이전에 완료됨

## I/O 모델

각 프로세스는 실행 도중(누적 CPU 실행량 기준)에 미리 정해진 시점마다 I/O를 수행하며,
이때 CPU를 반납하고 Waiting Queue로 이동한다. I/O는 프로세스마다 독립적으로 진행되는
병렬 모델로, 잔여 I/O 시간이 0이 되면 다시 준비 큐로 복귀한다. I/O의 발생 시점과 횟수가
모두 난수이므로, 동일한 알고리즘이라도 프로세스 간 도착·재진입 패턴이 다양하게 나타난다.

## 파일 구성

| 파일 | 설명 |
|------|------|
| `cpu_scheduler.c` | 시뮬레이터 전체 소스 코드 |
| `Makefile` | 빌드 스크립트 (`make`, `make run`, `make clean`) |
| `README.md` | 프로젝트 설명 |
