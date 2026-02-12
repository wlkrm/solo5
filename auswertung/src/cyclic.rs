use plotly::common::Mode;
use plotly::{Plot, Scatter};
use std::io;
use std::net::UdpSocket;
use std::os::unix::io::AsRawFd;
use std::sync::mpsc;
use std::thread;
use linux_rt::mman;

fn parse_u64_be(buf: &[u8]) -> u64 {
    let mut bytes = [0u8; 8];
    bytes.copy_from_slice(buf);
    u64::from_be_bytes(bytes)
}

fn bind_to_device(socket: &UdpSocket, iface: &str) -> io::Result<()> {
    let fd = socket.as_raw_fd();
    let iface_cstr = std::ffi::CString::new(iface).map_err(|_| {
        io::Error::new(io::ErrorKind::InvalidInput, "invalid interface")
    })?;
    let ret = unsafe {
        libc::setsockopt(
            fd,
            libc::SOL_SOCKET,
            libc::SO_BINDTODEVICE,
            iface_cstr.as_ptr() as *const libc::c_void,
            iface_cstr.as_bytes_with_nul().len() as libc::socklen_t,
        )
    };
    if ret == 0 {
        Ok(())
    } else {
        Err(io::Error::last_os_error())
    }
}

fn realtime_ns() -> io::Result<u64> {
    let mut ts = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    let rc = unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) };
    if rc != 0 {
        return Err(io::Error::last_os_error());
    }
    Ok((ts.tv_sec as u64)
        .saturating_mul(1_000_000_000)
        .saturating_add(ts.tv_nsec as u64))
}

fn build_series(
    planned_wakeup_time: &[u64],
    effective_wakeup_time: &[u64],
    effective_send_time: &[u64],
    effective_recv_time: &[u64],
) -> (Vec<f64>, Vec<f64>, Vec<f64>) {
    let planned_wakeup_time0 = planned_wakeup_time[0] as i128;

    let mut x = Vec::with_capacity(planned_wakeup_time.len());
    let mut effective_wakeup_time_rel = Vec::with_capacity(planned_wakeup_time.len());
    let mut effective_recv_time_rel = Vec::with_capacity(planned_wakeup_time.len());

    for i in 0..planned_wakeup_time.len() {
        let planned_wakeup_time_i = planned_wakeup_time[i] as i128;
        let effective_wakeup_time_i = effective_wakeup_time[i] as i128;
        let effective_send_time_i = effective_send_time[i] as i128;
        let effective_recv_time_i = effective_recv_time[i] as i128;

        let planned_wakeup_time_delta = planned_wakeup_time_i - planned_wakeup_time0;
        let effective_wakeup_time_delta = effective_wakeup_time_i - planned_wakeup_time_i;
        let effective_recv_time_delta = effective_recv_time_i - effective_send_time_i;

        x.push(planned_wakeup_time_delta as f64 / 1000.0);
        effective_wakeup_time_rel.push(effective_wakeup_time_delta as f64 / 1000.0);
        effective_recv_time_rel.push(effective_recv_time_delta as f64 / 1000.0);
    }

    (x, effective_wakeup_time_rel, effective_recv_time_rel)
}

fn build_inter_packet_latency(effective_recv_time: &[u64]) -> Vec<f64> {
    if effective_recv_time.len() < 2 {
        return Vec::new();
    }

    let mut deltas = Vec::with_capacity(effective_recv_time.len() - 1);
    for i in 1..effective_recv_time.len() {
        let delta_ns = effective_recv_time[i].saturating_sub(effective_recv_time[i - 1]);
        deltas.push(delta_ns as f64 / 1000.0);
    }
    deltas
}

fn build_inter_packet_series(effective_recv_time: &[u64]) -> (Vec<f64>, Vec<f64>) {
    let deltas = build_inter_packet_latency(effective_recv_time);
    let mut x = Vec::with_capacity(deltas.len());
    for i in 0..deltas.len() {
        x.push((i + 1) as f64);
    }
    (x, deltas)
}

fn main() -> io::Result<()> {
    const WRITE_EVERY: usize = 10_000;

    let args: Vec<String> = std::env::args().collect();
    let iface = args.get(1).map(String::as_str).unwrap_or("tap100");
    let bind_addr = args.get(2).map(String::as_str).unwrap_or("10.0.0.1:8000");
    let sample_limit: usize = args
        .get(3)
        .and_then(|s| s.parse().ok())
        .unwrap_or(2000);

    let socket = UdpSocket::bind(bind_addr)?;
    bind_to_device(&socket, iface)?;

    println!(
        "Listening on {} via {} (limit {})",
        bind_addr, iface, sample_limit
    );

    let (tx, rx) = mpsc::channel::<(Vec<u64>, Vec<u64>, Vec<u64>, Vec<u64>)>();
    let writer = thread::spawn(move || {
        while let Ok((planned_wakeup_time, effective_wakeup_time, effective_send_time, effective_recv_time)) = rx.recv() {
            let count = planned_wakeup_time.len();
            let (x, effective_wakeup_time_rel, effective_recv_time_rel) = build_series(&planned_wakeup_time, &effective_wakeup_time, &effective_send_time, &effective_recv_time);
            let (inter_x, inter_packet) = build_inter_packet_series(&effective_recv_time);

            let mut plot = Plot::new();
            plot.add_trace(
                Scatter::new(x.clone(), effective_wakeup_time_rel)
                    .mode(Mode::Lines)
                    .name("wakeup latency"),
            );
            plot.add_trace(
                Scatter::new(x, effective_recv_time_rel)
                    .mode(Mode::Lines)
                    .name("message latency"),
            );

            let html = plot.to_html();
            if let Err(err) = std::fs::write("cyclic_timestamps.html", html) {
                eprintln!("Failed to write cyclic_timestamps.html: {}", err);
                continue;
            }
            println!("Wrote cyclic_timestamps.html ({} samples)", count);

            let mut latency_plot = Plot::new();
            latency_plot.add_trace(
                Scatter::new(inter_x, inter_packet)
                    .mode(Mode::Lines)
                    .name("inter_packet_latency"),
            );
            let latency_html = latency_plot.to_html();
            if let Err(err) = std::fs::write("inter_packet_latency.html", latency_html) {
                eprintln!("Failed to write inter_packet_latency.html: {}", err);
                continue;
            }
            println!("Wrote inter_packet_latency.html ({} samples)", count);
        }
    });

    let mut planned_wakeup_time = Vec::with_capacity(sample_limit);
    let mut effective_wakeup_time = Vec::with_capacity(sample_limit);
    let mut effective_send_time = Vec::with_capacity(sample_limit);
    let mut effective_recv_time = Vec::with_capacity(sample_limit);

    let mut buf = [0u8; 2048];

    mman::mlockall(mman::MmanFlags::MCL_CURRENT | mman::MmanFlags::MCL_FUTURE)?;
    let mut cpu = linux_rt::CpuSet::empty();
    cpu.set(1);
    linux_rt::sched::set_affinity(linux_rt::sched::Pid::this(), cpu)?;
    linux_rt::sched::set_fifo(linux_rt::sched::Pid::this(), 90)?;

    let mut count = 0;
    while planned_wakeup_time.len() < sample_limit {
        count += 1;
        let (len, _addr) = socket.recv_from(&mut buf)?;
        let effective_recv_time_ns = realtime_ns()?;
        if len < 24 {
            eprintln!("Received short packet ({} bytes)", len);
            continue;
        }
        if count < 5 {
            continue;
        }
        planned_wakeup_time.push(parse_u64_be(&buf[0..8]));
        effective_wakeup_time.push(parse_u64_be(&buf[8..16]));
        effective_send_time.push(parse_u64_be(&buf[16..24]));
        effective_recv_time.push(effective_recv_time_ns);

        if planned_wakeup_time.len() % WRITE_EVERY == 0 || planned_wakeup_time.len() == sample_limit {
            if tx
                .send((planned_wakeup_time.clone(), effective_wakeup_time.clone(), effective_send_time.clone(), effective_recv_time.clone()))
                .is_err()
            {
                eprintln!("Writer thread disconnected; skipping HTML output.");
            }
        }
    }

    drop(tx);
    if let Err(err) = writer.join() {
        eprintln!("Writer thread panicked: {:?}", err);
    }

    Ok(())
}
