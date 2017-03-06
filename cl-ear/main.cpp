// cl-ear.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "json/json.hpp"
#include <boost/scope_exit.hpp>
#include <boost/algorithm/string.hpp>
#include <unordered_map>
#include <unordered_set>

#define NOMINMAX
#include <Windows.h>


using namespace std;
using nlohmann::json;

// convert UTF-8 string to wstring
std::wstring utf8_to_wstring(const std::string& str)
{
    std::wstring_convert<std::codecvt_utf8<wchar_t>> myconv;
    return myconv.from_bytes(str);
}

// convert wstring to UTF-8 string
std::string wstring_to_utf8(const std::wstring& str)
{
    std::wstring_convert<std::codecvt_utf8<wchar_t>> myconv;
    return myconv.to_bytes(str);
}

struct CompileCommand
{
    wstring directory;
    vector<wstring> commands;
    wstring file;

    json ToJson()const
    {
        json commandsArr = json::array();
        std::copy(commands.begin(), commands.end(), back_inserter(commandsArr));

        json obj = json::object();
        obj["directory"] = directory;
        obj["commands"] = commands;
        obj["file"] = file;

        return obj;
    }
};


class CompileCommandsBuilder
{
public:
    void Build(const wstring &directory, vector<wstring> compileArgs)
    {
        JoinMacroDefinitions(compileArgs);
        TranslateFlags(compileArgs);

        CompileCommand command;
        command.directory = directory;
        command.commands = std::move(compileArgs);
        m_commands = { command };
    }

    vector<CompileCommand> TakeCommands()
    {
        return std::move(m_commands);
    }

private:
    void JoinMacroDefinitions(vector<wstring> &compileArgs)
    {
        // Join macro definitions '/D MACRO' to '-DMACRO'
        for (auto it = compileArgs.begin(); it != compileArgs.end();)
        {
            auto nextIt = std::next(it);
            if ((*it == L"/D") && (nextIt != compileArgs.end()))
            {
                *it = L"-D" + *nextIt;
                it = compileArgs.erase(nextIt);
            }
            else
            {
                ++it;
            }
        }
    }

    void TranslateFlags(vector<wstring> &compileArgs)
    {
        vector<wstring> result;
        result.reserve(compileArgs.size());
        for (wstring arg : compileArgs)
        {
            if (boost::starts_with(arg, L"/"))
            {
                TranslateOneFlag(std::move(arg), result);
            }
            else
            {
                result.emplace_back(std::move(arg));
            }
        }

        compileArgs = result;
    }

    void TranslateOneFlag(wstring flag, vector<wstring> &result)
    {
        if (boost::starts_with(flag, L"/I"))
        {
            flag[0] = L'-';
            result.emplace_back(std::move(flag));
        }
        else
        {
            // Mapping based on heuristic.
            // See also `CLCompatOptions.td` in LLVM/Clang sources.
            const bool translated = IsIgnoredFlag(flag)
                || IsIgnoredByPrefix(flag)
                || MapSimpleFlag(flag, result)
                || MapFlagToSequence(flag, result)
                || MapSyntaxSwitch(flag, result);
            if (!translated)
            {
                std::wcerr << L"Cannot translate flag: " << flag << std::endl;
            }
        }
    }

    static bool IsIgnoredFlag(const wstring &flag)
    {
        static const unordered_set<wstring> IGNORED = {
            L"/nologo", // Suppresses display of sign-on banner.
            L"/Gm", // Enables minimal rebuild
            L"/EHsc", // Default C++ exceptions model, assume C functions never throw
            L"/EHc", // Default C++ exceptions model, assume C functions may throw
            // Runtime checks (replaced with sanitizers)
            L"/RTC1",
            L"/RTCs",
            L"/RTCu",
            L"/RTCsu",
            L"/RTCc",
            // Buffers security check. (replaced with sanitizers)
            L"/GS",
            // Static/Dynamic and Debug/Release C++ runtime switches (no direct replacement)
            L"/MT",
            L"/MTd",
            L"/MD",
            L"/MDd",
            // Floating point calculations - default and strict behaviors.
            L"/fp:precise",
            L"/fp:except",
            L"/fp:strict",
            // Enable/disable flags for additional passes can be ignored
            L"/analyze",
            L"/analyze-",
            L"/errorReport:queue",
            // Flags that equal to Clang defaults
            L"/Gd", // Uses the __cdecl calling convention (x86 only).
            L"/WX-", // Don't treat warnings as errors
        };

        return (IGNORED.find(flag) != IGNORED.end());
    }

    static bool IsIgnoredByPrefix(const wstring &flag)
    {
        static const wstring IGNORED[] = {
            L"/Fd",
            L"/Fo", // TODO: equal to `-ofile.o`
            L"/Fe", // TODO: equal to `-ofile.exe`
            L"/Zc", // Flags to switch MSVC back to standard behavior.
        };
        auto isFlagPrefix = [&flag](const wstring &prefix) {
            return boost::starts_with(flag, prefix);
        };
        return (std::find_if(begin(IGNORED), end(IGNORED), isFlagPrefix) != end(IGNORED));
    }

    // Maps `/<FLAG>` to `-<REPLACEMENT>`
    static bool MapSimpleFlag(const wstring &flag, vector<wstring> &result)
    {
        assert(flag.at(0) == L'/');
        static const unordered_map<wstring, wstring> MAPPING = {
            { L"/c", L"-c" }, // Compiles without linking.
            { L"/Od", L"-O0" }, // Disables optimization.
            { L"/O1", L"-Os" }, // Creates small code.
            { L"/O2", L"-O2" }, // Creates fast code.
            { L"/P", L"-E"}, // Writes preprocessor output to a file.
            { L"/Zi", L"-g" }, // Includes debug information in a program database compatible with Edit and Continue.
            { L"/fp:fast", L"-ffast-math" },
            { L"/W0", L"-w" }, // Disable all warnings
            { L"/W1", L"-Wall" }, // Warning level 1
            { L"/W2", L"-Wall" }, // Warning level 2
            { L"/W3", L"-Wall" }, // Warning level 3
            { L"/WX", L"-Werror" }, // Treat warnings as errors
            { L"/Zs", L"-fsyntax-only" }, // Perform only syntax check without compilation
        };
        auto foundIt = MAPPING.find(flag);
        if (foundIt != MAPPING.end())
        {
            result.emplace_back(foundIt->second);
            return true;
        }

        return false;
    }


    // Maps `/<FLAG>` to `-<REPLACEMENT1> -<REPLACEMENT2> ...`
    static bool MapFlagToSequence(const wstring &flag, vector<wstring> &result)
    {
        assert(flag.at(0) == L'/');
        static const unordered_map<wstring, vector<wstring>> MAPPING = {
            { L"/Tp", { L"-x", L"c++" } }, // C++ file
            { L"/TP", { L"-x", L"c++" } }, // C++ file
            { L"/Tc", { L"-x", L"c" } }, // C++ file
            { L"/TC", { L"-x", L"c" } }, // C++ file
            { L"/W4", { L"-Wall", L"-Wextra" } }, // Warning level 4
        };
        auto foundIt = MAPPING.find(flag);
        if (foundIt != MAPPING.end())
        {
            const vector<wstring> &src = foundIt->second;
            std::copy(src.begin(), src.end(), std::back_inserter(result));
            return true;
        }

        return false;
    }

    // Maps `/<FLAG>` to `-f<REPLACEMENT>`, or `/<FLAG>-` to `-fno-<REPLACEMENT>`.
    static bool MapSyntaxSwitch(const wstring &flag, vector<wstring> &result)
    {
        assert(flag.at(0) == L'/');
        static const unordered_map<wstring, wstring> MAPPING = {
            { L"Oy", L"omit-frame-pointer" },
            { L"Oi", L"builtin" }, // Enable use of builtin functions
        };
        wstring key = flag.substr(1);
        bool enableSwitch = true;

        if (!key.empty() && (key.back() == L'-'))
        {
            enableSwitch = false;
            key.erase(key.size() - 1);
        }

        auto foundIt = MAPPING.find(key);
        if (foundIt != MAPPING.end())
        {
            const wstring prefix = enableSwitch ? L"-f" : L"-fno-";
            result.emplace_back(prefix + foundIt->second);
            return true;
        }

        return false;
    }

    vector<CompileCommand> m_commands;
};

class CompilerEar
{
public:
    CompilerEar()
        : m_arguments(MakeArgsList())
    {
    }

    void WriteCompileDatabase()
    {
        vector<wstring> args = m_arguments;
        
        // MSBuild passes compiler flags within temporary file.
        if (args.size() == 1 && boost::starts_with(args[0], L"@"))
        {
            const wstring filepath = args[0].substr(1);
            args = ReadCompileFlagsFromFile(filepath);
        }

        vector<CompileCommand> command = TranslateCompileFlags(args);
        WriteCompileCommand(command);
    }

    void RunCompiler()
    {
        std::wstring command = L"cl.exe ";
        for (const wstring &arg : m_arguments)
        {
            command += L" \"";
            command += boost::replace_all_copy(arg, L"\"", L"\\\"");
            command += L"\"";
        }

        STARTUPINFO info = { sizeof(info) };
        PROCESS_INFORMATION processInfo;

        if (::CreateProcessW(nullptr, &command[0], nullptr, nullptr, TRUE, 0, nullptr, nullptr, &info, &processInfo))
        {
            WaitForSingleObject(processInfo.hProcess, INFINITE);
            CloseHandle(processInfo.hProcess);
            CloseHandle(processInfo.hThread);
        }
    }

private:
    static vector<CompileCommand> TranslateCompileFlags(vector<wstring> compileArgs)
    {
        CompileCommandsBuilder builder;
        builder.Build(GetWorkingDirectory(), compileArgs);

        return builder.TakeCommands();
    }

    static wstring GetWorkingDirectory()
    {
        // MSBuild calls compiler within project directory.
        wstring workdir;
        wchar_t currentDir[MAX_PATH];
        if (::GetCurrentDirectoryW(MAX_PATH, currentDir) != 0)
        {
            workdir = currentDir;
        }
        return workdir;
    }

    static void WriteCompileCommand(const vector<CompileCommand> &commands)
    {
        // TODO: write to JSON file
        for (const CompileCommand &command : commands)
        {
            printf("--- %d args ---\n", int(command.commands.size()));
            for (const wstring &arg : command.commands)
            {
                wprintf(L"  '%ls'\n", arg.c_str());
            }
        }
    }

    static vector<wstring> ReadCompileFlagsFromFile(const wstring &path)
    {
        FILE *input = _wfopen(path.c_str(), L"rb");
        if (input == nullptr)
        {
            throw std::runtime_error("Cannot open file: " + wstring_to_utf8(path));
        }
        BOOST_SCOPE_EXIT_ALL(&) {
            fclose(input);
        };

        constexpr size_t BUFFER_SIZE = 64 * 1024;
        wchar_t buffer[BUFFER_SIZE];

        wstring unicodeArgs;
        while (true)
        {
            size_t readCount = ::fread(buffer, sizeof(wchar_t), BUFFER_SIZE, input);
            if (readCount == 0)
            {
                break;
            }
            unicodeArgs.append(buffer, readCount);
        }
        // Remove BOM mark.
        if (unicodeArgs[0] != L'/')
        {
            unicodeArgs.erase(unicodeArgs.begin());
        }

        return SplitCommandLine(unicodeArgs);
    }

    static vector<wstring> SplitCommandLine(const wstring &unicodeArgs)
    {
        int numArgs = 0;
        LPWSTR* argsArray = ::CommandLineToArgvW(unicodeArgs.c_str(), &numArgs);
        BOOST_SCOPE_EXIT_ALL(&) {
            ::LocalFree(argsArray);
        };

        vector<wstring> ret;
        ret.reserve(numArgs);
        for (int i = 0; i < numArgs; ++i)
        {
            ret.emplace_back(argsArray[i]);
        }

        return ret;
    }

    static vector<wstring> MakeArgsList()
    {
        LPWSTR unicodeArgs = ::GetCommandLineW();
        vector<wstring> result = SplitCommandLine(unicodeArgs);
        result.erase(result.begin());
        return result;
    }

    const vector<wstring> m_arguments;
};

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    try
    {
        CompilerEar ear;
        ear.WriteCompileDatabase();
        ear.RunCompiler();
    }
    catch (const std::exception &ex)
    {
        fputs(ex.what(), stderr);
        return 1;
    }

    return 0;
}

