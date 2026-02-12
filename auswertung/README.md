# Cyclic
```bash
# Get 1000000 samples 10.0.0.1 tap100 IP
cargo rrun --release --bin cyclic tap100 10.0.0.1:8000 100000
```
# Ping/Pong
```bash
# pong ip: 10.0.0.2, get 10000 samples, cycletime 1000 µs 
cargo rrun --release --bin ping tap100 10.0.0.2:8000 100000 1000
```

# Access logs
```bash
python3 -m http.server 
# -> Go to browse
```