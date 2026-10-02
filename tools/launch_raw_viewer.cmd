@echo off
setlocal
if not defined RAW_VIEWER_PYTHON set "RAW_VIEWER_PYTHON=%USERPROFILE%\anaconda3\python.exe"
if not defined RAW_VIEWER_DECODER_PYTHON set "RAW_VIEWER_DECODER_PYTHON=%USERPROFILE%\OneDrive\Documents\Playground\.msi_build_venv\Scripts\python.exe"
if not exist "%RAW_VIEWER_PYTHON%" goto missing
if not exist "%RAW_VIEWER_DECODER_PYTHON%" goto missing
"%RAW_VIEWER_PYTHON%" "%~dp0raw_test_viewer.py" --decoder-python "%RAW_VIEWER_DECODER_PYTHON%" %*
if errorlevel 1 pause
exit /b
:missing
echo Existing Python runtimes were not found.
echo Set RAW_VIEWER_PYTHON to the Python matching the engine build.
echo Set RAW_VIEWER_DECODER_PYTHON to an existing Python with rawpy, NumPy and Pillow.
pause
exit /b 1
