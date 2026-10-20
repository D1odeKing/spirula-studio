// command_argv_test -- the command a finished batch runs is assembled here.
//
// Two properties this locks down: the message is ONE argument however the
// user quoted the token, and it carries nothing a JSON payload or a command
// line would have to escape (no backslash, no straight quote, no control
// character). The GUI's Test button is how a user checks their own command;
// this is how the substitution itself stays honest. Also command_exists, which
// checks the Tool locations.

#include "app/gui/Subprocess.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using gui::command_argv;
using gui::safe_arg;

namespace {

int g_failed = 0;

void check(bool ok, const std::string& what) {
    std::printf("%-4s %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) g_failed++;
}

void eq(const std::string& got, const std::string& want,
        const std::string& what) {
    if (got != want)
        std::printf("      got  <%s>\n      want <%s>\n", got.c_str(),
                    want.c_str());
    check(got == want, what);
}

const char* kToken = "{message}";

// The only characters the result may not contain, whatever went in.
bool json_safe(const std::string& s) {
    for (unsigned char c : s)
        if (c == '\\' || c == '"' || c < 0x20 || c == 0x7F) return false;
    return true;
}

void test_safe_arg() {
    eq(safe_arg("Done: 3   Failed: 0"), "Done: 3 Failed: 0",
       "runs of spaces collapse");
    eq(safe_arg(" trailing "), "trailing", "the ends are trimmed");
    eq(safe_arg("a\nb\tc"), "a b c", "control characters become spaces");
    eq(safe_arg("C:\\data\\scene"), "C:/data/scene",
       "a backslash becomes a slash");
    eq(safe_arg("say \"hi\""), "say ”hi”", "a straight double quote is curled");
    eq(safe_arg("it's `here`"), "it’s ’here’",
       "an apostrophe and a backtick are curled");
    eq(safe_arg("批处理结束。"), "批处理结束。", "UTF-8 passes through");
    check(json_safe(safe_arg("\"\\\t\x01' `")), "nothing unsafe survives");
    eq(safe_arg(""), "", "an empty message stays empty");
}

void test_command_argv() {
    std::vector<std::string> argv =
        command_argv("python notify_me.py --message \"{message}\"", kToken,
                     "Done: 2, failed: 0");
    check(argv.size() == 4, "a quoted token is one argument");
    eq(argv.size() > 0 ? argv[0] : "", "python", "the program comes first");
    eq(argv.size() > 3 ? argv[3] : "", "Done: 2, failed: 0",
       "the message lands whole in the quoted argument");

    // Unquoted: the split happens BEFORE the substitution, so a message with
    // spaces in it still cannot become several arguments.
    argv = command_argv("notify --message {message}", kToken, "a b c");
    check(argv.size() == 3, "an unquoted token is one argument too");
    eq(argv.size() > 2 ? argv[2] : "", "a b c", "and still holds it all");

    argv = command_argv("notify --to me --message=[{message}]", kToken, "hi");
    eq(argv.size() > 3 ? argv[3] : "", "--message=[hi]",
       "the token is replaced in place");

    argv = command_argv("notify {message} {message}", kToken, "x");
    check(argv.size() == 3 && argv[1] == "x" && argv[2] == "x",
          "every occurrence is replaced");

    argv = command_argv("notify --message \"{message}\"", kToken,
                        "she said \"go\"");
    eq(argv.size() > 2 ? argv[2] : "", "she said ”go”",
       "a quote in the message cannot close the argument");

    check(command_argv("", kToken, "x").empty(), "no command, no argv");
    argv = command_argv("notify --plain", kToken, "x");
    check(argv.size() == 2, "a command with no token runs unchanged");
}

// A command long enough to be worth writing over several lines is the normal
// case for a webhook, so the box takes line breaks and the split understands
// the continuation a pasted shell command carries.
void test_multiline() {
    std::vector<std::string> argv =
        command_argv("notify\n  --message\n  {message}", kToken, "a b");
    check(argv.size() == 3, "a line break separates arguments");
    eq(argv.size() > 2 ? argv[2] : "", "a b", "and the message is still whole");

    argv = command_argv("notify \\\n  --message x", kToken, "");
    check(argv.size() == 3, "a continuation leaves no stray backslash");
    eq(argv.size() > 0 ? argv[0] : "", "notify", "the program is unharmed");

    argv = command_argv("notify \\\r\n  --message x", kToken, "");
    check(argv.size() == 3, "a CRLF continuation too");

    argv = command_argv("notify --dir C:\\data\\x", kToken, "");
    eq(argv.size() > 2 ? argv[2] : "", "C:\\data\\x",
       "a backslash that is not a line break is a path");

    // The shape a webhook takes: a quoted JSON body with the token inside it.
    const char* curl =
        "curl -H \"Content-Type: application/json\" \\\n"
        "     -X POST \\\n"
        "     -d '{\"content\": \"<@1234> {message}\"}' \\\n"
        "     https://example.com/api/webhooks/1/token";
    argv = command_argv(curl, kToken, "Done: 2, failed: 0");
    check(argv.size() == 8, "the whole invocation is eight arguments");
    eq(argv.size() > 6 ? argv[6] : "",
       "{\"content\": \"<@1234> Done: 2, failed: 0\"}",
       "the JSON body keeps its own quotes and takes the message");
    eq(argv.size() > 7 ? argv[7] : "",
       "https://example.com/api/webhooks/1/token", "the URL survives intact");

    // The reason safe_arg exists: this body is parsed by whatever receives it.
    argv = command_argv(curl, kToken, "he said \"go\" C:\\x");
    eq(argv.size() > 6 ? argv[6] : "",
       "{\"content\": \"<@1234> he said ”go” C:/x\"}",
       "a message cannot break out of the JSON body");
}

// Issue #201: a folder typed as the ffmpeg location passed, then failed to
// start with nothing in the log.
void test_command_exists() {
    const std::string dir = std::filesystem::temp_directory_path().string();
    check(!gui::command_exists(dir), "a folder is not a command");
    check(!gui::command_exists(dir + "/"), "nor with a trailing separator");
#ifdef _WIN32
    check(gui::command_exists("cmd"), "a program on PATH is found");
#else
    check(gui::command_exists("sh"), "a program on PATH is found");
    check(gui::command_exists("/bin/sh"), "and by its path");
#endif
}

}  // namespace

int main() {
    test_safe_arg();
    test_command_argv();
    test_multiline();
    test_command_exists();
    if (g_failed) {
        std::printf("\n%d failed\n", g_failed);
        return 1;
    }
    std::printf("\nall passed\n");
    return 0;
}
