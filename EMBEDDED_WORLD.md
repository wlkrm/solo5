# Run PING-PONG

Ping sends the whole message with the populated ping_send to pong.
Pong populates the fileds and sends back the whole message.

```typescript
[PING] -msg-> [PONG] -msg-> [PING]

type Message = [
    ping_send: u64;   /* (nanosecs, CLOCK_MONOTONIC)*/
    pong_recv: u64;  /* (nanosecs, CLOCK_MONOTONIC)*/
    pong_senv: u64;    /* (nanosecs, CLOCK_MONOTONIC)*/
    ping_recv: u64;    /* (nanosecs, CLOCK_MONOTONIC)*/
]
```

In one terminal
```bash
bash run_pong.sh
```
In another terminal
```bash
cd auswertung
# pong ip: 10.0.0.2, get 10000 samples, cycletime 1000 µs 
cargo rrun --release --bin ping tap100 10.0.0.2:8000 100000 1000
```

# Run Cyclic Bench (Wakeup latency and outbound message latency)
Memory layout in message (big endian):
```typescript
type Message = [
    planned_wakeup_time: u64;   /* (nanosecs, CLOCK_MONOTONIC)*/
    effectiv_wakeup_time: u64;  /* (nanosecs, CLOCK_MONOTONIC)*/
    effectiv_send_time: u64;    /* (nanosecs, CLOCK_MONOTONIC)*/
    effectiv_recv_time: u64;    /* (nanosecs, CLOCK_MONOTONIC)*/
]
```

In one terminal
```bash
bash run_cyclic.sh
```
In another terminal
```bash
cd auswertung
# Get 1000000 samples 10.0.0.1 tap100 IP
cargo rrun --release --bin cyclic tap100 10.0.0.1:8000 100000
```
# Look at graphs
```bash
cd auswertung
python3 -m http.server
# Head over to browser and look at the generated html files in ./auswertung
```