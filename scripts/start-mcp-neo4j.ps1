# Reads .env from repo root, maps AURA_NEO4J_* -> CLI flags, starts mcp-server-neo4j
$repoRoot = Split-Path $PSScriptRoot -Parent
$envFile  = Join-Path $repoRoot ".env"
if (Test-Path $envFile) {
    Get-Content $envFile | ForEach-Object {
        if ($_ -match '^\s*([^#][^=]+)=(.*)$') {
            [System.Environment]::SetEnvironmentVariable($matches[1].Trim(), $matches[2].Trim())
        }
    }
}

function Coalesce { foreach ($v in $args) { if ($v) { return $v } } }

$uri      = Coalesce $env:AURA_NEO4J_URI      $env:NEO4J_URI
$user     = Coalesce $env:AURA_NEO4J_USER     $env:NEO4J_USERNAME $env:NEO4J_USER
$password = Coalesce $env:AURA_NEO4J_PASSWORD $env:NEO4J_PASSWORD
$database = Coalesce $env:AURA_NEO4J_DATABASE $env:NEO4J_DATABASE "neo4j"

if (-not $uri -or -not $password) {
    Write-Error "Missing NEO4J credentials in .env"
    exit 1
}

uvx mcp-server-neo4j --uri $uri --username $user --password $password --database $database
