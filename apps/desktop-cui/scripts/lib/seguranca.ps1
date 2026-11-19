# Proteções comuns dos scripts de teste do Briks (dot-source: . "$PSScriptRoot\lib\seguranca.ps1").
# Motivo: o PC do dono deu tela azul (BSOD) durante testes. Ver docs/plans/bsod-analise.md.
#
#  - Testes PERIGOSOS (GPU/encoder de hardware, janela topmost em tela cheia, tremer a tela, aviso real do Windows) só rodam com
#    BRIKS_TESTES_PERIGOSOS=1 (ou o parâmetro -Perigosos do script) e, mesmo assim, com o lock exclusivo da GPU.
#  - Todo processo de teste roda com DISJUNTOR: passou do prazo, a árvore de processos é morta (taskkill /T /F).

function Test-Perigosos { return ($env:BRIKS_TESTES_PERIGOSOS -eq "1") }

# Variáveis de ambiente que fazem o app abrir algo REAL na tela/GPU/sistema do dono, mesmo com BRIKS_HIDDEN=1.
$script:EnvPerigosas = @("BRIKS_TOAST_REAL", "BRIKS_NUDGE_REAL", "BRIKS_SYS_OPEN_REAL", "BRIKS_OVERLAY_FORCE", "BRIKS_TEST_FULLSCREEN", "BRIKS_MF_HW", "BRIKS_SFU_TEST_GPU", "BRIKS_SHARE_REAL")
# Comandos de roteiro que mexem em janela real: "win game" (janela topmost de tela cheia), "win fg/max/min", "sys ovl". (Tremer a tela e toast real são pelas variáveis BRIKS_NUDGE_REAL/BRIKS_TOAST_REAL.)
$script:RoteiroPerigoso = '^\s*(sys\s+)?win\s+(game|gameoff|fg|max|min)\b|^\s*sys\s+ovl\b'

# Devolve o motivo (texto) se o caso do harness é perigoso; $null se for seguro. $env = hashtable/objeto de env do caso, $roteiro = caminho.
function Get-MotivoPerigo($envCaso, [string]$roteiro) {
    $m = @()
    if ($envCaso) { foreach ($p in $envCaso.PSObject.Properties) { if ($script:EnvPerigosas -contains $p.Name) { $m += "env $($p.Name)" } } }
    if ($roteiro -and (Test-Path -LiteralPath $roteiro)) {
        $hit = Select-String -LiteralPath $roteiro -Pattern $script:RoteiroPerigoso -List -ErrorAction SilentlyContinue
        if ($hit) { $m += "roteiro usa '" + $hit.Line.Trim() + "'" }
    }
    if ($m.Count) { return ($m -join "; ") }
    return $null
}

# Lock exclusivo da GPU (mesmo arquivo que as sessões já usam). Devolve $true se pegou. Lock com mais de 2 h é considerado abandonado.
$script:LockGpuCaminho = "A:\BRiskCall\.gpu-test.lock"
function Enter-LockGpu {
    try {
        if (Test-Path $script:LockGpuCaminho) {
            if (((Get-Date) - (Get-Item $script:LockGpuCaminho).LastWriteTime).TotalHours -lt 2) { return $false }
            Remove-Item $script:LockGpuCaminho -Force -ErrorAction SilentlyContinue
        }
        $fs = [IO.File]::Open($script:LockGpuCaminho, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        $b = [Text.Encoding]::UTF8.GetBytes("pid=$PID $(Get-Date -Format s) $($MyInvocation.ScriptName)"); $fs.Write($b, 0, $b.Length); $fs.Close()
        return $true
    } catch { return $false }
}
function Exit-LockGpu { Remove-Item $script:LockGpuCaminho -Force -ErrorAction SilentlyContinue }

# Mata o processo e TODOS os filhos (o app de teste e o briks-core). Não falha se já saiu.
function Stop-Arvore([int]$processId) {
    try { & taskkill.exe /PID $processId /T /F 2>&1 | Out-Null } catch {}
}

# DISJUNTOR: espera o processo até $prazoSeg; se passar, mata a árvore e devolve $false. $true = terminou a tempo.
function Wait-ComDisjuntor([Diagnostics.Process]$proc, [int]$prazoSeg, [string]$nome = "") {
    if ($proc.WaitForExit($prazoSeg * 1000)) { return $true }
    Write-Host ("DISJUNTOR: '{0}' passou de {1} s; encerrando o processo e os filhos." -f $nome, $prazoSeg) -ForegroundColor Yellow
    Stop-Arvore $proc.Id
    $proc.WaitForExit(5000) | Out-Null
    return $false
}

# Se o dono liberou os perigosos, exige o lock da GPU; devolve $true se pode seguir com perigosos. Escreve a mensagem sempre.
function Resolve-Perigosos([bool]$pediu) {
    if ($pediu) { $env:BRIKS_TESTES_PERIGOSOS = "1" }
    if (-not (Test-Perigosos)) {
        Write-Host "Testes perigosos DESLIGADOS (padrão): os que usam GPU/encoder de hardware, janela topmost em tela cheia, tremer a tela ou aviso real do Windows serão PULADOS." -ForegroundColor Cyan
        Write-Host "  Para rodá-los de propósito: BRIKS_TESTES_PERIGOSOS=1 (ou -Perigosos), de preferência SÓ com você na frente do PC e nada importante aberto." -ForegroundColor Cyan
        return $false
    }
    if (-not (Enter-LockGpu)) {
        Write-Host "BRIKS_TESTES_PERIGOSOS=1, mas o lock da GPU ($script:LockGpuCaminho) está com outra sessão. Perigosos continuam PULADOS." -ForegroundColor Yellow
        $env:BRIKS_TESTES_PERIGOSOS = "0"
        return $false
    }
    Write-Host "ATENÇÃO: testes perigosos LIGADOS (lock da GPU pego). Isto já derrubou o PC com tela azul." -ForegroundColor Red
    return $true
}
