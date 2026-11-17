//! Streams a --ic-trace file and writes the block-instance aggregate as JSON.
//! Same semantics as `ictrace.py aggregate`, which loads the output.
//!
//!   ictrace-agg TRACE OUT.json

use std::collections::HashMap;
use std::fs::File;
use std::io::{BufReader, BufWriter, Read, Write};

const JOF_JUMP: u8 = 1;
const JOF_TARGET: u8 = 2;
const JOF_NOFALL: u8 = 4;
const MISS: i64 = -1;

struct Reader<R: Read> {
    r: R,
}

impl<R: Read> Reader<R> {
    fn bytes(&mut self, n: usize) -> Option<Vec<u8>> {
        let mut b = vec![0; n];
        self.r.read_exact(&mut b).ok()?;
        Some(b)
    }
    fn u8(&mut self) -> Option<u8> {
        let mut b = [0; 1];
        self.r.read_exact(&mut b).ok()?;
        Some(b[0])
    }
    fn u16(&mut self) -> Option<u16> {
        let mut b = [0; 2];
        self.r.read_exact(&mut b).ok()?;
        Some(u16::from_le_bytes(b))
    }
    fn u32(&mut self) -> Option<u32> {
        let mut b = [0; 4];
        self.r.read_exact(&mut b).ok()?;
        Some(u32::from_le_bytes(b))
    }
    fn u64(&mut self) -> Option<u64> {
        let mut b = [0; 8];
        self.r.read_exact(&mut b).ok()?;
        Some(u64::from_le_bytes(b))
    }
    fn string(&mut self) -> Option<String> {
        let n = self.u32()? as usize;
        Some(String::from_utf8_lossy(&self.bytes(n)?).into_owned())
    }
}

struct Script {
    filename: String,
    line: u32,
    column: u32,
    ops: Vec<(u32, u8, u16, u16)>,
    code: Vec<u8>,
    leader_of: HashMap<u32, u32>,
}

struct Current {
    script: u64,
    leader: u32,
    last_pc: u32,
    outcomes: Vec<(u32, i64)>,
}

#[derive(Default)]
struct State {
    scripts: HashMap<u64, Script>,
    recipes: HashMap<(u8, Vec<u8>), i64>,
    recipe_info: Vec<(u8, Vec<u8>)>,
    stub_site: HashMap<u64, (u64, u32, i64)>,
    fallback_script: HashMap<u64, u64>,
    pending: HashMap<u64, (u64, bool)>,
    current: HashMap<u64, Current>,
    instances: HashMap<(u64, u32, Vec<(u32, i64)>), u64>,
    hits: u64,
    misses: u64,
    unknown: u64,
}

impl State {
    fn close(&mut self, frame: u64) {
        if let Some(cur) = self.current.remove(&frame) {
            *self
                .instances
                .entry((cur.script, cur.leader, cur.outcomes))
                .or_insert(0) += 1;
        }
    }

    fn record(&mut self, frame: u64, script_id: u64, pc: u32, outcome: i64) {
        let leader = match self.scripts.get(&script_id) {
            Some(s) => match s.leader_of.get(&pc) {
                Some(l) => *l,
                None => return,
            },
            None => return,
        };
        if let Some(cur) = self.current.get_mut(&frame) {
            if cur.script == script_id && cur.leader == leader && pc > cur.last_pc {
                cur.last_pc = pc;
                cur.outcomes.push((pc, outcome));
                return;
            }
        }
        self.close(frame);
        self.current.insert(
            frame,
            Current {
                script: script_id,
                leader,
                last_pc: pc,
                outcomes: vec![(pc, outcome)],
            },
        );
    }

    fn flush_hit(&mut self, frame: u64) {
        let Some((stub, failed)) = self.pending.remove(&frame) else {
            return;
        };
        if failed {
            return;
        }
        let Some(&(fallback, pc, rid)) = self.stub_site.get(&stub) else {
            self.unknown += 1;
            return;
        };
        let Some(&script_id) = self.fallback_script.get(&fallback) else {
            self.unknown += 1;
            return;
        };
        self.hits += 1;
        self.record(frame, script_id, pc, rid);
    }
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{:02x}", x)).collect()
}

fn json_str(s: &str) -> String {
    let mut out = String::from("\"");
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        eprintln!("usage: ictrace-agg TRACE OUT.json");
        std::process::exit(2);
    }
    let file = File::open(&args[1]).expect("open trace");
    let mut r = Reader {
        r: BufReader::with_capacity(1 << 22, file),
    };

    assert_eq!(r.bytes(8).expect("magic"), b"ICTRACE1");
    let mut jsops = Vec::new();
    for _ in 0..r.u16().unwrap() {
        let len = r.u8().unwrap();
        let flags = r.u8().unwrap();
        jsops.push((len, flags, r.string().unwrap()));
    }
    let mut cacheops = Vec::new();
    for _ in 0..r.u16().unwrap() {
        let arglen = r.u8().unwrap();
        let transpile = r.u8().unwrap();
        cacheops.push((arglen, transpile, r.string().unwrap()));
    }
    let mut kinds = Vec::new();
    for _ in 0..r.u16().unwrap() {
        kinds.push(r.string().unwrap());
    }

    let mut st = State::default();
    loop {
        let Some(tag) = r.u8() else { break };
        let ok = (|| -> Option<()> {
            match tag {
                b'E' => {
                    let stub = r.u64()?;
                    let frame = r.u64()?;
                    st.flush_hit(frame);
                    st.pending.insert(frame, (stub, false));
                }
                b'X' => {
                    let stub = r.u64()?;
                    let frame = r.u64()?;
                    if let Some(p) = st.pending.get_mut(&frame) {
                        if p.0 == stub {
                            p.1 = true;
                        }
                    }
                }
                b'F' => {
                    let frame = r.u64()?;
                    let _icscript = r.u64()?;
                    let fallback = r.u64()?;
                    let script_id = r.u64()?;
                    let pc = r.u32()?;
                    st.flush_hit(frame);
                    st.fallback_script.insert(fallback, script_id);
                    st.misses += 1;
                    st.record(frame, script_id, pc, MISS);
                }
                b'A' => {
                    let stub = r.u64()?;
                    let _icscript = r.u64()?;
                    let fallback = r.u64()?;
                    let pc = r.u32()?;
                    let kind = r.u8()?;
                    let n = r.u32()? as usize;
                    let code = r.bytes(n)?;
                    let next = st.recipe_info.len() as i64;
                    let rid = *st.recipes.entry((kind, code.clone())).or_insert(next);
                    if rid == next {
                        st.recipe_info.push((kind, code));
                    }
                    st.stub_site.insert(stub, (fallback, pc, rid));
                }
                b'C' => {
                    let new = r.u64()?;
                    let old = r.u64()?;
                    if let Some(&site) = st.stub_site.get(&old) {
                        st.stub_site.insert(new, site);
                    }
                }
                b'B' => {
                    r.bytes(3)?;
                }
                b'T' => {
                    r.u16()?;
                }
                b'M' => {
                    r.u16()?;
                    r.string()?;
                    r.bytes(5)?;
                }
                b'S' => {
                    let id = r.u64()?;
                    let filename = r.string()?;
                    let line = r.u32()?;
                    let column = r.u32()?;
                    let n = r.u32()?;
                    let mut ops = Vec::with_capacity(n as usize);
                    for _ in 0..n {
                        ops.push((r.u32()?, r.u8()?, r.u16()?, r.u16()?));
                    }
                    let len = r.u32()? as usize;
                    let code = r.bytes(len)?;
                    let mut leader_of = HashMap::new();
                    let mut leader = 0;
                    let mut prev_ends = true;
                    for &(offset, op, _, _) in &ops {
                        let flags = jsops[op as usize].1;
                        if prev_ends || flags & JOF_TARGET != 0 {
                            leader = offset;
                        }
                        leader_of.insert(offset, leader);
                        prev_ends = flags & (JOF_JUMP | JOF_NOFALL) != 0;
                    }
                    st.scripts.insert(
                        id,
                        Script {
                            filename,
                            line,
                            column,
                            ops,
                            code,
                            leader_of,
                        },
                    );
                }
                _ => panic!("bad tag {}", tag),
            }
            Some(())
        })();
        if ok.is_none() {
            break;
        }
    }
    let frames: Vec<u64> = st.pending.keys().copied().collect();
    for f in frames {
        st.flush_hit(f);
    }
    let frames: Vec<u64> = st.current.keys().copied().collect();
    for f in frames {
        st.close(f);
    }

    let out = File::create(&args[2]).expect("create output");
    let mut w = BufWriter::new(out);
    write!(w, "{{\"jsops\":[").unwrap();
    for (i, (len, flags, name)) in jsops.iter().enumerate() {
        let sep = if i > 0 { "," } else { "" };
        write!(w, "{}[{},{},{}]", sep, len, flags, json_str(name)).unwrap();
    }
    write!(w, "],\"cacheops\":[").unwrap();
    for (i, (arglen, transpile, name)) in cacheops.iter().enumerate() {
        let sep = if i > 0 { "," } else { "" };
        write!(w, "{}[{},{},{}]", sep, arglen, transpile, json_str(name)).unwrap();
    }
    write!(w, "],\"kinds\":[").unwrap();
    for (i, k) in kinds.iter().enumerate() {
        let sep = if i > 0 { "," } else { "" };
        write!(w, "{}{}", sep, json_str(k)).unwrap();
    }
    write!(w, "],\"scripts\":{{").unwrap();
    for (i, (id, s)) in st.scripts.iter().enumerate() {
        let sep = if i > 0 { "," } else { "" };
        write!(
            w,
            "{}\"{}\":[{},{},{},[",
            sep,
            id,
            json_str(&s.filename),
            s.line,
            s.column
        )
        .unwrap();
        for (j, (o, op, u, d)) in s.ops.iter().enumerate() {
            let sep = if j > 0 { "," } else { "" };
            write!(w, "{}[{},{},{},{}]", sep, o, op, u, d).unwrap();
        }
        write!(w, "],\"{}\"]", hex(&s.code)).unwrap();
    }
    write!(w, "}},\"recipe_info\":[").unwrap();
    for (i, (kind, code)) in st.recipe_info.iter().enumerate() {
        let sep = if i > 0 { "," } else { "" };
        write!(w, "{}[{},\"{}\"]", sep, kind, hex(code)).unwrap();
    }
    write!(w, "],\"instances\":[").unwrap();
    for (i, ((sid, leader, outcomes), n)) in st.instances.iter().enumerate() {
        let sep = if i > 0 { "," } else { "" };
        write!(w, "{}[{},{},[", sep, sid, leader).unwrap();
        for (j, (pc, rid)) in outcomes.iter().enumerate() {
            let sep = if j > 0 { "," } else { "" };
            write!(w, "{}[{},{}]", sep, pc, rid).unwrap();
        }
        write!(w, "],{}]", n).unwrap();
    }
    writeln!(
        w,
        "],\"hits\":{},\"misses\":{},\"unknown_stubs\":{}}}",
        st.hits, st.misses, st.unknown
    )
    .unwrap();
}
