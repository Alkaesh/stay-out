@echo off
set _NT_SYMBOL_PATH=cache*C:\symbols;C:\Users\alga\Documents\GitHub\stay out\ZCodeProject\ZCodeProject\injector2\x64\Release
"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\kd.exe" -z "C:\Users\alga\Desktop\070326-31312-01.dmp" -c "!analyze -v; kb; q -d"
