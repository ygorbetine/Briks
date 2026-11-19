//! Anotações sobre as transmissões e Board da sala: lógica pura (sem rede), igual para a malha P2P (`mesh.rs`) e as data channels
//! do SFU (`sfudata.rs`), no mesmo molde do painel de sons (`sonsnet.rs`). Veja `docs/plans/anotacoes-board.md`.
//!
//! Tudo o que chega de outro participante é dado hostil: cada operação é revalidada campo a campo (faixas, tamanhos, tipos), o autor é a
//! identidade provada do canal (nunca um campo do JSON), há teto de memória por escopo e limite de taxa por autor. O que não passa é descartado.
use std::collections::{HashMap, HashSet};

use serde_json::{json, Map, Value};

pub const COORD_MAX: i64 = 10_000;
pub const MAX_OPS: usize = 4000;
pub const MAX_PTS_STROKE: usize = 2000;
pub const MAX_PTS_SCOPE: usize = 200_000;
pub const MAX_SCOPES: usize = 24;
pub const MAX_BATCH: usize = 200;
pub const MAX_TEXT: usize = 120;
pub const MAX_PERM_FPS: usize = 32;
const MAX_DEAD: usize = 20_000;
const MAX_W: i64 = 2000;
const RATE_WIN_MS: u64 = 1000;
const RATE_MSGS: u32 = 60;
const RATE_OPS: u32 = 600;
const RATE_PTS: u32 = 8000;
const RATE_LASER: u32 = 30;
const WANT_GAP_MS: u64 = 5_000;
const SNAP_WINDOW_MS: u64 = 30_000;
const SNAP_MAX_CHUNKS: u32 = 64;
const CHUNK_OPS: usize = 150;
const CHUNK_NUMS: usize = 8000;
const REPLY_MAX_CHUNKS: usize = 40;

#[derive(Debug, Clone, PartialEq)]
pub enum NetOut {
    /// `to` vazio = todos os pares autenticados
    Send { to: Vec<String>, kind: &'static str, body: Value },
    Ev(Value),
}

pub struct PeerRef<'a> { pub key: &'a str, pub fp: &'a str }

#[derive(Debug, Clone, PartialEq)]
struct Op { a: String, i: u32, c: u64, k: char, body: Value, pts: usize }

#[derive(Default)]
struct Scope {
    ops: Vec<Op>,
    index: HashMap<(String, u32), usize>,
    dead: HashSet<(String, u32)>,
    /// piso do "limpar tudo" do dono: (c, autor, i) menores já não existem
    floor_all: Option<(u64, String, u32)>,
    /// piso do "limpar os meus" por autor
    floor_own: HashMap<String, u64>,
    pts: usize,
}

#[derive(Default, Clone, PartialEq, Debug)]
struct Perm { all: bool, fps: HashSet<String> }

#[derive(Default)]
struct Rate { start: u64, msgs: u32, ops: u32, pts: u32, laser: u32 }

pub struct Net {
    me: String,
    scopes: HashMap<String, Scope>,
    perms: HashMap<String, Perm>,
    clock: u64,
    rate: HashMap<String, Rate>,
    want_seen: HashMap<String, u64>,
    asked_at: Option<u64>,
    snap_chunks: u32,
}

pub fn valid_fp(s: &str) -> bool { (8..=128).contains(&s.len()) && s.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_') }

/// `b:0..15` (Board) ou `s:<fp do dono>:<0|1>` (transmissão). Devolve o dono para escopos de transmissão.
fn parse_scope(s: &str) -> Option<Option<&str>> {
    if let Some(n) = s.strip_prefix("b:") {
        let ok = !n.is_empty() && n.len() <= 2 && n.bytes().all(|b| b.is_ascii_digit()) && !(n.len() == 2 && n.starts_with('0'));
        return if ok { n.parse::<u8>().ok().filter(|n| *n < 16).map(|_| None) } else { None };
    }
    let rest = s.strip_prefix("s:")?;
    let (fp, slot) = rest.rsplit_once(':')?;
    if !valid_fp(fp) || !(slot == "0" || slot == "1") { return None; }
    Some(Some(fp))
}

fn int(v: &Value, k: &str, lo: i64, hi: i64) -> Option<i64> { v.get(k)?.as_i64().filter(|n| (lo..=hi).contains(n)) }

fn pts_of(v: &Value, min_pts: usize, max_pts: usize) -> Option<Vec<i64>> {
    let a = v.get("p")?.as_array()?;
    if a.len() % 2 != 0 || a.len() < min_pts * 2 || a.len() > max_pts * 2 { return None; }
    a.iter().map(|n| n.as_i64().filter(|n| (0..=COORD_MAX).contains(n))).collect()
}

fn text_ok(s: &str) -> bool { !s.is_empty() && s.chars().count() <= MAX_TEXT && !s.chars().any(|c| c.is_control()) }

/// Valida e normaliza uma operação (só os campos conhecidos sobrevivem). `a` = autor já provado.
fn validate(v: &Value, a: &str) -> Option<Op> {
    let obj = v.as_object()?;
    let i = int(v, "i", 0, u32::MAX as i64)? as u32;
    let c = int(v, "c", 0, 1 << 48)? as u64;
    let k = obj.get("k")?.as_str()?;
    let mut b = Map::new();
    b.insert("i".into(), json!(i)); b.insert("c".into(), json!(c)); b.insert("k".into(), json!(k)); b.insert("a".into(), json!(a));
    let mut pts = 0usize;
    let kc = match k {
        "s" => {
            let p = pts_of(v, 1, MAX_PTS_STROKE)?;
            b.insert("m".into(), json!(int(v, "m", 0, 1)?)); b.insert("col".into(), json!(int(v, "col", 0, 0xFFFFFF)?)); b.insert("w".into(), json!(int(v, "w", 1, MAX_W)?));
            pts = p.len() / 2; b.insert("p".into(), json!(p)); 's'
        }
        "p" => {
            let p = pts_of(v, 1, 200)?;
            b.insert("tg".into(), json!(int(v, "tg", 0, u32::MAX as i64)?)); pts = p.len() / 2; b.insert("p".into(), json!(p)); 'p'
        }
        "g" => {
            let sh = v.get("sh")?.as_str().filter(|s| matches!(*s, "l" | "a" | "r" | "e"))?;
            b.insert("sh".into(), json!(sh)); b.insert("col".into(), json!(int(v, "col", 0, 0xFFFFFF)?)); b.insert("w".into(), json!(int(v, "w", 1, MAX_W)?)); b.insert("f".into(), json!(int(v, "f", 0, 1)?));
            for key in ["x0", "y0", "x1", "y1"] { b.insert(key.into(), json!(int(v, key, 0, COORD_MAX)?)); }
            pts = 2; 'g'
        }
        "t" => {
            let s = v.get("s")?.as_str().filter(|s| text_ok(s))?;
            b.insert("s".into(), json!(s)); b.insert("col".into(), json!(int(v, "col", 0, 0xFFFFFF)?)); b.insert("sz".into(), json!(int(v, "sz", 1, MAX_W)?));
            b.insert("x".into(), json!(int(v, "x", 0, COORD_MAX)?)); b.insert("y".into(), json!(int(v, "y", 0, COORD_MAX)?)); pts = 2; 't'
        }
        "d" => {
            let tg = v.get("tg")?.as_object()?;
            let ta = tg.get("a")?.as_str().filter(|s| valid_fp(s))?;
            let ti = tg.get("i")?.as_i64().filter(|n| (0..=u32::MAX as i64).contains(n))?;
            b.insert("tg".into(), json!({ "a": ta, "i": ti })); 'd'
        }
        "x" => { b.insert("all".into(), json!(v.get("all").and_then(Value::as_bool)?)); 'x' }
        _ => return None,
    };
    Some(Op { a: a.into(), i, c, k: kc, body: Value::Object(b), pts })
}

#[derive(Debug, PartialEq)]
enum Applied { New, Dup, Denied, Full, Ignored }

impl Net {
    pub fn new(me_fp: &str) -> Net {
        Net { me: me_fp.into(), scopes: HashMap::new(), perms: HashMap::new(), clock: 0, rate: HashMap::new(), want_seen: HashMap::new(), asked_at: None, snap_chunks: 0 }
    }

    fn may_draw(&self, scope: &str, a: &str) -> bool {
        match parse_scope(scope) {
            Some(None) => true,
            Some(Some(owner)) => a == owner || self.perms.get(owner).map(|p| p.all || p.fps.contains(a)).unwrap_or(false),
            None => false,
        }
    }
    /// quem pode apagar/limpar o que é dos outros: no Board todos; numa transmissão, só o dono dela
    fn privileged(&self, scope: &str, a: &str) -> bool {
        match parse_scope(scope) { Some(None) => true, Some(Some(owner)) => a == owner, None => false }
    }

    fn rate_ok(&mut self, fp: &str, now: u64, ops: u32, pts: u32, laser: bool) -> bool {
        if self.rate.len() > 256 { self.rate.retain(|_, r| now.saturating_sub(r.start) < RATE_WIN_MS * 5); }
        let r = self.rate.entry(fp.into()).or_default();
        if now.saturating_sub(r.start) >= RATE_WIN_MS { *r = Rate { start: now, ..Default::default() }; }
        if laser { r.laser += 1; return r.laser <= RATE_LASER; }
        r.msgs += 1; r.ops += ops; r.pts += pts;
        r.msgs <= RATE_MSGS && r.ops <= RATE_OPS && r.pts <= RATE_PTS
    }

    fn apply(&mut self, scope: &str, op: Op) -> Applied {
        if parse_scope(scope).is_none() { return Applied::Ignored; }
        if !self.may_draw(scope, &op.a) { return Applied::Denied; }
        self.clock = self.clock.max(op.c);
        let priv_ = self.privileged(scope, &op.a);
        if !self.scopes.contains_key(scope) && self.scopes.len() >= MAX_SCOPES { return Applied::Full; }
        let sc = self.scopes.entry(scope.to_string()).or_default();
        let key = (op.a.clone(), op.i);
        match op.k {
            's' | 'g' | 't' => {
                if sc.index.contains_key(&key) || sc.dead.contains(&key) { return Applied::Dup; }
                let t = (op.c, op.a.clone(), op.i);
                if let Some(f) = &sc.floor_all { if t < *f { return Applied::Ignored; } }
                if sc.floor_own.get(&op.a).map(|f| op.c < *f).unwrap_or(false) { return Applied::Ignored; }
                if sc.ops.len() >= MAX_OPS || sc.pts + op.pts > MAX_PTS_SCOPE { return Applied::Full; }
                sc.pts += op.pts; sc.index.insert(key, sc.ops.len()); sc.ops.push(op); Applied::New
            }
            'p' => {
                let tg = op.body["tg"].as_u64().unwrap_or(u64::MAX) as u32;
                let Some(&ix) = sc.index.get(&(op.a.clone(), tg)) else { return Applied::Ignored };
                let target = &mut sc.ops[ix];
                if target.k != 's' || target.pts + op.pts > MAX_PTS_STROKE || sc.pts + op.pts > MAX_PTS_SCOPE { return Applied::Full; }
                if let (Some(dst), Some(src)) = (target.body["p"].as_array_mut(), op.body["p"].as_array()) { dst.extend(src.iter().cloned()); }
                target.pts += op.pts; sc.pts += op.pts; Applied::New
            }
            'd' => {
                let (ta, ti) = (op.body["tg"]["a"].as_str().unwrap_or("").to_string(), op.body["tg"]["i"].as_u64().unwrap_or(0) as u32);
                if ta != op.a && !priv_ { return Applied::Denied; }
                let tk = (ta, ti);
                if sc.dead.contains(&tk) { return Applied::Dup; }
                if sc.dead.len() >= MAX_DEAD { sc.dead.clear(); }
                sc.dead.insert(tk.clone());
                if let Some(ix) = sc.index.remove(&tk) { Self::remove_at(sc, ix); }
                Applied::New
            }
            'x' => {
                let all = op.body["all"].as_bool().unwrap_or(false);
                if all && !priv_ { return Applied::Denied; }
                if all {
                    let t = (op.c, op.a.clone(), op.i);
                    if sc.floor_all.as_ref().map(|f| t <= *f).unwrap_or(false) { return Applied::Dup; }
                    sc.floor_all = Some(t.clone());
                    let keep: Vec<Op> = sc.ops.drain(..).filter(|o| (o.c, o.a.clone(), o.i) >= t).collect();
                    sc.ops = keep;
                } else {
                    if sc.floor_own.get(&op.a).map(|f| op.c <= *f).unwrap_or(false) { return Applied::Dup; }
                    sc.floor_own.insert(op.a.clone(), op.c);
                    let (me, c) = (op.a.clone(), op.c);
                    sc.ops.retain(|o| !(o.a == me && o.c < c));
                }
                sc.index.clear(); sc.pts = 0;
                for (n, o) in sc.ops.iter().enumerate() { sc.index.insert((o.a.clone(), o.i), n); sc.pts += o.pts; }
                Applied::New
            }
            _ => Applied::Ignored,
        }
    }

    fn remove_at(sc: &mut Scope, ix: usize) {
        let o = sc.ops.remove(ix);
        sc.pts = sc.pts.saturating_sub(o.pts);
        for n in sc.index.values_mut() { if *n > ix { *n -= 1; } }
    }

    fn drop_scope(&mut self, scope: &str) { self.scopes.remove(scope); }

    /// Mensagem de um par autenticado. Devolve eventos para a UI e (no `want`) a resposta.
    pub fn on_msg(&mut self, p: &PeerRef, kind: &str, v: &Value, now: u64) -> Vec<NetOut> {
        if !valid_fp(p.fp) { return vec![]; }
        let Some(scope) = v.get("scope").and_then(Value::as_str).filter(|s| s.len() <= 160 && parse_scope(s).is_some()).map(String::from) else {
            return if kind == "annot.perm" { self.on_perm(p, v) } else if kind == "annot.want" { self.on_want(p, now) } else { vec![] };
        };
        match kind {
            "annot.ops" | "annot.snap" => {
                let snap = kind == "annot.snap";
                if snap {
                    if self.asked_at.map(|t| now.saturating_sub(t) > SNAP_WINDOW_MS).unwrap_or(true) || self.snap_chunks >= SNAP_MAX_CHUNKS { return vec![]; }
                    self.snap_chunks += 1;
                }
                let Some(arr) = v.get("ops").and_then(Value::as_array).filter(|a| a.len() <= MAX_BATCH) else { return vec![] };
                let npts: usize = arr.iter().filter_map(|o| o.get("p").and_then(Value::as_array)).map(|a| a.len() / 2).sum();
                if !snap && !self.rate_ok(p.fp, now, arr.len() as u32, npts as u32, false) { return vec![]; }
                let mut ok = vec![]; let mut full = false;
                for o in arr {
                    let author = if snap { o.get("a").and_then(Value::as_str).filter(|s| valid_fp(s)).unwrap_or("") } else { p.fp };
                    if author.is_empty() { continue; }
                    let Some(op) = validate(o, author) else { continue };
                    let body = op.body.clone();
                    match self.apply(&scope, op) { Applied::New => ok.push(body), Applied::Full => full = true, _ => {} }
                }
                let mut out = vec![];
                if !ok.is_empty() { out.push(NetOut::Ev(json!({ "ev": "annot.ops", "scope": scope, "ops": ok, "snap": snap, "from_fp": p.fp }))); }
                if full { out.push(NetOut::Ev(json!({ "ev": "annot.full", "scope": scope }))); }
                out
            }
            "annot.laser" => {
                if !self.rate_ok(p.fp, now, 0, 0, true) || !self.may_draw(&scope, p.fp) { return vec![]; }
                let (Some(x), Some(y), Some(col)) = (int(v, "x", 0, COORD_MAX), int(v, "y", 0, COORD_MAX), int(v, "col", 0, 0xFFFFFF)) else { return vec![] };
                vec![NetOut::Ev(json!({ "ev": "annot.laser", "scope": scope, "a": p.fp, "x": x, "y": y, "col": col }))]
            }
            "annot.perm" => self.on_perm(p, v),
            "annot.want" => self.on_want(p, now),
            _ => vec![],
        }
    }

    fn on_perm(&mut self, p: &PeerRef, v: &Value) -> Vec<NetOut> {
        // só o próprio dono define quem desenha nas transmissões dele (o dono é o remetente autenticado)
        let all = v.get("all").and_then(Value::as_bool).unwrap_or(false);
        let fps: HashSet<String> = v.get("fps").and_then(Value::as_array).map(|a| a.iter().filter_map(Value::as_str).filter(|s| valid_fp(s)).take(MAX_PERM_FPS).map(String::from).collect()).unwrap_or_default();
        if self.perms.len() > 64 && !self.perms.contains_key(p.fp) { return vec![]; }
        let perm = Perm { all, fps };
        if self.perms.get(p.fp) == Some(&perm) { return vec![]; }
        let mut list: Vec<&String> = perm.fps.iter().collect(); list.sort();
        let ev = json!({ "ev": "annot.perm", "owner": p.fp, "all": all, "fps": list });
        self.perms.insert(p.fp.into(), perm);
        vec![NetOut::Ev(ev)]
    }

    fn on_want(&mut self, p: &PeerRef, now: u64) -> Vec<NetOut> {
        if self.want_seen.len() > 256 { self.want_seen.clear(); }
        if let Some(&t) = self.want_seen.get(p.key) { if now.saturating_sub(t) < WANT_GAP_MS { return vec![]; } }
        self.want_seen.insert(p.key.into(), now);
        let mut out = vec![];
        if let Some(pm) = self.perms.get(&self.me) {
            let mut fps: Vec<&String> = pm.fps.iter().collect(); fps.sort();
            out.push(NetOut::Send { to: vec![p.key.into()], kind: "annot.perm", body: json!({ "all": pm.all, "fps": fps }) });
        }
        let mut names: Vec<&String> = self.scopes.keys().collect(); names.sort();
        let mut chunks = 0;
        'outer: for name in names {
            let sc = &self.scopes[name];
            let (mut cur, mut nums): (Vec<Value>, usize) = (vec![], 0);
            for o in &sc.ops {
                let n = o.pts * 2 + 8;
                if !cur.is_empty() && (cur.len() >= CHUNK_OPS || nums + n > CHUNK_NUMS) {
                    out.push(NetOut::Send { to: vec![p.key.into()], kind: "annot.snap", body: json!({ "scope": name, "ops": std::mem::take(&mut cur) }) });
                    nums = 0; chunks += 1; if chunks >= REPLY_MAX_CHUNKS { break 'outer; }
                }
                cur.push(o.body.clone()); nums += n;
            }
            if !cur.is_empty() { out.push(NetOut::Send { to: vec![p.key.into()], kind: "annot.snap", body: json!({ "scope": name, "ops": cur }) }); chunks += 1; if chunks >= REPLY_MAX_CHUNKS { break; } }
        }
        out
    }

    /// Comando da UI (`annot.send`): `kind` = ops | laser | perm | drop | sync. Devolve o que enviar e eventos locais.
    pub fn on_cmd(&mut self, body: &Value, now: u64) -> Vec<NetOut> {
        let me = self.me.clone();
        match body.get("kind").and_then(Value::as_str).unwrap_or("") {
            "ops" => {
                let Some(scope) = body.get("scope").and_then(Value::as_str).filter(|s| parse_scope(s).is_some()).map(String::from) else { return vec![] };
                let Some(arr) = body.get("ops").and_then(Value::as_array).filter(|a| a.len() <= MAX_BATCH) else { return vec![] };
                if !self.may_draw(&scope, &me) { return vec![NetOut::Ev(json!({ "ev": "annot.denied", "scope": scope }))]; }
                let (mut ok, mut full) = (vec![], false);
                for o in arr {
                    let Some(op) = validate(o, &me) else { continue };
                    let b = op.body.clone();
                    match self.apply(&scope, op) { Applied::New => ok.push(b), Applied::Full => full = true, Applied::Denied => return vec![NetOut::Ev(json!({ "ev": "annot.denied", "scope": scope }))], _ => {} }
                }
                let mut out = vec![];
                if !ok.is_empty() { out.push(NetOut::Send { to: vec![], kind: "annot.ops", body: json!({ "scope": scope, "ops": ok }) }); }
                if full { out.push(NetOut::Ev(json!({ "ev": "annot.full", "scope": scope }))); }
                out
            }
            "laser" => {
                let Some(scope) = body.get("scope").and_then(Value::as_str).filter(|s| parse_scope(s).is_some()) else { return vec![] };
                if !self.may_draw(scope, &me) { return vec![]; }
                let (Some(x), Some(y), Some(col)) = (int(body, "x", 0, COORD_MAX), int(body, "y", 0, COORD_MAX), int(body, "col", 0, 0xFFFFFF)) else { return vec![] };
                vec![NetOut::Send { to: vec![], kind: "annot.laser", body: json!({ "scope": scope, "x": x, "y": y, "col": col }) }]
            }
            "perm" => {
                let all = body.get("all").and_then(Value::as_bool).unwrap_or(false);
                let fps: HashSet<String> = body.get("fps").and_then(Value::as_array).map(|a| a.iter().filter_map(Value::as_str).filter(|s| valid_fp(s) && *s != me).take(MAX_PERM_FPS).map(String::from).collect()).unwrap_or_default();
                let mut list: Vec<&String> = fps.iter().collect(); list.sort();
                let b = json!({ "all": all, "fps": list });
                self.perms.insert(me, Perm { all, fps });
                vec![NetOut::Send { to: vec![], kind: "annot.perm", body: b }]
            }
            "drop" => {
                if let Some(s) = body.get("scope").and_then(Value::as_str) { if matches!(parse_scope(s), Some(Some(_))) { self.drop_scope(s); } }
                vec![]
            }
            "drop_owner" => {   // a transmissão de `fp` acabou: some tudo dela e a permissão que ela deu
                if let Some(fp) = body.get("fp").and_then(Value::as_str).filter(|f| valid_fp(f)) {
                    for slot in ["0", "1"] { self.drop_scope(&format!("s:{fp}:{slot}")); }
                    if fp != me { self.perms.remove(fp); }
                }
                vec![]
            }
            "sync" => { self.asked_at = Some(now); self.snap_chunks = 0; vec![NetOut::Send { to: vec![], kind: "annot.want", body: json!({}) }] }
            _ => vec![],
        }
    }

    #[cfg(test)]
    fn live(&self, scope: &str) -> usize { self.scopes.get(scope).map(|s| s.ops.len()).unwrap_or(0) }
}

#[cfg(test)]
mod tests {
    use super::*;

    const A: &str = "aaaaaaaa11111111";
    const B: &str = "bbbbbbbb22222222";
    const C: &str = "cccccccc33333333";
    fn pr<'a>(fp: &'a str) -> PeerRef<'a> { PeerRef { key: fp, fp } }
    fn stroke(i: u32, c: u64, n: usize) -> Value { json!({ "i": i, "c": c, "k": "s", "m": 0, "col": 0xff0000, "w": 30, "p": (0..n * 2).map(|x| (x as i64 * 7) % 10_000).collect::<Vec<_>>() }) }
    fn ops(scope: &str, v: Vec<Value>) -> Value { json!({ "scope": scope, "ops": v }) }
    fn evs(o: &[NetOut]) -> Vec<&Value> { o.iter().filter_map(|x| if let NetOut::Ev(e) = x { Some(e) } else { None }).collect() }

    #[test]
    fn scope_parse() {
        assert_eq!(parse_scope("b:0"), Some(None)); assert_eq!(parse_scope("b:15"), Some(None));
        assert_eq!(parse_scope("b:16"), None); assert_eq!(parse_scope("b:01"), None); assert_eq!(parse_scope("b:-1"), None);
        assert_eq!(parse_scope(&format!("s:{A}:1")), Some(Some(A)));
        assert_eq!(parse_scope(&format!("s:{A}:2")), None); assert_eq!(parse_scope("s:x:0"), None); assert_eq!(parse_scope("zzz"), None);
    }

    #[test]
    fn board_accepts_valid_and_emits_with_author_from_channel() {
        let mut n = Net::new(A);
        let mut s = stroke(1, 1, 5); s["a"] = json!("forjado1234567890");
        let o = n.on_msg(&pr(B), "annot.ops", &ops("b:0", vec![s]), 1000);
        let e = evs(&o); assert_eq!(e.len(), 1);
        assert_eq!(e[0]["ops"][0]["a"], B);   // o autor é o do canal, não o do JSON
        assert_eq!(n.live("b:0"), 1);
    }

    #[test]
    fn rejects_invalid_fields() {
        let mut n = Net::new(A);
        let bad = vec![
            json!({ "i": 1, "c": 1, "k": "s", "m": 2, "col": 0, "w": 3, "p": [1, 2] }),
            json!({ "i": 2, "c": 1, "k": "s", "m": 0, "col": 0x1000000, "w": 3, "p": [1, 2] }),
            json!({ "i": 3, "c": 1, "k": "s", "m": 0, "col": 0, "w": 0, "p": [1, 2] }),
            json!({ "i": 4, "c": 1, "k": "s", "m": 0, "col": 0, "w": 3, "p": [1, 2, 3] }),
            json!({ "i": 5, "c": 1, "k": "s", "m": 0, "col": 0, "w": 3, "p": [1, 10001] }),
            json!({ "i": 6, "c": 1, "k": "s", "m": 0, "col": 0, "w": 3, "p": [-1, 5] }),
            json!({ "i": 7, "c": 1, "k": "s", "m": 0, "col": 0, "w": 3, "p": [] }),
            json!({ "i": 8, "c": 1, "k": "?", "p": [1, 2] }),
            json!({ "i": 9, "c": 1, "k": "g", "sh": "z", "col": 0, "w": 3, "f": 0, "x0": 1, "y0": 1, "x1": 2, "y1": 2 }),
            json!({ "i": 10, "c": 1, "k": "t", "s": "", "col": 0, "sz": 10, "x": 1, "y": 1 }),
            json!({ "i": 11, "c": 1, "k": "t", "s": "a\u{0}b", "col": 0, "sz": 10, "x": 1, "y": 1 }),
            json!({ "i": 12, "c": 1, "k": "t", "s": "x".repeat(MAX_TEXT + 1), "col": 0, "sz": 10, "x": 1, "y": 1 }),
            json!({ "i": 13, "c": 1, "k": "d", "tg": { "a": "curto", "i": 1 } }),
            json!({ "i": 14, "c": 1, "k": "x" }),
            json!({ "i": "um", "c": 1, "k": "x", "all": true }),
            json!({ "i": 1, "c": 1, "k": "s", "m": 0, "col": 0, "w": 3, "p": (0..(MAX_PTS_STROKE + 1) * 2).map(|_| 1).collect::<Vec<i64>>() }),
        ];
        for b in bad { assert!(n.on_msg(&pr(B), "annot.ops", &ops("b:0", vec![b.clone()]), 1000).is_empty(), "{b}"); }
        assert_eq!(n.live("b:0"), 0);
        assert!(n.on_msg(&pr(B), "annot.ops", &ops("b:99", vec![stroke(1, 1, 2)]), 1000).is_empty());
        assert!(n.on_msg(&pr(B), "annot.ops", &json!({ "scope": "b:0", "ops": "x" }), 1000).is_empty());
        assert!(n.on_msg(&pr(B), "annot.ops", &json!(42), 1000).is_empty());
    }

    #[test]
    fn merge_is_order_independent() {
        // no Board (sem dono): o apagar de A vale sobre o traço de B; mesmo se o apagar chegar antes do traço
        let mk_b = |order: &[usize]| {
            let mut n = Net::new(C);
            let all = vec![(A, stroke(1, 1, 3)), (B, stroke(1, 2, 3)), (A, json!({ "i": 2, "c": 3, "k": "d", "tg": { "a": B, "i": 1 } }))];
            for (t, &ix) in order.iter().enumerate() { let (fp, op) = &all[ix]; n.on_msg(&pr(fp), "annot.ops", &ops("b:1", vec![op.clone()]), 1000 + t as u64 * 2000); }
            n
        };
        let x = mk_b(&[0, 1, 2]); let y = mk_b(&[2, 1, 0]); let z = mk_b(&[1, 2, 0]);
        for n in [&x, &y, &z] { assert_eq!(n.live("b:1"), 1); assert!(n.scopes["b:1"].ops[0].a == A); }
    }

    #[test]
    fn stream_permissions() {
        let scope = format!("s:{A}:0");
        let mut n = Net::new(C);
        // B não pode até o dono liberar
        assert!(n.on_msg(&pr(B), "annot.ops", &ops(&scope, vec![stroke(1, 1, 2)]), 1000).is_empty());
        assert!(!n.on_msg(&pr(A), "annot.ops", &ops(&scope, vec![stroke(1, 1, 2)]), 1000).is_empty());   // dono sempre
        // permissão só vale se vier do dono: B tentando liberar a si mesmo libera apenas as transmissões dele
        n.on_msg(&pr(B), "annot.perm", &json!({ "all": true, "fps": [] }), 1000);
        assert!(n.on_msg(&pr(B), "annot.ops", &ops(&scope, vec![stroke(1, 1, 2)]), 2000).is_empty());
        assert!(!n.on_msg(&pr(B), "annot.ops", &ops(&format!("s:{B}:0"), vec![stroke(1, 1, 2)]), 2000).is_empty());
        // dono libera B
        n.on_msg(&pr(A), "annot.perm", &json!({ "all": false, "fps": [B] }), 1000);
        assert!(!n.on_msg(&pr(B), "annot.ops", &ops(&scope, vec![stroke(1, 1, 2)]), 3000).is_empty());
        assert!(n.on_msg(&pr(C), "annot.ops", &ops(&scope, vec![stroke(1, 1, 2)]), 3000).is_empty());
        // B não apaga o traço do dono; o dono apaga o de B
        let del_a = json!({ "i": 5, "c": 9, "k": "d", "tg": { "a": A, "i": 1 } });
        assert!(n.on_msg(&pr(B), "annot.ops", &ops(&scope, vec![del_a.clone()]), 4000).is_empty());
        assert_eq!(n.live(&scope), 2);
        let del_b = json!({ "i": 6, "c": 9, "k": "d", "tg": { "a": B, "i": 1 } });
        assert!(!n.on_msg(&pr(A), "annot.ops", &ops(&scope, vec![del_b]), 5000).is_empty());
        assert_eq!(n.live(&scope), 1);
        // "limpar tudo" só do dono; B só limpa os seus
        let clr_all = json!({ "i": 7, "c": 20, "k": "x", "all": true });
        assert!(n.on_msg(&pr(B), "annot.ops", &ops(&scope, vec![clr_all.clone()]), 6000).is_empty());
        assert!(!n.on_msg(&pr(A), "annot.ops", &ops(&scope, vec![clr_all]), 7000).is_empty());
        assert_eq!(n.live(&scope), 0);
    }

    #[test]
    fn local_send_needs_permission_and_stores_for_snapshot() {
        let scope = format!("s:{B}:0");
        let mut n = Net::new(A);
        let o = n.on_cmd(&json!({ "kind": "ops", "scope": scope, "ops": [stroke(1, 1, 3)] }), 1000);
        assert!(matches!(&o[0], NetOut::Ev(e) if e["ev"] == "annot.denied"));
        let o = n.on_cmd(&json!({ "kind": "ops", "scope": "b:2", "ops": [stroke(1, 1, 3), json!({ "i": 9 })] }), 1000);
        match &o[0] { NetOut::Send { kind, body, to } => { assert_eq!(*kind, "annot.ops"); assert!(to.is_empty()); assert_eq!(body["ops"].as_array().unwrap().len(), 1); } _ => panic!() }
        // alguém novo pede o estado: recebe o que tenho; só aceita snap se pediu
        let w = n.on_msg(&pr(C), "annot.want", &json!({}), 5000);
        assert!(w.iter().any(|x| matches!(x, NetOut::Send { kind: "annot.snap", to, .. } if to == &vec![C.to_string()])));
        assert!(n.on_msg(&pr(C), "annot.want", &json!({}), 6000).is_empty());   // pedido repetido: limitado
        let snap = w.into_iter().find_map(|x| if let NetOut::Send { kind: "annot.snap", body, .. } = x { Some(body) } else { None }).unwrap();
        let mut late = Net::new(C);
        assert!(late.on_msg(&pr(A), "annot.snap", &snap, 1000).is_empty());   // não pedi
        late.on_cmd(&json!({ "kind": "sync" }), 1000);
        let o = late.on_msg(&pr(A), "annot.snap", &snap, 2000);
        assert_eq!(evs(&o).len(), 1); assert_eq!(late.live("b:2"), 1);
        assert!(late.on_msg(&pr(A), "annot.snap", &snap, 2000 + SNAP_WINDOW_MS + 1).is_empty());   // janela expirou
    }

    #[test]
    fn limits_ops_points_and_rate() {
        let mut n = Net::new(A);
        // teto de operações por escopo
        for i in 0..(MAX_OPS as u32 + 5) { n.on_cmd(&json!({ "kind": "ops", "scope": "b:3", "ops": [stroke(i, i as u64, 1)] }), 1000); }
        assert_eq!(n.live("b:3"), MAX_OPS);
        // acréscimo de pontos respeita 2000 por traço
        let mut m = Net::new(A);
        m.on_cmd(&json!({ "kind": "ops", "scope": "b:4", "ops": [stroke(1, 1, 1990)] }), 1000);
        let p = json!({ "i": 2, "c": 2, "k": "p", "tg": 1, "p": (0..40).map(|x| x % 100).collect::<Vec<i64>>() });
        let o = m.on_cmd(&json!({ "kind": "ops", "scope": "b:4", "ops": [p] }), 1000);
        assert!(matches!(&o[0], NetOut::Ev(e) if e["ev"] == "annot.full"));
        // taxa por autor: 61ª mensagem em 1 s é descartada
        let mut r = Net::new(A); let mut got = 0;
        for i in 0..80u32 { if !r.on_msg(&pr(B), "annot.ops", &ops("b:5", vec![stroke(i, i as u64, 1)]), 1000 + i as u64).is_empty() { got += 1; } }
        assert_eq!(got, RATE_MSGS);
        assert!(!r.on_msg(&pr(B), "annot.ops", &ops("b:5", vec![stroke(500, 500, 1)]), 2500).is_empty());   // janela seguinte
        // limite de escopos
        let mut s = Net::new(A);
        for k in 0..16 { s.on_cmd(&json!({ "kind": "ops", "scope": format!("b:{k}"), "ops": [stroke(1, 1, 1)] }), 1000); }
        for slot in ["0", "1"] { for k in 0..8u8 { let fp = format!("dddddddd{k:08}"); s.on_msg(&pr(&fp), "annot.ops", &ops(&format!("s:{fp}:{slot}"), vec![stroke(1, 1, 1)]), 1000); } }
        assert!(s.scopes.len() <= MAX_SCOPES);
    }

    #[test]
    fn append_and_delete_and_clear_own() {
        let mut n = Net::new(C);
        n.on_msg(&pr(A), "annot.ops", &ops("b:0", vec![stroke(1, 1, 2)]), 1000);
        n.on_msg(&pr(A), "annot.ops", &ops("b:0", vec![json!({ "i": 2, "c": 2, "k": "p", "tg": 1, "p": [5, 5, 6, 6] })]), 1100);
        assert_eq!(n.scopes["b:0"].ops[0].pts, 4);
        // acréscimo vindo de outro autor ao traço de A não vale
        n.on_msg(&pr(B), "annot.ops", &ops("b:0", vec![json!({ "i": 9, "c": 3, "k": "p", "tg": 1, "p": [1, 1] })]), 1200);
        assert_eq!(n.scopes["b:0"].ops[0].pts, 4);
        n.on_msg(&pr(B), "annot.ops", &ops("b:0", vec![stroke(1, 4, 2)]), 1300);
        // B limpa só o dele
        n.on_msg(&pr(B), "annot.ops", &ops("b:0", vec![json!({ "i": 10, "c": 5, "k": "x", "all": false })]), 1400);
        assert_eq!(n.live("b:0"), 1);
        // traço atrasado de B (c menor que o limpar) não volta
        n.on_msg(&pr(B), "annot.ops", &ops("b:0", vec![stroke(2, 3, 2)]), 1500);
        assert_eq!(n.live("b:0"), 1);
        // apagado não ressuscita
        n.on_msg(&pr(A), "annot.ops", &ops("b:0", vec![json!({ "i": 11, "c": 6, "k": "d", "tg": { "a": A, "i": 1 } })]), 1600);
        n.on_msg(&pr(A), "annot.ops", &ops("b:0", vec![stroke(1, 1, 2)]), 1700);
        assert_eq!(n.live("b:0"), 0);
    }

    #[test]
    fn laser_and_perm_validation() {
        let mut n = Net::new(A);
        assert!(n.on_msg(&pr(B), "annot.laser", &json!({ "scope": "b:0", "x": 10001, "y": 5, "col": 1 }), 1).is_empty());
        assert_eq!(n.on_msg(&pr(B), "annot.laser", &json!({ "scope": "b:0", "x": 100, "y": 5, "col": 1 }), 1).len(), 1);
        let mut c = 0; for t in 0..100 { if !n.on_msg(&pr(C), "annot.laser", &json!({ "scope": "b:0", "x": 1, "y": 5, "col": 1 }), 10 + t).is_empty() { c += 1; } }
        assert_eq!(c, RATE_LASER);
        // fps inválidos e excesso na lista de permissão são cortados
        let many: Vec<String> = (0..100).map(|k| format!("eeeeeeee{k:08}")).collect();
        let o = n.on_msg(&pr(B), "annot.perm", &json!({ "all": false, "fps": many }), 1);
        assert_eq!(evs(&o)[0]["fps"].as_array().unwrap().len(), MAX_PERM_FPS);
        assert!(n.on_msg(&pr(B), "annot.perm", &json!({ "all": false, "fps": many }), 2).is_empty());   // sem mudança: sem evento
        let bad = n.on_msg(&pr(C), "annot.perm", &json!({ "all": false, "fps": ["x", 5, "ok"] }), 1);
        assert_eq!(evs(&bad)[0]["fps"].as_array().unwrap().len(), 0);
    }

    #[test]
    fn drop_owner_forgets_everything() {
        let scope = format!("s:{A}:0");
        let mut n = Net::new(C);
        n.on_msg(&pr(A), "annot.ops", &ops(&scope, vec![stroke(1, 1, 2)]), 1000);
        n.on_msg(&pr(A), "annot.perm", &json!({ "all": true }), 1000);
        n.on_cmd(&json!({ "kind": "drop_owner", "fp": A }), 2000);
        assert_eq!(n.live(&scope), 0);
        assert!(n.on_msg(&pr(B), "annot.ops", &ops(&scope, vec![stroke(1, 1, 2)]), 3000).is_empty());   // a liberação também sumiu
    }

    #[test]
    fn fuzz_garbage_never_panics() {
        let mut n = Net::new(A);
        let junk = [json!(null), json!([]), json!({ "scope": 5 }), json!({ "scope": "b:0", "ops": [null, 1, "x", [], { "k": 1 }, { "i": -1 }] }), json!({ "scope": "b:0", "ops": [{ "i": 1, "c": 1, "k": "s", "m": 0, "col": 0, "w": 3, "p": [1e30, 2] }] }), json!({ "scope": "s::0" }), json!({ "kind": 3 })];
        for (t, j) in junk.iter().enumerate() {
            for k in ["annot.ops", "annot.snap", "annot.laser", "annot.perm", "annot.want", "annot.zzz"] { n.on_msg(&pr(B), k, j, 100_000 * (t as u64 + 1)); }
            n.on_cmd(j, 1);
        }
        assert_eq!(n.live("b:0"), 0);
    }
}
