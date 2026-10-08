// The licence summaries and titles, in all 13 languages, carry no restriction the
// licences do not: no commercial-use, non-commercial or redistribution wording
// (Spirula redistributes no weights). A catalog edit that adds one fails here by
// language and message. A model with licences of its own scans its families beside it.

#include "i18n/Message.h"
#include "nn/tests/license_wording.h"
#include "i18n/catalog/Cli.h"
#include "i18n/catalog/Dataset.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace dmsg = spirula::i18n::msg::dataset;
namespace cmsg = spirula::i18n::msg::cli;
using spirula::i18n::Lang;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_failures++;
}

}  // namespace

int main() {
    struct Entry {
        const char* name;
        const spirula::i18n::Msg* msg;
    };
    const Entry entries[] = {
        {"license_full_text", &dmsg::license_full_text},
        {"license_accept", &dmsg::license_accept},
        {"license_not_accepted_download", &dmsg::license_not_accepted_download},
        {"license_accept_tick", &dmsg::license_accept_tick},
        {"license_declined_download", &dmsg::license_declined_download},
        {"batch_licence_declined", &dmsg::batch_licence_declined},
        {"license_ask_header", &cmsg::license_ask_header},
        {"license_ask_confirm", &cmsg::license_ask_confirm},
        {"license_ask_declined", &cmsg::license_ask_declined},
        {"license_no_terminal", &cmsg::license_no_terminal},
    };
    int scanned = 0;
    for (const Entry& e : entries) {
        for (unsigned l = 0; l < spirula::i18n::kLangCount; ++l) {
            ++scanned;
            const std::string hit = licwording::banned_in(e.msg->in((Lang)l));
            check(hit.empty(), std::string(e.name) + " [" + std::to_string(l) + "]" +
                                   (hit.empty() ? "" : " contains '" + hit + "'"));
        }
    }
    check(scanned == (int)(sizeof entries / sizeof entries[0]) * (int)spirula::i18n::kLangCount,
          "every message scanned in every language");
    // A translation that drops a placeholder asks for acceptance of nothing in particular,
    // or leaves the user without the flag to type.
    for (unsigned l = 0; l < spirula::i18n::kLangCount; ++l) {
        const std::string n = " [" + std::to_string(l) + "]";
        check(std::string(dmsg::license_accept_tick.in((Lang)l)).find("{0}") != std::string::npos,
              "license_accept_tick" + n + " carries the licence name");
        check(std::string(cmsg::license_ask_confirm.in((Lang)l)).find("{0}") != std::string::npos &&
                  std::string(cmsg::license_ask_confirm.in((Lang)l)).find("'yes'") != std::string::npos,
              "license_ask_confirm" + n + " names the licence and asks for 'yes'");
        const std::string t = cmsg::license_no_terminal.in((Lang)l);
        check(t.find("{0}") != std::string::npos && t.find("{1}") != std::string::npos &&
                  t.find("--accept-license {2}=yes") != std::string::npos,
              "license_no_terminal" + n + " carries the name, the URL and the exact flag");
    }
    // The scan can see: a sentence with the wording in it is caught.
    check(!licwording::banned_in("for non-commercial use only").empty(),
          "control: the English stem matches a restricted sentence");
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
