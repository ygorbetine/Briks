//! `codec.probe`: descobre o que o PC aguenta para a transmissão e recomenda o codec (modo "Automático" da UI).
//!
//! Três partes: (a) ficha técnica (codificador H.264 de hardware pelo Media Foundation e núcleos da CPU), (b) teste curto
//! (~3 s no total) que codifica quadros sintéticos de tela em cada codec disponível, na resolução e no FPS pedidos, medindo
//! ms por quadro e uso de CPU, e (c) `recommend`, função pura que junta as duas coisas. O teste real decide; a ficha só desempata.
use crate::codec::Codec;
use crate::video::Yuv;
use serde_json::{json, Value};
use std::time::{Duration, Instant};

/// Tempo total do teste (preparação incluída), repartido entre os codecs testados.
const TOTAL_MS: u64 = 3000;
/// Quanto do tempo de um quadro o codificador pode gastar e ainda contar como "aguenta": sobra o resto para o jogo/app transmitido.
const LIMIT_HW: f64 = 0.60;
const LIMIT_SW: f64 = 0.60;
/// VP9 pesa também em quem assiste (decodificação por CPU), por isso a régua é mais apertada.
const LIMIT_VP9: f64 = 0.45;

/// Resultado do teste de um codec.
#[derive(Clone, Debug, Default)]
pub struct Run {
    pub codec: &'static str,
    pub hardware: bool,
    /// nome do codificador (MFT) ou "libvpx"
    pub engine: String,
    pub tested: bool,
    pub error: Option<String>,
    /// média e p95 de ms por quadro
    pub ms: f64,
    pub p95: f64,
    /// uso do processador inteiro durante o teste, 0..100
    pub cpu: f64,
    pub frames: u32,
}

/// Ficha técnica do PC.
#[derive(Clone, Debug, Default)]
pub struct Machine {
    pub hw_name: Option<String>,
    pub threads: usize,
    pub cores: usize,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Pick {
    pub codec: Codec,
    pub hardware: bool,
    pub reason: String,
    /// quando nada aguenta com folga: altura (px) sugerida
    pub suggest_height: Option<u32>,
}

fn budget_ms(fps: u32) -> f64 { 1000.0 / fps.max(1) as f64 }

fn load(r: &Run, fps: u32) -> f64 { r.ms / budget_ms(fps) }

fn passes(r: &Run, fps: u32, limit: f64) -> bool { r.tested && r.error.is_none() && r.frames > 0 && load(r, fps) <= limit && r.p95 <= budget_ms(fps) }

/// "placa NVIDIA", "placa AMD", "vídeo integrado Intel" a partir do nome do MFT.
pub fn vendor_label(mft: &str) -> String {
    let l = mft.to_ascii_lowercase();
    if l.contains("nvidia") || l.contains("nvenc") { "placa NVIDIA".into() }
    else if l.contains("amd") || l.contains("radeon") || l.contains("amf") { "placa AMD".into() }
    else if l.contains("intel") || l.contains("quick") { "vídeo integrado Intel".into() }
    else { "codificador de vídeo na placa".into() }
}

/// Junta ficha técnica e teste real. Ordem de preferência: H.264 por placa; sem placa e CPU forte (8+ núcleos), VP9 até 1080p30;
/// senão H.264 por software ou VP8. A primeira opção da ordem que PASSA no teste ganha; se nenhuma passa, escolhe a mais folgada
/// e sugere baixar a resolução.
pub fn recommend(m: &Machine, runs: &[Run], height: u32, fps: u32) -> Pick {
    let find = |c: &str, hw: bool| runs.iter().find(|r| r.codec == c && r.hardware == hw);
    if let (Some(name), Some(r)) = (&m.hw_name, find("h264", true)) {
        if passes(r, fps, LIMIT_HW) {
            return Pick { codec: Codec::H264, hardware: true, suggest_height: None,
                reason: format!("Seu PC tem {}: use H.264, quase não usa o processador.", vendor_label(name)) };
        }
    }
    let strong = m.cores >= 8;
    if strong && height <= 1080 && fps <= 30 {
        if let Some(r) = find("vp9", false) {
            if passes(r, fps, LIMIT_VP9) {
                return Pick { codec: Codec::Vp9, hardware: false, suggest_height: None,
                    reason: format!("Seu processador é forte ({} núcleos): VP9 dá imagem mais nítida com a mesma internet. Quem assiste também precisa de um PC razoável; se não der, o app volta para H.264 sozinho.", m.cores) };
            }
        }
    }
    if let Some(r) = find("h264", false) {
        if passes(r, fps, LIMIT_SW) {
            let why = if m.hw_name.is_some() { "A placa de vídeo não deu conta no teste" } else { "Seu PC não tem placa de vídeo compatível" };
            return Pick { codec: Codec::H264, hardware: false, suggest_height: None,
                reason: format!("{why}: H.264 pelo processador roda com folga e funciona na tela de todo mundo.") };
        }
    }
    if let Some(r) = find("vp8", false) {
        if passes(r, fps, LIMIT_SW) {
            return Pick { codec: Codec::Vp8, hardware: false, suggest_height: None,
                reason: "Seu processador é mais simples: VP8 é o que pesa menos para codificar aqui.".into() };
        }
    }
    // nada passou: o menos pesado dos que rodaram, e baixar a resolução
    let best = runs.iter().filter(|r| r.tested && r.error.is_none() && r.frames > 0).min_by(|a, b| load(a, fps).partial_cmp(&load(b, fps)).unwrap_or(std::cmp::Ordering::Equal));
    let (codec, hardware) = match best { Some(r) => (Codec::from_name(r.codec).unwrap_or(Codec::H264), r.hardware), None => (Codec::H264, false) };
    let suggest = if height > 720 { Some(720) } else if height > 480 { Some(480) } else if height > 360 { Some(360) } else { None };
    let reason = match suggest {
        Some(h) => format!("Seu PC está no limite com {height}p a {fps} FPS: use {h}p ou menos para a transmissão não travar."),
        None => "Seu PC está no limite mesmo em resolução baixa: reduza os FPS para a transmissão não travar.".into(),
    };
    Pick { codec, hardware, reason, suggest_height: suggest }
}

// ------------------------------------------------------------------------------------------------------------------
// ficha técnica
// ------------------------------------------------------------------------------------------------------------------

/// Núcleos físicos (Windows); se a consulta falhar, metade das threads (hyper-threading) ou as próprias threads em PCs pequenos.
pub fn physical_cores(threads: usize) -> usize {
    #[cfg(windows)]
    unsafe {
        use windows::Win32::System::SystemInformation::{GetLogicalProcessorInformation, RelationProcessorCore, SYSTEM_LOGICAL_PROCESSOR_INFORMATION};
        let mut len = 0u32;
        let _ = GetLogicalProcessorInformation(None, &mut len);
        let sz = std::mem::size_of::<SYSTEM_LOGICAL_PROCESSOR_INFORMATION>() as u32;
        if len >= sz {
            let mut v: Vec<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> = vec![std::mem::zeroed(); (len / sz) as usize];
            if GetLogicalProcessorInformation(Some(v.as_mut_ptr()), &mut len).is_ok() {
                let n = v.iter().take((len / sz) as usize).filter(|i| i.Relationship == RelationProcessorCore).count();
                if n > 0 { return n; }
            }
        }
    }
    if threads >= 8 { threads / 2 } else { threads.max(1) }
}

/// Tempo de CPU do processo (usuário + kernel), em 100 ns.
fn cpu_time() -> u64 {
    #[cfg(windows)]
    unsafe {
        use windows::Win32::Foundation::FILETIME;
        use windows::Win32::System::Threading::{GetCurrentProcess, GetProcessTimes};
        let (mut c, mut e, mut k, mut u) = (FILETIME::default(), FILETIME::default(), FILETIME::default(), FILETIME::default());
        if GetProcessTimes(GetCurrentProcess(), &mut c, &mut e, &mut k, &mut u).is_ok() {
            let t = |f: FILETIME| ((f.dwHighDateTime as u64) << 32) | f.dwLowDateTime as u64;
            return t(k) + t(u);
        }
    }
    0
}

// ------------------------------------------------------------------------------------------------------------------
// quadros sintéticos e medição
// ------------------------------------------------------------------------------------------------------------------

/// Quadros BGRA parecidos com tela em movimento: fundo com blocos e gradiente, mais uma região de "ruído" (jogo/vídeo) que muda a cada quadro.
fn frames(w: usize, h: usize, n: usize) -> Vec<Yuv> {
    (0..n).map(|t| {
        let mut f = crate::capture::test_pattern(w, h, t * 3);
        let mut s = 0x9E3779B9u32 ^ (t as u32).wrapping_mul(2654435761);
        let (x0, y0, x1, y1) = (w / 4, h / 4, w * 3 / 4, h * 3 / 4);
        for y in y0..y1 { for x in x0..x1 {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            let o = (y * w + x) * 4;
            let base = f[o] as u32;
            f[o] = ((base + (s & 0x3F)) & 0xFF) as u8; f[o + 1] = f[o + 1].wrapping_add((s >> 8) as u8 & 0x1F); f[o + 2] = f[o + 2].wrapping_add((s >> 16) as u8 & 0x1F);
        } }
        crate::video::bgra_to_i420(&f, w, h, w, h)
    }).collect()
}

fn stats(run: &mut Run, mut ms: Vec<f64>, wall: Duration, cpu100ns: u64, threads: usize) {
    run.frames = ms.len() as u32;
    if ms.is_empty() { return; }
    run.ms = ms.iter().sum::<f64>() / ms.len() as f64;
    ms.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
    run.p95 = ms[((ms.len() as f64 * 0.95) as usize).min(ms.len() - 1)];
    let wall100 = wall.as_secs_f64() * 1e7;
    if wall100 > 0.0 && threads > 0 { run.cpu = (cpu100ns as f64 / wall100 / threads as f64 * 100.0).clamp(0.0, 100.0); }
}

/// Roda `step` em quadros em ciclo durante `slice` (mínimo 8 quadros medidos, máximo 240), depois de 3 de aquecimento.
fn measure(run: &mut Run, slice: Duration, threads: usize, n_frames: usize, mut step: impl FnMut(usize, bool) -> anyhow::Result<()>) {
    for i in 0..3 { if let Err(e) = step(i % n_frames, i == 0) { run.error = Some(e.to_string()); return; } }
    let (mut ms, t0, c0) = (Vec::new(), Instant::now(), cpu_time());
    let mut i = 3usize;
    while (t0.elapsed() < slice || ms.len() < 8) && ms.len() < 240 {
        let t = Instant::now();
        if let Err(e) = step(i % n_frames, false) { run.error = Some(e.to_string()); break; }
        ms.push(t.elapsed().as_secs_f64() * 1000.0);
        i += 1;
    }
    let (wall, c1) = (t0.elapsed(), cpu_time());
    run.tested = true;
    stats(run, ms, wall, c1.saturating_sub(c0), threads);
}

fn bench_h264(fr: &[Yuv], w: usize, h: usize, fps: u32, bps: u32, hw: bool, slice: Duration, threads: usize) -> Run {
    let mut run = Run { codec: "h264", hardware: hw, ..Default::default() };
    match crate::mfenc::MfEnc::new(w, h, fps, bps, hw) {
        Err(e) => { run.error = Some(e.to_string()); }
        Ok(mut enc) => {
            if enc.hardware != hw { run.error = Some("sem codificador desse tipo".into()); return run; }
            run.engine = enc.name.clone();
            let mut nv = Vec::new();
            measure(&mut run, slice, threads, fr.len(), |i, key| {
                crate::mfenc::i420_to_nv12(&fr[i].y, &fr[i].u, &fr[i].v, &mut nv);
                enc.encode(&nv, key).map(|_| ())
            });
        }
    }
    run
}

#[cfg(feature = "vpx")]
fn bench_vpx(codec: Codec, fr: &[Yuv], w: usize, h: usize, fps: u32, bps: u32, slice: Duration, threads: usize) -> Run {
    let mut run = Run { codec: codec.name(), hardware: false, engine: "libvpx".into(), ..Default::default() };
    match crate::vpx::VpxEnc::new(codec, w, h, fps as f32, bps) {
        Err(e) => { run.error = Some(e.to_string()); }
        Ok(mut enc) => measure(&mut run, slice, threads, fr.len(), |i, key| { if key { enc.force_idr(); } enc.encode(&fr[i]).map(|_| ()) }),
    }
    run
}

/// Teste completo. Bloqueante (~3 s): chamar em thread própria.
pub fn run(height: u32, fps: u32) -> Value { run_opts(height, fps, std::env::var_os("BRIKS_PROBE_NOHW").is_none()) }

/// `allow_hw = false` finge um PC sem placa (testes e diagnóstico: `BRIKS_PROBE_NOHW=1`).
pub fn run_opts(height: u32, fps: u32, allow_hw: bool) -> Value {
    let started = Instant::now();
    let height = height.clamp(240, 2160);
    let fps = fps.clamp(5, 120);
    let (w, h) = (((height as usize * 16 / 9) & !1).max(2), (height as usize) & !1);
    let threads = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(2);
    let mut m = Machine { hw_name: None, threads, cores: physical_cores(threads) };
    let bps = ((w * h) as f64 * fps as f64 * 0.07).clamp(2_000_000.0, 12_000_000.0) as u32;
    let fr = frames(w, h, 6);
    let mut runs: Vec<Run> = vec![];

    // O prazo de ~3 s já conta a preparação (quadros sintéticos): cada teste recebe uma fatia do que sobra.
    let deadline = started + Duration::from_millis(TOTAL_MS);
    let slice_for = |left: u32| (deadline.saturating_duration_since(Instant::now()) / left.max(1)).max(Duration::from_millis(150));
    let n_vpx: u32 = if cfg!(feature = "vpx") { 2 } else { 0 };
    // 1) H.264 por hardware, se o Media Foundation oferece um codificador de placa (sem placa a tentativa falha na hora)
    let hw_probe = if allow_hw { bench_h264(&fr, w, h, fps, bps, true, slice_for(2 + n_vpx), threads) } else { Run { codec: "h264", hardware: true, error: Some("desligado".into()), ..Default::default() } };
    if hw_probe.error.is_none() { m.hw_name = Some(hw_probe.engine.clone()); }
    runs.push(hw_probe);
    // 2) os de software
    runs.push(bench_h264(&fr, w, h, fps, bps, false, slice_for(1 + n_vpx), threads));
    #[cfg(feature = "vpx")]
    {
        runs.push(bench_vpx(Codec::Vp9, &fr, w, h, fps, bps, slice_for(2), threads));
        runs.push(bench_vpx(Codec::Vp8, &fr, w, h, fps, bps, slice_for(1), threads));
    }
    let pick = recommend(&m, &runs, height, fps);
    let budget = budget_ms(fps);
    let list: Vec<Value> = runs.iter().filter(|r| r.tested || r.error.is_some()).map(|r| {
        let ok = passes(r, fps, if r.codec == "vp9" { LIMIT_VP9 } else if r.hardware { LIMIT_HW } else { LIMIT_SW });
        json!({
            "codec": r.codec, "hardware": r.hardware, "engine": r.engine, "tested": r.tested, "error": r.error,
            "ms": (r.ms * 100.0).round() / 100.0, "p95": (r.p95 * 100.0).round() / 100.0, "budget_ms": (budget * 100.0).round() / 100.0,
            "cpu": r.cpu.round(), "frames": r.frames, "realtime": ok,
            "headroom": if r.tested && r.error.is_none() { ((1.0 - load(r, fps)) * 100.0).round() / 100.0 } else { -1.0 },
        })
    }).collect();
    json!({
        "ev": "codec.result", "ok": true,
        "recommended": pick.codec.name(), "hardware": pick.hardware, "reason": pick.reason, "suggest_height": pick.suggest_height,
        "hw_name": m.hw_name, "threads": m.threads, "cores": m.cores, "vpx": cfg!(feature = "vpx"),
        "width": w, "height": h, "fps": fps, "took_ms": started.elapsed().as_millis() as u64, "results": list,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn r(codec: &'static str, hw: bool, ms: f64) -> Run { Run { codec, hardware: hw, engine: if hw { "NVIDIA H.264 Encoder MFT".into() } else { "x".into() }, tested: true, error: None, ms, p95: ms * 1.3, cpu: 10.0, frames: 30 } }
    fn m(hw: bool, cores: usize) -> Machine { Machine { hw_name: hw.then(|| "NVIDIA H.264 Encoder MFT".to_string()), threads: cores * 2, cores } }

    #[test]
    fn hardware_ganha_e_fala_da_placa() {
        let p = recommend(&m(true, 4), &[r("h264", true, 3.0), r("h264", false, 12.0), r("vp9", false, 10.0)], 1080, 60);
        assert_eq!((p.codec, p.hardware), (Codec::H264, true));
        assert!(p.reason.contains("placa NVIDIA"), "{}", p.reason);
    }
    #[test]
    fn sem_placa_cpu_forte_vai_de_vp9_ate_1080p30() {
        let runs = [r("h264", false, 9.0), r("vp9", false, 10.0), r("vp8", false, 6.0)];
        assert_eq!(recommend(&m(false, 8), &runs, 1080, 30).codec, Codec::Vp9);
        // 60 FPS: a regra não permite VP9
        let p60 = recommend(&m(false, 8), &[r("h264", false, 5.0), r("vp9", false, 5.0), r("vp8", false, 3.0)], 1080, 60);
        assert_eq!(p60.codec, Codec::H264);
        // 1440p: idem
        assert_eq!(recommend(&m(false, 8), &runs, 1440, 30).codec, Codec::H264);
    }
    #[test]
    fn teste_real_derruba_a_ficha_tecnica() {
        // CPU forte na ficha, mas o VP9 não cabe no tempo do quadro (33 ms a 30 FPS; limite 45%)
        let p = recommend(&m(false, 12), &[r("h264", false, 8.0), r("vp9", false, 20.0)], 1080, 30);
        assert_eq!(p.codec, Codec::H264);
        // placa que falhou no teste cai para software
        let p = recommend(&m(true, 4), &[r("h264", true, 40.0), r("h264", false, 10.0)], 1080, 30);
        assert_eq!((p.codec, p.hardware), (Codec::H264, false));
        assert!(p.reason.contains("placa de vídeo"), "{}", p.reason);
    }
    #[test]
    fn cpu_fraca_h264_software_ou_vp8() {
        let p = recommend(&m(false, 4), &[r("h264", false, 12.0), r("vp8", false, 8.0), r("vp9", false, 5.0)], 720, 30);
        assert_eq!(p.codec, Codec::H264);   // VP9 nem entra: poucos núcleos
        let p = recommend(&m(false, 2), &[r("h264", false, 30.0), r("vp8", false, 9.0)], 720, 30);
        assert_eq!(p.codec, Codec::Vp8);
    }
    #[test]
    fn nada_aguenta_sugere_baixar_resolucao() {
        let p = recommend(&m(false, 2), &[r("h264", false, 70.0), r("vp8", false, 60.0)], 1080, 30);
        assert_eq!(p.suggest_height, Some(720));
        assert_eq!(p.codec, Codec::Vp8);   // o de menor carga
        assert!(p.reason.contains("720p"));
    }
    #[test]
    fn nada_testado_cai_em_h264() {
        let p = recommend(&Machine { threads: 4, cores: 2, hw_name: None }, &[], 720, 30);
        assert_eq!(p.codec, Codec::H264);
    }

    /// Simula PC sem placa: a reserva de software tem de rodar e a recomendação nunca pode ser "por placa".
    #[test]
    fn probe_real_sem_placa_usa_software() {
        let v = run_opts(360, 30, false);
        assert!(v["hw_name"].is_null(), "{v}");
        assert_eq!(v["hardware"], false, "{v}");
        let sw = v["results"].as_array().unwrap().iter().find(|r| r["codec"] == "h264" && r["hardware"] == false).expect("sem H.264 por software");
        assert!(sw["tested"] == true && sw["error"].is_null() && sw["frames"].as_u64().unwrap() >= 8, "{sw}");
        assert!(v["took_ms"].as_u64().unwrap() < 6000, "{v}");
    }

    /// Teste real, sem GPU obrigatória: com placa dá H.264 por hardware; sem, a reserva de software tem de rodar e a resposta ser coerente.
    #[test]
    fn probe_real_pequeno_responde_com_reserva_de_software() {
        let v = run(360, 30);
        assert_eq!(v["ev"], "codec.result");
        let rec = v["recommended"].as_str().unwrap();
        assert!(["h264", "vp8", "vp9"].contains(&rec), "{v}");
        assert!(!v["reason"].as_str().unwrap().is_empty());
        assert!(v["cores"].as_u64().unwrap() >= 1 && v["threads"].as_u64().unwrap() >= v["cores"].as_u64().unwrap());
        let res = v["results"].as_array().unwrap();
        let h264: Vec<_> = res.iter().filter(|r| r["codec"] == "h264" && r["tested"] == true && r["error"].is_null()).collect();
        assert!(!h264.is_empty(), "nem hardware nem software H.264 rodaram: {v}");
        assert!(h264.iter().all(|r| r["frames"].as_u64().unwrap() >= 8 && r["ms"].as_f64().unwrap() > 0.0));
        if v["hw_name"].is_null() { assert_eq!(v["hardware"], false, "sem placa não pode recomendar hardware: {v}"); }
        assert!(v["took_ms"].as_u64().unwrap() < 9000, "o teste passou muito de 3 s: {v}");
        eprintln!("{}", serde_json::to_string_pretty(&v).unwrap());
    }

    /// De ponta a ponta pelo IPC: `codec.probe` pede o teste e o core responde `codec.result` com o mesmo `id`.
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn ipc_codec_probe_responde_codec_result() {
        use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
        let dir = std::env::temp_dir().join(format!("briks-probe-ipc-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir); std::fs::create_dir_all(&dir).unwrap();
        let (l, port) = crate::ipc::bind().await.unwrap();
        let cfg = crate::ipc::Config { data_dir: dir, realtime_url: None, realtime_key: None };
        tokio::spawn(crate::ipc::serve(l, "segredo".into(), cfg));
        let s = tokio::net::TcpStream::connect(("127.0.0.1", port)).await.unwrap();
        let (rd, mut wr) = s.into_split();
        wr.write_all(b"{\"cmd\":\"hello\",\"token\":\"segredo\"}
{\"cmd\":\"codec.probe\",\"height\":360,\"fps\":30,\"id\":\"abc\"}
").await.unwrap();
        let mut lines = BufReader::new(rd).lines();
        let got = tokio::time::timeout(Duration::from_secs(20), async { while let Ok(Some(l)) = lines.next_line().await { let v: Value = serde_json::from_str(&l).unwrap(); if v["ev"] == "codec.result" { return Some(v); } } None }).await.expect("sem resposta em 20 s").expect("conexão fechou");
        assert_eq!(got["ok"], true, "{got}");
        assert_eq!(got["id"], "abc");
        assert!(got["recommended"].is_string() && got["results"].as_array().map_or(false, |a| !a.is_empty()), "{got}");
    }
}
