cmake -S . -B build -A x64 -G "Visual Studio 16 2019" -DSDL3_DIR=./ThirdParty/SDL3-VC/cmake
cmake --build build --config Release
pause