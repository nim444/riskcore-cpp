# riskcore-cpp — Distributed Extension Blueprint

## Overview

Add a distributed computing layer to the existing `riskcore-cpp` project.
The risk engine already computes VaR, Greeks, and Sharpe locally.
This extension scales it horizontally — producer publishes jobs to NATS or Solace,
multiple C++ worker pods pick them up, compute risk, publish results back.
Orchestrated on Kubernetes via minikube.

**Do NOT modify any existing files** unless explicitly stated below.
All new code lives under new directories: `distributed/`, `k8s/`, `docker/`.

---

## New directory structure to add

```
riskcore-cpp/                         ← existing repo root
├── ... (all existing files unchanged)
├── distributed/
│   ├── producer.cpp                  # publishes risk jobs to broker
│   ├── worker.cpp                    # subscribes, runs risk engine, publishes results
│   ├── collector.cpp                 # aggregates results, prints benchmark stats
│   └── broker_client.h              # unified NATS/Solace interface
├── docker/
│   ├── Dockerfile.worker
│   ├── Dockerfile.producer
│   └── Dockerfile.collector
├── k8s/
│   ├── namespace.yaml
│   ├── nats-deployment.yaml
│   ├── worker-deployment.yaml
│   ├── producer-job.yaml
│   ├── collector-deployment.yaml
│   └── hpa.yaml
├── scripts/
│   ├── deploy.sh
│   ├── benchmark.sh
│   └── teardown.sh
└── docker-compose.distributed.yml    # local dev without k8s
```

**Existing files to modify** (minimal changes only):
- `CMakeLists.txt` — add 3 new build targets: `producer`, `worker`, `collector`
- `README.md` — add distributed section at bottom

---

## Step 1 — Broker client abstraction

**File:** `distributed/broker_client.h`

Unified interface so same worker code works with NATS and Solace:

```cpp
#pragma once
#include <functional>
#include <string>
#include <memory>

using MessageCallback = std::function<void(const std::string& subject,
                                            const std::string& payload)>;
class BrokerClient {
public:
    virtual ~BrokerClient() = default;
    virtual bool connect(const std::string& url) = 0;
    virtual bool publish(const std::string& subject,
                         const std::string& payload) = 0;
    virtual bool subscribe(const std::string& subject,
                           MessageCallback cb) = 0;
    virtual void run() = 0;
    virtual void disconnect() = 0;
    virtual std::string name() const = 0;
};

std::unique_ptr<BrokerClient> makeBroker(const std::string& type);
```

Implement two concrete classes in the same file:

**NatsBrokerClient** — uses `nats.h` from cnats:
```cpp
// Key calls:
natsConnection_ConnectTo(&conn, url.c_str());
natsConnection_PublishString(conn, subject.c_str(), payload.c_str());
natsConnection_Subscribe(&sub, conn, subject.c_str(), onMsg, nullptr);
natsConnection_Flush(conn);
```

**SolaceBrokerClient** — uses Solace C API:
```cpp
// Key calls — Solace uses '/' topic separator not '.'
solClient_initialize(SOLCLIENT_LOG_DEFAULT_FILTER, nullptr);
solClient_session_create(props, ctx, &session, nullptr, 0);
solClient_session_connect(session);
solClient_session_sendMsg(session, msg);
solClient_session_topicSubscribeExt(session, flags, topic);
// Solace topic format: "taskflow/jobs/risk" not "jobs.risk"
```

`makeBroker("nats")` returns `NatsBrokerClient`.
`makeBroker("solace")` returns `SolaceBrokerClient`.

---

## Step 2 — Job and result structs

Add to **existing** `src/models.h` — append at the bottom, do not remove anything:

```cpp
// ── Distributed extension structs ──────────────────────────

struct RiskJob {
    std::string job_id;
    std::string ticker;
    std::string side;        // "LONG" or "SHORT"
    int quantity;
    double entry_price;
    long long submitted_at;  // epoch ms
};

struct JobResult {
    std::string job_id;
    std::string ticker;
    std::string worker_id;
    double var_95;
    double sharpe;
    double delta;
    double calc_time_ms;
    long long completed_at;
    bool success;
    std::string error;
};

struct BenchmarkStats {
    int total_jobs;
    int completed;
    int failed;
    double total_time_ms;
    double throughput;        // jobs/sec
    double avg_calc_time_ms;
    int worker_count;
};
```

---

## Step 3 — Producer

**File:** `distributed/producer.cpp`

CLI: `./producer --broker nats --jobs 1000`

```
--broker   nats | solace  (default: nats)
--jobs     N               (default: 1000)
--url      broker URL      (default: nats://localhost:4222)
```

Logic:
- Hardcode ticker pool: IBM, GOOG, NVDA, MSFT, AAPL, TSLA, AMZN, META
- Hardcode last prices matching `data/positions.json`
- For each job: pick random ticker, random side (70% LONG / 30% SHORT),
  random quantity 100–5000, assign sequential job_id `job-0001`
- Serialize to JSON with nlohmann/json
- Publish to subject `jobs.risk` (NATS) or topic `taskflow/jobs/risk` (Solace)
- After all jobs: publish sentinel `{"type":"done","count":N}` to `jobs.control`
- Print: `[producer] dispatched 1000 jobs → jobs.risk`

---

## Step 4 — Worker

**File:** `distributed/worker.cpp`

CLI: `./worker --broker nats --id worker-0 --threads 4`

```
--broker   nats | solace   (default: nats)
--id       worker ID       (default: worker-0)
--threads  thread pool N   (default: 4)
--url      broker URL      (default: nats://localhost:4222)
```

Logic:
- Subscribe to `jobs.risk`
- Use `std::thread` pool of N threads with `std::queue<RiskJob>`,
  `std::mutex`, and `std::condition_variable`
- On each message: deserialize JSON → `RiskJob`
- Build synthetic `MarketData`:
  ```cpp
  // Seed RNG with ticker hash for reproducibility across workers
  std::mt19937 rng(std::hash<std::string>{}(job.ticker));
  std::normal_distribution<double> dist(0.0004, 0.018);
  std::vector<double> returns(252);
  std::generate(returns.begin(), returns.end(), [&]{ return dist(rng); });
  MarketData md{job.ticker, returns, job.entry_price,
                computeAnnualisedVol(returns)};
  ```
- Call existing risk engine functions (already in `src/risk_engine.cpp`):
  ```cpp
  double var = computeVaR95(md, job.quantity);
  Greeks g   = computeGreeks(md.current_price, md.current_price,
                              0.045, 0.25, md.volatility);
  double sh  = computeSharpe(md.daily_returns);
  ```
- Build `JobResult`, serialize, publish to `results.risk`
- Print per job: `[worker-0] job-0042 NVDA SHORT var=$4,521 δ=−0.57 in 0.3ms`

---

## Step 5 — Collector

**File:** `distributed/collector.cpp`

CLI: `./collector --broker nats --expected 1000`

```
--broker    nats | solace  (default: nats)
--expected  N              (default: 1000)
--url       broker URL     (default: nats://localhost:4222)
--timeout   seconds        (default: 10)
```

Logic:
- Subscribe to `results.risk` and `jobs.control`
- Track: total received, success/fail, per-worker counts, calc times
- Every 100 results print: `[collector] 400/1000 (40%) — 23,812 jobs/sec`
- On sentinel or timeout: compute and print `BenchmarkStats`:

```
════════════════════════════════════════════════
  riskcore-cpp — distributed benchmark
════════════════════════════════════════════════
  Jobs dispatched :  1000
  Completed       :  998      Failed: 2
  Worker pods     :  3
  Total time      :  42.1ms
  Throughput      :  23,752 calcs/sec
  Avg calc time   :  0.31ms
────────────────────────────────────────────────
  Per-worker
  worker-0   334 jobs   14.1ms avg
  worker-1   333 jobs   14.0ms avg
  worker-2   333 jobs   14.2ms avg
════════════════════════════════════════════════
```

- Write raw results to `output/distributed_results.json`

---

## Step 6 — Update CMakeLists.txt

Add at the bottom of the **existing** `CMakeLists.txt`:

```cmake
# ── Distributed extension ─────────────────────────────────

find_package(PkgConfig REQUIRED)
pkg_check_modules(NATS REQUIRED libnats)

# Optional Solace — only if SDK present
if(EXISTS "/usr/local/solace/include/solclient/solClient.h")
  set(SOLACE_FOUND TRUE)
  set(SOLACE_INCLUDE "/usr/local/solace/include")
  set(SOLACE_LIB "/usr/local/solace/lib/libsolclient.dylib")
  add_compile_definitions(SOLACE_ENABLED)
endif()

set(DIST_COMMON distributed/broker_client.h src/risk_engine.cpp)

foreach(target producer worker collector)
  add_executable(${target} distributed/${target}.cpp ${DIST_COMMON})
  target_include_directories(${target} PRIVATE src distributed
    ${NATS_INCLUDE_DIRS})
  target_link_libraries(${target} PRIVATE
    nlohmann_json::nlohmann_json
    ${NATS_LIBRARIES}
    pthread)
  target_compile_options(${target} PRIVATE -O2 -Wall)
  if(SOLACE_FOUND)
    target_include_directories(${target} PRIVATE ${SOLACE_INCLUDE})
    target_link_libraries(${target} PRIVATE ${SOLACE_LIB})
  endif()
endforeach()
```

---

## Step 7 — Dockerfiles

**File:** `docker/Dockerfile.worker`

```dockerfile
FROM alpine:3.19 AS builder
RUN apk add --no-cache cmake clang clang-dev make git curl openssl-dev
RUN git clone https://github.com/nats-io/nats.c.git /nats.c && \
    cmake -B /nats.c/build /nats.c -DNATS_BUILD_EXAMPLES=OFF && \
    cmake --build /nats.c/build --parallel
COPY src/ /app/src/
COPY distributed/ /app/distributed/
COPY CMakeLists.txt /app/
RUN cmake -B /app/build /app -DCMAKE_BUILD_TYPE=Release && \
    cmake --build /app/build --target worker --parallel

FROM alpine:3.19
RUN apk add --no-cache libstdc++
COPY --from=builder /app/build/worker /usr/local/bin/worker
COPY --from=builder /nats.c/build/src/libnats.so* /usr/local/lib/
RUN ldconfig /usr/local/lib
ENTRYPOINT ["worker"]
```

Create identical `Dockerfile.producer` and `Dockerfile.collector`
changing only `--target worker` → `--target producer` / `--target collector`
and `ENTRYPOINT ["worker"]` accordingly.

---

## Step 8 — Kubernetes manifests

**File:** `k8s/namespace.yaml`
```yaml
apiVersion: v1
kind: Namespace
metadata:
  name: riskcore
```

**File:** `k8s/nats-deployment.yaml`
```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: nats
  namespace: riskcore
spec:
  replicas: 1
  selector:
    matchLabels: { app: nats }
  template:
    metadata:
      labels: { app: nats }
    spec:
      containers:
      - name: nats
        image: nats:2.10-alpine
        ports:
        - containerPort: 4222
        - containerPort: 8222
---
apiVersion: v1
kind: Service
metadata:
  name: nats
  namespace: riskcore
spec:
  selector: { app: nats }
  ports:
  - name: client
    port: 4222
  - name: monitor
    port: 8222
```

**File:** `k8s/worker-deployment.yaml`
```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: risk-worker
  namespace: riskcore
spec:
  replicas: 3
  selector:
    matchLabels: { app: risk-worker }
  template:
    metadata:
      labels: { app: risk-worker }
    spec:
      containers:
      - name: worker
        image: riskcore/worker:latest
        imagePullPolicy: Never
        args: ["--broker", "nats", "--id", "$(POD_NAME)", "--threads", "4"]
        env:
        - name: POD_NAME
          valueFrom:
            fieldRef:
              fieldPath: metadata.name
        - name: NATS_URL
          value: "nats://nats:4222"
        resources:
          requests: { cpu: "250m", memory: "64Mi" }
          limits:   { cpu: "500m", memory: "128Mi" }
```

**File:** `k8s/producer-job.yaml`
```yaml
apiVersion: batch/v1
kind: Job
metadata:
  name: risk-producer
  namespace: riskcore
spec:
  template:
    spec:
      containers:
      - name: producer
        image: riskcore/producer:latest
        imagePullPolicy: Never
        args: ["--broker", "nats", "--jobs", "1000"]
        env:
        - name: NATS_URL
          value: "nats://nats:4222"
      restartPolicy: Never
```

**File:** `k8s/collector-deployment.yaml`
```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: risk-collector
  namespace: riskcore
spec:
  replicas: 1
  selector:
    matchLabels: { app: risk-collector }
  template:
    metadata:
      labels: { app: risk-collector }
    spec:
      containers:
      - name: collector
        image: riskcore/collector:latest
        imagePullPolicy: Never
        args: ["--broker", "nats", "--expected", "1000"]
        env:
        - name: NATS_URL
          value: "nats://nats:4222"
```

**File:** `k8s/hpa.yaml`
```yaml
apiVersion: autoscaling/v2
kind: HorizontalPodAutoscaler
metadata:
  name: risk-worker-hpa
  namespace: riskcore
spec:
  scaleTargetRef:
    apiVersion: apps/v1
    kind: Deployment
    name: risk-worker
  minReplicas: 1
  maxReplicas: 10
  metrics:
  - type: Resource
    resource:
      name: cpu
      target:
        type: Utilization
        averageUtilization: 60
```

---

## Step 9 — Scripts

**File:** `scripts/deploy.sh`
```bash
#!/bin/bash
set -e
echo "Starting minikube..."
minikube start --cpus=4 --memory=4g

echo "Building images inside minikube..."
eval $(minikube docker-env)
docker build -f docker/Dockerfile.worker    -t riskcore/worker:latest .
docker build -f docker/Dockerfile.producer  -t riskcore/producer:latest .
docker build -f docker/Dockerfile.collector -t riskcore/collector:latest .

echo "Deploying..."
kubectl apply -f k8s/
kubectl wait --for=condition=ready pod \
  -l app=risk-worker -n riskcore --timeout=90s

echo "Done. Run: ./scripts/benchmark.sh 3 1000"
```

**File:** `scripts/benchmark.sh`
```bash
#!/bin/bash
WORKERS=${1:-3}
JOBS=${2:-1000}
echo "Scaling to $WORKERS workers..."
kubectl scale deployment risk-worker --replicas=$WORKERS -n riskcore
kubectl wait --for=condition=ready pod \
  -l app=risk-worker -n riskcore --timeout=30s
kubectl delete job risk-producer -n riskcore --ignore-not-found
kubectl create job risk-producer \
  --image=riskcore/producer:latest -n riskcore \
  -- ./producer --broker nats --jobs $JOBS
echo "Watching collector..."
kubectl logs -f -l app=risk-collector -n riskcore
```

**File:** `scripts/teardown.sh`
```bash
#!/bin/bash
kubectl delete namespace riskcore
minikube stop
```

---

## Step 10 — docker-compose.distributed.yml

Local dev without Kubernetes — for rapid iteration:

```yaml
version: '3.8'
services:
  nats:
    image: nats:2.10-alpine
    ports: ["4222:4222", "8222:8222"]

  collector:
    build: { context: ., dockerfile: docker/Dockerfile.collector }
    environment: { NATS_URL: nats://nats:4222 }
    depends_on: [nats]
    command: ["--broker", "nats", "--expected", "1000"]

  worker:
    build: { context: ., dockerfile: docker/Dockerfile.worker }
    environment: { NATS_URL: nats://nats:4222 }
    depends_on: [nats]
    command: ["--broker", "nats", "--id", "worker"]
    deploy:
      replicas: 3

  producer:
    build: { context: ., dockerfile: docker/Dockerfile.producer }
    environment: { NATS_URL: nats://nats:4222 }
    depends_on: [nats, worker, collector]
    command: ["--broker", "nats", "--jobs", "1000"]
```

Run: `docker compose -f docker-compose.distributed.yml up --scale worker=3`

---

## Step 11 — Solace setup (enterprise broker)

```bash
# Pull and start Solace PubSub+ (free developer edition)
docker run -d -p 8080:8080 -p 55554:55555 \
  --shm-size=2g \
  --env username_admin_globalaccesslevel=admin \
  --env username_admin_password=admin \
  --name=solace \
  solace/solace-pubsub-standard

# Verify: open http://localhost:8080 (admin/admin)

# Download Solace C API from https://solace.com/downloads/
# Extract to /usr/local/solace/
# Then rebuild: cmake -B build && cmake --build build --parallel
```

Run with Solace:
```bash
./collector --broker solace --expected 1000 --url tcp://localhost:55554 &
./worker    --broker solace --id worker-0   --url tcp://localhost:55554 &
./producer  --broker solace --jobs 1000     --url tcp://localhost:55554
```

Solace topic format uses `/` not `.`:
- NATS:   `jobs.risk`, `results.risk`, `jobs.control`
- Solace: `taskflow/jobs/risk`, `taskflow/results/risk`, `taskflow/jobs/control`

---

## Step 12 — README additions

Append this section to the **existing** `README.md`:

````markdown
## Distributed Mode

Scale the risk engine across multiple worker nodes using
[NATS](https://nats.io) (dev) or
[Solace PubSub+](https://solace.com) (enterprise).

### Quick start (local)

```bash
# Install NATS
brew install nats-server cnats

# Build distributed targets
cmake -B build && cmake --build build --parallel

# Run locally (3 terminals)
nats-server &
./build/collector --broker nats --expected 1000 &
./build/worker    --broker nats --id worker-0 &
./build/worker    --broker nats --id worker-1 &
./build/worker    --broker nats --id worker-2 &
./build/producer  --broker nats --jobs 1000
```

### Kubernetes (minikube)

```bash
minikube start --cpus=4 --memory=4g
./scripts/deploy.sh

# Benchmark at 3 workers
./scripts/benchmark.sh 3 1000

# Scale to 10 and re-benchmark
./scripts/benchmark.sh 10 1000
```

### Benchmark results

| Workers | Jobs | Time   | Throughput       |
|---------|------|--------|------------------|
| 1       | 1000 | 120ms  | ~8,300 calcs/sec |
| 3       | 1000 | 42ms   | ~23,800 calcs/sec|
| 10      | 1000 | 13ms   | ~76,900 calcs/sec|

### Solace (enterprise broker)

```bash
docker run -d -p 8080:8080 -p 55554:55555 --shm-size=2g \
  --env username_admin_globalaccesslevel=admin \
  --env username_admin_password=admin \
  solace/solace-pubsub-standard

./build/worker    --broker solace --url tcp://localhost:55554 &
./build/producer  --broker solace --jobs 1000 --url tcp://localhost:55554
```

### Architecture

```
producer → NATS/Solace → worker-0 ─┐
                       → worker-1 ─┤→ collector → benchmark stats
                       → worker-N ─┘
```
````

---

## Build order for Claude Code

Execute in this exact order:

1. Append distributed structs to `src/models.h`
2. `distributed/broker_client.h` — full NATS + Solace implementation
3. `distributed/producer.cpp`
4. `distributed/worker.cpp`
5. `distributed/collector.cpp`
6. Append distributed targets to `CMakeLists.txt`
7. `docker/Dockerfile.worker`
8. `docker/Dockerfile.producer`
9. `docker/Dockerfile.collector`
10. `k8s/namespace.yaml`
11. `k8s/nats-deployment.yaml`
12. `k8s/worker-deployment.yaml`
13. `k8s/producer-job.yaml`
14. `k8s/collector-deployment.yaml`
15. `k8s/hpa.yaml`
16. `scripts/deploy.sh` + `benchmark.sh` + `teardown.sh`
17. `docker-compose.distributed.yml`
18. Append distributed section to `README.md`

**Verify with local test first:**
```bash
brew install cnats nats-server
cmake -B build && cmake --build build --parallel
nats-server &
./build/collector --broker nats --expected 20 &
./build/worker    --broker nats --id w0 &
./build/worker    --broker nats --id w1 &
./build/producer  --broker nats --jobs 20
# Should print benchmark table
```

**Then Kubernetes:**
```bash
./scripts/deploy.sh
./scripts/benchmark.sh 3 1000
./scripts/benchmark.sh 10 1000
# Fill in README benchmark table with real numbers
```

---

## Notes for Claude Code

- Do NOT touch `src/main.cpp`, `src/ws_server.cpp`, `src/data_loader.cpp`,
  or `web/index.html` — existing engine unchanged
- Worker reuses `computeVaR95`, `computeGreeks`, `computeSharpe` from
  `src/risk_engine.cpp` — include path is `../src/risk_engine.h`
- Solace compilation is conditional — if SDK not found, builds NATS only,
  `--broker solace` prints "Solace SDK not found, install from solace.com/downloads"
- Thread pool pattern: `std::queue<RiskJob>` protected by `std::mutex`,
  workers wait on `std::condition_variable`, main thread enqueues on message receipt
- Worker ID in Kubernetes comes from `POD_NAME` env var —
  check `getenv("POD_NAME")` before using `--id` flag value
- Benchmark throughput = `completed_jobs / total_time_seconds` — print this prominently
