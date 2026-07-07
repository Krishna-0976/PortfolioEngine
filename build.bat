@echo off 
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" 
cl src/main.cpp include/sqlite3.c /EHsc /Fe:main.exe /I"C:\Program Files\OpenSSL-Win64\include" /link /LIBPATH:"C:\Program Files\OpenSSL-Win64\lib\VC\x64\MT" libssl.lib libcrypto.lib 
