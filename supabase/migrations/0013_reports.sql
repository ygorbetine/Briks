-- Briks 1.5.28: denúncias (conteúdo violento/sexual/ódio/ilegal) com quadro da transmissão como prova. NÃO aplicada: revisar e rodar no SQL Editor (idempotente).
--
--   * `reports` guarda quem denunciou, quem foi denunciado (conta, se o fingerprint dele é conhecido, e o próprio fingerprint), o motivo, a sala, a hora e o caminho
--     da prova. Ninguém lê ou grava a tabela direto (RLS ligada, sem política, sem grant): só a função `report_submit` (security definer) grava, e só
--     o dono do projeto (SQL Editor / service role) lê para moderar.
--   * A prova é UM quadro JPEG (até 512 KB) no bucket PRIVADO `report-evidence`, na pasta do próprio denunciante: `report-evidence/<auth.uid()>/<id>.jpg`.
--     O denunciante só consegue ENVIAR (insert); não lê, não troca e não apaga o que enviou. Nenhum link público.
--   * Limite: 20 denúncias por hora por pessoa (a 21ª recebe `too_many_reports`), motivo só entre os cinco conhecidos, textos curtos.
--   * Nada disso aparece para a pessoa denunciada nem para outros usuários.

create table if not exists public.reports (
  id uuid primary key default gen_random_uuid(),
  reporter uuid not null references auth.users(id) on delete cascade,
  reported_user uuid references auth.users(id) on delete set null,
  reported_fp text,
  reported_name text,
  reason text not null,
  room text,
  evidence_path text,
  status text not null default 'open',
  created_at timestamptz not null default now(),
  constraint reports_reason check (reason in ('violencia', 'sexual', 'odio', 'ilegal', 'outro')),
  constraint reports_status check (status in ('open', 'reviewing', 'closed'))
);
create index if not exists reports_open_idx on public.reports (status, created_at desc);
create index if not exists reports_reporter_idx on public.reports (reporter, created_at desc);
alter table public.reports enable row level security;
revoke all on public.reports from anon, authenticated;

create or replace function public.report_submit(p_fp text, p_name text, p_reason text, p_room text default null, p_evidence text default null)
returns uuid
language plpgsql security definer set search_path = public, pg_temp as $$
declare
  uid uuid := auth.uid();
  fp text := nullif(left(btrim(coalesce(p_fp, '')), 128), '');
  who uuid;
  ev text := nullif(left(btrim(coalesce(p_evidence, '')), 200), '');
  rid uuid := gen_random_uuid();
begin
  if uid is null then raise exception 'not_authenticated'; end if;
  if p_reason is null or p_reason not in ('violencia', 'sexual', 'odio', 'ilegal', 'outro') then raise exception 'invalid_reason'; end if;
  if (select count(*) from public.reports where reporter = uid and created_at > now() - interval '1 hour') >= 20 then raise exception 'too_many_reports'; end if;
  -- a prova só vale se estiver na pasta do próprio denunciante
  if ev is not null and ev not like uid::text || '/%' then ev := null; end if;
  if fp is not null then select user_id into who from public.profiles where fingerprint = fp limit 1; end if;
  if who = uid then raise exception 'invalid_user'; end if;
  insert into public.reports (id, reporter, reported_user, reported_fp, reported_name, reason, room, evidence_path)
  values (rid, uid, who, fp, nullif(left(btrim(coalesce(p_name, '')), 96), ''), p_reason, nullif(left(btrim(coalesce(p_room, '')), 96), ''), ev);
  return rid;
end $$;
revoke all on function public.report_submit(text, text, text, text, text) from public, anon;
grant execute on function public.report_submit(text, text, text, text, text) to authenticated;

-- Storage: bucket privado das provas. Só envio, na pasta do próprio denunciante.
insert into storage.buckets (id, name, public, file_size_limit, allowed_mime_types)
values ('report-evidence', 'report-evidence', false, 524288, array['image/jpeg'])
on conflict (id) do update set public = false, file_size_limit = 524288, allowed_mime_types = array['image/jpeg'];

drop policy if exists report_evidence_insert_own on storage.objects;
create policy report_evidence_insert_own on storage.objects for insert to authenticated
  with check (bucket_id = 'report-evidence' and (storage.foldername(name))[1] = auth.uid()::text);
