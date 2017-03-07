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

namespace
{
const wchar_t COMPILE_DB_FILENAME[] = L"compile_commands.json";
}

class Utils
{
public:
	Utils() = delete;

	// Adds '\' to directory path end if it wasn't added already.
	static wstring AddDirectorySlash(const wstring &dir)
	{
		wstring result = dir;
		if (!dir.empty())
		{
			if ((dir.back() != L'\\') && (dir.back() != L'/'))
			{
				result += L'\\';
			}
		}
		return result;
	}

	// convert UTF-8 string to wstring
	static std::wstring utf8_to_wstring(const std::string& str)
	{
		std::wstring_convert<std::codecvt_utf8<wchar_t>> myconv;
		return myconv.from_bytes(str);
	}

	// convert wstring to UTF-8 string
	static std::string wstring_to_utf8(const std::wstring& str)
	{
		std::wstring_convert<std::codecvt_utf8<wchar_t>> myconv;
		return myconv.to_bytes(str);
	}

	// convert vector<wstring> to UTF-8 vector<string>
	static std::vector<std::string> wstring_list_to_utf8(const std::vector<std::wstring> &list)
	{
		std::vector<std::string> result(list.size());
		std::transform(list.begin(), list.end(), result.begin(), &Utils::wstring_to_utf8);
		return result;
	}

	static std::wstring ConvertSlashesToUnix(const std::wstring& path)
	{
		return boost::replace_all_copy(path, L"\\", L"/");
	}
};

struct CompileCommand
{
    wstring directory;
    vector<wstring> commands;
    wstring file;

    json ToJson()const
    {
		vector<wstring> commandsWithFile = commands;
		commandsWithFile.push_back(Utils::ConvertSlashesToUnix(file));
		const wstring commandsJoined = boost::join(commandsWithFile, L" ");

        json obj = json::object();
        obj["directory"] = Utils::wstring_to_utf8(Utils::ConvertSlashesToUnix(directory));
        obj["command"] = Utils::wstring_to_utf8(commandsJoined);
        obj["file"] = Utils::wstring_to_utf8(Utils::ConvertSlashesToUnix(file));

        return obj;
    }
};


class Environment
{
public:
	Environment() = delete;

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

	static vector<wstring> GetSdkIncludePaths()
	{
		const char *includeVar = getenv("INCLUDE");
		vector<wstring> result;

		if (includeVar)
		{
			const wstring includeVarUtf16 = Utils::utf8_to_wstring(includeVar);
			boost::split(result, includeVarUtf16, boost::is_any_of(L";"), boost::algorithm::token_compress_on);

			// Erase empty results (just to ensure correctness).
			auto newEnd = std::remove_if(result.begin(), result.end(), [](const wstring &str) {
				return str.empty();
			});
			result.erase(newEnd, result.end());
		}

		return result;
	}

	static vector<wstring> GetMsCompatibilityFlags()
	{
		return{
			L"-fms-compatibility",
			L"-fdelayed-template-parsing",
			L"-fms-extensions",
		};
	}
};


class CompileCommandsBuilder
{
public:
    void Build(vector<wstring> compileArgs)
	{
		const wstring directory = Environment::GetWorkingDirectory();

        JoinMacroDefinitions(compileArgs);

		vector<wstring> inputFiles;
		vector<wstring> compilerFlags = Environment::GetMsCompatibilityFlags();
		AppendSdkIncludePaths(Environment::GetSdkIncludePaths(), compilerFlags);
        TranslateFlags(compileArgs, compilerFlags, inputFiles);


		m_commands.clear();
		m_commands.reserve(inputFiles.size());
		for (const wstring &relativePath : inputFiles)
		{
			CompileCommand command;
			command.directory = directory;
			command.file = Utils::AddDirectorySlash(directory) + relativePath;
			command.commands = compilerFlags;
			m_commands.push_back(command);
		}
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

	void AppendSdkIncludePaths(const vector<wstring> &sdkIncludePaths, vector<wstring> &compilerFlags)
	{
		compilerFlags.reserve(compilerFlags.size() + sdkIncludePaths.size());
		std::transform(sdkIncludePaths.begin(), sdkIncludePaths.end(), std::back_inserter(compilerFlags), [](const wstring &path) {
			return L"-I\"" + Utils::ConvertSlashesToUnix(path) + L"\"";
		});
	}

    void TranslateFlags(const vector<wstring> &compileArgs, vector<wstring> &compilerFlags, vector<wstring> &inputFiles)
    {
		compilerFlags.reserve(compilerFlags.size() + compileArgs.size());
		inputFiles.reserve(inputFiles.size() + compileArgs.size());

        for (wstring arg : compileArgs)
        {
            if (boost::starts_with(arg, L"/"))
            {
                TranslateOneFlag(std::move(arg), compilerFlags);
            }
			// Joined macro definition.
			else if (boost::starts_with(arg, L"-"))
			{
				compilerFlags.emplace_back(std::move(arg));
			}
            else
            {
				inputFiles.emplace_back(std::move(arg));
            }
        }
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
            { L"/ZI", L"-g" }, // Includes debug information in a program database compatible with Edit and Continue.
            { L"/Zi", L"-g" }, // Generates complete debugging information.
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
        WriteCompileCommandsJson(command);
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
        builder.Build(compileArgs);

        return builder.TakeCommands();
    }

    static void WriteCompileCommandsJson(const vector<CompileCommand> &commands)
    {
		const wstring workdir = Environment::GetWorkingDirectory();
		const wstring databasePath = Utils::AddDirectorySlash(workdir) + COMPILE_DB_FILENAME;

		json commandsArr = json::array();

		// Optionally read existing JSON array.
		{
			ifstream input(databasePath);
			input.exceptions(ios::badbit);
			if (input.is_open())
			{
				input >> commandsArr;
			}
		}

		// Add commands to JSON array.
		for (const CompileCommand &command : commands)
		{
			commandsArr.push_back(command.ToJson());
		}

		// Write compilation database file.
		{
			ofstream output(databasePath);
			if (!output.is_open())
			{
				throw std::runtime_error("Cannot write compile_commands.json file at " + Utils::wstring_to_utf8(workdir));
			}
			output.exceptions(ios::badbit | ios::failbit);
			output << commandsArr;
		}
    }

    static vector<wstring> ReadCompileFlagsFromFile(const wstring &path)
    {
        FILE *input = _wfopen(path.c_str(), L"rb");
        if (input == nullptr)
        {
            throw std::runtime_error("Cannot open file: " + Utils::wstring_to_utf8(path));
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

