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
    planned: &[u64],
    actual: &[u64],
    send: &[u64],
    recv: &[u64],
) -> (Vec<f64>, Vec<f64>, Vec<f64>) {
    let planned0 = planned[0] as i128;

    let mut x = Vec::with_capacity(planned.len());
    let mut actual_rel = Vec::with_capacity(planned.len());
    let mut recv_rel = Vec::with_capacity(planned.len());

    for i in 0..planned.len() {
        let planned_i = planned[i] as i128;
        let actual_i = actual[i] as i128;
        let send_i = send[i] as i128;
        let recv_i = recv[i] as i128;

        let planned_delta = planned_i - planned0;
        let actual_delta = actual_i - planned_i;
        let recv_delta = recv_i - send_i;

        x.push(planned_delta as f64 / 1000.0);
        actual_rel.push(actual_delta as f64 / 1000.0);
        recv_rel.push(recv_delta as f64 / 1000.0);
    }

    (x, actual_rel, recv_rel)
}

fn build_inter_packet_latency(recv: &[u64]) -> Vec<f64> {
    if recv.len() < 2 {
        return Vec::new();
    }

    let mut deltas = Vec::with_capacity(recv.len() - 1);
    for i in 1..recv.len() {
        let delta_ns = recv[i].saturating_sub(recv[i - 1]);
        deltas.push(delta_ns as f64 / 1000.0);
    }
    deltas
}

fn build_inter_packet_series(recv: &[u64]) -> (Vec<f64>, Vec<f64>) {
    let deltas = build_inter_packet_latency(recv);
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
        while let Ok((planned, actual, send, recv)) = rx.recv() {
            let count = planned.len();
            let (x, actual_rel, recv_rel) = build_series(&planned, &actual, &send, &recv);
            let (inter_x, inter_packet) = build_inter_packet_series(&recv);

            let mut plot = Plot::new();
            plot.add_trace(
                Scatter::new(x.clone(), actual_rel)
                    .mode(Mode::Lines)
                    .name("wakeup_latency"),
            );
            plot.add_trace(
                Scatter::new(x, recv_rel)
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

    let mut planned = Vec::with_capacity(sample_limit);
    let mut actual = Vec::with_capacity(sample_limit);
    let mut send = Vec::with_capacity(sample_limit);
    let mut recv = Vec::with_capacity(sample_limit);

    let mut buf = [0u8; 2048];

    mman::mlockall(mman::MmanFlags::MCL_CURRENT | mman::MmanFlags::MCL_FUTURE)?;
    let mut cpu = linux_rt::CpuSet::empty();
    cpu.set(1);
    linux_rt::sched::set_affinity(linux_rt::sched::Pid::this(), cpu)?;
    linux_rt::sched::set_fifo(linux_rt::sched::Pid::this(), 90)?;

    let mut count = 0;
    while planned.len() < sample_limit {
        count += 1;
        let (len, _addr) = socket.recv_from(&mut buf)?;
        let recv_ns = realtime_ns()?;
        if len < 24 {
            eprintln!("Received short packet ({} bytes)", len);
            continue;
        }
        if count < 5 {
            continue;
        }
        planned.push(parse_u64_be(&buf[0..8]));
        actual.push(parse_u64_be(&buf[8..16]));
        send.push(parse_u64_be(&buf[16..24]));
        recv.push(recv_ns);

        if planned.len() % WRITE_EVERY == 0 || planned.len() == sample_limit {
            if tx
                .send((planned.clone(), actual.clone(), send.clone(), recv.clone()))
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
