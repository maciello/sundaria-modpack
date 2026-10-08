# Steam launch option (Windows):
#   powershell -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\path\to\update.ps1 %command%
# Mirrors update.py. Any failure -> game starts anyway.
$Api = if ($env:MODPACK_API) { $env:MODPACK_API } else { "https://api.github.com/repos/maciello/sundaria-modpack/releases/latest" }
$exe = $args | Where-Object { $_ -like "*Archon-Win64-Shipping.exe" } | Select-Object -First 1
if ($exe) {
  try {
    $root = Split-Path (Split-Path (Split-Path (Split-Path $exe)))
    $rel = Invoke-RestMethod -Uri $Api -TimeoutSec 10
    $ver = Join-Path $root ".modpack-version"
    if (-not ((Test-Path $ver) -and ((Get-Content $ver -Raw) -eq $rel.tag_name))) {
      $url = ($rel.assets | Where-Object { $_.name -like "*.zip" } | Select-Object -First 1).browser_download_url
      $tmp = Join-Path ([IO.Path]::GetTempPath()) "modpack.zip"
      Invoke-WebRequest -Uri $url -OutFile $tmp -TimeoutSec 60
      Add-Type -AssemblyName System.IO.Compression.FileSystem
      $zip = [IO.Compression.ZipFile]::OpenRead($tmp)
      $names = $zip.Entries | Where-Object { $_.Name } | ForEach-Object { $_.FullName }
      $zip.Dispose()
      $full = [IO.Path]::GetFullPath($root)
      foreach ($n in $names) {  # trust boundary: refuse paths escaping the game root
        if (-not [IO.Path]::GetFullPath((Join-Path $root $n)).StartsWith($full)) { throw "bad path in zip: $n" }
      }
      $list = Join-Path $root ".modpack-files"
      if (Test-Path $list) { Get-Content $list | ForEach-Object { Remove-Item (Join-Path $root $_) -ErrorAction SilentlyContinue } }
      Expand-Archive -Path $tmp -DestinationPath $root -Force
      Set-Content $list $names
      Set-Content $ver $rel.tag_name -NoNewline
    }
  } catch { Write-Host "[modpack] update skipped: $_" }
}
if ($args.Count -gt 0) { & $args[0] @($args | Select-Object -Skip 1) }
