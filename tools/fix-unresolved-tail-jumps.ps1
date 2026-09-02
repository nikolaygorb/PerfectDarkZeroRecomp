# Auto-reapplies the "cross-function tail-jump" workaround
# after every `rexglue codegen` run, since codegen regenerates these files
# from scratch and doesn't preserve hand edits. Safe to re-run: a file with
# no remaining broken gotos is left untouched.
#
# What it looks for: a `goto loc_XXXXXXXX;` whose target label isn't declared
# anywhere in the same generated .cpp file (PPC compilers sometimes emit a
# shared tail block split across two functions by rexglue's Discover/Merge
# phases). Each such goto is
# replaced with a logged early return instead, matching the pattern already
# established for sub_82401A40's two branches.
param(
    [string]$GeneratedDir = (Join-Path $PSScriptRoot "..\generated\default")
)

$files = Get-ChildItem -LiteralPath $GeneratedDir -Filter "*.cpp"
$totalPatched = 0

foreach ($file in $files) {
    $lines = [System.IO.File]::ReadAllLines($file.FullName)

    $declared = [System.Collections.Generic.HashSet[string]]::new()
    foreach ($line in $lines) {
        if ($line -match '^(loc_[0-9A-Fa-f]+):') {
            [void]$declared.Add($Matches[1])
        }
    }

    $changed = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        if ($line -match 'goto (loc_[0-9A-Fa-f]+);' -and -not $declared.Contains($Matches[1])) {
            $label = $Matches[1]
            $lines[$i] = $line -replace "goto $label;", `
                "{ REXLOG_WARN(""unresolved tail-jump to $label (shared tail block in a different generated function)""); return; }"
            $changed = $true
            $totalPatched++
            Write-Host "  [fix-tail-jumps] $($file.Name): patched unresolved goto $label"
        }
    }

    if ($changed) {
        [System.IO.File]::WriteAllLines($file.FullName, $lines)
    }
}

Write-Host "[fix-tail-jumps] done, $totalPatched unresolved tail-jump(s) patched"
