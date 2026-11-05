cmake -S . -B build86 -A Win32 -G "Visual Studio 16 2019" -DSDL3_DIR=./ThirdParty/SDL3-VC/cmake
cmake --build build86 --config Release
pause