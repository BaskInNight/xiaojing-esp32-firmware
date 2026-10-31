@echo off
set IDF_PATH=D:\Program\esp\.espressif\v5.5.4\esp-idf
set IDF_TOOLS_PATH=C:\Espressif\tools
set IDF_PYTHON_ENV_PATH=D:\Program\esp\.espressif\python_env\idf5.5_py3.10_env
set ESP_ROM_ELF_DIR=C:\Espressif\tools\esp-rom-elfs\20241011
set PATH=%IDF_PYTHON_ENV_PATH%\Scripts;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;%PATH%
cd /d D:\UserData\ESP32Project\IDF\ESP32-IDF-Test
python "%IDF_PATH%\tools\idf.py" build
