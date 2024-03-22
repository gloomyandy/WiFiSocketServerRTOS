#!/bin/sh
#extract firmware version from header file
VER=`awk 'sub(/.*VERSION_MAIN/,""){print $1}' src/Config.h  | awk 'gsub(/"/, "", $1)'`

OUTPUT=releases/${VER}

mkdir -p ${OUTPUT}
rm -f ${OUTPUT}/*

# build 8266 version. We run this via cmd.exe so that we can "hide" that it is being run from
# a 64 bit msys shell. Without this the build does not seem to work.
rm -f sdkconfig
rm -rf build
cmd << EOFXXX
set MSYSTEM=
set MSYS2=
\\msys32\\usr\\bin\\bash.exe build8266.sh
EOFXXX
if [ -f ./build/DuetWiFiServer.bin ]; then
	mv ./build/DuetWiFiServer.bin ${OUTPUT}/DuetWiFiServer-${VER}.bin
fi 

rm -f sdkconfig
rm -rf build
cmd << EOFXXX
set MSYSTEM=
set MSYS2=
..\esp-idf\export.bat
idf.py -DSUPPORT_ETHERNET=1 set-target esp32 build
EOFXXX
if [ -f ./build/DuetWiFiModule_32.bin ]; then
	mv ./build/DuetWiFiModule_32.bin ${OUTPUT}/DuetWiFiModule_eth-${VER}.bin
fi 

rm -f sdkconfig
rm -rf build
cmd << EOFXXX
set MSYSTEM=
set MSYS2=
..\esp-idf\export.bat
idf.py set-target esp32 build
EOFXXX
if [ -f ./build/DuetWiFiModule_32.bin ]; then
	mv ./build/DuetWiFiModule_32.bin ${OUTPUT}/DuetWiFiModule_32-${VER}.bin
fi 

