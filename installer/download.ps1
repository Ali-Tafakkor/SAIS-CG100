param(
    [Parameter(Mandatory=$true)][string]$Url,
    [Parameter(Mandatory=$true)][string]$Output,
    [Parameter(Mandatory=$true)][long]$MaximumBytes
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
if (-not $Url.StartsWith('https://')) { throw 'Only HTTPS downloads are permitted.' }
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$response = Invoke-WebRequest -Uri $Url -OutFile $Output -UseBasicParsing -PassThru -TimeoutSec 45 -MaximumRedirection 5
if ($response.BaseResponse.ResponseUri.Scheme -ne 'https') {
    throw 'A release download redirected away from HTTPS.'
}
if ((Get-Item -LiteralPath $Output).Length -gt $MaximumBytes) {
    throw 'Release asset exceeds its size limit.'
}
