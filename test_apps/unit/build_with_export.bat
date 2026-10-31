@echo off
set IDF_PATH=D:\Program\esp\.espressif\v5.5.4\esp-idf
set IDF_TOOLS_PATH=C:\Espressif\tools
call D:\Program\esp\.espressif\v5.5.4\esp-idf\export.bat
cd /d D:\UserData\ESP32Project\IDF\ESP32-IDF-Test\test_apps\unit
python "%IDF_PATH%\tools\idf.py" build
echo EXIT_CODE=%ERRORLEVEL%
