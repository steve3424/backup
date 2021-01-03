@echo off

mkdir ..\build
pushd ..\build

cl ..\src\main.c -Z7 /link user32.lib shlwapi.lib

popd
