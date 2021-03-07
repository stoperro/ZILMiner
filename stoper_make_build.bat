mkdir build
pushd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=f:\vcpkg\scripts\buildsystems\vcpkg.cmake -DHUNTER_ENABLED=OFF -DETHASHCL=OFF -DETHASHCUDA=ON
popd
