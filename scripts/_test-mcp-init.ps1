# Sends initialize + notifications/initialized to mcp-server-neo4j and prints the response
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

$messages = @(
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"0.1"}}}'
    '{"jsonrpc":"2.0","method":"notifications/initialized","params":{}}'
)

$messages | uvx mcp-server-neo4j --uri $uri --username $user --password $password --database $database
