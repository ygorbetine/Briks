<#
  Roda os testes do briks-core (cargo test) de forma SEGURA para o PC: sem GPU por padrão, com limite de tempo e disjuntor.
  Uso:  powershell -NoProfile -File apps\desktop-cui\scripts\testes-core.ps1 [-Filtro nome] [-PrazoSeg 900] [-Perigosos] [-Release]

  Padrão (seguro): BRIKS_ENCODER=mf-soft BRIKS_DECODER=openh264 BRIKS_CONVERT=cpu BRIKS_PROBE_NOHW=1, no máximo 2 testes ao mesmo tempo, testes
  #[ignore] NÃO rodam, e a árvore de processos é morta se passar do prazo. Os testes de GPU (gpuconv) e o codificador de hardware só com
  -Perigosos / BRIKS_TESTES_PERIGOSOS=1 (e o lock da GPU). Ver docs/plans/bsod-analise.md.
#>
param([string]$Filtro = "", [int]$PrazoSeg = 900, [switch]$Perigosos, [switch]$Release, [int]$Threads = 2)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "lib\seguranca.ps1")
$core = Join-Path (Split-Path -Parent $PSScriptRoot) "briks-core"
$ok = Resolve-Perigosos ([bool]$Perigosos)
if (-not $ok) {
    $env:BRIKS_ENCODER = "mf-soft"; $env:BRIKS_DECODER = "openh264"; $env:BRIKS_CONVERT = "cpu"; $env:BRIKS_PROBE_NOHW = "1"
    Remove-Item Env:BRIKS_MF_HW, Env:BRIKS_SFU_TEST_GPU -ErrorAction SilentlyContinue
}
if (-not $env:CMAKE_POLICY_VERSION_MINIMUM) { $env:CMAKE_POLICY_VERSION_MINIMUM = "3.5" }
$args = @("test", "--offline"); if ($Release) { $args += "--release" }
if ($Filtro) { $args += $Filtro }
$args += @("--", "--test-threads=$Threads")
Write-Host ("cargo " + ($args -join " ") + "  (prazo $PrazoSeg s)")
$psi = New-Object Diagnostics.ProcessStartInfo
$psi.FileName = "cargo"; $psi.Arguments = ($args -join " "); $psi.WorkingDirectory = $core; $psi.UseShellExecute = $false
$p = [Diagnostics.Process]::Start($psi)
$terminou = Wait-ComDisjuntor $p $PrazoSeg "cargo test"
if ($ok) { Exit-LockGpu }
if (-not $terminou) { Write-Host "Interrompido pelo disjuntor." -ForegroundColor Red; exit 2 }
exit $p.ExitCode
