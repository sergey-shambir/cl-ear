:: Installs build dependencies using Conan, see https://conan.io/
::  - to see list of Conan packages, use `conan search --remote all`

pip install conan
conan remote add bincrafters https://api.bintray.com/conan/bincrafters/public-conan

pushd "%~dp0msbuild"
conan install --build=missing -s arch=x86 -s build_type=Debug "%~dp0conanfile.txt"
conan install --build=missing -s arch=x86_64 -s build_type=Debug "%~dp0conanfile.txt"
conan install --build=missing -s arch=x86 -s build_type=Release "%~dp0conanfile.txt"
conan install --build=missing -s arch=x86_64 -s build_type=Release "%~dp0conanfile.txt"
popd
