//! Conversa privada (DM) fora da sala de voz: um canal direto entre dois amigos, aberto pela presença, sem servidor guardando mensagem.
//!
//! Como se encontram: cada pessoa, com o Briks aberto e logada, observa a PRÓPRIA caixa de chegada: um canal de presença do Realtime
//! cujo nome é um hash do id da conta (`inbox:<id>`). Quem quer escrever (e já tem a impressão digital do amigo, vinda da lista de
//! amigos da conta) entra na caixa do amigo só para ficar visível com `{fp}` ("toque") e abre, ao mesmo tempo, a sua ponta do canal
//! direto `chat:<dm-do-par>` (a mesma malha P2P autenticada das chamadas, só com DataChannel, sem áudio). Quem observa a caixa vê o
//! toque, a interface confere que `fp` é de um amigo e abre a outra ponta; os dois se acham, provam a identidade com a chave de cada
//! um e a caixa de saída entrega o que estava esperando (mais o histórico assinado que faltar de cada lado).
//!
//! Privacidade: o toque carrega só a impressão digital de quem escreveu (nunca texto, nome ou id de conta); o conteúdo das
//! mensagens só passa pelo canal direto autenticado. Nada fica guardado no servidor: se o amigo está offline, a mensagem espera na
//! caixa de saída DESTE computador (e no histórico assinado local) e segue quando os dois estiverem online; o toque some quando o
//! canal direto abre, quando a conversa é fechada ou quando o Briks fecha.
use crate::identity::{self, Identity, PublicJwk};
use crate::realtime::{self, Realtime, RealtimeCfg, RtEvent};
use crate::signaling::{secure_room_id, topic_name};
use serde_json::{json, Value};
use tokio::sync::mpsc;

/// Canais diretos abertos ao mesmo tempo (o mais antigo sai quando chega mais um).
pub const MAX_LINKS: usize = 6;

pub fn valid_id(s: &str) -> bool { !s.is_empty() && s.len() <= 64 && s.chars().all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_') }
pub fn valid_fp(s: &str) -> bool { !s.is_empty() && s.len() <= 72 && s.chars().all(|c| c.is_ascii_alphanumeric()) }

/// Canal de presença onde alguém avisa que quer falar com `user_id`.
pub fn inbox_topic(user_id: &str) -> String { topic_name("friend", &secure_room_id("friend", &format!("inbox:{user_id}"))) }

/// O toque de quem quer falar: só a impressão digital.
pub fn knock_meta(my_fp: &str) -> Value { json!({ "fp": my_fp, "knock": true }) }

/// Lê o toque de um peer da caixa. Descarta o que não tem o formato certo e o próprio.
pub fn read_knock(meta: &Value, my_fp: &str) -> Option<String> {
    let fp = meta["fp"].as_str()?;
    (meta["knock"].as_bool() == Some(true) && valid_fp(fp) && fp != my_fp).then(|| fp.to_string())
}

/// O aviso "sua lista de amigos/pedidos mudou": sem nenhum dado (nem impressão digital); quem recebe só refaz a consulta ao servidor.
pub fn poke_meta() -> Value { json!({ "poke": true }) }
pub fn is_poke(meta: &Value) -> bool { meta["poke"].as_bool() == Some(true) }

/// Sinais assinados na caixa de chegada (1.6): "call" (estou ligando; dados {room, name}), "ginv" (convidei você para um grupo; {gid, name}).
/// Vão como metadado de presença, junto com a chave pública e a assinatura de quem enviou: o receptor confere que a impressão digital é da chave,
/// que a assinatura vale e que o sinal foi feito PARA a conta dele e há pouco tempo. Quem não é amigo ainda é descartado pela interface.
pub const SIG_KINDS: [&str; 5] = ["call", "ginv", "nudge", "hi", "bye"];   // hi = "estou online" (dados {ack}); bye = "saí" (online/offline entre amigos, sem servidor guardando nada)   // nudge (chamar atenção; dados {name}): só vale de amigo, a interface confere
pub const SIG_MAX_AGE_MS: u64 = 15 * 60 * 1000;
pub fn signal_canonical(kind: &str, data: &str, ts: u64, to_user: &str) -> String { format!("briks-sig1|{kind}|{data}|{ts}|{to_user}") }
pub fn signal_meta(id: &Identity, kind: &str, data: &Value, ts: u64, to_user: &str) -> Value {
    let d = data.to_string();
    let s = id.sign(&signal_canonical(kind, &d, ts, to_user));
    json!({ "fp": id.fingerprint(), "sig": { "k": kind, "d": d, "ts": ts }, "jwk": id.public_jwk(), "s": s })
}
/// Lê e confere um sinal: (impressão digital de quem enviou, tipo, dados). None se qualquer verificação falha.
pub fn read_signal(meta: &Value, my_fp: &str, my_user: &str, now: u64) -> Option<(String, String, Value)> {
    let fp = meta["fp"].as_str()?;
    let (kind, d, ts) = (meta["sig"]["k"].as_str()?, meta["sig"]["d"].as_str()?, meta["sig"]["ts"].as_u64()?);
    if !valid_fp(fp) || fp == my_fp || !SIG_KINDS.contains(&kind) || d.len() > 1024 || now.abs_diff(ts) > SIG_MAX_AGE_MS { return None; }
    let jwk: PublicJwk = serde_json::from_value(meta["jwk"].clone()).ok()?;
    if identity::fingerprint(&jwk) != fp || !identity::verify(&jwk, &signal_canonical(kind, d, ts, my_user), meta["s"].as_str()?) { return None; }
    let data: Value = serde_json::from_str(d).ok().filter(|v: &Value| v.is_object())?;
    Some((fp.to_string(), kind.to_string(), data))
}
/// Envia um sinal: entra na caixa de `to_user` com o sinal na presença e fica `hold` (o receptor que abrir o Briks nesse tempo ainda vê).
/// Devolve a conexão: fechá-la antes retira o sinal (o receptor recebe `dm.signal.gone`).
pub fn send_signal(cfg: RealtimeCfg, to_user: &str, meta: Value, hold: std::time::Duration) -> Realtime {
    let (rt, mut rx) = realtime::connect(cfg, inbox_topic(to_user), uuid::Uuid::new_v4().to_string(), meta);
    let rt2 = rt.clone();
    tokio::spawn(async move {
        let drain = async { while rx.recv().await.is_some() {} };
        let _ = tokio::time::timeout(hold, drain).await;
        rt2.close().await;
    });
    rt
}

/// Observa a minha caixa e emite `{"ev":"dm.wake","fp":...}` para cada toque e `{"ev":"friends.poke"}` para cada aviso de lista.
pub fn watch_inbox(cfg: RealtimeCfg, my_user_id: &str, my_fp: String, tx: mpsc::Sender<Value>) -> Realtime {
    let (rt, mut rx) = realtime::connect(cfg, inbox_topic(my_user_id), uuid::Uuid::new_v4().to_string(), json!({ "inbox": true }));
    let my_user = my_user_id.to_string();
    tokio::spawn(async move {
        let mut last_poke: Option<std::time::Instant> = None;
        let mut live: std::collections::HashMap<String, (String, String, Value)> = Default::default();   // peer_id -> sinal que ele mostrava (para avisar quando some)
        while let Some(e) = rx.recv().await {
            if let RtEvent::PeerLeft(pid) = &e {
                if let Some((fp, kind, data)) = live.remove(pid) { if tx.send(json!({ "ev": "dm.signal.gone", "fp": fp, "kind": kind, "data": data })).await.is_err() { return; } }
                continue;
            }
            if let RtEvent::PeerJoined { peer_id, meta } = &e {
                let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0);
                if let Some((fp, kind, data)) = read_signal(meta, &my_fp, &my_user, now) {
                    crate::netlog::write(&format!("sinal {kind} recebido de {}", &fp[..fp.len().min(8)]));
                    if live.len() < 64 && live.insert(peer_id.clone(), (fp.clone(), kind.clone(), data.clone())).is_none() {
                        if tx.send(json!({ "ev": "dm.signal", "fp": fp, "kind": kind, "data": data })).await.is_err() { return; }
                    }
                    continue;
                }
                if meta["sig"].is_object() { crate::netlog::write("sinal descartado: assinatura, destinatario ou idade invalidos"); continue; }
            }
            if let RtEvent::PeerJoined { meta, .. } = e {
                if is_poke(&meta) {
                    // no máx. 1 aviso a cada 1,5 s (quem não é amigo só consegue fazer a UI refazer uma consulta ao servidor)
                    if last_poke.map_or(true, |t| t.elapsed() >= std::time::Duration::from_millis(1500)) {
                        last_poke = Some(std::time::Instant::now());
                        if tx.send(json!({ "ev": "friends.poke" })).await.is_err() { return; }
                    }
                    continue;
                }
                if let Some(fp) = read_knock(&meta, &my_fp) { if tx.send(json!({ "ev": "dm.wake", "fp": fp })).await.is_err() { return; } }
            }
        }
    });
    rt
}

/// Avisa `user_id` que a lista dele mudou: entra na caixa dele por alguns segundos e sai (nada é guardado no servidor).
pub fn poke(cfg: RealtimeCfg, user_id: &str) {
    let (rt, mut rx) = realtime::connect(cfg, inbox_topic(user_id), uuid::Uuid::new_v4().to_string(), poke_meta());
    tokio::spawn(async move {
        let drain = async { while rx.recv().await.is_some() {} };
        let _ = tokio::time::timeout(std::time::Duration::from_secs(5), drain).await;
        rt.close().await;
    });
}

/// Toca na caixa de `friend_user_id`. Devolve a conexão: quem chama a fecha quando o canal direto abrir.
pub fn knock(cfg: RealtimeCfg, friend_user_id: &str, my_fp: &str) -> Realtime {
    let (rt, mut rx) = realtime::connect(cfg, inbox_topic(friend_user_id), uuid::Uuid::new_v4().to_string(), knock_meta(my_fp));
    tokio::spawn(async move { while rx.recv().await.is_some() {} });   // só precisa ficar visível
    rt
}

#[cfg(test)]
mod t {
    use super::*;

    #[test]
    fn inbox_names_differ_per_account_and_never_contain_the_id() {
        let (a, b) = (inbox_topic("user-a"), inbox_topic("user-b"));
        assert_ne!(a, b);
        assert!(a.starts_with("realtime:briks:friend:") && !a.contains("user-a"));
        assert_ne!(a, topic_name("friend", &secure_room_id("friend", "user-a")), "não colide com o nome simples");
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn a_poke_reaches_the_inbox_as_friends_poke_with_no_data_and_is_rate_limited() {
        let port = crate::mock::start(0).await.unwrap();
        let cfg = || RealtimeCfg::new(&format!("ws://127.0.0.1:{port}"), "chave-de-teste");
        let (tx, mut rx) = mpsc::channel::<Value>(16);
        let _inbox = watch_inbox(cfg(), "user-b", "ab12".repeat(16), tx);
        tokio::time::sleep(std::time::Duration::from_millis(600)).await;
        poke(cfg(), "user-b");
        let ev = tokio::time::timeout(std::time::Duration::from_secs(10), rx.recv()).await.expect("o aviso não chegou").unwrap();
        assert_eq!(ev, json!({ "ev": "friends.poke" }), "o aviso não carrega nenhum dado");
        assert!(is_poke(&poke_meta()) && !is_poke(&knock_meta("ab12")) && !is_poke(&json!({ "inbox": true })));
        // um aviso colado no outro (menos de 1,5 s) não vira um segundo evento
        poke(cfg(), "user-b");
        assert!(tokio::time::timeout(std::time::Duration::from_millis(900), rx.recv()).await.is_err(), "segundo aviso em seguida deve ser descartado");
    }

    #[test]
    fn a_knock_carries_only_a_valid_fingerprint_and_never_myself() {
        let fp = "ab12".repeat(16);
        assert_eq!(read_knock(&knock_meta(&fp), "outro"), Some(fp.clone()));
        assert_eq!(read_knock(&knock_meta(&fp), &fp), None, "o próprio toque não conta");
        assert_eq!(read_knock(&json!({ "fp": fp }), "x"), None, "sem a marca de toque");
        assert_eq!(read_knock(&json!({ "fp": "../etc", "knock": true }), "x"), None);
        assert_eq!(read_knock(&json!({ "fp": "a".repeat(73), "knock": true }), "x"), None);
        assert_eq!(read_knock(&json!({ "inbox": true }), "x"), None);
        assert!(valid_id("abc-123_x") && !valid_id("a b") && !valid_id(""));
    }

    fn now_ms() -> u64 { std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0) }

    #[test]
    fn a_signal_is_checked_for_signature_recipient_age_and_sender() {
        let id = Identity::generate();
        let data = json!({ "room": "dm-abc", "name": "Ana" });
        let meta = signal_meta(&id, "call", &data, now_ms(), "user-b");
        let (fp, kind, d) = read_signal(&meta, "outro-fp", "user-b", now_ms()).expect("sinal válido");
        assert_eq!((fp.as_str(), kind.as_str()), (id.fingerprint().as_str(), "call"));
        assert_eq!(d, data);
        assert!(read_signal(&meta, "outro-fp", "user-c", now_ms()).is_none(), "feito para outra conta");
        assert!(read_signal(&meta, &id.fingerprint(), "user-b", now_ms()).is_none(), "o próprio sinal");
        assert!(read_signal(&meta, "outro-fp", "user-b", now_ms() + SIG_MAX_AGE_MS + 5000).is_none(), "velho demais");
        let mut bad = meta.clone(); bad["sig"]["d"] = json!("{\"room\":\"dm-outra\"}");
        assert!(read_signal(&bad, "outro-fp", "user-b", now_ms()).is_none(), "dados adulterados");
        let mut bad = meta.clone(); bad["fp"] = json!("ab12".repeat(16));
        assert!(read_signal(&bad, "outro-fp", "user-b", now_ms()).is_none(), "fp que não é da chave");
        let mut bad = meta.clone(); bad["sig"]["k"] = json!("ginv");
        assert!(read_signal(&bad, "outro-fp", "user-b", now_ms()).is_none(), "tipo adulterado");
        let other = signal_meta(&Identity::generate(), "call", &data, now_ms(), "user-b");
        let mut bad = meta.clone(); bad["jwk"] = other["jwk"].clone();
        assert!(read_signal(&bad, "outro-fp", "user-b", now_ms()).is_none(), "chave de outro");
        assert!(read_signal(&signal_meta(&id, "xyz", &data, now_ms(), "user-b"), "o", "user-b", now_ms()).is_none(), "tipo desconhecido");
    }

    #[test]
    fn a_nudge_signal_is_valid_only_signed_for_me_and_fresh() {
        let id = Identity::generate();
        let data = json!({ "name": "Ana" });
        let meta = signal_meta(&id, "nudge", &data, now_ms(), "user-b");
        let (fp, kind, d) = read_signal(&meta, "outro-fp", "user-b", now_ms()).expect("nudge válido");
        assert_eq!((fp.as_str(), kind.as_str(), &d), (id.fingerprint().as_str(), "nudge", &data));
        assert!(read_signal(&meta, "outro-fp", "user-c", now_ms()).is_none(), "feito para outra conta");
        assert!(read_signal(&meta, &id.fingerprint(), "user-b", now_ms()).is_none(), "o próprio sinal");
        assert!(read_signal(&meta, "outro-fp", "user-b", now_ms() + SIG_MAX_AGE_MS + 5000).is_none(), "velho demais");
        assert!(read_signal(&meta, "outro-fp", "user-b", now_ms().saturating_sub(SIG_MAX_AGE_MS + 5000)).is_none(), "do futuro demais");
        let mut bad = meta.clone(); bad["sig"]["d"] = json!("{\"name\":\"Outro\"}");
        assert!(read_signal(&bad, "outro-fp", "user-b", now_ms()).is_none(), "dados adulterados");
        let mut bad = meta.clone(); bad["sig"]["k"] = json!("call");
        assert!(read_signal(&bad, "outro-fp", "user-b", now_ms()).is_none(), "tipo trocado invalida a assinatura");
        let big = signal_meta(&id, "nudge", &json!({ "name": "x".repeat(2000) }), now_ms(), "user-b");
        assert!(read_signal(&big, "outro-fp", "user-b", now_ms()).is_none(), "dados grandes demais");
        assert!(read_signal(&signal_meta(&id, "shake", &data, now_ms(), "user-b"), "o", "user-b", now_ms()).is_none(), "tipo desconhecido");
        let not_obj = signal_meta(&id, "nudge", &json!("texto"), now_ms(), "user-b");
        assert!(read_signal(&not_obj, "outro-fp", "user-b", now_ms()).is_none(), "dados que não são objeto");
    }

    #[test]
    fn online_hi_and_bye_signals_are_signed_for_the_friend_only() {
        let id = Identity::generate();
        for kind in ["hi", "bye"] {
            let meta = signal_meta(&id, kind, &json!({ "ack": true }), now_ms(), "user-b");
            let (fp, k, d) = read_signal(&meta, "outro-fp", "user-b", now_ms()).expect("aviso de presença válido");
            assert_eq!((fp.as_str(), k.as_str(), d["ack"].as_bool()), (id.fingerprint().as_str(), kind, Some(true)));
            assert!(read_signal(&meta, "outro-fp", "user-c", now_ms()).is_none(), "feito para outra conta");
        }
        let mut forged = signal_meta(&id, "hi", &json!({ "ack": false }), now_ms(), "user-b");
        forged["sig"]["k"] = json!("bye");
        assert!(read_signal(&forged, "outro-fp", "user-b", now_ms()).is_none(), "trocar hi por bye invalida a assinatura");
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn a_signal_reaches_the_inbox_and_goes_away_when_withdrawn_and_forged_ones_are_dropped() {
        let port = crate::mock::start(0).await.unwrap();
        let cfg = || RealtimeCfg::new(&format!("ws://127.0.0.1:{port}"), "chave-de-teste");
        let (tx, mut rx) = mpsc::channel::<Value>(16);
        let mut icfg = cfg(); icfg.leave_grace = std::time::Duration::from_millis(200);
        let _inbox = watch_inbox(icfg, "user-b", "ab12".repeat(16), tx);
        tokio::time::sleep(std::time::Duration::from_millis(600)).await;
        let id = Identity::generate();
        // para outra conta: o receptor descarta
        let wrong = send_signal(cfg(), "user-b", signal_meta(&id, "call", &json!({ "room": "x" }), now_ms(), "user-z"), std::time::Duration::from_secs(30));
        assert!(tokio::time::timeout(std::time::Duration::from_millis(1200), rx.recv()).await.is_err(), "sinal para outro destinatário deve ser descartado");
        wrong.close().await;
        // com a hora velha: descartado
        let old = send_signal(cfg(), "user-b", signal_meta(&id, "call", &json!({ "room": "x" }), now_ms() - SIG_MAX_AGE_MS - 60_000, "user-b"), std::time::Duration::from_secs(30));
        assert!(tokio::time::timeout(std::time::Duration::from_millis(1200), rx.recv()).await.is_err(), "sinal velho deve ser descartado");
        old.close().await;
        // válido: chega; fechar retira
        let data = json!({ "room": "dm-1", "name": "Ana" });
        let h = send_signal(cfg(), "user-b", signal_meta(&id, "call", &data, now_ms(), "user-b"), std::time::Duration::from_secs(30));
        let ev = tokio::time::timeout(std::time::Duration::from_secs(10), rx.recv()).await.expect("o sinal não chegou").unwrap();
        assert_eq!(ev["ev"], "dm.signal");
        assert_eq!((ev["fp"].as_str().unwrap(), ev["kind"].as_str().unwrap()), (id.fingerprint().as_str(), "call"));
        assert_eq!(ev["data"], data);
        h.close().await;
        let gone = tokio::time::timeout(std::time::Duration::from_secs(10), rx.recv()).await.expect("dm.signal.gone não veio").unwrap();
        assert_eq!(gone["ev"], "dm.signal.gone");
        assert_eq!(gone["data"], data);
    }
}
