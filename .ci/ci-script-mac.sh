# set -ex

# CPU_ARCHITECTURE=""
# if [[ `uname -a` =~ ^Darwin.* ]] && [[ `uname -a` =~ .*arm64$ ]]
# then
#     CPU_ARCHITECTURE="ARM64"
#     CMAKE_MORE_OPTIONS="${CMAKE_MORE_OPTIONS} -DBUILD_SSE2_CODEPATHS=OFF"
# else
# 	CPU_ARCHITECTURE="Intel"
# fi;

# mkdir build
cd build

#TODO figure out how to get icu4c building properly
#TODO make sure to SSE2_CODEPATHS=OFF for arm64 (see above commented code)
cmake .. \
    -G $GENERATOR \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX"\
    -DCMAKE_BUILD_TYPE=Release \
    -DUSE_ICU=Off \
    -DBUILD_SSE2_CODEPATHS=OFF \
    -DBINARY_PACKAGE_BUILD=OFF 

cmake --build . --target install



