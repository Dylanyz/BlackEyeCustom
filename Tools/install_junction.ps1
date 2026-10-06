<#
    install_junction.ps1 - link the BlackEyeCustom plugin folder into the engine as an engine plugin.

    Shared junction script for the Mad Rice UE plugins (one copy per repo). Per-repo values:
    BlackEyeCustom and  ("" when the repo root is the plugin, "Plugin\<Name>" otherwise).

    Creates:  <Engine>\Engine\Plugins\Marketplace\BlackEyeCustom  ->  <plugin folder>
    so the plugin is available to every project on that engine with no per-project copy.

    Safe to re-run: if the junction already points here it just reports status. It refuses to
    touch anything else at that path (a real folder, or a junction to somewhere else), and it
    never deletes anything. Creating a junction under Program Files may need an elevated shell.

    Usage:
      Tools\install_junction.ps1            # create if missing, then print status
      Tools\install_junction.ps1 -Status    # print status only
      Tools\install_junction.ps1 -Engine "C:\Program Files\Epic Games\UE_5.8"

    Do not create the junction while a project ALSO has a copy in its Plugins\ folder: a plugin
    found at both an engine path and a project path fails to load.
#>
[CmdletBinding()]
param(
    [string] $Repo,
    [string] $Engine,
    [switch] $Status
)

$ErrorActionPreference = "Stop"

$PluginName   = "BlackEyeCustom"
$PluginSubdir = ""
if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
$Target = (Resolve-Path $(if ($PluginSubdir) { Join-Path $Repo $PluginSubdir } else { $Repo })).Path

# Engine: the version the .uplugin targets, else the newest UE_* install.
if (-not $Engine) {
    $want = $null
    $upJson = Get-Content (Join-Path $Target "$PluginName.uplugin") -Raw | ConvertFrom-Json
    if ($upJson.EngineVersion -match '^(\d+)\.(\d+)') { $want = "UE_$($Matches[1]).$($Matches[2])" }
    $cands = @()
    foreach ($r in @("C:\Program Files\Epic Games", "D:\Program Files\Epic Games")) {
        if (Test-Path $r) { $cands += Get-ChildItem $r -Directory -Filter "UE_*" -ErrorAction SilentlyContinue }
    }
    $cands = $cands | Sort-Object Name -Descending
    $pick = $cands | Where-Object { $_.Name -eq $want } | Select-Object -First 1
    if (-not $pick) { $pick = $cands | Select-Object -First 1 }
    if (-not $pick) { throw "No Unreal Engine install found. Pass -Engine explicitly." }
    $Engine = $pick.FullName
}

$Marketplace = Join-Path $Engine "Engine\Plugins\Marketplace"
$Link = Join-Path $Marketplace $PluginName

function Get-LinkInfo([string] $Path) {
    # Returns $null if nothing is there, else @{ IsJunction; Target }.
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    if (-not $item) { return $null }
    $isLink = [bool]($item.Attributes -band [IO.FileAttributes]::ReparsePoint)
    $tgt = $null
    if ($isLink) { $tgt = @($item.Target)[0] }
    return @{ IsJunction = ($isLink -and $item.LinkType -eq "Junction"); IsLink = $isLink; LinkType = $item.LinkType; Target = $tgt }
}

function Test-SamePath([string] $A, [string] $B) {
    if (-not $A -or -not $B) { return $false }
    $na = [IO.Path]::GetFullPath($A).TrimEnd('\')
    $nb = [IO.Path]::GetFullPath($B).TrimEnd('\')
    return [string]::Equals($na, $nb, [StringComparison]::OrdinalIgnoreCase)
}

function Show-Status {
    $info = Get-LinkInfo $Link
    "$PluginName junction"
    "  engine   $Engine"
    "  link     $Link"
    "  target   $Target"
    if (-not $info) {
        "  state    MISSING"
    } elseif ($info.IsJunction -and (Test-SamePath $info.Target $Target)) {
        "  state    OK (junction -> this repo)"
    } elseif ($info.IsLink) {
        "  state    CONFLICT: a $($info.LinkType) to $($info.Target)"
    } else {
        "  state    CONFLICT: a real folder, not a junction"
    }
    $uplugin = Join-Path $Link "$PluginName.uplugin"
    "  uplugin  {0}" -f $(if (Test-Path $uplugin) { "visible through the link" } else { "not visible" })
}

if ($Status) { Show-Status; return }

if (-not (Test-Path $Marketplace)) { throw "Engine Marketplace folder not found: $Marketplace" }

$existing = Get-LinkInfo $Link
if ($existing) {
    if ($existing.IsJunction -and (Test-SamePath $existing.Target $Target)) {
        Write-Host "Junction already in place." -ForegroundColor Green
        Show-Status
        return
    }
    Show-Status
    throw "Refusing to continue: $Link exists and is not a junction to $Target. Inspect it and remove it by hand if it is safe to."
}

New-Item -ItemType Junction -Path $Link -Target $Target | Out-Null
Write-Host "Created junction $Link -> $Target" -ForegroundColor Green
Show-Status
