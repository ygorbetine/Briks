# Release do app nativo (CUI). POR PADRAO NAO PUBLICA: gera o instalador, confere o latest.yml e imprime os comandos.
# Uso: powershell -NoProfile -File scripts/release-cui.ps1 -Version 1.4.0            (ensaio: so monta e confere)
#      $env:BRIKS_RELEASE_OK = "1"; powershell -NoProfile -File scripts/release-cui.ps1 -Version 1.4.0 -Publish   (tag + push + release)
# -Publish exige BRIKS_RELEASE_OK=1 (ok explicito do usuario), arvore limpa, branch atual e gh autenticado.
# Reverter: gh release delete v<versao> --cleanup-tag --yes   (o latest/download/latest.yml volta ao release anterior)
param([Parameter(Mandatory)][string]$Version, [switch]$Publish, [string]$Branch = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$tag = "v$Version"
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "Versao invalida: $Version" }
if (git -C $root status --porcelain) { throw "Arvore suja: faca commit antes." }
if (-not $Branch) { $Branch = (git -C $root rev-parse --abbrev-ref HEAD).Trim() }
if (git -C $root tag -l $tag) { throw "A tag $tag ja existe." }

& "$PSScriptRoot\build-installer.ps1" -Version $Version; if ($LASTEXITCODE) { throw "build-installer falhou" }
$out = Join-Path $root "apps\desktop-cui\installer-out"
$exe = Join-Path $out "Briks-Setup-$Version.exe"; $yml = Join-Path $out "latest.yml"
$sha = [Convert]::ToBase64String([Security.Cryptography.SHA512]::Create().ComputeHash([IO.File]::ReadAllBytes($exe)))
if ((Get-Content $yml -Raw) -notmatch [regex]::Escape($sha)) { throw "sha512 do latest.yml nao confere com o instalador" }
if ((Get-Content $yml -Raw) -notmatch "version: $([regex]::Escape($Version))") { throw "versao do latest.yml difere" }
$hex = (Get-FileHash $exe -Algorithm SHA256).Hash
Write-Host "Instalador: $exe`nSHA-256: $hex`nSHA-512 (b64): $sha`nlatest.yml: $yml"

# notas da release: o aviso do certificado vai sempre no topo (o Windows pode apontar o instalador como falso positivo)
$notesFile = Join-Path $env:TEMP "briks-release-notes-$Version.md"
$notesText = "Briks $Version (app nativo)`n`n**Aviso:** o Briks ainda nao tem certificado de assinatura de codigo. Por isso o Windows (SmartScreen ou Microsoft Defender) pode apontar o instalador como **falso positivo**. Para o SmartScreen, clique em *Mais informacoes > Executar assim mesmo*. Se o Defender bloquear o arquivo, restaure-o em *Seguranca do Windows > Protecao contra virus e ameacas > Historico de protecao*. Baixe somente desta pagina. O codigo e aberto (AGPL-3.0). Detalhes no [README](https://github.com/ygorbetine/Briks#aviso-o-windows-pode-apontar-o-briks-como-falso-positivo)."
[IO.File]::WriteAllText($notesFile, $notesText, (New-Object Text.UTF8Encoding($false)))
$cmds = @("git push origin $Branch", "git tag -a $tag -m `"Briks $Version`"", "git push origin $tag",
          "gh release create $tag `"$exe`" `"$yml`" --title `"Briks $Version`" --notes-file `"$notesFile`"")
if (-not $Publish) { Write-Host "`nENSAIO: nada foi publicado. Comandos de publicacao:"; $cmds | ForEach-Object { Write-Host "  $_" }; return }
if ($env:BRIKS_RELEASE_OK -ne "1") { throw "Defina BRIKS_RELEASE_OK=1 (ok explicito do usuario) para publicar." }
git -C $root push origin $Branch; if ($LASTEXITCODE) { throw "push falhou" }
git -C $root tag -a $tag -m "Briks $Version"; git -C $root push origin $tag; if ($LASTEXITCODE) { throw "push da tag falhou" }
gh release create $tag $exe $yml --title "Briks $Version" --notes-file $notesFile --repo ygorbetine/Briks; if ($LASTEXITCODE) { throw "gh release falhou" }
Write-Host "Publicado: https://github.com/ygorbetine/Briks/releases/tag/$tag"
