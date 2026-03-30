$winPath = $PSScriptRoot

$wslPath = wsl -d Ubuntu wslpath -u "$winPath"

Write-Host "--- Starting building and compiling mljOS via Ubuntu ---" -ForegroundColor Cyan

# 3. Запускаем команду: зайти в папку -> дать права -> выполнить
wsl -d Ubuntu -e bash -c "cd '$wslPath' && chmod +x build.sh && ./build.sh"

# 4. Проверяем код выхода (был ли успех в Linux)
if ($LASTEXITCODE -eq 0) {
    Write-Host "--- Building completed successfully! ---" -ForegroundColor Green
} else {
    Write-Host "--- Error occurred during building (Code: $LASTEXITCODE) ---" -ForegroundColor Red
}