//! fm1pack: the FM-1 firmware's flash blobs, built from the Rust engine.
//!
//!   fm1pack tables OUT [--full]        the tables blob ("ORBT"); --full adds the filter and
//!                                      shaper tables the FM-1 computes
//!   fm1pack kit PREFIX [--codec C] [--regions N,..]
//!                                      the drum kit ("ORBK") as PREFIX_0.bin, PREFIX_1.bin, ..
//!                                      one per flash region; C is even (default), noisy, flac,
//!                                      adpcm or pcm (the host's bit-exact reference)
//!   fm1pack song IN.rbs OUT [--ref WAV] [--idle N]
//!                                      a song ("ORBS"), and the Rust engine's render of it
//!
//! The layouts are read by fm1/engine (orb_samples.cc, the song loader); all little endian.

use std::io::Write;
use std::path::Path;
use std::process::{Command, Stdio};
use std::sync::Arc;

use orb_engine::frame::export;
use orb_engine::frame::{Devices, Engine};
use orb_engine::samples::{COUNT, FIRST_ID, LAST_ID, NAMES_808, NAMES_909, STARTUP_ID};
use orb_engine::song::{Device, LoadForm, Song};
use orb_engine::tables::Tables;

const FLAC_BLOCK: usize = 256;
const ADPCM_BLOCK: usize = 32;

/// The noise-like samples (cymbals, hats, snare noise, the 808 snare noise layers): the only ones
/// the `noisy` codec rounds.
const NOISY: &[u16] = &[
    1006, 1007, 1008, 1009, // 808 N1..N4
    1014, 1022, // 808 CY, OH
    1034, 1035, 1036, // 909 sdn1..3
    1042, 1043, 1044, 1045, // 909 chh, ohh, ccy, rcy
];

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let r = match args.first().map(String::as_str) {
        Some("tables") if args.len() >= 2 => tables(&args[1], &args[2..]),
        Some("kit") if args.len() >= 2 => kit(&args[1], &args[2..]),
        Some("song") if args.len() >= 3 => song(&args[1], &args[2], &args[3..]),
        Some("study") => study(args.get(1).map(String::as_str) == Some("all")),
        _ => Err(USAGE.into()),
    };
    if let Err(e) = r {
        eprintln!("fm1pack: {e}");
        std::process::exit(1);
    }
}

const USAGE: &str = "usage: fm1pack tables OUT [--full] | kit PREFIX [--codec even|noisy|flac|adpcm|pcm] [--regions N,..] | \
                     song IN.rbs OUT [--ref WAV] [--idle N]";

type R = Result<(), String>;

fn write(path: &str, b: &[u8]) -> R {
    std::fs::write(path, b).map_err(|e| format!("{path}: {e}"))?;
    println!("{path}: {} B", b.len());
    Ok(())
}

fn pad4(b: &mut Vec<u8>) {
    while b.len() % 4 != 0 {
        b.push(0);
    }
}

// ---- tables

/// The FM-1's tables leave out the filter and shaper tables, which it computes; `--full` keeps
/// them (the host's bit-exact build).
fn tables(out: &str, opts: &[String]) -> R {
    let full = match opts {
        [] => false,
        [o] if o == "--full" => true,
        _ => return Err(USAGE.into()),
    };
    let t = Tables::new();
    let mut b = Vec::new();
    b.extend_from_slice(b"ORBT");
    b.extend_from_slice(&1u32.to_le_bytes());
    b.extend_from_slice(&(full as u32).to_le_bytes());
    let mut f = |x: f32| b.extend_from_slice(&x.to_le_bytes());
    t.level.iter().for_each(|&x| f(x));
    t.tb303_pitch.iter().for_each(|&x| f(x));
    if full {
        t.tb303_filter.iter().flatten().flatten().for_each(|&x| f(x));
        t.pcf_lowpass.iter().flatten().flatten().for_each(|&x| f(x));
        t.pcf_bandpass.iter().flatten().flatten().for_each(|&x| f(x));
        t.dist_shaper.iter().flatten().for_each(|&x| f(x));
    }
    for x in t.tr808_tom_step.iter().chain(t.tr909_tom_step.iter()) {
        b.extend_from_slice(&x.to_le_bytes());
    }
    for x in t.tr909_decay {
        b.extend_from_slice(&x.to_le_bytes());
    }
    for s in [&t.tb303_smoother, &t.pcf_smoother] {
        for x in [s.b2, s.a, s.g] {
            b.extend_from_slice(&x.to_le_bytes());
        }
    }
    write(out, &b)
}

// ---- kit

#[derive(Clone, Copy, PartialEq)]
enum Codec {
    Pcm = 0,
    Flac = 1,
    Adpcm = 2,
}

fn name(id: u16) -> &'static str {
    match id {
        STARTUP_ID => "startup",
        ..STARTUP_ID => NAMES_808[(id - FIRST_ID) as usize],
        _ => NAMES_909[(id - STARTUP_ID - 1) as usize],
    }
}

/// The FM-1's kit regions (fm1/README.md, the flash map): 400000 B of the app area after the
/// firmware, the data flash 0xB7000..0xE0000 and 0xE5000..0xFB000.
const FM1_REGIONS: &[usize] = &[400_000, 0x29000, 0x16000];

/// One sample encoded: its codec, its bytes and (FLAC) its frame index.
struct Enc {
    codec: Codec,
    data: Vec<u8>,
    index: Vec<u32>,
}

impl Enc {
    fn new(codec: Codec, pcm: &[i16]) -> Result<Enc, String> {
        let (data, index) = match codec {
            Codec::Pcm => (pcm.iter().flat_map(|x| x.to_le_bytes()).collect(), vec![]),
            Codec::Flac => flac_encode(pcm)?,
            Codec::Adpcm => (adpcm_encode(pcm), vec![]),
        };
        Ok(Enc { codec, data, index })
    }

    /// The bytes it takes in a region (data and index, each 4-aligned).
    fn size(&self) -> usize {
        self.data.len().next_multiple_of(4) + 4 * self.index.len()
    }
}

/// Places the samples into the regions, largest first, each into the first region with room
/// (region 0 after the header): the region of each sample, or None when they do not fit.
fn place(sizes: &[usize], caps: &[usize], head: usize) -> Option<Vec<usize>> {
    let mut free: Vec<usize> = caps.to_vec();
    free[0] = free[0].checked_sub(head)?;
    let mut order: Vec<usize> = (0..sizes.len()).collect();
    order.sort_by_key(|&i| std::cmp::Reverse(sizes[i]));
    let mut at = vec![0; sizes.len()];
    for i in order {
        let r = free.iter().position(|&f| f >= sizes[i])?;
        free[r] -= sizes[i];
        at[i] = r;
    }
    Some(at)
}

/// `kit PREFIX [--codec C] [--regions N,N,..]`: PREFIX_0.bin (the header and region 0's
/// samples) and PREFIX_r.bin for the other regions. `even` (the default) rounds samples to one
/// SNR target, the highest at which the kit fits, largest first, the rest lossless; `noisy` does
/// the same with only the noise-like samples.
fn kit(out: &str, opts: &[String]) -> R {
    let mut mode = "even".to_string();
    let mut caps: Vec<usize> = FM1_REGIONS.to_vec();
    let mut it = opts.iter();
    while let Some(o) = it.next() {
        let v = it.next().ok_or(USAGE)?;
        match o.as_str() {
            "--codec" => mode = v.clone(),
            "--regions" => {
                caps = v.split(',').map(|s| s.parse().map_err(|_| "bad --regions")).collect::<Result<_, _>>()?;
                if caps.is_empty() || caps.len() > 4 {
                    return Err("1 to 4 regions".into());
                }
            }
            _ => return Err(USAGE.into()),
        }
    }
    let samples = rbm::standard::builtin_samples();
    let mut pcms = Vec::new();
    let mut encs = Vec::new();
    for id in FIRST_ID..=LAST_ID {
        // the startup sound is no drum: an empty slot
        // whole, trailing zeros included: the frame count is the voices' length (their `left`,
        // the pitched voice's end, the snare's shorter sample), not only where the reads end
        let pcm: &[i16] = if id == STARTUP_ID { &[] } else { samples.get(id).audio() };
        let codec = match mode.as_str() {
            _ if pcm.is_empty() => Codec::Pcm,
            "pcm" => Codec::Pcm,
            "flac" => Codec::Flac,
            "adpcm" => Codec::Adpcm,
            "even" | "noisy" => Codec::Flac,
            _ => return Err(format!("unknown codec {mode}")),
        };
        encs.push(Enc::new(codec, pcm)?);
        pcms.push(pcm);
    }
    let head = 16 + 8 * caps.len() + 24 * COUNT;
    let sizes = |encs: &[Enc]| encs.iter().map(Enc::size).collect::<Vec<_>>();
    let rounded = mode == "even" || mode == "noisy";
    if !rounded && place(&sizes(&encs), &caps, head).is_none() {
        let total: usize = sizes(&encs).iter().sum();
        return Err(format!("the kit ({} B) does not fit the regions {caps:?}", total + head));
    }
    let mut snrs: Vec<Option<f64>> = vec![None; encs.len()];
    if rounded {
        // the candidates, largest first, go near-lossless at one target, the highest at which the
        // fewest of them make the kit fit
        let mut noisy: Vec<usize> = (0..encs.len())
            .filter(|&i| !pcms[i].is_empty() && (mode == "even" || NOISY.contains(&(FIRST_ID + i as u16))))
            .collect();
        noisy.sort_by_key(|&i| std::cmp::Reverse(encs[i].size()));
        let mut chosen = None;
        'search: for t in (24..=60).rev() {
            let mut trial: Vec<usize> = sizes(&encs);
            let mut lossy = Vec::new();
            for &i in &noisy {
                if place(&trial, &caps, head).is_some() {
                    chosen = Some((t, lossy));
                    break 'search;
                }
                let e = Enc::new(Codec::Flac, &near_lossless(pcms[i], t as f64))?;
                trial[i] = e.size().min(trial[i]);
                lossy.push((i, e));
            }
            if place(&trial, &caps, head).is_some() {
                chosen = Some((t, lossy));
                break;
            }
        }
        let (t, lossy) = chosen.ok_or("the kit does not fit even with its noisy samples at 24 dB")?;
        if !lossy.is_empty() {
            println!("  near-lossless at {t} dB: {} samples", lossy.len());
        }
        for (i, e) in lossy {
            if e.size() < encs[i].size() {
                let q = near_lossless(pcms[i], t as f64);
                snrs[i] = Some(snr(pcms[i], &q));
                encs[i] = e;
            }
        }
    }
    let at = place(&sizes(&encs), &caps, head).unwrap();
    let mut bodies: Vec<Vec<u8>> = vec![Vec::new(); caps.len()];
    bodies[0].resize(head, 0);
    let mut entries = Vec::new();
    for (i, e) in encs.iter().enumerate() {
        let id = FIRST_ID + i as u16;
        let b = &mut bodies[at[i]];
        let data_off = b.len();
        b.extend_from_slice(&e.data);
        pad4(b);
        let index_off = if e.index.is_empty() { 0 } else { b.len() };
        e.index.iter().for_each(|o| b.extend_from_slice(&o.to_le_bytes()));
        let snr = match (e.codec, snrs[i]) {
            (Codec::Adpcm, _) => format!("  SNR {:.1} dB", snr(pcms[i], &adpcm_decode(&e.data, pcms[i].len()))),
            (_, Some(s)) => format!("  SNR {s:.1} dB"),
            _ => String::new(),
        };
        println!(
            "  {id} {:<7} {:<5} {:>7} -> {:>6} B  region {}{snr}",
            name(id),
            ["pcm", "flac", "adpcm"][e.codec as usize],
            2 * pcms[i].len(),
            e.size(),
            at[i]
        );
        entries.push((pcms[i].len() as u32, e.codec as u8, at[i] as u8, data_off as u32, e.data.len() as u32, index_off as u32, e.index.len() as u32));
    }
    let mut h = Vec::with_capacity(head);
    h.extend_from_slice(b"ORBK");
    for x in [2, COUNT as u32, caps.len() as u32] {
        h.extend_from_slice(&x.to_le_bytes());
    }
    for (r, b) in bodies.iter().enumerate() {
        h.extend_from_slice(&(b.len() as u32).to_le_bytes());
        h.extend_from_slice(&(if r == 0 { 0 } else { crc32(b) }).to_le_bytes());
    }
    for (frames, codec, region, off, n, ioff, icount) in entries {
        h.extend_from_slice(&frames.to_le_bytes());
        h.extend_from_slice(&[codec, region, 0, 0]);
        for x in [off, n, ioff, icount] {
            h.extend_from_slice(&x.to_le_bytes());
        }
    }
    assert_eq!(h.len(), head);
    bodies[0][..head].copy_from_slice(&h);
    for (r, b) in bodies.iter().enumerate() {
        println!("  region {r}: {} of {} B", b.len(), caps[r]);
        write(&format!("{out}_{r}.bin"), b)?;
    }
    Ok(())
}

fn crc32(b: &[u8]) -> u32 {
    !b.iter().fold(!0u32, |mut c, &x| {
        c ^= x as u32;
        for _ in 0..8 {
            c = (c >> 1) ^ (0xEDB8_8320 & (c & 1).wrapping_neg());
        }
        c
    })
}

/// `pcm` rounded, per FLAC frame, to a power-of-two step whose noise is at least `snr` dB below
/// the frame's own level: the low bits FLAC then stores as wasted bits cost nothing, and the
/// noise follows the sound's envelope.
fn near_lossless(pcm: &[i16], snr: f64) -> Vec<i16> {
    let mut out = Vec::with_capacity(pcm.len());
    for f in pcm.chunks(FLAC_BLOCK) {
        let rms = (f.iter().map(|&x| (x as f64).powi(2)).sum::<f64>() / f.len() as f64).sqrt();
        // uniform rounding noise is step^2 / 12
        let step = rms * 12f64.sqrt() * 10f64.powf(-snr / 20.0);
        let k = if step >= 2.0 { (step.log2().floor() as u32).min(12) } else { 0 };
        let hi = (i16::MAX as i32 >> k) << k;
        for &x in f {
            let q = if k == 0 {
                x as i32
            } else {
                let h = 1 << (k - 1);
                (((x as i32 + h) >> k) << k).clamp(i16::MIN as i32, hi)
            };
            out.push(q as i16);
        }
    }
    out
}

/// `study`: the noisy samples' size and SNR as ADPCM and near-lossless FLAC.
fn study(all: bool) -> R {
    let samples = rbm::standard::builtin_samples();
    let ids: Vec<u16> = if all { (FIRST_ID..FIRST_ID + 46).filter(|&i| i != 1024).collect() } else { NOISY.to_vec() };
    for id in ids {
        let pcm = samples.get(id).audio();
        let a = adpcm_encode(pcm);
        let mut line = format!(
            "{id} {:<5} adpcm {:>6} B {:>5.1} dB",
            name(id),
            a.len(),
            snr(pcm, &adpcm_decode(&a, pcm.len()))
        );
        for t in [36.0, 42.0, 48.0, 54.0, 60.0] {
            let q = near_lossless(pcm, t);
            let (d, i) = flac_encode(&q)?;
            line += &format!(" | {t:.0}: {:>6} B {:>5.1} dB", d.len() + 4 * i.len(), snr(pcm, &q));
        }
        println!("{line}");
    }
    Ok(())
}

/// The FLAC frames of `pcm` (mono, 16 bit, FLAC_BLOCK samples a frame) without the stream's
/// metadata, and each frame's byte offset in them.
fn flac_encode(pcm: &[i16]) -> Result<(Vec<u8>, Vec<u32>), String> {
    let mut child = Command::new("flac")
        .args([
            "-s", "-c", "-8", "--no-md5-sum", "--force-raw-format", "--endian=little", "--sign=signed",
            "--channels=1", "--bps=16", "--sample-rate=44100", "--no-padding", "--no-seektable",
        ])
        .arg(format!("--blocksize={FLAC_BLOCK}"))
        .arg("-")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()
        .map_err(|e| format!("cannot run flac (brew install flac): {e}"))?;
    let raw: Vec<u8> = pcm.iter().flat_map(|x| x.to_le_bytes()).collect();
    let mut stdin = child.stdin.take().unwrap();
    let writer = std::thread::spawn(move || stdin.write_all(&raw));
    let o = child.wait_with_output().map_err(|e| e.to_string())?;
    writer.join().unwrap().map_err(|e| e.to_string())?;
    if !o.status.success() || !o.stdout.starts_with(b"fLaC") {
        return Err("flac failed".into());
    }
    // skip the metadata blocks
    let s = &o.stdout;
    let mut p = 4;
    loop {
        let last = s[p] & 0x80 != 0;
        p += 4 + u32::from_be_bytes([0, s[p + 1], s[p + 2], s[p + 3]]) as usize;
        if last {
            break;
        }
    }
    let frames = &s[p..];
    let count = pcm.len().div_ceil(FLAC_BLOCK);
    let mut index = vec![0u32];
    for k in 1..count {
        let start = *index.last().unwrap() as usize;
        let next = (start + 6..frames.len())
            .find(|&q| frame_header(&frames[q..]) == Some(k as u32) && crc16(&frames[start..q]) == 0)
            .ok_or(format!("flac: frame {k} not found"))?;
        index.push(next as u32);
    }
    let last = *index.last().unwrap() as usize;
    if frame_header(&frames[last..]) != Some(count as u32 - 1) || crc16(&frames[last..]) != 0 {
        return Err("flac: bad last frame".into());
    }
    Ok((frames.to_vec(), index))
}

/// The frame number of a valid fixed-block-size frame header at the start of `b`.
fn frame_header(b: &[u8]) -> Option<u32> {
    if b.len() < 6 || b[0] != 0xFF || b[1] != 0xF8 {
        return None;
    }
    let mut p = 4;
    let c = b[p] as u32;
    let extra = (c << 24).leading_ones().saturating_sub(1) as usize;
    let mut num = c & (0x7F >> extra.min(6));
    for i in 0..extra {
        num = num << 6 | (*b.get(p + 1 + i)? as u32 & 0x3F);
    }
    p += 1 + extra;
    p += match b[2] >> 4 {
        6 => 1,
        7 => 2,
        _ => 0,
    };
    p += match b[2] & 15 {
        12 => 1,
        13 | 14 => 2,
        _ => 0,
    };
    (crc8(b.get(..p)?) == *b.get(p)?).then_some(num)
}

fn crc8(b: &[u8]) -> u8 {
    b.iter().fold(0u8, |mut c, &x| {
        c ^= x;
        for _ in 0..8 {
            c = if c & 0x80 != 0 { c << 1 ^ 0x07 } else { c << 1 };
        }
        c
    })
}

fn crc16(b: &[u8]) -> u16 {
    b.iter().fold(0u16, |mut c, &x| {
        c ^= (x as u16) << 8;
        for _ in 0..8 {
            c = if c & 0x8000 != 0 { c << 1 ^ 0x8005 } else { c << 1 };
        }
        c
    })
}

/// The block ADPCM of orb_samples.cc: per 32 samples a byte (filter << 4 | shift) and 16 bytes
/// of 4-bit codes, low nibble first. Each block takes the filter and shift of least error.
const ADPCM_F: [[i32; 2]; 5] = [[0, 0], [60, 0], [115, -52], [98, -55], [122, -60]];

fn adpcm_step(code: i32, sh: u32, f: [i32; 2], h1: i32, h2: i32) -> i32 {
    (((code * 4096) >> sh) + ((h1 * f[0] + h2 * f[1] + 32) >> 6)).clamp(-32768, 32767)
}

/// The paths a block's search keeps per sample, and the codes it tries from each (the nearest
/// to the target and their neighbours).
const ADPCM_BEAM: usize = 6;
const ADPCM_TRY: i32 = 1;

#[derive(Clone, Copy)]
struct Trail {
    err: i64,
    h1: i32,
    h2: i32,
    codes: [u8; 16],
}

/// The codes of one block with filter f and shift sh: a beam search over the code sequences
/// (the decoder's history makes a locally worse code pay later), least squared error.
fn adpcm_block(x: &[i32], f: [i32; 2], sh: u32, h1: i32, h2: i32) -> Trail {
    let mut beam = vec![Trail { err: 0, h1, h2, codes: [0; 16] }];
    let mut next: Vec<Trail> = Vec::new();
    for (i, &xi) in x.iter().enumerate() {
        next.clear();
        for p in &beam {
            let pred = (p.h1 * f[0] + p.h2 * f[1] + 32) >> 6;
            // the code whose step lands nearest the target, then its neighbours
            let near = (((xi - pred) as i64 * (1 << sh) + 2048).div_euclid(4096)).clamp(-8, 7) as i32;
            for code in (near - ADPCM_TRY).max(-8)..=(near + ADPCM_TRY).min(7) {
                let s = adpcm_step(code, sh, f, p.h1, p.h2);
                let e = (s - xi) as i64;
                let mut q = *p;
                q.err += e * e;
                q.codes[i >> 1] |= ((code & 15) as u8) << ((i & 1) * 4);
                q.h2 = p.h1;
                q.h1 = s;
                next.push(q);
            }
        }
        next.sort_by_key(|q| q.err);
        // one path per decoder state: the rest can only do worse from here
        let mut seen = std::collections::HashSet::new();
        next.retain(|q| seen.insert((q.h1, q.h2)));
        next.truncate(ADPCM_BEAM);
        std::mem::swap(&mut beam, &mut next);
    }
    beam[0]
}

fn adpcm_encode(pcm: &[i16]) -> Vec<u8> {
    let (mut h1, mut h2) = (0i32, 0i32);
    let mut out = Vec::new();
    for blk in pcm.chunks(ADPCM_BLOCK) {
        let x: Vec<i32> = (0..ADPCM_BLOCK).map(|i| *blk.get(i).unwrap_or(&0) as i32).collect();
        let mut best: Option<(Trail, u8)> = None;
        for (fi, &f) in ADPCM_F.iter().enumerate() {
            for sh in 0..=12u32 {
                let p = adpcm_block(&x, f, sh, h1, h2);
                if best.as_ref().is_none_or(|(b, _)| p.err < b.err) {
                    best = Some((p, (fi as u8) << 4 | sh as u8));
                }
            }
        }
        let (p, head) = best.unwrap();
        out.push(head);
        out.extend_from_slice(&p.codes);
        (h1, h2) = (p.h1, p.h2);
    }
    out
}

fn adpcm_decode(data: &[u8], n: usize) -> Vec<i16> {
    let (mut h1, mut h2) = (0i32, 0i32);
    let mut out = Vec::with_capacity(n + ADPCM_BLOCK);
    for b in data.chunks(17) {
        let f = ADPCM_F[(b[0] >> 4).min(4) as usize];
        for i in 0..ADPCM_BLOCK {
            let code = ((b[1 + (i >> 1)] >> ((i & 1) * 4)) & 15) as i32;
            let s = adpcm_step((code ^ 8) - 8, (b[0] & 15) as u32, f, h1, h2);
            out.push(s as i16);
            (h2, h1) = (h1, s);
        }
    }
    out.truncate(n);
    out
}

fn snr(a: &[i16], b: &[i16]) -> f64 {
    let sig: f64 = a.iter().map(|&x| (x as f64).powi(2)).sum();
    let err: f64 = a.iter().zip(b).map(|(&x, &y)| (x as f64 - y as f64).powi(2)).sum();
    10.0 * (sig / err.max(1e-9)).log10()
}

// ---- song: "ORBS", u32 version 1,
//      u8 form (0 RBS, 1 1.x), u8 block count, u16 0,
//      the globals: u8 song_mode, play, byte2, loop_on, u32 tempo, locate, loop_start,
//      loop_end, u8 shuffle, vintage, byte22, 0,
//      char[32] title (NUL padded),
//      the msg-41 blocks in load order: u8 device, 0, u16 length, payload, padded to 4,
//      u32 node count of each of the 9 tracks,
//      the nodes, track by track in list order: i32 tick, u8 ctrl, u8 value.
// The nodes are the track lists as the Rust engine's load leaves them (the msg-37 batches
// inserted into the lists the devices' init_track made), which the FM-1 plays from flash
// instead of inserting the batches into a node pool it has no RAM for.

fn song(input: &str, out: &str, opts: &[String]) -> R {
    let mut reference = None;
    let mut idle = 9088; // orb render's default: the reference corpus' capture protocol
    let mut it = opts.iter();
    while let Some(o) = it.next() {
        let v = it.next().ok_or(USAGE)?;
        match o.as_str() {
            "--ref" => reference = Some(v.clone()),
            "--idle" => idle = v.parse().map_err(|_| "bad --idle")?,
            _ => return Err(USAGE.into()),
        }
    }
    let data = std::fs::read(input).map_err(|e| format!("{input}: {e}"))?;
    let song = if rbs::rb1x::is_1x(&data) {
        let p = rbs::rb1x::Song1x::parse(&data).map_err(|e| format!("{input}: {e}"))?;
        orb_engine::load::convert_1x(&p).map_err(|e| format!("{input}: {e}"))?
    } else {
        let p = rbs::Song::parse(&data).map_err(|e| format!("{input}: {e}"))?;
        orb_engine::load::convert(&p).map_err(|e| format!("{input}: {e:?}"))?
    };
    if song.mod_name != "Standard ReBirth" {
        eprintln!("fm1pack: warning: the song names the mod \"{}\"; the FM-1 plays the standard kit", song.mod_name);
    }
    // with the drum machines: their init_track makes their tracks' tick-0 events and quantisation
    let mut loaded = Engine::new(
        Arc::new(Tables::new()),
        Devices::implemented(Some(rbm::standard::builtin_samples())),
    );
    loaded.load(&song).map_err(|r| format!("{input}: the original refuses this song: {r}"))?;
    let blob = song_blob(&song, &loaded, Path::new(input));
    write(out, &blob)?;
    if let Some(wav) = reference {
        let mut engine = Engine::new(
            Arc::new(Tables::new()),
            Devices::implemented(Some(rbm::standard::builtin_samples())),
        );
        let pcm = export::export_song(&mut engine, &song, idle).map_err(|r| format!("load refused: {r}"))?;
        write(&wav, &export::wav_bytes(&pcm))?;
    }
    Ok(())
}

fn song_blob(song: &Song, loaded: &Engine, path: &Path) -> Vec<u8> {
    let blocks = song.form.device_blocks();
    let g = &song.globals;
    let mut b = Vec::new();
    b.extend_from_slice(b"ORBS");
    b.extend_from_slice(&1u32.to_le_bytes());
    b.push((song.form == LoadForm::Rb1x) as u8);
    b.push(blocks.len() as u8);
    b.extend_from_slice(&0u16.to_le_bytes());
    b.extend_from_slice(&[g.song_mode, g.play, g.byte2, g.loop_on]);
    for x in [g.tempo, g.locate, g.loop_start, g.loop_end] {
        b.extend_from_slice(&x.to_le_bytes());
    }
    b.extend_from_slice(&[g.shuffle, g.vintage, g.byte22, 0]);
    let mut title = [0u8; 32];
    let stem = path.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
    for (d, c) in title[..31].iter_mut().zip(stem.bytes()) {
        *d = c;
    }
    b.extend_from_slice(&title);
    for &d in blocks {
        let p = song.device_payload(d);
        assert_eq!(p.len(), Device::data_len(d));
        b.extend_from_slice(&[d.id(), 0]);
        b.extend_from_slice(&(p.len() as u16).to_le_bytes());
        b.extend_from_slice(&p);
        pad4(&mut b);
    }
    let seq = &loaded.frame.seq;
    for t in 0..9 {
        b.extend_from_slice(&(seq.events(t).len() as u32).to_le_bytes());
    }
    for t in 0..9 {
        for e in seq.events(t) {
            b.extend_from_slice(&e.tick.to_le_bytes());
            b.extend_from_slice(&[e.ctrl, e.value]);
        }
    }
    b
}
