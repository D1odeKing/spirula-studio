#pragma once
// Licence consent on a terminal: the CLI half of the GUI's accept dialog. A
// terminal prints the terms and wants `yes`; without one it refuses, naming
// `--accept-license <family>=yes`. The record is gui.conf's accepted_license=
// lines (core/LicenseConsent.h), the one the GUI reads too.

#include <cstdio>
#include <functional>
#include <string>

namespace app {

struct ConsentIO {
    std::FILE*                       out = stdout;  // the terms are printed here
    bool                             tty = false;   // may ask on stdin
    std::function<std::string()>     read_line;     // one answer; used when `tty`
};

// stdin/stdout, asking only when both are a terminal and SS_NO_AUTO_FETCH is
// unset: a GUI started from a terminal runs its children with that set, and
// they must not wait on its stdin.
ConsentIO stdio_consent();
// Whether a prompt may be put to a terminal: `is_tty`, and SS_NO_AUTO_FETCH unset.
bool terminal_allowed(bool is_tty);

// Asks for each unaccepted family of a comma list (null or empty: none) and
// records a "yes". Throws nn::Error without a terminal, on any other answer, or
// for an unknown family. The one-argument form uses stdio_consent().
void require_license(const char* family, const ConsentIO& io);
void require_license(const char* family);

// A `--accept-license` value: a comma list of `family` (asks on a TTY, refuses
// otherwise) or `family=yes` (no question). Prints each family's terms, then
// records. Throws nn::Error on an unknown family or a declined question.
void accept_licenses(const std::string& spec, const ConsentIO& io);

// Removes every `--accept-license <spec>` / `--accept-license=<spec>` from
// argv, accepts what they name on stdin/stdout, and returns how many it found.
int consume_accept_license_args(int& argc, char** argv);

// Makes nn::ensure_file ask through stdio_consent(). Call once at startup.
void install_license_gate();

}  // namespace app
