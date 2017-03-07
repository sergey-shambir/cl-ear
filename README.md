Project "cl-ear" is a cl.exe compiler ear. It's command line tool which listens MSBuild commands for MSVC and writes them into compile_commands.json

Example usage:

```bash
msbuild /t:clean "C:\Projects\MyApp.vcxproj"
msbuild /t:Build /p:Configuration=Release /p:CLToolExe=cl-ear.exe /p:CLToolPath="%~dp0\Release" "C:\Projects\MyApp.vcxproj"
```

Known limitations:

- existing compile_commands.json are always appended, so remove them manually before running cl-ear
- PCH compiler flags will be not translated
- a lot of other common compiler flags will be not translated too
